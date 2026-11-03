/* Logging. Formats and writes to stderr, so it allocates in libc and blocks:
 * never call it from a path a real-time thread can wait on. */
#ifndef RTC_LOG_H
#define RTC_LOG_H

#include <stdbool.h>

enum rtc_log_level { RTC_LOG_DEBUG, RTC_LOG_INFO, RTC_LOG_WARN, RTC_LOG_ERROR };

extern bool rtc_log_debug_enabled;

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
void rtc_log(enum rtc_log_level level, const char *component, const char *fmt, ...);

#define RTC_DEBUG(c, ...)                                                                                    \
	do {                                                                                                 \
		if (rtc_log_debug_enabled)                                                                   \
			rtc_log(RTC_LOG_DEBUG, c, __VA_ARGS__);                                              \
	} while (0)
#define RTC_INFO(c, ...)  rtc_log(RTC_LOG_INFO, c, __VA_ARGS__)
#define RTC_WARN(c, ...)  rtc_log(RTC_LOG_WARN, c, __VA_ARGS__)
#define RTC_ERROR(c, ...) rtc_log(RTC_LOG_ERROR, c, __VA_ARGS__)

#endif
