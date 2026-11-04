/* Scheduling statistics from kernel trace events. Platform independent: the
 * QNX trace input turns trace events into rtc_sched_event and feeds them here,
 * so the logic is tested on the host.
 *
 * Per thread, per interval: wakeups, READY->RUNNING latency (histogram:
 * log2 with 8 linear sub-buckets, ~12% resolution), run time, preemptions
 * (RUNNING->READY), highest priority seen. Per CPU: context switches and
 * interrupts. Per interrupt vector: count. Names and per-thread state
 * persist across intervals; only the counters reset.
 */
#ifndef RTC_SCHED_H
#define RTC_SCHED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTC_HIST_SUB          8
#define RTC_HIST_MAX_LOG      35 /* values clamp at 2^35 ns (~34 s) */
#define RTC_HIST_BUCKETS      (RTC_HIST_MAX_LOG * RTC_HIST_SUB)
#define RTC_SCHED_MAX_CPUS    64
#define RTC_SCHED_MAX_VECTORS 64
#define RTC_SCHED_NAME        32

typedef struct {
	uint32_t b[RTC_HIST_BUCKETS];
	uint64_t n;
	uint64_t max;
} rtc_hist;

void rtc_hist_add(rtc_hist *h, uint64_t v);
/* Upper bound of the bucket holding quantile q (0..1); exact max for q=1. */
uint64_t rtc_hist_quantile(const rtc_hist *h, double q);
/* Bucket index and its [lo, hi] range, exposed for tests. */
unsigned rtc_hist_bucket(uint64_t v);
uint64_t rtc_hist_bucket_hi(unsigned idx);

enum rtc_sched_kind {
	RTC_EV_READY,   /* thread became ready to run */
	RTC_EV_RUNNING, /* thread started running on cpu */
	RTC_EV_BLOCKED, /* any other state (blocked, waiting...) */
	RTC_EV_DEAD,    /* thread destroyed: its slot is freed */
};

typedef struct {
	unsigned cpu;
	uint64_t t_ns;
	uint32_t pid, tid;
	enum rtc_sched_kind kind;
	int prio; /* -1 if unknown */
} rtc_sched_event;

typedef struct {
	bool used;
	uint32_t pid, tid;
	enum rtc_sched_kind state;
	uint64_t ready_at, run_since;
	char name[RTC_SCHED_NAME];
	/* interval counters */
	uint64_t wakeups, preemptions, run_ns;
	int max_prio;
	rtc_hist lat;
} rtc_sched_thread;

typedef struct {
	uint32_t pid;
	char name[RTC_SCHED_NAME];
} rtc_sched_proc;

typedef struct {
	rtc_sched_thread *threads;
	size_t max_threads, nthreads;
	rtc_sched_proc *procs;
	size_t max_procs, nprocs;
	uint64_t cpu_switches[RTC_SCHED_MAX_CPUS];
	uint64_t cpu_interrupts[RTC_SCHED_MAX_CPUS];
	uint32_t vec_id[RTC_SCHED_MAX_VECTORS];
	uint64_t vec_count[RTC_SCHED_MAX_VECTORS];
	unsigned nvec;
	uint64_t threads_overflow, vectors_overflow, events, drops;
} rtc_sched;

int rtc_sched_init(rtc_sched *s, size_t max_threads);
void rtc_sched_free(rtc_sched *s);
void rtc_sched_event_in(rtc_sched *s, const rtc_sched_event *e);
void rtc_sched_thread_name(rtc_sched *s, uint32_t pid, uint32_t tid, const char *name);
void rtc_sched_process_name(rtc_sched *s, uint32_t pid, const char *name);
void rtc_sched_interrupt(rtc_sched *s, unsigned cpu, uint32_t vector);
/* The kernel dropped events: pending READY/RUNNING timestamps are no longer
 * trustworthy, so they are forgotten (no garbage latency), and counted. */
void rtc_sched_dropped(rtc_sched *s, uint64_t n);

/* One reported thread of an interval. */
typedef struct {
	uint32_t pid, tid;
	char process[RTC_SCHED_NAME], thread[RTC_SCHED_NAME];
	uint64_t wakeups, preemptions, run_ns;
	uint64_t p50_ns, p99_ns, max_ns;
	int prio;
} rtc_sched_row;

typedef struct {
	rtc_sched_row *rows;
	size_t nrows, max_rows;
	uint64_t threads_omitted;
	unsigned ncpu;
	uint64_t cpu_switches[RTC_SCHED_MAX_CPUS], cpu_interrupts[RTC_SCHED_MAX_CPUS];
	unsigned nvec;
	uint32_t vec_id[RTC_SCHED_MAX_VECTORS];
	uint64_t vec_count[RTC_SCHED_MAX_VECTORS];
} rtc_sched_snapshot;

int rtc_sched_snapshot_init(rtc_sched_snapshot *o, size_t max_rows);
void rtc_sched_snapshot_free(rtc_sched_snapshot *o);

/* Copies the interval's rows into out and resets the interval counters.
 * Reported: threads that woke at least once (pid 1 idle threads excluded):
 * every one whose highest priority >= min_prio, then the top `top` others
 * by p99 latency, up to out->max_rows; the rest are counted as omitted. */
void rtc_sched_take(rtc_sched *s, unsigned ncpu, int min_prio, size_t top, rtc_sched_snapshot *out);

#endif
