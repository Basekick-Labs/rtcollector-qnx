#include "t.h"

#include "../src/core/msgpack.h"

#include <string.h>

static bool bytes(const rtc_mp *w, const char *hex)
{
	size_t n = strlen(hex) / 2;
	if (w->len != n)
		return false;
	for (size_t i = 0; i < n; i++) {
		unsigned v;
		sscanf(hex + 2 * i, "%2x", &v);
		if (w->buf[i] != v)
			return false;
	}
	return true;
}

#define ENC(stmt, hex)                                                                                       \
	do {                                                                                                 \
		uint8_t b[64];                                                                               \
		rtc_mp w;                                                                                    \
		rtc_mp_init(&w, b, sizeof(b));                                                               \
		stmt;                                                                                        \
		CHECK(rtc_mp_ok(&w) && bytes(&w, hex));                                                      \
	} while (0)

static void scalars(void)
{
	ENC(rtc_mp_nil(&w), "c0");
	ENC(rtc_mp_bool(&w, true), "c3");
	ENC(rtc_mp_bool(&w, false), "c2");
	ENC(rtc_mp_int(&w, 0), "00");
	ENC(rtc_mp_int(&w, 127), "7f");
	ENC(rtc_mp_int(&w, -1), "ff");
	ENC(rtc_mp_int(&w, -32), "e0");
	ENC(rtc_mp_int(&w, 128), "d30000000000000080");
	ENC(rtc_mp_int(&w, -33), "d3ffffffffffffffdf");
	/* 3.0 must stay a float: Arc types the column from the first value. */
	ENC(rtc_mp_f64(&w, 3.0), "cb4008000000000000");
}

static void strings_and_containers(void)
{
	ENC(rtc_mp_cstr(&w, "m"), "a16d");
	ENC(rtc_mp_array(&w, 3), "93");
	ENC(rtc_mp_array(&w, 16), "dc0010");
	ENC(rtc_mp_map(&w, 15), "8f");
	ENC(rtc_mp_map(&w, 70000), "df00011170");
	char s32[33];
	memset(s32, 'x', 32);
	s32[32] = 0;
	uint8_t b[64];
	rtc_mp w;
	rtc_mp_init(&w, b, sizeof(b));
	rtc_mp_cstr(&w, s32);
	CHECK(rtc_mp_ok(&w) && b[0] == 0xd9 && b[1] == 32 && w.len == 34);
}

static void overflow_marks_failed(void)
{
	uint8_t b[4];
	rtc_mp w;
	rtc_mp_init(&w, b, sizeof(b));
	rtc_mp_int(&w, 1000);
	CHECK(!rtc_mp_ok(&w));
	size_t len = w.len;
	rtc_mp_nil(&w);
	CHECK(w.len == len); /* no-op after failure */
}

const t_case msgpack_cases[] = {
    {"msgpack", "scalars", scalars},
    {"msgpack", "strings_and_containers", strings_and_containers},
    {"msgpack", "overflow_marks_failed", overflow_marks_failed},
    {0},
};
