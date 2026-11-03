/* Configuration: rtcollector's YAML shape.
 *
 *   interval: 10            seconds between collection rounds
 *   flush_interval: 60      seconds between sends (also sends when a batch fills)
 *   buffer_bytes: 1MiB      ring size, allocated once (rounded to a power of two)
 *   max_record_bytes: 4096  largest record an input may produce
 *   max_buffer_size: 5000   accepted from rtcollector configs, ignored (see docs)
 *   warn_on_buffer: true
 *   hostname: ''            empty: gethostname()
 *   tags: {k: v}            added to every row at send time; win over input tags
 *   debug: false
 *   once: false             same as --once
 *   shutdown_timeout: 5     seconds for the final flush
 *   inputs: [internal, ...] a name, or {name: {config}}
 *   outputs:
 *     - arc: {url, database, token, batch_bytes, timeout, max_measurements_per_batch}
 *     - stdout: {}
 */
#ifndef RTC_CONFIG_H
#define RTC_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTC_CFG_MAX_TAGS 16
#define RTC_CFG_MAX_INPUTS 16

typedef struct {
	char key[128];
	char val[256];
} rtc_kv;

typedef struct {
	bool enabled;
	char url[512];
	char database[65];
	char token[512];
	size_t batch_bytes;
	uint32_t timeout_ms;
	uint32_t max_measurements;
} rtc_arc_cfg;

typedef struct {
	bool enabled;
	size_t batch_bytes;
} rtc_stdout_cfg;

typedef struct {
	double interval_s;
	double flush_interval_s;
	double shutdown_timeout_s;
	size_t buffer_bytes;
	uint32_t max_record_bytes;
	bool warn_on_buffer;
	bool debug;
	bool once;
	char hostname[256];
	rtc_kv tags[RTC_CFG_MAX_TAGS];
	unsigned ntags;
	char inputs[RTC_CFG_MAX_INPUTS][32];
	unsigned ninputs;
	rtc_arc_cfg arc;
	rtc_stdout_cfg out_stdout;
} rtc_config;

void rtc_config_defaults(rtc_config *c);

/* Loads and validates. Returns 0, or -1 with the reason logged (exit 2). */
int rtc_config_load(rtc_config *c, const char *path);
int rtc_config_load_string(rtc_config *c, const char *yaml, size_t len);
/* Validation alone (also run by the loaders). */
int rtc_config_validate(rtc_config *c);

/* "64KiB", "1MiB", "4096" -> bytes. Returns 0 or -1. */
int rtc_parse_size(const char *s, size_t *out);

/* Arc's rule for database and measurement names: ^[a-zA-Z][a-zA-Z0-9_-]*$ */
bool rtc_valid_arc_name(const char *s, size_t n, size_t max);

#endif
