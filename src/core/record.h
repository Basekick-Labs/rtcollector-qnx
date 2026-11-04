/* Binary record payload.
 *
 * A record is what an input produces and what the ring stores after its
 * header. Layout (little-endian, unaligned, every length bounds-checked on
 * decode):
 *
 *   u8  kind                      RTC_REC_BIN
 *   u16 measurement_len, bytes
 *   u8  ntags   { u16 klen, k, u16 vlen, v }*
 *   u8  nfields { u16 klen, k, u8 type, value }*
 *       value: I64/F64 8 bytes, BOOL 1 byte, STR u16 len + bytes
 *
 * Building never allocates: the builder writes into a caller buffer and fails
 * when it would overflow.
 */
#ifndef RTC_RECORD_H
#define RTC_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { RTC_REC_BIN = 1 };

enum rtc_ftype { RTC_F_I64 = 1, RTC_F_F64 = 2, RTC_F_BOOL = 3, RTC_F_STR = 4 };

#define RTC_REC_MAX_TAGS   32
#define RTC_REC_MAX_FIELDS 64

typedef struct {
	const char *p;
	uint16_t len;
} rtc_str;

typedef struct {
	rtc_str key;
	uint8_t type;
	union {
		int64_t i;
		double f;
		bool b;
		rtc_str s;
	} v;
} rtc_field;

typedef struct {
	rtc_str measurement;
	uint8_t ntags;
	rtc_str tag_k[RTC_REC_MAX_TAGS];
	rtc_str tag_v[RTC_REC_MAX_TAGS];
	uint8_t nfields;
	rtc_field fields[RTC_REC_MAX_FIELDS];
} rtc_rec_view;

typedef struct {
	uint8_t *buf;
	size_t cap;
	size_t len;
	size_t ntags_at;
	size_t nfields_at;
	uint8_t ntags;
	uint8_t nfields;
	bool in_fields;
	bool failed;
	bool overflow; /* failed because the buffer was too small */
} rtc_rec_builder;

/* Start a record. Tags must all be added before the first field. */
void rtc_rb_init(rtc_rec_builder *b, uint8_t *buf, size_t cap, const char *measurement);
void rtc_rb_tag(rtc_rec_builder *b, const char *k, const char *v);
void rtc_rb_i64(rtc_rec_builder *b, const char *k, int64_t v);
void rtc_rb_f64(rtc_rec_builder *b, const char *k, double v);
void rtc_rb_bool(rtc_rec_builder *b, const char *k, bool v);
void rtc_rb_str(rtc_rec_builder *b, const char *k, const char *v);
/* Returns the payload length, or 0 if the record overflowed or is invalid
 * (no fields, too many tags or fields, a string longer than 65535). */
size_t rtc_rb_finish(rtc_rec_builder *b);

/* Decode a payload. Returns false on any malformed or out-of-bounds length.
 * The view points into buf. */
bool rtc_rec_decode(const uint8_t *buf, size_t len, rtc_rec_view *out);

#endif
