#include "shim.h"
#include <time.h>
static int64_t now_us(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000000LL+t.tv_nsec/1000;}
absolute_time_t get_absolute_time(void){return now_us();}
int64_t absolute_time_diff_us(absolute_time_t a, absolute_time_t b){return b-a;}
void busy_wait_us(uint64_t us){(void)us;}
