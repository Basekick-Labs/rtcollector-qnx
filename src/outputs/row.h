/* Rows as every output sees them: which tags a row carries, with which
 * values, and whether Arc would accept the row (shared by arc, stdout, mqtt).
 *
 * Tag precedence, the same everywhere:
 *   - "host" first, then the configured tags, in every row;
 *   - a configured tag's value wins over the record's tag of that name;
 *   - a record's own "host" tag wins over the hostname;
 *   - the record's other tags follow (a name is matched exactly; Arc's
 *     admission refuses one that differs only in case).
 */
#ifndef RTC_ROW_H
#define RTC_ROW_H

#include "../core/config.h"
#include "../core/counters.h"
#include "../core/record.h"

#include <stdbool.h>
#include <stddef.h>

#define RTC_ROW_MAX_COLS 128

/* An injected tag: k, and v when it is configured (NULL for "host" without a
 * configured host: the record's tag or the hostname). */
typedef struct {
	const char *k;
	size_t kl;
	const char *v;
} rtc_row_inj;

/* Fills out with host then the configured tags; returns how many. */
unsigned rtc_row_injected(const rtc_config *cfg, rtc_row_inj out[RTC_CFG_MAX_TAGS + 1]);

/* The configured host: tags.host if set, else hostname. Never a record's. */
const char *rtc_row_cfg_host(const rtc_config *cfg, const char *hostname);

/* The value of injected tag in for record v (precedence above). */
const char *rtc_row_inj_value(const rtc_row_inj *in, const rtc_rec_view *v, const char *hostname,
                              size_t *len);

/* Is the record's tag key one of the injected ones (exact match)? */
bool rtc_row_is_injected(const rtc_row_inj *in, unsigned nin, rtc_str key);

/* The record's first tag called k (exact), or NULL. */
const char *rtc_row_tag(const rtc_rec_view *v, const char *k, size_t kl, size_t *len);

/* Columns of one group of rows that go out together (an Arc item, an MQTT
 * message): names, tag or field, and a field's type. */
typedef struct {
	rtc_str name;
	uint8_t is_tag;
	uint8_t type;
} rtc_row_col;

typedef struct {
	uint16_t ncols;
	rtc_row_col cols[RTC_ROW_MAX_COLS];
} rtc_row_cols;

/* Would Arc accept row v next to the group's columns? On success the row's
 * new columns are added to s. Refusals are counted in k: invalid_names (not
 * UTF-8, "time" in any case, a case-only clash, a tag also a field),
 * type_conflicts, columns_over_limit. NaN and infinite floats are left out
 * of the row (counted in fields_nonfinite when count_nonfinite, so a row
 * encoded again is counted once); a row left with no field is refused. */
bool rtc_row_admit(rtc_row_cols *s, const rtc_rec_view *v, const rtc_row_inj *in, unsigned nin,
                   rtc_output_counters *k, bool count_nonfinite);

bool rtc_utf8_valid(const char *s, size_t n);

#endif
