/* rmgr: /dev/rtcollector, a QNX resource manager programs write line
 * protocol to, one write() per line (or several whole lines):
 *
 *   char line[256];
 *   int n = snprintf(line, sizeof line, "loop,axis=x jitter_us=%di\n", j);
 *   write(fd, line, n);
 *
 * The write handler is the hot path: a real-time thread waits on it. It
 * copies the bytes into a preallocated ring and replies. No allocation, no
 * logging; work bounded by the size of the write. (The resource-manager
 * layer takes the device's attribute lock around each handler; held for the
 * copy only.) Parsing happens later on the collection thread (lpdrain.c).
 *
 * A small pool of dispatch threads (rmgr_threads, default 4), each a bare
 * dispatch_block/dispatch_handler loop with its own ring, so every ring has
 * one producer. Why a pool: in QNX 8 a message send can be preempted inside
 * the kernel call, and the server thread it is being delivered to stays
 * reserved for it. With one thread every other writer then waits until the
 * preempted sender runs again (traced: a priority-5 writer preempted mid-send
 * by a priority-30 CPU hog held a priority-50 writer for 20-47 ms; with 4
 * threads the worst write was 44 us). The channel has a FIXED priority
 * (rmgr_priority, default 63, meant to be above every writer): with priority
 * inheritance a low-priority writer's handler ran at that writer's priority
 * and anything in between could preempt it mid-handler.
 *
 * Opens and closes run on the same threads and allocate the per-open state
 * (iofunc defaults): open the device once, not per write.
 *
 * Contract for writers (README): while the collector runs, a write waits for
 * at most a handler run per writer ahead of it, unless more than
 * rmgr_threads lower-priority senders are preempted mid-send at once. If the
 * collector is stopped (SIGSTOP), writes block. When it shuts down the
 * channel is destroyed at once, so writes fail (EBADF; reopen). Writes over
 * max_write_bytes fail with EMSGSIZE. Data lost because a buffer is full is
 * counted, never reported to the writer.
 */

/* Before the QNX headers: the OCB type the handlers receive. */
struct _iofunc_ocb;
#define RESMGR_OCB_T struct _iofunc_ocb
#define IOFUNC_OCB_T struct _iofunc_ocb

#include "../core/collector.h"
#include "../core/log.h"
#include "../core/lpdrain.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define C "rmgr"

/* Singleton state (one collector per process, see cpu.c). */
static struct {
	rtc_collector *c;
	unsigned n; /* handler threads */
	rtc_ring ring[RTC_RMGR_MAX_THREADS];
	rtc_lp_drain drain[RTC_RMGR_MAX_THREADS];
	uint8_t *scratch[RTC_RMGR_MAX_THREADS]; /* one write each, kind byte first */
	size_t max_write;
	int chid;
	dispatch_t *dpp;
	int id;
	int pulse_code;
	int coid;
	pthread_t thread[RTC_RMGR_MAX_THREADS];
	unsigned started;
	bool running;
	volatile bool stopping;
	_Atomic bool dead;   /* a dispatch thread exited on an error */
	name_attach_t *lock; /* /dev/name/local/rtcollector<path>: one collector per path */
	resmgr_connect_funcs_t connect_funcs;
	resmgr_io_funcs_t io_funcs;
	iofunc_attr_t attr;
	char status[512];
	int status_len;
} st;

/* Which handler thread this is: its ring, scratch buffer and counters. */
static _Thread_local unsigned self;

static int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb)
{
	int rc = iofunc_write_verify(ctp, msg, ocb, NULL);
	if (rc != EOK)
		return rc;
	size_t off = sizeof(msg->i);
	switch (msg->i.xtype & _IO_XTYPE_MASK) {
	case _IO_XTYPE_NONE:
		break;
	case _IO_XTYPE_OFFSET: /* pwrite: a stream, the offset is ignored */
		off += sizeof(struct _xtype_offset);
		break;
	default:
		return ENOSYS;
	}
	uint64_t nbytes = _IO_WRITE_GET_NBYTES(msg);
	rtc_rmgr_counters *k = &st.c->rmgr_counters[self];
	uint8_t *scratch = st.scratch[self];
	if (nbytes > st.max_write) {
		rtc_counter_add(&k->oversize, 1);
		return EMSGSIZE;
	}
	/* The writer's priority (the handler itself runs at rmgr_priority). */
	if (ctp->info.priority > 0 && (uint64_t)ctp->info.priority > rtc_counter_get(&k->max_prio_seen))
		rtc_counter_set(&k->max_prio_seen, (uint64_t)ctp->info.priority);
	if (nbytes > 0) {
		/* The receive buffer is sized for the largest write, so the bytes
		 * are normally already here: copy them rather than MsgRead. */
		if (ctp->info.msglen >= 0 && (uint64_t)ctp->info.msglen >= off + nbytes) {
			memcpy(scratch + 1, (const char *)msg + off, nbytes);
		} else {
			rtc_counter_add(&k->msgreads, 1);
			if (resmgr_msgread(ctp, scratch + 1, nbytes, off) != (ssize_t)nbytes) {
				rtc_counter_add(&k->errors, 1);
				return EFAULT;
			}
		}
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		uint64_t now = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
		if (rtc_ring_push(&st.ring[self], scratch, (uint32_t)nbytes + 1, now) != RTC_PUSH_OK)
			rtc_counter_add(&k->dropped, 1);
		rtc_counter_add(&k->writes, 1);
		rtc_counter_add(&k->bytes, nbytes);
	}
	_IO_SET_WRITE_NBYTES(ctp, nbytes);
	return _RESMGR_NPARTS(0);
}

/* `cat /dev/rtcollector`: one status line, then EOF. The snapshot is taken
 * at offset 0, so small reads continue the same line. */
static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
	int rc = iofunc_read_verify(ctp, msg, ocb, NULL);
	if (rc != EOK)
		return rc;
	if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
		return ENOSYS;
	if (ocb->offset == 0) {
		rtc_rmgr_counters k;
		rtc_rmgr_sum(st.c, &k);
		const rtc_collect_counters *q = &st.c->counters;
		st.status_len = snprintf(
		    st.status, sizeof(st.status),
		    "rtcollector writes=%llu bytes=%llu dropped=%llu oversize=%llu lines=%llu malformed=%llu "
		    "bad_timestamp=%llu too_large=%llu max_writer_prio=%llu rmgr_prio=%d threads=%u\n",
		    (unsigned long long)rtc_counter_get(&k.writes),
		    (unsigned long long)rtc_counter_get(&k.bytes),
		    (unsigned long long)(rtc_counter_get(&k.dropped) +
		                         rtc_counter_get(&q->rmgr_drain_dropped)),
		    (unsigned long long)rtc_counter_get(&k.oversize),
		    (unsigned long long)rtc_counter_get(&q->lp_lines),
		    (unsigned long long)rtc_counter_get(&q->lp_malformed),
		    (unsigned long long)rtc_counter_get(&q->lp_bad_timestamp),
		    (unsigned long long)rtc_counter_get(&q->lp_too_large),
		    (unsigned long long)rtc_counter_get(&k.max_prio_seen), st.c->cfg.rmgr_priority, st.n);
		if (st.status_len < 0)
			st.status_len = 0;
		if ((size_t)st.status_len >= sizeof(st.status))
			st.status_len = sizeof(st.status) - 1;
	}
	off_t pos = ocb->offset;
	size_t n = 0;
	if (pos < st.status_len) {
		n = (size_t)(st.status_len - pos);
		if (n > msg->i.nbytes)
			n = msg->i.nbytes;
		ocb->offset += (off_t)n;
	}
	_IO_SET_READ_NBYTES(ctp, n);
	if (n == 0)
		return _RESMGR_NPARTS(0);
	SETIOV(ctp->iov, st.status + pos, n);
	return _RESMGR_NPARTS(1);
}

static int on_pulse(message_context_t *ctp, int code, unsigned flags, void *handle)
{
	(void)ctp;
	(void)code;
	(void)flags;
	(void)handle;
	st.stopping = true;
	return 0;
}

static void *dispatch_main(void *arg)
{
	self = (unsigned)(uintptr_t)arg;
	dispatch_context_t *ctp = dispatch_context_alloc(st.dpp);
	int err = ctp ? 0 : errno;
	while (ctp && !st.stopping) {
		dispatch_context_t *got = dispatch_block(ctp);
		if (!got) {
			if (errno == EINTR)
				continue;
			err = errno;
			break;
		}
		ctp = got;
		dispatch_handler(ctp);
	}
	if (ctp)
		dispatch_context_free(ctp);
	if (!st.stopping && !atomic_exchange(&st.dead, true)) {
		/* Died on an error: unregister so new opens fail, and say so.
		 * stop() destroys the channel (writers then fail, not block). */
		RTC_ERROR(C, "%s stopped on an error (%s)", st.c->cfg.rmgr_path, strerror(err));
		rtc_counter_set(&st.c->counters.rmgr_unavailable, 1);
		resmgr_detach(st.dpp, st.id, _RESMGR_DETACH_ALL);
	}
	return NULL;
}

static int init(rtc_collector *c)
{
	memset(&st, 0, sizeof(st));
	st.c = c;
	st.id = -1;
	st.coid = -1;
	st.chid = -1;
	atomic_init(&st.dead, false);
	st.max_write = c->cfg.max_write_bytes;
	st.n = c->cfg.rmgr_threads;
	c->rmgr_nthreads = st.n;
	for (unsigned i = 0; i < st.n; i++) {
		st.scratch[i] = malloc(st.max_write + 1);
		if (!st.scratch[i] ||
		    rtc_ring_init(&st.ring[i], c->cfg.rmgr_buffer_bytes / st.n, (uint32_t)st.max_write + 1,
		                  1) != 0 ||
		    rtc_lp_drain_init(&st.drain[i], &st.ring[i], st.max_write, c->cfg.rmgr_drain_bytes) !=
		        0) {
			RTC_ERROR(C, "cannot allocate the %s buffers", c->cfg.rmgr_path);
			return -1; /* memory, not permission: the configuration cannot work */
		}
		memset(st.scratch[i], 0, st.max_write + 1); /* touch the pages now */
		st.scratch[i][0] = RTC_REC_LP;
	}
	return 0;
}

static void unavailable(rtc_collector *c, const char *why)
{
	RTC_WARN(C, "%s is not available: %s; the collector runs without it", c->cfg.rmgr_path, why);
	rtc_counter_set(&c->counters.rmgr_unavailable, 1);
}

/* Unregisters and destroys the channel: writers blocked on it fail now. */
static void teardown(void)
{
	if (st.id != -1 && st.dpp && !atomic_load(&st.dead))
		resmgr_detach(st.dpp, st.id, _RESMGR_DETACH_ALL);
	st.id = -1;
	if (st.lock)
		name_detach(st.lock, 0);
	st.lock = NULL;
	if (st.dpp)
		dispatch_destroy(st.dpp);
	st.dpp = NULL;
}

static void stop(rtc_collector *c)
{
	if (!st.running)
		return;
	/* Wake each dispatch thread with our own pulse, then destroy the
	 * channel. If a pulse cannot be sent, destroying the channel first makes
	 * dispatch_block fail, which ends the threads too. */
	st.stopping = true;
	bool pulsed = !atomic_load(&st.dead);
	for (unsigned i = 0; pulsed && i < st.started; i++)
		if (MsgSendPulse(st.coid, -1, st.pulse_code, 0) == -1) {
			RTC_WARN(C, "cannot wake the %s threads (%s); destroying the channel",
			         c->cfg.rmgr_path, strerror(errno));
			pulsed = false;
		}
	if (!pulsed)
		teardown();
	for (unsigned i = 0; i < st.started; i++)
		pthread_join(st.thread[i], NULL);
	teardown();
	st.started = 0;
	st.running = false;
}

static void start(rtc_collector *c)
{
	/* Fixed priority: handlers run at rmgr_priority, not the writer's. */
	st.chid = ChannelCreate(_NTO_CHF_DISCONNECT | _NTO_CHF_UNBLOCK | _NTO_CHF_FIXED_PRIORITY);
	st.dpp = st.chid == -1 ? NULL : dispatch_create_channel(st.chid, 0);
	if (!st.dpp) {
		unavailable(c, strerror(errno));
		return;
	}
	/* Exclusive claim between collectors, atomic: name_attach fails with
	 * EEXIST if another collector holds the same name. The name vanishes with
	 * the process, so a crash leaves no stale lock. The path is escaped
	 * unambiguously ('_' -> "__", '/' -> "_s"), so distinct paths never share
	 * a name. Its own dispatch handle (NULL): name_attach's error and detach
	 * paths must not touch the device's handle (they corrupted it in
	 * testing). */
	char name[2 * sizeof(c->cfg.rmgr_path) + 16];
	size_t n = (size_t)snprintf(name, sizeof(name), "rtcollector");
	for (const char *q = c->cfg.rmgr_path; *q && n + 3 < sizeof(name); q++) {
		if (*q == '_' || *q == '/') {
			name[n++] = '_';
			name[n++] = *q == '_' ? '_' : 's';
		} else {
			name[n++] = *q;
		}
	}
	name[n] = '\0';
	st.lock = name_attach(NULL, name, 0);
	if (!st.lock) {
		unavailable(c, errno == EEXIST ? "another rtcollector already serves this path"
		                               : strerror(errno));
		teardown();
		return;
	}
	/* Something else (not a collector) registered the path. */
	struct stat sb;
	if (stat(c->cfg.rmgr_path, &sb) == 0) {
		unavailable(c, "the path already exists (another program?)");
		teardown();
		return;
	}
	iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &st.connect_funcs, _RESMGR_IO_NFUNCS, &st.io_funcs);
	st.io_funcs.write = io_write;
	st.io_funcs.write64 = io_write;
	st.io_funcs.read = io_read;
	iofunc_attr_init(&st.attr, S_IFCHR | (c->cfg.rmgr_mode & 0777), NULL, NULL);
	st.attr.gid = (gid_t)c->cfg.rmgr_group;
	/* Receive buffer for a whole write: header, pwrite offset, max_write. */
	resmgr_attr_t ra;
	memset(&ra, 0, sizeof(ra));
	ra.nparts_max = 1;
	ra.msg_max_size = sizeof(io_write_t) + sizeof(struct _xtype_offset) + st.max_write;
	st.id = resmgr_attach(st.dpp, &ra, c->cfg.rmgr_path, _FTYPE_ANY, 0, &st.connect_funcs, &st.io_funcs,
	                      &st.attr);
	if (st.id == -1) {
		int e = errno;
		unavailable(c, e == EPERM ? "registering a path needs root or PROCMGR_AID_PATHSPACE"
		                          : strerror(e));
		teardown();
		return;
	}
	st.pulse_code = pulse_attach(st.dpp, MSG_FLAG_ALLOC_PULSE, 0, on_pulse, NULL);
	st.coid = message_connect(st.dpp, MSG_FLAG_SIDE_CHANNEL);
	if (st.pulse_code == -1 || st.coid == -1) {
		unavailable(c, "cannot set up its channel");
		teardown();
		return;
	}
	for (unsigned i = 0; i < st.n; i++) {
		if (rtc_start_thread(c, &st.thread[i], dispatch_main, (void *)(uintptr_t)i,
		                     c->cfg.rmgr_priority, "rmgr") != 0)
			break;
		st.started++;
	}
	st.running = true;
	if (st.started < st.n) {
		unavailable(c, "cannot start its threads");
		stop(c);
		return;
	}
	c->lp_drains = st.drain;
	atomic_store(&c->lp_ndrains, st.n); /* the collection thread converts from now on */
	RTC_INFO(C, "%s ready (mode %03o, group %d, %u threads at priority %d, max %zu bytes per write)",
	         c->cfg.rmgr_path, c->cfg.rmgr_mode & 0777, c->cfg.rmgr_group, st.n, c->cfg.rmgr_priority,
	         st.max_write);
}

static void destroy(rtc_collector *c)
{
	(void)c;
	teardown();
	for (unsigned i = 0; i < st.n; i++) {
		rtc_lp_drain_free(&st.drain[i]);
		rtc_ring_free(&st.ring[i]);
		free(st.scratch[i]);
		st.scratch[i] = NULL;
	}
}

/* rmgr has no per-round work: the collection thread drains its rings. */
static int gather(rtc_collector *c)
{
	(void)c;
	return 0;
}

const rtc_input_def rtc_input_rmgr = {
    .name = "rmgr", .init = init, .gather = gather, .start = start, .stop = stop, .destroy = destroy};
