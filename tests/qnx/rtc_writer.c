/* Test client for /dev/rtcollector (QNX). Writes N lines in one of several
 * client idioms and reports write() latency.
 *
 *   rtc_writer [-p path] [-n lines] [-m publish|snprintf|dprintf|pwrite|zero|big|max] [-s measurement]
 *              [-u sleep_us] [-t] [-T explicit_ts_ns] [-P prio] [-c cpu]
 *
 * -m max writes valid lines of max_write_bytes (4096) each: the largest copy
 * the device does. -P runs SCHED_FIFO at prio, -c pins to one CPU.
 *
 * -t prints p50/p99/max write latency in microseconds. Exit status: 0 if
 * every write succeeded. */
#include "../../include/rtc_publish.h"

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#ifdef __QNX__
#include <sys/neutrino.h>
#endif
#ifdef __QNX__
#include <unix.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static int cmp(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
	const char *path = "/dev/rtcollector", *mode = "publish", *m = "writer_test";
	long n = 10, sleep_us = 0;
	int timing = 0;
	unsigned long long ts = 0;
	int prio = 0, cpu = -1;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-p") && i + 1 < argc)
			path = argv[++i];
		else if (!strcmp(argv[i], "-n") && i + 1 < argc)
			n = atol(argv[++i]);
		else if (!strcmp(argv[i], "-m") && i + 1 < argc)
			mode = argv[++i];
		else if (!strcmp(argv[i], "-s") && i + 1 < argc)
			m = argv[++i];
		else if (!strcmp(argv[i], "-u") && i + 1 < argc)
			sleep_us = atol(argv[++i]);
		else if (!strcmp(argv[i], "-T") && i + 1 < argc)
			ts = strtoull(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "-t"))
			timing = 1;
		else if (!strcmp(argv[i], "-P") && i + 1 < argc)
			prio = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-c") && i + 1 < argc)
			cpu = atoi(argv[++i]);
	}
#ifdef __QNX__
	if (cpu >= 0 && ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)(1u << cpu)) == -1) {
		perror("rtc_writer: runmask");
		return 2;
	}
#endif
	if (prio > 0) {
		struct sched_param sp = {.sched_priority = prio};
		if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
			fprintf(stderr, "rtc_writer: cannot run at priority %d\n", prio);
			return 2;
		}
	}
	int fd = open(path, O_WRONLY);
	if (fd < 0) {
		perror(path);
		return 2;
	}
	uint64_t *lat = timing ? calloc((size_t)n, sizeof(uint64_t)) : NULL;
	int failed = 0;
	for (long i = 0; i < n; i++) {
		uint64_t t0 = now_ns();
		int rc;
		if (!strcmp(mode, "zero")) {
			rc = (int)write(fd, "", 0);
		} else if (!strcmp(mode, "pwrite")) {
			char line[128];
			int len = snprintf(line, sizeof(line), "%s,mode=pwrite seq=%ldi\n", m, i);
			rc = (int)pwrite(fd, line, (size_t)len, 12345);
		} else if (!strcmp(mode, "wblock")) {
			/* writeblock() sends a combine message (lseek + write). */
			static char block[4096];
			memset(block, ' ', sizeof(block));
			int len = snprintf(block, sizeof(block), "%s,mode=wblock seq=%ldi", m, i);
			block[len] = ' ';
			block[sizeof(block) - 1] = '\n';
			rc = writeblock(fd, sizeof(block), 0, 1, block) == 1 ? (int)sizeof(block) : -1;
		} else if (!strcmp(mode, "big")) {
			static char line[8192];
			memset(line, 'x', sizeof(line));
			rc = (int)write(fd, line, sizeof(line));
			if (rc < 0)
				printf("errno=%d %s\n", errno, strerror(errno));
		} else if (!strcmp(mode, "max")) {
			static char line[4096];
			int len = snprintf(line, sizeof(line), "%s,mode=max seq=%ldi,pad=\"", m, i);
			memset(line + len, 'x', sizeof(line) - (size_t)len - 2);
			line[sizeof(line) - 2] = '"';
			line[sizeof(line) - 1] = '\n';
			rc = (int)write(fd, line, sizeof(line));
		} else if (!strcmp(mode, "dprintf")) {
			rc = dprintf(fd, "%s,mode=dprintf seq=%ldi\n", m, i);
		} else if (!strcmp(mode, "snprintf")) {
			char line[256];
			int len = ts ? snprintf(line, sizeof(line), "%s,mode=snprintf seq=%ldi %llu\n", m, i,
			                        ts + (unsigned long long)i)
			             : snprintf(line, sizeof(line), "%s,mode=snprintf seq=%ldi\n", m, i);
			rc = (int)write(fd, line, (size_t)len);
		} else {
			rc = rtc_publish(fd, "%s,mode=publish seq=%ldi", m, i);
		}
		if (lat)
			lat[i] = now_ns() - t0;
		if (rc < 0)
			failed++;
		if (sleep_us)
			usleep((useconds_t)sleep_us);
	}
	if (lat) {
		long worst = 0;
		for (long i = 1; i < n; i++)
			if (lat[i] > lat[worst])
				worst = i;
		printf("worst_index=%ld worst_us=%.1f\n", worst, lat[worst] / 1e3);
		qsort(lat, (size_t)n, sizeof(*lat), cmp);
		printf("writes=%ld failed=%d p50_us=%.1f p99_us=%.1f max_us=%.1f\n", n, failed,
		       lat[n / 2] / 1e3, lat[(size_t)(n * 99 / 100)] / 1e3, lat[n - 1] / 1e3);
	} else {
		printf("writes=%ld failed=%d\n", n, failed);
	}
	close(fd);
	return failed ? 1 : 0;
}
