/* Drains a ring of raw line-protocol writes (filled by /dev/rtcollector)
 * into the collector's ring as binary records. Runs on the collection
 * thread, the ring's only reader. Each ring record is one write:
 * [u8 RTC_REC_LP][bytes], its header time the receive time. A write holds
 * one or more lines; a line without a trailing newline at the end of the
 * write is still a line. Lines split across writes are not reassembled. */
#ifndef RTC_LPDRAIN_H
#define RTC_LPDRAIN_H

#include "ring.h"

#include <stddef.h>
#include <stdint.h>

enum { RTC_REC_LP = 2 };

struct rtc_collector;

typedef struct {
	rtc_ring *ring;
	uint8_t *raw; /* copies of writes, one drain step */
	size_t raw_cap;
	rtc_ring_rec *recs;
	size_t max_recs;
} rtc_lp_drain;

/* Allocates the drain's buffers for writes of up to max_write bytes and
 * steps of up to step_bytes. Returns 0 or -1. */
int rtc_lp_drain_init(rtc_lp_drain *d, rtc_ring *ring, size_t max_write, size_t step_bytes);
void rtc_lp_drain_free(rtc_lp_drain *d);

/* Converts up to step_bytes of pending writes. Returns the number of writes
 * consumed (0 when the ring is empty). */
size_t rtc_lp_drain_step(struct rtc_collector *c, rtc_lp_drain *d);

#endif
