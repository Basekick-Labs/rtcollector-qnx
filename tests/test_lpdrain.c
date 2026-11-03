/* The drain: raw writes in the rmgr ring -> binary records in the collector's
 * ring, on the collection thread. Host-tested with a ring filled by the test
 * in place of the resource manager. */
#include "t.h"

#include "../src/core/collector.h"
#include "../src/core/lpdrain.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <string.h>

static rtc_collector C;
static rtc_ring LPR;
static rtc_lp_drain D;

static void setup(size_t lp_ring_bytes)
{
	rtc_config cfg;
	rtc_config_defaults(&cfg);
	cfg.out_stdout.enabled = true;
	snprintf(cfg.hostname, sizeof(cfg.hostname), "pi5");
	CHECK(rtc_config_validate(&cfg) == 0);
	CHECK(rtc_collector_init(&C, &cfg) == 0);
	CHECK(rtc_ring_init(&LPR, lp_ring_bytes, 4097, 1) == 0);
	CHECK(rtc_lp_drain_init(&D, &LPR, 4096, 64u << 10) == 0);
	C.lp_drain = &D;
}

static void teardown(void)
{
	rtc_lp_drain_free(&D);
	rtc_ring_free(&LPR);
	rtc_collector_destroy(&C);
}

/* What io_write does: [kind][bytes] with the receive time. */
static enum rtc_push_result write_raw(const char *s, uint64_t recv_ts)
{
	uint8_t buf[4200];
	size_t n = strlen(s);
	buf[0] = RTC_REC_LP;
	memcpy(buf + 1, s, n);
	return rtc_ring_push(&LPR, buf, (uint32_t)n + 1, recv_ts);
}

/* Records now in the collector's ring, decoded. */
static size_t collected(rtc_rec_view *v, uint64_t *ts, size_t max)
{
	static uint8_t dst[1 << 16];
	static rtc_ring_rec recs[512];
	uint64_t dropped;
	size_t n = rtc_ring_read(&C.ring, 0, dst, sizeof(dst), recs, 512, NULL, NULL, &dropped);
	for (size_t i = 0; i < n && i < max; i++) {
		CHECK(rtc_rec_decode(recs[i].payload, recs[i].len, &v[i]));
		ts[i] = recs[i].ts_ns;
	}
	return n;
}

static void lines_timestamps_and_counts(void)
{
	setup(1 << 16);
	uint64_t recv = rtc_now_ns();
	write_raw("a v=1i\n", recv);
	char two[128];
	uint64_t explicit_ts = recv - 3600ull * 1000000000ull; /* an hour ago, in ns */
	snprintf(two, sizeof(two), "b v=2i\nc v=3i %llu\n", (unsigned long long)explicit_ts);
	write_raw(two, recv + 1);      /* two lines, one with its own timestamp */
	write_raw("d v=4i", recv + 2); /* no trailing newline: a line */
	write_raw("garbage\nm v=1 1800000000\n# comment\n\n", recv + 3); /* malformed, bad ts, blank */
	write_raw("big=1u,x=18446744073709551615u", recv + 4);           /* malformed: no measurement/field */
	CHECK_EQ_U(rtc_lp_drain_step(&C, &D), 5);
	rtc_rec_view v[16];
	uint64_t ts[16];
	size_t n = collected(v, ts, 16);
	CHECK_EQ_U(n, 4);
	CHECK(v[0].measurement.len == 1 && v[0].measurement.p[0] == 'a' && ts[0] == recv);
	CHECK(v[1].measurement.p[0] == 'b' && ts[1] == recv + 1);
	CHECK(v[2].measurement.p[0] == 'c' && ts[2] == explicit_ts);
	CHECK(v[3].measurement.p[0] == 'd' && ts[3] == recv + 2);
	CHECK_EQ_U(rtc_counter_get(&C.counters.lp_lines), 4);
	CHECK_EQ_U(rtc_counter_get(&C.counters.lp_malformed), 2);
	CHECK_EQ_U(rtc_counter_get(&C.counters.lp_bad_timestamp), 1);
	CHECK_EQ_U(rtc_lp_drain_step(&C, &D), 0);
	teardown();
}

/* A writer faster than the drain: overwritten writes are counted. */
static void overflow_counted(void)
{
	setup(4096 * 2 + 64);
	uint64_t recv = rtc_now_ns();
	int ok = 0;
	for (int i = 0; i < 2000; i++) /* 28 bytes each: well past the 16 KiB ring */
		ok += write_raw("m v=1i\n", recv) == RTC_PUSH_OK;
	rtc_lp_drain_step(&C, &D);
	uint64_t lines = rtc_counter_get(&C.counters.lp_lines);
	uint64_t lost = rtc_counter_get(&C.counters.rmgr_drain_dropped);
	CHECK(lost > 0);
	CHECK_EQ_U(lines + lost, (uint64_t)ok);
	teardown();
}

/* Receive time before the clock floor: counted, not sent. */
static void receive_time_floor(void)
{
	setup(1 << 16);
	write_raw("m v=1i\n", 1000); /* 1970 */
	rtc_lp_drain_step(&C, &D);
	CHECK_EQ_U(rtc_counter_get(&C.counters.records_clock_invalid), 1);
	rtc_rec_view v[2];
	uint64_t ts[2];
	CHECK_EQ_U(collected(v, ts, 2), 0);
	teardown();
}

/* A writer thread in place of the resource manager while the collector's
 * threads run and stop: every accepted write is converted or counted. */
static _Atomic bool writer_stop;
static uint64_t writer_ok;

static void *writer(void *arg)
{
	(void)arg;
	uint64_t recv = rtc_now_ns();
	while (!atomic_load(&writer_stop)) {
		if (write_raw("w,k=v x=1i\n", recv) == RTC_PUSH_OK)
			writer_ok++;
	}
	return NULL;
}

static void threaded_drain_accounts_for_every_write(void)
{
	setup(1 << 14);
	FILE *saved = stdout;
	stdout = fopen("/dev/null", "w");
	C.cfg.interval_s = 0.02;
	C.cfg.flush_interval_s = 0.02;
	atomic_store(&writer_stop, false);
	writer_ok = 0;
	pthread_t t;
	CHECK(rtc_collector_start(&C) == 0);
	pthread_create(&t, NULL, writer, NULL);
	struct timespec ts = {0, 300000000};
	nanosleep(&ts, NULL);
	atomic_store(&writer_stop, true); /* the "rmgr" stops before the collector, as in production */
	pthread_join(t, NULL);
	rtc_collector_stop(&C);
	fclose(stdout);
	stdout = saved;
	uint64_t lines = rtc_counter_get(&C.counters.lp_lines);
	uint64_t lost = rtc_counter_get(&C.counters.rmgr_drain_dropped);
	fprintf(stderr, "     writes=%llu converted=%llu overwritten=%llu\n", (unsigned long long)writer_ok,
	        (unsigned long long)lines, (unsigned long long)lost);
	CHECK(writer_ok > 1000);
	CHECK_EQ_U(lines + lost, writer_ok);
	teardown();
}

const t_case lpdrain_cases[] = {
    {"lpdrain", "lines_timestamps_and_counts", lines_timestamps_and_counts},
    {"lpdrain", "overflow_counted", overflow_counted},
    {"lpdrain", "receive_time_floor", receive_time_floor},
    {"lpdrain", "threaded_drain_accounts_for_every_write", threaded_drain_accounts_for_every_write},
    {0},
};
