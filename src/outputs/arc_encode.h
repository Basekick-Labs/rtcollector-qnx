/* Columnar MessagePack encoding for Arc's /api/v1/write/msgpack.
 *
 * One item per measurement per request:
 *   {"m": name, "columns": {"time": [ns...], "host": [...], <tags>, <fields>},
 *    "tag_keys": ["host", <tags>]}
 * Rows with different tag or field sets are null-filled. Never an all-nil
 * column, never a nil time.
 *
 * Rows Arc would refuse are validated out first, so one bad row cannot make
 * Arc refuse the whole request:
 *   - measurement not matching ^[a-zA-Z][a-zA-Z0-9_-]*$ (max 128)
 *   - a tag or field name that is not UTF-8, is "time" in any case, differs
 *     only in case from another column, or is both a tag and a field
 *   - a field whose type differs from the first row's in this request
 *   - NaN or infinite floats are dropped from the row; a row left with no
 *     fields is refused
 */
#ifndef RTC_ARC_ENCODE_H
#define RTC_ARC_ENCODE_H

#include "../core/config.h"
#include "../core/counters.h"
#include "../core/record.h"
#include "../core/ring.h"

#include <stddef.h>
#include <stdint.h>

#define RTC_ARC_MAX_COLS 128

enum { RTC_V_SKIP = 0, RTC_V_SENT = 1, RTC_V_REJECTED = 2 };

/* flags[] bits, per record in the batch, kept across retries. */
#define RTC_ARC_FLAG_SEEN 0x01

typedef struct {
	rtc_str name;
	uint8_t is_tag;
	uint8_t type;
} rtc_arc_col;

typedef struct {
	rtc_str m;
	uint16_t ncols;
	uint32_t nrows;
	rtc_arc_col cols[RTC_ARC_MAX_COLS];
} rtc_arc_group;

typedef struct {
	rtc_arc_group *groups;
	uint32_t max_groups;
	uint32_t ngroups;
	int32_t *row_group; /* per record in the batch, -1 if not in the request */
	size_t max_recs;
} rtc_arc_enc;

int rtc_arc_enc_init(rtc_arc_enc *e, size_t max_recs, uint32_t max_groups);
void rtc_arc_enc_free(rtc_arc_enc *e);

/* Encodes records [start, start+n) that are not done. flags[] (per record,
 * zeroed when the batch is read) makes validation counters count a record
 * once even when it is encoded again for a retry or a split. Writes a verdict per
 * record (RTC_V_*), counts validation drops in k, and returns the body
 * length, 0 when there is nothing to send, or SIZE_MAX when the body does not
 * fit in cap (send fewer records). */
size_t rtc_arc_encode(rtc_arc_enc *e, const rtc_config *cfg, const char *hostname, const rtc_ring_rec *recs,
                      const uint8_t *done, uint8_t *flags, size_t start, size_t n, uint8_t *verdict,
                      rtc_output_counters *k, uint8_t *out, size_t cap);

/* Measurement name of the group a record was encoded into, or NULL. */
const rtc_str *rtc_arc_record_measurement(const rtc_arc_enc *e, size_t rec);

bool rtc_utf8_valid(const char *s, size_t n);

#endif
