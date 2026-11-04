#include "t.h"

#include "../src/core/ring.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* Payload used by these tests: [u64 id][filler...][u32 crc of the rest]. */
static uint32_t crc32(const uint8_t *p, size_t n)
{
	uint32_t c = 0xffffffffu;
	for (size_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
	}
	return ~c;
}

static uint32_t make_payload(uint8_t *p, uint64_t id, uint32_t len)
{
	memcpy(p, &id, 8);
	for (uint32_t i = 8; i < len - 4; i++)
		p[i] = (uint8_t)(id * 31 + i);
	uint32_t c = crc32(p, len - 4);
	memcpy(p + len - 4, &c, 4);
	return len;
}

static bool check_payload(const uint8_t *p, uint32_t len, uint64_t *id)
{
	if (len < 12)
		return false;
	uint32_t c;
	memcpy(&c, p + len - 4, 4);
	memcpy(id, p, 8);
	return c == crc32(p, len - 4);
}

static void push_n(rtc_ring *r, uint64_t first, unsigned n, uint32_t len)
{
	uint8_t p[512];
	for (unsigned i = 0; i < n; i++) {
		make_payload(p, first + i, len);
		CHECK(rtc_ring_push(r, p, len, 1000 + first + i) == RTC_PUSH_OK);
	}
}

static void roundtrip_and_wrap_at_every_offset(void)
{
	/* For every start offset in a 256-byte ring, a record crossing the end
	 * must come back intact. */
	for (unsigned off = 0; off < 256; off++) {
		rtc_ring r;
		CHECK(rtc_ring_init(&r, 256, 100, 1) == 0);
		uint8_t dst[512];
		rtc_ring_rec recs[16];
		uint64_t dropped;
		/* Advance head/tail by `off` bytes using a record of that size. */
		if (off >= RTC_RING_HDR + 12) {
			uint8_t f[256];
			uint32_t fl = off - RTC_RING_HDR;
			if (fl <= 100) {
				make_payload(f, 999, fl);
				rtc_ring_push(&r, f, fl, 0);
				rtc_ring_read(&r, 0, dst, sizeof(dst), recs, 16, NULL, NULL, &dropped);
			}
		}
		uint64_t seq0 = r.next_seq;
		push_n(&r, 1, 3, 60);
		size_t n = rtc_ring_read(&r, 0, dst, sizeof(dst), recs, 16, NULL, NULL, &dropped);
		CHECK_EQ_U(n, 3);
		CHECK_EQ_U(dropped, 0);
		for (size_t i = 0; i < n; i++) {
			uint64_t id;
			CHECK(check_payload(recs[i].payload, recs[i].len, &id));
			CHECK_EQ_U(id, 1 + i);
			CHECK_EQ_U(recs[i].seq, seq0 + i);
			CHECK_EQ_U(recs[i].ts_ns, 1001 + i);
		}
		rtc_ring_free(&r);
	}
}

static void eviction_is_counted_by_seq_gap(void)
{
	rtc_ring r;
	CHECK(rtc_ring_init(&r, 256, 100, 1) == 0);
	/* 80-byte records (20 header + 60): 3 fit in 256. Push 10. */
	push_n(&r, 0, 10, 60);
	uint8_t dst[512];
	rtc_ring_rec recs[16];
	uint64_t dropped;
	size_t n = rtc_ring_read(&r, 0, dst, sizeof(dst), recs, 16, NULL, NULL, &dropped);
	CHECK_EQ_U(n, 3);
	CHECK_EQ_U(dropped, 7);
	uint64_t id;
	CHECK(check_payload(recs[0].payload, recs[0].len, &id) && id == 7);
	/* Nothing more, nothing double counted. */
	n = rtc_ring_read(&r, 0, dst, sizeof(dst), recs, 16, NULL, NULL, &dropped);
	CHECK_EQ_U(n, 0);
	CHECK_EQ_U(dropped, 0);
	rtc_ring_free(&r);
}

static void oversize_is_refused(void)
{
	rtc_ring r;
	CHECK(rtc_ring_init(&r, 256, 100, 1) == 0);
	uint8_t p[200] = {0};
	CHECK(rtc_ring_push(&r, p, 101, 0) == RTC_PUSH_OVERSIZE);
	CHECK(rtc_ring_push(&r, p, 0, 0) == RTC_PUSH_OVERSIZE);
	rtc_ring_free(&r);
	CHECK(rtc_ring_init(&r, 64, 100, 1) == -1); /* cannot hold one record */
}

/* While output 0 holds a claim (inside the copy hook), the producer must not
 * overwrite claimed bytes: it drops the new record instead. */
typedef struct {
	rtc_ring *r;
	int pushed_ok, dropped_newest;
} hook_ctx;

static void push_inside_claim(void *arg, unsigned o)
{
	(void)o;
	hook_ctx *h = arg;
	uint8_t p[128];
	for (int i = 0; i < 6; i++) {
		make_payload(p, 100 + (uint64_t)i, 60);
		enum rtc_push_result res = rtc_ring_push(h->r, p, 60, 0);
		if (res == RTC_PUSH_OK)
			h->pushed_ok++;
		else if (res == RTC_PUSH_DROPPED)
			h->dropped_newest++;
	}
	h->r->copy_hook = NULL;
}

static void claim_blocks_overwrite(void)
{
	rtc_ring r;
	CHECK(rtc_ring_init(&r, 256, 100, 1) == 0);
	push_n(&r, 0, 3, 60); /* full */
	hook_ctx h = {&r, 0, 0};
	r.copy_hook = push_inside_claim;
	r.copy_hook_arg = &h;
	uint8_t dst[512];
	rtc_ring_rec recs[16];
	uint64_t dropped;
	size_t n = rtc_ring_read(&r, 0, dst, sizeof(dst), recs, 16, NULL, NULL, &dropped);
	CHECK_EQ_U(h.pushed_ok, 0);
	CHECK_EQ_U(h.dropped_newest, 6);
	/* The three claimed records come back intact. */
	CHECK_EQ_U(n, 3);
	for (size_t i = 0; i < n; i++) {
		uint64_t id;
		CHECK(check_payload(recs[i].payload, recs[i].len, &id));
		CHECK_EQ_U(id, i);
	}
	/* After the claim is released, pushes succeed again. */
	push_n(&r, 50, 2, 60);
	rtc_ring_free(&r);
}

static void two_outputs_independent(void)
{
	rtc_ring r;
	CHECK(rtc_ring_init(&r, 1024, 100, 2) == 0);
	push_n(&r, 0, 5, 40);
	uint8_t dst[1024];
	rtc_ring_rec recs[16];
	uint64_t dropped;
	CHECK_EQ_U(rtc_ring_read(&r, 0, dst, sizeof(dst), recs, 16, NULL, NULL, &dropped), 5);
	CHECK_EQ_U(rtc_ring_pending(&r, 0), 0);
	CHECK(rtc_ring_pending(&r, 1) > 0);
	/* Output 1 reads two at a time. */
	CHECK_EQ_U(rtc_ring_read(&r, 1, dst, 2 * 40, recs, 16, NULL, NULL, &dropped), 2);
	CHECK_EQ_U(rtc_ring_read(&r, 1, dst, sizeof(dst), recs, 16, NULL, NULL, &dropped), 3);
	CHECK_EQ_U(dropped, 0);
	rtc_ring_free(&r);
}

static bool stop_at_id3(void *arg, const rtc_ring_rec *rec)
{
	(void)arg;
	uint64_t id;
	check_payload(rec->payload, rec->len, &id);
	return id != 3;
}

static void accept_callback_leaves_rest(void)
{
	rtc_ring r;
	CHECK(rtc_ring_init(&r, 1024, 100, 1) == 0);
	push_n(&r, 0, 6, 40);
	uint8_t dst[1024];
	rtc_ring_rec recs[16];
	uint64_t dropped;
	CHECK_EQ_U(rtc_ring_read(&r, 0, dst, sizeof(dst), recs, 16, stop_at_id3, NULL, &dropped), 3);
	CHECK_EQ_U(rtc_ring_read(&r, 0, dst, sizeof(dst), recs, 16, NULL, NULL, &dropped), 3);
	uint64_t id;
	check_payload(recs[0].payload, recs[0].len, &id);
	CHECK_EQ_U(id, 3);
	rtc_ring_free(&r);
}

/* --- stress --------------------------------------------------------------- */

#define STRESS_N 200000

typedef struct {
	rtc_ring *r;
	_Atomic bool done;
	uint64_t dropped_newest;
} prod_ctx;

static void *producer(void *arg)
{
	prod_ctx *p = arg;
	uint8_t buf[256];
	for (uint64_t id = 0; id < STRESS_N; id++) {
		uint32_t len = 12 + (uint32_t)((id * 7919) % 180);
		make_payload(buf, id, len);
		if (rtc_ring_push(p->r, buf, len, id) == RTC_PUSH_DROPPED)
			p->dropped_newest++;
	}
	atomic_store(&p->done, true);
	return NULL;
}

static _Atomic unsigned hook_calls;

static void slow_copy(void *arg, unsigned o)
{
	(void)o;
	(void)arg;
	/* Every few reads, stall inside the claim so the producer runs into it
	 * and has to choose between eviction and drop-newest mid-copy. */
	if (atomic_fetch_add(&hook_calls, 1) % 40 == 0) {
		struct timespec s = {0, 200000};
		nanosleep(&s, NULL);
	}
}

static void stress_every_record_delivered_once_or_counted(void)
{
	rtc_ring r;
	CHECK(rtc_ring_init(&r, 4096, 200, 1) == 0);
	r.copy_hook = slow_copy;
	prod_ctx p = {&r, false, 0};
	pthread_t t;
	pthread_create(&t, NULL, producer, &p);

	uint8_t *dst = malloc(2048);
	rtc_ring_rec recs[256];
	uint64_t delivered = 0, gaps = 0, last_id = 0, bad = 0, order = 0;
	bool any = false;
	for (unsigned iter = 0;; iter++) {
		bool fin = atomic_load(&p.done);
		if (iter % 16 == 15) {
			/* Stall outside any claim too, so the producer must evict. */
			struct timespec s = {0, 300000};
			nanosleep(&s, NULL);
		}
		uint64_t dropped;
		size_t n = rtc_ring_read(&r, 0, dst, 2048, recs, 256, NULL, NULL, &dropped);
		gaps += dropped;
		for (size_t i = 0; i < n; i++) {
			uint64_t id;
			if (!check_payload(recs[i].payload, recs[i].len, &id))
				bad++;
			if (any && id <= last_id)
				order++;
			last_id = id;
			any = true;
			delivered++;
		}
		if (fin && n == 0 && rtc_ring_pending(&r, 0) == 0)
			break;
	}
	pthread_join(t, NULL);
	CHECK_EQ_U(bad, 0);
	CHECK_EQ_U(order, 0);
	CHECK_EQ_U(delivered + gaps + p.dropped_newest, STRESS_N);
	CHECK(gaps > 0);             /* the test must actually exercise eviction */
	CHECK(p.dropped_newest > 0); /* and the claim path */
	fprintf(stderr, "     stress: delivered=%llu evicted=%llu dropped_newest=%llu\n",
	        (unsigned long long)delivered, (unsigned long long)gaps,
	        (unsigned long long)p.dropped_newest);
	free(dst);
	rtc_ring_free(&r);
}

/* Eviction must not pass another output's claim: with output 0 copying,
 * output 1 must not lose records silently (review H2). */
typedef struct {
	rtc_ring *r;
	enum rtc_push_result res;
} one_push;

static void push_one(void *arg, unsigned o)
{
	(void)o;
	one_push *p = arg;
	uint8_t b[128];
	make_payload(b, 77, 60);
	p->res = rtc_ring_push(p->r, b, 60, 0);
	p->r->copy_hook = NULL;
}

static void eviction_respects_claims_of_every_output(void)
{
	rtc_ring r;
	CHECK(rtc_ring_init(&r, 128, 100, 2) == 0);
	push_n(&r, 0, 1, 60); /* 80 of 128 bytes */
	one_push p = {&r, RTC_PUSH_OK};
	r.copy_hook = push_one;
	r.copy_hook_arg = &p;
	uint8_t dst[256];
	rtc_ring_rec recs[4];
	uint64_t dropped;
	CHECK_EQ_U(rtc_ring_read(&r, 0, dst, sizeof(dst), recs, 4, NULL, NULL, &dropped), 1);
	CHECK(p.res == RTC_PUSH_DROPPED); /* evicting seq 0 would pass output 0's claim */
	/* Output 1 still gets seq 0. */
	CHECK_EQ_U(rtc_ring_read(&r, 1, dst, sizeof(dst), recs, 4, NULL, NULL, &dropped), 1);
	CHECK_EQ_U(dropped, 0);
	CHECK_EQ_U(rtc_ring_unseen(&r, 1), 0);
	rtc_ring_free(&r);
}

/* An eviction that empties the ring is still counted once the producer stops. */
static void unseen_counts_evictions_without_a_later_record(void)
{
	rtc_ring r;
	CHECK(rtc_ring_init(&r, 256, 100, 1) == 0);
	push_n(&r, 0, 10, 60);
	CHECK_EQ_U(rtc_ring_unseen(&r, 0), 10);
	uint8_t dst[512];
	rtc_ring_rec recs[16];
	uint64_t dropped;
	rtc_ring_read(&r, 0, dst, sizeof(dst), recs, 16, NULL, NULL, &dropped);
	CHECK_EQ_U(rtc_ring_unseen(&r, 0), 0);
	rtc_ring_free(&r);
}

/* Several outputs holding claims at once: the producer respects the oldest,
 * whichever output holds it. Output `late` has already read past seq 0; while
 * output `old` copies seq 0, `late` claims too, and a push that would evict
 * seq 0 is dropped instead. Every assignment of the roles to outputs 0-2. */
typedef struct {
	rtc_ring *r;
	unsigned old, late;
	enum rtc_push_result res;
	size_t inner_n;
	bool pushed;
} nested_ctx;

static void nested_claims(void *arg, unsigned o)
{
	nested_ctx *h = arg;
	if (o == h->old) {
		uint8_t dst[512];
		rtc_ring_rec recs[8];
		uint64_t dropped;
		h->inner_n = rtc_ring_read(h->r, h->late, dst, sizeof(dst), recs, 8, NULL, NULL, &dropped);
	} else if (o == h->late && !h->pushed) {
		uint8_t b[128];
		make_payload(b, 99, 60);
		h->res = rtc_ring_push(h->r, b, 60, 0);
		h->pushed = true;
	}
}

static void concurrent_claims_protect_the_oldest(void)
{
	for (unsigned old = 0; old < 3; old++)
		for (unsigned late = 0; late < 3; late++) {
			if (late == old)
				continue;
			unsigned idle = 3 - old - late;
			rtc_ring r;
			CHECK(rtc_ring_init(&r, 256, 100, 3) == 0);
			push_n(&r, 0, 3, 60); /* 240 of 256 bytes */
			uint8_t dst[512];
			rtc_ring_rec recs[8];
			uint64_t dropped;
			CHECK_EQ_U(rtc_ring_read(&r, late, dst, sizeof(dst), recs, 1, NULL, NULL, &dropped),
			           1);
			nested_ctx h = {&r, old, late, RTC_PUSH_OK, 0, false};
			r.copy_hook = nested_claims;
			r.copy_hook_arg = &h;
			size_t n = rtc_ring_read(&r, old, dst, sizeof(dst), recs, 8, NULL, NULL, &dropped);
			r.copy_hook = NULL;
			CHECK(h.pushed);
			CHECK(h.res == RTC_PUSH_DROPPED); /* evicting seq 0 would pass old's claim */
			CHECK_EQ_U(h.inner_n, 2);
			CHECK_EQ_U(n, 3);
			for (size_t i = 0; i < n; i++) {
				uint64_t id;
				CHECK(check_payload(recs[i].payload, recs[i].len, &id));
				CHECK_EQ_U(id, i);
			}
			/* The output that never claimed still has everything. */
			CHECK_EQ_U(rtc_ring_read(&r, idle, dst, sizeof(dst), recs, 8, NULL, NULL, &dropped),
			           3);
			CHECK_EQ_U(dropped, 0);
			rtc_ring_free(&r);
		}
}

/* Reading as an output the ring was not sized for is a wiring bug: the
 * producer would ignore that claim. It aborts instead of copying. */
static void read_past_noutputs_aborts(void)
{
	fflush(stderr);
	pid_t pid = fork();
	if (pid == 0) {
		rtc_ring r;
		if (rtc_ring_init(&r, 256, 100, 2) != 0)
			_exit(3);
		uint8_t dst[256];
		rtc_ring_rec recs[4];
		uint64_t dropped;
		rtc_ring_read(&r, 2, dst, sizeof(dst), recs, 4, NULL, NULL, &dropped);
		_exit(0);
	}
	int st = 0;
	CHECK(pid > 0 && waitpid(pid, &st, 0) == pid);
	CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);
}

/* Three readers on their own threads and one producer (one sender thread per
 * output): each reader gets every record intact and in order, or counts it. */
typedef struct {
	rtc_ring *r;
	prod_ctx *p;
	unsigned o;
	uint64_t delivered, gaps, bad, order;
} reader_ctx;

static void *reader(void *arg)
{
	reader_ctx *x = arg;
	uint8_t *dst = malloc(2048);
	rtc_ring_rec recs[256];
	uint64_t last_id = 0;
	bool any = false;
	for (unsigned iter = 0;; iter++) {
		bool fin = atomic_load(&x->p->done);
		if (iter % (13 + x->o) == 0) {
			struct timespec s = {0, 200000 + 100000 * x->o};
			nanosleep(&s, NULL);
		}
		uint64_t dropped;
		size_t n = rtc_ring_read(x->r, x->o, dst, 2048, recs, 256, NULL, NULL, &dropped);
		x->gaps += dropped;
		for (size_t i = 0; i < n; i++) {
			uint64_t id;
			if (!check_payload(recs[i].payload, recs[i].len, &id))
				x->bad++;
			if (any && id <= last_id)
				x->order++;
			last_id = id;
			any = true;
			x->delivered++;
		}
		if (fin && n == 0 && rtc_ring_pending(x->r, x->o) == 0)
			break;
	}
	free(dst);
	return NULL;
}

static void stress_three_reader_threads(void)
{
	rtc_ring r;
	CHECK(rtc_ring_init(&r, 4096, 200, 3) == 0);
	r.copy_hook = slow_copy;
	prod_ctx p = {&r, false, 0};
	reader_ctx x[3];
	pthread_t rt[3], pt;
	for (unsigned i = 0; i < 3; i++) {
		x[i] = (reader_ctx){&r, &p, i, 0, 0, 0, 0};
		pthread_create(&rt[i], NULL, reader, &x[i]);
	}
	pthread_create(&pt, NULL, producer, &p);
	pthread_join(pt, NULL);
	for (unsigned i = 0; i < 3; i++)
		pthread_join(rt[i], NULL);
	uint64_t gaps = 0;
	for (unsigned i = 0; i < 3; i++) {
		CHECK_EQ_U(x[i].bad, 0);
		CHECK_EQ_U(x[i].order, 0);
		CHECK_EQ_U(x[i].delivered + x[i].gaps + p.dropped_newest, STRESS_N);
		gaps += x[i].gaps;
	}
	CHECK(gaps > 0);
	CHECK(p.dropped_newest > 0);
	fprintf(stderr, "     3 readers: delivered=%llu/%llu/%llu dropped_newest=%llu\n",
	        (unsigned long long)x[0].delivered, (unsigned long long)x[1].delivered,
	        (unsigned long long)x[2].delivered, (unsigned long long)p.dropped_newest);
	rtc_ring_free(&r);
}

const t_case ring_cases[] = {
    {"ring", "concurrent_claims_protect_the_oldest", concurrent_claims_protect_the_oldest},
    {"ring", "stress_three_reader_threads", stress_three_reader_threads},
    {"ring", "read_past_noutputs_aborts", read_past_noutputs_aborts},
    {"ring", "eviction_respects_claims_of_every_output", eviction_respects_claims_of_every_output},
    {"ring", "unseen_counts_evictions_without_a_later_record",
     unseen_counts_evictions_without_a_later_record},
    {"ring", "roundtrip_and_wrap_at_every_offset", roundtrip_and_wrap_at_every_offset},
    {"ring", "eviction_is_counted_by_seq_gap", eviction_is_counted_by_seq_gap},
    {"ring", "oversize_is_refused", oversize_is_refused},
    {"ring", "claim_blocks_overwrite", claim_blocks_overwrite},
    {"ring", "two_outputs_independent", two_outputs_independent},
    {"ring", "accept_callback_leaves_rest", accept_callback_leaves_rest},
    {"ring", "stress_every_record_delivered_once_or_counted", stress_every_record_delivered_once_or_counted},
    {0},
};
