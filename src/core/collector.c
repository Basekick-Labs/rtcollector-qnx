#include "collector.h"

#include "log.h"
#include "lpdrain.h"

#include <errno.h>
#include <sched.h>
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

const char *rtc_hostname(const rtc_collector *c)
{
	return c->cfg.hostname;
}

int rtc_output_alloc_batch(rtc_output *o, size_t batch_bytes, uint32_t max_record)
{
	(void)max_record;
	o->batch_bytes = batch_bytes;
	/* The smallest record is a few payload bytes, so a batch can hold more
	 * records than this; reading simply stops at max_recs. */
	o->max_recs = batch_bytes / 16 + 1;
	o->batch_buf = malloc(batch_bytes);
	o->recs = calloc(o->max_recs, sizeof(*o->recs));
	o->done = calloc(o->max_recs, 1);
	o->flags = calloc(o->max_recs, 1);
	if (!o->batch_buf || !o->recs || !o->done || !o->flags)
		return -1;
	o->limit = o->max_recs;
	return 0;
}

void rtc_output_free_batch(rtc_output *o)
{
	free(o->batch_buf);
	free(o->recs);
	free(o->done);
	free(o->flags);
}

int rtc_collector_init(rtc_collector *c, const rtc_config *cfg)
{
	memset(c, 0, sizeof(*c));
	c->cfg = *cfg;
	if (!c->cfg.hostname[0] && gethostname(c->cfg.hostname, sizeof(c->cfg.hostname) - 1) != 0)
		snprintf(c->cfg.hostname, sizeof(c->cfg.hostname), "unknown");
	/* A day before the build: tolerates a clock a little behind the build host. */
	c->clock_floor_ns = ((uint64_t)RTC_BUILD_EPOCH - 86400u) * 1000000000u;
	atomic_init(&c->abort_after_ns, 0);
	atomic_init(&c->stop, false);
	atomic_init(&c->stop_collect, false);
	atomic_init(&c->lp_ndrains, 0);

	unsigned nout =
	    (cfg->arc.enabled ? 1 : 0) + (cfg->mqtt.enabled ? 1 : 0) + (cfg->out_stdout.enabled ? 1 : 0);
	if (rtc_ring_init(&c->ring, cfg->buffer_bytes, cfg->max_record_bytes, nout) != 0) {
		RTC_ERROR(C, "cannot allocate the buffer (buffer_bytes=%zu)", cfg->buffer_bytes);
		return -1;
	}
	c->scratch = malloc(cfg->max_record_bytes);
	if (!c->scratch)
		return -1;

	/* Inputs report missing permissions through counters; init fails only
	 * for errors that make the input meaningless. */
	for (unsigned i = 0; i < cfg->ninputs; i++) {
		const rtc_input_def *in = rtc_input_find(cfg->inputs[i]);
		if (!in)
			continue;
		if (in->init && in->init(c) != 0) {
			RTC_ERROR(C, "input %s failed to start", in->name);
			return -1;
		}
		c->inputs[c->ninputs++] = in;
	}

	if (cfg->arc.enabled) {
		if (rtc_output_arc_create(c, c->noutputs, &c->outputs[c->noutputs]) != 0)
			return -1;
		c->noutputs++;
	}
	if (cfg->mqtt.enabled) {
		if (rtc_output_mqtt_create(c, c->noutputs, &c->outputs[c->noutputs]) != 0)
			return -1;
		c->noutputs++;
	}
	if (cfg->out_stdout.enabled) {
		if (rtc_output_stdout_create(c, c->noutputs, &c->outputs[c->noutputs]) != 0)
			return -1;
		c->noutputs++;
	}
	/* The ring respects the claims of its first noutputs readers only: an
	 * output it was not sized for would copy bytes the producer overwrites. */
	if (c->noutputs != c->ring.noutputs) {
		RTC_ERROR(C, "%u outputs but a ring for %u", c->noutputs, c->ring.noutputs);
		return -1;
	}
	uint64_t now = rtc_mono_ns();
	for (unsigned i = 0; i < c->noutputs; i++) {
		c->outputs[i]->collector = c;
		c->outputs[i]->last_flush_ns = now;
	}
	return 0;
}

void rtc_collector_destroy(rtc_collector *c)
{
	for (unsigned i = 0; i < c->noutputs; i++)
		if (c->outputs[i] && c->outputs[i]->destroy)
			c->outputs[i]->destroy(c->outputs[i]);
	for (unsigned i = 0; i < c->ninputs; i++)
		if (c->inputs[i]->destroy)
			c->inputs[i]->destroy(c);
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
	rtc_emit_record(c, b->buf, len, 0);
}

void rtc_emit_record(rtc_collector *c, const uint8_t *rec, size_t len, uint64_t ts)
{
	if (ts == 0)
		ts = rtc_now_ns();
	if (ts < c->clock_floor_ns) {
		/* A device without a battery-backed clock boots near 1970. Arc reads
		 * the time unit from the value's magnitude, so such a timestamp
		 * would be stored decades off. Drop until the clock is set. */
		rtc_counter_add(&c->counters.records_clock_invalid, 1);
		return;
	}
	switch (rtc_ring_push(&c->ring, rec, (uint32_t)len, ts)) {
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
	while (!atomic_load(&c->stop_collect)) {
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
		while (!atomic_load(&c->stop_collect)) {
			/* Convert what /dev/rtcollector received; bounded per step. */
			unsigned nd = atomic_load(&c->lp_ndrains);
			for (unsigned d = 0; d < nd; d++)
				rtc_lp_drain_step(c, &c->lp_drains[d]);
			uint64_t n = rtc_mono_ns();
			if (n >= next)
				break;
			uint64_t max_slice = nd ? 20000000u : 100000000u;
			uint64_t slice = next - n < max_slice ? next - n : max_slice;
			struct timespec s = {(time_t)(slice / 1000000000u), (long)(slice % 1000000000u)};
			nanosleep(&s, NULL);
		}
	}
	/* The resource manager has stopped (rtc_collector_stop stops it first):
	 * convert what it received, then count anything left as dropped. */
	unsigned nd = atomic_load(&c->lp_ndrains);
	for (unsigned d = 0; d < nd; d++) {
		rtc_lp_drain *lpd = &c->lp_drains[d];
		for (int i = 0; i < 1024 && rtc_lp_drain_step(c, lpd) > 0; i++)
			;
		uint64_t left = rtc_ring_unseen(lpd->ring, 0);
		if (left)
			rtc_counter_add(&c->counters.rmgr_drain_dropped, left);
	}
	return NULL;
}

/* --- sender ---------------------------------------------------------------- */

static bool output_pass(rtc_collector *c, rtc_output *o, bool final, uint64_t deadline_ns)
{
	uint64_t flush = (uint64_t)llround(c->cfg.flush_interval_s * 1e9);
	for (int rounds = 0; rounds < 64; rounds++) {
		uint64_t now = rtc_mono_ns();
		if (!o->has_batch) {
			uint64_t pending = rtc_ring_pending(&c->ring, o->idx);
			if (pending == 0) {
				if (o->draining || final)
					o->last_flush_ns = now;
				o->draining = false;
				if (o->idle && !final)
					o->idle(o, now);
				return true;
			}
			/* Once a flush is due, keep reading until the ring is empty:
			 * a batch can stop early (measurement cap, batch size) and the
			 * rest must not wait for the next flush_interval. */
			if (!o->draining) {
				bool due =
				    final || now - o->last_flush_ns >= flush || pending >= o->batch_bytes;
				if (!due) {
					if (o->idle)
						o->idle(o, now);
					return true;
				}
				o->draining = true;
			}
			if (o->batch_begin)
				o->batch_begin(o);
			uint64_t dropped = 0;
			o->nrecs = rtc_ring_read(&c->ring, o->idx, o->batch_buf, o->batch_bytes, o->recs,
			                         o->max_recs, o->accept, o, &dropped);
			if (dropped) {
				rtc_counter_add(&o->counters.records_dropped, dropped);
				if (c->cfg.warn_on_buffer)
					RTC_WARN(o->name,
					         "buffer full: %llu records were dropped before they could "
					         "be sent",
					         (unsigned long long)dropped);
			}
			if (o->nrecs == 0)
				continue; /* evicted while we looked: check again */
			memset(o->done, 0, o->nrecs);
			memset(o->flags, 0, o->nrecs);
			o->has_batch = true;
		}
		if (now < o->next_attempt_ns)
			return false;
		enum rtc_send_outcome r = o->send(o, deadline_ns);
		if (r == RTC_SEND_RETRY) {
			rtc_counter_add(&o->counters.retries, 1);
			o->backoff_ns = o->backoff_ns ? o->backoff_ns * 2 : 1000000000u;
			if (o->backoff_ns > 60000000000u)
				o->backoff_ns = 60000000000u;
			/* While flushing at shutdown or --once, retry sooner, but never
			 * in a tight loop. */
			uint64_t wait = final && o->backoff_ns > 250000000u ? 250000000u : o->backoff_ns;
			o->next_attempt_ns = rtc_mono_ns() + wait;
			return false;
		}
		o->backoff_ns = 0;
		o->next_attempt_ns = 0;
		if (r == RTC_SEND_DONE)
			o->has_batch = false;
	}
	return false;
}

bool rtc_sender_pass(rtc_collector *c, bool final, uint64_t deadline_ns)
{
	bool idle = true;
	for (unsigned i = 0; i < c->noutputs; i++)
		if (!output_pass(c, c->outputs[i], final, deadline_ns))
			idle = false;
	return idle;
}

void rtc_collector_account_unsent(rtc_collector *c, bool log)
{
	for (unsigned i = 0; i < c->noutputs; i++) {
		rtc_output *o = c->outputs[i];
		uint64_t unsent = 0;
		if (o->has_batch)
			for (size_t j = 0; j < o->nrecs; j++)
				if (!o->done[j]) {
					o->done[j] = 1;
					unsent++;
				}
		o->has_batch = false;
		/* Everything the producer wrote that this output never read, whether
		 * still in the ring or evicted after its last read. */
		uint64_t unseen = rtc_ring_unseen(&c->ring, o->idx);
		c->ring.expect_seq[o->idx] = c->ring.next_seq;
		c->ring.cursor[o->idx] = atomic_load(&c->ring.head);
		if (unsent + unseen) {
			rtc_counter_add(&o->counters.records_dropped, unsent + unseen);
			if (log)
				RTC_WARN(o->name, "%llu records were not sent and are dropped",
				         (unsigned long long)(unsent + unseen));
		}
	}
}

/* One output's sender thread. The final flush is bounded by shutdown_timeout
 * (abort_after_ns, set by stop) for each output on its own. */
static void *send_main(void *arg)
{
	rtc_output *o = arg;
	rtc_collector *c = o->collector;
	while (!atomic_load(&c->stop)) {
		output_pass(c, o, false, 0);
		struct timespec s = {0, 50000000}; /* 50 ms */
		nanosleep(&s, NULL);
	}
	uint64_t deadline = atomic_load(&c->abort_after_ns);
	while (rtc_mono_ns() < deadline) {
		if (output_pass(c, o, true, deadline))
			break;
		struct timespec s = {0, 20000000};
		nanosleep(&s, NULL);
	}
	if (o->finish)
		o->finish(o, deadline);
	if (c->sender_exit_delay_ns) {
		struct timespec s = {(time_t)(c->sender_exit_delay_ns / 1000000000u),
		                     (long)(c->sender_exit_delay_ns % 1000000000u)};
		nanosleep(&s, NULL);
	}
	atomic_store(&o->sender_done, true);
	return NULL;
}

/* Stops and joins every sender started so far. Untimed on purpose: an output
 * is freed after this, so no sender may still be running. The bound comes
 * from every blocking call of an output honouring abort_after_ns (Arc's
 * requests, stdout's writes). */
static void join_senders(rtc_collector *c)
{
	atomic_store(&c->stop, true);
	for (unsigned i = 0; i < c->nsend; i++)
		pthread_join(c->send_threads[i], NULL);
	c->nsend = 0;
}

/* Starts a thread at a fixed SCHED_RR priority set at creation, so it never
 * runs at the inherited one. On QNX a non-root user may not go above 63
 * (PROCMGR_AID_PRIORITY): then it starts with the inherited priority, once
 * logged and counted. Elsewhere the priority keys are ignored. Called from
 * the main thread and from the trace monitor thread. */
int rtc_start_thread_at(rtc_collector *c, pthread_t *t, void *(*fn)(void *), void *arg, int prio,
                        const char *what, bool *fell_back)
{
	if (fell_back)
		*fell_back = false;
	if (c->fail_thread_at && atomic_fetch_add(&c->threads_started, 1) + 1 == c->fail_thread_at)
		return EAGAIN;
#ifdef RTC_QNX
	pthread_attr_t a;
	struct sched_param sp = {.sched_priority = prio};
	int rc = pthread_attr_init(&a);
	if (rc == 0) {
		rc = pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
		if (rc == 0)
			rc = pthread_attr_setschedpolicy(&a, SCHED_RR);
		if (rc == 0)
			rc = pthread_attr_setschedparam(&a, &sp);
		if (rc == 0)
			rc = pthread_create(t, &a, fn, arg);
		pthread_attr_destroy(&a);
		if (rc == 0)
			return 0;
	}
	RTC_WARN(C, "cannot run the %s thread at priority %d (%s); using the inherited priority", what, prio,
	         strerror(rc));
	/* The one counter with two writers (main and trace monitor threads). */
	atomic_fetch_add_explicit(&c->counters.priority_fallback.v, 1, memory_order_relaxed);
	if (fell_back)
		*fell_back = true;
#else
	(void)prio;
	(void)what;
#endif
	return pthread_create(t, NULL, fn, arg);
}

int rtc_start_thread(rtc_collector *c, pthread_t *t, void *(*fn)(void *), void *arg, int prio,
                     const char *what)
{
	return rtc_start_thread_at(c, t, fn, arg, prio, what, NULL);
}

int rtc_collector_start(rtc_collector *c)
{
	for (unsigned i = 0; i < c->noutputs; i++) {
		char what[48];
		snprintf(what, sizeof(what), "%s sender", c->outputs[i]->name);
		atomic_store(&c->outputs[i]->sender_done, false);
		int rc = rtc_start_thread(c, &c->send_threads[i], send_main, c->outputs[i],
		                          c->cfg.send_priority, what);
		if (rc != 0) {
			RTC_ERROR(C, "cannot start the %s sender thread: %s", c->outputs[i]->name,
			          strerror(rc));
			atomic_store(&c->abort_after_ns, rtc_mono_ns());
			join_senders(c);
			return -1;
		}
		c->nsend++;
	}
	int rc =
	    rtc_start_thread(c, &c->collect_thread, collect_main, c, c->cfg.collect_priority, "collection");
	if (rc != 0) {
		RTC_ERROR(C, "cannot start the collection thread: %s", strerror(rc));
		atomic_store(&c->abort_after_ns, rtc_mono_ns());
		atomic_store(&c->stop_collect, true);
		join_senders(c);
		return -1;
	}
	for (unsigned i = 0; i < c->ninputs; i++)
		if (c->inputs[i]->start)
			c->inputs[i]->start(c); /* failures are counted by the input, never fatal */
	return 0;
}

void rtc_collector_stop(rtc_collector *c)
{
	/* Inputs with threads of their own (the resource manager) stop first, so
	 * the collection thread's final drain sees everything they accepted. */
	for (unsigned i = 0; i < c->ninputs; i++)
		if (c->inputs[i]->stop)
			c->inputs[i]->stop(c);
	/* The deadline first: a request in flight right now is aborted when it
	 * passes, even though it was started without one. */
	uint64_t budget = (uint64_t)llround(c->cfg.shutdown_timeout_s * 1e9);
	atomic_store(&c->abort_after_ns, rtc_mono_ns() + (budget ? budget : 1));
	/* The collection thread ends first (its final drain may still push
	 * records), then each sender flushes what it left. */
	atomic_store(&c->stop_collect, true);
	pthread_join(c->collect_thread, NULL);
	join_senders(c);
	rtc_collector_account_unsent(c, true);
	if (atomic_load(&c->lp_ndrains)) {
		/* The internal input no longer runs: report the device's totals here. */
		rtc_rmgr_counters sum;
		rtc_rmgr_sum(c, &sum);
		const rtc_rmgr_counters *r = &sum;
		const rtc_collect_counters *k = &c->counters;
		RTC_INFO("rmgr",
		         "%s totals: writes=%llu lines=%llu malformed=%llu bad_timestamp=%llu too_large=%llu "
		         "uint_dropped=%llu errors=%llu dropped=%llu oversize=%llu",
		         c->cfg.rmgr_path, (unsigned long long)rtc_counter_get(&r->writes),
		         (unsigned long long)rtc_counter_get(&k->lp_lines),
		         (unsigned long long)rtc_counter_get(&k->lp_malformed),
		         (unsigned long long)rtc_counter_get(&k->lp_bad_timestamp),
		         (unsigned long long)rtc_counter_get(&k->lp_too_large),
		         (unsigned long long)rtc_counter_get(&k->lp_uint_dropped),
		         (unsigned long long)rtc_counter_get(&r->errors),
		         (unsigned long long)(rtc_counter_get(&r->dropped) +
		                              rtc_counter_get(&k->rmgr_drain_dropped)),
		         (unsigned long long)rtc_counter_get(&r->oversize));
	}
}

void rtc_rmgr_sum(const rtc_collector *c, rtc_rmgr_counters *out)
{
	memset(out, 0, sizeof(*out));
	for (unsigned i = 0; i < c->rmgr_nthreads && i < RTC_RMGR_MAX_THREADS; i++) {
		const rtc_rmgr_counters *k = &c->rmgr_counters[i];
		rtc_counter_add(&out->writes, rtc_counter_get(&k->writes));
		rtc_counter_add(&out->bytes, rtc_counter_get(&k->bytes));
		rtc_counter_add(&out->dropped, rtc_counter_get(&k->dropped));
		rtc_counter_add(&out->oversize, rtc_counter_get(&k->oversize));
		rtc_counter_add(&out->errors, rtc_counter_get(&k->errors));
		rtc_counter_add(&out->msgreads, rtc_counter_get(&k->msgreads));
		if (rtc_counter_get(&k->max_prio_seen) > rtc_counter_get(&out->max_prio_seen))
			rtc_counter_set(&out->max_prio_seen, rtc_counter_get(&k->max_prio_seen));
	}
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
		     rtc_counter_get(&k->records_outcome_unknown) +
		     rtc_counter_get(&k->points_dropped_partial);
	}
	return n;
}

int rtc_collector_run_once(rtc_collector *c)
{
	gather_round(c);
	uint64_t budget =
	    (uint64_t)llround((c->cfg.shutdown_timeout_s > 0 ? c->cfg.shutdown_timeout_s : 5) * 1e9);
	uint64_t deadline = rtc_mono_ns() + budget;
	atomic_store(&c->abort_after_ns, deadline);
	bool idle = false;
	while (!idle && rtc_mono_ns() < deadline) {
		idle = rtc_sender_pass(c, true, deadline);
		if (!idle) {
			struct timespec s = {0, 20000000};
			nanosleep(&s, NULL);
		}
	}
	rtc_collector_account_unsent(c, true);
	if (rtc_counter_get(&c->counters.records_gathered) == 0) {
		RTC_ERROR(C, "--once: no records were gathered");
		return 1;
	}
	if (!idle || losses(c)) {
		RTC_ERROR(C, "--once: %llu records were dropped, rejected or of unknown outcome",
		          (unsigned long long)losses(c));
		return 1;
	}
	return 0;
}
