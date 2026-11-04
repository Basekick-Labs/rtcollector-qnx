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
	 * ready again until 1000 (latency 600), runs, blocks at 1100 (run 100). */
	ev(0, 100, 7, 1, RTC_EV_READY, 40);
	ev(0, 150, 7, 1, RTC_EV_RUNNING, 40);
	ev(0, 400, 7, 1, RTC_EV_READY, 40);
	ev(0, 900, 7, 1, RTC_EV_READY, 40);    /* repeat READY does not move the start */
	ev(1, 1000, 7, 1, RTC_EV_RUNNING, 40); /* resumes on another CPU */
	ev(1, 1100, 7, 1, RTC_EV_BLOCKED, 40);
	rtc_sched_thread_name(&S, 7, 1, "loop");
	rtc_sched_process_name(&S, 7, "proc/boot/control");
	rtc_sched_snapshot o;
	CHECK(rtc_sched_snapshot_init(&o, 8) == 0);
	rtc_sched_take(&S, 2, 30, 4, &o);
	CHECK_EQ_U(o.nrows, 1);
	const rtc_sched_row *r = &o.rows[0];
	CHECK_EQ_U(r->wakeups, 2);
	CHECK_EQ_U(r->preemptions, 1);
	CHECK_EQ_U(r->run_ns, 350);
	CHECK_EQ_U(r->max_ns, 600);
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

const t_case sched_cases[] = {
    {"sched", "out_of_order_ignored", out_of_order_ignored},
    {"sched", "histogram", histogram},
    {"sched", "latency_preemption_runtime", latency_preemption_runtime},
    {"sched", "drops_reset_pending_state", drops_reset_pending_state},
    {"sched", "selection", selection},
    {"sched", "table_churn_and_overflow", table_churn_and_overflow},
    {"sched", "interrupts", interrupts},
    {0},
};
