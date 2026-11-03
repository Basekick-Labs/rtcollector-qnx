/* CPU utilisation from idle time, platform-independent. */
#ifndef RTC_CPUMATH_H
#define RTC_CPUMATH_H

#include <stdint.h>

/* Utilisation in percent over [wall0, wall1] given a core's cumulative idle
 * time at both ends (all ns). Returns -1 when the interval is empty. Idle
 * going backwards counts as 0; idle exceeding the interval (cores are read
 * one after another, so the two clocks are not sampled at the same instant)
 * is clamped, so the result is always within [0, 100]. */
double rtc_cpu_usage_percent(uint64_t idle0, uint64_t idle1, uint64_t wall0, uint64_t wall1);

#endif
