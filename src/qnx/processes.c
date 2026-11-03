/* processes: process and thread counts.
 *
 * Counting processes needs only readdir("/proc"). Thread counts need
 * DCMD_PROC_INFO on /proc/<pid>/as, which requires the
 * PROCMGR_AID_XPROCESS_QUERY ability (root by default). Access is probed
 * once at init; without it only the process count is reported and the
 * procfs_denied counter says why, instead of failing ~400 opens a round.
 *
 * A scan costs about 50 µs per process, so it runs every processes_every
 * rounds (default 6) and stops at max_processes (overflow counted).
 *
 *   processes processes=18i,threads=131i
 */
#include "../core/collector.h"

#include <devctl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/procfs.h>
#include <unistd.h>

/* Singleton state, see cpu.c. */
static struct {
	bool can_query;
	unsigned round;
} st;

static int init(rtc_collector *c)
{
	int fd = open("/proc/1/as", O_RDONLY);
	st.can_query = fd >= 0;
	if (fd >= 0)
		close(fd);
	else
		rtc_counter_set(&c->counters.procfs_denied, 1);
	st.round = 0;
	return 0;
}

static int gather(rtc_collector *c)
{
	if (st.round++ % c->cfg.processes_every != 0)
		return 0;
	DIR *d = opendir("/proc");
	if (!d)
		return -1;
	uint64_t procs = 0, threads = 0;
	bool overflow = false;
	struct dirent *e;
	while ((e = readdir(d))) {
		char *end;
		long pid = strtol(e->d_name, &end, 10);
		if (*end || pid <= 0)
			continue;
		if (procs == c->cfg.max_processes) {
			overflow = true;
			break;
		}
		procs++;
		if (!st.can_query)
			continue;
		char path[64];
		snprintf(path, sizeof(path), "/proc/%ld/as", pid);
		int fd = open(path, O_RDONLY);
		if (fd < 0) {
			if (errno != ENOENT && errno != ESRCH) /* exited since readdir: not an error */
				rtc_counter_add(&c->counters.processes_query_errors, 1);
			continue;
		}
		procfs_info info;
		int rc = devctl(fd, DCMD_PROC_INFO, &info, sizeof(info), NULL);
		if (rc == EOK)
			threads += info.num_threads;
		else if (rc != ENOENT && rc != ESRCH)
			rtc_counter_add(&c->counters.processes_query_errors, 1);
		close(fd);
	}
	closedir(d);
	if (overflow)
		rtc_counter_add(&c->counters.processes_overflow, 1);
	rtc_rec_builder b;
	rtc_rb_init(&b, c->scratch, c->cfg.max_record_bytes, "processes");
	rtc_rb_i64(&b, "processes", (int64_t)procs);
	if (st.can_query)
		rtc_rb_i64(&b, "threads", (int64_t)threads);
	if (overflow)
		rtc_rb_bool(&b, "truncated", true); /* counts stop at max_processes */
	rtc_emit(c, &b);
	return 0;
}

const rtc_input_def rtc_input_processes = {.name = "processes", .init = init, .gather = gather};
