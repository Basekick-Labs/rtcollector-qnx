/* Configuration: rtcollector's YAML shape.
 *
 *   interval: 10            seconds between collection rounds
 *   flush_interval: =interval  seconds between sends (also sends when a batch fills)
 *   buffer_bytes: 1MiB      ring size, allocated once (rounded to a power of two)
 *   max_record_bytes: 4096  largest record an input may produce (min 512)
 *   max_buffer_size: 5000   accepted from rtcollector configs, ignored (see docs)
 *   warn_on_buffer: true
 *   hostname: ''            empty: gethostname()
 *   tags: {k: v}            added to every row at send time; win over input tags
 *   debug: false
 *   once: false             same as --once
 *   shutdown_timeout: 5     seconds for the final flush
 *   collect_priority: 10    QNX: collection thread priority (SCHED_RR, 1-253)
 *   send_priority: 9        QNX: sender thread priority
 *   processes_every: 6      rounds between process scans
 *   max_processes: 4096     a scan stops here (overflow counted)
 *   rmgr_path: /dev/rtcollector   rmgr input: where programs write line protocol
 *   rmgr_mode: 0660         its permissions (octal); rmgr_group: 0 its group
 *   rmgr_priority: 63       its handlers' fixed priority: above every writer
 *   rmgr_threads: 4         handler threads, each with an rmgr_buffer_bytes buffer
 *   trace_interval: =interval  seconds per kernel-trace interval (input trace)
 *   trace_priority: 15      the trace logger's priority (below real-time work)
 *   trace_min_priority: 40, trace_top: 16, trace_max_rows: 64   which threads get rows
 *   trace_max_threads: 1024, trace_max_bytes: 4MiB   bounds
 *   rmgr_buffer_bytes: 256KiB, max_write_bytes: 4096, rmgr_drain_bytes: 64KiB
 *   inputs: [internal, ...] a name, or {name: {config}}
 *   outputs:
 *     - arc: {url, database, token, batch_bytes, timeout, max_measurements_per_batch (1-256)}
 *     - stdout: {}
 *     - mqtt: {url, topic, format, time_precision, client_id, username, password,
 *              keepalive, timeout, batch_bytes, max_message_bytes, status_topic}
 */
#ifndef RTC_CONFIG_H
#define RTC_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTC_CFG_MAX_TAGS   16
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
	char ca_file[512]; /* CA bundle for https; empty: libcurl's default */
	size_t batch_bytes;
	bool batch_set; /* batch_bytes given explicitly */
	uint32_t timeout_ms;
	uint32_t max_measurements;
} rtc_arc_cfg;

typedef struct {
	bool enabled;
	size_t batch_bytes;
	bool batch_set;
} rtc_stdout_cfg;

#define RTC_MQTT_TOPIC_MAX 1024

typedef struct {
	bool enabled;
	char url[512];
	char host[256]; /* from url: a name or an address ([v6] without brackets) */
	char port[8];
	char topic[RTC_MQTT_TOPIC_MAX + 1];        /* template: {host}, {measurement} */
	char status_topic[RTC_MQTT_TOPIC_MAX + 1]; /* template: {host}; "" = no presence */
	bool json;                                 /* format: json (default msgpack) */
	uint64_t time_div;                         /* ns per unit of time_precision */
	char client_id[128];                       /* "": rtc + hash(host) + random */
	char username[256];
	char password[256];
	uint32_t keepalive_s; /* 0 = off */
	uint32_t timeout_ms;
	size_t batch_bytes;
	bool batch_set;
	size_t max_message_bytes; /* the whole PUBLISH packet */
} rtc_mqtt_cfg;

typedef struct {
	double interval_s;
	double flush_interval_s;
	double shutdown_timeout_s;
	int collect_priority; /* QNX scheduling priority, SCHED_RR */
	int send_priority;
	unsigned processes_every; /* rounds between process scans */
	/* /dev/rtcollector (input rmgr, QNX) */
	char rmgr_path[256];
	unsigned rmgr_mode;    /* permission bits, octal in YAML */
	int rmgr_group;        /* gid owning the device */
	int rmgr_priority;     /* fixed priority of the device's handler threads */
	unsigned rmgr_threads; /* handler threads (each with its own buffer) */
	/* kernel trace (input trace, QNX) */
	double trace_interval_s; /* 0 = interval */
	int trace_priority;
	int trace_min_priority;
	unsigned trace_top;
	unsigned trace_max_rows;
	unsigned trace_max_threads;
	size_t trace_max_bytes;
	size_t rmgr_buffer_bytes;
	size_t max_write_bytes;
	size_t rmgr_drain_bytes;
	uint64_t max_processes;
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
	rtc_mqtt_cfg mqtt;
} rtc_config;

void rtc_config_defaults(rtc_config *c);

/* Loads and validates. Returns 0, or -1 with the reason logged (exit 2). */
int rtc_config_load(rtc_config *c, const char *path);
int rtc_config_load_string(rtc_config *c, const char *yaml, size_t len);
/* Validation alone (also run by the loaders). */
int rtc_config_validate(rtc_config *c);

/* "64KiB", "1MiB", "4096" -> bytes. Returns 0 or -1. */
int rtc_parse_size(const char *s, size_t *out);

/* Expands an MQTT topic template: {host} and {measurement} (m, may be NULL
 * for a template without it). Returns the length, or -1 when it does not fit
 * in cap or names another placeholder. */
int rtc_mqtt_topic_expand(const char *tmpl, const char *host, const char *m, size_t ml, char *out,
                          size_t cap);

/* Arc's rule for database and measurement names: ^[a-zA-Z][a-zA-Z0-9_-]*$ */
bool rtc_valid_arc_name(const char *s, size_t n, size_t max);

#endif
