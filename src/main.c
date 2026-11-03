/* rtcollector: real-time-safe telemetry collector.
 *
 *   rtcollector --config PATH [-d|--debug] [--once]
 *
 * Exit codes: 0 ok, 1 runtime failure (or --once with losses), 2 bad
 * configuration or arguments. */
#include "core/collector.h"
#include "core/log.h"

#include <curl/curl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#ifndef RTC_VERSION
#define RTC_VERSION "0.1.0-dev"
#endif

static void usage(FILE *f)
{
	fprintf(f, "usage: rtcollector --config PATH [-d|--debug] [--once]\n");
}

int main(int argc, char **argv)
{
	const char *path = NULL;
	bool debug = false, once = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--config") && i + 1 < argc)
			path = argv[++i];
		else if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--debug"))
			debug = true;
		else if (!strcmp(argv[i], "--once"))
			once = true;
		else if (!strcmp(argv[i], "--version")) {
			printf("rtcollector %s\n", RTC_VERSION);
			return 0;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(stdout);
			return 0;
		} else {
			usage(stderr);
			return 2;
		}
	}
	if (!path) {
		usage(stderr);
		return 2;
	}

	static rtc_config cfg;
	rtc_config_defaults(&cfg);
	if (rtc_config_load(&cfg, path) != 0)
		return 2;
	if (debug)
		cfg.debug = true;
	if (once)
		cfg.once = true;
	rtc_log_debug_enabled = cfg.debug;

	if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
		RTC_ERROR("main", "cannot initialise libcurl");
		return 1;
	}

	signal(SIGPIPE, SIG_IGN);
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);

	static rtc_collector col;
	if (rtc_collector_init(&col, &cfg) != 0) {
		rtc_collector_destroy(&col);
		curl_global_cleanup();
		return 1;
	}

	int rc = 0;
	if (cfg.once) {
		rc = rtc_collector_run_once(&col);
	} else {
		/* Signals go to sigwait in this thread; block them before any
		 * thread starts so the workers inherit the mask. --once keeps the
		 * default handlers, so Ctrl-C still interrupts it. */
		pthread_sigmask(SIG_BLOCK, &set, NULL);
		RTC_INFO("main",
		         "rtcollector %s starting: interval=%gs flush_interval=%gs buffer=%llu bytes host=%s",
		         RTC_VERSION, cfg.interval_s, cfg.flush_interval_s, (unsigned long long)col.ring.cap,
		         col.cfg.hostname);
		if (rtc_collector_start(&col) != 0) {
			rc = 1;
		} else {
			int sig = 0;
			sigwait(&set, &sig);
			RTC_INFO("main", "signal %d: stopping (final flush up to %gs)", sig,
			         cfg.shutdown_timeout_s);
			rtc_collector_stop(&col);
		}
	}
	rtc_collector_destroy(&col);
	curl_global_cleanup();
	return rc;
}
