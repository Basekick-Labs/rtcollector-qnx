#include "t.h"

#include "../src/core/record.h"

#include <math.h>
#include <string.h>

static void roundtrip(void)
{
	uint8_t buf[512];
	rtc_rec_builder b;
	rtc_rb_init(&b, buf, sizeof(buf), "cpu");
	rtc_rb_tag(&b, "core", "0");
	rtc_rb_i64(&b, "n", -42);
	rtc_rb_f64(&b, "user", 12.5);
	rtc_rb_bool(&b, "ok", true);
	rtc_rb_str(&b, "note", "hi \"there\"");
	size_t len = rtc_rb_finish(&b);
	CHECK(len > 0);

	rtc_rec_view v;
	CHECK(rtc_rec_decode(buf, len, &v));
	CHECK(v.measurement.len == 3 && !memcmp(v.measurement.p, "cpu", 3));
	CHECK_EQ_U(v.ntags, 1);
	CHECK(v.tag_k[0].len == 4 && !memcmp(v.tag_v[0].p, "0", 1));
	CHECK_EQ_U(v.nfields, 4);
	CHECK(v.fields[0].type == RTC_F_I64 && v.fields[0].v.i == -42);
	CHECK(v.fields[1].type == RTC_F_F64 && v.fields[1].v.f == 12.5);
	CHECK(v.fields[2].type == RTC_F_BOOL && v.fields[2].v.b);
	CHECK(v.fields[3].type == RTC_F_STR && v.fields[3].v.s.len == 10);
}

static void refuses_bad_records(void)
{
	uint8_t buf[64];
	rtc_rec_builder b;

	rtc_rb_init(&b, buf, sizeof(buf), "m"); /* no fields */
	CHECK_EQ_U(rtc_rb_finish(&b), 0);

	rtc_rb_init(&b, buf, sizeof(buf), "m"); /* overflow */
	rtc_rb_str(&b, "f", "0123456789012345678901234567890123456789012345678901234567890123456789");
	CHECK_EQ_U(rtc_rb_finish(&b), 0);

	rtc_rb_init(&b, buf, sizeof(buf), "m"); /* tag after a field */
	rtc_rb_i64(&b, "f", 1);
	rtc_rb_tag(&b, "t", "v");
	CHECK_EQ_U(rtc_rb_finish(&b), 0);

	uint8_t big[8192];
	rtc_rb_init(&b, big, sizeof(big), "m"); /* too many fields */
	for (int i = 0; i < RTC_REC_MAX_FIELDS + 1; i++)
		rtc_rb_i64(&b, "f", i);
	CHECK_EQ_U(rtc_rb_finish(&b), 0);
}

static void decode_rejects_every_truncation(void)
{
	uint8_t buf[256];
	rtc_rec_builder b;
	rtc_rb_init(&b, buf, sizeof(buf), "disk");
	rtc_rb_tag(&b, "dev", "sda");
	rtc_rb_f64(&b, "used", 0.5);
	rtc_rb_str(&b, "fs", "ext4");
	size_t len = rtc_rb_finish(&b);
	rtc_rec_view v;
	for (size_t n = 0; n < len; n++)
		CHECK(!rtc_rec_decode(buf, n, &v));
	CHECK(rtc_rec_decode(buf, len, &v));
	/* Trailing garbage is refused too. */
	buf[len] = 0;
	CHECK(!rtc_rec_decode(buf, len + 1, &v));
	/* Corrupting bytes one at a time: some must be refused (lengths, types). */
	uint8_t copy[256];
	bool any_refused = false;
	for (size_t i = 0; i < len; i++) {
		memcpy(copy, buf, len);
		copy[i] ^= 0xff;
		if (!rtc_rec_decode(copy, len, &v))
			any_refused = true;
	}
	CHECK(any_refused);
}

static void nan_survives_encoding(void)
{
	uint8_t buf[64];
	rtc_rec_builder b;
	rtc_rb_init(&b, buf, sizeof(buf), "m");
	rtc_rb_f64(&b, "x", NAN);
	size_t len = rtc_rb_finish(&b);
	rtc_rec_view v;
	CHECK(rtc_rec_decode(buf, len, &v));
	CHECK(isnan(v.fields[0].v.f)); /* the Arc encoder, not the record, decides */
}

const t_case record_cases[] = {
    {"record", "roundtrip", roundtrip},
    {"record", "refuses_bad_records", refuses_bad_records},
    {"record", "decode_rejects_every_truncation", decode_rejects_every_truncation},
    {"record", "nan_survives_encoding", nan_survives_encoding},
    {0},
};
