#include "t.h"

#include "../src/core/sched.h"

#include <string.h>

static rtc_sched S;

static void ev(unsigned cpu, uint64_t t, uint32_t pid, uint32_t tid, enum rtc_sched_kind k, int prio)
{
	rtc_sched_event e = {cpu, t, pid, tid, k, prio};
	rtc_sched_event_in(&S, &e);
}

static void histogram(void)
{
	rtc_hist h;
	memset(&h, 0, sizeof(h));
	/* Every value lands in a bucket whose upper bound is within 12.5% above. */
	for (uint64_t v = 1; v < (1ull << 34); v = v * 3 / 2 + 1) {
		unsigned b = rtc_hist_bucket(v);
		uint64_t hi = rtc_hist_bucket_hi(b);
		CHECK(hi >= v);
		CHECK(hi - v <= v / 8 + 1);
	}
	for (int i = 0; i < 99; i++)
		rtc_hist_add(&h, 1000);
	rtc_hist_add(&h, 5000000);
	CHECK(rtc_hist_quantile(&h, 0.5) >= 1000 && rtc_hist_quantile(&h, 0.5) <= 1125);
	CHECK(rtc_hist_quantile(&h, 0.99) >= 5000000 &&
	      rtc_hist_quantile(&h, 0.99) <= 5000000); /* capped at max */
	CHECK_EQ_U(rtc_hist_quantile(&h, 1.0), 5000000);
	/* 5 ms and 6 ms are distinguishable (log2 alone would not be). */
	CHECK(rtc_hist_bucket(5000000) != rtc_hist_bucket(6000000));
}

static void latency_preemption_runtime(void)
{
	CHECK(rtc_sched_init(&S, 64) == 0);
	/* thread A: ready at 100, runs at 150 (latency 50), preempted at 400 (run 250),
	 * ready again (a repeat at 900: an event was lost, the wait restarts) until
	 * 1000 (latency 100), runs, blocks at 1100 (run 100). */
	ev(0, 100, 7, 1, RTC_EV_READY, 40);
	ev(0, 150, 7, 1, RTC_EV_RUNNING, 40);
	ev(0, 400, 7, 1, RTC_EV_READY, 40);
	ev(0, 900, 7, 1, RTC_EV_READY, 40);    /* repeat READY restarts the wait */
	ev(1, 1000, 7, 1, RTC_EV_RUNNING, 40); /* resumes on another CPU */
	ev(1, 1100, 7, 1, RTC_EV_BLOCKED, 40);
	rtc_sched_thread_name(&S, 7, 1, "loop", 0);
	rtc_sched_process_name(&S, 7, "proc/boot/control");
	rtc_sched_snapshot o;
	CHECK(rtc_sched_snapshot_init(&o, 8) == 0);
	rtc_sched_take(&S, 2, 30, 4, &o);
	CHECK_EQ_U(o.nrows, 1);
	const rtc_sched_row *r = &o.rows[0];
	CHECK_EQ_U(r->wakeups, 2);
	CHECK_EQ_U(r->preemptions, 1);
	CHECK_EQ_U(r->run_ns, 350);
	CHECK_EQ_U(r->max_ns, 100);
	CHECK_EQ_U(S.ready_repeats, 1);
	CHECK(!strcmp(r->process, "control") && !strcmp(r->thread, "loop"));
	CHECK_EQ_U(o.cpu_switches[0], 1);
	CHECK_EQ_U(o.cpu_switches[1], 1);
	/* Next interval: counters reset, names kept. */
	ev(0, 2000, 7, 1, RTC_EV_READY, 40);
	ev(0, 2010, 7, 1, RTC_EV_RUNNING, 40);
	rtc_sched_take(&S, 2, 30, 4, &o);
	CHECK(o.nrows == 1 && o.rows[0].wakeups == 1 && o.rows[0].max_ns == 10 &&
	      !strcmp(o.rows[0].thread, "loop"));
	/* READY in one interval, RUNNING in the next. */
	ev(0, 3000, 7, 1, RTC_EV_READY, 40);
	rtc_sched_take(&S, 2, 30, 4, &o);
	CHECK_EQ_U(o.nrows, 0);
	ev(0, 3500, 7, 1, RTC_EV_RUNNING, 40);
	rtc_sched_take(&S, 2, 30, 4, &o);
	CHECK(o.nrows == 1 && o.rows[0].max_ns == 500);
	rtc_sched_snapshot_free(&o);
	rtc_sched_free(&S);
}

static void drops_reset_pending_state(void)
{
	CHECK(rtc_sched_init(&S, 64) == 0);
	ev(0, 100, 9, 1, RTC_EV_READY, 50);
	rtc_sched_dropped(&S, 1000); /* events lost: the READY above may be stale */
	ev(0, 5000000, 9, 1, RTC_EV_RUNNING, 50);
	rtc_sched_snapshot o;
	rtc_sched_snapshot_init(&o, 8);
	rtc_sched_take(&S, 1, 30, 4, &o);
	CHECK_EQ_U(o.nrows, 0); /* no garbage 5 ms latency */
	CHECK_EQ_U(S.drops, 1000);
	rtc_sched_snapshot_free(&o);
	rtc_sched_free(&S);
}

static void selection(void)
{
	CHECK(rtc_sched_init(&S, 128) == 0);
	/* 3 high-priority threads, 10 low ones with growing latency, pid 1 idle. */
	for (uint32_t i = 0; i < 3; i++) {
		ev(0, 10, 100, i, RTC_EV_READY, 50);
		ev(0, 20, 100, i, RTC_EV_RUNNING, 50);
	}
	for (uint32_t i = 0; i < 10; i++) {
		ev(0, 10, 200, i, RTC_EV_READY, 10);
		ev(0, 10 + 1000 * (i + 1), 200, i, RTC_EV_RUNNING, 10);
	}
	ev(0, 10, 1, 1, RTC_EV_READY, 0);
	ev(0, 99999, 1, 1, RTC_EV_RUNNING, 0);
	rtc_sched_snapshot o;
	rtc_sched_snapshot_init(&o, 6);
	rtc_sched_take(&S, 1, 40, 2, &o);
	CHECK_EQ_U(o.nrows, 5); /* 3 high + top 2 */
	CHECK(o.rows[3].pid == 200 && o.rows[4].pid == 200);
	CHECK(o.rows[3].max_ns == 10000 && o.rows[4].max_ns == 9000); /* the worst two */
	CHECK_EQ_U(o.threads_omitted, 8);
	for (size_t i = 0; i < o.nrows; i++)
		CHECK(o.rows[i].pid != 1);
	/* max_rows caps even the high-priority pass. */
	rtc_sched_snapshot small;
	rtc_sched_snapshot_init(&small, 2);
	for (uint32_t i = 0; i < 3; i++) {
		ev(0, 30, 100, i, RTC_EV_READY, 50);
		ev(0, 40, 100, i, RTC_EV_RUNNING, 50);
	}
	rtc_sched_take(&S, 1, 40, 2, &small);
	CHECK(small.nrows == 2 && small.threads_omitted == 1);
	rtc_sched_snapshot_free(&small);
	rtc_sched_snapshot_free(&o);
	rtc_sched_free(&S);
}

static void table_churn_and_overflow(void)
{
	CHECK(rtc_sched_init(&S, 8) == 0);
	/* 1000 short-lived threads through an 8-slot table: freed on DEAD. */
	for (uint32_t i = 1; i <= 1000; i++) {
		ev(0, i * 10, 300, i, RTC_EV_READY, 10);
		ev(0, i * 10 + 1, 300, i, RTC_EV_RUNNING, 10);
		ev(0, i * 10 + 2, 300, i, RTC_EV_DEAD, 10);
	}
	CHECK_EQ_U(S.threads_overflow, 0);
	/* Without DEAD: overflow is counted, nothing breaks. */
	for (uint32_t i = 1; i <= 20; i++)
		ev(0, 5, 400, i, RTC_EV_READY, 10);
	CHECK_EQ_U(S.nthreads, 8); /* exactly the configured capacity */
	CHECK_EQ_U(S.threads_overflow, 12);
	rtc_sched_free(&S);
}

static void interrupts(void)
{
	CHECK(rtc_sched_init(&S, 8) == 0);
	rtc_sched_interrupt(&S, 0, 0x80010000u);
	rtc_sched_interrupt(&S, 1, 0x80010000u);
	rtc_sched_interrupt(&S, 1, 5);
	rtc_sched_snapshot o;
	rtc_sched_snapshot_init(&o, 2);
	rtc_sched_take(&S, 2, 30, 2, &o);
	CHECK_EQ_U(o.nvec, 2);
	CHECK(o.vec_id[0] == 0x80010000u && o.vec_count[0] == 2 && o.vec_count[1] == 1);
	CHECK(o.cpu_interrupts[0] == 1 && o.cpu_interrupts[1] == 2);
	rtc_sched_take(&S, 2, 30, 2, &o);
	CHECK(o.vec_count[0] == 0 && o.cpu_interrupts[1] == 0); /* reset per interval */
	rtc_sched_snapshot_free(&o);
	rtc_sched_free(&S);
}

/* A late event from another CPU's buffer must not leave a stale READY. */
static void out_of_order_ignored(void)
{
	CHECK(rtc_sched_init(&S, 16) == 0);
	ev(0, 1000, 5, 1, RTC_EV_READY, 40);
	ev(0, 1010, 5, 1, RTC_EV_RUNNING, 40);
	ev(0, 1100, 5, 1, RTC_EV_BLOCKED, 40);
	ev(1, 1005, 5, 1, RTC_EV_READY, 40);            /* late: older than the newest */
	ev(0, 9000000000ull, 5, 1, RTC_EV_RUNNING, 40); /* would have been a 9 s latency */
	rtc_sched_snapshot o;
	rtc_sched_snapshot_init(&o, 4);
	rtc_sched_take(&S, 2, 30, 2, &o);
	CHECK(o.nrows == 1 && o.rows[0].max_ns == 10 && o.rows[0].wakeups == 1);
	CHECK_EQ_U(S.out_of_order, 1);
	rtc_sched_snapshot_free(&o);
	rtc_sched_free(&S);
}

/* Lost DEAD events: idle, blocked threads leave the table after two quiet
 * intervals; a thread that is READY all along stays (its wait is the point). */
static void idle_threads_evicted(void)
{
	CHECK(rtc_sched_init(&S, 16) == 0);
	rtc_sched_snapshot o;
	rtc_sched_snapshot_init(&o, 64);
	for (uint32_t i = 1; i <= 15; i++) { /* fills the table; no DEAD ever */
		ev(0, 10, 300, i, RTC_EV_READY, 10);
		ev(0, 11, 300, i, RTC_EV_RUNNING, 10);
		ev(0, 12, 300, i, RTC_EV_BLOCKED, 10);
	}
	ev(0, 20, 400, 1, RTC_EV_READY, 10); /* waits for ever */
	CHECK_EQ_U(S.nthreads, 16);
	rtc_sched_take(&S, 1, 40, 2, &o); /* they were seen in this interval */
	CHECK_EQ_U(S.threads_evicted, 0);
	rtc_sched_take(&S, 1, 40, 2, &o); /* one quiet interval */
	CHECK_EQ_U(S.threads_evicted, 0);
	rtc_sched_take(&S, 1, 40, 2, &o); /* two, table full: down to half */
	CHECK_EQ_U(S.threads_evicted, 8);
	CHECK_EQ_U(S.nthreads, 8);
	/* The READY thread kept its pending wakeup: it is measured when it runs. */
	ev(0, 5000, 400, 1, RTC_EV_RUNNING, 10);
	uint64_t ov = S.threads_overflow;
	for (uint32_t i = 100; i < 108; i++) /* new threads get slots again */
		ev(0, 6000, 500, i, RTC_EV_READY, 10);
	CHECK_EQ_U(S.threads_overflow, ov);
	rtc_sched_take(&S, 1, 40, 4, &o);
	bool found = false;
	for (size_t i = 0; i < o.nrows; i++)
		if (o.rows[i].pid == 400 && o.rows[i].max_ns == 4980)
			found = true;
	CHECK(found);
	rtc_sched_snapshot_free(&o);
	rtc_sched_free(&S);
}

/* A table under three-quarters full evicts nothing: a long sleeper keeps its
 * slot and name. */
static void no_eviction_without_pressure(void)
{
	CHECK(rtc_sched_init(&S, 64) == 0);
	rtc_sched_snapshot o;
	rtc_sched_snapshot_init(&o, 64);
	rtc_sched_thread_name(&S, 9, 1, "watchdog", 0);
	ev(0, 10, 9, 1, RTC_EV_READY, 50);
	ev(0, 11, 9, 1, RTC_EV_RUNNING, 50);
	ev(0, 12, 9, 1, RTC_EV_BLOCKED, 50);
	for (uint32_t i = 1; i <= 40; i++) /* more than half full, under three-quarters */
		ev(0, 13, 10, i, RTC_EV_BLOCKED, 10);
	for (int i = 0; i < 5; i++)
		rtc_sched_take(&S, 1, 40, 2, &o);
	CHECK_EQ_U(S.threads_evicted, 0);
	ev(0, 100, 9, 1, RTC_EV_READY, 50);
	ev(0, 101, 9, 1, RTC_EV_RUNNING, 50);
	rtc_sched_take(&S, 1, 40, 2, &o);
	CHECK(o.nrows == 1 && !strcmp(o.rows[0].thread, "watchdog"));
	rtc_sched_snapshot_free(&o);
	rtc_sched_free(&S);
}

/* Backward-shift deletion keeps every lookup right under heavy churn:
 * compare the table with a plain list of what should be in it. */
static void deletion_keeps_lookups(void)
{
	CHECK(rtc_sched_init(&S, 64) == 0);
	enum { KEYS = 200 };
	bool live[KEYS] = {false};
	uint32_t x = 12345;
	for (int step = 0; step < 20000; step++) {
		x = x * 1103515245u + 12345u;
		uint32_t k = (x >> 8) % KEYS;
		uint32_t pid = 7 + k % 5, tid = k; /* clustered homes */
		if (live[k]) {
			ev(0, (uint64_t)step + 1, pid, tid, RTC_EV_DEAD, 10);
			live[k] = false;
		} else if (S.nthreads < 60) {
			ev(0, (uint64_t)step + 1, pid, tid, RTC_EV_READY, 10);
			live[k] = true;
		}
		if (step % 997 == 0) { /* every live key is findable, nothing else is */
			size_t n = 0;
			for (uint32_t j = 0; j < KEYS; j++) {
				size_t found = 0;
				for (size_t i = 0; i < S.max_threads; i++)
					if (S.threads[i].used && S.threads[i].pid == 7 + j % 5 &&
					    S.threads[i].tid == j)
						found++;
				CHECK_EQ_U(found, live[j] ? 1 : 0);
				n += live[j];
			}
			CHECK_EQ_U(S.nthreads, n);
		}
	}
	/* And events still reach the right thread: a READY then RUNNING on
	 * every live key records exactly one wakeup each. */
	for (uint32_t j = 0; j < KEYS; j++)
		if (live[j]) {
			ev(0, 100000, 7 + j % 5, j, RTC_EV_BLOCKED, 10);
			ev(0, 100001, 7 + j % 5, j, RTC_EV_READY, 10);
			ev(0, 100003, 7 + j % 5, j, RTC_EV_RUNNING, 10);
		}
	CHECK_EQ_U(S.threads_overflow, 0);
	rtc_sched_snapshot o;
	rtc_sched_snapshot_init(&o, 64);
	rtc_sched_take(&S, 1, 0, 0, &o);
	CHECK_EQ_U(o.nrows, S.nthreads);
	for (size_t i = 0; i < o.nrows; i++)
		CHECK(o.rows[i].wakeups == 1 && o.rows[i].max_ns == 2);
	rtc_sched_snapshot_free(&o);
	rtc_sched_free(&S);
}

/* More high-priority threads than rows: the worst p99 are reported, not
 * whichever the hash put first; a thread that woke and exited is omitted,
 * counted. */
static void high_priority_selection_by_latency(void)
{
	CHECK(rtc_sched_init(&S, 64) == 0);
	for (uint32_t i = 1; i <= 10; i++) {
		ev(0, 100, 50, i, RTC_EV_READY, 60);
		ev(0, 100 + i * 1000, 50, i, RTC_EV_RUNNING, 60); /* latency i µs */
	}
	ev(0, 100, 51, 1, RTC_EV_READY, 60);
	ev(0, 200, 51, 1, RTC_EV_RUNNING, 60);
	ev(0, 300, 51, 1, RTC_EV_DEAD, 60);
	rtc_sched_snapshot o;
	rtc_sched_snapshot_init(&o, 3);
	rtc_sched_take(&S, 1, 40, 2, &o);
	CHECK_EQ_U(o.nrows, 3);
	bool want[11] = {false};
	for (size_t i = 0; i < o.nrows; i++)
		want[o.rows[i].tid] = true;
	CHECK(want[10] && want[9] && want[8]);
	CHECK_EQ_U(o.threads_omitted, 7 + 1);
	rtc_sched_snapshot_free(&o);
	rtc_sched_free(&S);
}

/* --- the batch: events of a file applied in time order ------------------- */

static rtc_sched_batch B;

static void bev(unsigned cpu, uint64_t t, uint32_t pid, uint32_t tid, enum rtc_sched_kind k)
{
	rtc_sched_event e = {cpu, t, pid, tid, k, 20};
	rtc_sched_batch_add(&S, &B, &e);
}

static rtc_sched_thread *thread_of(uint32_t pid, uint32_t tid)
{
	for (size_t i = 0; i < S.max_threads; i++)
		if (S.threads[i].used && S.threads[i].pid == pid && S.threads[i].tid == tid)
			return &S.threads[i];
	return NULL;
}

/* A batch in the middle of a stream (rtc_sched_batch_clear starts a gap). */
static void batch_setup(size_t cap)
{
	CHECK(rtc_sched_init(&S, 4096) == 0);
	CHECK(rtc_sched_batch_init(&B, cap) == 0);
	B.head = false;
}

static void batch_teardown(void)
{
	rtc_sched_batch_free(&B);
	rtc_sched_free(&S);
}

/* A thread woken every 10 µs, waking on CPU0 or CPU1 and running on CPU1
 * or CPU2, whose buffers reach the file one after the other. Every real
 * wait is 100 ns. In arrival order the late RUNNING of the first wake-up
 * reads as a 10.1 µs wait (the bug); sorted, both waits are 100 ns. */
static void feed_buffers(void)
{
	/* CPU0's buffer */
	bev(0, 20000, 9, 1, RTC_EV_READY);
	rtc_sched_batch_cpu_time(&B, 0, 40000);
	/* CPU1's buffer */
	bev(1, 30000, 9, 1, RTC_EV_READY);
	bev(1, 30100, 9, 1, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 1, 40000);
	/* CPU2's buffer */
	bev(2, 20100, 9, 1, RTC_EV_RUNNING);
	bev(2, 21000, 9, 1, RTC_EV_BLOCKED);
	rtc_sched_batch_cpu_time(&B, 2, 40000);
}

static void batch_buffer_order(void)
{
	/* Applied as it arrives: the repeat and the out-of-order rules keep the
	 * waits right, but the first wake-up is lost. */
	CHECK(rtc_sched_init(&S, 64) == 0);
	ev(0, 20000, 9, 1, RTC_EV_READY, 20);
	ev(1, 30000, 9, 1, RTC_EV_READY, 20);
	ev(1, 30100, 9, 1, RTC_EV_RUNNING, 20);
	ev(2, 20100, 9, 1, RTC_EV_RUNNING, 20);
	ev(2, 21000, 9, 1, RTC_EV_BLOCKED, 20);
	CHECK_EQ_U(thread_of(9, 1)->wakeups, 1);
	CHECK_EQ_U(S.out_of_order, 2);
	rtc_sched_free(&S);

	/* The same buffers through the batch. */
	batch_setup(64);
	feed_buffers();
	rtc_sched_batch_apply(&S, &B, true, NULL);
	rtc_sched_thread *t = thread_of(9, 1);
	CHECK(t != NULL);
	if (t) {
		CHECK_EQ_U(t->wakeups, 2);
		CHECK_EQ_U(t->lat.max, 100);
		CHECK_EQ_U(t->run_ns, 900);
		CHECK_EQ_U(t->preemptions, 0);
	}
	CHECK_EQ_U(S.out_of_order, 0);
	CHECK_EQ_U(S.ready_repeats, 0);
	CHECK_EQ_U(B.n, 0); /* everything was before every CPU's newest time */
	batch_teardown();
}

/* The same buffers split over two files: CPU2's events arrive with the
 * next file. Its earlier event in the first file makes it hold the
 * watermark, so the first file's later events wait. */
static void batch_watermark_carry(void)
{
	batch_setup(64);
	rtc_sched_batch_cpu_time(&B, 2, 10000); /* CPU2 reached 10 µs */
	bev(0, 20000, 9, 1, RTC_EV_READY);
	bev(1, 30000, 9, 1, RTC_EV_READY);
	bev(1, 30100, 9, 1, RTC_EV_RUNNING);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	rtc_sched_thread *t = thread_of(9, 1);
	CHECK(t == NULL || t->wakeups == 0);
	CHECK_EQ_U(B.n, 3); /* all after 10 µs: carried */
	CHECK_EQ_U(B.lag_ns, 20100);
	/* The next file. */
	bev(2, 20100, 9, 1, RTC_EV_RUNNING);
	bev(2, 21000, 9, 1, RTC_EV_BLOCKED);
	rtc_sched_batch_cpu_time(&B, 2, 40000);
	rtc_sched_batch_cpu_time(&B, 0, 40000);
	rtc_sched_batch_cpu_time(&B, 1, 40000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	t = thread_of(9, 1);
	CHECK(t != NULL);
	if (t) {
		CHECK_EQ_U(t->wakeups, 2);
		CHECK_EQ_U(t->lat.max, 100);
	}
	CHECK_EQ_U(S.out_of_order, 0);
	CHECK_EQ_U(B.discarded, 0);
	CHECK_EQ_U(B.n, 0);
	batch_teardown();
}

/* A RUNNING delivered before the READY that caused it. */
static void batch_running_before_ready(void)
{
	batch_setup(64);
	bev(1, 20100, 9, 1, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 1, 30000);
	bev(0, 20000, 9, 1, RTC_EV_READY);
	rtc_sched_batch_cpu_time(&B, 0, 30000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	rtc_sched_thread *t = thread_of(9, 1);
	CHECK(t && t->wakeups == 1 && t->lat.max == 100);
	batch_teardown();
}

/* Equal times. A READY and the RUNNING it caused can share a timestamp on
 * two CPUs: READY first. Otherwise arrival order (here RUNNING then
 * BLOCKED at the same time on one CPU: a run of 0, not a preemption).
 * Many entries, so a sort that ignores seq shows it. */
static void batch_ties(void)
{
	batch_setup(16384);
	for (uint32_t i = 1; i <= 2000; i++) /* CPU1's buffer first */ {
		bev(1, 1000, 30, i, RTC_EV_RUNNING);
		bev(1, 1000, 30, i, RTC_EV_BLOCKED);
		bev(1, 1600, 30, i, RTC_EV_RUNNING);
	}
	rtc_sched_batch_cpu_time(&B, 1, 9000);
	for (uint32_t i = 1; i <= 2000; i++) {
		bev(0, 1000, 30, i, RTC_EV_READY);
		bev(0, 1500, 30, i, RTC_EV_READY);
	}
	rtc_sched_batch_cpu_time(&B, 0, 9000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	uint64_t wk = 0, pre = 0, max = 0;
	for (uint32_t i = 1; i <= 2000; i++) {
		rtc_sched_thread *t = thread_of(30, i);
		if (!t)
			continue;
		wk += t->wakeups;
		pre += t->preemptions;
		if (t->lat.max > max)
			max = t->lat.max;
	}
	CHECK_EQ_U(wk, 4000);
	CHECK_EQ_U(pre, 0);
	CHECK_EQ_U(max, 100);
	CHECK_EQ_U(S.ready_repeats, 0);
	batch_teardown();
}

/* A truncated file: CPU1's last buffers were dropped, so it ends at 20 µs
 * while CPU0 goes on. CPU0's READY at 30 µs has no RUNNING in the file (it
 * was on CPU1): applied, the thread's next RUNNING would read as a wait.
 * Past the watermark it is dropped, counted, and waits are forgotten. */
static void batch_truncated_tail(void)
{
	batch_setup(64);
	bev(1, 15000, 9, 2, RTC_EV_READY);
	bev(1, 15050, 9, 2, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 1, 20000);
	bev(0, 30000, 9, 1, RTC_EV_READY);
	rtc_sched_batch_cpu_time(&B, 0, 40000);
	rtc_sched_batch_apply(&S, &B, false, NULL);
	CHECK_EQ_U(B.discarded, 1);
	CHECK_EQ_U(B.n, 0);
	rtc_sched_thread *t2 = thread_of(9, 2);
	CHECK(t2 && t2->wakeups == 1 && t2->lat.max == 50); /* before the watermark: kept */
	/* The next file starts after a gap. */
	rtc_sched_batch_clear(&S, &B);
	bev(0, 50000, 9, 1, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 0, 60000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	rtc_sched_thread *t = thread_of(9, 1);
	CHECK(t == NULL || t->wakeups == 0);
	batch_teardown();
}

/* The kernel dropped events of CPU1 from 21 µs to 26 µs; the marker says
 * so at 21 µs (the start). A READY on CPU0 at 23 µs whose RUNNING was lost
 * on CPU1 must not pair with a later RUNNING: the reset goes at CPU1's next
 * event (26 µs), after the READY. */
static void batch_gap_at_next_event(void)
{
	batch_setup(64);
	bev(0, 23000, 9, 1, RTC_EV_READY);
	bev(0, 30000, 9, 1, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 0, 40000);
	rtc_sched_batch_gap(&S, &B, 1, 21000, 7);
	rtc_sched_batch_cpu_time(&B, 1, 26000);
	rtc_sched_batch_cpu_time(&B, 1, 40000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	rtc_sched_thread *t = thread_of(9, 1);
	CHECK(t && t->wakeups == 0);
	CHECK_EQ_U(S.drops, 7);
	batch_teardown();

	/* A marker with no later event of its CPU yet holds the watermark: its
	 * reset and everything after it wait for the next file. */
	batch_setup(64);
	rtc_sched_batch_cpu_time(&B, 1, 45000);
	bev(0, 20000, 9, 1, RTC_EV_READY);
	bev(0, 20100, 9, 1, RTC_EV_RUNNING);
	bev(0, 52000, 9, 1, RTC_EV_READY);
	rtc_sched_batch_cpu_time(&B, 0, 60000);
	rtc_sched_batch_gap(&S, &B, 1, 50000, 3);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	t = thread_of(9, 1);
	CHECK(t && t->wakeups == 1 && t->lat.max == 100);
	CHECK_EQ_U(S.drops, 0);
	CHECK_EQ_U(B.n, 2); /* the gap and the READY at 52 µs */
	/* CPU1's next event: the reset lands at 55 µs, after that READY. */
	rtc_sched_batch_cpu_time(&B, 1, 55000);
	bev(0, 58000, 9, 1, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 1, 70000);
	rtc_sched_batch_cpu_time(&B, 0, 70000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	CHECK_EQ_U(S.drops, 3);
	CHECK(t->wakeups == 1); /* the READY at 52 µs was forgotten, not paired */
	batch_teardown();
}

/* More events than the batch holds: what is safe is applied, the rest is
 * dropped and counted, and no wait is invented. */
static void batch_overflow(void)
{
	batch_setup(4);
	rtc_sched_batch_cpu_time(&B, 2, 10000);
	bev(0, 20000, 9, 1, RTC_EV_READY);
	bev(1, 30000, 9, 1, RTC_EV_READY);
	bev(1, 30100, 9, 1, RTC_EV_RUNNING);
	bev(2, 20100, 9, 1, RTC_EV_RUNNING);
	bev(2, 21000, 9, 1, RTC_EV_BLOCKED); /* the 5th: overflow */
	bev(1, 50000, 9, 1, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 0, 60000);
	rtc_sched_batch_cpu_time(&B, 1, 60000);
	rtc_sched_batch_cpu_time(&B, 2, 60000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	CHECK_EQ_U(B.overflow, 1);
	CHECK(B.discarded >= 1);
	/* After the overflow the CPUs resume at different times (a gap): what
	 * is left is trimmed or applied, never a wait that did not happen. */
	rtc_sched_thread *t = thread_of(9, 1);
	CHECK(t == NULL || t->lat.n == 0 || t->lat.max <= 100);
	batch_teardown();
}

/* tids are reused at once. The new thread's name arrives (applied on
 * arrival) before the old thread's events, which are applied sorted, DEAD
 * included: the slot and the name stay with the new thread. */
static void batch_tid_reuse_keeps_name(void)
{
	batch_setup(64);
	rtc_sched_thread_name(&S, 5, 2, "new", 11000);
	bev(0, 1000, 5, 2, RTC_EV_READY);
	bev(0, 1100, 5, 2, RTC_EV_RUNNING);
	bev(0, 10000, 5, 2, RTC_EV_DEAD);
	bev(0, 12000, 5, 2, RTC_EV_READY);
	bev(0, 12300, 5, 2, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 0, 20000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	rtc_sched_thread *t = thread_of(5, 2);
	CHECK(t != NULL);
	if (t) {
		CHECK(!strcmp(t->name, "new"));
		CHECK_EQ_U(t->wakeups, 1);
		CHECK_EQ_U(t->lat.max, 300);
	}
	CHECK_EQ_U(S.exited_unreported, 1);
	/* A later DEAD (this thread's own) frees the slot as before. */
	bev(0, 25000, 5, 2, RTC_EV_DEAD);
	rtc_sched_batch_cpu_time(&B, 0, 30000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	CHECK(thread_of(5, 2) == NULL);
	batch_teardown();
}

/* An event older than the thread's newest forgets its pending wait. */
static void out_of_order_resets_wait(void)
{
	CHECK(rtc_sched_init(&S, 64) == 0);
	ev(0, 100, 9, 1, RTC_EV_READY, 20);
	ev(0, 200, 9, 1, RTC_EV_RUNNING, 20);
	ev(0, 300, 9, 1, RTC_EV_READY, 20);
	ev(1, 250, 9, 1, RTC_EV_BLOCKED, 20); /* older: ignored */
	ev(0, 5000, 9, 1, RTC_EV_RUNNING, 20);
	rtc_sched_thread *t = thread_of(9, 1);
	CHECK(t && t->wakeups == 1 && t->lat.max == 100);
	CHECK_EQ_U(S.out_of_order, 1);
	rtc_sched_free(&S);
}

/* A CPU seen before and silent now holds the watermark once; silent twice
 * it is released (counted) and pending waits are forgotten. */
static void batch_silent_cpu(void)
{
	batch_setup(64);
	rtc_sched_batch_cpu_time(&B, 0, 10000);
	rtc_sched_batch_cpu_time(&B, 1, 10000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	bev(0, 20000, 9, 1, RTC_EV_READY);
	rtc_sched_batch_cpu_time(&B, 0, 30000);
	rtc_sched_batch_apply(&S, &B, true, NULL); /* CPU1 silent once: holds at 10 µs */
	CHECK_EQ_U(B.n, 1);
	CHECK_EQ_U(B.cpu_silent, 0);
	rtc_sched_batch_cpu_time(&B, 0, 40000);
	rtc_sched_batch_apply(&S, &B, true, NULL); /* twice: released */
	CHECK_EQ_U(B.cpu_silent, 1);
	CHECK_EQ_U(B.n, 0);
	batch_teardown();
}

/* clear forgets what was kept (counted); a stopping apply applies nothing. */
static void batch_clear_and_stop(void)
{
	batch_setup(64);
	rtc_sched_batch_cpu_time(&B, 1, 10000);
	bev(0, 20000, 9, 1, RTC_EV_READY);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	CHECK_EQ_U(B.n, 1);
	rtc_sched_batch_clear(&S, &B);
	CHECK_EQ_U(B.n, 0);
	CHECK_EQ_U(B.discarded, 1);
	_Atomic bool stop = true;
	bev(0, 30000, 9, 3, RTC_EV_READY);
	bev(0, 30100, 9, 3, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 0, 40000);
	rtc_sched_batch_apply(&S, &B, true, &stop);
	CHECK(thread_of(9, 3) == NULL);
	CHECK_EQ_U(B.n, 0);
	batch_teardown();
}

/* An event exactly at the watermark waits: the slower CPU's next buffer can
 * start at the same time, with an event that sorts before it. */
static void batch_strictly_before_watermark(void)
{
	batch_setup(64);
	rtc_sched_batch_cpu_time(&B, 1, 20000);
	bev(0, 20000, 9, 1, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 0, 30000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	CHECK_EQ_U(B.n, 1);
	bev(1, 20000, 9, 1, RTC_EV_READY); /* CPU1's next buffer */
	rtc_sched_batch_cpu_time(&B, 1, 40000);
	rtc_sched_batch_cpu_time(&B, 0, 40000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	rtc_sched_thread *t = thread_of(9, 1);
	CHECK(t && t->wakeups == 1 && t->lat.max == 0 && t->state == RTC_EV_RUNNING);
	batch_teardown();
}

/* A drop marker from a CPU not seen yet in this batch holds the watermark
 * (its CPU's own time cannot: it has none). */
static void batch_gap_unknown_cpu_holds(void)
{
	batch_setup(64);
	bev(0, 20000, 9, 1, RTC_EV_READY);
	rtc_sched_batch_gap(&S, &B, 1, 10000, 4);
	bev(0, 30000, 9, 1, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 0, 40000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	rtc_sched_thread *t = thread_of(9, 1);
	CHECK(t == NULL || t->wakeups == 0);
	CHECK_EQ_U(B.n, 3);
	/* CPU1 resumes at 25 µs: the loss may hide the RUNNING of the READY at
	 * 20 µs, so the reset lands after it and 30 µs pairs with nothing. */
	rtc_sched_batch_cpu_time(&B, 1, 25000);
	rtc_sched_batch_cpu_time(&B, 1, 50000);
	rtc_sched_batch_cpu_time(&B, 0, 50000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	t = thread_of(9, 1);
	CHECK(t && t->wakeups == 0);
	CHECK_EQ_U(S.drops, 4);
	batch_teardown();
}

/* The order itself: time, then gap, READY, the rest, then arrival. */
static void batch_order_key(void)
{
	rtc_sched_qev a = {.t_ns = 10, .seq = 1, .kind = RTC_EV_RUNNING};
	rtc_sched_qev b = {.t_ns = 10, .seq = 2, .kind = RTC_EV_BLOCKED};
	CHECK(rtc_sched_qev_cmp(&a, &b) < 0 && rtc_sched_qev_cmp(&b, &a) > 0);
	rtc_sched_qev r = {.t_ns = 10, .seq = 9, .kind = RTC_EV_READY};
	CHECK(rtc_sched_qev_cmp(&r, &a) < 0);
	rtc_sched_qev g = {.t_ns = 10, .seq = 99, .kind = RTC_SCHED_GAP};
	CHECK(rtc_sched_qev_cmp(&g, &r) < 0);
	rtc_sched_qev e = {.t_ns = 9, .seq = 100, .kind = RTC_EV_BLOCKED};
	CHECK(rtc_sched_qev_cmp(&e, &g) < 0);
	CHECK(rtc_sched_qev_cmp(&a, &a) == 0);
	rtc_sched_qev big = {.t_ns = UINT64_MAX, .seq = 0, .kind = RTC_EV_READY};
	CHECK(rtc_sched_qev_cmp(&e, &big) < 0); /* no subtraction overflow */
}

/* A truncated file can hold no buffer of a quiet CPU at all. With the
 * system's CPUs known, one without an event holds everything back: an
 * incomplete file is then dropped (counted), not applied without that
 * CPU's share; a following file carries until it shows up, and a CPU
 * silent for two files in a row is released. */
static void batch_expected_cpu(void)
{
	batch_setup(64);
	rtc_sched_batch_expect(&B, 2);
	/* Only CPU0 in a truncated file: the thread ran on CPU1 in between. */
	bev(0, 20000, 9, 1, RTC_EV_READY);
	bev(0, 30000, 9, 1, RTC_EV_READY);
	bev(0, 30100, 9, 1, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 0, 40000);
	rtc_sched_batch_apply(&S, &B, false, NULL);
	rtc_sched_thread *t = thread_of(9, 1);
	CHECK(t == NULL || t->wakeups == 0);
	CHECK_EQ_U(B.discarded, 3);
	/* Contiguous files: held while CPU1 is missing, applied when it comes. */
	rtc_sched_batch_clear(&S, &B);
	bev(0, 50000, 9, 1, RTC_EV_READY);
	rtc_sched_batch_cpu_time(&B, 0, 60000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	CHECK_EQ_U(B.n, 1);
	rtc_sched_batch_cpu_time(&B, 1, 45000); /* CPU1 resumes before the READY */
	bev(1, 50100, 9, 1, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 1, 70000);
	rtc_sched_batch_cpu_time(&B, 0, 70000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	t = thread_of(9, 1);
	CHECK(t && t->wakeups == 1 && t->lat.max == 100);
	batch_teardown();

	/* A CPU that never has an event (offline) is released after two files. */
	batch_setup(64);
	rtc_sched_batch_expect(&B, 2);
	bev(0, 1000, 9, 2, RTC_EV_READY);
	bev(0, 1100, 9, 2, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 0, 5000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	CHECK_EQ_U(B.n, 2); /* held (a cpu_time is not an entry) */
	rtc_sched_batch_cpu_time(&B, 0, 9000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	CHECK_EQ_U(B.cpu_silent, 1);
	rtc_sched_thread *t2 = thread_of(9, 2);
	CHECK(t2 && t2->wakeups == 1 && t2->lat.max == 100); /* the held events, applied */
	/* Released: later files apply without it. */
	bev(0, 10000, 9, 2, RTC_EV_READY);
	bev(0, 10050, 9, 2, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 0, 20000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	t2 = thread_of(9, 2);
	CHECK(t2 && t2->wakeups == 2 && t2->lat.n == 2);
	batch_teardown();
}

/* After a gap (the first file, or one after a truncated file), each CPU's
 * first buffer can start at a different time: CPU1's starts before the
 * gap ended, with a READY whose RUNNING (on CPU0) was in the lost part.
 * Events before the latest first time of the CPUs are not complete: they
 * are dropped (counted), or the READY pairs with a RUNNING a period later. */
static void batch_head_after_gap(void)
{
	batch_setup(64);
	rtc_sched_batch_clear(&S, &B);
	bev(1, 18000, 9, 1, RTC_EV_READY); /* CPU1's buffer starts early */
	bev(1, 21000, 9, 2, RTC_EV_READY);
	rtc_sched_batch_cpu_time(&B, 1, 40000);
	rtc_sched_batch_cpu_time(&B, 0, 20000); /* CPU0's starts at 20 µs */
	bev(0, 21100, 9, 2, RTC_EV_RUNNING);
	bev(0, 23000, 9, 1, RTC_EV_RUNNING); /* the next period's run */
	rtc_sched_batch_cpu_time(&B, 0, 40000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	rtc_sched_thread *t = thread_of(9, 1);
	CHECK(t == NULL || t->wakeups == 0);
	rtc_sched_thread *t2 = thread_of(9, 2);
	CHECK(t2 && t2->wakeups == 1 && t2->lat.max == 100);
	CHECK_EQ_U(B.discarded, 1);
	/* The next contiguous file is not trimmed. */
	bev(1, 41000, 9, 3, RTC_EV_READY);
	bev(0, 41200, 9, 3, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 0, 50000);
	rtc_sched_batch_cpu_time(&B, 1, 50000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	rtc_sched_thread *t3 = thread_of(9, 3);
	CHECK(t3 && t3->wakeups == 1 && t3->lat.max == 200);
	CHECK_EQ_U(B.discarded, 1);
	batch_teardown();

	/* A CPU that resumes only in the second file after the gap: the first
	 * file is held, and the head is the time that CPU resumed. */
	batch_setup(64);
	rtc_sched_batch_expect(&B, 2);
	rtc_sched_batch_clear(&S, &B);
	bev(0, 25000, 9, 4, RTC_EV_READY); /* its RUNNING, on CPU1, was lost */
	rtc_sched_batch_cpu_time(&B, 0, 28000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	CHECK_EQ_U(B.n, 1);
	rtc_sched_batch_cpu_time(&B, 1, 30000); /* CPU1 resumes */
	bev(1, 35000, 9, 4, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 1, 40000);
	rtc_sched_batch_cpu_time(&B, 0, 40000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	rtc_sched_thread *t4 = thread_of(9, 4);
	CHECK(t4 == NULL || t4->wakeups == 0);
	batch_teardown();
}

/* Review H1: a drop marker before the head's time, with its CPU's next
 * event still to come, must not be trimmed away; and the head waits for
 * every CPU to reach its time. */
static void review_head_keeps_gap_and_waits(void)
{
	batch_setup(64);
	rtc_sched_batch_clear(&S, &B);
	rtc_sched_batch_cpu_time(&B, 1, 5000);
	rtc_sched_batch_gap(&S, &B, 1, 6000, 3); /* CPU1 loses events from 6 µs */
	rtc_sched_batch_cpu_time(&B, 0, 10000);  /* h = 10 µs */
	bev(0, 20000, 9, 1, RTC_EV_READY);       /* its RUNNING on CPU1 was lost */
	rtc_sched_batch_cpu_time(&B, 0, 30000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	rtc_sched_batch_cpu_time(&B, 1, 25000); /* CPU1's next event: end of the loss */
	bev(1, 50000, 9, 1, RTC_EV_RUNNING);    /* the next period's run */
	rtc_sched_batch_cpu_time(&B, 1, 60000);
	rtc_sched_batch_cpu_time(&B, 0, 60000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	rtc_sched_thread *t = thread_of(9, 1);
	CHECK(t == NULL || t->wakeups == 0);
	CHECK_EQ_U(S.drops, 3);
	batch_teardown();

	batch_setup(64);
	rtc_sched_batch_expect(&B, 3);
	rtc_sched_batch_clear(&S, &B);
	rtc_sched_batch_cpu_time(&B, 2, 20000); /* CPU2's first buffer: 20..30 µs */
	rtc_sched_batch_cpu_time(&B, 2, 30000);
	rtc_sched_batch_cpu_time(&B, 1, 50000); /* CPU1 resumes at 50 µs: h */
	bev(1, 60000, 7, 7, RTC_EV_BLOCKED);
	rtc_sched_batch_cpu_time(&B, 1, 70000);
	rtc_sched_batch_cpu_time(&B, 0, 10000);
	rtc_sched_batch_cpu_time(&B, 0, 70000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	CHECK(B.head);                     /* CPU2 has not reached h */
	bev(2, 40000, 9, 1, RTC_EV_READY); /* before h: its RUNNING (CPU1, 45 µs) was lost */
	rtc_sched_batch_cpu_time(&B, 2, 80000);
	bev(1, 90000, 9, 1, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 1, 95000);
	rtc_sched_batch_cpu_time(&B, 0, 95000);
	rtc_sched_batch_cpu_time(&B, 2, 95000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	t = thread_of(9, 1);
	CHECK(t == NULL || t->wakeups == 0);
	CHECK(!B.head);
	batch_teardown();
}

/* Review H2: an overflow that leaves nothing to carry still forgets the
 * pending waits (what follows starts after a gap). */
static void review_overflow_resets(void)
{
	batch_setup(3);
	rtc_sched_batch_cpu_time(&B, 1, 10000);
	bev(0, 1000, 8, 1, RTC_EV_READY);
	bev(0, 2000, 8, 1, RTC_EV_RUNNING);
	bev(0, 4900, 9, 1, RTC_EV_READY);
	rtc_sched_batch_cpu_time(&B, 0, 5000);
	bev(0, 5200, 9, 1, RTC_EV_RUNNING); /* overflow: all three applied, nothing left */
	bev(0, 6000, 9, 1, RTC_EV_BLOCKED);
	rtc_sched_batch_cpu_time(&B, 1, 12000);
	bev(0, 19000, 9, 1, RTC_EV_READY);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	bev(0, 19100, 9, 1, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 0, 30000);
	rtc_sched_batch_cpu_time(&B, 1, 30000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	rtc_sched_thread *t = thread_of(9, 1);
	CHECK_EQ_U(B.overflow, 1);
	CHECK(t == NULL || t->lat.n == 0 || t->lat.max <= 100);
	batch_teardown();

	/* The thread's READY before the overflow stays pending unless reset; its
	 * next READY falls before the head's time and is trimmed: the RUNNING
	 * after the head must not pair with the old READY. */
	batch_setup(3);
	rtc_sched_batch_cpu_time(&B, 1, 10000);
	bev(0, 1000, 8, 1, RTC_EV_READY);
	bev(0, 2000, 8, 1, RTC_EV_RUNNING);
	bev(0, 4900, 9, 1, RTC_EV_READY);
	rtc_sched_batch_cpu_time(&B, 0, 5000);
	bev(0, 5200, 9, 1, RTC_EV_RUNNING); /* overflow: all three applied, nothing left */
	bev(0, 6000, 9, 1, RTC_EV_BLOCKED);
	rtc_sched_batch_cpu_time(&B, 1, 12000); /* h = 12 µs */
	rtc_sched_batch_cpu_time(&B, 0, 12000);
	bev(0, 12500, 9, 1, RTC_EV_RUNNING); /* its READY (11 µs) was trimmed */
	rtc_sched_batch_cpu_time(&B, 0, 30000);
	rtc_sched_batch_cpu_time(&B, 1, 30000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	t = thread_of(9, 1);
	CHECK(t == NULL || t->lat.n == 0);
	batch_teardown();
}

/* Review M1 + M2: a CPU with an open drop marker that goes silent is
 * released once, the marker lands, and the batch does not grow. */
static void review_silent_cpu_with_gap(void)
{
	batch_setup(64);
	rtc_sched_batch_expect(&B, 2);
	rtc_sched_batch_cpu_time(&B, 0, 1000);
	rtc_sched_batch_cpu_time(&B, 1, 1000);
	rtc_sched_batch_gap(&S, &B, 1, 2000, 5);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	for (int f = 1; f <= 8; f++) {
		uint64_t base = (uint64_t)f * 10000;
		bev(0, base + 100, 9, 1, RTC_EV_READY);
		bev(0, base + 200, 9, 1, RTC_EV_RUNNING);
		rtc_sched_batch_cpu_time(&B, 0, base + 9000);
		rtc_sched_batch_apply(&S, &B, true, NULL);
	}
	rtc_sched_thread *t = thread_of(9, 1);
	CHECK(t && t->lat.n >= 6 && t->lat.max == 100);
	CHECK_EQ_U(B.cpu_silent, 1); /* once, not again as never seen */
	CHECK(B.n <= 2);
	CHECK(B.lag_ns < 10000);
	CHECK_EQ_U(S.drops, 5);
	batch_teardown();
}

/* Review M3: in incomplete files a CPU with no event holds the file back
 * (it is not released: its buffers may be the dropped ones); counted. */
static void review_files_held_counted(void)
{
	batch_setup(64);
	rtc_sched_batch_expect(&B, 2);
	for (int f = 1; f <= 4; f++) {
		uint64_t base = (uint64_t)f * 10000;
		bev(0, base + 100, 9, 1, RTC_EV_READY);
		bev(0, base + 200, 9, 1, RTC_EV_RUNNING);
		rtc_sched_batch_cpu_time(&B, 0, base + 9000);
		rtc_sched_batch_apply(&S, &B, false, NULL);
	}
	CHECK_EQ_U(B.files_held, 4);
	CHECK_EQ_U(B.discarded, 8);
	CHECK_EQ_U(B.cpu_silent, 0);
	batch_teardown();
}

/* Review M4: a READY repeated because the RUNNING between was late (a
 * released CPU's buffer) restarts the wait instead of reading as one. */
static void review_repeat_restarts(void)
{
	batch_setup(64);
	bev(0, 100000, 9, 1, RTC_EV_READY);
	bev(0, 300000, 9, 1, RTC_EV_READY);
	bev(0, 301000, 9, 1, RTC_EV_RUNNING);
	rtc_sched_batch_cpu_time(&B, 0, 400000);
	rtc_sched_batch_apply(&S, &B, true, NULL);
	rtc_sched_thread *t = thread_of(9, 1);
	CHECK(t && t->wakeups == 1 && t->lat.max == 1000);
	CHECK_EQ_U(S.ready_repeats, 1);
	batch_teardown();
}

/* A DEAD older than the slot's newest event is the previous thread's: the
 * slot (the next thread with this pid/tid, not named) stays. */
static void review_dead_older_than_slot(void)
{
	CHECK(rtc_sched_init(&S, 64) == 0);
	ev(0, 1000, 5, 3, RTC_EV_READY, 20);
	ev(0, 1200, 5, 3, RTC_EV_RUNNING, 20);
	ev(1, 900, 5, 3, RTC_EV_DEAD, 20); /* late: before the new thread's events */
	CHECK(thread_of(5, 3) != NULL);
	rtc_sched_free(&S);
}

const t_case sched_cases[] = {
    {"sched", "out_of_order_ignored", out_of_order_ignored},
    {"sched", "histogram", histogram},
    {"sched", "latency_preemption_runtime", latency_preemption_runtime},
    {"sched", "drops_reset_pending_state", drops_reset_pending_state},
    {"sched", "selection", selection},
    {"sched", "table_churn_and_overflow", table_churn_and_overflow},
    {"sched", "interrupts", interrupts},
    {"sched", "idle_threads_evicted", idle_threads_evicted},
    {"sched", "no_eviction_without_pressure", no_eviction_without_pressure},
    {"sched", "deletion_keeps_lookups", deletion_keeps_lookups},
    {"sched", "high_priority_selection_by_latency", high_priority_selection_by_latency},
    {"sched", "batch_buffer_order", batch_buffer_order},
    {"sched", "batch_watermark_carry", batch_watermark_carry},
    {"sched", "batch_running_before_ready", batch_running_before_ready},
    {"sched", "batch_ties", batch_ties},
    {"sched", "batch_truncated_tail", batch_truncated_tail},
    {"sched", "batch_gap_at_next_event", batch_gap_at_next_event},
    {"sched", "batch_overflow", batch_overflow},
    {"sched", "batch_tid_reuse_keeps_name", batch_tid_reuse_keeps_name},
    {"sched", "out_of_order_resets_wait", out_of_order_resets_wait},
    {"sched", "batch_silent_cpu", batch_silent_cpu},
    {"sched", "batch_clear_and_stop", batch_clear_and_stop},
    {"sched", "batch_strictly_before_watermark", batch_strictly_before_watermark},
    {"sched", "batch_gap_unknown_cpu_holds", batch_gap_unknown_cpu_holds},
    {"sched", "batch_order_key", batch_order_key},
    {"sched", "batch_expected_cpu", batch_expected_cpu},
    {"sched", "batch_head_after_gap", batch_head_after_gap},
    {"sched", "review_head_keeps_gap_and_waits", review_head_keeps_gap_and_waits},
    {"sched", "review_overflow_resets", review_overflow_resets},
    {"sched", "review_silent_cpu_with_gap", review_silent_cpu_with_gap},
    {"sched", "review_files_held_counted", review_files_held_counted},
    {"sched", "review_repeat_restarts", review_repeat_restarts},
    {"sched", "review_dead_older_than_slot", review_dead_older_than_slot},
    {0},
};
