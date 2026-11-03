#include "t.h"

#include "../src/outputs/arc_encode.h"
#include "mp_read.h"

#include <math.h>
#include <string.h>

#define MAXR 64

typedef struct {
	uint8_t store[MAXR][512];
	rtc_ring_rec recs[MAXR];
	uint8_t done[MAXR];
	uint8_t verdict[MAXR];
	uint8_t flags[MAXR];
	size_t n;
} batch;

static rtc_rec_builder *begin(batch *b, const char *m)
{
	static rtc_rec_builder rb;
	rtc_rb_init(&rb, b->store[b->n], sizeof(b->store[0]), m);
	return &rb;
}

static void end(batch *b, rtc_rec_builder *rb, uint64_t ts)
{
	size_t len = rtc_rb_finish(rb);
	CHECK(len > 0);
	b->recs[b->n] = (rtc_ring_rec){b->n, ts, (uint32_t)len, b->store[b->n]};
	b->n++;
}

static mp_node *encode(batch *b, const rtc_config *cfg, rtc_output_counters *k, size_t *len_out)
{
	static uint8_t out[1 << 16];
	rtc_arc_enc e;
	CHECK(rtc_arc_enc_init(&e, MAXR, 8) == 0);
	size_t len = rtc_arc_encode(&e, cfg, "pi5", b->recs, b->done, b->flags, 0, b->n, b->verdict, k, out,
	                            sizeof(out));
	rtc_arc_enc_free(&e);
	if (len_out)
		*len_out = len;
	if (len == 0 || len == SIZE_MAX)
		return NULL;
	return mp_parse(out, len);
}

static mp_node *item_for(mp_node *root, const char *m)
{
	for (uint32_t i = 0; root && i < root->n; i++)
		if (mp_str_is(mp_get(root->kids[i], "m"), m))
			return root->kids[i];
	return NULL;
}

static void one_item_per_measurement_with_null_fill(void)
{
	batch b = {0};
	rtc_config cfg;
	rtc_config_defaults(&cfg);
	rtc_output_counters k = {0};

	rtc_rec_builder *r = begin(&b, "cpu");
	rtc_rb_tag(r, "core", "0");
	rtc_rb_f64(r, "user", 3.0);
	end(&b, r, 1793734227000000001ull);
	r = begin(&b, "mem");
	rtc_rb_i64(r, "free", 100);
	end(&b, r, 1793734227000000002ull);
	r = begin(&b, "cpu");
	rtc_rb_tag(r, "core", "1");
	rtc_rb_f64(r, "user", 1.5);
	rtc_rb_f64(r, "system", 0.5);
	end(&b, r, 1793734227000000003ull);

	mp_node *root = encode(&b, &cfg, &k, NULL);
	CHECK(root && root->t == MP_ARR && root->n == 2);
	mp_node *cpu = item_for(root, "cpu");
	mp_node *cols = mp_get(cpu, "columns");
	CHECK(cols && cols->n == 5); /* time, host, core, user, system */
	mp_node *time = mp_get(cols, "time");
	CHECK(time && time->n == 2 && time->kids[0]->t == MP_INT &&
	      time->kids[1]->i == 1793734227000000003ll);
	mp_node *user = mp_get(cols, "user");
	CHECK(user && user->kids[0]->t == MP_F64 && user->kids[0]->f == 3.0); /* stays float */
	mp_node *sys = mp_get(cols, "system");
	CHECK(sys && sys->kids[0]->t == MP_NIL && sys->kids[1]->t == MP_F64);
	mp_node *host = mp_get(cols, "host");
	CHECK(host && mp_str_is(host->kids[0], "pi5"));
	mp_node *tk = mp_get(cpu, "tag_keys");
	CHECK(tk && tk->n == 2 && mp_str_is(tk->kids[0], "host") && mp_str_is(tk->kids[1], "core"));
	CHECK(item_for(root, "mem") != NULL);
	for (size_t i = 0; i < b.n; i++)
		CHECK_EQ_U(b.verdict[i], RTC_V_SENT);
	mp_free_all();
}

static void configured_tags_win_and_record_host_beats_hostname(void)
{
	batch b = {0};
	rtc_config cfg;
	rtc_config_defaults(&cfg);
	cfg.ntags = 1;
	strcpy(cfg.tags[0].key, "site");
	strcpy(cfg.tags[0].val, "lab");
	rtc_output_counters k = {0};

	rtc_rec_builder *r = begin(&b, "app");
	rtc_rb_tag(r, "site", "elsewhere");
	rtc_rb_tag(r, "host", "robot-7");
	rtc_rb_i64(r, "x", 1);
	end(&b, r, 1793734227000000000ull);

	mp_node *root = encode(&b, &cfg, &k, NULL);
	mp_node *cols = mp_get(item_for(root, "app"), "columns");
	CHECK(mp_str_is(mp_get(cols, "site")->kids[0], "lab"));
	CHECK(mp_str_is(mp_get(cols, "host")->kids[0], "robot-7"));
	CHECK_EQ_U(cols->n, 4); /* time host site x */
	mp_free_all();
}

static void invalid_rows_are_rejected_not_sent(void)
{
	batch b = {0};
	rtc_config cfg;
	rtc_config_defaults(&cfg);
	rtc_output_counters k = {0};
	rtc_rec_builder *r;

	r = begin(&b, "bad name"); /* 0: measurement */
	rtc_rb_i64(r, "x", 1);
	end(&b, r, 1);
	r = begin(&b, "m"); /* 1: field named time */
	rtc_rb_i64(r, "Time", 1);
	end(&b, r, 1);
	r = begin(&b, "m"); /* 2: good, sets x as int */
	rtc_rb_i64(r, "x", 1);
	end(&b, r, 1);
	r = begin(&b, "m"); /* 3: x as float: type conflict */
	rtc_rb_f64(r, "x", 1.0);
	end(&b, r, 1);
	r = begin(&b, "m"); /* 4: X vs x: case twin */
	rtc_rb_i64(r, "X", 1);
	end(&b, r, 1);
	r = begin(&b, "m"); /* 5: only a NaN field */
	rtc_rb_f64(r, "y", NAN);
	end(&b, r, 1);
	r = begin(&b, "m"); /* 6: NaN dropped, x kept */
	rtc_rb_i64(r, "x", 2);
	rtc_rb_f64(r, "z", INFINITY);
	end(&b, r, 1);
	r = begin(&b, "m"); /* 7: tag and field with one name */
	rtc_rb_tag(r, "x", "t");
	rtc_rb_i64(r, "w", 2);
	end(&b, r, 1);
	r = begin(&b, "m"); /* 8: a field named like the injected host tag */
	rtc_rb_i64(r, "host", 2);
	end(&b, r, 1);
	r = begin(&b, "m"); /* 9: invalid UTF-8 tag key */
	rtc_rb_tag(r, "\xff\xfe", "v");
	rtc_rb_i64(r, "x", 3);
	end(&b, r, 1);

	mp_node *root = encode(&b, &cfg, &k, NULL);
	uint8_t want[] = {RTC_V_REJECTED, RTC_V_REJECTED, RTC_V_SENT,     RTC_V_REJECTED, RTC_V_REJECTED,
	                  RTC_V_REJECTED, RTC_V_SENT,     RTC_V_REJECTED, RTC_V_REJECTED, RTC_V_REJECTED};
	for (size_t i = 0; i < b.n; i++) {
		if (b.verdict[i] != want[i])
			fprintf(stderr, "     row %zu: verdict %u, want %u\n", i, b.verdict[i], want[i]);
		CHECK_EQ_U(b.verdict[i], want[i]);
	}
	mp_node *cols = mp_get(item_for(root, "m"), "columns");
	CHECK(cols && cols->n == 3); /* time host x: no all-nil z or y column */
	CHECK(mp_get(cols, "z") == NULL && mp_get(cols, "y") == NULL);
	CHECK_EQ_U(rtc_counter_get(&k.fields_nonfinite), 2);
	CHECK_EQ_U(rtc_counter_get(&k.type_conflicts), 2); /* float x, tag x */
	mp_free_all();
}

static void nothing_to_send_and_overflow(void)
{
	batch b = {0};
	rtc_config cfg;
	rtc_config_defaults(&cfg);
	rtc_output_counters k = {0};
	rtc_rec_builder *r = begin(&b, "m");
	rtc_rb_i64(r, "x", 1);
	end(&b, r, 1);
	b.done[0] = 1;
	size_t len;
	encode(&b, &cfg, &k, &len);
	CHECK_EQ_U(len, 0);

	b.done[0] = 0;
	uint8_t tiny[8];
	rtc_arc_enc e;
	rtc_arc_enc_init(&e, MAXR, 8);
	len = rtc_arc_encode(&e, &cfg, "pi5", b.recs, b.done, b.flags, 0, b.n, b.verdict, &k, tiny,
	                     sizeof(tiny));
	CHECK(len == SIZE_MAX);
	rtc_arc_enc_free(&e);
}

static void utf8(void)
{
	CHECK(rtc_utf8_valid("abc", 3));
	CHECK(rtc_utf8_valid("\xc3\xa9", 2));
	CHECK(rtc_utf8_valid("\xf0\x9f\x98\x80", 4));
	CHECK(!rtc_utf8_valid("\xc3", 1));
	CHECK(!rtc_utf8_valid("\xc0\xaf", 2));     /* overlong */
	CHECK(!rtc_utf8_valid("\xed\xa0\x80", 3)); /* surrogate */
}

const t_case arc_encode_cases[] = {
    {"arc_encode", "one_item_per_measurement_with_null_fill", one_item_per_measurement_with_null_fill},
    {"arc_encode", "configured_tags_win_and_record_host_beats_hostname",
     configured_tags_win_and_record_host_beats_hostname},
    {"arc_encode", "invalid_rows_are_rejected_not_sent", invalid_rows_are_rejected_not_sent},
    {"arc_encode", "nothing_to_send_and_overflow", nothing_to_send_and_overflow},
    {"arc_encode", "utf8", utf8},
    {0},
};
