#include "lp.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define TOK_MAX 4096 /* longest name, value or number token (a line is at most a write) */

typedef struct {
	const char *p;
	const char *end;
} cur;

/* Copies an escaped token into dst until an unescaped stop character.
 * escapable: the characters a backslash makes literal. Returns the length,
 * or -1 if the token is empty or too long. */
static int token(cur *c, const char *stops, const char *escapable, char *dst)
{
	size_t n = 0;
	while (c->p < c->end && !strchr(stops, *c->p)) {
		char ch = *c->p++;
		if (ch == '\\' && c->p < c->end && strchr(escapable, *c->p))
			ch = *c->p++;
		if (n == TOK_MAX - 1)
			return -1;
		dst[n++] = ch;
	}
	dst[n] = '\0';
	return n ? (int)n : -1;
}

/* Strict decimal float: [-+]?digits[.digits][(e|E)[-+]?digits] or .digits. */
static bool is_decimal(const char *s)
{
	const char *p = s;
	bool digits = false;
	if (*p == '-') /* InfluxDB refuses a leading + */
		p++;
	while (*p >= '0' && *p <= '9') {
		p++;
		digits = true;
	}
	if (*p == '.') {
		p++;
		while (*p >= '0' && *p <= '9') {
			p++;
			digits = true;
		}
	}
	if (!digits)
		return false;
	if (*p == 'e' || *p == 'E') {
		p++;
		if (*p == '-' || *p == '+')
			p++;
		if (!(*p >= '0' && *p <= '9'))
			return false;
		while (*p >= '0' && *p <= '9')
			p++;
	}
	return *p == '\0';
}

static bool is_integer(const char *s, size_t n, bool allow_sign)
{
	size_t i = 0;
	if (allow_sign && n > 0 && s[0] == '-')
		i = 1;
	if (i == n)
		return false;
	for (; i < n; i++)
		if (s[i] < '0' || s[i] > '9')
			return false;
	return true;
}

static int parse_bool(const char *s)
{
	static const char *const t[] = {"t", "T", "true", "True", "TRUE"};
	static const char *const f[] = {"f", "F", "false", "False", "FALSE"};
	for (size_t i = 0; i < 5; i++) {
		if (!strcmp(s, t[i]))
			return 1;
		if (!strcmp(s, f[i]))
			return 0;
	}
	return -1;
}

enum rtc_lp_result rtc_lp_parse(const char *line, size_t len, const rtc_lp_limits *lim, uint8_t *out,
                                size_t cap, size_t *rec_len, uint64_t *ts_ns, unsigned *uint_dropped)
{
	cur c = {line, line + len};
	char key[TOK_MAX], val[TOK_MAX];
	*uint_dropped = 0;
	*ts_ns = 0;

	while (c.p < c.end && (*c.p == ' ' || *c.p == '\t' || *c.p == '\r'))
		c.p++;
	while (c.end > c.p && (c.end[-1] == ' ' || c.end[-1] == '\t' || c.end[-1] == '\r'))
		c.end--;
	if (c.p == c.end || *c.p == '#')
		return RTC_LP_EMPTY;
	/* A NUL would silently cut names and strings (the record builder takes
	 * C strings): refuse the line. */
	if (memchr(c.p, '\0', (size_t)(c.end - c.p)))
		return RTC_LP_MALFORMED;

	if (token(&c, ", ", ", ", key) < 0)
		return RTC_LP_MALFORMED;
	rtc_rec_builder b;
	rtc_rb_init(&b, out, cap, key);

	/* Tags. */
	while (c.p < c.end && *c.p == ',') {
		c.p++;
		if (token(&c, "=, ", ",= ", key) < 0 || c.p == c.end || *c.p != '=')
			return RTC_LP_MALFORMED;
		c.p++;
		if (token(&c, ", ", ",= ", val) < 0)
			return RTC_LP_MALFORMED;
		rtc_rb_tag(&b, key, val);
	}
	if (c.p == c.end || *c.p != ' ')
		return RTC_LP_MALFORMED; /* no field set */
	while (c.p < c.end && *c.p == ' ')
		c.p++;

	/* Fields. */
	unsigned nfields = 0;
	for (;;) {
		if (token(&c, "=, ", ",= ", key) < 0 || c.p == c.end || *c.p != '=')
			return RTC_LP_MALFORMED;
		c.p++;
		if (c.p < c.end && *c.p == '"') {
			c.p++;
			size_t n = 0;
			for (;;) {
				if (c.p == c.end)
					return RTC_LP_MALFORMED; /* unterminated string */
				char ch = *c.p++;
				if (ch == '"')
					break;
				if (ch == '\\' && c.p < c.end && (*c.p == '"' || *c.p == '\\'))
					ch = *c.p++;
				if (n == TOK_MAX - 1)
					return RTC_LP_MALFORMED;
				val[n++] = ch;
			}
			val[n] = '\0';
			rtc_rb_str(&b, key, val);
			nfields++;
		} else {
			int n = token(&c, ", ", "", val);
			if (n <= 0) /* token() never returns 0, but be explicit */
				return RTC_LP_MALFORMED;
			char last = val[n - 1];
			int bv;
			if (last == 'i' && is_integer(val, (size_t)n - 1, true)) {
				val[n - 1] = '\0';
				errno = 0;
				long long v = strtoll(val, NULL, 10);
				if (errno == ERANGE)
					return RTC_LP_MALFORMED;
				rtc_rb_i64(&b, key, (int64_t)v);
				nfields++;
			} else if (last == 'u' && is_integer(val, (size_t)n - 1, false)) {
				val[n - 1] = '\0';
				errno = 0;
				unsigned long long v = strtoull(val, NULL, 10);
				if (errno == ERANGE || v > INT64_MAX) {
					(*uint_dropped)++; /* Arc has no unsigned 64-bit type */
				} else {
					rtc_rb_i64(&b, key, (int64_t)v);
					nfields++;
				}
			} else if ((bv = parse_bool(val)) >= 0) {
				rtc_rb_bool(&b, key, bv == 1);
				nfields++;
			} else if (is_decimal(val)) {
				errno = 0;
				double v = strtod(val, NULL);
				if (errno == ERANGE)
					return RTC_LP_MALFORMED;
				rtc_rb_f64(&b, key, v);
				nfields++;
			} else {
				return RTC_LP_MALFORMED;
			}
		}
		if (c.p < c.end && *c.p == ',') {
			c.p++;
			continue;
		}
		break;
	}

	/* Timestamp. */
	if (c.p < c.end) {
		if (*c.p != ' ')
			return RTC_LP_MALFORMED;
		while (c.p < c.end && *c.p == ' ')
			c.p++;
		size_t n = (size_t)(c.end - c.p);
		if (n == 0 || n >= 32 || !is_integer(c.p, n, true))
			return RTC_LP_MALFORMED;
		memcpy(val, c.p, n);
		val[n] = '\0';
		if (val[0] == '-')
			return RTC_LP_BAD_TIMESTAMP;
		errno = 0;
		unsigned long long ts = strtoull(val, NULL, 10);
		if (errno == ERANGE || ts < lim->floor_ns || ts > lim->ceiling_ns)
			return RTC_LP_BAD_TIMESTAMP;
		*ts_ns = ts;
	}

	if (nfields == 0)
		return *uint_dropped ? RTC_LP_NO_FIELDS : RTC_LP_MALFORMED;
	if (b.overflow)
		return RTC_LP_TOO_LARGE; /* the record does not fit max_record_bytes */
	size_t l = rtc_rb_finish(&b);
	if (l == 0)
		return RTC_LP_MALFORMED; /* too many tags or fields */
	*rec_len = l;
	return RTC_LP_OK;
}
