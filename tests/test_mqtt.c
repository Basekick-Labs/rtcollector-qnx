/* The MQTT output against a scripted broker on loopback. */
#include "t.h"

#include "../src/core/collector.h"
#include "../src/core/config.h"
#include "fake_arc.h"
#include "fake_broker.h"
#include "mp_read.h"

#include <curl/curl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STATUS "rtcollector/pi5"

static rtc_collector C;
static fake_broker *B;

static void sleep_ms(long ms)
{
	struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}

static double since(uint64_t t0)
{
	return (double)(rtc_mono_ns() - t0) / 1e9;
}

static void base_cfg(rtc_config *cfg)
{
	rtc_config_defaults(cfg);
	cfg->mqtt.enabled = true;
	snprintf(cfg->mqtt.url, sizeof(cfg->mqtt.url), "mqtt://127.0.0.1:%d", fake_broker_port(B));
	snprintf(cfg->hostname, sizeof(cfg->hostname), "pi5");
	cfg->buffer_bytes = 1 << 18;
	cfg->max_record_bytes = 512;
	cfg->mqtt.batch_bytes = 1 << 16;
	cfg->mqtt.timeout_ms = 1000;
}

/* Starts the broker with s and a collector with the mqtt output; tweak (may
 * be NULL) changes the configuration before validation. */
static int setup(const fb_script *s, void (*tweak)(rtc_config *))
{
	fb_script sc = *s;
	if (!sc.status_topic)
		sc.status_topic = STATUS;
	B = fake_broker_start(&sc);
	if (!B)
		return -1;
	rtc_config cfg;
	base_cfg(&cfg);
	if (tweak)
		tweak(&cfg);
	if (rtc_config_validate(&cfg) != 0)
		return -1;
	return rtc_collector_init(&C, &cfg);
}

static void teardown(void)
{
	rtc_collector_destroy(&C);
	fake_broker_stop(B);
	B = NULL;
	mp_free_all();
}

static rtc_output *O(void)
{
	return C.outputs[0];
}

static uint64_t K(rtc_counter *k)
{
	return rtc_counter_get(k);
}

static void emit(const char *m, int n)
{
	for (int i = 0; i < n; i++) {
		rtc_rec_builder b;
		rtc_rb_init(&b, C.scratch, C.cfg.max_record_bytes, m);
		rtc_rb_tag(&b, "core", "0");
		rtc_rb_i64(&b, "n", i);
		rtc_emit(&C, &b);
	}
}

/* Final-mode passes until idle, retrying at once (the backoff is the
 * collector's, tested elsewhere). */
static bool drain(int max_passes)
{
	for (int i = 0; i < max_passes; i++) {
		O()->next_attempt_ns = 0;
		if (rtc_sender_pass(&C, true, 0))
			return true;
	}
	return false;
}

/* Data PUBLISH k (presence skipped). */
static bool data_pub(size_t k, fb_pub *p)
{
	for (size_t i = 0, seen = 0; fb_pub_get(B, i, p); i++)
		if (strcmp(p->topic, STATUS) && seen++ == k)
			return true;
	return false;
}

static size_t ndata(void)
{
	size_t n = 0;
	fb_pub p;
	for (size_t i = 0; fb_pub_get(B, i, &p); i++)
		n += strcmp(p.topic, STATUS) != 0;
	return n;
}

static void one_message_per_measurement(void)
{
	fb_script s = {0};
	CHECK(setup(&s, NULL) == 0);
	emit("cpu", 2);
	emit("mem", 1);
	emit("cpu", 1);
	CHECK(drain(10));
	CHECK_EQ_U(K(&O()->counters.records_sent), 4);
	CHECK_EQ_U(K(&O()->counters.requests), 2);
	CHECK_EQ_U(ndata(), 2);
	fb_pub p;
	CHECK(data_pub(0, &p) && !strcmp(p.topic, "rtcollector/pi5/cpu") && p.flags == 0x2);
	mp_node *root = mp_parse(p.payload, p.len);
	CHECK(root && root->t == MP_ARR && root->n == 3);
	mp_node *row = root ? root->kids[0] : NULL;
	CHECK(mp_str_is(mp_get(row, "m"), "cpu"));
	mp_node *t = mp_get(row, "t");
	CHECK(t && t->t == MP_INT && t->i > 1700000000000000 && t->i < 2100000000000000); /* us */
	mp_node *tags = mp_get(row, "tags");
	CHECK(tags && tags->n == 2 && mp_str_is(mp_get(tags, "host"), "pi5") &&
	      mp_str_is(mp_get(tags, "core"), "0"));
	mp_node *f = mp_get(mp_get(row, "fields"), "n");
	CHECK(f && f->t == MP_INT && f->i == 0);
	CHECK(mp_get(root->kids[2], "fields") && mp_get(mp_get(root->kids[2], "fields"), "n")->i == 0);
	CHECK(data_pub(1, &p) && !strcmp(p.topic, "rtcollector/pi5/mem"));
	/* Presence: retained online before the data. */
	fb_pub first;
	CHECK(fb_pub_get(B, 0, &first) && !strcmp(first.topic, STATUS) && first.retain &&
	      !strcmp((char *)first.payload, "online"));
	teardown();
}

static void json_cfg(rtc_config *c)
{
	c->mqtt.json = true;
	c->mqtt.time_div = 1000000; /* ms */
	c->ntags = 1;
	snprintf(c->tags[0].key, sizeof(c->tags[0].key), "site");
	snprintf(c->tags[0].val, sizeof(c->tags[0].val), "lab");
}

static void json_rows(void)
{
	fb_script s = {0};
	CHECK(setup(&s, json_cfg) == 0);
	rtc_rec_builder b;
	rtc_rb_init(&b, C.scratch, C.cfg.max_record_bytes, "app");
	rtc_rb_tag(&b, "host", "a/b"); /* a record's host: in the row, not the topic */
	rtc_rb_tag(&b, "k", "q\"\\\n\x01");
	rtc_rb_f64(&b, "f", 3.0);
	rtc_rb_f64(&b, "big", 1e300);
	rtc_rb_f64(&b, "nan", NAN);
	rtc_rb_i64(&b, "i", -7);
	rtc_rb_bool(&b, "ok", false);
	rtc_rb_str(&b, "s", "\xc3\xa9");
	rtc_emit_record(&C, b.buf, rtc_rb_finish(&b), 1793734227123456789ull);
	CHECK(drain(10));
	fb_pub p;
	CHECK(data_pub(0, &p) && !strcmp(p.topic, "rtcollector/pi5/app"));
	const char *want =
	    "[{\"m\":\"app\",\"t\":1793734227123,\"tags\":{\"host\":\"a/b\",\"site\":\"lab\","
	    "\"k\":\"q\\\"\\\\\\u000a\\u0001\"},\"fields\":{\"f\":3.0,\"big\":1.0000000000000001e+300,"
	    "\"i\":-7,\"ok\":false,\"s\":\"\xc3\xa9\"}}]";
	CHECK(p.payload && !strcmp((char *)p.payload, want));
	if (p.payload && strcmp((char *)p.payload, want))
		fprintf(stderr, "     got:  %s\n     want: %s\n", (char *)p.payload, want);
	CHECK_EQ_U(K(&O()->counters.fields_nonfinite), 1);
	CHECK_EQ_U(K(&O()->counters.records_sent), 1);
	teardown();
}

static void cfg_host_tag(rtc_config *c)
{
	c->ntags = 1;
	snprintf(c->tags[0].key, sizeof(c->tags[0].key), "host");
	snprintf(c->tags[0].val, sizeof(c->tags[0].val), "cell-3");
	c->mqtt.status_topic[0] = '\0';
}

/* {host} is the configured host (tags.host here), never a record's. */
static void topic_host_is_configured(void)
{
	fb_script s = {0};
	CHECK(setup(&s, cfg_host_tag) == 0);
	rtc_rec_builder b;
	rtc_rb_init(&b, C.scratch, C.cfg.max_record_bytes, "app");
	rtc_rb_tag(&b, "host", "robot-7");
	rtc_rb_i64(&b, "x", 1);
	rtc_emit(&C, &b);
	CHECK(drain(10));
	fb_pub p;
	CHECK(fb_npubs(B) == 1 && fb_pub_get(B, 0, &p) && !strcmp(p.topic, "rtcollector/cell-3/app"));
	mp_node *root = mp_parse(p.payload, p.len);
	CHECK(root && mp_str_is(mp_get(mp_get(root->kids[0], "tags"), "host"), "cell-3"));
	teardown();
}

static void connack_refused_keeps_the_data(void)
{
	fb_script s = {.connack = {5, 4}, .nconnack = 2};
	CHECK(setup(&s, NULL) == 0);
	emit("cpu", 3);
	O()->next_attempt_ns = 0;
	CHECK(!rtc_sender_pass(&C, true, 0));
	CHECK_EQ_U(K(&O()->counters.last_status), 5);
	CHECK_EQ_U(K(&O()->counters.request_errors), 1);
	CHECK_EQ_U(K(&O()->counters.records_sent), 0);
	CHECK(drain(10));
	CHECK_EQ_U(fb_connects(B), 3);
	CHECK_EQ_U(K(&O()->counters.last_status), 0);
	CHECK_EQ_U(K(&O()->counters.records_sent), 3);
	CHECK_EQ_U(K(&O()->counters.records_rejected) + K(&O()->counters.records_dropped), 0);
	teardown();
}

static void session_present_is_refused(void)
{
	fb_script s = {.session_present = true};
	CHECK(setup(&s, NULL) == 0);
	emit("cpu", 1);
	O()->next_attempt_ns = 0;
	CHECK(!rtc_sender_pass(&C, true, 0));
	CHECK_EQ_U(ndata(), 0);
	CHECK_EQ_U(K(&O()->counters.request_errors), 1);
	teardown();
}

static void short_timeout(rtc_config *c)
{
	c->mqtt.timeout_ms = 300;
}

/* No PUBACK within timeout: the connection is closed (a late PUBACK must
 * never reach a later message) and the rows go again on a new one. */
static void puback_timeout_reconnects_and_resends(void)
{
	fb_script s = {.action = {FB_SILENT}, .naction = 1};
	CHECK(setup(&s, short_timeout) == 0);
	emit("cpu", 4);
	O()->next_attempt_ns = 0;
	CHECK(!rtc_sender_pass(&C, true, 0));
	CHECK_EQ_U(K(&O()->counters.records_sent), 0);
	CHECK_EQ_U(K(&O()->counters.request_errors), 1);
	CHECK(drain(5));
	CHECK_EQ_U(fb_connects(B), 2);
	CHECK_EQ_U(K(&O()->counters.records_sent), 4);
	CHECK_EQ_U(K(&O()->counters.records_resent), 4);
	CHECK_EQ_U(K(&O()->counters.records_outcome_unknown), 0);
	teardown();
}

/* Nothing listening: kept, counted, and nothing sent. */
static void unreachable_broker_keeps_the_data(void)
{
	fb_script s = {0};
	CHECK(setup(&s, NULL) == 0);
	rtc_collector_destroy(&C);
	int port = fake_broker_port(B);
	fake_broker_stop(B); /* the port is closed now */
	B = NULL;
	rtc_config cfg;
	rtc_config_defaults(&cfg);
	cfg.mqtt.enabled = true;
	snprintf(cfg.mqtt.url, sizeof(cfg.mqtt.url), "mqtt://127.0.0.1:%d", port);
	snprintf(cfg.hostname, sizeof(cfg.hostname), "pi5");
	cfg.max_record_bytes = 512;
	CHECK(rtc_config_validate(&cfg) == 0);
	CHECK(rtc_collector_init(&C, &cfg) == 0);
	emit("cpu", 2);
	for (int i = 0; i < 3; i++) {
		O()->next_attempt_ns = 0;
		CHECK(!rtc_sender_pass(&C, true, 0));
	}
	CHECK_EQ_U(K(&O()->counters.request_errors), 3);
	CHECK_EQ_U(K(&O()->counters.records_sent), 0);
	CHECK(O()->has_batch);
	rtc_collector_account_unsent(&C, false);
	CHECK_EQ_U(K(&O()->counters.records_dropped), 2);
	rtc_collector_destroy(&C);
}

static void small_ceiling(rtc_config *c)
{
	c->mqtt.max_message_bytes = 1 << 16;
}

/* A close after a PUBLISH: the rows are sent again (records_resent) and
 * messages are cut at half that size; 32 acknowledged messages later the
 * ceiling doubles. */
static void close_after_publish_resends_and_cuts(void)
{
	fb_script s = {.action = {FB_CLOSE}, .naction = 1};
	CHECK(setup(&s, small_ceiling) == 0);
	emit("cpu", 200);
	O()->next_attempt_ns = 0;
	CHECK(!rtc_sender_pass(&C, true, 0));
	CHECK_EQ_U(K(&O()->counters.records_sent), 0);
	fb_pub p;
	CHECK(data_pub(0, &p));
	size_t first = p.len;
	CHECK(drain(20));
	CHECK_EQ_U(K(&O()->counters.records_sent), 200);
	CHECK_EQ_U(K(&O()->counters.records_resent), 200);
	CHECK(ndata() >= 3);
	for (size_t i = 1; data_pub(i, &p); i++)
		CHECK(p.len <= first / 2);
	teardown();
}

static void ceiling_grows_back(void)
{
	fb_script s = {.action = {FB_CLOSE}, .naction = 1};
	CHECK(setup(&s, small_ceiling) == 0);
	emit("cpu", 40);
	CHECK(drain(20));
	fb_pub p;
	CHECK(data_pub(0, &p));
	size_t full = p.len;
	size_t before = ndata();
	/* 40 rows took one message, then halves; now 32 more acks, one row each
	 * per measurement. */
	for (int i = 0; i < 32; i++) {
		char m[16];
		snprintf(m, sizeof(m), "m%d", i);
		emit(m, 1);
	}
	CHECK(drain(80));
	/* The ceiling is back to twice S/2 rounded down: one byte short of the
	 * first packet, so 39 of those rows fit in one message again. */
	emit("cpu", 39);
	CHECK(drain(20));
	size_t last = ndata();
	CHECK(data_pub(last - 1, &p));
	CHECK_EQ_U(last, before + 32 + 1);
	mp_node *root = mp_parse(p.payload, p.len);
	CHECK(root && root->n == 39 && p.len < full);
	teardown();
}

static void tiny_packets(rtc_config *c)
{
	c->mqtt.status_topic[0] = '\0';
}

/* A row the broker always refuses (its packet limit): dropped after exactly
 * 3 closes, each on a fresh connection. */
static void one_row_exit_after_three_closes(void)
{
	fb_script s = {.max_packet = 40};
	CHECK(setup(&s, tiny_packets) == 0);
	emit("cpu", 1);
	for (int i = 0; i < 2; i++) {
		O()->next_attempt_ns = 0;
		CHECK(!rtc_sender_pass(&C, true, 0));
	}
	CHECK_EQ_U(K(&O()->counters.records_rejected), 0); /* not after 2 */
	CHECK(drain(3));
	CHECK_EQ_U(fb_connects(B), 3);
	CHECK_EQ_U(K(&O()->counters.records_rejected), 1);
	CHECK_EQ_U(K(&O()->counters.records_sent), 0);
	teardown();
}

/* The shutdown deadline after the whole message was written: outcome
 * unknown, and send() returns with nothing in flight. */
static void deadline_after_written_is_outcome_unknown(void)
{
	fb_script s = {.action = {FB_SILENT}, .naction = 1};
	CHECK(setup(&s, NULL) == 0);
	emit("cpu", 5);
	uint64_t deadline = rtc_mono_ns() + 300000000u;
	atomic_store(&C.abort_after_ns, deadline);
	uint64_t t0 = rtc_mono_ns();
	while (rtc_mono_ns() < deadline && !rtc_sender_pass(&C, true, deadline))
		sleep_ms(10);
	CHECK(since(t0) < 0.8);
	CHECK_EQ_U(K(&O()->counters.records_outcome_unknown), 5);
	CHECK_EQ_U(K(&O()->counters.records_sent), 0);
	CHECK(!O()->has_batch);
	teardown();
}

static void big_messages(rtc_config *c)
{
	c->buffer_bytes = 32u << 20;
	c->max_record_bytes = 4096;
	c->mqtt.batch_bytes = 16u << 20;
	c->mqtt.max_message_bytes = 16u << 20;
}

/* The deadline before the whole message is written (the broker stopped
 * reading): the records stay, and are counted as dropped at shutdown. */
static void deadline_before_written_is_kept(void)
{
	fb_script s = {0};
	CHECK(setup(&s, big_messages) == 0);
	emit("cpu", 1); /* connected, then the broker stops reading */
	CHECK(drain(5));
	sleep_ms(50);
	fake_broker_pause_reads(B, true);
	/* 12 MB in one message: more than both socket buffers hold. */
	static char blob[3800];
	memset(blob, 'x', sizeof(blob) - 1);
	for (int i = 0; i < 3200; i++) {
		rtc_rec_builder b;
		rtc_rb_init(&b, C.scratch, C.cfg.max_record_bytes, "big");
		rtc_rb_str(&b, "s", blob);
		rtc_emit(&C, &b);
	}
	uint64_t deadline = rtc_mono_ns() + 400000000u;
	atomic_store(&C.abort_after_ns, deadline);
	while (rtc_mono_ns() < deadline && !rtc_sender_pass(&C, true, deadline))
		sleep_ms(10);
	CHECK(O()->has_batch);
	CHECK_EQ_U(K(&O()->counters.records_outcome_unknown), 0);
	rtc_collector_account_unsent(&C, false);
	CHECK_EQ_U(K(&O()->counters.records_dropped), 3200);
	CHECK_EQ_U(K(&O()->counters.records_sent), 1);
	fake_broker_pause_reads(B, false);
	teardown();
}

static void threads_cfg(rtc_config *c)
{
	c->interval_s = 0.05;
	c->flush_interval_s = 0.05;
	c->shutdown_timeout_s = 1;
}

/* Presence over a whole run: will in the CONNECT, retained online, and at a
 * clean stop retained offline then DISCONNECT. */
static void presence_online_offline_disconnect(void)
{
	fb_script s = {0};
	CHECK(setup(&s, threads_cfg) == 0);
	CHECK(rtc_collector_start(&C) == 0);
	for (int i = 0; i < 100 && ndata() == 0; i++)
		sleep_ms(20);
	rtc_collector_stop(&C);
	size_t n = fb_npubs(B);
	fb_pub first, last;
	CHECK(fb_pub_get(B, 0, &first) && !strcmp(first.topic, STATUS) && first.retain &&
	      !strcmp((char *)first.payload, "online"));
	CHECK(fb_pub_get(B, n - 1, &last) && !strcmp(last.topic, STATUS) && last.retain &&
	      !strcmp((char *)last.payload, "offline"));
	for (int i = 0; i < 100 && fb_disconnects(B) == 0; i++)
		sleep_ms(10); /* the broker thread reads it after stop returns */
	CHECK_EQ_U(fb_disconnects(B), 1);
	uint8_t conn[512];
	size_t cl = fb_last_connect(B, conn, sizeof(conn));
	CHECK(cl > 8 && conn[7] == 0x2e); /* will retain, will QoS 1, will, clean session */
	teardown();
}

static void once_cfg(rtc_config *c)
{
	c->once = true;
}

static void once_has_no_presence(void)
{
	fb_script s = {0};
	CHECK(setup(&s, once_cfg) == 0);
	CHECK(rtc_collector_run_once(&C) == 0);
	CHECK(ndata() >= 1);
	CHECK_EQ_U(fb_npubs(B), ndata()); /* no presence */
	uint8_t conn[512];
	CHECK(fb_last_connect(B, conn, sizeof(conn)) > 8 && conn[7] == 0x02); /* no will */
	teardown();
}

/* A message stuck at the deadline: no DISCONNECT, so the broker fires the will. */
static void out_of_time_stop_has_no_disconnect(void)
{
	fb_script s = {.action = {FB_SILENT}, .naction = 1};
	CHECK(setup(&s, threads_cfg) == 0);
	C.cfg.shutdown_timeout_s = 0.3;
	CHECK(rtc_collector_start(&C) == 0);
	for (int i = 0; i < 100 && ndata() == 0; i++)
		sleep_ms(20);
	uint64_t t0 = rtc_mono_ns();
	rtc_collector_stop(&C);
	CHECK(since(t0) < 0.8);
	CHECK_EQ_U(fb_disconnects(B), 0);
	CHECK(K(&O()->counters.records_outcome_unknown) >= 1);
	teardown();
}

static void keepalive_cfg(rtc_config *c)
{
	threads_cfg(c);
	c->mqtt.keepalive_s = 1;
	c->interval_s = 30; /* one round, then idle */
	c->flush_interval_s = 0.05;
}

static void keepalive_pings_and_closes_without_pingresp(void)
{
	fb_script s = {0};
	CHECK(setup(&s, keepalive_cfg) == 0);
	CHECK(rtc_collector_start(&C) == 0);
	for (int i = 0; i < 100 && ndata() == 0; i++)
		sleep_ms(20);
	sleep_ms(1300);
	CHECK(fb_pings(B) >= 1);
	CHECK(fb_connected(B));
	rtc_collector_stop(&C);
	teardown();

	fb_script s2 = {.no_pingresp = true};
	CHECK(setup(&s2, keepalive_cfg) == 0);
	C.cfg.mqtt.timeout_ms = 500;
	CHECK(rtc_collector_start(&C) == 0);
	for (int i = 0; i < 100 && ndata() == 0; i++)
		sleep_ms(20);
	sleep_ms(1500);
	CHECK(fb_pings(B) >= 1);
	CHECK(!fb_connected(B)); /* no PINGRESP within timeout: closed */
	rtc_collector_stop(&C);
	teardown();
}

/* Rows waiting for flush_interval (not due yet) still get keepalive. */
static void keepalive_while_rows_wait_for_flush(void)
{
	fb_script s = {0};
	CHECK(setup(&s, NULL) == 0);
	C.cfg.mqtt.keepalive_s = 1;
	C.cfg.flush_interval_s = 60;
	emit("cpu", 1);
	CHECK(drain(5)); /* connected */
	emit("cpu", 1);
	O()->last_flush_ns = rtc_mono_ns();
	sleep_ms(600);
	CHECK(rtc_sender_pass(&C, false, 0)); /* not due: nothing sent */
	CHECK_EQ_U(K(&O()->counters.records_sent), 1);
	for (int i = 0; i < 50 && fb_pings(B) == 0; i++)
		sleep_ms(10);
	CHECK_EQ_U(fb_pings(B), 1);
	teardown();
}

/* Only closes on a fresh connection count toward the one-row exit: a close
 * after the connection acknowledged data is not evidence against the row. */
static void one_row_exit_counts_fresh_connections_only(void)
{
	fb_script s = {.action = {FB_ACK, FB_CLOSE, FB_CLOSE, FB_CLOSE}, .naction = 4};
	CHECK(setup(&s, tiny_packets) == 0);
	emit("a", 1);
	emit("b", 1);
	CHECK(drain(10));
	CHECK_EQ_U(K(&O()->counters.records_sent), 2);
	CHECK_EQ_U(K(&O()->counters.records_rejected), 0);
	CHECK_EQ_U(fb_connects(B), 4); /* three closes, then b goes through */
	teardown();
}

/* The broker going away while idle is noticed (EOF), and the next message
 * reconnects. */
static void eof_while_idle_reconnects(void)
{
	fb_script s = {0};
	CHECK(setup(&s, keepalive_cfg) == 0);
	C.cfg.mqtt.keepalive_s = 0;
	emit("cpu", 1);
	CHECK(drain(5));
	fake_broker_drop_client(B);
	for (int i = 0; i < 50 && fb_connected(B); i++)
		sleep_ms(10);
	O()->idle(O(), rtc_mono_ns());
	emit("cpu", 1);
	CHECK(drain(5));
	CHECK_EQ_U(fb_connects(B), 2);
	CHECK_EQ_U(K(&O()->counters.records_sent), 2);
	CHECK_EQ_U(K(&O()->counters.records_resent), 0);
	teardown();
}

/* A broker that accepts and never answers holds neither shutdown nor Arc. */
static void hung_broker_holds_neither_shutdown_nor_arc(void)
{
	static bool curl_ready;
	if (!curl_ready) {
		curl_global_init(CURL_GLOBAL_DEFAULT);
		curl_ready = true;
	}
	fake_arc *arc = fake_arc_start(NULL, 0);
	fb_script s = {.no_connack = true, .status_topic = STATUS};
	B = fake_broker_start(&s);
	rtc_config cfg;
	base_cfg(&cfg);
	threads_cfg(&cfg);
	cfg.mqtt.timeout_ms = 30000;
	cfg.shutdown_timeout_s = 0.5;
	cfg.arc.enabled = true;
	snprintf(cfg.arc.url, sizeof(cfg.arc.url), "http://127.0.0.1:%d", fake_arc_port(arc));
	cfg.arc.batch_bytes = 1 << 14;
	CHECK(rtc_config_validate(&cfg) == 0);
	CHECK(rtc_collector_init(&C, &cfg) == 0);
	CHECK(rtc_collector_start(&C) == 0);
	sleep_ms(600);
	CHECK(fake_arc_requests(arc) >= 2);
	uint64_t t0 = rtc_mono_ns();
	rtc_collector_stop(&C);
	CHECK(since(t0) < 1.0);
	CHECK_EQ_U(K(&C.outputs[1]->counters.records_sent), 0);
	rtc_collector_destroy(&C);
	fake_broker_stop(B);
	fake_arc_stop(arc);
	B = NULL;
}

static void client_id_of(const char *host, char *out, size_t cap)
{
	fb_script s = {0};
	B = fake_broker_start(&s);
	rtc_config cfg;
	base_cfg(&cfg);
	snprintf(cfg.hostname, sizeof(cfg.hostname), "%s", host);
	CHECK(rtc_config_validate(&cfg) == 0);
	CHECK(rtc_collector_init(&C, &cfg) == 0);
	emit("cpu", 1);
	CHECK(drain(5));
	uint8_t conn[512];
	size_t n = fb_last_connect(B, conn, sizeof(conn));
	size_t il = n > 12 ? (size_t)(conn[10] << 8 | conn[11]) : 0;
	snprintf(out, cap, "%.*s", (int)il, (char *)conn + 12);
	rtc_collector_destroy(&C);
	fake_broker_stop(B);
	B = NULL;
}

/* Two hosts that share a long prefix, or one host started twice, get
 * different ids. */
static void client_ids_differ(void)
{
	char a[64], b[64], a2[64];
	client_id_of("robot-arm-controller-01", a, sizeof(a));
	client_id_of("robot-arm-controller-02", b, sizeof(b));
	client_id_of("robot-arm-controller-01", a2, sizeof(a2));
	CHECK(strlen(a) == 19 && !strncmp(a, "rtc", 3));
	CHECK(strncmp(a, b, 11) != 0);                    /* the host hash differs */
	CHECK(!strncmp(a, a2, 11) && strcmp(a, a2) != 0); /* same host, new random part */
}

/* --- configuration ------------------------------------------------------- */

static int load(const char *yaml)
{
	rtc_config cfg;
	rtc_config_defaults(&cfg);
	return rtc_config_load_string(&cfg, yaml, strlen(yaml));
}

static void config_validation(void)
{
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://10.0.2.2:18830}\n") == 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: 'mqtt://[::1]'}\n") == 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtts://broker}\n") != 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://u:p@broker}\n") != 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://broker/x}\n") != 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://broker:0}\n") != 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: http://broker}\n") != 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {}\n") != 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b, password: x}\n") != 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b, username: u, password: x}\n") == 0);
	CHECK(
	    load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b, username: u, password: 'secret:x'}\n") !=
	    0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b, topic: 'a/+/{measurement}'}\n") != 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b, topic: '$SYS/{measurement}'}\n") != 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b, topic: 'a/{nope}'}\n") != 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b, topic: "
	           "'{measurement}/{measurement}'}\n") != 0);
	CHECK(load("hostname: 'pi#5'\noutputs:\n  - mqtt: {url: mqtt://b}\n") != 0); /* {host} expanded */
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b, status_topic: "
	           "'rtcollector/{host}/up'}\n") != 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b, topic: 'x/{host}', status_topic: "
	           "'x/{host}'}\n") != 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b, status_topic: "
	           "'rtcollector/{host}/9up'}\n") == 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b, status_topic: 's/{measurement}'}\n") !=
	      0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b, status_topic: ''}\n") == 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b, max_message_bytes: 16KiB}\n") != 0);
	/* The floor follows the configuration: small records, a small message. */
	CHECK(load("hostname: pi5\nmax_record_bytes: 512\noutputs:\n  - mqtt: {url: mqtt://b, "
	           "max_message_bytes: 4KiB}\n") == 0);
	CHECK(load("hostname: pi5\nmax_record_bytes: 512\ntags: {site: "
	           "'0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567"
	           "890123456789'}\n"
	           "outputs:\n  - mqtt: {url: mqtt://b, max_message_bytes: 4KiB}\n") != 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b, keepalive: 65536}\n") != 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b, timeout: 0.5}\n") != 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b, format: xml}\n") != 0);
	CHECK(load("hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b, time_precision: min}\n") != 0);

	rtc_config cfg;
	rtc_config_defaults(&cfg);
	const char *y =
	    "hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b:1884, format: json, time_precision: ms,"
	    " keepalive: 0, timeout: 2, client_id: me}\n";
	CHECK(rtc_config_load_string(&cfg, y, strlen(y)) == 0);
	CHECK(cfg.mqtt.json && cfg.mqtt.time_div == 1000000 && cfg.mqtt.keepalive_s == 0 &&
	      cfg.mqtt.timeout_ms == 2000 && !strcmp(cfg.mqtt.client_id, "me"));
	CHECK(!strcmp(cfg.mqtt.host, "b") && !strcmp(cfg.mqtt.port, "1884"));
	setenv("RTC_MQTT_PASSWORD", "fromenv", 1);
	rtc_config_defaults(&cfg);
	y = "hostname: pi5\noutputs:\n  - mqtt: {url: mqtt://b, username: u, password: 'secret:x'}\n";
	CHECK(rtc_config_load_string(&cfg, y, strlen(y)) == 0 && !strcmp(cfg.mqtt.password, "fromenv"));
	unsetenv("RTC_MQTT_PASSWORD");
}

/* An mqtt-only configuration builds a ring for one output. */
static void mqtt_only_collector(void)
{
	fb_script s = {0};
	CHECK(setup(&s, NULL) == 0);
	CHECK_EQ_U(C.noutputs, 1);
	CHECK_EQ_U(C.ring.noutputs, 1);
	CHECK(!strcmp(O()->name, "mqtt"));
	teardown();
}

const t_case mqtt_cases[] = {
    {"mqtt", "one_message_per_measurement", one_message_per_measurement},
    {"mqtt", "json_rows", json_rows},
    {"mqtt", "topic_host_is_configured", topic_host_is_configured},
    {"mqtt", "connack_refused_keeps_the_data", connack_refused_keeps_the_data},
    {"mqtt", "session_present_is_refused", session_present_is_refused},
    {"mqtt", "puback_timeout_reconnects_and_resends", puback_timeout_reconnects_and_resends},
    {"mqtt", "unreachable_broker_keeps_the_data", unreachable_broker_keeps_the_data},
    {"mqtt", "close_after_publish_resends_and_cuts", close_after_publish_resends_and_cuts},
    {"mqtt", "ceiling_grows_back", ceiling_grows_back},
    {"mqtt", "one_row_exit_after_three_closes", one_row_exit_after_three_closes},
    {"mqtt", "deadline_after_written_is_outcome_unknown", deadline_after_written_is_outcome_unknown},
    {"mqtt", "deadline_before_written_is_kept", deadline_before_written_is_kept},
    {"mqtt", "presence_online_offline_disconnect", presence_online_offline_disconnect},
    {"mqtt", "once_has_no_presence", once_has_no_presence},
    {"mqtt", "out_of_time_stop_has_no_disconnect", out_of_time_stop_has_no_disconnect},
    {"mqtt", "keepalive_pings_and_closes_without_pingresp", keepalive_pings_and_closes_without_pingresp},
    {"mqtt", "keepalive_while_rows_wait_for_flush", keepalive_while_rows_wait_for_flush},
    {"mqtt", "one_row_exit_counts_fresh_connections_only", one_row_exit_counts_fresh_connections_only},
    {"mqtt", "eof_while_idle_reconnects", eof_while_idle_reconnects},
    {"mqtt", "hung_broker_holds_neither_shutdown_nor_arc", hung_broker_holds_neither_shutdown_nor_arc},
    {"mqtt", "client_ids_differ", client_ids_differ},
    {"mqtt", "config_validation", config_validation},
    {"mqtt", "mqtt_only_collector", mqtt_only_collector},
    {0},
};
