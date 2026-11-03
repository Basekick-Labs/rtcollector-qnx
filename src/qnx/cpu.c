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

static struct {
	unsigned ncpu;
	clockid_t id[MAX_CPUS];
	uint64_t idle[MAX_CPUS];
	uint64_t wall;
	bool ok[MAX_CPUS];
} st;

static bool read_idle(unsigned i, uint64_t *ns)
{
	return st.ok[i] && ClockTime(st.id[i], NULL, ns) == 0;
}

/* With procfs access (root), confirm the idle-thread convention once. */
static void verify_idle_threads(void)
{
	int fd = open("/proc/1/as", O_RDONLY);
	if (fd < 0)
		return; /* not root: rely on the convention */
	for (unsigned i = 0; i < st.ncpu; i++) {
		procfs_status s = {0};
		s.tid = (pthread_t)(i + 1);
		if (devctl(fd, DCMD_PROC_TIDSTATUS, &s, sizeof(s), NULL) != EOK ||
		    s.tid != (pthread_t)(i + 1) || s.priority != 0 || s.policy != SCHED_FIFO) {
			RTC_WARN("cpu",
			         "procnto thread %u is not an idle thread (prio %u, policy %u); cpu%u may be "
			         "wrong",
			         i + 1, (unsigned)s.priority, (unsigned)s.policy, i);
		}
	}
	close(fd);
}

static int init(rtc_collector *c)
{
	(void)c;
	st.ncpu = _syspage_ptr->num_cpu;
	if (st.ncpu > MAX_CPUS)
		st.ncpu = MAX_CPUS;
	for (unsigned i = 0; i < st.ncpu; i++) {
		int id = ClockId(1, (int)i + 1);
		st.ok[i] = id != -1;
		st.id[i] = id;
		if (!read_idle(i, &st.idle[i])) {
			st.ok[i] = false;
			RTC_WARN("cpu", "cannot read the idle time of cpu%u: %s", i, strerror(errno));
		}
	}
	st.wall = rtc_mono_ns();
	verify_idle_threads();
	return 0;
}

static int gather(rtc_collector *c)
{
	uint64_t wall = rtc_mono_ns();
	/* The first round follows init closely (with --once, immediately): give
	 * it a window long enough to mean something. Only ever the first round. */
	if (wall - st.wall < 100000000u) {
		uint64_t left = 100000000u - (wall - st.wall);
		struct timespec ts = {0, (long)left};
		nanosleep(&ts, NULL);
		wall = rtc_mono_ns();
	}
	double sum = 0;
	unsigned n = 0;
	int rc = 0;
	for (unsigned i = 0; i < st.ncpu; i++) {
		uint64_t idle;
		if (!read_idle(i, &idle)) {
			rc = -1;
			continue;
		}
		double u = rtc_cpu_usage_percent(st.idle[i], idle, st.wall, wall);
		st.idle[i] = idle;
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
	st.wall = wall;
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
