/* Scheduling statistics from kernel trace events. Platform independent: the
 * QNX trace input turns trace events into rtc_sched_event and feeds them here,
 * so the logic is tested on the host.
 *
 * Per thread, per interval: wakeups, READY->RUNNING latency (histogram:
 * log2 with 8 linear sub-buckets, ~12% resolution), run time, preemptions
 * (RUNNING->READY), highest priority seen. Per CPU: context switches and
 * interrupts. Per interrupt vector: count. Names and per-thread state
 * persist across intervals; only the counters reset.
 *
 * Order: the kernel writes each CPU's events to its own buffers, and the
 * buffers reach the trace file as they fill, so a thread that wakes on one
 * CPU and runs on another has its events out of time order in the file.
 * Applied in that order they read as waits that did not happen. Events go
 * through an rtc_sched_batch instead: sorted by time, and applied only up
 * to the newest time every CPU has reached (the watermark); later events
 * wait for the next file, which holds the rest of the slower CPUs.
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
	uint64_t last_t; /* newest event applied: an older one arriving later is ignored
	                  * and resets the pending wait */
	uint64_t name_t; /* time of the name event: a DEAD older than it was the
	                  * previous thread with this pid/tid */
	char name[RTC_SCHED_NAME];
	bool seen;     /* had an event this interval */
	unsigned idle; /* intervals in a row without an event */
	bool picked;   /* rtc_sched_take: already a row */
	uint64_t p99;  /* rtc_sched_take: this interval's p99 */
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
	size_t max_threads, nthreads; /* max_threads: table slots, one more than the capacity */
	rtc_sched_proc *procs;
	size_t max_procs, nprocs;
	uint64_t cpu_switches[RTC_SCHED_MAX_CPUS];
	uint64_t cpu_interrupts[RTC_SCHED_MAX_CPUS];
	uint32_t vec_id[RTC_SCHED_MAX_VECTORS];
	uint64_t vec_count[RTC_SCHED_MAX_VECTORS];
	unsigned nvec;
	uint64_t threads_overflow, vectors_overflow, events, drops;
	uint64_t out_of_order;      /* events older than the thread's newest: ignored, the
	                             * thread's pending wait forgotten */
	uint64_t ready_repeats;     /* READY while already READY: an event of the thread was lost */
	uint64_t threads_evicted;   /* idle threads dropped from the table (their DEAD was lost) */
	uint64_t exited_unreported; /* threads that woke and exited before the interval's rows */
} rtc_sched;

int rtc_sched_init(rtc_sched *s, size_t max_threads);
void rtc_sched_free(rtc_sched *s);
/* Applies one event now. In time order only: the trace input goes through
 * rtc_sched_batch. */
void rtc_sched_event_in(rtc_sched *s, const rtc_sched_event *e);
/* Names a thread (t_ns: the name event's time, 0 if unknown). */
void rtc_sched_thread_name(rtc_sched *s, uint32_t pid, uint32_t tid, const char *name, uint64_t t_ns);
void rtc_sched_process_name(rtc_sched *s, uint32_t pid, const char *name);
void rtc_sched_interrupt(rtc_sched *s, unsigned cpu, uint32_t vector);
/* The kernel dropped events: pending READY/RUNNING timestamps are no longer
 * trustworthy, so they are forgotten (no garbage latency), and counted. */
void rtc_sched_dropped(rtc_sched *s, uint64_t n);

/* Events of one trace file, applied in time order (see the top). One
 * allocation at init; nothing allocates afterwards. */
typedef struct {
	uint64_t t_ns;
	uint32_t pid, tid; /* a gap: pid = events the kernel dropped */
	uint32_t seq;      /* arrival order, for equal times */
	uint8_t cpu;       /* RTC_SCHED_NO_CPU when >= RTC_SCHED_MAX_CPUS */
	uint8_t kind;      /* enum rtc_sched_kind, or RTC_SCHED_GAP */
	int16_t prio;
} rtc_sched_qev;

#define RTC_SCHED_GAP    0xffu
#define RTC_SCHED_NO_CPU 0xffu

typedef struct {
	rtc_sched_qev *ev;
	size_t n, cap;
	uint32_t seq;
	uint64_t last[RTC_SCHED_MAX_CPUS];   /* newest time seen per CPU */
	uint64_t known;                      /* CPUs that hold the watermark */
	uint64_t present;                    /* CPUs with an event since the last apply */
	unsigned absent[RTC_SCHED_MAX_CPUS]; /* applies in a row without an event */
	uint64_t gap_open;                   /* CPUs with a gap waiting for their next event */
	size_t gap_idx[RTC_SCHED_MAX_CPUS];
	/* counters (the parser thread only) */
	uint64_t discarded;     /* events past the watermark of an incomplete file */
	uint64_t overflow;      /* the batch filled up before the end of a file */
	uint64_t cpu_backwards; /* an event older than the one before it on its CPU */
	uint64_t cpu_silent;    /* a CPU with no event for two applies: released */
	uint64_t lag_ns;        /* last apply: newest time - watermark */
} rtc_sched_batch;

int rtc_sched_batch_init(rtc_sched_batch *b, size_t cap);
void rtc_sched_batch_free(rtc_sched_batch *b);
/* A thread event (READY, RUNNING, BLOCKED, DEAD). */
void rtc_sched_batch_add(rtc_sched *s, rtc_sched_batch *b, const rtc_sched_event *e);
/* Any other event of a CPU (an interrupt, a name): the CPU has reached t. */
void rtc_sched_batch_cpu_time(rtc_sched_batch *b, unsigned cpu, uint64_t t_ns);
/* The kernel dropped n events of a CPU. Pending waits are forgotten at the
 * time of that CPU's next event, so the whole loss lies before the reset
 * whether the marker comes at its start or its end. */
void rtc_sched_batch_gap(rtc_sched *s, rtc_sched_batch *b, unsigned cpu, uint64_t t_ns, uint64_t n);
/* End of a file: sorts, applies every event before the watermark. The rest
 * is kept for the next file (carry: the next file follows this one), or
 * discarded and counted, with pending waits forgotten. stop, if set and
 * true, abandons the apply (shutdown). */
void rtc_sched_batch_apply(rtc_sched *s, rtc_sched_batch *b, bool carry, const _Atomic bool *stop);
/* Forgets everything kept (the next file does not follow): counted. */
void rtc_sched_batch_clear(rtc_sched *s, rtc_sched_batch *b);
/* The batch's order: time, then a gap's reset, READY, the rest, then arrival.
 * Exposed for tests (qsort need not be stable: arrival is part of the key). */
int rtc_sched_qev_cmp(const void *a, const void *b);

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
 * those whose highest priority >= min_prio (by p99 latency if there are more
 * than out->max_rows), then the `top` others with the highest p99, up to
 * out->max_rows; the rest are counted as omitted. When the table is three-
 * quarters full, threads without an event for two intervals that are not
 * ready or running are evicted, down to half full: their exit may have been in events the
 * kernel or the collector dropped. */
void rtc_sched_take(rtc_sched *s, unsigned ncpu, int min_prio, size_t top, rtc_sched_snapshot *out);

#endif
