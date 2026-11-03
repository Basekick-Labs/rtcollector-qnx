/* arc: columnar MessagePack to Arc, zstd-compressed, over HTTP (libcurl).
 *
 * Status policy (Arc's answers, internal/api/msgpack.go and partial_write.go):
 *   2xx                         written: done
 *   400 "partial write: N ..."  the other points were written: never resend
 *   400 other                   nothing written, the request is refused:
 *                               pre-validation should prevent it; drop, count
 *   401 / 403                   credentials: keep the data, back off
 *   413                         too large: halve the records per request
 *   500 + written_measurements  stopped part-way: resend only the measurements
 *                               not listed, at most once more
 *   other 5xx, network errors   nothing written: resend with backoff
 *   504, timeouts               outcome unknown: resend (duplicates possible;
 *                               a drop is worse than a duplicate)
 */
#include "../core/collector.h"
#include "../core/log.h"
#include "arc_encode.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#define RESP_CAP 8192

typedef struct {
	CURL *curl;
	struct curl_slist *headers;
	char url[600];
	ZSTD_CCtx *zctx;
	uint8_t *mp;
	size_t mp_cap;
	uint8_t *z;
	size_t z_cap;
	uint8_t *verdict;
	char resp[RESP_CAP];
	size_t resp_len;
	rtc_arc_enc enc;

	/* accept(): distinct measurements in the batch being read */
	rtc_str seen[1024];
	uint32_t nseen;

	unsigned partial_resends;
	bool auth_logged;
	long last_logged_status;
} arc_impl;

static size_t on_body(char *p, size_t size, size_t n, void *arg)
{
	arc_impl *a = arg;
	size_t len = size * n;
	size_t room = RESP_CAP - 1 - a->resp_len;
	size_t take = len < room ? len : room;
	memcpy(a->resp + a->resp_len, p, take);
	a->resp_len += take;
	a->resp[a->resp_len] = '\0';
	return len;
}

static void batch_begin(rtc_output *o)
{
	arc_impl *a = o->impl;
	a->nseen = 0;
	a->partial_resends = 0;
}

/* Keep at most max_measurements distinct measurements in one batch, so every
 * measurement is one item and Arc's written_measurements is unambiguous. */
static bool accept_rec(void *arg, const rtc_ring_rec *rec)
{
	rtc_output *o = arg;
	arc_impl *a = o->impl;
	if (rec->len < 3 || rec->payload[0] != RTC_REC_BIN)
		return true; /* malformed: let the encoder reject and count it */
	uint16_t ml = (uint16_t)(rec->payload[1] | (rec->payload[2] << 8));
	if ((size_t)ml + 3 > rec->len)
		return true;
	const char *m = (const char *)rec->payload + 3;
	for (uint32_t i = 0; i < a->nseen; i++)
		if (a->seen[i].len == ml && !memcmp(a->seen[i].p, m, ml))
			return true;
	if (a->nseen == o->collector->cfg.arc.max_measurements)
		return false;
	a->seen[a->nseen++] = (rtc_str){m, ml};
	return true;
}

static long post(rtc_output *o, const uint8_t *body, size_t len, uint32_t timeout_ms, CURLcode *cc)
{
	arc_impl *a = o->impl;
	a->resp_len = 0;
	a->resp[0] = '\0';
	curl_easy_setopt(a->curl, CURLOPT_POSTFIELDS, (const char *)body);
	curl_easy_setopt(a->curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)len);
	curl_easy_setopt(a->curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
	rtc_counter_add(&o->counters.requests, 1);
	*cc = curl_easy_perform(a->curl);
	long status = 0;
	if (*cc == CURLE_OK)
		curl_easy_getinfo(a->curl, CURLINFO_RESPONSE_CODE, &status);
	rtc_counter_set(&o->counters.last_status, (uint64_t)status);
	return status;
}

/* Parses "partial write: N points dropped" from an Arc error body. */
static uint64_t partial_points(const char *body)
{
	const char *p = strstr(body, "partial write: ");
	if (!p)
		return 0;
	return strtoull(p + 15, NULL, 10);
}

/* Is measurement m listed in the body's "written_measurements": [...]? Arc's
 * measurement names match ^[a-zA-Z][a-zA-Z0-9_-]*$, so no escapes occur. */
static bool written(const char *body, const rtc_str *m)
{
	const char *p = strstr(body, "\"written_measurements\"");
	if (!p || !(p = strchr(p, '[')))
		return false;
	const char *end = strchr(p, ']');
	if (!end)
		return false;
	while ((p = strchr(p, '"')) && p < end) {
		const char *q = strchr(p + 1, '"');
		if (!q || q > end)
			break;
		if ((size_t)(q - p - 1) == m->len && !memcmp(p + 1, m->p, m->len))
			return true;
		p = q + 1;
	}
	return false;
}

static void log_status(rtc_output *o, long status, const char *what)
{
	arc_impl *a = o->impl;
	if (a->last_logged_status == status)
		return;
	a->last_logged_status = status;
	RTC_WARN(o->name, "%s (HTTP %ld): %.300s", what, status, a->resp);
}

static size_t remaining(const rtc_output *o)
{
	size_t n = 0;
	for (size_t i = 0; i < o->nrecs; i++)
		n += !o->done[i];
	return n;
}

static enum rtc_send_outcome send_batch(rtc_output *o, uint32_t timeout_cap_ms)
{
	arc_impl *a = o->impl;
	rtc_collector *c = o->collector;
	size_t first = 0;
	while (first < o->nrecs && o->done[first])
		first++;
	if (first == o->nrecs)
		return RTC_SEND_DONE;
	size_t n = o->nrecs - first < o->limit ? o->nrecs - first : o->limit;

	size_t len = rtc_arc_encode(&a->enc, &c->cfg, c->cfg.hostname, o->recs, o->done, first, n, a->verdict,
	                            &o->counters, a->mp, a->mp_cap);
	for (size_t i = first; i < first + n; i++)
		if (a->verdict[i] == RTC_V_REJECTED) {
			o->done[i] = 1;
			rtc_counter_add(&o->counters.records_rejected, 1);
		}
	if (len == SIZE_MAX) {
		o->limit = n > 1 ? n / 2 : 1;
		if (n == 1) { /* one record that cannot be encoded in mp_cap */
			o->done[first] = 1;
			rtc_counter_add(&o->counters.records_rejected, 1);
		}
		return remaining(o) ? RTC_SEND_AGAIN : RTC_SEND_DONE;
	}
	if (len == 0)
		return remaining(o) ? RTC_SEND_AGAIN : RTC_SEND_DONE;

	size_t zl = ZSTD_compress2(a->zctx, a->z, a->z_cap, a->mp, len);
	if (ZSTD_isError(zl)) {
		RTC_ERROR(o->name, "zstd: %s", ZSTD_getErrorName(zl));
		return RTC_SEND_RETRY;
	}

	uint32_t timeout = c->cfg.arc.timeout_ms;
	if (timeout_cap_ms && timeout_cap_ms < timeout)
		timeout = timeout_cap_ms;
	CURLcode cc;
	long status = post(o, a->z, zl, timeout, &cc);

	size_t in_req = 0;
	for (size_t i = first; i < first + n; i++)
		in_req += a->verdict[i] == RTC_V_SENT;

	if (cc != CURLE_OK) {
		rtc_counter_add(&o->counters.request_errors, 1);
		if (cc == CURLE_OPERATION_TIMEDOUT && timeout_cap_ms &&
		    timeout_cap_ms <= c->cfg.arc.timeout_ms) {
			/* Cut off by the shutdown budget: Arc may have written it. */
			for (size_t i = first; i < first + n; i++)
				if (a->verdict[i] == RTC_V_SENT)
					o->done[i] = 1;
			rtc_counter_add(&o->counters.records_outcome_unknown, in_req);
			RTC_WARN(o->name,
			         "shutdown: a request was cut off; %zu records may or may not be stored",
			         in_req);
			return remaining(o) ? RTC_SEND_AGAIN : RTC_SEND_DONE;
		}
		if (a->last_logged_status != -(long)cc) {
			a->last_logged_status = -(long)cc;
			RTC_WARN(o->name, "cannot reach Arc at %s: %s; retrying", a->url,
			         curl_easy_strerror(cc));
		}
		return RTC_SEND_RETRY;
	}

	if (status >= 200 && status < 300) {
		for (size_t i = first; i < first + n; i++)
			if (a->verdict[i] == RTC_V_SENT)
				o->done[i] = 1;
		rtc_counter_add(&o->counters.records_sent, in_req);
		a->auth_logged = false;
		a->last_logged_status = 0;
		o->limit = o->nrecs; /* allow full requests again after a split */
		return remaining(o) ? RTC_SEND_AGAIN : RTC_SEND_DONE;
	}

	rtc_counter_add(&o->counters.request_errors, 1);
	if (status == 400) {
		for (size_t i = first; i < first + n; i++)
			if (a->verdict[i] == RTC_V_SENT)
				o->done[i] = 1;
		uint64_t pts = partial_points(a->resp);
		if (pts) {
			rtc_counter_add(&o->counters.points_dropped_partial, pts);
			rtc_counter_add(&o->counters.records_sent, in_req); /* the others were written */
			log_status(o, status, "Arc dropped some points and wrote the rest");
		} else {
			rtc_counter_add(&o->counters.records_rejected, in_req);
			log_status(o, status, "Arc refused the request; its records are dropped");
		}
		return remaining(o) ? RTC_SEND_AGAIN : RTC_SEND_DONE;
	}
	if (status == 401 || status == 403) {
		if (!a->auth_logged) {
			a->auth_logged = true;
			RTC_ERROR(o->name,
			          "Arc refused the token (HTTP %ld); keeping the data and retrying: %.300s",
			          status, a->resp);
		}
		return RTC_SEND_RETRY;
	}
	if (status == 413) {
		if (n == 1) {
			o->done[first] = 1;
			rtc_counter_add(&o->counters.records_rejected, 1);
		}
		o->limit = n > 1 ? n / 2 : 1;
		log_status(o, status, "request too large; sending smaller requests");
		return remaining(o) ? RTC_SEND_AGAIN : RTC_SEND_DONE;
	}
	if (status == 500 && strstr(a->resp, "\"written_measurements\"")) {
		size_t wrote = 0;
		for (size_t i = first; i < first + n; i++) {
			if (a->verdict[i] != RTC_V_SENT)
				continue;
			const rtc_str *m = rtc_arc_record_measurement(&a->enc, i);
			if (m && written(a->resp, m)) {
				o->done[i] = 1;
				wrote++;
			}
		}
		rtc_counter_add(&o->counters.records_sent, wrote);
		log_status(o, status,
		           "Arc stopped part-way; resending only the measurements it did not write");
		if (++a->partial_resends > 1) {
			size_t left = 0;
			for (size_t i = first; i < first + n; i++)
				if (a->verdict[i] == RTC_V_SENT && !o->done[i]) {
					o->done[i] = 1;
					left++;
				}
			rtc_counter_add(&o->counters.records_rejected, left);
			RTC_ERROR(o->name, "Arc stopped part-way twice; %zu records dropped", left);
			return remaining(o) ? RTC_SEND_AGAIN : RTC_SEND_DONE;
		}
		return RTC_SEND_RETRY;
	}
	log_status(o, status, "Arc did not accept the request; retrying");
	return RTC_SEND_RETRY;
}

static void destroy(rtc_output *o)
{
	arc_impl *a = o->impl;
	if (a) {
		if (a->curl)
			curl_easy_cleanup(a->curl);
		curl_slist_free_all(a->headers);
		ZSTD_freeCCtx(a->zctx);
		free(a->mp);
		free(a->z);
		free(a->verdict);
		rtc_arc_enc_free(&a->enc);
		free(a);
	}
	rtc_output_free_batch(o);
	free(o);
}

int rtc_output_arc_create(rtc_collector *c, unsigned idx, rtc_output **out)
{
	const rtc_arc_cfg *cfg = &c->cfg.arc;
	rtc_output *o = calloc(1, sizeof(*o));
	arc_impl *a = calloc(1, sizeof(*a));
	if (!o || !a) {
		free(o);
		free(a);
		return -1;
	}
	o->name = "arc";
	o->idx = idx;
	o->collector = c;
	o->impl = a;
	o->accept = accept_rec;
	o->batch_begin = batch_begin;
	o->send = send_batch;
	o->destroy = destroy;
	if (rtc_output_alloc_batch(o, cfg->batch_bytes, c->cfg.max_record_bytes) != 0)
		goto fail;

	/* Worst case body: every byte of the batch, plus a nil per missing cell
	 * (columns x rows), plus names. Over it, the request is split. */
	a->mp_cap = 2 * cfg->batch_bytes + 64 * o->max_recs + 65536;
	a->mp = malloc(a->mp_cap);
	a->z_cap = ZSTD_compressBound(a->mp_cap);
	a->z = malloc(a->z_cap);
	a->verdict = calloc(o->max_recs, 1);
	a->zctx = ZSTD_createCCtx();
	if (!a->mp || !a->z || !a->verdict || !a->zctx ||
	    rtc_arc_enc_init(&a->enc, o->max_recs, cfg->max_measurements))
		goto fail;
	ZSTD_CCtx_setParameter(a->zctx, ZSTD_c_compressionLevel, 3);

	snprintf(a->url, sizeof(a->url), "%s/api/v1/write/msgpack", cfg->url);
	size_t ul = strlen(cfg->url);
	if (ul && cfg->url[ul - 1] == '/')
		snprintf(a->url, sizeof(a->url), "%sapi/v1/write/msgpack", cfg->url);

	char h[700];
	a->headers = curl_slist_append(NULL, "Content-Type: application/msgpack");
	snprintf(h, sizeof(h), "x-arc-database: %s", cfg->database);
	a->headers = curl_slist_append(a->headers, h);
	if (cfg->token[0]) {
		snprintf(h, sizeof(h), "Authorization: Bearer %s", cfg->token);
		a->headers = curl_slist_append(a->headers, h);
	}
	a->headers = curl_slist_append(a->headers, "Expect:");
	a->curl = curl_easy_init();
	if (!a->headers || !a->curl)
		goto fail;
	curl_easy_setopt(a->curl, CURLOPT_URL, a->url);
	curl_easy_setopt(a->curl, CURLOPT_HTTPHEADER, a->headers);
	curl_easy_setopt(a->curl, CURLOPT_POST, 1L);
	curl_easy_setopt(a->curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(a->curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
	curl_easy_setopt(a->curl, CURLOPT_WRITEFUNCTION, on_body);
	curl_easy_setopt(a->curl, CURLOPT_WRITEDATA, a);
	*out = o;
	return 0;
fail:
	RTC_ERROR("arc", "cannot set up the Arc output");
	destroy(o);
	return -1;
}
