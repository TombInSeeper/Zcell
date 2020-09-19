#include "assert.h"

#include "util/bitmap.h"
#include "util/log.h"
#include "util/chrono.h"
#include "util/uint_test.h"


void seq_alloc_perf_test(bitmap_t *b) {
    int i;
    int hint = 0;
    uint64_t st = now();
    for ( i = 0 ; i < b->bit_length ; ++i) {
        hint = bitmap_find_next_set_and_clr(b, hint);
    }
    uint64_t end = now();
    log_info("seq allocavg_lat =  %lf ns",  (end - st ) / 128.0 * 1000.0);
}

void perf_test() {
    bitmap_t *b = bitmap_constructor(128,1); 
    
    seq_alloc_perf_test(b);


    bitmap_destructor(b);
}


void func_test() {
    bitmap_t *b = bitmap_constructor(256,1); 

    ASSERT_EQ(bitmap_get_bit(b,0),1);
    ASSERT_EQ(bitmap_get_bit(b,1),1);
    ASSERT_EQ(bitmap_get_bit(b,32),1);
    ASSERT_EQ(bitmap_get_bit(b,33),1);
    ASSERT_EQ(bitmap_get_bit(b,63),1);
    ASSERT_EQ(bitmap_get_bit(b,64),1);
    ASSERT_EQ(bitmap_get_bit(b,127),1);
    ASSERT_EQ(bitmap_get_bit(b,128),1);
    ASSERT_EQ(bitmap_get_bit(b,255),1);

    bitmap_set_bit(b,1);
    ASSERT_EQ(bitmap_get_bit(b,1),1);

    ASSERT_EQ(bitmap_find_next_set_and_clr(b,0), 0);
    ASSERT_EQ(bitmap_find_next_set_and_clr(b,0), 1);
    ASSERT_EQ(bitmap_find_next_set_and_clr(b,0), 2);
    ASSERT_EQ(bitmap_find_next_set_and_clr(b,0), 3);
    ASSERT_EQ(bitmap_find_next_set_and_clr(b,0), 4);

    bitmap_set_bit(b,2);
    ASSERT_EQ(bitmap_find_next_set_and_clr(b,0), 2);

    bitmap_destructor(b);
    log_info("Pass!\n");
}


int main() {
    func_test();
    perf_test();
    return 0;
}