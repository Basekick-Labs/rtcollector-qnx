/* Line-protocol parser: one line into a binary record. Never allocates.
 *
 *   measurement[,tag=value...] field=value[,field=value...] [timestamp_ns]
 *
 * Fields: float (finite decimal only: no nan, inf or hex), 123i int64,
 * 123u uint (above INT64_MAX the field is dropped and counted), bool
 * t|T|true|True|TRUE|f|F|false|False|FALSE, "string" with \" and \\.
 * Escapes: measurement \, and \space; tag keys, tag values and field keys
 * \, \= and \space. Timestamp: nanoseconds only, within [floor, ceiling].
 */
#ifndef RTC_LP_H
#define RTC_LP_H

#include "record.h"

#include <stddef.h>
#include <stdint.h>

enum rtc_lp_result {
	RTC_LP_OK = 0,
	RTC_LP_EMPTY,         /* blank line or # comment: nothing to do */
	RTC_LP_MALFORMED,     /* syntax, bounds or an unparsable value */
	RTC_LP_BAD_TIMESTAMP, /* not a nanosecond time within [floor, ceiling] */
	RTC_LP_NO_FIELDS,     /* every field was dropped (uint out of range) */
};

typedef struct {
	uint64_t floor_ns;   /* earliest acceptable explicit timestamp */
	uint64_t ceiling_ns; /* latest acceptable explicit timestamp */
} rtc_lp_limits;

/* Parses line[0..len) (no newline) into a record built in out[0..cap).
 * On RTC_LP_OK: *rec_len is the record length, *ts_ns the timestamp or 0 if
 * the line had none. *uint_dropped counts fields dropped for range. */
enum rtc_lp_result rtc_lp_parse(const char *line, size_t len, const rtc_lp_limits *lim, uint8_t *out,
                                size_t cap, size_t *rec_len, uint64_t *ts_ns, unsigned *uint_dropped);

#endif
