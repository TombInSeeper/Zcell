#ifndef COMMON_H
#define COMMON_H

#include <emmintrin.h>
#include <stdint.h>
#include <malloc.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>



/**
 * 
 * DEBUG LEVEL
 * 
 * 
 */
#ifndef NDEBUG
#define MSGR_DEBUG 
#define MSGR_DEBUG_LEVEL 10
#else
#define MSGR_DEBUG 
#define MSGR_DEBUG_LEVEL 1
#endif




typedef void (*cb_func_t) (void* , int status_code);

static inline void core_mask_convert(const char *m , uint32_t cores[] , uint32_t *n)
{
    uint64_t cm;
    sscanf(m , "0x%lx" , &cm);
    uint64_t i;
    *n = 0;
    for (i = 0 ; i < 64 ; ++i) {
        if (( 1ull << i ) & cm ) {
            cores[*n] = i;
            *n = *n + 1;
        }
    }
}



//Okay... X86 only, little endian
typedef uint8_t  _u8;
typedef uint16_t _le16;
typedef uint32_t _le32;
typedef uint64_t _le64;

#define le16_to_cpu(u) (u)
#define cpu_to_le16(u) (u)
#define le32_to_cpu(u) (u)
#define cpu_to_le32(u) (u)
#define le64_to_cpu(u) (u)
#define cpu_to_le64(u) (u)


//Cancel struct aligned
#define _packed __attribute__((packed))


//concat macro function
#define LC_CONCAT2(s1, s2) s1##s2
#define LC_CONCAT(s1, s2) LC_CONCAT2(s1, s2)

//Mfence
#define mb()    asm volatile("mfence":::"memory")
#define rmb()   asm volatile("lfence":::"memory")
#define wmb()   asm volatile("sfence" ::: "memory")

#define CEIL_ALIGN(v , align) (((v) + (align) - 1) & (~((align)-1)) ) 
#define FLOOR_ALIGN(v , align) (((v)) & (~((align)-1)) ) 

#define SWAP(x,y) do \
{   unsigned char swap_temp[sizeof(x) == sizeof(y) ? (signed)sizeof(x) : -1]; \
    memcpy(swap_temp,&y,sizeof(x)); \
    memcpy(&y,&x,       sizeof(x)); \
    memcpy(&x,swap_temp,sizeof(x)); \
} while(0)

#endif