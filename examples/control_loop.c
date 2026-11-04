/* rtc_loop: a periodic real-time loop that measures itself (QNX 8.0).
 *
 * It is the instrument for rtcollector's overhead measurement and the
 * example application of the codelab: a 1 kHz control loop that publishes a
 * few counters to /dev/rtcollector.
 *
 *   rtc_loop [-p prio] [-c cpu] [-P period_us] [-w work_us] [-d deadline_us]
 *            [-t seconds] [-r publish_hz] [-m sleep|precise]
 *            [-H histogram.csv] [-L max_p50_wake_us] [-W warmup_ms] [-I work_iters]
 *
 * Each cycle k is released at r_k = t0 + k * period. After waking it records
 *   wake     = now - r_k                   (how late the wake-up was)
 * then does a fixed amount of work (iterations calibrated at start to about
 * work_us, or -I iterations, so runs under different loads do the same work),
 * optionally publishes, and records
 *   response = end of cycle - r_k          (what a deadline is judged on)
 *   publish  = duration of the write() to /dev/rtcollector
 * A miss is a cycle whose response exceeds the deadline (default: the
 * period). A cycle that ends at or after the next release skips the
 * releases it ran over (counted once as an overrun, and per release as
 * skipped). The published window counters include warm-up cycles; the
 * summary's counts do not.
 *
 * Nothing in the loop allocates, logs or blocks except the publish write.
 * The device is opened once before the loop; if a write fails the loop stops
 * publishing (publish_errors) and never reopens. The first warmup_ms of
 * cycles are run but not counted.
 *
 * At the end, one summary line on stdout (all times in ns):
 *   rtc_loop mech=.. n=.. wake_p50=.. ... resp_max=.. misses=.. overruns=..
 * and, with -H, every non-empty histogram bucket as CSV for pooling runs.
 * Exit status: 0; 2 bad arguments or setup failure; 3 the wake mechanism is
 * not supported or too coarse (p50 wake above -L). */
#include "../include/rtc_publish.h"
#include "loopstats.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <time.h>
#include <unistd.h>

/* sleep: clock_nanosleep, absolute; precise: the same with TIMER_PRECISE (a
 * high-resolution timer, not rounded to the clock tick). A timer with a pulse
 * was tried and dropped: on QNX 8.0 under QEMU its pulse never arrived. */
enum { M_SLEEP, M_PRECISE };
static const char *mech_name[] = {"sleep", "precise"};

static volatile sig_atomic_t stop;
static void on_signal(int s)
{
	(void)s;
	stop = 1;
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static struct timespec ts_of(uint64_t ns)
{
	return (struct timespec){(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
}

static volatile uint64_t sink;

static void work(uint64_t iters)
{
	uint64_t x = sink;
	for (uint64_t i = 0; i < iters; i++)
		x = x * 6364136223846793005ull + 1442695040888963407ull;
	sink = x;
}

/* Iterations that take about us microseconds on this CPU, now. */
static uint64_t calibrate(uint64_t us)
{
	if (!us)
		return 0;
	uint64_t iters = 1000;
	for (;;) {
		uint64_t t = now_ns();
		work(iters);
		uint64_t d = now_ns() - t;
		if (d >= 20000000u) /* measured over 20 ms at least */
			return iters * us * 1000u / d;
		iters *= 2;
	}
}

/* Waits until release (ns, CLOCK_MONOTONIC). 0, or an errno value. */
static int wait_until(int mech, uint64_t release)
{
	struct timespec ts = ts_of(release);
	unsigned flags = TIMER_ABSTIME | (mech == M_PRECISE ? TIMER_PRECISE : 0);
	int r;
	while ((r = clock_nanosleep(CLOCK_MONOTONIC, (int)flags, &ts, NULL)) == EINTR && !stop)
		;
	return r == EINTR ? 0 : r;
}

static ls_hist h_wake, h_resp, h_pub;

static void dump(FILE *f, const char *name, const ls_hist *h)
{
	for (unsigned i = 0; i < LS_BUCKETS; i++)
		if (h->b[i])
			fprintf(f, "%s,%" PRIu64 ",%" PRIu64 "\n", name, ls_lower(i), h->b[i]);
	fprintf(f, "%s_max,%" PRIu64 ",%" PRIu64 "\n", name, h->max_ns, h->n);
}

static void summary(const char *k, const ls_hist *h)
{
	printf(" %s_n=%" PRIu64 " %s_p50=%" PRIu64 " %s_p99=%" PRIu64 " %s_p999=%" PRIu64 " %s_p9999=%" PRIu64
	       " %s_max=%" PRIu64,
	       k, h->n, k, ls_quantile(h, 0.5), k, ls_quantile(h, 0.99), k, ls_quantile(h, 0.999), k,
	       ls_quantile(h, 0.9999), k, h->max_ns);
}

int main(int argc, char **argv)
{
	int prio = 50, cpu = 3, mech = M_SLEEP;
	uint64_t period_us = 1000, work_us = 100, deadline_us = 0, secs = 60, rate = 0, max_p50_us = 0,
	         warm_ms = 1000, fixed_iters = 0;
	const char *hist_path = NULL;
	if ((argc - 1) % 2) {
		fprintf(stderr, "rtc_loop: every option takes a value\n");
		return 2;
	}
	for (int i = 1; i + 1 < argc; i += 2) {
		const char *a = argv[i], *v = argv[i + 1];
		if (!strcmp(a, "-p"))
			prio = atoi(v);
		else if (!strcmp(a, "-c"))
			cpu = atoi(v);
		else if (!strcmp(a, "-P"))
			period_us = strtoull(v, NULL, 10);
		else if (!strcmp(a, "-w"))
			work_us = strtoull(v, NULL, 10);
		else if (!strcmp(a, "-d"))
			deadline_us = strtoull(v, NULL, 10);
		else if (!strcmp(a, "-t"))
			secs = strtoull(v, NULL, 10);
		else if (!strcmp(a, "-r"))
			rate = strtoull(v, NULL, 10);
		else if (!strcmp(a, "-L"))
			max_p50_us = strtoull(v, NULL, 10);
		else if (!strcmp(a, "-W"))
			warm_ms = strtoull(v, NULL, 10);
		else if (!strcmp(a, "-I"))
			fixed_iters = strtoull(v, NULL, 10);
		else if (!strcmp(a, "-H"))
			hist_path = v;
		else if (!strcmp(a, "-m")) {
			mech = !strcmp(v, "precise") ? M_PRECISE : M_SLEEP;
			if (strcmp(v, mech_name[mech])) {
				fprintf(stderr, "rtc_loop: -m sleep|precise\n");
				return 2;
			}
		} else {
			fprintf(stderr, "rtc_loop: unknown option %s\n", a);
			return 2;
		}
	}
	if (period_us < 50 || period_us > 1000000 || secs < 1 || prio < 1 || prio > 253 || cpu < 0 ||
	    cpu > 31) {
		fprintf(stderr, "rtc_loop: bad arguments\n");
		return 2;
	}
	uint64_t period = period_us * 1000u, deadline = (deadline_us ? deadline_us : period_us) * 1000u;
	uint64_t pub_every = rate ? 1000000u / period_us / rate : 0;
	if (rate && !pub_every)
		pub_every = 1;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);
	pthread_setname_np(pthread_self(), "loop");
	if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)(1u << cpu)) == -1) {
		perror("rtc_loop: runmask");
		return 2;
	}
	struct sched_param sp = {.sched_priority = prio};
	if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
		fprintf(stderr, "rtc_loop: cannot run at priority %d (root?)\n", prio);
		return 2;
	}
	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
		perror("rtc_loop: mlockall (continuing)");
	{
		volatile char stack[64 * 1024]; /* prefault the stack, publish frame included */
		for (size_t i = 0; i < sizeof(stack); i += 512)
			stack[i] = 0;
	}
	int fd = -1;
	if (rate) {
		fd = open("/dev/rtcollector", O_WRONLY);
		if (fd < 0)
			perror("rtc_loop: /dev/rtcollector (publishing off)");
	}
	ls_init(&h_wake);
	ls_init(&h_resp);
	ls_init(&h_pub);
	uint64_t iters = fixed_iters ? fixed_iters : calibrate(work_us);
	struct _clockperiod cp = {0, 0};
	(void)ClockPeriod(CLOCK_REALTIME, NULL, &cp, 0);

	uint64_t misses = 0, overruns = 0, skipped = 0, publish_errors = 0, warm_cycles = 0;
	uint64_t w_wake = 0, w_resp = 0, w_miss = 0, w_cycles = 0; /* the published window */
	uint64_t t0 = now_ns() + 10000000u, warm_end = t0 + warm_ms * 1000000u;
	uint64_t end_at = warm_end + secs * 1000000000u;
	uint64_t k = 0;
	while (!stop) {
		uint64_t release = t0 + k * period;
		if (release >= end_at)
			break;
		int e = wait_until(mech, release);
		if (e) {
			fprintf(stderr, "rtc_loop: %s wake-up: %s\n", mech_name[mech], strerror(e));
			return 3;
		}
		if (stop)
			break; /* woken by a signal, not by the release */
		uint64_t woke = now_ns();
		work(iters);
		uint64_t pub_ns = 0;
		bool published = false;
		if (fd >= 0 && pub_every && k % pub_every == 0) {
			uint64_t t = now_ns();
			int rc = rtc_publish(fd,
			                     "control_loop,loop=main wake_max_ns=%" PRIu64
			                     "i,response_max_ns=%" PRIu64 "i,misses=%" PRIu64
			                     "i,cycles=%" PRIu64 "i",
			                     w_wake, w_resp, w_miss, w_cycles);
			pub_ns = now_ns() - t;
			published = true;
			if (rc < 0) {
				publish_errors++;
				fd = -1; /* stop publishing; never reopen inside the loop */
			}
			w_wake = w_resp = w_miss = w_cycles = 0;
		}
		uint64_t end = now_ns();
		uint64_t wake = woke > release ? woke - release : 0, resp = end - release;
		if (release < warm_end) {
			warm_cycles++;
		} else {
			ls_add(&h_wake, wake);
			ls_add(&h_resp, resp);
			if (published)
				ls_add(&h_pub, pub_ns);
			if (resp > deadline) {
				misses++;
				w_miss++;
			}
		}
		if (wake > w_wake)
			w_wake = wake;
		if (resp > w_resp)
			w_resp = resp;
		w_cycles++;
		bool over;
		uint64_t before = skipped;
		k = ls_next_release(t0, period, k, end, &skipped, &over);
		if (release < warm_end)
			skipped = before; /* warm-up: not counted */
		else
			overruns += over;
	}

	printf("rtc_loop mech=%s prio=%d cpu=%d period_ns=%" PRIu64 " deadline_ns=%" PRIu64
	       " work_iters=%" PRIu64 " warmup_cycles=%" PRIu64 " clock_period_ns=%lu rate_hz=%" PRIu64,
	       mech_name[mech], prio, cpu, period, deadline, iters, warm_cycles, (unsigned long)cp.nsec,
	       rate);
	summary("wake", &h_wake);
	summary("resp", &h_resp);
	summary("pub", &h_pub);
	printf(" resp_ge20us=%" PRIu64 " resp_ge50us=%" PRIu64 " resp_ge100us=%" PRIu64
	       " resp_ge200us=%" PRIu64,
	       ls_above(&h_resp, 20000), ls_above(&h_resp, 50000), ls_above(&h_resp, 100000),
	       ls_above(&h_resp, 200000));
	printf(" misses=%" PRIu64 " overruns=%" PRIu64 " skipped=%" PRIu64 " publish_errors=%" PRIu64 "\n",
	       misses, overruns, skipped, publish_errors);
	if (hist_path) {
		FILE *f = fopen(hist_path, "w");
		if (f) {
			dump(f, "wake", &h_wake);
			dump(f, "resp", &h_resp);
			dump(f, "pub", &h_pub);
			fclose(f);
		} else
			perror(hist_path);
	}
	if (max_p50_us && ls_quantile(&h_wake, 0.5) > max_p50_us * 1000u) {
		fprintf(stderr,
		        "rtc_loop: %s wake-ups are too coarse (p50 %" PRIu64 " ns > %" PRIu64 " us)\n",
		        mech_name[mech], ls_quantile(&h_wake, 0.5), max_p50_us);
		return 3;
	}
	return 0;
}
