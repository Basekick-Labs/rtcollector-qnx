/* Minimal MessagePack encoder into a caller buffer. Never allocates.
 * Once a write would overflow, the writer is marked failed and every later
 * call is a no-op; check rtc_mp_ok() at the end. */
#ifndef RTC_MSGPACK_H
#define RTC_MSGPACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint8_t *buf;
	size_t cap;
	size_t len;
	bool failed;
} rtc_mp;

void rtc_mp_init(rtc_mp *w, uint8_t *buf, size_t cap);
static inline bool rtc_mp_ok(const rtc_mp *w)
{
	return !w->failed;
}

void rtc_mp_nil(rtc_mp *w);
void rtc_mp_bool(rtc_mp *w, bool v);
void rtc_mp_int(rtc_mp *w, int64_t v);
/* Always float64, even for integral values: Arc decides a column's type from
 * its first non-nil value, and 3.0 sent as 3 would make it an integer column. */
void rtc_mp_f64(rtc_mp *w, double v);
void rtc_mp_str(rtc_mp *w, const char *s, size_t n);
void rtc_mp_cstr(rtc_mp *w, const char *s);
void rtc_mp_array(rtc_mp *w, uint32_t n);
void rtc_mp_map(rtc_mp *w, uint32_t n);

#endif
