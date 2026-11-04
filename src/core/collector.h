/* The collector: configuration, the ring, the inputs and the outputs, and the
 * two threads that run them (collection and sender). */
#ifndef RTC_COLLECTOR_H
#define RTC_COLLECTOR_H

#include "config.h"
#include "counters.h"
#include "record.h"
#include "lpdrain.h"
#include "ring.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define RTC_RMGR_MAX_THREADS 16

typedef struct rtc_collector rtc_collector;
typedef struct rtc_output rtc_output;

typedef struct {
	const char *name;
	int (*init)(rtc_collector *c);
	/* Called once per round on the collection thread. Builds records into
	 * c->scratch and emits each with rtc_emit(). Returns 0 or -1. */
	int (*gather)(rtc_collector *c);
	/* Optional, for inputs with a thread of their own: started after the
	 * collector's threads (never under --once), stopped before them. */
	void (*start)(rtc_collector *c);
	void (*stop)(rtc_collector *c);
	void (*destroy)(rtc_collector *c);
} rtc_input_def;

enum rtc_send_outcome {
	RTC_SEND_DONE,  /* batch fully accounted for */
	RTC_SEND_AGAIN, /* more of the batch to send now (split, or rest after success) */
	RTC_SEND_RETRY, /* keep the batch, back off */
};

struct rtc_output {
	const char *name;
	unsigned idx; /* output index in the ring */
	size_t batch_bytes;
	rtc_output_counters counters;

	/* Batch: a private copy of records from the ring, kept across retries. */
	uint8_t *batch_buf;
	rtc_ring_rec *recs;
	uint8_t *done;
	uint8_t *flags; /* per record, owned by the output (e.g. counted once) */
	size_t max_recs;
	size_t nrecs;
	size_t limit; /* records per request; halved on 413 or encode overflow */
	bool has_batch;

	uint64_t last_flush_ns;
	bool draining; /* a flush is in progress: keep sending until the ring is empty */
	uint64_t next_attempt_ns;
	uint64_t backoff_ns;
	_Atomic bool sender_done; /* set by its sender thread as its last act */

	rtc_ring_accept accept; /* optional: limit what goes in one batch */
	void (*batch_begin)(rtc_output *o);
	/* deadline_ns: CLOCK_MONOTONIC time by which a request must end, 0 for none. */
	enum rtc_send_outcome (*send)(rtc_output *o, uint64_t deadline_ns);
	void (*destroy)(rtc_output *o);
	void *impl;
	rtc_collector *collector;
};

struct rtc_collector {
	rtc_config cfg;
	rtc_ring ring;
	rtc_collect_counters counters;
	/* /dev/rtcollector: one counter set per handler thread (single writer each). */
	rtc_rmgr_counters rmgr_counters[RTC_RMGR_MAX_THREADS];
	unsigned rmgr_nthreads;
	/* Set by the rmgr input: raw writes the collection thread converts. */
	rtc_lp_drain *lp_drains;     /* one per rmgr handler thread */
	_Atomic unsigned lp_ndrains; /* published after lp_drains is set; 0 = rmgr not running */
	uint8_t *scratch;            /* max_record_bytes, collection thread only */

	const rtc_input_def *inputs[RTC_CFG_MAX_INPUTS];
	unsigned ninputs;
	rtc_output *outputs[RTC_RING_MAX_OUTPUTS];
	unsigned noutputs;

	uint64_t clock_floor_ns;
	_Atomic bool stop_collect; /* collection thread: stop (set first) */
	_Atomic bool stop; /* sender: final flush, then stop (set after the collection thread ended) */
	/* Set at shutdown and in --once: in-flight requests are aborted once
	 * CLOCK_MONOTONIC passes it. 0 = no limit. */
	_Atomic uint64_t abort_after_ns;
	pthread_t collect_thread;
	/* One sender thread per output (send_threads[i] runs outputs[i]), so a
	 * destination that hangs never delays another. nsend: how many started. */
	pthread_t send_threads[RTC_RING_MAX_OUTPUTS];
	unsigned nsend;
	/* Test seams. fail_thread_at: thread start number fail_thread_at
	 * (1-based, counted in threads_started) fails as if pthread_create had,
	 * 0 = never. sender_exit_delay_ns: each sender waits this long before it
	 * ends, so a test can tell a joined sender from one merely told to stop. */
	unsigned fail_thread_at;
	_Atomic unsigned threads_started;
	uint64_t sender_exit_delay_ns;
};

/* Builds everything from the configuration. Allocations happen here only.
 * Returns 0 or -1 (logged). */
int rtc_collector_init(rtc_collector *c, const rtc_config *cfg);
void rtc_collector_destroy(rtc_collector *c);

/* Threads. */
int rtc_collector_start(rtc_collector *c);
/* Stops collection, then lets every output's sender flush, all within the
 * same shutdown_timeout. */
void rtc_collector_stop(rtc_collector *c);

/* --once: one collection round and one send pass, no threads. Returns 0 if
 * every record was accepted by every output, 1 otherwise. */
int rtc_collector_run_once(rtc_collector *c);

/* Collection-thread helpers for inputs. */
void rtc_emit(rtc_collector *c, rtc_rec_builder *b);
/* An already-built record with its timestamp (0 = now). */
void rtc_emit_record(rtc_collector *c, const uint8_t *rec, size_t len, uint64_t ts);
/* Starts a thread at a fixed SCHED_RR priority on QNX (see collector.c). */
int rtc_start_thread(rtc_collector *c, pthread_t *t, void *(*fn)(void *), void *arg, int prio,
                     const char *what);
/* The same; *fell_back (if not NULL) tells whether this thread got the
 * inherited priority instead of prio. */
int rtc_start_thread_at(rtc_collector *c, pthread_t *t, void *(*fn)(void *), void *arg, int prio,
                        const char *what, bool *fell_back);
uint64_t rtc_now_ns(void);  /* CLOCK_REALTIME */
uint64_t rtc_mono_ns(void); /* CLOCK_MONOTONIC */
const char *rtc_hostname(const rtc_collector *c);

/* Shared by outputs. */
int rtc_output_alloc_batch(rtc_output *o, size_t batch_bytes, uint32_t max_record);
void rtc_output_free_batch(rtc_output *o);

/* Sum of the per-thread /dev/rtcollector counters (max for max_prio_seen). */
void rtc_rmgr_sum(const rtc_collector *c, rtc_rmgr_counters *out);

/* Built-in inputs and outputs. */
extern const rtc_input_def rtc_input_internal;
#ifdef RTC_QNX
extern const rtc_input_def rtc_input_cpu;
extern const rtc_input_def rtc_input_mem;
extern const rtc_input_def rtc_input_processes;
extern const rtc_input_def rtc_input_rmgr;
extern const rtc_input_def rtc_input_trace;
#endif
/* The input called name in this build, or NULL. */
const rtc_input_def *rtc_input_find(const char *name);
int rtc_output_arc_create(rtc_collector *c, unsigned idx, rtc_output **out);
int rtc_output_stdout_create(rtc_collector *c, unsigned idx, rtc_output **out);

/* Sender for --once and tests: one pass over every output, on the calling
 * thread. With threads, each output runs on its own sender instead.
 * final: flush regardless of intervals.
 * deadline_ns bounds every request (0 = none). Returns true when no output
 * holds an unsent batch or pending ring data. */
bool rtc_sender_pass(rtc_collector *c, bool final, uint64_t deadline_ns);

/* Counts every record an output has not delivered as dropped. Call only after
 * the collection thread has stopped. */
void rtc_collector_account_unsent(rtc_collector *c, bool log);

#endif
