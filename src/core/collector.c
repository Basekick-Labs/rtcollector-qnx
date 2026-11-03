#include "collector.h"

#include "log.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef RTC_BUILD_EPOCH
#define RTC_BUILD_EPOCH 1793664000 /* 2026-11-03; the Makefile passes the build time */
#endif

#define C "collector"

static const rtc_input_def *all_inputs[] = {&rtc_input_internal};

uint64_t rtc_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

uint64_t rtc_mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

const char *rtc_hostname(const rtc_collector *c) { return c->cfg.hostname; }

int rtc_output_alloc_batch(rtc_output *o, size_t batch_bytes, uint32_t max_record)
{
	(void)max_record;
	o->batch_bytes = batch_bytes;
	/* The smallest record is a header plus a few payload bytes; one entry
	 * per 16 bytes of batch cannot run out before the bytes do. */
	o->max_recs = batch_bytes / 16 + 1;
	o->batch_buf = malloc(batch_bytes);
	o->recs = calloc(o->max_recs, sizeof(*o->recs));
	o->done = calloc(o->max_recs, 1);
	if (!o->batch_buf || !o->recs || !o->done)
		return -1;
	o->limit = o->max_recs;
	return 0;
}

void rtc_output_free_batch(rtc_output *o)
{
	free(o->batch_buf);
	free(o->recs);
	free(o->done);
}

int rtc_collector_init(rtc_collector *c, const rtc_config *cfg)
{
	memset(c, 0, sizeof(*c));
	c->cfg = *cfg;
	if (!c->cfg.hostname[0] && gethostname(c->cfg.hostname, sizeof(c->cfg.hostname) - 1) != 0)
		snprintf(c->cfg.hostname, sizeof(c->cfg.hostname), "unknown");
	c->clock_floor_ns = (uint64_t)RTC_BUILD_EPOCH * 1000000000u;
	atomic_init(&c->clock_valid, false);
	atomic_init(&c->stop, false);

	unsigned nout = (cfg->arc.enabled ? 1 : 0) + (cfg->out_stdout.enabled ? 1 : 0);
	if (rtc_ring_init(&c->ring, cfg->buffer_bytes, cfg->max_record_bytes, nout) != 0) {
		RTC_ERROR(C, "cannot allocate the buffer (buffer_bytes=%zu)", cfg->buffer_bytes);
		return -1;
	}
	c->scratch = malloc(cfg->max_record_bytes);
	if (!c->scratch)
		return -1;

	for (unsigned i = 0; i < cfg->ninputs; i++)
		for (size_t j = 0; j < sizeof(all_inputs) / sizeof(all_inputs[0]); j++)
			if (!strcmp(cfg->inputs[i], all_inputs[j]->name)) {
				if (all_inputs[j]->init && all_inputs[j]->init(c) != 0) {
					RTC_ERROR(C, "input %s failed to start", all_inputs[j]->name);
					return -1;
				}
				c->inputs[c->ninputs++] = all_inputs[j];
			}

	if (cfg->arc.enabled) {
		if (rtc_output_arc_create(c, c->noutputs, &c->outputs[c->noutputs]) != 0)
			return -1;
		c->noutputs++;
	}
	if (cfg->out_stdout.enabled) {
		if (rtc_output_stdout_create(c, c->noutputs, &c->outputs[c->noutputs]) != 0)
			return -1;
		c->noutputs++;
	}
	uint64_t now = rtc_mono_ns();
	for (unsigned i = 0; i < c->noutputs; i++)
		c->outputs[i]->last_flush_ns = now;
	return 0;
}

void rtc_collector_destroy(rtc_collector *c)
{
	for (unsigned i = 0; i < c->noutputs; i++)
		if (c->outputs[i] && c->outputs[i]->destroy)
			c->outputs[i]->destroy(c->outputs[i]);
	free(c->scratch);
	rtc_ring_free(&c->ring);
}

void rtc_emit(rtc_collector *c, rtc_rec_builder *b)
{
	size_t len = rtc_rb_finish(b);
	if (len == 0) {
		rtc_counter_add(&c->counters.records_invalid, 1);
		return;
	}
	uint64_t ts = rtc_now_ns();
	if (ts < c->clock_floor_ns) {
		/* A device without a battery-backed clock boots near 1970. Arc reads
		 * the time unit from the value's magnitude, so such a timestamp
		 * would be stored decades off. Drop until the clock is set. */
		rtc_counter_add(&c->counters.records_clock_invalid, 1);
		atomic_store_explicit(&c->clock_valid, false, memory_order_relaxed);
		return;
	}
	atomic_store_explicit(&c->clock_valid, true, memory_order_relaxed);
	switch (rtc_ring_push(&c->ring, b->buf, (uint32_t)len, ts)) {
	case RTC_PUSH_OK:
		rtc_counter_add(&c->counters.records_gathered, 1);
		break;
	case RTC_PUSH_DROPPED:
		rtc_counter_add(&c->counters.records_dropped_newest, 1);
		break;
	case RTC_PUSH_OVERSIZE:
		rtc_counter_add(&c->counters.records_oversize, 1);
		break;
	}
}

static void gather_round(rtc_collector *c)
{
	uint64_t t0 = rtc_mono_ns();
	for (unsigned i = 0; i < c->ninputs; i++)
		if (c->inputs[i]->gather(c) != 0)
			rtc_counter_add(&c->counters.gather_errors, 1);
	rtc_counter_set(&c->counters.gather_ns_last, rtc_mono_ns() - t0);
	rtc_counter_add(&c->counters.rounds, 1);
}

static void *collect_main(void *arg)
{
	rtc_collector *c = arg;
	uint64_t interval = (uint64_t)llround(c->cfg.interval_s * 1e9);
	uint64_t next = rtc_mono_ns();
	while (!atomic_load(&c->stop)) {
		gather_round(c);
		next += interval;
		uint64_t now = rtc_mono_ns();
		if (now >= next) {
			/* Overran: skip the missed rounds, never burst to catch up. */
			uint64_t missed = (now - next) / interval + 1;
			rtc_counter_add(&c->counters.rounds_skipped, missed);
			next += missed * interval;
		}
		/* The deadline is absolute (previous deadline + interval), so drift does
		 * not accumulate. Sleep toward it in short slices so stop is honoured
		 * promptly; macOS has no clock_nanosleep(TIMER_ABSTIME). */
		while (!atomic_load(&c->stop)) {
			uint64_t n = rtc_mono_ns();
			if (n >= next)
				break;
			uint64_t slice = next - n < 100000000u ? next - n : 100000000u;
			struct timespec s = {(time_t)(slice / 1000000000u), (long)(slice % 1000000000u)};
			nanosleep(&s, NULL);
		}
	}
	return NULL;
}

/* --- sender ---------------------------------------------------------------- */

static bool output_pass(rtc_collector *c, rtc_output *o, bool final, uint32_t timeout_cap_ms)
{
	uint64_t flush = (uint64_t)llround(c->cfg.flush_interval_s * 1e9);
	for (int rounds = 0; rounds < 64; rounds++) {
		uint64_t now = rtc_mono_ns();
		if (!o->has_batch) {
			uint64_t pending = rtc_ring_pending(&c->ring, o->idx);
			if (pending == 0) {
				o->last_flush_ns = now;
				return true;
			}
			bool due = final || now - o->last_flush_ns >= flush || pending >= o->batch_bytes;
			if (!due)
				return true;
			if (o->batch_begin)
				o->batch_begin(o);
			uint64_t dropped = 0;
			o->nrecs = rtc_ring_read(&c->ring, o->idx, o->batch_buf, o->batch_bytes, o->recs,
						 o->max_recs, o->accept, o, &dropped);
			if (dropped) {
				rtc_counter_add(&o->counters.records_dropped, dropped);
				if (c->cfg.warn_on_buffer)
					RTC_WARN(o->name, "buffer full: %llu records were dropped before they could be sent",
						 (unsigned long long)dropped);
			}
			if (o->nrecs == 0)
				return rtc_ring_pending(&c->ring, o->idx) == 0;
			memset(o->done, 0, o->nrecs);
			o->limit = o->nrecs;
			o->has_batch = true;
		}
		if (!final && now < o->next_attempt_ns)
			return false;
		enum rtc_send_outcome r = o->send(o, timeout_cap_ms);
		if (r == RTC_SEND_RETRY) {
			rtc_counter_add(&o->counters.retries, 1);
			o->backoff_ns = o->backoff_ns ? o->backoff_ns * 2 : 1000000000u;
			if (o->backoff_ns > 60000000000u)
				o->backoff_ns = 60000000000u;
			o->next_attempt_ns = rtc_mono_ns() + o->backoff_ns;
			return false;
		}
		o->backoff_ns = 0;
		o->next_attempt_ns = 0;
		if (r == RTC_SEND_DONE) {
			o->has_batch = false;
			o->last_flush_ns = rtc_mono_ns();
		}
	}
	return false;
}

bool rtc_sender_pass(rtc_collector *c, bool final, uint32_t timeout_cap_ms)
{
	bool idle = true;
	for (unsigned i = 0; i < c->noutputs; i++)
		if (!output_pass(c, c->outputs[i], final, timeout_cap_ms))
			idle = false;
	return idle;
}

static void *send_main(void *arg)
{
	rtc_collector *c = arg;
	while (!atomic_load(&c->stop)) {
		rtc_sender_pass(c, false, 0);
		struct timespec s = {0, 50000000}; /* 50 ms */
		nanosleep(&s, NULL);
	}
	/* Final flush, bounded by shutdown_timeout. */
	uint64_t deadline = rtc_mono_ns() + (uint64_t)llround(c->cfg.shutdown_timeout_s * 1e9);
	for (;;) {
		uint64_t now = rtc_mono_ns();
		if (now >= deadline)
			break;
		uint32_t left_ms = (uint32_t)((deadline - now) / 1000000u);
		if (left_ms == 0 || rtc_sender_pass(c, true, left_ms))
			break;
		struct timespec s = {0, 20000000};
		nanosleep(&s, NULL);
	}
	for (unsigned i = 0; i < c->noutputs; i++) {
		rtc_output *o = c->outputs[i];
		uint64_t left = rtc_ring_pending(&c->ring, o->idx);
		size_t unsent = 0;
		if (o->has_batch)
			for (size_t j = 0; j < o->nrecs; j++)
				unsent += !o->done[j];
		if (unsent || left)
			RTC_WARN(o->name, "shutdown: %zu records in the current batch and %llu bytes in the buffer "
				 "were not sent",
				 unsent, (unsigned long long)left);
	}
	return NULL;
}

int rtc_collector_start(rtc_collector *c)
{
	int rc = pthread_create(&c->send_thread, NULL, send_main, c);
	if (rc != 0) {
		RTC_ERROR(C, "cannot start the sender thread: %s", strerror(rc));
		return -1;
	}
	rc = pthread_create(&c->collect_thread, NULL, collect_main, c);
	if (rc != 0) {
		RTC_ERROR(C, "cannot start the collection thread: %s", strerror(rc));
		atomic_store(&c->stop, true);
		pthread_join(c->send_thread, NULL);
		return -1;
	}
	return 0;
}

void rtc_collector_stop(rtc_collector *c)
{
	atomic_store(&c->stop, true);
	pthread_join(c->collect_thread, NULL);
	pthread_join(c->send_thread, NULL);
}

static uint64_t losses(const rtc_collector *c)
{
	uint64_t n = rtc_counter_get(&c->counters.records_dropped_newest) +
		     rtc_counter_get(&c->counters.records_oversize) +
		     rtc_counter_get(&c->counters.records_clock_invalid) +
		     rtc_counter_get(&c->counters.records_invalid);
	for (unsigned i = 0; i < c->noutputs; i++) {
		const rtc_output_counters *k = &c->outputs[i]->counters;
		n += rtc_counter_get(&k->records_dropped) + rtc_counter_get(&k->records_rejected) +
		     rtc_counter_get(&k->records_outcome_unknown) + rtc_counter_get(&k->points_dropped_partial);
	}
	return n;
}

int rtc_collector_run_once(rtc_collector *c)
{
	gather_round(c);
	uint64_t budget = (uint64_t)llround((c->cfg.shutdown_timeout_s > 0 ? c->cfg.shutdown_timeout_s : 5) * 1e9);
	uint64_t deadline = rtc_mono_ns() + budget;
	bool idle = false;
	while (!idle) {
		uint64_t now = rtc_mono_ns();
		if (now >= deadline)
			break;
		uint32_t left_ms = (uint32_t)((deadline - now) / 1000000u);
		if (left_ms == 0)
			break;
		idle = rtc_sender_pass(c, true, left_ms);
		if (!idle) {
			struct timespec s = {0, 20000000};
			nanosleep(&s, NULL);
		}
	}
	if (!idle) {
		RTC_ERROR(C, "--once: not everything was sent before the timeout");
		return 1;
	}
	if (rtc_counter_get(&c->counters.records_gathered) == 0) {
		RTC_ERROR(C, "--once: no records were gathered");
		return 1;
	}
	if (losses(c)) {
		RTC_ERROR(C, "--once: %llu records were dropped or rejected", (unsigned long long)losses(c));
		return 1;
	}
	return 0;
}
