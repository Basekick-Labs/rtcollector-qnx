/* The Arc output against a scripted server: one test per status shape. */
#include "t.h"

#include "../src/core/collector.h"
#include "fake_arc.h"
#include "mp_read.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef RTC_ASAN /* set by `make SANITIZE=1` */
#define RTC_ASAN 0
#endif

typedef struct {
	rtc_collector c;
	fake_arc *f;
} rig;

static rig R;

static unsigned setup_max_measurements = 64;
static bool setup_stdout;

static int setup(const fake_reply *script, size_t n, size_t buffer_bytes, size_t batch_bytes)
{
	static bool curl_ready;
	if (!curl_ready) {
		curl_global_init(CURL_GLOBAL_DEFAULT);
		curl_ready = true;
	}
	R.f = fake_arc_start(script, n);
	if (!R.f)
		return -1;
	rtc_config cfg;
	rtc_config_defaults(&cfg);
	cfg.arc.enabled = true;
	snprintf(cfg.arc.url, sizeof(cfg.arc.url), "http://127.0.0.1:%d", fake_arc_port(R.f));
	snprintf(cfg.arc.database, sizeof(cfg.arc.database), "telemetry");
	snprintf(cfg.hostname, sizeof(cfg.hostname), "pi5");
	cfg.buffer_bytes = buffer_bytes;
	cfg.arc.batch_bytes = batch_bytes;
	cfg.arc.timeout_ms = 2000;
	cfg.max_record_bytes = 512;
	cfg.arc.max_measurements = setup_max_measurements;
	cfg.out_stdout.enabled = setup_stdout;
	cfg.out_stdout.batch_bytes = batch_bytes;
	if (rtc_config_validate(&cfg) != 0)
		return -1;
	return rtc_collector_init(&R.c, &cfg);
}

static void teardown(void)
{
	rtc_collector_destroy(&R.c);
	fake_arc_stop(R.f);
	mp_free_all();
}

static void emit(const char *m, int n)
{
	for (int i = 0; i < n; i++) {
		rtc_rec_builder b;
		rtc_rb_init(&b, R.c.scratch, R.c.cfg.max_record_bytes, m);
		rtc_rb_tag(&b, "core", "0");
		rtc_rb_i64(&b, "n", i);
		rtc_emit(&R.c, &b);
	}
}

/* Final-mode passes until idle; retries are spaced (up to 250 ms) by the
 * sender's own backoff, so sleep between passes. */
static int drain(int max_passes)
{
	for (int i = 0; i < max_passes * 20; i++) {
		if (rtc_sender_pass(&R.c, true, 0))
			return i;
		struct timespec ts = {0, 20000000};
		nanosleep(&ts, NULL);
	}
	return -1;
}

static rtc_output_counters *K(void)
{
	return &R.c.outputs[0]->counters;
}

/* Measurement names in request i. */
static unsigned items(size_t i, const char **names, unsigned max)
{
	size_t len;
	int z;
	const uint8_t *b = fake_arc_body(R.f, i, &len, &z);
	mp_node *root = b ? mp_parse(b, len) : NULL;
	unsigned n = 0;
	for (uint32_t k = 0; root && root->t == MP_ARR && k < root->n && n < max; k++) {
		mp_node *m = mp_get(root->kids[k], "m");
		static char store[16][64];
		snprintf(store[n], sizeof(store[n]), "%.*s", m ? (int)m->n : 0, m ? m->s : "");
		names[n] = store[n];
		n++;
	}
	return n;
}

static void ok_204(void)
{
	CHECK(setup(NULL, 0, 1 << 16, 1 << 14) == 0);
	emit("cpu", 3);
	emit("mem", 2);
	CHECK(drain(10) >= 0);
	CHECK_EQ_U(fake_arc_requests(R.f), 1);
	CHECK_EQ_U(rtc_counter_get(&K()->records_sent), 5);
	size_t len;
	int z;
	CHECK(fake_arc_body(R.f, 0, &len, &z) && z == 1);
	CHECK(!strcmp(fake_arc_header(R.f, 0, "x-arc-database"), "telemetry"));
	const char *n[4];
	CHECK_EQ_U(items(0, n, 4), 2);
	teardown();
}

static void partial_400_is_not_resent(void)
{
	fake_reply s[] = {{400,
	                   "{\"error\":\"partial write: 2 points dropped: 2 in \\\"cpu\\\": bad; the other "
	                   "points were written\"}"}};
	CHECK(setup(s, 1, 1 << 16, 1 << 14) == 0);
	emit("cpu", 3);
	CHECK(drain(10) >= 0);
	CHECK_EQ_U(fake_arc_requests(R.f), 1);
	CHECK_EQ_U(rtc_counter_get(&K()->points_dropped_partial), 2);
	CHECK_EQ_U(rtc_counter_get(&K()->records_rejected), 0);
	teardown();
}

static void refused_400_drops_the_batch(void)
{
	fake_reply s[] = {{400, "{\"error\":\"malformed MessagePack\"}"}};
	CHECK(setup(s, 1, 1 << 16, 1 << 14) == 0);
	emit("cpu", 3);
	CHECK(drain(10) >= 0);
	CHECK_EQ_U(fake_arc_requests(R.f), 1);
	CHECK_EQ_U(rtc_counter_get(&K()->records_rejected), 3);
	teardown();
}

static void stopped_500_resends_only_unwritten(void)
{
	fake_reply s[] = {
	    /* Arc's real shape: keys sorted, the error text (which also names the
	     * written measurements) first. */
	    {500, "{\"error\":\"write stopped at measurement mem after writing [cpu]. Resending the "
	          "whole request writes the written rows again\",\"rows_written\":2,"
	          "\"stopped_at_measurement\":\"mem\",\"written_measurements\":[\"cpu\"]}"}};
	CHECK(setup(s, 1, 1 << 16, 1 << 14) == 0);
	emit("cpu", 2);
	emit("mem", 2);
	CHECK(drain(10) >= 0);
	CHECK_EQ_U(fake_arc_requests(R.f), 2);
	const char *n[4];
	CHECK_EQ_U(items(0, n, 4), 2);
	unsigned k = items(1, n, 4);
	CHECK(k == 1 && !strcmp(n[0], "mem"));
	CHECK_EQ_U(rtc_counter_get(&K()->records_sent), 4);
	CHECK_EQ_U(rtc_counter_get(&K()->records_rejected), 0);
	teardown();
}

static void auth_401_keeps_data(void)
{
	fake_reply s[] = {{401, "{\"error\":\"invalid token\"}"}, {403, "{\"error\":\"no write\"}"}};
	CHECK(setup(s, 2, 1 << 16, 1 << 14) == 0);
	emit("cpu", 4);
	CHECK(drain(10) >= 0);
	CHECK_EQ_U(fake_arc_requests(R.f), 3);
	CHECK_EQ_U(rtc_counter_get(&K()->records_sent), 4);
	CHECK_EQ_U(rtc_counter_get(&K()->records_rejected), 0);
	CHECK(rtc_counter_get(&K()->retries) >= 2);
	teardown();
}

static void too_large_413_splits(void)
{
	fake_reply s[] = {{413, "{\"error\":\"too large\"}"}};
	CHECK(setup(s, 1, 1 << 16, 1 << 14) == 0);
	emit("cpu", 8);
	CHECK(drain(10) >= 0);
	CHECK_EQ_U(fake_arc_requests(R.f), 3); /* 8 refused, then 4 + 4 */
	CHECK_EQ_U(rtc_counter_get(&K()->records_sent), 8);
	teardown();
}

static void unavailable_503_then_ok_loses_nothing(void)
{
	fake_reply s[] = {{503, "{}"}, {502, "{}"}, {504, "{}"}};
	CHECK(setup(s, 3, 1 << 16, 1 << 14) == 0);
	emit("cpu", 5);
	CHECK(drain(10) >= 0);
	CHECK_EQ_U(fake_arc_requests(R.f), 4);
	CHECK_EQ_U(rtc_counter_get(&K()->records_sent), 5);
	teardown();
}

/* Arc unavailable while the source keeps producing: the ring evicts, and every
 * record ends up sent or counted as dropped. Nothing disappears silently. */
static void outage_accounts_for_every_record(void)
{
	fake_reply s[8];
	for (int i = 0; i < 8; i++)
		s[i] = (fake_reply){503, "{}"};
	CHECK(setup(s, 8, 4096, 2048) == 0);
	int emitted = 0;
	for (int round = 0; round < 10; round++) {
		emit("cpu", 20);
		emitted += 20;
		rtc_sender_pass(&R.c, true, 0);
	}
	CHECK(drain(50) >= 0);
	uint64_t sent = rtc_counter_get(&K()->records_sent);
	uint64_t dropped = rtc_counter_get(&K()->records_dropped);
	uint64_t newest = rtc_counter_get(&R.c.counters.records_dropped_newest);
	CHECK(dropped > 0);
	CHECK_EQ_U(sent + dropped + newest, (uint64_t)emitted);
	teardown();
}

static long maxrss_kb(void)
{
	struct rusage u;
	getrusage(RUSAGE_SELF, &u);
#ifdef __APPLE__
	return u.ru_maxrss / 1024;
#else
	return u.ru_maxrss;
#endif
}

static void memory_constant_after_first_requests(void)
{
	CHECK(setup(NULL, 0, 1 << 16, 1 << 14) == 0);
	long at10 = 0;
	for (int i = 0; i < 1000; i++) {
		emit("cpu", 20);
		emit("mem", 5);
		rtc_sender_pass(&R.c, true, 0);
		if (i == 10)
			at10 = maxrss_kb();
	}
	long at1000 = maxrss_kb();
	fprintf(stderr, "     maxrss: request 10 %ld KiB, request 1000 %ld KiB\n", at10, at1000);
#if RTC_ASAN
	(void)at10; /* ASan's shadow memory and quarantine make RSS meaningless */
#else
	CHECK(at1000 - at10 < 1024);
#endif
	CHECK_EQ_U(rtc_counter_get(&K()->records_sent), 25000);
	teardown();
}

static void clock_before_floor_drops_and_counts(void)
{
	CHECK(setup(NULL, 0, 1 << 16, 1 << 14) == 0);
	R.c.clock_floor_ns = UINT64_MAX; /* as if the device booted in 1970 */
	emit("cpu", 3);
	CHECK_EQ_U(rtc_counter_get(&R.c.counters.records_clock_invalid), 3);
	CHECK_EQ_U(rtc_ring_pending(&R.c.ring, 0), 0);
	CHECK(drain(5) >= 0);
	CHECK_EQ_U(fake_arc_requests(R.f), 0);
	teardown();
}

static void one_measurement_per_request_when_capped(void)
{
	setup_max_measurements = 1;
	CHECK(setup(NULL, 0, 1 << 16, 1 << 14) == 0);
	emit("cpu", 2);
	emit("mem", 2);
	emit("disk", 2);
	CHECK(drain(10) >= 0);
	CHECK_EQ_U(fake_arc_requests(R.f), 3);
	for (size_t i = 0; i < 3; i++) {
		const char *n[4];
		CHECK_EQ_U(items(i, n, 4), 1);
	}
	CHECK_EQ_U(rtc_counter_get(&K()->records_sent), 6);
	teardown();
	setup_max_measurements = 64;
}

/* Arc failing does not hold back the stdout output, and vice versa. */
static void outputs_progress_independently(void)
{
	fake_reply s[] = {{503, "{}"}, {503, "{}"}, {503, "{}"}};
	setup_stdout = true;
	CHECK(setup(s, 3, 1 << 16, 1 << 14) == 0);
	CHECK_EQ_U(R.c.noutputs, 2);
	FILE *saved = stdout;
	stdout = fopen("/dev/null", "w");
	emit("cpu", 4);
	rtc_sender_pass(&R.c, false, 0); /* not final: arc backs off after one 503 */
	uint64_t long_ago = rtc_mono_ns() - (uint64_t)(R.c.cfg.flush_interval_s * 1e9) - 1;
	R.c.outputs[0]->last_flush_ns = long_ago;
	R.c.outputs[1]->last_flush_ns = long_ago;
	rtc_sender_pass(&R.c, false, 0);
	CHECK_EQ_U(rtc_counter_get(&R.c.outputs[1]->counters.records_sent), 4);
	CHECK_EQ_U(rtc_counter_get(&R.c.outputs[0]->counters.records_sent), 0);
	CHECK(drain(10) >= 0);
	CHECK_EQ_U(rtc_counter_get(&R.c.outputs[0]->counters.records_sent), 4);
	fclose(stdout);
	stdout = saved;
	teardown();
	setup_stdout = false;
}

/* "partial write" without "the other points were written": nothing stored. */
static void partial_400_all_dropped_is_not_sent(void)
{
	fake_reply s[] = {{400, "{\"error\":\"partial write: 3 points dropped: 3 in \\\"cpu\\\": bad;\"}"}};
	CHECK(setup(s, 1, 1 << 16, 1 << 14) == 0);
	emit("cpu", 3);
	CHECK(drain(10) >= 0);
	CHECK_EQ_U(rtc_counter_get(&K()->records_sent), 0);
	CHECK_EQ_U(rtc_counter_get(&K()->records_rejected), 3);
	CHECK_EQ_U(rtc_counter_get(&K()->points_dropped_partial), 3);
	teardown();
}

/* A flush that stops early (measurement cap) keeps going in the same pass,
 * instead of leaving the rest for the next flush_interval. */
static void flush_drains_backlog_in_one_pass(void)
{
	setup_max_measurements = 1;
	CHECK(setup(NULL, 0, 1 << 16, 1 << 14) == 0);
	R.c.cfg.flush_interval_s = 3600; /* only "due" because we force it below */
	emit("cpu", 1);
	emit("mem", 1);
	emit("disk", 1);
	/* Make the flush due. Relative to now: CLOCK_MONOTONIC can be smaller than
	 * flush_interval on a freshly booted machine (CI). */
	R.c.outputs[0]->last_flush_ns = rtc_mono_ns() - 3600000000000ull - 1;
	rtc_sender_pass(&R.c, false, 0);
	CHECK_EQ_U(fake_arc_requests(R.f), 3);
	CHECK_EQ_U(rtc_counter_get(&K()->records_sent), 3);
	teardown();
	setup_max_measurements = 64;
}

const t_case arc_http_cases[] = {
    {"arc_http", "partial_400_all_dropped_is_not_sent", partial_400_all_dropped_is_not_sent},
    {"arc_http", "flush_drains_backlog_in_one_pass", flush_drains_backlog_in_one_pass},
    {"arc_http", "clock_before_floor_drops_and_counts", clock_before_floor_drops_and_counts},
    {"arc_http", "one_measurement_per_request_when_capped", one_measurement_per_request_when_capped},
    {"arc_http", "outputs_progress_independently", outputs_progress_independently},
    {"arc_http", "ok_204", ok_204},
    {"arc_http", "partial_400_is_not_resent", partial_400_is_not_resent},
    {"arc_http", "refused_400_drops_the_batch", refused_400_drops_the_batch},
    {"arc_http", "stopped_500_resends_only_unwritten", stopped_500_resends_only_unwritten},
    {"arc_http", "auth_401_keeps_data", auth_401_keeps_data},
    {"arc_http", "too_large_413_splits", too_large_413_splits},
    {"arc_http", "unavailable_503_then_ok_loses_nothing", unavailable_503_then_ok_loses_nothing},
    {"arc_http", "outage_accounts_for_every_record", outage_accounts_for_every_record},
    {"arc_http", "memory_constant_after_first_requests", memory_constant_after_first_requests},
    {0},
};

/* --- lifecycle: threads, shutdown, --once ------------------------------------ */

static double since(uint64_t t0)
{
	return (double)(rtc_mono_ns() - t0) / 1e9;
}

/* SIGTERM while a request hangs: stop returns within shutdown_timeout plus a
 * margin, not after the request timeout (review H1). */
static void shutdown_budget_bounds_a_hanging_request(void)
{
	fake_reply s[] = {{0, ""}};
	CHECK(setup(s, 1, 1 << 16, 1 << 14) == 0);
	R.c.cfg.arc.timeout_ms = 30000;
	R.c.cfg.shutdown_timeout_s = 0.5;
	R.c.cfg.interval_s = 0.05;
	R.c.cfg.flush_interval_s = 0.05;
	CHECK(rtc_collector_start(&R.c) == 0);
	for (int i = 0; i < 100 && fake_arc_requests(R.f) == 0; i++) {
		struct timespec ts = {0, 20000000};
		nanosleep(&ts, NULL);
	}
	CHECK_EQ_U(fake_arc_requests(R.f), 1); /* the request is hanging now */
	uint64_t t0 = rtc_mono_ns();
	rtc_collector_stop(&R.c);
	double took = since(t0);
	fprintf(stderr, "     stop took %.2fs\n", took);
	CHECK(took < 1.0);
	/* The hung request went out: its records are of unknown outcome; those
	 * gathered meanwhile are counted as dropped. Nothing vanishes. */
	uint64_t gathered = rtc_counter_get(&R.c.counters.records_gathered);
	uint64_t accounted = rtc_counter_get(&K()->records_sent) + rtc_counter_get(&K()->records_dropped) +
	                     rtc_counter_get(&K()->records_outcome_unknown);
	CHECK_EQ_U(accounted, gathered);
	CHECK(rtc_counter_get(&K()->records_outcome_unknown) > 0);
	teardown();
}

static void threads_deliver_and_stop_cleanly(void)
{
	CHECK(setup(NULL, 0, 1 << 16, 1 << 14) == 0);
	R.c.cfg.interval_s = 0.02;
	R.c.cfg.flush_interval_s = 0.05;
	CHECK(rtc_collector_start(&R.c) == 0);
	struct timespec ts = {0, 300000000};
	nanosleep(&ts, NULL);
	rtc_collector_stop(&R.c);
	uint64_t gathered = rtc_counter_get(&R.c.counters.records_gathered);
	CHECK(gathered > 5);
	CHECK_EQ_U(rtc_counter_get(&K()->records_sent), gathered);
	CHECK_EQ_U(rtc_counter_get(&K()->records_dropped), 0);
	teardown();
}

static void once_exit_codes(void)
{
	CHECK(setup(NULL, 0, 1 << 16, 1 << 14) == 0);
	CHECK_EQ_U(rtc_collector_run_once(&R.c), 0);
	teardown();

	fake_reply s[] = {{400, "{\"error\":\"malformed MessagePack\"}"}};
	CHECK(setup(s, 1, 1 << 16, 1 << 14) == 0);
	CHECK_EQ_U(rtc_collector_run_once(&R.c), 1);
	teardown();

	fake_reply h[] = {{0, ""}};
	CHECK(setup(h, 1, 1 << 16, 1 << 14) == 0);
	R.c.cfg.shutdown_timeout_s = 0.5;
	uint64_t t0 = rtc_mono_ns();
	CHECK_EQ_U(rtc_collector_run_once(&R.c), 1);
	CHECK(since(t0) < 1.5);
	teardown();
}

/* Arc failing through shutdown: what was never sent is counted as dropped,
 * so sent + dropped covers everything gathered. */
static void shutdown_counts_unsent_as_dropped(void)
{
	fake_reply s[32];
	for (int i = 0; i < 32; i++)
		s[i] = (fake_reply){503, "{}"};
	CHECK(setup(s, 32, 1 << 16, 1 << 14) == 0);
	R.c.cfg.interval_s = 0.02;
	R.c.cfg.flush_interval_s = 0.02;
	R.c.cfg.shutdown_timeout_s = 0.2;
	CHECK(rtc_collector_start(&R.c) == 0);
	struct timespec ts = {0, 300000000};
	nanosleep(&ts, NULL);
	rtc_collector_stop(&R.c);
	uint64_t gathered = rtc_counter_get(&R.c.counters.records_gathered);
	CHECK(gathered > 5);
	CHECK_EQ_U(rtc_counter_get(&K()->records_sent), 0);
	CHECK_EQ_U(rtc_counter_get(&K()->records_dropped), gathered);
	teardown();
}

/* One sender per output: Arc hanging on a request does not hold back stdout,
 * and stop still ends within shutdown_timeout. */
static void hung_arc_does_not_delay_stdout(void)
{
	fake_reply s[] = {{0, ""}};
	setup_stdout = true;
	CHECK(setup(s, 1, 1 << 16, 1 << 14) == 0);
	R.c.cfg.arc.timeout_ms = 30000;
	R.c.cfg.shutdown_timeout_s = 0.5;
	R.c.cfg.interval_s = 0.05;
	R.c.cfg.flush_interval_s = 0.05;
	FILE *saved = stdout;
	stdout = fopen("/dev/null", "w");
	CHECK(rtc_collector_start(&R.c) == 0);
	for (int i = 0; i < 100 && fake_arc_requests(R.f) == 0; i++) {
		struct timespec ts = {0, 20000000};
		nanosleep(&ts, NULL);
	}
	CHECK_EQ_U(fake_arc_requests(R.f), 1); /* hanging now */
	uint64_t before = rtc_counter_get(&R.c.outputs[1]->counters.records_sent);
	struct timespec ts = {0, 400000000};
	nanosleep(&ts, NULL);
	uint64_t after = rtc_counter_get(&R.c.outputs[1]->counters.records_sent);
	CHECK(after > before);
	CHECK_EQ_U(fake_arc_requests(R.f), 1);
	uint64_t t0 = rtc_mono_ns();
	rtc_collector_stop(&R.c);
	CHECK(since(t0) < 1.0);
	fclose(stdout);
	stdout = saved;
	uint64_t gathered = rtc_counter_get(&R.c.counters.records_gathered);
	CHECK_EQ_U(rtc_counter_get(&R.c.outputs[1]->counters.records_sent), gathered);
	uint64_t accounted = rtc_counter_get(&K()->records_sent) + rtc_counter_get(&K()->records_dropped) +
	                     rtc_counter_get(&K()->records_outcome_unknown);
	CHECK_EQ_U(accounted, gathered);
	teardown();
	setup_stdout = false;
}

/* A thread that cannot start: the senders already running are stopped and
 * joined before start returns, whichever thread failed. Each sender lingers
 * 100 ms before it ends, so only a join makes sender_done true by then. */
static void failed_start_unwinds_every_sender(void)
{
	setup_stdout = true;
	CHECK(setup(NULL, 0, 1 << 16, 1 << 14) == 0);
	unsigned nout = R.c.noutputs;
	teardown();
	/* Thread starts 1..nout are the senders, nout + 1 the collection thread. */
	for (unsigned at = 2; at <= nout + 1; at++) {
		CHECK(setup(NULL, 0, 1 << 16, 1 << 14) == 0);
		R.c.cfg.flush_interval_s = 0.01;
		R.c.fail_thread_at = at;
		R.c.sender_exit_delay_ns = 100000000u;
		emit("cpu", 3);
		CHECK(rtc_collector_start(&R.c) == -1);
		CHECK_EQ_U(R.c.nsend, 0);
		CHECK(atomic_load(&R.c.stop));
		for (unsigned i = 0; i < at - 1 && i < nout; i++)
			CHECK(atomic_load(&R.c.outputs[i]->sender_done));
		CHECK_EQ_U(rtc_counter_get(&K()->records_sent), 0);
		CHECK_EQ_U(fake_arc_requests(R.f), 0);
		teardown();
	}
	setup_stdout = false;
}

/* A stdout nobody reads (a full pipe) does not hold shutdown: its writes give
 * up at the deadline, and what was not written is counted as dropped. */
static void full_stdout_pipe_does_not_hold_shutdown(void)
{
	int fds[2];
	CHECK(pipe(fds) == 0);
	int fl = fcntl(fds[1], F_GETFL);
	fcntl(fds[1], F_SETFL, fl | O_NONBLOCK);
	char junk[4096];
	memset(junk, 'x', sizeof(junk));
	while (write(fds[1], junk, sizeof(junk)) > 0)
		;
	fcntl(fds[1], F_SETFL, fl); /* full, and blocking again */
	setup_stdout = true;
	CHECK(setup(NULL, 0, 1 << 16, 1 << 14) == 0);
	R.c.cfg.interval_s = 0.02;
	R.c.cfg.flush_interval_s = 0.02;
	R.c.cfg.shutdown_timeout_s = 0.5;
	FILE *saved = stdout;
	stdout = fdopen(fds[1], "w");
	CHECK(rtc_collector_start(&R.c) == 0);
	struct timespec ts = {0, 300000000};
	nanosleep(&ts, NULL);
	uint64_t t0 = rtc_mono_ns();
	rtc_collector_stop(&R.c);
	double took = since(t0);
	fprintf(stderr, "     stop with a full stdout took %.2fs\n", took);
	CHECK(took < 1.0);
	fclose(stdout);
	stdout = saved;
	close(fds[0]);
	const rtc_output_counters *k = &R.c.outputs[1]->counters;
	uint64_t gathered = rtc_counter_get(&R.c.counters.records_gathered);
	CHECK(gathered > 5);
	CHECK_EQ_U(rtc_counter_get(&k->records_sent), 0);
	CHECK_EQ_U(rtc_counter_get(&k->records_dropped), gathered);
	CHECK_EQ_U(rtc_counter_get(&K()->records_sent), gathered); /* Arc unaffected */
	teardown();
	setup_stdout = false;
}

/* Each sender's final flush ignores flush_interval: stop sends everything
 * gathered even when the next flush is far away. */
static void stop_flushes_before_flush_interval(void)
{
	setup_stdout = true;
	CHECK(setup(NULL, 0, 1 << 16, 1 << 14) == 0);
	R.c.cfg.interval_s = 0.02;
	R.c.cfg.flush_interval_s = 30;
	R.c.cfg.shutdown_timeout_s = 1;
	FILE *saved = stdout;
	stdout = fopen("/dev/null", "w");
	CHECK(rtc_collector_start(&R.c) == 0);
	struct timespec ts = {0, 200000000};
	nanosleep(&ts, NULL);
	rtc_collector_stop(&R.c);
	fclose(stdout);
	stdout = saved;
	uint64_t gathered = rtc_counter_get(&R.c.counters.records_gathered);
	CHECK(gathered > 5);
	CHECK_EQ_U(rtc_counter_get(&K()->records_sent), gathered);
	CHECK_EQ_U(rtc_counter_get(&R.c.outputs[1]->counters.records_sent), gathered);
	teardown();
	setup_stdout = false;
}

const t_case lifecycle_cases[] = {
    {"lifecycle", "stop_flushes_before_flush_interval", stop_flushes_before_flush_interval},
    {"lifecycle", "hung_arc_does_not_delay_stdout", hung_arc_does_not_delay_stdout},
    {"lifecycle", "failed_start_unwinds_every_sender", failed_start_unwinds_every_sender},
    {"lifecycle", "full_stdout_pipe_does_not_hold_shutdown", full_stdout_pipe_does_not_hold_shutdown},
    {"lifecycle", "shutdown_counts_unsent_as_dropped", shutdown_counts_unsent_as_dropped},
    {"lifecycle", "shutdown_budget_bounds_a_hanging_request", shutdown_budget_bounds_a_hanging_request},
    {"lifecycle", "threads_deliver_and_stop_cleanly", threads_deliver_and_stop_cleanly},
    {"lifecycle", "once_exit_codes", once_exit_codes},
    {0},
};
