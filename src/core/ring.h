/* Single-producer ring of variable-length records with per-output claims.
 *
 * Positions are monotonic 64-bit byte offsets; the physical index is
 * pos & (cap - 1). Each record is a 20-byte header [u32 len][u64 seq]
 * [u64 ts_ns] followed by len payload bytes, and may wrap across the end of
 * the buffer.
 *
 * Ownership:
 *   producer: head, tail, next_seq, the bytes it writes
 *   output o: cursor[o], next_seq_expected[o], claim[o]
 *
 * The producer never waits. When it needs space it advances tail past the
 * oldest records (eviction). Before it overwrites any byte it checks every
 * output's claim; if a write would overwrite a byte at or after a claim, it
 * drops the new record instead (drop-newest) and counts it.
 *
 * An output reading the ring publishes a claim at its cursor, re-checks tail
 * (store-then-load on both sides, seq_cst: either the output sees the new
 * tail and moves its cursor, or the producer sees the claim and leaves the
 * bytes alone), copies whole records into its own buffer, and releases the
 * claim. It never hands ring memory to anything that outlives the copy.
 *
 * Drops are counted by sequence gaps: records the producer evicted before an
 * output read them show up as a jump in seq. Never by bytes.
 */
#ifndef RTC_RING_H
#define RTC_RING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTC_RING_HDR         20u
#define RTC_RING_MAX_OUTPUTS 4
#define RTC_RING_NO_CLAIM    UINT64_MAX

typedef struct {
	uint8_t *buf;
	uint64_t cap;        /* power of two */
	uint32_t max_record; /* payload bytes */
	unsigned noutputs;

	_Atomic uint64_t head;
	_Atomic uint64_t tail;
	uint64_t next_seq; /* producer only */

	_Atomic uint64_t claim[RTC_RING_MAX_OUTPUTS];
	uint64_t cursor[RTC_RING_MAX_OUTPUTS];
	uint64_t expect_seq[RTC_RING_MAX_OUTPUTS];

	/* Test seam: called by rtc_ring_read between claiming and copying. */
	void (*copy_hook)(void *arg);
	void *copy_hook_arg;
} rtc_ring;

enum rtc_push_result { RTC_PUSH_OK = 0, RTC_PUSH_DROPPED = 1, RTC_PUSH_OVERSIZE = 2 };

/* Allocates the buffer once. cap_bytes is rounded up to a power of two and
 * must hold at least one maximal record. Returns 0 or -1. */
int rtc_ring_init(rtc_ring *r, size_t cap_bytes, uint32_t max_record, unsigned noutputs);
void rtc_ring_free(rtc_ring *r);

/* Producer. Bounded work, no allocation, no blocking. */
enum rtc_push_result rtc_ring_push(rtc_ring *r, const uint8_t *payload, uint32_t len, uint64_t ts_ns);

/* One record copied out by rtc_ring_read. */
typedef struct {
	uint64_t seq;
	uint64_t ts_ns;
	uint32_t len;
	const uint8_t *payload; /* points into the caller's buffer */
} rtc_ring_rec;

/* Accept callback: return false to stop before this record (it stays for the
 * next read). */
typedef bool (*rtc_ring_accept)(void *arg, const rtc_ring_rec *rec);

/* Output o copies whole records into dst (dst_cap bytes) and recs (max_recs
 * entries). Returns the number of records copied; *dropped receives the
 * number of records this output lost to eviction since its last read. */
size_t rtc_ring_read(rtc_ring *r, unsigned o, uint8_t *dst, size_t dst_cap, rtc_ring_rec *recs,
                     size_t max_recs, rtc_ring_accept accept, void *accept_arg, uint64_t *dropped);

/* Bytes not yet read by output o (approximate, for flush decisions). */
uint64_t rtc_ring_pending(rtc_ring *r, unsigned o);

#endif
