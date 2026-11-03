/* stdout: one line per record in line-protocol form, for debugging and for
 * --once smoke tests. Tags from the configuration are added the same way the
 * Arc output adds them. */
#include "../core/collector.h"
#include "../core/log.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void esc(FILE *f, const char *s, size_t n, const char *special)
{
	for (size_t i = 0; i < n; i++) {
		if (strchr(special, s[i]))
			fputc('\\', f);
		fputc(s[i], f);
	}
}

static bool print_rec(rtc_collector *c, const rtc_ring_rec *r, FILE *f)
{
	rtc_rec_view v;
	if (!rtc_rec_decode(r->payload, r->len, &v))
		return false;
	esc(f, v.measurement.p, v.measurement.len, ", ");
	const char *host = c->cfg.hostname;
	size_t hl = strlen(host);
	for (unsigned i = 0; i < v.ntags; i++)
		if (v.tag_k[i].len == 4 && !memcmp(v.tag_k[i].p, "host", 4)) {
			host = v.tag_v[i].p;
			hl = v.tag_v[i].len;
		}
	for (unsigned i = 0; i < c->cfg.ntags; i++)
		if (!strcmp(c->cfg.tags[i].key, "host")) {
			host = c->cfg.tags[i].val;
			hl = strlen(host);
		}
	fputs(",host=", f);
	esc(f, host, hl, ",= ");
	for (unsigned i = 0; i < c->cfg.ntags; i++) {
		if (!strcmp(c->cfg.tags[i].key, "host"))
			continue;
		fputc(',', f);
		esc(f, c->cfg.tags[i].key, strlen(c->cfg.tags[i].key), ",= ");
		fputc('=', f);
		esc(f, c->cfg.tags[i].val, strlen(c->cfg.tags[i].val), ",= ");
	}
	for (unsigned i = 0; i < v.ntags; i++) {
		bool skip = v.tag_k[i].len == 4 && !memcmp(v.tag_k[i].p, "host", 4);
		for (unsigned j = 0; j < c->cfg.ntags && !skip; j++)
			skip = strlen(c->cfg.tags[j].key) == v.tag_k[i].len &&
			       !memcmp(c->cfg.tags[j].key, v.tag_k[i].p, v.tag_k[i].len);
		if (skip)
			continue;
		fputc(',', f);
		esc(f, v.tag_k[i].p, v.tag_k[i].len, ",= ");
		fputc('=', f);
		esc(f, v.tag_v[i].p, v.tag_v[i].len, ",= ");
	}
	for (unsigned i = 0; i < v.nfields; i++) {
		const rtc_field *fl = &v.fields[i];
		fputc(i ? ',' : ' ', f);
		esc(f, fl->key.p, fl->key.len, ",= ");
		fputc('=', f);
		switch (fl->type) {
		case RTC_F_I64:
			fprintf(f, "%" PRId64 "i", fl->v.i);
			break;
		case RTC_F_F64:
			fprintf(f, "%.17g", fl->v.f);
			break;
		case RTC_F_BOOL:
			fputs(fl->v.b ? "true" : "false", f);
			break;
		case RTC_F_STR:
			fputc('"', f);
			esc(f, fl->v.s.p, fl->v.s.len, "\"\\");
			fputc('"', f);
			break;
		}
	}
	fprintf(f, " %" PRIu64 "\n", r->ts_ns);
	return true;
}

static enum rtc_send_outcome send_batch(rtc_output *o, uint64_t deadline_ns)
{
	(void)deadline_ns;
	for (size_t i = 0; i < o->nrecs; i++) {
		if (o->done[i])
			continue;
		o->done[i] = 1;
		if (print_rec(o->collector, &o->recs[i], stdout))
			rtc_counter_add(&o->counters.records_sent, 1);
		else
			rtc_counter_add(&o->counters.records_rejected, 1);
	}
	fflush(stdout);
	rtc_counter_add(&o->counters.requests, 1);
	return RTC_SEND_DONE;
}

static void destroy(rtc_output *o)
{
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
	if (rtc_output_alloc_batch(o, c->cfg.out_stdout.batch_bytes, c->cfg.max_record_bytes) != 0) {
		destroy(o);
		return -1;
	}
	*out = o;
	return 0;
}
