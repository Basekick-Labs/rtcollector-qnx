/* The collector: configuration, the ring, the inputs and the outputs, and the
 * two threads that run them (collection and sender). */
#ifndef RTC_COLLECTOR_H
#define RTC_COLLECTOR_H

#include "config.h"
#include "counters.h"
#include "record.h"
#include "ring.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct rtc_collector rtc_collector;
typedef struct rtc_output rtc_output;

typedef struct {
	const char *name;
	int (*init)(rtc_collector *c);
	/* Called once per round on the collection thread. Builds records into
	 * c->scratch and emits each with rtc_emit(). Returns 0 or -1. */
	int (*gather)(rtc_collector *c);
} rtc_input_def;

enum rtc_send_outcome {
	RTC_SEND_DONE,	/* batch fully accounted for */
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
	size_t max_recs;
	size_t nrecs;
	size_t limit; /* records per request; halved on 413 or encode overflow */
	bool has_batch;

	uint64_t last_flush_ns;
	uint64_t next_attempt_ns;
	uint64_t backoff_ns;

	rtc_ring_accept accept; /* optional: limit what goes in one batch */
	void (*batch_begin)(rtc_output *o);
	enum rtc_send_outcome (*send)(rtc_output *o, uint32_t timeout_ms);
	void (*destroy)(rtc_output *o);
	void *impl;
	rtc_collector *collector;
};

struct rtc_collector {
	rtc_config cfg;
	rtc_ring ring;
	rtc_collect_counters counters;
	uint8_t *scratch; /* max_record_bytes, collection thread only */

	const rtc_input_def *inputs[RTC_CFG_MAX_INPUTS];
	unsigned ninputs;
	rtc_output *outputs[RTC_RING_MAX_OUTPUTS];
	unsigned noutputs;

	uint64_t clock_floor_ns;
	_Atomic bool clock_valid;
	_Atomic bool stop;
	pthread_t collect_thread, send_thread;
};

/* Builds everything from the configuration. Allocations happen here only.
 * Returns 0 or -1 (logged). */
int rtc_collector_init(rtc_collector *c, const rtc_config *cfg);
void rtc_collector_destroy(rtc_collector *c);

/* Threads. */
int rtc_collector_start(rtc_collector *c);
/* Stops collection, then lets the sender flush until shutdown_timeout. */
void rtc_collector_stop(rtc_collector *c);

/* --once: one collection round and one send pass, no threads. Returns 0 if
 * every record was accepted by every output, 1 otherwise. */
int rtc_collector_run_once(rtc_collector *c);

/* Collection-thread helpers for inputs. */
void rtc_emit(rtc_collector *c, rtc_rec_builder *b);
uint64_t rtc_now_ns(void);	 /* CLOCK_REALTIME */
uint64_t rtc_mono_ns(void);	 /* CLOCK_MONOTONIC */
const char *rtc_hostname(const rtc_collector *c);

/* Shared by outputs. */
int rtc_output_alloc_batch(rtc_output *o, size_t batch_bytes, uint32_t max_record);
void rtc_output_free_batch(rtc_output *o);

/* Built-in inputs and outputs. */
extern const rtc_input_def rtc_input_internal;
int rtc_output_arc_create(rtc_collector *c, unsigned idx, rtc_output **out);
int rtc_output_stdout_create(rtc_collector *c, unsigned idx, rtc_output **out);

/* Sender: one pass over the outputs. final: flush regardless of intervals.
 * Returns true when no output holds an unsent batch or pending ring data. */
bool rtc_sender_pass(rtc_collector *c, bool final, uint32_t timeout_cap_ms);

#endif
