/* stdout: one line per record in line-protocol form, for debugging and for
 * --once smoke tests. Tags from the configuration are added the same way the
 * Arc output adds them (row.h). Lines are not validated: stdout prints what
 * it can decode. */
#include "../core/collector.h"
#include "../core/log.h"
#include "row.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

/* One line, formatted in memory, then written with write(2) under the
 * shutdown deadline: a stdout nobody reads (a full pipe) cannot hold shutdown. */
typedef struct {
	char *p;
	size_t len, cap;
	bool overflow;
} line_buf;

static void ob_putc(line_buf *b, char ch)
{
	if (b->len + 1 >= b->cap) {
		b->overflow = true;
		return;
	}
	b->p[b->len++] = ch;
}

static void ob_puts(line_buf *b, const char *s)
{
	while (*s)
		ob_putc(b, *s++);
}

static void ob_printf(line_buf *b, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= b->cap - b->len) {
		b->overflow = true;
		return;
	}
	b->len += (size_t)n;
}

static void esc(line_buf *f, const char *s, size_t n, const char *special)
{
	for (size_t i = 0; i < n; i++) {
		if (strchr(special, s[i]))
			ob_putc(f, '\\');
		ob_putc(f, s[i]);
	}
}

static bool print_rec(rtc_collector *c, const rtc_ring_rec *r, line_buf *f)
{
	rtc_rec_view v;
	if (!rtc_rec_decode(r->payload, r->len, &v))
		return false;
	esc(f, v.measurement.p, v.measurement.len, ", ");
	rtc_row_inj in[RTC_CFG_MAX_TAGS + 1];
	unsigned nin = rtc_row_injected(&c->cfg, in);
	for (unsigned t = 0; t < nin; t++) {
		size_t vl;
		const char *val = rtc_row_inj_value(&in[t], &v, c->cfg.hostname, &vl);
		ob_putc(f, ',');
		esc(f, in[t].k, in[t].kl, ",= ");
		ob_putc(f, '=');
		esc(f, val, vl, ",= ");
	}
	for (unsigned i = 0; i < v.ntags; i++) {
		if (rtc_row_is_injected(in, nin, v.tag_k[i]))
			continue;
		ob_putc(f, ',');
		esc(f, v.tag_k[i].p, v.tag_k[i].len, ",= ");
		ob_putc(f, '=');
		esc(f, v.tag_v[i].p, v.tag_v[i].len, ",= ");
	}
	for (unsigned i = 0; i < v.nfields; i++) {
		const rtc_field *fl = &v.fields[i];
		ob_putc(f, i ? ',' : ' ');
		esc(f, fl->key.p, fl->key.len, ",= ");
		ob_putc(f, '=');
		switch (fl->type) {
		case RTC_F_I64:
			ob_printf(f, "%" PRId64 "i", fl->v.i);
			break;
		case RTC_F_F64:
			ob_printf(f, "%.17g", fl->v.f);
			break;
		case RTC_F_BOOL:
			ob_puts(f, fl->v.b ? "true" : "false");
			break;
		case RTC_F_STR:
			ob_putc(f, '"');
			esc(f, fl->v.s.p, fl->v.s.len, "\"\\");
			ob_putc(f, '"');
			break;
		}
	}
	ob_printf(f, " %" PRIu64 "\n", r->ts_ns);
	return true;
}

/* Writes all of p, never past abort_after_ns (when set). Returns false when
 * cut off or on an error; chunks of PIPE_BUF after select says writable do not block. */
static bool write_all(rtc_collector *c, int fd, const char *p, size_t n)
{
	if (fd < 0 || fd >= FD_SETSIZE)
		return false; /* no usable stdout; select() cannot take it */
	while (n) {
		uint64_t abort_after = atomic_load(&c->abort_after_ns);
		if (abort_after && rtc_mono_ns() >= abort_after)
			return false;
		/* select, not poll: macOS poll() answers POLLNVAL for devices
		 * (/dev/null, ttys). */
		fd_set ws;
		FD_ZERO(&ws);
		FD_SET(fd, &ws);
		struct timeval tv = {0, 50000};
		int r = select(fd + 1, NULL, &ws, NULL, &tv);
		if (r < 0 && errno != EINTR)
			return false;
		if (r <= 0)
			continue;
		ssize_t w = write(fd, p, n < PIPE_BUF ? n : PIPE_BUF);
		if (w < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return false;
		}
		p += w;
		n -= (size_t)w;
	}
	return true;
}

static enum rtc_send_outcome send_batch(rtc_output *o, uint64_t deadline_ns)
{
	(void)deadline_ns; /* abort_after_ns bounds every write, as for Arc */
	line_buf *b = o->impl;
	int fd = fileno(stdout);
	rtc_counter_add(&o->counters.requests, 1);
	for (size_t i = 0; i < o->nrecs; i++) {
		if (o->done[i])
			continue;
		b->len = 0;
		b->overflow = false;
		if (!print_rec(o->collector, &o->recs[i], b) || b->overflow) {
			o->done[i] = 1;
			rtc_counter_add(&o->counters.records_rejected, 1);
			continue;
		}
		if (!write_all(o->collector, fd, b->p, b->len)) {
			rtc_counter_add(&o->counters.request_errors, 1);
			return RTC_SEND_RETRY; /* kept; counted as dropped if never written */
		}
		o->done[i] = 1;
		rtc_counter_add(&o->counters.records_sent, 1);
	}
	return RTC_SEND_DONE;
}

static void destroy(rtc_output *o)
{
	line_buf *b = o->impl;
	if (b)
		free(b->p);
	free(b);
	rtc_output_free_batch(o);
	free(o);
}

int rtc_output_stdout_create(rtc_collector *c, unsigned idx, rtc_output **out)
{
	rtc_output *o = calloc(1, sizeof(*o));
	if (!o)
		return -1;
	o->name = "stdout";
	o->idx = idx;
	o->collector = c;
	o->send = send_batch;
	o->destroy = destroy;
	line_buf *b = calloc(1, sizeof(*b));
	o->impl = b;
	/* Escaping at most doubles a record; the configuration tags and the
	 * timestamp come on top. */
	size_t cap = 2 * (size_t)c->cfg.max_record_bytes + RTC_CFG_MAX_TAGS * 2 * (128 + 256) + 64;
	if (b)
		b->p = malloc(cap);
	if (b && b->p)
		b->cap = cap;
	if (!b || !b->p ||
	    rtc_output_alloc_batch(o, c->cfg.out_stdout.batch_bytes, c->cfg.max_record_bytes) != 0) {
		destroy(o);
		return -1;
	}
	*out = o;
	return 0;
}
