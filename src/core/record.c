#include "record.h"

#include <string.h>

static void put(rtc_rec_builder *b, const void *p, size_t n)
{
	if (b->failed)
		return;
	if (n > b->cap - b->len) {
		b->failed = true;
		return;
	}
	memcpy(b->buf + b->len, p, n);
	b->len += n;
}

static void put_u8(rtc_rec_builder *b, uint8_t v)
{
	put(b, &v, 1);
}

static void put_u16(rtc_rec_builder *b, uint16_t v)
{
	uint8_t le[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
	put(b, le, 2);
}

static void put_u64(rtc_rec_builder *b, uint64_t v)
{
	uint8_t le[8];
	for (int i = 0; i < 8; i++)
		le[i] = (uint8_t)(v >> (8 * i));
	put(b, le, 8);
}

static void put_str(rtc_rec_builder *b, const char *s)
{
	size_t n = strlen(s);
	if (n > UINT16_MAX) {
		b->failed = true;
		return;
	}
	put_u16(b, (uint16_t)n);
	put(b, s, n);
}

void rtc_rb_init(rtc_rec_builder *b, uint8_t *buf, size_t cap, const char *measurement)
{
	memset(b, 0, sizeof(*b));
	b->buf = buf;
	b->cap = cap;
	put_u8(b, RTC_REC_BIN);
	put_str(b, measurement);
	b->ntags_at = b->len;
	put_u8(b, 0);
}

void rtc_rb_tag(rtc_rec_builder *b, const char *k, const char *v)
{
	if (b->in_fields || b->ntags == RTC_REC_MAX_TAGS) {
		b->failed = true;
		return;
	}
	put_str(b, k);
	put_str(b, v);
	b->ntags++;
}

static bool start_field(rtc_rec_builder *b, const char *k, uint8_t type)
{
	if (!b->in_fields) {
		b->in_fields = true;
		b->nfields_at = b->len;
		put_u8(b, 0);
	}
	if (b->nfields == RTC_REC_MAX_FIELDS) {
		b->failed = true;
		return false;
	}
	put_str(b, k);
	put_u8(b, type);
	b->nfields++;
	return !b->failed;
}

void rtc_rb_i64(rtc_rec_builder *b, const char *k, int64_t v)
{
	if (start_field(b, k, RTC_F_I64))
		put_u64(b, (uint64_t)v);
}

void rtc_rb_f64(rtc_rec_builder *b, const char *k, double v)
{
	uint64_t bits;
	memcpy(&bits, &v, 8);
	if (start_field(b, k, RTC_F_F64))
		put_u64(b, bits);
}

void rtc_rb_bool(rtc_rec_builder *b, const char *k, bool v)
{
	if (start_field(b, k, RTC_F_BOOL))
		put_u8(b, v ? 1 : 0);
}

void rtc_rb_str(rtc_rec_builder *b, const char *k, const char *v)
{
	if (start_field(b, k, RTC_F_STR))
		put_str(b, v);
}

size_t rtc_rb_finish(rtc_rec_builder *b)
{
	if (b->failed || b->nfields == 0)
		return 0;
	b->buf[b->ntags_at] = b->ntags;
	b->buf[b->nfields_at] = b->nfields;
	return b->len;
}

typedef struct {
	const uint8_t *p;
	size_t len;
	size_t off;
	bool bad;
} rd;

static bool take(rd *r, size_t n, const uint8_t **out)
{
	if (r->bad || n > r->len - r->off) {
		r->bad = true;
		return false;
	}
	*out = r->p + r->off;
	r->off += n;
	return true;
}

static uint8_t get_u8(rd *r)
{
	const uint8_t *q;
	return take(r, 1, &q) ? q[0] : 0;
}

static uint16_t get_u16(rd *r)
{
	const uint8_t *q;
	return take(r, 2, &q) ? (uint16_t)(q[0] | (q[1] << 8)) : 0;
}

static uint64_t get_u64(rd *r)
{
	const uint8_t *q;
	uint64_t v = 0;
	if (!take(r, 8, &q))
		return 0;
	for (int i = 0; i < 8; i++)
		v |= (uint64_t)q[i] << (8 * i);
	return v;
}

static rtc_str get_str(rd *r)
{
	rtc_str s = {0};
	uint16_t n = get_u16(r);
	const uint8_t *q;
	if (take(r, n, &q)) {
		s.p = (const char *)q;
		s.len = n;
	}
	return s;
}

bool rtc_rec_decode(const uint8_t *buf, size_t len, rtc_rec_view *out)
{
	rd r = {buf, len, 0, false};
	if (get_u8(&r) != RTC_REC_BIN)
		return false;
	out->measurement = get_str(&r);
	out->ntags = get_u8(&r);
	if (out->ntags > RTC_REC_MAX_TAGS)
		return false;
	for (uint8_t i = 0; i < out->ntags; i++) {
		out->tag_k[i] = get_str(&r);
		out->tag_v[i] = get_str(&r);
	}
	out->nfields = get_u8(&r);
	if (out->nfields == 0 || out->nfields > RTC_REC_MAX_FIELDS)
		return false;
	for (uint8_t i = 0; i < out->nfields; i++) {
		rtc_field *f = &out->fields[i];
		f->key = get_str(&r);
		f->type = get_u8(&r);
		switch (f->type) {
		case RTC_F_I64:
			f->v.i = (int64_t)get_u64(&r);
			break;
		case RTC_F_F64: {
			uint64_t bits = get_u64(&r);
			memcpy(&f->v.f, &bits, 8);
			break;
		}
		case RTC_F_BOOL:
			f->v.b = get_u8(&r) != 0;
			break;
		case RTC_F_STR:
			f->v.s = get_str(&r);
			break;
		default:
			return false;
		}
	}
	return !r.bad && r.off == len && out->measurement.len > 0;
}
