/* rmgr: /dev/rtcollector, a QNX resource manager programs write line
 * protocol to, one write() per line (or several whole lines):
 *
 *   char line[256];
 *   int n = snprintf(line, sizeof line, "loop,axis=x jitter_us=%di\n", j);
 *   write(fd, line, n);
 *
 * The write handler is the hot path: a real-time thread waits on it. It
 * copies the bytes into a preallocated ring and replies. No allocation, no
 * logging, no locks; work bounded by the size of the write. Parsing happens
 * later on the collection thread (lpdrain.c).
 *
 * One dispatch thread, a bare dispatch_block/dispatch_handler loop, so the
 * ring has one producer. The channel keeps priority inheritance: while
 * handling a write the thread runs at the writer's priority.
 *
 * Contract for writers (README): a write waits for the handler in progress
 * plus one handler run per queued writer at equal or higher priority, while
 * the collector runs. If the collector is stopped, a write blocks; after it
 * exits, writes fail with EBADF (reopen). Writes over max_write_bytes fail
 * with EMSGSIZE. Data lost because the buffer is full is counted, never
 * reported to the writer.
 */
/* Before the QNX headers: the OCB type the handlers receive. */
struct _iofunc_ocb;
#define RESMGR_OCB_T struct _iofunc_ocb
#define IOFUNC_OCB_T struct _iofunc_ocb

#include "../core/collector.h"
#include "../core/log.h"
#include "../core/lpdrain.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/stat.h>
#include <time.h>

#define C "rmgr"

/* Singleton state (one collector per process, see cpu.c). The handler
 * fields are touched only by the dispatch thread. */
static struct {
	rtc_collector *c;
	rtc_ring ring;
	rtc_lp_drain drain;
	uint8_t *scratch; /* one write, kind byte first */
	size_t max_write;
	dispatch_t *dpp;
	int id;
	int pulse_code;
	int coid;
	pthread_t thread;
	bool running;
	volatile bool stopping;
	resmgr_connect_funcs_t connect_funcs;
	resmgr_io_funcs_t io_funcs;
	iofunc_attr_t attr;
	char status[512];
} st;

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
	rtc_rmgr_counters *k = &st.c->rmgr_counters;
	if (nbytes > st.max_write) {
		rtc_counter_add(&k->oversize, 1);
		return EMSGSIZE;
	}
	if (ctp->info.priority > 0 && (uint64_t)ctp->info.priority > rtc_counter_get(&k->max_prio_seen))
		rtc_counter_set(&k->max_prio_seen, (uint64_t)ctp->info.priority);
	if (nbytes > 0) {
		if (resmgr_msgread(ctp, st.scratch + 1, nbytes, off) != (ssize_t)nbytes)
			return EFAULT;
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		uint64_t now = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
		if (rtc_ring_push(&st.ring, st.scratch, (uint32_t)nbytes + 1, now) != RTC_PUSH_OK)
			rtc_counter_add(&k->dropped, 1);
		rtc_counter_add(&k->writes, 1);
		rtc_counter_add(&k->bytes, nbytes);
	}
	_IO_SET_WRITE_NBYTES(ctp, nbytes);
	return _RESMGR_NPARTS(0);
}

/* `cat /dev/rtcollector`: one status line, then EOF. */
static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
	int rc = iofunc_read_verify(ctp, msg, ocb, NULL);
	if (rc != EOK)
		return rc;
	if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
		return ENOSYS;
	const rtc_rmgr_counters *k = &st.c->rmgr_counters;
	const rtc_collect_counters *q = &st.c->counters;
	int len = snprintf(
	    st.status, sizeof(st.status),
	    "rtcollector writes=%llu bytes=%llu dropped=%llu oversize=%llu lines=%llu "
	    "malformed=%llu bad_timestamp=%llu max_prio_seen=%llu\n",
	    (unsigned long long)rtc_counter_get(&k->writes), (unsigned long long)rtc_counter_get(&k->bytes),
	    (unsigned long long)(rtc_counter_get(&k->dropped) + rtc_counter_get(&q->rmgr_drain_dropped)),
	    (unsigned long long)rtc_counter_get(&k->oversize),
	    (unsigned long long)rtc_counter_get(&q->lp_lines),
	    (unsigned long long)rtc_counter_get(&q->lp_malformed),
	    (unsigned long long)rtc_counter_get(&q->lp_bad_timestamp),
	    (unsigned long long)rtc_counter_get(&k->max_prio_seen));
	if (len < 0)
		len = 0;
	if ((size_t)len >= sizeof(st.status))
		len = sizeof(st.status) - 1;
	off_t pos = ocb->offset;
	size_t n = 0;
	if (pos < len) {
		n = (size_t)(len - pos);
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
	(void)arg;
	dispatch_context_t *ctp = dispatch_context_alloc(st.dpp);
	if (!ctp)
		return NULL;
	while (!st.stopping) {
		dispatch_context_t *got = dispatch_block(ctp);
		if (!got) {
			if (errno == EINTR)
				continue;
			break;
		}
		ctp = got;
		dispatch_handler(ctp);
	}
	dispatch_context_free(ctp);
	return NULL;
}

static int init(rtc_collector *c)
{
	memset(&st, 0, sizeof(st));
	st.c = c;
	st.id = -1;
	st.coid = -1;
	st.max_write = c->cfg.max_write_bytes;
	st.scratch = malloc(st.max_write + 1);
	if (!st.scratch ||
	    rtc_ring_init(&st.ring, c->cfg.rmgr_buffer_bytes, (uint32_t)st.max_write + 1, 1) != 0 ||
	    rtc_lp_drain_init(&st.drain, &st.ring, st.max_write, c->cfg.rmgr_drain_bytes) != 0) {
		RTC_ERROR(C, "cannot allocate the %s buffers", c->cfg.rmgr_path);
		return -1; /* memory, not permission: the configuration cannot work */
	}
	st.scratch[0] = RTC_REC_LP;
	return 0;
}

static void unavailable(rtc_collector *c, const char *why)
{
	RTC_WARN(C, "%s is not available: %s; the collector runs without it", c->cfg.rmgr_path, why);
	rtc_counter_set(&c->counters.rmgr_unavailable, 1);
}

static void start(rtc_collector *c)
{
	struct stat sb;
	if (stat(c->cfg.rmgr_path, &sb) == 0) {
		/* A second attach would silently shadow the first: refuse. */
		unavailable(c, "the path is already registered (another collector?)");
		return;
	}
	st.dpp = dispatch_create();
	if (!st.dpp) {
		unavailable(c, strerror(errno));
		return;
	}
	iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &st.connect_funcs, _RESMGR_IO_NFUNCS, &st.io_funcs);
	st.io_funcs.write = io_write;
	st.io_funcs.write64 = io_write;
	st.io_funcs.read = io_read;
	iofunc_attr_init(&st.attr, S_IFCHR | (c->cfg.rmgr_mode & 0777), NULL, NULL);
	st.attr.gid = (gid_t)c->cfg.rmgr_group;
	st.id = resmgr_attach(st.dpp, NULL, c->cfg.rmgr_path, _FTYPE_ANY, 0, &st.connect_funcs, &st.io_funcs,
	                      &st.attr);
	if (st.id == -1) {
		int e = errno;
		unavailable(c, e == EPERM ? "registering a path needs root or PROCMGR_AID_PATHSPACE"
		                          : strerror(e));
		dispatch_destroy(st.dpp);
		st.dpp = NULL;
		return;
	}
	st.pulse_code = pulse_attach(st.dpp, MSG_FLAG_ALLOC_PULSE, 0, on_pulse, NULL);
	st.coid = message_connect(st.dpp, MSG_FLAG_SIDE_CHANNEL);
	if (st.pulse_code == -1 || st.coid == -1 ||
	    rtc_start_thread(c, &st.thread, dispatch_main, NULL, c->cfg.collect_priority, "rmgr") != 0) {
		unavailable(c, "cannot start its thread");
		resmgr_detach(st.dpp, st.id, _RESMGR_DETACH_ALL);
		dispatch_destroy(st.dpp);
		st.dpp = NULL;
		return;
	}
	st.running = true;
	c->lp_drain = &st.drain; /* the collection thread converts from now on */
	RTC_INFO(C, "%s ready (mode %03o, group %d, max %zu bytes per write)", c->cfg.rmgr_path,
	         c->cfg.rmgr_mode & 0777, c->cfg.rmgr_group, st.max_write);
}

static void stop(rtc_collector *c)
{
	(void)c;
	if (!st.running)
		return;
	/* Wake the dispatch thread with our own pulse, then unregister. Writers
	 * still holding the path get EBADF from here on. */
	MsgSendPulse(st.coid, -1, st.pulse_code, 0);
	pthread_join(st.thread, NULL);
	resmgr_detach(st.dpp, st.id, _RESMGR_DETACH_ALL);
	st.running = false;
}

static void destroy(rtc_collector *c)
{
	(void)c;
	if (st.dpp)
		dispatch_destroy(st.dpp);
	st.dpp = NULL;
	rtc_lp_drain_free(&st.drain);
	rtc_ring_free(&st.ring);
	free(st.scratch);
	st.scratch = NULL;
}

/* rmgr has no per-round work: the collection thread drains its ring. */
static int gather(rtc_collector *c)
{
	(void)c;
	return 0;
}

const rtc_input_def rtc_input_rmgr = {
    .name = "rmgr", .init = init, .gather = gather, .start = start, .stop = stop, .destroy = destroy};
