#include "lpdrain.h"

#include "collector.h"
#include "lp.h"

#include <stdlib.h>
#include <string.h>

int rtc_lp_drain_init(rtc_lp_drain *d, rtc_ring *ring, size_t max_write, size_t step_bytes)
{
	memset(d, 0, sizeof(*d));
	d->ring = ring;
	d->raw_cap = step_bytes > max_write + 1 ? step_bytes : max_write + 1;
	d->max_recs = d->raw_cap / 2 + 1; /* a write is at least 2 bytes: kind + 1 */
	d->raw = malloc(d->raw_cap);
	d->recs = calloc(d->max_recs, sizeof(*d->recs));
	return d->raw && d->recs ? 0 : -1;
}

void rtc_lp_drain_free(rtc_lp_drain *d)
{
	free(d->raw);
	free(d->recs);
	d->raw = NULL;
	d->recs = NULL;
}

static void one_line(rtc_collector *c, const char *s, size_t n, uint64_t recv_ts, const rtc_lp_limits *lim)
{
	size_t len;
	uint64_t ts;
	unsigned dropped;
	enum rtc_lp_result r =
	    rtc_lp_parse(s, n, lim, c->scratch, c->cfg.max_record_bytes, &len, &ts, &dropped);
	if (dropped && (r == RTC_LP_OK || r == RTC_LP_NO_FIELDS))
		rtc_counter_add(&c->counters.lp_uint_dropped, dropped);
	switch (r) {
	case RTC_LP_OK:
		rtc_counter_add(&c->counters.lp_lines, 1);
		rtc_emit_record(c, c->scratch, len, ts ? ts : recv_ts);
		break;
	case RTC_LP_EMPTY:
	case RTC_LP_NO_FIELDS:
		break;
	case RTC_LP_MALFORMED:
		rtc_counter_add(&c->counters.lp_malformed, 1);
		break;
	case RTC_LP_BAD_TIMESTAMP:
		rtc_counter_add(&c->counters.lp_bad_timestamp, 1);
		break;
	case RTC_LP_TOO_LARGE:
		rtc_counter_add(&c->counters.lp_too_large, 1);
		break;
	}
}

size_t rtc_lp_drain_step(rtc_collector *c, rtc_lp_drain *d)
{
	uint64_t dropped = 0;
	size_t n = rtc_ring_read(d->ring, 0, d->raw, d->raw_cap, d->recs, d->max_recs, NULL, NULL, &dropped);
	if (dropped)
		rtc_counter_add(&c->counters.rmgr_drain_dropped, dropped);
	rtc_lp_limits lim = {c->clock_floor_ns, rtc_now_ns() + 86400ull * 1000000000ull};
	for (size_t i = 0; i < n; i++) {
		const rtc_ring_rec *r = &d->recs[i];
		if (r->len < 1 || r->payload[0] != RTC_REC_LP)
			continue;
		const char *p = (const char *)r->payload + 1, *end = (const char *)r->payload + r->len;
		while (p < end) {
			const char *nl = memchr(p, '\n', (size_t)(end - p));
			const char *eol = nl ? nl : end;
			one_line(c, p, (size_t)(eol - p), r->ts_ns, &lim);
			p = nl ? nl + 1 : end;
		}
	}
	return n;
}
