#include "t.h"

#include "../src/core/config.h"

#include <stdlib.h>
#include <string.h>

static int load(rtc_config *c, const char *y)
{
	rtc_config_defaults(c);
	return rtc_config_load_string(c, y, strlen(y));
}

/* The shape of rtcollector's own config.example.yml: Linux inputs we do not
 * have and a redistimeseries output are skipped with warnings. */
static const char *rtcollector_example = "interval: 10\n"
                                         "flush_interval: 60\n"
                                         "max_buffer_size: 5000\n"
                                         "warn_on_buffer: true\n"
                                         "hostname: ''\n"
                                         "debug: true\n"
                                         "once: false\n"
                                         "inputs:\n"
                                         "  - system\n"
                                         "  - linux_cpu\n"
                                         "  - docker:\n"
                                         "      endpoint: \"unix:///var/run/docker.sock\"\n"
                                         "outputs:\n"
                                         "  - redistimeseries:\n"
                                         "      host: localhost\n"
                                         "      port: 6379\n"
                                         "  - arc:\n"
                                         "      url: http://localhost:8000\n"
                                         "      database: telemetry\n";

static void loads_rtcollector_shape(void)
{
	rtc_config c;
	CHECK(load(&c, rtcollector_example) == 0);
	CHECK(c.interval_s == 10 && c.flush_interval_s == 60);
	CHECK(c.debug && !c.once && c.warn_on_buffer);
	CHECK(c.arc.enabled && !strcmp(c.arc.database, "telemetry"));
	CHECK_EQ_U(c.ninputs, 1); /* only internal, auto-added */
	CHECK(!strcmp(c.inputs[0], "internal"));
}

static void defaults(void)
{
	rtc_config c;
	CHECK(load(&c, "outputs:\n  - stdout: {}\n") == 0);
	CHECK(c.flush_interval_s == c.interval_s);
	CHECK_EQ_U(c.buffer_bytes, 1u << 20);
	CHECK_EQ_U(c.max_record_bytes, 4096);
}

static void non_default_values(void)
{
	rtc_config c;
	CHECK(load(&c,
	           "interval: 0.5\nflush_interval: 3\nbuffer_bytes: 64KiB\nmax_record_bytes: 1KiB\n"
	           "tags: {site: lab, rack: \"7\"}\nshutdown_timeout: 0\n"
	           "outputs:\n  - arc:\n      url: https://arc.example:8443/\n      database: d-1\n"
	           "      batch_bytes: 32KiB\n      timeout: 2.5\n      max_measurements_per_batch: 1\n") ==
	      0);
	CHECK(c.interval_s == 0.5 && c.flush_interval_s == 3 && c.shutdown_timeout_s == 0);
	CHECK_EQ_U(c.buffer_bytes, 64u << 10);
	CHECK_EQ_U(c.max_record_bytes, 1024);
	CHECK_EQ_U(c.arc.batch_bytes, 32u << 10);
	CHECK_EQ_U(c.arc.timeout_ms, 2500);
	CHECK_EQ_U(c.arc.max_measurements, 1);
	CHECK_EQ_U(c.ntags, 2);
	CHECK(!strcmp(c.tags[1].key, "rack") && !strcmp(c.tags[1].val, "7"));
}

static void invalid_values_are_refused(void)
{
	rtc_config c;
	const char *bad[] = {
	    "interval: 0\noutputs: [stdout]\n",
	    "interval: -1\noutputs: [stdout]\n",
	    "interval: abc\noutputs: [stdout]\n",
	    "flush_interval: 0\noutputs: [stdout]\n",
	    "buffer_bytes: 10\noutputs: [stdout]\n",
	    "buffer_bytes: 1XB\noutputs: [stdout]\n",
	    "max_record_bytes: 8\noutputs: [stdout]\n",
	    "outputs: []\n",
	    "outputs:\n  - arc: {url: localhost:8000}\n",
	    "outputs:\n  - arc: {url: 'http://x', database: '1bad'}\n",
	    "outputs:\n  - arc: {url: 'http://x', database: 'a/b'}\n",
	    "buffer_bytes: 64KiB\noutputs:\n  - arc: {url: 'http://x', batch_bytes: 1MiB}\n",
	    "outputs:\n  - arc: {url: 'http://x', timeout: 0}\n",
	    "outputs:\n  - arc: {url: 'http://x', max_measurements_per_batch: 0}\n",
	    "warn_on_buffer: maybe\noutputs: [stdout]\n",
	    "tags: [a, b]\noutputs: [stdout]\n",
	    "- just\n- a list\n",
	    "interval: [\n",
	    "tags: {Time: x}\noutputs: [stdout]\n",
	    "tags: {site: a, Site: b}\noutputs: [stdout]\n",
	    "tags: {Host: x}\noutputs: [stdout]\n",
	    "interval: 1e-10\noutputs: [stdout]\n",
	    "outputs:\n  - arc: {url: 'http://x', timeout: 0.0001}\n",
	    "outputs:\n  - arc: {url: 'http://x', max_measurements_per_batch: 257}\n",
	    "outputs:\n  - arc: {url: 'http://x', token: 'secret:arc/token'}\n",
	    "max_record_bytes: 256\noutputs: [stdout]\n",
	    "collect_priority: 0\noutputs: [stdout]\n",
	    "send_priority: 254\noutputs: [stdout]\n",
	    "collect_priority: 10.5\noutputs: [stdout]\n",
	    "processes_every: 0\noutputs: [stdout]\n",
	    "collect_priority: 1e20\noutputs: [stdout]\n",
	    "rmgr_mode: ''\noutputs: [stdout]\n",
	    "rmgr_mode: 0999\noutputs: [stdout]\n",
	    "rmgr_priority: 254\noutputs: [stdout]\n",
	    "max_write_bytes: 32\noutputs: [stdout]\n",
	    "rmgr_drain_bytes: 32MiB\noutputs: [stdout]\n",
	    "rmgr_path: dev/x\noutputs: [stdout]\n",
	    "max_processes: 0\noutputs: [stdout]\n",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		int rc = load(&c, bad[i]);
		if (rc != -1)
			fprintf(stderr, "     accepted: %s\n", bad[i]);
		CHECK(rc == -1);
	}
}

static void sizes(void)
{
	size_t v;
	CHECK(rtc_parse_size("4096", &v) == 0 && v == 4096);
	CHECK(rtc_parse_size("64KiB", &v) == 0 && v == 65536);
	CHECK(rtc_parse_size("1MiB", &v) == 0 && v == 1048576);
	CHECK(rtc_parse_size("2 MB", &v) == 0 && v == 2097152);
	CHECK(rtc_parse_size("-1", &v) == -1);
	CHECK(rtc_parse_size("", &v) == -1);
	CHECK(rtc_parse_size("99999999999999999999GiB", &v) == -1);
}

static void arc_names(void)
{
	CHECK(rtc_valid_arc_name("cpu", 3, 128));
	CHECK(rtc_valid_arc_name("a_b-c9", 6, 128));
	CHECK(!rtc_valid_arc_name("9a", 2, 128));
	CHECK(!rtc_valid_arc_name("a b", 3, 128));
	CHECK(!rtc_valid_arc_name("a.b", 3, 128));
	CHECK(!rtc_valid_arc_name("", 0, 128));
	CHECK(!rtc_valid_arc_name("abcd", 4, 3));
}

static void token_from_env(void)
{
	rtc_config c;
	setenv("RTC_ARC_TOKEN", "secret-token", 1);
	CHECK(load(&c, "outputs:\n  - arc: {url: 'http://x', token: other}\n") == 0);
	CHECK(!strcmp(c.arc.token, "secret-token"));
	unsetenv("RTC_ARC_TOKEN");
}

/* Only buffer_bytes set: the default batch is clamped, not an error about a
 * key the operator never wrote. */
static void small_buffer_clamps_default_batch(void)
{
	rtc_config c;
	CHECK(load(&c, "buffer_bytes: 128KiB\noutputs:\n  - arc: {url: 'http://x'}\n") == 0);
	CHECK(c.arc.batch_bytes <= c.buffer_bytes);
	CHECK(load(&c, "buffer_bytes: 128KiB\noutputs:\n  - arc: {url: 'http://x', batch_bytes: 1MiB}\n") ==
	      -1);
}

static void qnx_keys(void)
{
	rtc_config c;
	CHECK(load(&c, "collect_priority: 20\nsend_priority: 5\nprocesses_every: 1\nmax_processes: 10\n"
	               "outputs:\n  - arc: {url: 'https://x', ca_file: /etc/hosts}\n") == 0);
	CHECK(c.collect_priority == 20 && c.send_priority == 5);
	CHECK(c.processes_every == 1 && c.max_processes == 10);
	CHECK(!strcmp(c.arc.ca_file, "/etc/hosts"));
	/* A CA bundle that cannot be read is refused at load, not discovered at
	 * the first TLS handshake. */
	CHECK(load(&c, "outputs:\n  - arc: {url: 'https://x', ca_file: /nonexistent/ca.pem}\n") == -1);
	CHECK(load(&c, "outputs: [stdout]\n") == 0);
	CHECK(c.collect_priority == 10 && c.send_priority == 9 && c.processes_every == 6);
}

const t_case config_cases[] = {
    {"config", "qnx_keys", qnx_keys},
    {"config", "small_buffer_clamps_default_batch", small_buffer_clamps_default_batch},
    {"config", "loads_rtcollector_shape", loads_rtcollector_shape},
    {"config", "defaults", defaults},
    {"config", "non_default_values", non_default_values},
    {"config", "invalid_values_are_refused", invalid_values_are_refused},
    {"config", "sizes", sizes},
    {"config", "arc_names", arc_names},
    {"config", "token_from_env", token_from_env},
    {0},
};
