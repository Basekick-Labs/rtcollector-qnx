#include "config.h"

#include "log.h"
#include "collector.h"
#include "ring.h"
#include "../outputs/row.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <yaml.h>

#define C "config"

void rtc_config_defaults(rtc_config *c)
{
	memset(c, 0, sizeof(*c));
	c->interval_s = 10;
	c->flush_interval_s = -1; /* = interval */
	c->shutdown_timeout_s = 5;
	c->collect_priority = 10;
	c->send_priority = 9;
	c->processes_every = 6;
	c->max_processes = 4096;
	snprintf(c->rmgr_path, sizeof(c->rmgr_path), "/dev/rtcollector");
	c->rmgr_mode = 0660;
	c->rmgr_group = 0;
	c->rmgr_priority = 63;
	c->rmgr_threads = 4;
	c->trace_interval_s = 0;
	c->trace_priority = 15;
	c->trace_min_priority = 40;
	c->trace_top = 16;
	c->trace_max_rows = 64;
	c->trace_max_threads = 1024;
	c->trace_max_bytes = 4u << 20;
	c->rmgr_buffer_bytes = 256u << 10;
	c->max_write_bytes = 4096;
	c->rmgr_drain_bytes = 64u << 10;
	c->buffer_bytes = 1u << 20;
	c->max_record_bytes = 4096;
	c->warn_on_buffer = true;
	c->arc.batch_bytes = 256u << 10;
	c->arc.timeout_ms = 10000;
	c->arc.max_measurements = 64;
	snprintf(c->arc.database, sizeof(c->arc.database), "default");
	c->out_stdout.batch_bytes = 64u << 10;
	snprintf(c->mqtt.topic, sizeof(c->mqtt.topic), "rtcollector/{host}/{measurement}");
	snprintf(c->mqtt.status_topic, sizeof(c->mqtt.status_topic), "rtcollector/{host}");
	c->mqtt.time_div = 1000; /* us: Arc's own unit */
	c->mqtt.keepalive_s = 60;
	c->mqtt.timeout_ms = 10000;
	c->mqtt.batch_bytes = 256u << 10;
	c->mqtt.max_message_bytes = 128u << 10;
}

int rtc_mqtt_topic_expand(const char *tmpl, const char *host, const char *m, size_t ml, char *out, size_t cap)
{
	size_t n = 0;
	for (const char *p = tmpl; *p;) {
		const char *add = p;
		size_t al = 1;
		if (*p == '{') {
			if (!strncmp(p, "{host}", 6)) {
				add = host;
				al = strlen(host);
				p += 6;
			} else if (!strncmp(p, "{measurement}", 13) && m) {
				add = m;
				al = ml;
				p += 13;
			} else
				return -1;
		} else
			p++;
		if (al >= cap - n)
			return -1;
		memcpy(out + n, add, al);
		n += al;
	}
	out[n] = '\0';
	return (int)n;
}

bool rtc_valid_arc_name(const char *s, size_t n, size_t max)
{
	if (n == 0 || n > max || !isalpha((unsigned char)s[0]))
		return false;
	for (size_t i = 1; i < n; i++) {
		unsigned char ch = (unsigned char)s[i];
		if (!(isalnum(ch) || ch == '_' || ch == '-'))
			return false;
	}
	return true;
}

int rtc_parse_size(const char *s, size_t *out)
{
	char *end;
	errno = 0;
	unsigned long long v = strtoull(s, &end, 10);
	if (end == s || errno || s[0] == '-')
		return -1;
	while (*end == ' ')
		end++;
	unsigned long long mul = 1;
	if (*end == '\0' || strcmp(end, "B") == 0)
		mul = 1;
	else if (strcmp(end, "KiB") == 0 || strcmp(end, "KB") == 0 || strcmp(end, "K") == 0)
		mul = 1ull << 10;
	else if (strcmp(end, "MiB") == 0 || strcmp(end, "MB") == 0 || strcmp(end, "M") == 0)
		mul = 1ull << 20;
	else if (strcmp(end, "GiB") == 0 || strcmp(end, "GB") == 0 || strcmp(end, "G") == 0)
		mul = 1ull << 30;
	else
		return -1;
	if (v > SIZE_MAX / mul)
		return -1;
	*out = (size_t)(v * mul);
	return 0;
}

/* --- YAML helpers ------------------------------------------------------- */

typedef struct {
	yaml_document_t *doc;
	int errors;
} ctx;

static yaml_node_t *node(ctx *x, int id)
{
	return yaml_document_get_node(x->doc, id);
}

static const char *scalar(yaml_node_t *n)
{
	return n && n->type == YAML_SCALAR_NODE ? (const char *)n->data.scalar.value : NULL;
}

static void err(ctx *x, const char *key, const char *why)
{
	RTC_ERROR(C, "%s: %s", key, why);
	x->errors++;
}

static void get_num(ctx *x, yaml_node_t *n, const char *key, double *out)
{
	const char *s = scalar(n);
	char *end;
	if (!s) {
		err(x, key, "must be a number");
		return;
	}
	errno = 0;
	double v = strtod(s, &end);
	if (end == s || *end || errno || !isfinite(v)) {
		err(x, key, "must be a number");
		return;
	}
	*out = v;
}

static void get_bool(ctx *x, yaml_node_t *n, const char *key, bool *out)
{
	const char *s = scalar(n);
	if (s && (!strcmp(s, "true") || !strcmp(s, "True") || !strcmp(s, "yes")))
		*out = true;
	else if (s && (!strcmp(s, "false") || !strcmp(s, "False") || !strcmp(s, "no")))
		*out = false;
	else
		err(x, key, "must be true or false");
}

static void get_size(ctx *x, yaml_node_t *n, const char *key, size_t *out)
{
	const char *s = scalar(n);
	if (!s || rtc_parse_size(s, out) != 0)
		err(x, key, "must be a size such as 4096, 64KiB or 1MiB");
}

static void get_str(ctx *x, yaml_node_t *n, const char *key, char *dst, size_t cap)
{
	const char *s = scalar(n);
	if (!s) {
		err(x, key, "must be a string");
		return;
	}
	if (strlen(s) >= cap) {
		err(x, key, "is too long");
		return;
	}
	snprintf(dst, cap, "%s", s);
}

static void load_arc(ctx *x, rtc_config *c, yaml_node_t *m)
{
	c->arc.enabled = true;
	if (!m || m->type != YAML_MAPPING_NODE)
		return; /* `- arc:` with no settings: defaults, url missing -> validate fails */
	for (yaml_node_pair_t *p = m->data.mapping.pairs.start; p < m->data.mapping.pairs.top; p++) {
		const char *k = scalar(node(x, p->key));
		yaml_node_t *v = node(x, p->value);
		if (!k)
			continue;
		if (!strcmp(k, "url"))
			get_str(x, v, "outputs.arc.url", c->arc.url, sizeof(c->arc.url));
		else if (!strcmp(k, "database"))
			get_str(x, v, "outputs.arc.database", c->arc.database, sizeof(c->arc.database));
		else if (!strcmp(k, "ca_file"))
			get_str(x, v, "outputs.arc.ca_file", c->arc.ca_file, sizeof(c->arc.ca_file));
		else if (!strcmp(k, "token"))
			get_str(x, v, "outputs.arc.token", c->arc.token, sizeof(c->arc.token));
		else if (!strcmp(k, "batch_bytes")) {
			get_size(x, v, "outputs.arc.batch_bytes", &c->arc.batch_bytes);
			c->arc.batch_set = true;
		} else if (!strcmp(k, "timeout")) {
			double t = 0;
			get_num(x, v, "outputs.arc.timeout", &t);
			if (t < 0.001 || t > 600)
				err(x, "outputs.arc.timeout", "must be from 0.001 to 600 seconds");
			else
				c->arc.timeout_ms = (uint32_t)(t * 1000);
		} else if (!strcmp(k, "max_measurements_per_batch")) {
			double t = 0;
			get_num(x, v, "outputs.arc.max_measurements_per_batch", &t);
			if (t < 1 || t > 256 || t != floor(t))
				err(x, "outputs.arc.max_measurements_per_batch",
				    "must be an integer from 1 to 256");
			else
				c->arc.max_measurements = (uint32_t)t;
		} else
			RTC_WARN(C, "outputs.arc.%s: unknown key, ignored", k);
	}
}

static void load_stdout(ctx *x, rtc_config *c, yaml_node_t *m)
{
	c->out_stdout.enabled = true;
	if (!m || m->type != YAML_MAPPING_NODE)
		return;
	for (yaml_node_pair_t *p = m->data.mapping.pairs.start; p < m->data.mapping.pairs.top; p++) {
		const char *k = scalar(node(x, p->key));
		if (k && !strcmp(k, "batch_bytes")) {
			get_size(x, node(x, p->value), "outputs.stdout.batch_bytes",
			         &c->out_stdout.batch_bytes);
			c->out_stdout.batch_set = true;
		} else if (k)
			RTC_WARN(C, "outputs.stdout.%s: unknown key, ignored", k);
	}
}

static void load_mqtt(ctx *x, rtc_config *c, yaml_node_t *m)
{
	rtc_mqtt_cfg *q = &c->mqtt;
	q->enabled = true;
	if (!m || m->type != YAML_MAPPING_NODE)
		return; /* url missing -> validate fails */
	for (yaml_node_pair_t *p = m->data.mapping.pairs.start; p < m->data.mapping.pairs.top; p++) {
		const char *k = scalar(node(x, p->key));
		yaml_node_t *v = node(x, p->value);
		if (!k)
			continue;
		double t = 0;
		if (!strcmp(k, "url"))
			get_str(x, v, "outputs.mqtt.url", q->url, sizeof(q->url));
		else if (!strcmp(k, "topic"))
			get_str(x, v, "outputs.mqtt.topic", q->topic, sizeof(q->topic));
		else if (!strcmp(k, "status_topic"))
			get_str(x, v, "outputs.mqtt.status_topic", q->status_topic, sizeof(q->status_topic));
		else if (!strcmp(k, "client_id"))
			get_str(x, v, "outputs.mqtt.client_id", q->client_id, sizeof(q->client_id));
		else if (!strcmp(k, "username"))
			get_str(x, v, "outputs.mqtt.username", q->username, sizeof(q->username));
		else if (!strcmp(k, "password"))
			get_str(x, v, "outputs.mqtt.password", q->password, sizeof(q->password));
		else if (!strcmp(k, "format")) {
			const char *f = scalar(v);
			if (f && !strcmp(f, "json"))
				q->json = true;
			else if (f && !strcmp(f, "msgpack"))
				q->json = false;
			else
				err(x, "outputs.mqtt.format", "must be msgpack or json");
		} else if (!strcmp(k, "time_precision")) {
			const char *f = scalar(v);
			static const struct {
				const char *n;
				uint64_t div;
			} units[] = {{"ns", 1}, {"us", 1000}, {"ms", 1000000}, {"s", 1000000000}};
			size_t u = 0;
			while (u < 4 && !(f && !strcmp(f, units[u].n)))
				u++;
			if (u == 4)
				err(x, "outputs.mqtt.time_precision", "must be ns, us, ms or s");
			else
				q->time_div = units[u].div;
		} else if (!strcmp(k, "keepalive")) {
			get_num(x, v, "outputs.mqtt.keepalive", &t);
			if (t < 0 || t > 65535 || t != floor(t))
				err(x, "outputs.mqtt.keepalive",
				    "must be a whole number of seconds from 0 to 65535");
			else
				q->keepalive_s = (uint32_t)t;
		} else if (!strcmp(k, "timeout")) {
			get_num(x, v, "outputs.mqtt.timeout", &t);
			if (t < 1 || t > 300)
				err(x, "outputs.mqtt.timeout", "must be from 1 to 300 seconds");
			else
				q->timeout_ms = (uint32_t)(t * 1000);
		} else if (!strcmp(k, "batch_bytes")) {
			get_size(x, v, "outputs.mqtt.batch_bytes", &q->batch_bytes);
			q->batch_set = true;
		} else if (!strcmp(k, "max_message_bytes"))
			get_size(x, v, "outputs.mqtt.max_message_bytes", &q->max_message_bytes);
		else
			RTC_WARN(C, "outputs.mqtt.%s: unknown key, ignored", k);
	}
}

static void add_input(ctx *x, rtc_config *c, const char *name)
{
	(void)x;
	if (!rtc_input_find(name)) {
		RTC_WARN(C, "inputs: %s is not available in this build, skipped", name);
		return;
	}
	for (unsigned i = 0; i < c->ninputs; i++)
		if (!strcmp(c->inputs[i], name))
			return;
	if (c->ninputs == RTC_CFG_MAX_INPUTS)
		return;
	snprintf(c->inputs[c->ninputs++], sizeof(c->inputs[0]), "%s", name);
}

static void load_doc(ctx *x, rtc_config *c)
{
	yaml_node_t *root = yaml_document_get_root_node(x->doc);
	if (!root) {
		err(x, "file", "is empty");
		return;
	}
	if (root->type != YAML_MAPPING_NODE) {
		err(x, "file", "top level must be a mapping");
		return;
	}
	for (yaml_node_pair_t *p = root->data.mapping.pairs.start; p < root->data.mapping.pairs.top; p++) {
		const char *k = scalar(node(x, p->key));
		yaml_node_t *v = node(x, p->value);
		if (!k)
			continue;
		if (!strcmp(k, "interval"))
			get_num(x, v, "interval", &c->interval_s);
		else if (!strcmp(k, "flush_interval"))
			get_num(x, v, "flush_interval", &c->flush_interval_s);
		else if (!strcmp(k, "collect_priority") || !strcmp(k, "send_priority") ||
		         !strcmp(k, "processes_every") || !strcmp(k, "max_processes")) {
			double t = 0;
			get_num(x, v, k, &t);
			if (t != floor(t))
				err(x, k, "must be an integer");
			else if (!strcmp(k, "collect_priority"))
				c->collect_priority =
				    t >= 0 && t <= 1000 ? (int)t : -1; /* range-checked in validate */
			else if (!strcmp(k, "send_priority"))
				c->send_priority = t >= 0 && t <= 1000 ? (int)t : -1;
			else if (!strcmp(k, "processes_every"))
				c->processes_every = t >= 0 && t <= 1000000 ? (unsigned)t : 0;
			else
				c->max_processes = t >= 0 && t <= 1e9 ? (uint64_t)t : 0;
		} else if (!strcmp(k, "rmgr_path"))
			get_str(x, v, "rmgr_path", c->rmgr_path, sizeof(c->rmgr_path));
		else if (!strcmp(k, "rmgr_mode")) {
			const char *m = scalar(v);
			char *end = NULL;
			long mode = m ? strtol(m, &end, 8) : -1;
			if (!m || end == m || *end || mode < 0 || mode > 0777)
				err(x, "rmgr_mode", "must be octal permission bits, such as 0660");
			else
				c->rmgr_mode = (unsigned)mode;
		} else if (!strcmp(k, "rmgr_priority")) {
			double t = 0;
			get_num(x, v, "rmgr_priority", &t);
			if (t < 1 || t > 253 || t != floor(t))
				err(x, "rmgr_priority", "must be an integer from 1 to 253");
			else
				c->rmgr_priority = (int)t;
		} else if (!strcmp(k, "rmgr_threads")) {
			double t = 0;
			get_num(x, v, "rmgr_threads", &t);
			if (t < 1 || t > 16 || t != floor(t))
				err(x, "rmgr_threads", "must be an integer from 1 to 16");
			else
				c->rmgr_threads = (unsigned)t;
		} else if (!strcmp(k, "trace_interval")) {
			get_num(x, v, "trace_interval", &c->trace_interval_s);
		} else if (!strcmp(k, "trace_max_bytes")) {
			get_size(x, v, "trace_max_bytes", &c->trace_max_bytes);
		} else if (!strcmp(k, "trace_priority") || !strcmp(k, "trace_min_priority") ||
		           !strcmp(k, "trace_top") || !strcmp(k, "trace_max_rows") ||
		           !strcmp(k, "trace_max_threads")) {
			double t = -1;
			get_num(x, v, k, &t);
			if (t < 0 || t > 1000000 || t != floor(t))
				err(x, k, "must be a non-negative integer");
			else if (!strcmp(k, "trace_priority"))
				c->trace_priority = (int)t;
			else if (!strcmp(k, "trace_min_priority"))
				c->trace_min_priority = (int)t;
			else if (!strcmp(k, "trace_top"))
				c->trace_top = (unsigned)t;
			else if (!strcmp(k, "trace_max_rows"))
				c->trace_max_rows = (unsigned)t;
			else
				c->trace_max_threads = (unsigned)t;
		} else if (!strcmp(k, "rmgr_group")) {
			double t = -1;
			get_num(x, v, "rmgr_group", &t);
			if (t < 0 || t > 65535 || t != floor(t))
				err(x, "rmgr_group", "must be a group id from 0 to 65535");
			else
				c->rmgr_group = (int)t;
		} else if (!strcmp(k, "rmgr_buffer_bytes"))
			get_size(x, v, "rmgr_buffer_bytes", &c->rmgr_buffer_bytes);
		else if (!strcmp(k, "max_write_bytes"))
			get_size(x, v, "max_write_bytes", &c->max_write_bytes);
		else if (!strcmp(k, "rmgr_drain_bytes"))
			get_size(x, v, "rmgr_drain_bytes", &c->rmgr_drain_bytes);
		else if (!strcmp(k, "shutdown_timeout"))
			get_num(x, v, "shutdown_timeout", &c->shutdown_timeout_s);
		else if (!strcmp(k, "buffer_bytes"))
			get_size(x, v, "buffer_bytes", &c->buffer_bytes);
		else if (!strcmp(k, "max_record_bytes")) {
			size_t s = 0;
			get_size(x, v, "max_record_bytes", &s);
			if (s > (1u << 20))
				err(x, "max_record_bytes", "must be at most 1MiB");
			else
				c->max_record_bytes = (uint32_t)s;
		} else if (!strcmp(k, "max_buffer_size"))
			RTC_WARN(
			    C, "max_buffer_size is ignored: rtcollector counted single-value series, "
			       "this collector buffers multi-field rows; size the buffer with buffer_bytes");
		else if (!strcmp(k, "warn_on_buffer"))
			get_bool(x, v, "warn_on_buffer", &c->warn_on_buffer);
		else if (!strcmp(k, "debug"))
			get_bool(x, v, "debug", &c->debug);
		else if (!strcmp(k, "once"))
			get_bool(x, v, "once", &c->once);
		else if (!strcmp(k, "hostname"))
			get_str(x, v, "hostname", c->hostname, sizeof(c->hostname));
		else if (!strcmp(k, "tags")) {
			if (!v || v->type != YAML_MAPPING_NODE) {
				if (!(scalar(v) && !scalar(v)[0]))
					err(x, "tags", "must be a mapping");
				continue;
			}
			for (yaml_node_pair_t *t = v->data.mapping.pairs.start; t < v->data.mapping.pairs.top;
			     t++) {
				const char *tk = scalar(node(x, t->key)), *tv = scalar(node(x, t->value));
				if (!tk || !tv) {
					err(x, "tags", "keys and values must be strings");
					continue;
				}
				if (c->ntags == RTC_CFG_MAX_TAGS) {
					err(x, "tags", "too many (max 16)");
					break;
				}
				rtc_kv *kv = &c->tags[c->ntags++];
				get_str(x, node(x, t->key), "tags", kv->key, sizeof(kv->key));
				get_str(x, node(x, t->value), "tags", kv->val, sizeof(kv->val));
			}
		} else if (!strcmp(k, "inputs")) {
			if (!v || v->type != YAML_SEQUENCE_NODE) {
				err(x, "inputs", "must be a list");
				continue;
			}
			for (yaml_node_item_t *i = v->data.sequence.items.start;
			     i < v->data.sequence.items.top; i++) {
				yaml_node_t *in = node(x, *i);
				if (scalar(in))
					add_input(x, c, scalar(in));
				else if (in && in->type == YAML_MAPPING_NODE &&
				         in->data.mapping.pairs.start < in->data.mapping.pairs.top) {
					const char *n = scalar(node(x, in->data.mapping.pairs.start->key));
					if (n)
						add_input(x, c, n);
				}
			}
		} else if (!strcmp(k, "outputs")) {
			if (!v || v->type != YAML_SEQUENCE_NODE) {
				err(x, "outputs", "must be a list");
				continue;
			}
			for (yaml_node_item_t *i = v->data.sequence.items.start;
			     i < v->data.sequence.items.top; i++) {
				yaml_node_t *out = node(x, *i);
				const char *name = NULL;
				yaml_node_t *settings = NULL;
				if (scalar(out))
					name = scalar(out);
				else if (out && out->type == YAML_MAPPING_NODE &&
				         out->data.mapping.pairs.start < out->data.mapping.pairs.top) {
					name = scalar(node(x, out->data.mapping.pairs.start->key));
					settings = node(x, out->data.mapping.pairs.start->value);
				}
				if (!name)
					continue;
				if (!strcmp(name, "arc"))
					load_arc(x, c, settings);
				else if (!strcmp(name, "stdout"))
					load_stdout(x, c, settings);
				else if (!strcmp(name, "mqtt"))
					load_mqtt(x, c, settings);
				else
					RTC_WARN(C, "outputs: %s is not available in this build, skipped",
					         name);
			}
		} else
			RTC_WARN(C, "%s: unknown key, ignored", k);
	}
}

static int load_parser(rtc_config *c, yaml_parser_t *parser)
{
	yaml_document_t doc;
	if (!yaml_parser_load(parser, &doc)) {
		RTC_ERROR(C, "YAML error at line %zu: %s", parser->problem_mark.line + 1,
		          parser->problem ? parser->problem : "unknown");
		return -1;
	}
	ctx x = {&doc, 0};
	load_doc(&x, c);
	yaml_document_delete(&doc);
	if (x.errors)
		return -1;
	return rtc_config_validate(c);
}

int rtc_config_load_string(rtc_config *c, const char *yaml, size_t len)
{
	yaml_parser_t parser;
	if (!yaml_parser_initialize(&parser))
		return -1;
	yaml_parser_set_input_string(&parser, (const unsigned char *)yaml, len);
	int rc = load_parser(c, &parser);
	yaml_parser_delete(&parser);
	return rc;
}

int rtc_config_load(rtc_config *c, const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		RTC_ERROR(C, "cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	yaml_parser_t parser;
	if (!yaml_parser_initialize(&parser)) {
		fclose(f);
		return -1;
	}
	yaml_parser_set_input_file(&parser, f);
	int rc = load_parser(c, &parser);
	yaml_parser_delete(&parser);
	fclose(f);
	return rc;
}

/* mqtt://host[:port] or mqtt://[v6][:port]; nothing else. */
static int parse_mqtt_url(rtc_mqtt_cfg *q)
{
	const char *u = q->url;
	if (!strncmp(u, "mqtts://", 8)) {
		RTC_ERROR(C, "outputs.mqtt.url: TLS (mqtts://) is not in this build yet");
		return 1;
	}
	if (strncmp(u, "mqtt://", 7)) {
		RTC_ERROR(C, "outputs.mqtt.url: must start with mqtt://");
		return 1;
	}
	const char *h = u + 7, *he, *rest;
	if (*h == '[') {
		he = strchr(h, ']');
		if (!he) {
			RTC_ERROR(C, "outputs.mqtt.url: unclosed [ in the address");
			return 1;
		}
		h++;
		rest = he + 1;
	} else {
		he = h + strcspn(h, ":/?#@");
		rest = he;
	}
	if (he == h || (size_t)(he - h) >= sizeof(q->host) || strpbrk(u + 7, "@/?#") ||
	    (*rest && *rest != ':')) {
		RTC_ERROR(C, "outputs.mqtt.url: must be mqtt://host or mqtt://host:port (no user, path or "
		             "query; set username and password as keys)");
		return 1;
	}
	snprintf(q->host, sizeof(q->host), "%.*s", (int)(he - h), h);
	snprintf(q->port, sizeof(q->port), "1883");
	if (*rest == ':') {
		char *end;
		errno = 0;
		long port = strtol(rest + 1, &end, 10);
		if (end == rest + 1 || *end || errno || port < 1 || port > 65535) {
			RTC_ERROR(C, "outputs.mqtt.url: the port must be from 1 to 65535");
			return 1;
		}
		snprintf(q->port, sizeof(q->port), "%ld", port);
	}
	return 0;
}

/* An expanded topic: errors for what a broker refuses, warnings for what some
 * brokers do (AWS IoT: 7 slashes, 256 bytes). */
static int check_topic(const char *key, const char *t, size_t len_worst)
{
	size_t n = strlen(t);
	int bad = 0;
	if (!n || !rtc_utf8_valid(t, n) || strpbrk(t, "+#") || t[0] == '$') {
		RTC_ERROR(C, "%s: \"%s\" must be non-empty UTF-8 without + or # and not start with $", key,
		          t);
		bad++;
	}
	if (len_worst > RTC_MQTT_TOPIC_MAX) {
		RTC_ERROR(C, "%s: up to %zu bytes with a 128-byte measurement; at most %d", key, len_worst,
		          RTC_MQTT_TOPIC_MAX);
		bad++;
	}
	if (bad)
		return bad;
	unsigned slashes = 0;
	for (size_t i = 0; i < n; i++)
		slashes += t[i] == '/';
	if (t[0] == '/' || t[n - 1] == '/' || strstr(t, "//"))
		RTC_WARN(C, "%s: \"%s\" has an empty level", key, t);
	if (slashes > 7 || len_worst > 256)
		RTC_WARN(C,
		         "%s: \"%s\" is deeper than 8 levels or longer than 256 bytes, which some "
		         "brokers (AWS IoT) refuse",
		         key, t);
	return 0;
}

static int validate_mqtt(rtc_config *c)
{
	rtc_mqtt_cfg *q = &c->mqtt;
	int bad = parse_mqtt_url(q);
	if (!c->hostname[0] && gethostname(c->hostname, sizeof(c->hostname) - 1) != 0)
		snprintf(c->hostname, sizeof(c->hostname), "unknown");
	const char *host = c->hostname;
	for (unsigned i = 0; i < c->ntags; i++)
		if (!strcmp(c->tags[i].key, "host"))
			host = c->tags[i].val;

	char m128[129], t[RTC_MQTT_TOPIC_MAX * 2], tw[RTC_MQTT_TOPIC_MAX * 2];
	memset(m128, 'm', 128);
	m128[128] = '\0';
	const char *mp = strstr(q->topic, "{measurement}");
	if (mp && strstr(mp + 1, "{measurement}")) {
		RTC_ERROR(C, "outputs.mqtt.topic: {measurement} may appear once");
		bad++;
	} else if (rtc_mqtt_topic_expand(q->topic, host, "m", 1, t, sizeof(t)) < 0 ||
	           rtc_mqtt_topic_expand(q->topic, host, m128, 128, tw, sizeof(tw)) < 0) {
		RTC_ERROR(C,
		          "outputs.mqtt.topic: only {host} and {measurement} may be used, within %d "
		          "bytes",
		          RTC_MQTT_TOPIC_MAX);
		bad++;
	} else
		bad += check_topic("outputs.mqtt.topic", t, strlen(tw));
	if (q->status_topic[0]) {
		char s[RTC_MQTT_TOPIC_MAX * 2];
		if (rtc_mqtt_topic_expand(q->status_topic, host, NULL, 0, s, sizeof(s)) < 0) {
			RTC_ERROR(C, "outputs.mqtt.status_topic: only {host} may be used, within %d bytes",
			          RTC_MQTT_TOPIC_MAX);
			bad++;
		} else if (!(bad += check_topic("outputs.mqtt.status_topic", s, strlen(s)))) {
			/* Must not be the data topic of any measurement. */
			char pre[RTC_MQTT_TOPIC_MAX * 2], post[RTC_MQTT_TOPIC_MAX * 2];
			bool clash = false;
			if (!mp)
				clash =
				    rtc_mqtt_topic_expand(q->topic, host, NULL, 0, pre, sizeof(pre)) >= 0 &&
				    !strcmp(pre, s);
			else {
				char tmpl[RTC_MQTT_TOPIC_MAX + 1];
				snprintf(tmpl, sizeof(tmpl), "%.*s", (int)(mp - q->topic), q->topic);
				int pl = rtc_mqtt_topic_expand(tmpl, host, NULL, 0, pre, sizeof(pre));
				int ql = rtc_mqtt_topic_expand(mp + 13, host, NULL, 0, post, sizeof(post));
				size_t sl = strlen(s);
				if (pl >= 0 && ql >= 0 && sl > (size_t)(pl + ql) &&
				    !strncmp(s, pre, (size_t)pl) && !strcmp(s + sl - (size_t)ql, post))
					clash = rtc_valid_arc_name(s + pl, sl - (size_t)pl - (size_t)ql, 128);
			}
			if (clash) {
				RTC_ERROR(
				    C,
				    "outputs.mqtt.status_topic: \"%s\" is also a data topic; use another "
				    "depth or prefix",
				    s);
				bad++;
			}
		}
	}

	/* Worst case for one row: JSON escapes a byte into six; the injected
	 * tags (up to 16 x 384 bytes); keys and punctuation; the PUBLISH header. */
	size_t worst =
	    6 * (size_t)c->max_record_bytes + 6 * (RTC_CFG_MAX_TAGS * 384) + 256 + RTC_MQTT_TOPIC_MAX + 9;
	if (q->max_message_bytes < 1024 || q->max_message_bytes > (256u << 20) ||
	    q->max_message_bytes < worst) {
		RTC_ERROR(C,
		          "outputs.mqtt.max_message_bytes: must be from 1KiB to 256MiB and at least %zu "
		          "(6 x max_record_bytes + 6 x 16 x 384 + %d)",
		          worst, 256 + RTC_MQTT_TOPIC_MAX + 9);
		bad++;
	}
	if (q->batch_bytes < c->max_record_bytes || q->batch_bytes > c->buffer_bytes) {
		RTC_ERROR(C, "outputs.mqtt.batch_bytes: must be between max_record_bytes and buffer_bytes");
		bad++;
	}
	const char *env = getenv("RTC_MQTT_PASSWORD");
	if (env && *env) {
		if (strlen(env) >= sizeof(q->password)) {
			RTC_ERROR(C, "RTC_MQTT_PASSWORD: is too long");
			bad++;
		} else
			snprintf(q->password, sizeof(q->password), "%s", env);
	} else if (!strncmp(q->password, "secret:", 7)) {
		RTC_ERROR(C, "outputs.mqtt.password: secret: references are not supported; set "
		             "RTC_MQTT_PASSWORD");
		bad++;
	}
	if (q->password[0] && !q->username[0]) {
		RTC_ERROR(C, "outputs.mqtt.password: needs a username (MQTT 3.1.1)");
		bad++;
	}
	if (q->client_id[0]) {
		bool plain = strlen(q->client_id) <= 23;
		for (const char *p = q->client_id; *p && plain; p++)
			plain = isalnum((unsigned char)*p);
		if (!plain)
			RTC_WARN(C, "outputs.mqtt.client_id: over 23 bytes or not only letters and digits; "
			            "some brokers refuse it");
		if (!rtc_utf8_valid(q->client_id, strlen(q->client_id))) {
			RTC_ERROR(C, "outputs.mqtt.client_id: must be UTF-8");
			bad++;
		}
	}
	return bad;
}

int rtc_config_validate(rtc_config *c)
{
	int bad = 0;
	if (!(c->interval_s >= 0.001) || c->interval_s > 86400) {
		RTC_ERROR(C, "interval: must be from 0.001 to 86400 seconds");
		bad++;
	}
	if (c->flush_interval_s < 0)
		c->flush_interval_s = c->interval_s;
	else if (c->flush_interval_s < 0.001 || c->flush_interval_s > 86400) {
		RTC_ERROR(C, "flush_interval: must be from 0.001 to 86400 seconds");
		bad++;
	} else if (c->flush_interval_s < c->interval_s)
		RTC_WARN(
		    C,
		    "flush_interval (%g) is shorter than interval (%g): sends happen at most once per round",
		    c->flush_interval_s, c->interval_s);
	if (!(c->shutdown_timeout_s >= 0) || c->shutdown_timeout_s > 600) {
		RTC_ERROR(C, "shutdown_timeout: must be from 0 to 600 seconds");
		bad++;
	}
	if (c->max_record_bytes < 512) {
		RTC_ERROR(C, "max_record_bytes: must be at least 512 (the internal input's records need it)");
		bad++;
	}
	if (c->buffer_bytes < (size_t)RTC_RING_HDR + c->max_record_bytes) {
		RTC_ERROR(C, "buffer_bytes: must hold at least one record of max_record_bytes");
		bad++;
	}
	if (!c->arc.enabled && !c->out_stdout.enabled && !c->mqtt.enabled) {
		RTC_ERROR(C, "outputs: at least one output (arc, mqtt or stdout) is required");
		bad++;
	}
	if (c->collect_priority < 1 || c->collect_priority > 253 || c->send_priority < 1 ||
	    c->send_priority > 253) {
		RTC_ERROR(C, "collect_priority, send_priority: must be from 1 to 253");
		bad++;
	}
	if (c->processes_every < 1 || c->processes_every > 1000000) {
		RTC_ERROR(C, "processes_every: must be from 1 to 1000000");
		bad++;
	}
	if (c->max_write_bytes < 64 || c->max_write_bytes > (64u << 10)) {
		RTC_ERROR(C, "max_write_bytes: must be from 64 to 64KiB");
		bad++;
	}
	if (c->rmgr_buffer_bytes < RTC_RING_HDR + c->max_write_bytes + 1) {
		RTC_ERROR(C, "rmgr_buffer_bytes: must hold at least one write of max_write_bytes");
		bad++;
	}
	if (c->rmgr_drain_bytes < c->max_write_bytes + 1 || c->rmgr_drain_bytes > (16u << 20)) {
		RTC_ERROR(C, "rmgr_drain_bytes: must be from max_write_bytes to 16MiB");
		bad++;
	}
	if (c->trace_interval_s == 0) /* one per collection round, from 1 s to 3600 s */
		c->trace_interval_s = c->interval_s < 1 ? 1 : c->interval_s > 3600 ? 3600 : c->interval_s;
	if (c->trace_interval_s < 1 || c->trace_interval_s > 3600) {
		RTC_ERROR(C, "trace_interval: must be from 1 to 3600 seconds");
		bad++;
	}
	if (c->trace_priority < 1 || c->trace_priority > 253) {
		RTC_ERROR(C, "trace_priority: must be from 1 to 253");
		bad++;
	}
	if (c->trace_min_priority > 255) {
		RTC_ERROR(C, "trace_min_priority: must be from 0 to 255");
		bad++;
	}
	if (c->trace_top > 256) {
		RTC_ERROR(C, "trace_top: must be from 0 to 256");
		bad++;
	}
	if (c->trace_max_rows < 1 || c->trace_max_rows > 4096) {
		RTC_ERROR(C, "trace_max_rows: must be from 1 to 4096");
		bad++;
	}
	if (c->trace_max_threads < 16 || c->trace_max_threads > 16384) {
		RTC_ERROR(C, "trace_max_threads: must be from 16 to 16384");
		bad++;
	}
	if (c->trace_max_bytes < (64u << 10) || c->trace_max_bytes > (64u << 20)) {
		RTC_ERROR(C, "trace_max_bytes: must be from 64KiB to 64MiB");
		bad++;
	}
	if (c->rmgr_path[0] != '/') {
		RTC_ERROR(C, "rmgr_path: must be an absolute path");
		bad++;
	}
	if (c->max_processes < 1 || c->max_processes > 1000000) {
		RTC_ERROR(C, "max_processes: must be from 1 to 1000000");
		bad++;
	}
	/* A default batch larger than a small buffer is clamped, rather than an
	 * error about a key the operator never set. */
	size_t half = c->buffer_bytes / 2 > c->max_record_bytes ? c->buffer_bytes / 2 : c->buffer_bytes;
	if (!c->arc.batch_set && c->arc.batch_bytes > half)
		c->arc.batch_bytes = half;
	if (!c->out_stdout.batch_set && c->out_stdout.batch_bytes > half)
		c->out_stdout.batch_bytes = half;
	if (!c->mqtt.batch_set && c->mqtt.batch_bytes > half)
		c->mqtt.batch_bytes = half;
	if (c->mqtt.enabled)
		bad += validate_mqtt(c);
	if (c->arc.enabled && !strncmp(c->arc.token, "secret:", 7) && !getenv("RTC_ARC_TOKEN")) {
		RTC_ERROR(C, "outputs.arc.token: secret: references are not supported; set RTC_ARC_TOKEN");
		bad++;
	}
	if (c->arc.enabled) {
		if (strncmp(c->arc.url, "http://", 7) && strncmp(c->arc.url, "https://", 8)) {
			RTC_ERROR(C, "outputs.arc.url: must start with http:// or https://");
			bad++;
		}
		if (c->arc.ca_file[0] && access(c->arc.ca_file, R_OK) != 0) {
			RTC_ERROR(C, "outputs.arc.ca_file: cannot read %s: %s", c->arc.ca_file,
			          strerror(errno));
			bad++;
		}
		if (!rtc_valid_arc_name(c->arc.database, strlen(c->arc.database), 64)) {
			RTC_ERROR(C,
			          "outputs.arc.database: must start with a letter and contain only letters, "
			          "digits, _ or - (max 64)");
			bad++;
		}
		if (c->arc.batch_bytes < c->max_record_bytes || c->arc.batch_bytes > c->buffer_bytes) {
			RTC_ERROR(
			    C, "outputs.arc.batch_bytes: must be between max_record_bytes and buffer_bytes");
			bad++;
		}
		const char *env = getenv("RTC_ARC_TOKEN");
		if (env && *env) {
			if (strlen(env) >= sizeof(c->arc.token)) {
				RTC_ERROR(C, "RTC_ARC_TOKEN: is too long");
				bad++;
			} else
				snprintf(c->arc.token, sizeof(c->arc.token), "%s", env);
		}
	}
	if (c->out_stdout.enabled && (c->out_stdout.batch_bytes < c->max_record_bytes ||
	                              c->out_stdout.batch_bytes > c->buffer_bytes)) {
		RTC_ERROR(C, "outputs.stdout.batch_bytes: must be between max_record_bytes and buffer_bytes");
		bad++;
	}
	for (unsigned i = 0; i < c->ntags; i++) {
		const char *k = c->tags[i].key;
		bool time_alike = strlen(k) == 4 && strncasecmp(k, "time", 4) == 0;
		if (strcasecmp(k, "host") == 0 && strcmp(k, "host") != 0) {
			RTC_ERROR(C, "tags: \"%s\" differs from the host tag only in letter case", k);
			bad++;
		}
		if (!k[0] || time_alike) {
			RTC_ERROR(C, "tags: \"%s\" cannot be a tag name (time is the timestamp)", k);
			bad++;
		}
		for (unsigned j = 0; j < i; j++)
			if (strcasecmp(k, c->tags[j].key) == 0) {
				RTC_ERROR(C, "tags: \"%s\" and \"%s\" differ only in letter case",
				          c->tags[j].key, k);
				bad++;
			}
	}

	/* Like rtcollector, the internal input is always on: its drop counters
	 * are how an operator sees that the collector stayed out of the way. */
	bool have_internal = false;
	for (unsigned i = 0; i < c->ninputs; i++)
		if (!strcmp(c->inputs[i], "internal"))
			have_internal = true;
	if (!have_internal && c->ninputs < RTC_CFG_MAX_INPUTS)
		snprintf(c->inputs[c->ninputs++], sizeof(c->inputs[0]), "internal");
	return bad ? -1 : 0;
}
