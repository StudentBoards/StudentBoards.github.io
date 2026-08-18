/* Host shims so svf.c/jtag.c logic can be tested without RP2040 hardware. */
#ifndef SHIM_H
#define SHIM_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define count_of(a) (sizeof(a)/sizeof((a)[0]))
typedef int64_t absolute_time_t;
absolute_time_t get_absolute_time(void);
int64_t absolute_time_diff_us(absolute_time_t a, absolute_time_t b);
void busy_wait_us(uint64_t us);
#endif
