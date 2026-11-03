#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

bool rtc_log_debug_enabled = false;

static const char *names[] = {"debug", "info", "warn", "error"};

void rtc_log(enum rtc_log_level level, const char *component, const char *fmt, ...)
{
	char line[1024];
	struct timespec ts;
	struct tm tm;
	char when[32] = "";
	if (clock_gettime(CLOCK_REALTIME, &ts) == 0 && gmtime_r(&ts.tv_sec, &tm))
		strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fprintf(stderr, "%s %s %s: %s\n", when, names[level], component, line);
}
