#include "ring.h"

#include <stdlib.h>
#include <string.h>

static uint64_t round_pow2(uint64_t v)
{
	uint64_t p = 1;
	while (p < v && p < (UINT64_C(1) << 62))
		p <<= 1;
	return p;
}

int rtc_ring_init(rtc_ring *r, size_t cap_bytes, uint32_t max_record, unsigned noutputs)
{
	memset(r, 0, sizeof(*r));
	if (noutputs == 0 || noutputs > RTC_RING_MAX_OUTPUTS || max_record == 0)
		return -1;
	uint64_t cap = round_pow2(cap_bytes);
	if (cap < (uint64_t)RTC_RING_HDR + max_record)
		return -1;
	r->buf = malloc((size_t)cap);
	if (!r->buf)
		return -1;
	/* Touch every page now: a page fault later would happen on the path a
	 * writer waits on, and resolving it can wait on locks held by
	 * lower-priority threads. */
	memset(r->buf, 0, (size_t)cap);
	r->cap = cap;
	r->max_record = max_record;
	r->noutputs = noutputs;
	atomic_init(&r->head, 0);
	atomic_init(&r->tail, 0);
	for (unsigned o = 0; o < RTC_RING_MAX_OUTPUTS; o++)
		atomic_init(&r->claim[o], RTC_RING_NO_CLAIM);
	return 0;
}

void rtc_ring_free(rtc_ring *r)
{
	free(r->buf);
	r->buf = NULL;
}

static void copy_in(rtc_ring *r, uint64_t pos, const void *src, size_t n)
{
	uint64_t i = pos & (r->cap - 1);
	size_t first = (size_t)(r->cap - i) < n ? (size_t)(r->cap - i) : n;
	memcpy(r->buf + i, src, first);
	if (first < n)
		memcpy(r->buf, (const uint8_t *)src + first, n - first);
}

static void copy_out(const rtc_ring *r, uint64_t pos, void *dst, size_t n)
{
	uint64_t i = pos & (r->cap - 1);
	size_t first = (size_t)(r->cap - i) < n ? (size_t)(r->cap - i) : n;
	memcpy(dst, r->buf + i, first);
	if (first < n)
		memcpy((uint8_t *)dst + first, r->buf, n - first);
}

static void put_hdr(uint8_t h[RTC_RING_HDR], uint32_t len, uint64_t seq, uint64_t ts)
{
	for (int i = 0; i < 4; i++)
		h[i] = (uint8_t)(len >> (8 * i));
	for (int i = 0; i < 8; i++) {
		h[4 + i] = (uint8_t)(seq >> (8 * i));
		h[12 + i] = (uint8_t)(ts >> (8 * i));
	}
}

static void get_hdr(const uint8_t h[RTC_RING_HDR], uint32_t *len, uint64_t *seq, uint64_t *ts)
{
	uint32_t l = 0;
	uint64_t s = 0, t = 0;
	for (int i = 0; i < 4; i++)
		l |= (uint32_t)h[i] << (8 * i);
	for (int i = 0; i < 8; i++) {
		s |= (uint64_t)h[4 + i] << (8 * i);
		t |= (uint64_t)h[12 + i] << (8 * i);
	}
	*len = l;
	*seq = s;
	*ts = t;
}

static uint64_t min_claim(rtc_ring *r)
{
	uint64_t m = RTC_RING_NO_CLAIM;
	for (unsigned o = 0; o < r->noutputs; o++) {
		uint64_t c = atomic_load_explicit(&r->claim[o], memory_order_seq_cst);
		if (c < m)
			m = c;
	}
	return m;
}

enum rtc_push_result rtc_ring_push(rtc_ring *r, const uint8_t *payload, uint32_t len, uint64_t ts_ns)
{
	if (len == 0 || len > r->max_record)
		return RTC_PUSH_OVERSIZE;
	uint64_t total = (uint64_t)RTC_RING_HDR + len;
	uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
	uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);

	if (head + total - tail > r->cap) {
		/* Evict whole records from the oldest end, but never a record at or
		 * after a claim: an output is copying it. If the space cannot be
		 * made without that, evict nothing and drop the new record. The
		 * headers read here are bytes this thread wrote. The loop frees at
		 * most total / (RTC_RING_HDR + 1) + 1 records. */
		uint64_t limit = min_claim(r);
		uint64_t t = tail;
		while (head + total - t > r->cap) {
			if (t >= limit)
				return RTC_PUSH_DROPPED;
			uint8_t h[RTC_RING_HDR];
			uint32_t l;
			uint64_t s, ts;
			copy_out(r, t, h, RTC_RING_HDR);
			get_hdr(h, &l, &s, &ts);
			t += RTC_RING_HDR + l;
		}
		atomic_store_explicit(&r->tail, t, memory_order_seq_cst);
	}

	/* The bytes this write overwrites are those at logical positions below
	 * head + total - cap. None of them may be at or after a claim. This
	 * load comes after the tail store (store-then-load, seq_cst), which is
	 * what makes the reader's claim-then-check-tail safe: either the reader
	 * saw the new tail and moved past it, or this load sees its claim. */
	uint64_t claim = min_claim(r);
	if (claim != RTC_RING_NO_CLAIM && head + total > claim + r->cap)
		return RTC_PUSH_DROPPED;

	uint8_t h[RTC_RING_HDR];
	put_hdr(h, len, r->next_seq, ts_ns);
	copy_in(r, head, h, RTC_RING_HDR);
	copy_in(r, head + RTC_RING_HDR, payload, len);
	r->next_seq++;
	atomic_store_explicit(&r->head, head + total, memory_order_release);
	return RTC_PUSH_OK;
}

size_t rtc_ring_read(rtc_ring *r, unsigned o, uint8_t *dst, size_t dst_cap, rtc_ring_rec *recs,
                     size_t max_recs, rtc_ring_accept accept, void *accept_arg, uint64_t *dropped)
{
	/* min_claim() looks at outputs below noutputs only: a reader past them
	 * would copy bytes the producer may overwrite meanwhile. A wiring bug,
	 * never a run-time condition. */
	if (o >= r->noutputs)
		abort();
	uint64_t cur = r->cursor[o];
	*dropped = 0;

	/* Claim, then re-check tail; repeat until the claim is at or after tail. */
	for (;;) {
		uint64_t t = atomic_load_explicit(&r->tail, memory_order_seq_cst);
		if (cur < t)
			cur = t;
		atomic_store_explicit(&r->claim[o], cur, memory_order_seq_cst);
		uint64_t t2 = atomic_load_explicit(&r->tail, memory_order_seq_cst);
		if (t2 <= cur)
			break;
		cur = t2;
	}
	if (r->copy_hook)
		r->copy_hook(r->copy_hook_arg, o);

	uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
	size_t used = 0, n = 0;
	while (cur < head && n < max_recs) {
		uint8_t h[RTC_RING_HDR];
		uint32_t len;
		uint64_t seq, ts;
		if (head - cur < RTC_RING_HDR)
			break; /* cannot happen with a correct producer */
		copy_out(r, cur, h, RTC_RING_HDR);
		get_hdr(h, &len, &seq, &ts);
		if (len == 0 || len > r->max_record || head - cur - RTC_RING_HDR < len)
			break; /* cannot happen with a correct producer */
		if (len > dst_cap - used)
			break;
		copy_out(r, cur + RTC_RING_HDR, dst + used, len);
		rtc_ring_rec rec = {seq, ts, len, dst + used};
		if (accept && !accept(accept_arg, &rec))
			break;
		if (seq > r->expect_seq[o])
			*dropped += seq - r->expect_seq[o];
		r->expect_seq[o] = seq + 1;
		recs[n++] = rec;
		used += len;
		cur += RTC_RING_HDR + len;
	}

	atomic_store_explicit(&r->claim[o], RTC_RING_NO_CLAIM, memory_order_release);
	r->cursor[o] = cur;
	return n;
}

uint64_t rtc_ring_pending(rtc_ring *r, unsigned o)
{
	uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
	uint64_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
	uint64_t cur = r->cursor[o] < tail ? tail : r->cursor[o];
	return head > cur ? head - cur : 0;
}

uint64_t rtc_ring_unseen(const rtc_ring *r, unsigned o)
{
	return r->next_seq > r->expect_seq[o] ? r->next_seq - r->expect_seq[o] : 0;
}
