/* The stdout output's lines: tag precedence and escaping. */
#include "t.h"

#include "../src/core/collector.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NREC 8

typedef struct {
	uint8_t store[NREC][512];
	uint32_t len[NREC];
	size_t n;
} recs;

static rtc_rec_builder *begin(recs *b, const char *m)
{
	static rtc_rec_builder rb;
	rtc_rb_init(&rb, b->store[b->n], sizeof(b->store[0]), m);
	return &rb;
}

static void end(recs *b, rtc_rec_builder *rb)
{
	b->len[b->n] = (uint32_t)rtc_rb_finish(rb);
	CHECK(b->len[b->n] > 0);
	b->n++;
}

/* Runs the stdout output over the records and returns what it printed. */
static const char *print(const rtc_config *cfg, recs *b)
{
	static char out[8192];
	out[0] = '\0';
	rtc_collector *c = calloc(1, sizeof(*c));
	CHECK(c != NULL);
	if (!c)
		return out;
	c->cfg = *cfg;
	rtc_output *o;
	CHECK(rtc_output_stdout_create(c, 0, &o) == 0);
	for (size_t i = 0; i < b->n; i++) {
		o->recs[i] = (rtc_ring_rec){i, 1000 + i, b->len[i], b->store[i]};
		o->done[i] = 0;
	}
	o->nrecs = b->n;

	int fds[2];
	CHECK(pipe(fds) == 0);
	fflush(stdout);
	int saved = dup(STDOUT_FILENO);
	dup2(fds[1], STDOUT_FILENO);
	enum rtc_send_outcome r = o->send(o, 0);
	dup2(saved, STDOUT_FILENO);
	close(saved);
	close(fds[1]);
	size_t got = 0;
	ssize_t n;
	while ((n = read(fds[0], out + got, sizeof(out) - 1 - got)) > 0)
		got += (size_t)n;
	out[got] = '\0';
	close(fds[0]);
	CHECK(r == RTC_SEND_DONE);
	CHECK_EQ_U(rtc_counter_get(&o->counters.records_sent), b->n);
	o->destroy(o);
	free(c);
	return out;
}

static void cfg_init(rtc_config *cfg)
{
	rtc_config_defaults(cfg);
	snprintf(cfg->hostname, sizeof(cfg->hostname), "pi5");
	cfg->max_record_bytes = 512;
	cfg->out_stdout.batch_bytes = 4096;
}

static void tag_precedence(void)
{
	rtc_config cfg;
	cfg_init(&cfg);
	cfg.ntags = 1;
	snprintf(cfg.tags[0].key, sizeof(cfg.tags[0].key), "site");
	snprintf(cfg.tags[0].val, sizeof(cfg.tags[0].val), "lab");
	recs b = {0};
	rtc_rec_builder *r = begin(&b, "app"); /* record host beats the hostname; config tags win */
	rtc_rb_tag(r, "site", "elsewhere");
	rtc_rb_tag(r, "host", "robot-7");
	rtc_rb_tag(r, "core", "0");
	rtc_rb_i64(r, "x", 1);
	end(&b, r);
	r = begin(&b, "app"); /* no host tag: the hostname */
	rtc_rb_f64(r, "y", 1.5);
	end(&b, r);
	const char *out = print(&cfg, &b);
	CHECK(!strcmp(out, "app,host=robot-7,site=lab,core=0 x=1i 1000\n"
	                   "app,host=pi5,site=lab y=1.5 1001\n"));
	if (strcmp(out, "app,host=robot-7,site=lab,core=0 x=1i 1000\napp,host=pi5,site=lab y=1.5 1001\n"))
		fprintf(stderr, "     got: %s", out);
}

static void configured_host_beats_record_host(void)
{
	rtc_config cfg;
	cfg_init(&cfg);
	cfg.ntags = 1;
	snprintf(cfg.tags[0].key, sizeof(cfg.tags[0].key), "host");
	snprintf(cfg.tags[0].val, sizeof(cfg.tags[0].val), "cell-3");
	recs b = {0};
	rtc_rec_builder *r = begin(&b, "app");
	rtc_rb_tag(r, "host", "robot-7");
	rtc_rb_bool(r, "ok", true);
	end(&b, r);
	r = begin(&b, "app");
	rtc_rb_str(r, "s", "a \"q\"");
	end(&b, r);
	const char *out = print(&cfg, &b);
	CHECK(!strcmp(out, "app,host=cell-3 ok=true 1000\napp,host=cell-3 s=\"a \\\"q\\\"\" 1001\n"));
	if (strcmp(out, "app,host=cell-3 ok=true 1000\napp,host=cell-3 s=\"a \\\"q\\\"\" 1001\n"))
		fprintf(stderr, "     got: %s", out);
}

/* Two host tags in one record: the first counts, as in the Arc output. */
static void first_record_host_counts(void)
{
	rtc_config cfg;
	cfg_init(&cfg);
	recs b = {0};
	rtc_rec_builder *r = begin(&b, "app");
	rtc_rb_tag(r, "host", "a");
	rtc_rb_tag(r, "host", "b");
	rtc_rb_i64(r, "x", 1);
	end(&b, r);
	const char *out = print(&cfg, &b);
	CHECK(!strcmp(out, "app,host=a x=1i 1000\n"));
	if (strcmp(out, "app,host=a x=1i 1000\n"))
		fprintf(stderr, "     got: %s", out);
}

const t_case stdout_cases[] = {
    {"stdout", "first_record_host_counts", first_record_host_counts},
    {"stdout", "tag_precedence", tag_precedence},
    {"stdout", "configured_host_beats_record_host", configured_host_beats_record_host},
    {0},
};
