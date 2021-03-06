#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "pm.h"
#include "pm_impl.h"
#include "util/log.h"
#include "util/common.h"
#include "util/assert.h"

#define PM_LOG_REGION_SIZE 8192

struct pmem_t {    
    void *map_base;
    uint64_t log_region_ofst;
};

union pm_log_header_t {
    struct {
        uint64_t valid : 1;
        uint64_t rsv : 15;
        uint64_t nr_logs:16;
        uint64_t align_length:32;
    };
    uint8_t align[64];
};

struct pm_log_entry_t {
    void *paddr;
    uint64_t ofst;
    uint64_t length;
    uint64_t value[0];
};


extern struct pmem_t *pmem_open(const char *path, uint64_t cpu,  uint64_t *pmem_size) {
    struct pmem_t *p = calloc(1, sizeof(struct pmem_t));
    if(!p) {        
        return NULL;
    }

    // struct stat st_;
    // int rc = stat(path,&st_);
    // if(rc) {
    //     free(p);
    //     log_err("Cannot get stat of %s, errs: %s" , path , strerror(errno));
    //     return NULL;
    // }
    // uint64_t fsize = st_.st_size;
    
    // if(fsize & ((1 << 20) - 1 )) {
    //     log_err("Pmem file must be aligned to 2MiB\n");
    //     free(p);
    //     return NULL;
    // }

    size_t fsize = 4ull << 30;
    int is_pmem;
    p->map_base = pmem_map_file(path, fsize , PMEM_FILE_CREATE , 0666 , NULL, &is_pmem);
    assert(p->map_base);
    if(!is_pmem) {
        log_info("\n");\
        log_info("Using **DRAM** as PMEM\n");
        log_info("\n");
    }

    *pmem_size = fsize;
    //Per cpu
    p->log_region_ofst = 4096 + cpu * PM_LOG_REGION_SIZE;
    
    return p;
}

extern void pmem_read(struct pmem_t *pmem, void *dest, uint64_t offset , size_t length){
    void *src = (char*)(pmem->map_base) + offset;
    memcpy(dest, src, length);
}

extern void pmem_write(struct pmem_t *pmem, int sync, const void* src, uint64_t offset, size_t length){
    void *dst = pmem->map_base + offset;
    // nvmem_memcpy(sync,dst,src,length);
    if(sync) {
        pmem_memcpy_persist(dst,  src , length);
    } else {
        pmem_memcpy_nodrain(dst,  src , length);
    }
}

extern void pmem_recovery(struct pmem_t *pmem) {
    uint64_t offset_log_reg = pmem->log_region_ofst;
    union pm_log_header_t lh;
    char  pm_log_pload [4096];
    uint64_t offset_ulog_pload = offset_log_reg + sizeof(lh);
    pmem_read(pmem, &lh,offset_log_reg,sizeof(lh));
    
    uint64_t cpu = (offset_log_reg >> 12) - 1 ;
    if(lh.valid) {
        log_critical("Pmem transaction in cpu[%lu] need to replay.\n" , cpu);
        uint16_t n = lh.nr_logs;
        uint32_t len = lh.align_length;
        assert( len % 256 == 0);
        pmem_read(pmem,pm_log_pload,offset_ulog_pload,len);
        int i;
        char *cur_entry = pm_log_pload;
        for(i = 0; i < n ; ++i) {
            struct pm_log_entry_t* e = (void*)cur_entry;
            assert(e->length % 64 == 0);
            assert(e->ofst % 64 == 0);
            pmem_write(pmem, 1 , e->value, e->ofst, e->length);
            cur_entry += sizeof(*e) + e->length;
        }
        memset(&lh,0,sizeof(lh));
        pmem_write(pmem,1,&lh,offset_log_reg,sizeof(lh));
        // log_critical("Pmem transaction roll forward done.\n");
        log_critical("Pmem transaction  in cpu[%lu] replay done.\n" , cpu);
    }
}



union pmem_transaction_t {
    struct {
        union  pm_log_header_t lh;
        struct pm_log_entry_t  le[0];
    };
    uint8_t align[PM_LOG_REGION_SIZE];
};

extern union pmem_transaction_t* pmem_transaction_alloc(struct pmem_t *pmem) {
    union pmem_transaction_t* p = malloc(sizeof(union pmem_transaction_t));
    if(!p) {
        return p;
    }
    memset(&p->lh , 0 , sizeof(p->lh));
    return p;
}


extern bool pmem_transaction_add(struct pmem_t *pmem, union pmem_transaction_t *tx,
    const uint64_t pmem_ofst, void* mem_addr, size_t len, void *new_value)  
{
    uint32_t log_len = sizeof(struct pm_log_entry_t) + len;
    log_debug("Add log: pofst=0x%lx , maddr:%p, length=%lu , log_addr=%u\n", 
        pmem_ofst, mem_addr , len , tx->lh.align_length );
    
    
    struct pm_log_entry_t *pl = (void*)((char*)(tx->le)+tx->lh.align_length);

    tx->lh.align_length += log_len;
    tx->lh.nr_logs++;    

    if(tx->lh.align_length > PM_LOG_REGION_SIZE) {
        log_err("Cannot add more log into this Transaction , now align length=%u\n",
            tx->lh.align_length);
        return false;
    }


    
    
    pl->length = len;
    pl->ofst   = pmem_ofst; 
    pl->paddr  = mem_addr; 

    memcpy(pl->value , new_value, len);

    return true;
}

extern bool pmem_transaction_apply(struct pmem_t *pmem, union pmem_transaction_t *tx) {

    log_debug("Transaction orig length %u, nr_logs:%u \n" , 
        tx->lh.align_length,
        tx->lh.nr_logs );
    tx->lh.align_length = CEIL_ALIGN(tx->lh.align_length , 256);
    log_debug("Transaction length ceil align to %u\n" , tx->lh.align_length );

    //Step1. 
    //.....
    pmem_write(pmem, 1 , tx->le, 
        pmem->log_region_ofst + sizeof(union pm_log_header_t),
        tx->lh.align_length);
    
    //Step2.
    //....
    pmem_write(pmem,1,&tx->lh,pmem->log_region_ofst, sizeof(tx->lh));

    //Step3.
    size_t i;
    struct pm_log_entry_t *pl = tx->le;
    for(i = 0 ; i < tx->lh.nr_logs; ++i) {
        pmem_write(pmem,0, pl->value, pl->ofst, pl->length);
        pl = (void*)((char*)pl + sizeof(struct pm_log_entry_t) + pl->length);
    }
    
    pmem_drain();


    uint32_t nr_logs = tx->lh.nr_logs;

    //Step4. 
    memset(&tx->lh , 0 , sizeof(tx->lh));
    pmem_write(pmem,1, &tx->lh, pmem->log_region_ofst, sizeof(tx->lh));

    //Step5. apply 延迟修改的内存内容
    pl = tx->le;
    for(i = 0 ; i < nr_logs; ++i) {
        if(pl->paddr) {
            log_debug("Deffered apply memory update\n");
            memcpy(pl->paddr, pl->value, pl->length);
        }
        pl = (void*)((char*)pl + sizeof(struct pm_log_entry_t) + pl->length);
    }

    return true;
}

extern void pmem_transaction_free(struct pmem_t *pmem, union pmem_transaction_t *tx) {
    free(tx);
}

extern void pmem_close(struct pmem_t *pmem) {
    free(pmem);
}