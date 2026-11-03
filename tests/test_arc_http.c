/* The Arc output against a scripted server: one test per status shape. */
#include "t.h"

#include "../src/core/collector.h"
#include "fake_arc.h"
#include "mp_read.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

#ifndef RTC_ASAN /* set by `make SANITIZE=1` */
#define RTC_ASAN 0
#endif

typedef struct {
	rtc_collector c;
	fake_arc *f;
} rig;

static rig R;

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
	cfg.max_record_bytes = 256;
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

static int drain(int max_passes)
{
	for (int i = 0; i < max_passes; i++)
		if (rtc_sender_pass(&R.c, true, 0))
			return i;
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
	    {500, "{\"error\":\"write stopped\",\"rows_written\":2,\"written_measurements\":[\"cpu\"],"
	          "\"stopped_at_measurement\":\"mem\"}"}};
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

const t_case arc_http_cases[] = {
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
