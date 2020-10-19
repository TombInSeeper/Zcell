
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/util.h"

#include "util/log.h"
#include "util/fixed_cache.h"

#include "messager.h"
#include "objectstore.h"
#include "operation.h"


#define NR_REACTOR_MAX 256

static const char *g_base_ip = "0.0.0.0";
static int g_base_port = 18000;
static const char *g_core_mask = "0x1";
static int g_store_type = NULLSTORE;
static int g_idle = 0;

static void parse_args(int argc , char **argv) {
    int opt = -1;
	while ((opt = getopt(argc, argv, "i:p:c:s:d")) != -1) {
		switch (opt) {
		case 'i':
			g_base_ip = optarg;
			break;
        case 'd':
            g_idle = 1;
            break;
		case 'p':
			g_base_port = atoi(optarg);
			break;
        case 'c':
			g_core_mask = (optarg);
			break;
        case 's':
            if(!strcmp(optarg,"null")){
                log_info("Ostore type is nullstore \n");
                g_store_type = NULLSTORE;
            } else if (!strcmp(optarg,"chunk")) {
                log_info("Ostore type is chunkstore\n");
                g_store_type = CHUNKSTORE;
            } else {
                log_err("Unknown storage backend:%s \n" , optarg);
                exit(1);
            }
            break;
		default:
			fprintf(stderr, "Usage: %s [-i ip] [-p port] [-c core_mask]\n", argv[0]);
			exit(1);
		}
	}
}


struct small_object_t {
    uint8_t raw[120];
    SLIST_ENTRY(small_object_t) hook;
};


enum RunningLevel {
    BUSY_MAX,
    BUSY1,
    BUSY2,
    BUSY3,
    IDLE = 64
};


typedef struct reactor_ctx_t {
    int reactor_id;
    const char *ip;
    int port;

    const msgr_server_if_t *msgr_impl;
    const objstore_impl_t  *os_impl;


    bool idle_enable;
    uint64_t idle_poll_start_us;
    uint64_t idle_poll_exe_us;
    uint64_t rx_last_window;
    uint64_t tx_last_window;
    uint64_t rx_io_last_window;
    uint64_t tx_io_last_window;
    struct spdk_poller *idle_poller;

    int running_level;
    volatile bool running;
} reactor_ctx_t;
static reactor_ctx_t g_reactor_ctxs[NR_REACTOR_MAX];
static inline reactor_ctx_t* reactor_ctx() {
    return &g_reactor_ctxs[spdk_env_get_current_core()];
}
static int reactor_reduce_state() {
    int i;
    int r = 0;
    SPDK_ENV_FOREACH_CORE(i) {
        r += g_reactor_ctxs[i].running;
    }
    return r;
}


static void *alloc_meta_buffer(uint32_t sz) {
    return malloc(sz);
}
static void free_meta_buffer(void *p) {
    free(p);
}

static void *alloc_data_buffer(uint32_t sz) {
    
    // static __thread tls_data_buf[4 << 20];
    
    void *ptr;
    if(sz <= 0x1000) {
        // log_debug("[fixed_cahce] \n");
        // ptr =  fcache_get(reactor_ctx()->dma_pages); 
        sz = 0x1000;
    }
    log_debug("[spdk_dma_malloc] \n");
    uint32_t align = (sz % 0x1000 == 0 )? 0x1000 : 0;
    ptr =  spdk_dma_malloc(sz, align, NULL);
    return ptr;
}
static void free_data_buffer(void *p) {
    // fcache_t *fc = reactor_ctx()->dma_pages;
    // if(fcache_in(fc , p)) {
        // log_debug("[fixed_cahce] \n");
        // fcache_put(fc, p);
    // } else {
    log_debug("[spdk_dma_free] \n");
    spdk_dma_free(p);
    // }
}


/**
 * 复用 request 结构生成 response
 * 这里我们对 request 的 meta_buffer 和 data_buffer 的处理方式是 lazy 的：
 * 只要将 header 的 meta_length 和 data_length 长度置 0， 
 * 那么 messager 发送时就不会发送 meta_buffer 和 data_buffer 的内容，：
 * request message 析构时，meta_buffer 和 data_buffer 如果是非空指针，会被自动被释放
 * 
 * meta_buffer : glibc free (后续可能也加上用户重载函数)
 * data_buffer : .. 用户重载的函数
 * 
 * 
 */
static inline void _response_with_reusing_request(message_t *request, uint16_t status_code) {
    request->header.status = cpu_to_le16(status_code);
    message_state_reset(request);
    log_debug("Perpare to send response :[status=%u , meta_len=%u, data_len=%u]\n",
        request->header.status,
        request->header.meta_length,
        request->header.data_length);
    reactor_ctx()->msgr_impl->messager_sendmsg(request);
}

static inline void _response_broken_op(message_t *request, uint16_t status_code) {
    request->header.data_length = 0;
    request->header.meta_length = 0;
    _response_with_reusing_request(request, status_code);
}

//Operation handle
static void _do_op_unknown(message_t *request) {
    _response_broken_op(request, UNKNOWN_OP);
}

static void oss_op_cb(void *ctx, int status_code) {
    message_t *request = ctx;
    if(status_code != OSTORE_EXECUTE_OK) {
        _response_broken_op(request,status_code);
        free(request);
        return;
    }
    _response_with_reusing_request(request, status_code);
    free(request);
}


//Step1:Check
static bool oss_op_valid(message_t *request) {
    int op = le16_to_cpu(request->header.type);
    bool rc = false;
    switch (op) {
        case msg_oss_op_stat: {
            rc = (le32_to_cpu(request->header.data_length) == 0) && 
            (le16_to_cpu(request->header.meta_length) == 0) && 
            (request->data_buffer == NULL) && 
            (request->meta_buffer == NULL);       
        }
            break;
        case msg_oss_op_create: {
            rc = (le32_to_cpu(request->header.data_length) == 0) && 
            (le16_to_cpu(request->header.meta_length) == sizeof(op_create_t)) && 
            (request->data_buffer == NULL) && 
            (request->meta_buffer != NULL);       
        }
            break; 
        case msg_oss_op_delete: {
            rc = (le32_to_cpu(request->header.data_length) == 0) && 
            (le16_to_cpu(request->header.meta_length) == sizeof(op_delete_t)) && 
            (request->data_buffer == NULL) && 
            (request->meta_buffer != NULL) ;       
        }
            break;
        case msg_oss_op_write:{
            op_write_t *op = (op_write_t *)request->meta_buffer;
            rc =  (request->data_buffer != NULL) && 
            (request->meta_buffer != NULL) && 
            (le32_to_cpu(request->header.data_length) == le32_to_cpu(op->len)) && 
            (le16_to_cpu(request->header.meta_length) > 0 );       
        }
            break;
        case msg_oss_op_read:{
            rc = (le32_to_cpu(request->header.data_length) == 0) && 
            (le16_to_cpu(request->header.meta_length) > 0 ) && 
            (request->data_buffer == NULL) && 
            (request->meta_buffer != NULL);       
        }
            /* code */
            break;        
        default:
            break;
    }
    return rc;
}

//Step2: prepare response structure reusing request 
// 复用 request 结构填充成 response 结构
static int  oss_op_refill_request_with_reponse(message_t *request) {
    int op = le16_to_cpu(request->header.type);
    int rc = 0;
    switch (op) {
        case msg_oss_op_stat: {
            request->header.meta_length = 0;             
            request->header.data_length = sizeof(op_stat_result_t);
            request->data_buffer = alloc_data_buffer(sizeof(op_stat_result_t));
        }
            break; 
        case msg_oss_op_create: {
            request->header.meta_length = 0;      
        }
            break; 
        case msg_oss_op_delete: {
            request->header.meta_length = 0;            
        }
            break;
        case msg_oss_op_write:{
            request->header.meta_length = 0;             
            request->header.data_length = 0;             
        }
            break;
        case msg_oss_op_read:{
            op_read_t *op = (op_read_t *)request->meta_buffer;
            request->header.meta_length = 0;             
            request->header.data_length = op->len; 
            request->data_buffer = alloc_data_buffer(le32_to_cpu(op->len));           
        }
            /* code */
            break;        
        default:
            break;
    }
    return rc;
}



/** 
 * 由于是异步操作，所以需要复制 request 内容到全局
 * 需要 malloc 
 * 子例程的异步上下文指针是 request 本身加上一段 store_type 依赖的上下文
 */ 
static void _do_op_oss(message_t * _request) {

    const objstore_impl_t *os_impl = reactor_ctx()->os_impl;

    const int async_op_ctx_sz = os_impl->obj_async_op_context_size();
    void *ctx = malloc(sizeof(message_t) + async_op_ctx_sz);
    
    message_t *request = ctx;
    memcpy(request, _request, sizeof(message_t));

    log_debug("Prepare to execute op:[seq=%lu,op_code=%u,meta_len=%u,data_len=%u]\n", request->header.seq,
        request->header.type, request->header.meta_length, request->header.data_length);

    int rc = INVALID_OP;
    if(!oss_op_valid(request)) {
        goto label_broken_op;
    }
    oss_op_refill_request_with_reponse(request);

    rc = os_impl->obj_async_op_call(request,oss_op_cb);
    
    
    if(rc == OSTORE_SUBMIT_OK) {
        return;
    }

label_broken_op:
    _response_broken_op(request, rc );
    free(request);
    return;
}

static void _do_op_ping(message_t *request) {
    _response_with_reusing_request(request, SUCCESS);
}

static void op_execute(message_t *request) {
    int op_code = le16_to_cpu(request->header.type);
    if(op_code == msg_ping) {
        _do_op_ping(request);
    } else if (MSG_TYPE_OSS(op_code)) {
        _do_op_oss(request);
    } else {
        _do_op_unknown(request);
    }
}

static void _on_recv_message(message_t *m) {
    // log_info("Recv a message done , m->meta=%u, m->data=%u\n" , m->header.meta_length ,m->header.data_length);
    log_debug("Recv a message done , m->id=%lu, m->meta=%u, m->data=%u\n" , m->header.seq,
     m->header.meta_length ,m->header.data_length);
    message_t _m ;
    /**
     * 承接 original message *m* 中的所有内容
     * 阻止 _on_** 调用后释放 m 内的 meta_buffer 和 data_buffer
     * 尽管这个操作看起来有些奇怪
     */
    message_move(&_m, m);
    
    reactor_ctx()->rx_last_window += message_get_data_len(&_m) +
        message_get_meta_len(&_m) + sizeof(message_t);
    reactor_ctx()->rx_io_last_window++;

    op_execute(&_m);
}

static void _on_send_message(message_t *m) {
    log_debug("Send a message done , m->id=%lu, m->meta=%u, m->data=%u\n" , m->header.seq,
     m->header.meta_length ,m->header.data_length);
    reactor_ctx()->tx_last_window += message_get_data_len(m) +
        message_get_meta_len(m) + sizeof(message_t);
    reactor_ctx()->tx_io_last_window++;
}



static void _idle_reset(void *rctx_) {
    reactor_ctx_t *rctx = rctx_;
    rctx->idle_poll_start_us = now();
    rctx->rx_last_window = 0;
    rctx->tx_last_window = 0;
    rctx->rx_io_last_window = 0;
    rctx->tx_io_last_window = 0;
}

static int _do_idle(void *rctx_) {
    reactor_ctx_t *rctx = rctx_;

    //1ms
    static const uint64_t window_10Gbps = 
        (1250 * 1000 * 1000ULL) / (1000); 
    //1ms
    static const uint64_t window_iops = 16; 
    
    uint64_t dx = spdk_max(rctx->tx_last_window , rctx->rx_last_window);
    uint64_t dx_iops = spdk_max(rctx->tx_io_last_window , rctx->rx_io_last_window);
    
    log_debug("dx=%lu,dx_iops=%lu\n",dx , dx_iops);
    
    if(dx >= window_10Gbps / 2  || dx_iops >= window_iops / 2) {
        rctx->running_level = BUSY_MAX;
        return 0;
    }else if (dx >= window_10Gbps / 4 || dx_iops >= window_iops / 4 ) {
        rctx->running_level = BUSY1;
        int i = 1000;
        while(--i)
            spdk_pause();
        return 0;
    }else if (dx >= window_10Gbps / 8 || dx_iops >= window_iops / 8) {
        rctx->running_level = BUSY2;
        int i = 10000;
        while(--i)
            spdk_pause();       
        return 0;
    } else if ( dx > 0  || dx_iops > 0 ) {
        usleep( (1000/dx_iops) / 5);     
    } else {
        rctx->running_level++;
        if(rctx->running_level == IDLE) {
            usleep(100);
            --rctx->running_level;
            return 0;
        }
    }
    return  0;
}

static int idle_poll(void *rctx_) {
    reactor_ctx_t *rctx = rctx_;
    uint64_t now_ = now();
    uint64_t dur = now_ - rctx->idle_poll_start_us;
    if(dur >= rctx->idle_poll_exe_us) {
        _do_idle(rctx_);
        _idle_reset(rctx_);
        return 1;
    }
    return 0;
}

//Stop routine
int _ostore_stop(const objstore_impl_t *oimpl){
    int rc = oimpl->unmount();
    return rc;
}
int _msgr_stop(const msgr_server_if_t *smsgr_impl) {
    smsgr_impl->messager_stop();
    smsgr_impl->messager_fini();
    return 0;
}
void _per_reactor_stop(void * ctx , void *err) {
    (void)err;
    (void)ctx;
    reactor_ctx_t * rctx = reactor_ctx();
    log_info("Stopping server[%d],[%s:%d]....\n", rctx->reactor_id,rctx->ip,rctx->port);
    
    _msgr_stop(rctx->msgr_impl);

    _ostore_stop(rctx->os_impl);
    
    //...
    if(rctx->idle_enable)
        spdk_poller_unregister(&rctx->idle_poller);

    rctx->running = false;
    log_info("Stopping server[%d],[%s:%d]....done\n", rctx->reactor_id,rctx->ip,rctx->port);
    return;
}
void _sys_fini() {
    int i;
    SPDK_ENV_FOREACH_CORE(i) {
        if(i != spdk_env_get_first_core())  {
            struct spdk_event * e = spdk_event_allocate(i,_per_reactor_stop,NULL,NULL);
            spdk_event_call(e);
        }
    }

    if(spdk_env_get_current_core() == spdk_env_get_first_core()) {
        _per_reactor_stop( NULL, NULL);
        while (reactor_reduce_state() != 0) //Wait until 
            spdk_delay_us(1000);

        //IF master
        log_info("Stoping app....\n");
        spdk_app_stop(0);
    }
}

int _ostore_boot(const objstore_impl_t *oimpl , int new) {
    //TODO get ostore global config
    //....
    const char *dev_list[] = {"Nvme0n1" , NULL ,NULL};
    int flags = 0;
    int rc;
    if(new) {
        rc = oimpl->mkfs(dev_list,flags);
        assert (rc == OSTORE_EXECUTE_OK);
    }
    rc = oimpl->mount(dev_list,flags);
    return rc;
}
int _msgr_boot(const msgr_server_if_t *smsgr_impl) {

    //TODO get msgr global config
    //....
    reactor_ctx_t *rctx = reactor_ctx();
    messager_conf_t conf = {
        .sock_type = 0,
        .ip = rctx->ip,
        .port = rctx->port,
        .on_recv_message = _on_recv_message,
        .on_send_message = _on_send_message,
        .data_buffer_alloc = alloc_data_buffer,
        .data_buffer_free = free_data_buffer
    };
    int rc = smsgr_impl->messager_init(&conf);
    if(rc) {
        return rc;
    }
    rc = smsgr_impl->messager_start();
    return rc;
}
void _per_reactor_boot(void * ctx , void *err) {
    (void)err;
    (void)ctx;
    reactor_ctx_t *rctx = reactor_ctx();

    rctx->idle_enable = g_idle;
    if(rctx->idle_enable) {
        rctx->idle_poller = spdk_poller_register(idle_poll,rctx, 100);
        rctx->idle_poll_start_us = now();
        rctx->idle_poll_exe_us = 1000;
    }

    //ObjectStore initialize
    rctx->os_impl = ostore_get_impl(g_store_type);
    _ostore_boot(rctx->os_impl,true);
    log_info("Booting object store, type =[%d]....done\n", g_store_type);


    //Msgr initialize
    rctx->msgr_impl = msgr_get_server_impl();
    _msgr_boot(rctx->msgr_impl);
    log_info("Booting messager ....done\n");

    rctx->running = true;
    // spdk_thread_get
    log_info("Booting server[%d],[%s:%d]....done\n", rctx->reactor_id,rctx->ip,rctx->port);
}
void _sys_init(void *arg) {
    (void)arg;
    int i;
    //prepare per reactor context
    SPDK_ENV_FOREACH_CORE(i) {
        reactor_ctx_t myctx = {
            .reactor_id = i,
            .ip = g_base_ip,
            .port = g_base_port + i,
        };
        memcpy(&g_reactor_ctxs[i], &myctx, sizeof(myctx));
    }


    SPDK_ENV_FOREACH_CORE(i) {
        if(i != spdk_env_get_first_core())  {
            struct spdk_event * e = spdk_event_allocate(i,_per_reactor_boot,NULL,NULL);
            spdk_event_call(e);
        }      
    }

    spdk_delay_us(1000);
    
    if(spdk_env_get_current_core() == spdk_env_get_first_core()) {
        _per_reactor_boot(NULL, NULL);       
        while (reactor_reduce_state() != spdk_env_get_core_count())
            spdk_delay_us(1000);

        log_info("All reactors are running\n");
    }
}


int spdk_app_run() {
    struct spdk_app_opts opts;
    spdk_app_opts_init(&opts);
    opts.reactor_mask = g_core_mask;
    opts.shutdown_cb = _sys_fini;
    opts.config_file = "spdk.conf";
    opts.print_level = 1;
    int rc = spdk_app_start(&opts , _sys_init , NULL);
    if(rc) {
        return -1;
    }
    spdk_app_fini();
    return 0;
}

int main( int argc , char **argv) {
    parse_args(argc ,argv);
    return spdk_app_run();
}
