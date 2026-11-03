/* cpu: per-core utilisation from the run time of procnto's idle threads.
 *
 * procnto (pid 1) has one idle thread per CPU, tids 1..num_cpu, at priority
 * 0 FIFO. Their run time is the core's idle time. ClockTime(ClockId(1, tid))
 * reads it with a kernel call: no root, no message to procnto.
 *
 *   cpu,cpu=cpu0 usage_percent=12.5,idle_percent=87.5
 *   cpu,cpu=cpu-total ...
 *
 * The baseline is taken at init, so the first round (and --once) reports,
 * over at least 100 ms.
 */
#include "../core/collector.h"
#include "../core/cpumath.h"
#include "../core/log.h"

#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/procfs.h>
#include <sys/syspage.h>
#include <time.h>
#include <unistd.h>

#define MAX_CPUS 64

/* File-static state: one collector per process, so each input's state is a
 * singleton (documented in CLAUDE.md). Collection thread only. */
static struct {
	unsigned ncpu;
	clockid_t id[MAX_CPUS];
	bool usable[MAX_CPUS];    /* ClockId worked at init */
	bool have_base[MAX_CPUS]; /* idle[i]/wall[i] hold a valid baseline */
	uint64_t idle[MAX_CPUS];
	uint64_t wall[MAX_CPUS];
	bool first_round;
} st;

/* With procfs access (root), confirm the idle-thread convention once. */
static void verify_idle_threads(void)
{
	int fd = open("/proc/1/as", O_RDONLY);
	if (fd < 0)
		return; /* not root: rely on the convention */
	for (unsigned i = 0; i < st.ncpu; i++) {
		procfs_status s = {0};
		s.tid = (pthread_t)(i + 1);
		int rc = devctl(fd, DCMD_PROC_TIDSTATUS, &s, sizeof(s), NULL);
		if (rc != EOK)
			RTC_WARN("cpu", "cannot check procnto thread %u: %s", i + 1, strerror(rc));
		else if (s.tid != (pthread_t)(i + 1) || s.priority != 0 || s.policy != SCHED_FIFO)
			RTC_WARN(
			    "cpu",
			    "procnto thread %u is not an idle thread (tid %u, prio %u, policy %u); cpu%u "
			    "may be wrong",
			    i + 1, (unsigned)s.tid, (unsigned)s.priority, (unsigned)s.policy, i);
	}
	close(fd);
}

static void rebase(unsigned i, uint64_t wall)
{
	st.have_base[i] = ClockTime(st.id[i], NULL, &st.idle[i]) == 0;
	st.wall[i] = wall;
}

static int init(rtc_collector *c)
{
	(void)c;
	st.ncpu = _syspage_ptr->num_cpu;
	if (st.ncpu > MAX_CPUS) {
		RTC_WARN("cpu", "%u CPUs; reporting the first %u", st.ncpu, MAX_CPUS);
		st.ncpu = MAX_CPUS;
	}
	uint64_t wall = rtc_mono_ns();
	for (unsigned i = 0; i < st.ncpu; i++) {
		int id = ClockId(1, (int)i + 1);
		st.usable[i] = id != -1;
		st.id[i] = id;
		if (st.usable[i])
			rebase(i, wall);
		if (!st.usable[i] || !st.have_base[i])
			RTC_WARN("cpu", "cannot read the idle time of cpu%u: %s", i, strerror(errno));
	}
	st.first_round = true;
	verify_idle_threads();
	return 0;
}

static int gather(rtc_collector *c)
{
	if (st.first_round) {
		/* The first round follows init closely (with --once, immediately):
		 * give it a window long enough to mean something. */
		st.first_round = false;
		uint64_t since = rtc_mono_ns() - st.wall[0];
		if (since < 100000000u) {
			struct timespec ts = {0, (long)(100000000u - since)};
			nanosleep(&ts, NULL);
		}
	}
	double sum = 0;
	unsigned n = 0;
	int rc = 0;
	for (unsigned i = 0; i < st.ncpu; i++) {
		if (!st.usable[i])
			continue; /* reported once at init */
		uint64_t wall = rtc_mono_ns(), idle;
		if (ClockTime(st.id[i], NULL, &idle) != 0) {
			st.have_base[i] = false; /* re-baseline on the next good read */
			rc = -1;
			continue;
		}
		if (!st.have_base[i]) {
			st.idle[i] = idle;
			st.wall[i] = wall;
			st.have_base[i] = true;
			continue;
		}
		double u = rtc_cpu_usage_percent(st.idle[i], idle, st.wall[i], wall);
		st.idle[i] = idle;
		st.wall[i] = wall;
		if (u < 0)
			continue;
		char tag[16];
		snprintf(tag, sizeof(tag), "cpu%u", i);
		rtc_rec_builder b;
		rtc_rb_init(&b, c->scratch, c->cfg.max_record_bytes, "cpu");
		rtc_rb_tag(&b, "cpu", tag);
		rtc_rb_f64(&b, "usage_percent", u);
		rtc_rb_f64(&b, "idle_percent", 100.0 - u);
		rtc_emit(c, &b);
		sum += u;
		n++;
	}
	if (n) {
		rtc_rec_builder b;
		rtc_rb_init(&b, c->scratch, c->cfg.max_record_bytes, "cpu");
		rtc_rb_tag(&b, "cpu", "cpu-total");
		rtc_rb_f64(&b, "usage_percent", sum / n);
		rtc_rb_f64(&b, "idle_percent", 100.0 - sum / n);
		rtc_rb_i64(&b, "cores", n);
		rtc_emit(c, &b);
	}
	return rc;
}

const rtc_input_def rtc_input_cpu = {"cpu", init, gather};
