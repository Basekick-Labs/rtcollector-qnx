/* rtc_publish: write one metric line to /dev/rtcollector (rtcollector for
 * QNX). Header-only; no library to link.
 *
 *   int fd = open("/dev/rtcollector", O_WRONLY);
 *   rtc_publish(fd, "control_loop,axis=x jitter_us=%di", jitter);
 *
 * Formats into a stack buffer and sends it with a single write(), adding the
 * trailing newline. Use this (or snprintf + write) rather than dprintf:
 * dprintf sends literal text one character per write, and the collector
 * does not reassemble lines split across writes.
 *
 * Returns the bytes written, or -1 with errno set: EMSGSIZE if the line is
 * longer than the collector accepts (4096 bytes by default) or than
 * RTC_PUBLISH_MAX; EBADF if the collector exited (reopen the device).
 *
 * Licence: Apache-2.0, like rtcollector for QNX.
 */
#ifndef RTC_PUBLISH_H
#define RTC_PUBLISH_H

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#ifndef RTC_PUBLISH_MAX
#define RTC_PUBLISH_MAX 1024
#endif

/* clang-format 18 and 23 disagree on the line after the attribute. */
// clang-format off
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static inline int rtc_publish(int fd, const char *fmt, ...)
// clang-format on
{
	char line[RTC_PUBLISH_MAX];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return -1;
	if ((size_t)n >= sizeof(line) - 1) {
		errno = EMSGSIZE;
		return -1;
	}
	line[n++] = '\n';
	return (int)write(fd, line, (size_t)n);
}

#endif
