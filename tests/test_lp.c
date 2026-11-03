#include "t.h"

#include "../src/core/lp.h"

#include <string.h>

static const rtc_lp_limits LIM = {1700000000000000000ull, 1900000000000000000ull};

static enum rtc_lp_result parse(const char *s, rtc_rec_view *v, uint64_t *ts, unsigned *ud)
{
	static uint8_t buf[4096];
	size_t len = 0;
	unsigned d = 0;
	uint64_t t = 0;
	enum rtc_lp_result r = rtc_lp_parse(s, strlen(s), &LIM, buf, sizeof(buf), &len, &t, &d);
	if (ts)
		*ts = t;
	if (ud)
		*ud = d;
	if (r == RTC_LP_OK && v)
		CHECK(rtc_rec_decode(buf, len, v));
	return r;
}

static bool streq(rtc_str s, const char *c)
{
	return s.len == strlen(c) && !memcmp(s.p, c, s.len);
}

static void types_and_timestamp(void)
{
	rtc_rec_view v;
	uint64_t ts;
	CHECK(parse("loop,axis=x f=1.5,i=-42i,u=7u,b=true,s=\"hi \\\"there\\\"\" 1800000000000000000", &v,
	            &ts, NULL) == RTC_LP_OK);
	CHECK(streq(v.measurement, "loop"));
	CHECK(v.ntags == 1 && streq(v.tag_k[0], "axis") && streq(v.tag_v[0], "x"));
	CHECK_EQ_U(v.nfields, 5);
	CHECK(v.fields[0].type == RTC_F_F64 && v.fields[0].v.f == 1.5);
	CHECK(v.fields[1].type == RTC_F_I64 && v.fields[1].v.i == -42);
	CHECK(v.fields[2].type == RTC_F_I64 && v.fields[2].v.i == 7);
	CHECK(v.fields[3].type == RTC_F_BOOL && v.fields[3].v.b);
	CHECK(v.fields[4].type == RTC_F_STR && streq(v.fields[4].v.s, "hi \"there\""));
	CHECK_EQ_U(ts, 1800000000000000000ull);
	CHECK(parse("m v=1", &v, &ts, NULL) == RTC_LP_OK && ts == 0);
	/* 3.0 stays a float; 3 too (no i suffix). */
	CHECK(parse("m v=3", &v, NULL, NULL) == RTC_LP_OK && v.fields[0].type == RTC_F_F64);
}

static void escapes(void)
{
	rtc_rec_view v;
	CHECK(parse("my\\ m\\,x,t\\=k=a\\ b\\,c f\\ k=1i", &v, NULL, NULL) == RTC_LP_OK);
	CHECK(streq(v.measurement, "my m,x"));
	CHECK(streq(v.tag_k[0], "t=k") && streq(v.tag_v[0], "a b,c"));
	CHECK(streq(v.fields[0].key, "f k"));
	CHECK(parse("m s=\"a,b c=d\"", &v, NULL, NULL) == RTC_LP_OK && streq(v.fields[0].v.s, "a,b c=d"));
}

static void bools(void)
{
	const char *t[] = {"t", "T", "true", "True", "TRUE"}, *f[] = {"f", "F", "false", "False", "FALSE"};
	char line[64];
	rtc_rec_view v;
	for (int i = 0; i < 5; i++) {
		snprintf(line, sizeof(line), "m b=%s", t[i]);
		CHECK(parse(line, &v, NULL, NULL) == RTC_LP_OK && v.fields[0].type == RTC_F_BOOL &&
		      v.fields[0].v.b);
		snprintf(line, sizeof(line), "m b=%s", f[i]);
		CHECK(parse(line, &v, NULL, NULL) == RTC_LP_OK && v.fields[0].type == RTC_F_BOOL &&
		      !v.fields[0].v.b);
	}
	CHECK(parse("m b=tRuE", NULL, NULL, NULL) == RTC_LP_MALFORMED);
}

static void refused(void)
{
	const char *bad[] = {
	    "m",                         /* no fields */
	    "m ",                        /* no fields */
	    ",t=1 v=1",                  /* no measurement */
	    "m,t v=1",                   /* tag without value */
	    "m,t= v=1",                  /* empty tag value */
	    "m v=",                      /* empty field value */
	    "m v=nan",                   /* not finite */
	    "m v=inf",                   /* not finite */
	    "m v=0x10",                  /* hex */
	    "m v=1e999",                 /* ERANGE */
	    "m v=99999999999999999999i", /* ERANGE */
	    "m v=1.5i",                  /* not an integer */
	    "m v=-1u",                   /* unsigned with a sign */
	    "m v=\"open",                /* unterminated string */
	    "m v=1 12x",                 /* timestamp not a number */
	    "m v=1 1 2",                 /* trailing garbage */
	    "m v=abc",                   /* not a value */
	    "m v=-",                     /* a sign, no digits */
	    "m v=.",                     /* a point, no digits */
	    "m v=e5",                    /* an exponent, no digits */
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		enum rtc_lp_result r = parse(bad[i], NULL, NULL, NULL);
		if (r != RTC_LP_MALFORMED)
			fprintf(stderr, "     accepted or misclassified: %s (%d)\n", bad[i], r);
		CHECK(r == RTC_LP_MALFORMED);
	}
}

static void timestamps(void)
{
	/* Seconds, milliseconds, microseconds: not nanoseconds, refused. */
	CHECK(parse("m v=1 1800000000", NULL, NULL, NULL) == RTC_LP_BAD_TIMESTAMP);
	CHECK(parse("m v=1 1800000000000", NULL, NULL, NULL) == RTC_LP_BAD_TIMESTAMP);
	CHECK(parse("m v=1 1800000000000000", NULL, NULL, NULL) == RTC_LP_BAD_TIMESTAMP);
	CHECK(parse("m v=1 -5", NULL, NULL, NULL) == RTC_LP_BAD_TIMESTAMP);
	CHECK(parse("m v=1 1950000000000000000", NULL, NULL, NULL) == RTC_LP_BAD_TIMESTAMP); /* ceiling */
	CHECK(parse("m v=1 1800000000000000000", NULL, NULL, NULL) == RTC_LP_OK);
}

static void uint_range_and_blank(void)
{
	rtc_rec_view v;
	unsigned d;
	CHECK(parse("m big=18446744073709551615u,ok=1i", &v, NULL, &d) == RTC_LP_OK);
	CHECK(d == 1 && v.nfields == 1 && streq(v.fields[0].key, "ok"));
	CHECK(parse("m big=18446744073709551615u", NULL, NULL, &d) == RTC_LP_NO_FIELDS && d == 1);
	CHECK(parse("m v=9223372036854775807u", &v, NULL, &d) == RTC_LP_OK && v.fields[0].v.i == INT64_MAX);
	CHECK(parse("", NULL, NULL, NULL) == RTC_LP_EMPTY);
	CHECK(parse("   ", NULL, NULL, NULL) == RTC_LP_EMPTY);
	CHECK(parse("# a comment", NULL, NULL, NULL) == RTC_LP_EMPTY);
	CHECK(parse("m v=1\r", &v, NULL, NULL) == RTC_LP_OK); /* CRLF */
}

static void bounds(void)
{
	/* More fields than a record holds. */
	char line[4096] = "m ";
	for (int i = 0; i < RTC_REC_MAX_FIELDS + 1; i++) {
		char f[16];
		snprintf(f, sizeof(f), "%sf%d=1", i ? "," : "", i);
		strcat(line, f);
	}
	CHECK(parse(line, NULL, NULL, NULL) == RTC_LP_MALFORMED);
	/* A record that does not fit the output buffer. */
	uint8_t small[16];
	size_t len;
	uint64_t ts;
	unsigned d;
	CHECK(rtc_lp_parse("measurement v=1", 15, &LIM, small, sizeof(small), &len, &ts, &d) ==
	      RTC_LP_MALFORMED);
}

const t_case lp_cases[] = {
    {"lp", "types_and_timestamp", types_and_timestamp},
    {"lp", "escapes", escapes},
    {"lp", "bools", bools},
    {"lp", "refused", refused},
    {"lp", "timestamps", timestamps},
    {"lp", "uint_range_and_blank", uint_range_and_blank},
    {"lp", "bounds", bounds},
    {0},
};
