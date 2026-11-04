/* CPU hog with a duty cycle: busy for B ms, sleep S ms, for T seconds.
 *   rtc_hog -b 20 -s 5 -t 10 [-p prio] [-c cpu] [-u busy_us -U sleep_us]
 * -p runs it SCHED_FIFO at prio, -c pins it to one CPU; -u/-U give the duty
 * cycle in microseconds instead (a short calibrated disturbance).
 * The sleeps let lower-priority threads run (so a low-priority writer really
 * writes while the hog competes with its handler). */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#ifdef __QNX__
#include <sys/neutrino.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv)
{
	pthread_setname_np(pthread_self(), "hog"); /* the trace validator looks for it */
	long busy = 20, sleep_ms = 5, secs = 10, busy_us = 0, sleep_us = 0, prio = 0, cpu = -1;
	for (int i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "-b"))
			busy = atol(argv[i + 1]);
		else if (!strcmp(argv[i], "-s"))
			sleep_ms = atol(argv[i + 1]);
		else if (!strcmp(argv[i], "-t"))
			secs = atol(argv[i + 1]);
		else if (!strcmp(argv[i], "-u"))
			busy_us = atol(argv[i + 1]);
		else if (!strcmp(argv[i], "-U"))
			sleep_us = atol(argv[i + 1]);
		else if (!strcmp(argv[i], "-p"))
			prio = atol(argv[i + 1]);
		else if (!strcmp(argv[i], "-c"))
			cpu = atol(argv[i + 1]);
	}
#ifdef __QNX__
	if (cpu >= 0 && ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)(1u << cpu)) == -1) {
		perror("rtc_hog: runmask");
		return 2;
	}
#endif
	if (prio > 0) {
		struct sched_param sp = {.sched_priority = (int)prio};
		if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
			fprintf(stderr, "rtc_hog: cannot run at priority %ld\n", prio);
			return 2;
		}
	}
	uint64_t busy_ns = busy_us ? (uint64_t)busy_us * 1000u : (uint64_t)busy * 1000000u;
	uint64_t sleep_ns = busy_us ? (uint64_t)sleep_us * 1000u : (uint64_t)sleep_ms * 1000000u;
	uint64_t end = now_ns() + (uint64_t)secs * 1000000000u;
	volatile uint64_t x = 0;
	while (now_ns() < end) {
		uint64_t until = now_ns() + busy_ns;
		while (now_ns() < until)
			x++;
		struct timespec ts = {(time_t)(sleep_ns / 1000000000u), (long)(sleep_ns % 1000000000u)};
		nanosleep(&ts, NULL);
	}
	return (int)(x & 0);
}
