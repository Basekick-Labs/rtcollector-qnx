/* trace: QNX kernel trace events as scheduling rows.
 *
 * The collector is its own trace logger (libtracelog, sys/tracelog.h):
 * tracelogger cannot rotate files, and parsing its FIFO stream hangs
 * traceparser. A tracer thread runs tracelog_start; libtracelog calls our
 * writer with raw kernel buffers, which we only copy into the current
 * interval file in /dev/shmem (RAM). A rotator thread flushes every
 * trace_interval, waits for the partial buffers, and switches to the next
 * file, which starts with the session's saved header. A parse thread parses
 * each finished file with libtraceparser into portable scheduling statistics
 * (core/sched.c) and publishes the interval; gather() emits it on the
 * collection thread (the only producer of the inputs' ring).
 *
 * Kernel classes: THREAD (wide: priorities), PROCESS (names), INT
 * (deliveries). The filter is kernel-global and reapplied every session.
 *
 * Cooperative: tracelog_monitor tells us when the trace facility is free.
 * When another tracer (tracelogger, the IDE) acquires it, we stop within
 * milliseconds and resume afterwards (counted as yielded).
 *
 * Needs root or PROCMGR_AID_TRACE: without it tracelog_start fails with
 * EPERM; the input is unavailable (counted) and the collector runs on.
 * Under load the kernel drops trace events (CONTROL DROPPED): counted, and
 * pending per-thread state is reset so no garbage latency is reported.
 *
 *   sched,process=control,thread=loop,pid=..,tid=.. wakeups=..,latency_p50_ns=..,
 *     latency_p99_ns=..,latency_max_ns=..,run_ns=..,preemptions=..,priority=..
 *   cpu_sched,cpu=cpu0 context_switches=..,interrupts=..
 *   irq,vector=0x80010000 count=..
 *   internal_trace sessions=..,yielded=..,intervals=..,bytes=..,...
 */
#include "../core/collector.h"
#include "../core/log.h"
#include "../core/sched.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <sys/trace.h>
#include <sys/tracelog.h>
#include <sys/traceparser.h>
#include <time.h>
#include <unistd.h>

/* libtracelog ships only as a shared library. It is loaded when the input
 * starts, not linked: an image without it must still run the collector, with
 * this input unavailable. The macros send the calls below through it. */
static struct {
	int (*instance_init)(tracelog_instance_t **);
	int (*instance_destroy)(tracelog_instance_t *);
	int (*setwriter)(tracelog_instance_t *, tracelog_write_t, tracelog_writev_t, void *);
	int (*start)(tracelog_instance_t *, int);
	int (*flush)(tracelog_instance_t *);
	int (*stop)(tracelog_instance_t *);
	int (*monitor)(void (*)(int));
	int (*monitor_stop)(void);
} tl;
#define tracelog_instance_init    tl.instance_init
#define tracelog_instance_destroy tl.instance_destroy
#define tracelog_setwriter        tl.setwriter
#define tracelog_start            tl.start
#define tracelog_flush            tl.flush
#define tracelog_stop             tl.stop
#define tracelog_monitor          tl.monitor
#define tracelog_monitor_stop     tl.monitor_stop

static bool load_tracelog(void)
{
	void *h = dlopen("libtracelog.so.1", RTLD_NOW);
	if (!h)
		return false;
	/* Assigned through void ** so the object-to-function pointer conversion
	 * is the one POSIX defines for dlsym. */
	*(void **)&tl.instance_init = dlsym(h, "tracelog_instance_init");
	*(void **)&tl.instance_destroy = dlsym(h, "tracelog_instance_destroy");
	*(void **)&tl.setwriter = dlsym(h, "tracelog_setwriter");
	*(void **)&tl.start = dlsym(h, "tracelog_start");
	*(void **)&tl.flush = dlsym(h, "tracelog_flush");
	*(void **)&tl.stop = dlsym(h, "tracelog_stop");
	*(void **)&tl.monitor = dlsym(h, "tracelog_monitor");
	*(void **)&tl.monitor_stop = dlsym(h, "tracelog_monitor_stop");
	if (!tl.instance_init || !tl.instance_destroy || !tl.setwriter || !tl.start || !tl.flush ||
	    !tl.stop || !tl.monitor || !tl.monitor_stop) {
		memset(&tl, 0, sizeof(tl));
		dlclose(h);
		return false;
	}
	return true; /* never closed: the input lives as long as the process */
}

#define C          "trace"
#define HDR_MAX    (64 * 1024)
#define QUEUE      2 /* finished files waiting to be parsed */
#define PATH_MAX_T 96

/* Counters, one writing thread each. */
typedef struct {
	rtc_counter sessions, yielded;        /* monitor thread */
	rtc_counter bytes, bytes_dropped;     /* writer callback (tracer thread) */
	rtc_counter intervals, files_dropped; /* rotator thread */
	rtc_counter events, kernel_dropped, parse_ns, parse_errors, threads_overflow, snapshots_lost,
	    out_of_order;        /* parser */
	rtc_counter unavailable; /* set once, by whichever thread finds out */
} trace_counters;

/* Singleton state (one collector per process, see cpu.c). */
static struct {
	rtc_collector *c;
	unsigned ncpu;
	trace_counters k;

	/* session (monitor thread starts/stops it) */
	pthread_mutex_t smu;
	tracelog_instance_t *in;
	pthread_t tracer, rotator, monitor, parser;
	bool session, monitor_running, parser_running;
	_Atomic bool stop_rotator, stopping;
	_Atomic bool session_ending; /* session_stop() is stopping the tracer */

	/* current interval file (writer callback and rotator, under fmu) */
	pthread_mutex_t fmu;
	int fd;
	uint64_t file_bytes;
	uint64_t writes;
	char hdr[HDR_MAX];
	size_t hdr_len;
	bool hdr_done;
	unsigned seq;
	char path[PATH_MAX_T];

	/* parse queue (rotator -> parser) */
	pthread_mutex_t qmu;
	pthread_cond_t qcv;
	char queue[QUEUE][PATH_MAX_T];
	unsigned qn;

	/* parser state and the published interval */
	rtc_sched sched;
	rtc_sched_snapshot snap[2]; /* parser fills [back], gather reads [front] */
	unsigned back;
	pthread_mutex_t pmu;
	bool fresh; /* snap[back ^ 1] is published and unread */
	uint64_t published_at;
	rtc_sched_snapshot out; /* gather's copy */

	/* per-file parse context */
	struct traceparser_state *tps;
	uint64_t cps;
} st;

static uint64_t now_real(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* --- writer: raw kernel buffers into the current file ------------------- */

static ssize_t on_write(void *cookie, const char *data, size_t sz)
{
	(void)cookie;
	pthread_mutex_lock(&st.fmu);
	st.writes++;
	/* The session's header (keywords + system page) comes first, in small
	 * writes; keep it to start every later file of the session. */
	if (!st.hdr_done && st.hdr_len + sz <= sizeof(st.hdr)) {
		memcpy(st.hdr + st.hdr_len, data, sz);
		st.hdr_len += sz;
		char *e = memmem(st.hdr, st.hdr_len, "TRACE_HEADER_END::", 18);
		char *sl = memmem(st.hdr, st.hdr_len, "TRACE_SYSPAGE_LEN::", 19);
		if (e && sl) {
			size_t want = (size_t)(e - st.hdr) + 18 + (size_t)strtoul(sl + 19, NULL, 10);
			if (st.hdr_len >= want) {
				st.hdr_len = want;
				st.hdr_done = true;
			}
		}
	}
	ssize_t r = (ssize_t)sz;
	if (st.fd >= 0 && st.file_bytes + sz <= st.c->cfg.trace_max_bytes) {
		if (write(st.fd, data, sz) == (ssize_t)sz) {
			st.file_bytes += sz;
			rtc_counter_add(&st.k.bytes, sz);
		} else {
			rtc_counter_add(&st.k.bytes_dropped, sz);
		}
	} else {
		rtc_counter_add(&st.k.bytes_dropped, sz); /* over trace_max_bytes this interval */
	}
	pthread_mutex_unlock(&st.fmu);
	return r; /* tell libtracelog it was consumed either way */
}

static int open_next(void)
{
	snprintf(st.path, sizeof(st.path), "/dev/shmem/rtcollector-trace-%d-%u.kev", (int)getpid(), st.seq++);
	st.fd = open(st.path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	st.file_bytes = 0;
	return st.fd;
}

/* Hands the finished file to the parser; drops the oldest if it is behind. */
static void enqueue(const char *path)
{
	pthread_mutex_lock(&st.qmu);
	if (st.qn == QUEUE) {
		unlink(st.queue[0]);
		memmove(st.queue[0], st.queue[1], sizeof(st.queue[0]) * (QUEUE - 1));
		st.qn--;
		rtc_counter_add(&st.k.files_dropped, 1);
	}
	snprintf(st.queue[st.qn++], PATH_MAX_T, "%s", path);
	pthread_cond_signal(&st.qcv);
	pthread_mutex_unlock(&st.qmu);
}

/* Closes the current file and starts the next one with the saved header. */
static void rotate(bool start_next)
{
	pthread_mutex_lock(&st.fmu);
	if (st.fd >= 0) {
		close(st.fd);
		st.fd = -1;
		enqueue(st.path);
		rtc_counter_add(&st.k.intervals, 1);
	}
	if (start_next && open_next() >= 0 && st.hdr_done) {
		if (write(st.fd, st.hdr, st.hdr_len) != (ssize_t)st.hdr_len) {
			close(st.fd);
			unlink(st.path);
			st.fd = -1;
		}
	}
	pthread_mutex_unlock(&st.fmu);
}

static void sleep_ms(unsigned ms)
{
	struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}

static void *rotator_main(void *arg)
{
	(void)arg;
	unsigned interval_ms = (unsigned)(st.c->cfg.trace_interval_s * 1000);
	while (!atomic_load(&st.stop_rotator) && rtc_counter_get(&st.k.unavailable) == 0) {
		for (unsigned waited = 0; waited < interval_ms && !atomic_load(&st.stop_rotator);
		     waited += 20)
			sleep_ms(20);
		if (atomic_load(&st.stop_rotator))
			break;
		/* Push the partial kernel buffers out, wait (briefly) for them to
		 * reach the writer, then switch files. */
		pthread_mutex_lock(&st.fmu);
		uint64_t before = st.writes;
		pthread_mutex_unlock(&st.fmu);
		tracelog_flush(st.in);
		for (unsigned w = 0; w < 50; w++) {
			pthread_mutex_lock(&st.fmu);
			uint64_t now = st.writes;
			pthread_mutex_unlock(&st.fmu);
			if (now >= before + st.ncpu)
				break;
			sleep_ms(1);
		}
		rotate(true);
	}
	return NULL;
}

static void *tracer_main(void *arg)
{
	(void)arg;
	/* The writer callback runs on this thread, at its priority. */
	int rc = tracelog_start(st.in, 1);
	/* tracelog_start returns when the session ends; a failure that is not
	 * ours to cause (we stopped it, or the collector is exiting) means the
	 * trace cannot be had: say so in the counters, not only in the log. */
	if (rc != 0 && !atomic_load(&st.session_ending) && !atomic_load(&st.stopping)) {
		if (rc == EPERM)
			RTC_WARN(C, "kernel tracing needs root or PROCMGR_AID_TRACE; the collector runs without it");
		else
			RTC_WARN(C, "kernel tracing failed: %s; the collector runs without it", strerror(rc));
		rtc_counter_set(&st.k.unavailable, 1);
		rtc_counter_set(&st.c->counters.trace_unavailable, 1);
	}
	return NULL;
}

/* Called with smu held. */
static void session_start(void)
{
	if (st.session)
		return;
	if (tracelog_instance_init(&st.in) != 0 || tracelog_setwriter(st.in, on_write, NULL, NULL) != 0) {
		RTC_WARN(C, "cannot set up the trace logger");
		return;
	}
	/* Only what the rows need; the filter is global, so set it every time. */
	TraceEvent(_NTO_TRACE_DELALLCLASSES);
	TraceEvent(_NTO_TRACE_ADDCLASS, _NTO_TRACE_THREAD);
	TraceEvent(_NTO_TRACE_ADDCLASS, _NTO_TRACE_PROCESS);
	TraceEvent(_NTO_TRACE_ADDCLASS, _NTO_TRACE_INT);
	TraceEvent(_NTO_TRACE_SETCLASSWIDE, _NTO_TRACE_THREAD); /* priorities */
	pthread_mutex_lock(&st.fmu);
	st.hdr_len = 0;
	st.hdr_done = false;
	open_next();
	pthread_mutex_unlock(&st.fmu);
	atomic_store(&st.stop_rotator, false);
	atomic_store(&st.session_ending, false);
	if (rtc_start_thread(st.c, &st.tracer, tracer_main, NULL, st.c->cfg.trace_priority, "trace") != 0) {
		tracelog_instance_destroy(st.in);
		st.in = NULL;
		return;
	}
	rtc_start_thread(st.c, &st.rotator, rotator_main, NULL, st.c->cfg.trace_priority, "trace rotator");
	st.session = true;
	rtc_counter_add(&st.k.sessions, 1);
}

/* Called with smu held. */
static void session_stop(void)
{
	if (!st.session)
		return;
	atomic_store(&st.stop_rotator, true);
	atomic_store(&st.session_ending, true);
	pthread_join(st.rotator, NULL);
	tracelog_stop(st.in);
	pthread_join(st.tracer, NULL);
	tracelog_instance_destroy(st.in);
	st.in = NULL;
	rotate(false); /* the partial interval is parsed too */
	st.session = false;
}

static void on_monitor(int acquired)
{
	pthread_mutex_lock(&st.smu);
	if (acquired && !atomic_load(&st.stopping) && rtc_counter_get(&st.k.unavailable) == 0) {
		session_start();
	} else if (!acquired && st.session) {
		session_stop();
		rtc_counter_add(&st.k.yielded, 1); /* another tracer wants it */
	}
	pthread_mutex_unlock(&st.smu);
}

static void *monitor_main(void *arg)
{
	(void)arg;
	int rc = tracelog_monitor(on_monitor);
	if (rc != 0 && rc != EINTR && !atomic_load(&st.stopping)) {
		/* EEXIST: another process holds the monitor (a second collector).
		 * Without it this one can never trace: say so in the counters, not
		 * only in the log. */
		if (rc == EEXIST)
			RTC_WARN(C, "another process monitors the kernel trace (a second collector?); tracing disabled");
		else
			RTC_WARN(C, "trace monitor ended: %s; tracing disabled", strerror(rc));
		rtc_counter_set(&st.k.unavailable, 1);
		rtc_counter_set(&st.c->counters.trace_unavailable, 1);
	}
	return NULL;
}

/* --- parser ---------------------------------------------------------------- */

static uint64_t to_ns(uint64_t cycles)
{
	if (st.cps == 0)
		return 0;
	return cycles / st.cps * 1000000000u + cycles % st.cps * 1000000000u / st.cps;
}

static uint64_t event_ns(struct traceparser_state *s)
{
	unsigned len;
	const uint64_t *clk = traceparser_get_info(s, _TRACEPARSER_INFO_CLK, &len);
	if (st.cps == 0) {
		const char *cps = traceparser_get_info(s, _TRACEPARSER_INFO_CYCLES_PER_SEC, &len);
		st.cps = cps ? strtoull(cps, NULL, 10) : 0;
	}
	return clk ? to_ns(*clk) : 0;
}

static int cb_thread(struct traceparser_state *s, void *u, unsigned h, unsigned t, unsigned *b, unsigned l)
{
	(void)u;
	(void)t;
	if (l < 2)
		return 0;
	unsigned ev = _NTO_TRACE_GETEVENT(h);
	rtc_sched_event e = {.cpu = _NTO_TRACE_GETCPU(h),
	                     .t_ns = event_ns(s),
	                     .pid = b[0],
	                     .tid = b[1],
	                     .prio = l >= 4 ? (int)b[2] : -1};
	if (ev == _NTO_TRACE_THREADY)
		e.kind = RTC_EV_READY;
	else if (ev == _NTO_TRACE_THRUNNING)
		e.kind = RTC_EV_RUNNING;
	else if (ev == _NTO_TRACE_THDEAD || ev == _NTO_TRACE_THDESTROY)
		e.kind = RTC_EV_DEAD;
	else
		e.kind = RTC_EV_BLOCKED;
	rtc_sched_event_in(&st.sched, &e);
	return 0;
}

/* Registered with the external codes; the callback sees the internal ones. */
static int cb_names(struct traceparser_state *s, void *u, unsigned h, unsigned t, unsigned *b, unsigned l)
{
	(void)s;
	(void)u;
	(void)t;
	unsigned ev = _NTO_TRACE_GETEVENT(h);
	if (l < 3)
		return 0;
	char name[RTC_SCHED_NAME * 2];
	size_t room = (l - 2) * sizeof(unsigned);
	if (room > sizeof(name) - 1)
		room = sizeof(name) - 1;
	memcpy(name, &b[2], room);
	name[room] = '\0';
	if (ev == _TRACE_PR_TH_NAME_T)
		rtc_sched_thread_name(&st.sched, b[0], b[1], name);
	else if (ev == _TRACE_PR_TH_CREATE_P_NAME)
		rtc_sched_process_name(&st.sched, b[1], name);
	return 0;
}

static int cb_int(struct traceparser_state *s, void *u, unsigned h, unsigned t, unsigned *b, unsigned l)
{
	(void)s;
	(void)u;
	(void)t;
	if (l >= 1)
		rtc_sched_interrupt(&st.sched, _NTO_TRACE_GETCPU(h), b[0]);
	return 0;
}

static int cb_control(struct traceparser_state *s, void *u, unsigned h, unsigned t, unsigned *b, unsigned l)
{
	(void)s;
	(void)u;
	(void)t;
	if (_NTO_TRACE_GETEVENT(h) == _NTO_TRACE_CONTROLDROPPED && l >= 1) {
		rtc_sched_dropped(&st.sched, b[0]);
		rtc_counter_add(&st.k.kernel_dropped, b[0]);
	}
	return 0;
}

static void parse_file(const char *path)
{
	struct traceparser_state *s = traceparser_init(NULL);
	if (!s) {
		rtc_counter_add(&st.k.parse_errors, 1);
		return;
	}
	st.cps = 0;
	traceparser_cs_range(s, NULL, cb_thread, _NTO_TRACE_THREAD, _NTO_TRACE_THFIRST, _NTO_TRACE_THLAST);
	traceparser_cs(s, NULL, cb_names, _NTO_TRACE_PROCESS, _NTO_TRACE_PROCTHREAD_NAME);
	traceparser_cs(s, NULL, cb_names, _NTO_TRACE_PROCESS, _NTO_TRACE_PROCCREATE_NAME);
	traceparser_cs_range(s, NULL, cb_int, _NTO_TRACE_INT, _NTO_TRACE_INTFIRST, _NTO_TRACE_INTLAST);
	traceparser_cs(s, NULL, cb_control, _NTO_TRACE_CONTROL, _NTO_TRACE_CONTROLDROPPED);
	uint64_t before = st.sched.events;
	struct timespec a, z;
	clock_gettime(CLOCK_MONOTONIC, &a);
	if (traceparser(s, NULL, path) != 0)
		rtc_counter_add(&st.k.parse_errors, 1);
	clock_gettime(CLOCK_MONOTONIC, &z);
	traceparser_destroy(&s);
	rtc_counter_add(&st.k.events, st.sched.events - before);
	rtc_counter_set(&st.k.parse_ns,
	                (uint64_t)(z.tv_sec - a.tv_sec) * 1000000000u + (uint64_t)(z.tv_nsec - a.tv_nsec));
	rtc_counter_set(&st.k.threads_overflow, st.sched.threads_overflow);
	rtc_counter_set(&st.k.out_of_order, st.sched.out_of_order);

	/* Publish this interval. */
	rtc_sched_take(&st.sched, st.ncpu, st.c->cfg.trace_min_priority, st.c->cfg.trace_top,
	               &st.snap[st.back]);
	pthread_mutex_lock(&st.pmu);
	if (st.fresh)
		rtc_counter_add(&st.k.snapshots_lost, 1); /* gather did not take the previous one */
	st.back ^= 1;
	st.fresh = true;
	st.published_at = now_real();
	pthread_mutex_unlock(&st.pmu);
}

static void *parser_main(void *arg)
{
	(void)arg;
	for (;;) {
		char path[PATH_MAX_T];
		pthread_mutex_lock(&st.qmu);
		while (st.qn == 0 && !atomic_load(&st.stopping))
			pthread_cond_wait(&st.qcv, &st.qmu);
		if (st.qn == 0) {
			pthread_mutex_unlock(&st.qmu);
			break;
		}
		snprintf(path, sizeof(path), "%s", st.queue[0]);
		memmove(st.queue[0], st.queue[1], sizeof(st.queue[0]) * (QUEUE - 1));
		st.qn--;
		pthread_mutex_unlock(&st.qmu);
		parse_file(path);
		unlink(path);
	}
	return NULL;
}

/* --- input ------------------------------------------------------------------ */

static int init(rtc_collector *c)
{
	memset(&st, 0, sizeof(st));
	st.c = c;
	st.fd = -1;
	st.ncpu = _syspage_ptr->num_cpu;
	pthread_mutex_init(&st.smu, NULL);
	pthread_mutex_init(&st.fmu, NULL);
	pthread_mutex_init(&st.qmu, NULL);
	pthread_mutex_init(&st.pmu, NULL);
	pthread_cond_init(&st.qcv, NULL);
	if (rtc_sched_init(&st.sched, c->cfg.trace_max_threads) != 0 ||
	    rtc_sched_snapshot_init(&st.snap[0], c->cfg.trace_max_rows) != 0 ||
	    rtc_sched_snapshot_init(&st.snap[1], c->cfg.trace_max_rows) != 0 ||
	    rtc_sched_snapshot_init(&st.out, c->cfg.trace_max_rows) != 0) {
		RTC_ERROR(C, "cannot allocate the trace tables");
		return -1;
	}
	return 0;
}

static void start(rtc_collector *c)
{
	if (!load_tracelog()) {
		RTC_WARN(C, "cannot load libtracelog.so.1 (%s); the collector runs without kernel tracing",
		         dlerror());
		rtc_counter_set(&st.k.unavailable, 1);
		rtc_counter_set(&c->counters.trace_unavailable, 1);
		return;
	}
	/* The parser runs below everything else of ours. */
	int prio = c->cfg.send_priority > 1 ? c->cfg.send_priority - 1 : 1;
	if (rtc_start_thread(c, &st.parser, parser_main, NULL, prio, "trace parser") != 0)
		return;
	st.parser_running = true;
	if (rtc_start_thread(c, &st.monitor, monitor_main, NULL, c->cfg.trace_priority, "trace monitor") == 0)
		st.monitor_running = true;
}

static void stop(rtc_collector *c)
{
	(void)c;
	atomic_store(&st.stopping, true);
	if (st.monitor_running) {
		tracelog_monitor_stop();
		pthread_join(st.monitor, NULL);
		st.monitor_running = false;
	}
	pthread_mutex_lock(&st.smu);
	session_stop();
	pthread_mutex_unlock(&st.smu);
	if (st.parser_running) {
		pthread_mutex_lock(&st.qmu);
		pthread_cond_signal(&st.qcv);
		pthread_mutex_unlock(&st.qmu);
		pthread_join(st.parser, NULL);
		st.parser_running = false;
	}
	/* Files the parser did not get to. */
	pthread_mutex_lock(&st.qmu);
	for (unsigned i = 0; i < st.qn; i++)
		unlink(st.queue[i]);
	st.qn = 0;
	pthread_mutex_unlock(&st.qmu);
}

static void destroy(rtc_collector *c)
{
	(void)c;
	rtc_sched_free(&st.sched);
	rtc_sched_snapshot_free(&st.snap[0]);
	rtc_sched_snapshot_free(&st.snap[1]);
	rtc_sched_snapshot_free(&st.out);
}

static void emit_u(rtc_rec_builder *b, const char *k, uint64_t v)
{
	rtc_rb_i64(b, k, v > INT64_MAX ? INT64_MAX : (int64_t)v);
}

static int gather(rtc_collector *c)
{
	rtc_rec_builder b;
	bool have = false;
	pthread_mutex_lock(&st.pmu);
	if (st.fresh) {
		const rtc_sched_snapshot *p = &st.snap[st.back ^ 1];
		st.out.nrows = p->nrows;
		memcpy(st.out.rows, p->rows, p->nrows * sizeof(*p->rows));
		st.out.threads_omitted = p->threads_omitted;
		st.out.ncpu = p->ncpu;
		memcpy(st.out.cpu_switches, p->cpu_switches, sizeof(p->cpu_switches));
		memcpy(st.out.cpu_interrupts, p->cpu_interrupts, sizeof(p->cpu_interrupts));
		st.out.nvec = p->nvec;
		memcpy(st.out.vec_id, p->vec_id, sizeof(p->vec_id));
		memcpy(st.out.vec_count, p->vec_count, sizeof(p->vec_count));
		st.fresh = false;
		have = true;
	}
	pthread_mutex_unlock(&st.pmu);

	if (have) {
		char pid[16], tid[16];
		for (size_t i = 0; i < st.out.nrows; i++) {
			const rtc_sched_row *r = &st.out.rows[i];
			snprintf(pid, sizeof(pid), "%" PRIu32, r->pid);
			snprintf(tid, sizeof(tid), "%" PRIu32, r->tid);
			rtc_rb_init(&b, c->scratch, c->cfg.max_record_bytes, "sched");
			rtc_rb_tag(&b, "process", r->process[0] ? r->process : pid);
			rtc_rb_tag(&b, "thread", r->thread[0] ? r->thread : tid);
			rtc_rb_tag(&b, "pid", pid);
			rtc_rb_tag(&b, "tid", tid);
			emit_u(&b, "wakeups", r->wakeups);
			emit_u(&b, "latency_p50_ns", r->p50_ns);
			emit_u(&b, "latency_p99_ns", r->p99_ns);
			emit_u(&b, "latency_max_ns", r->max_ns);
			emit_u(&b, "run_ns", r->run_ns);
			emit_u(&b, "preemptions", r->preemptions);
			rtc_rb_i64(&b, "priority", r->prio);
			rtc_emit(c, &b);
		}
		for (unsigned i = 0; i < st.out.ncpu; i++) {
			char cpu[16];
			snprintf(cpu, sizeof(cpu), "cpu%u", i);
			rtc_rb_init(&b, c->scratch, c->cfg.max_record_bytes, "cpu_sched");
			rtc_rb_tag(&b, "cpu", cpu);
			emit_u(&b, "context_switches", st.out.cpu_switches[i]);
			emit_u(&b, "interrupts", st.out.cpu_interrupts[i]);
			rtc_emit(c, &b);
		}
		for (unsigned i = 0; i < st.out.nvec; i++) {
			if (st.out.vec_count[i] == 0)
				continue;
			char vec[16];
			snprintf(vec, sizeof(vec), "0x%08" PRIx32, st.out.vec_id[i]);
			rtc_rb_init(&b, c->scratch, c->cfg.max_record_bytes, "irq");
			rtc_rb_tag(&b, "vector", vec);
			emit_u(&b, "count", st.out.vec_count[i]);
			rtc_emit(c, &b);
		}
	}

	rtc_rb_init(&b, c->scratch, c->cfg.max_record_bytes, "internal_trace");
	emit_u(&b, "sessions", rtc_counter_get(&st.k.sessions));
	emit_u(&b, "yielded", rtc_counter_get(&st.k.yielded));
	emit_u(&b, "intervals", rtc_counter_get(&st.k.intervals));
	emit_u(&b, "bytes", rtc_counter_get(&st.k.bytes));
	emit_u(&b, "bytes_dropped", rtc_counter_get(&st.k.bytes_dropped));
	emit_u(&b, "files_dropped", rtc_counter_get(&st.k.files_dropped));
	emit_u(&b, "events", rtc_counter_get(&st.k.events));
	emit_u(&b, "kernel_dropped", rtc_counter_get(&st.k.kernel_dropped));
	emit_u(&b, "parse_ns", rtc_counter_get(&st.k.parse_ns));
	emit_u(&b, "parse_errors", rtc_counter_get(&st.k.parse_errors));
	emit_u(&b, "threads_overflow", rtc_counter_get(&st.k.threads_overflow));
	emit_u(&b, "out_of_order", rtc_counter_get(&st.k.out_of_order));
	emit_u(&b, "threads_omitted", have ? st.out.threads_omitted : 0);
	emit_u(&b, "snapshots_lost", rtc_counter_get(&st.k.snapshots_lost));
	emit_u(&b, "unavailable", rtc_counter_get(&st.k.unavailable));
	rtc_emit(c, &b);
	return 0;
}

const rtc_input_def rtc_input_trace = {
    .name = "trace", .init = init, .gather = gather, .start = start, .stop = stop, .destroy = destroy};
