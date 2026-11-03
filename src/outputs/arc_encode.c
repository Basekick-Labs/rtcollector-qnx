#include "arc_encode.h"

#include "../core/msgpack.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int rtc_arc_enc_init(rtc_arc_enc *e, size_t max_recs, uint32_t max_groups)
{
	memset(e, 0, sizeof(*e));
	e->groups = calloc(max_groups, sizeof(*e->groups));
	e->row_group = calloc(max_recs, sizeof(*e->row_group));
	if (!e->groups || !e->row_group)
		return -1;
	e->max_groups = max_groups;
	e->max_recs = max_recs;
	return 0;
}

void rtc_arc_enc_free(rtc_arc_enc *e)
{
	free(e->groups);
	free(e->row_group);
}

bool rtc_utf8_valid(const char *s, size_t n)
{
	const unsigned char *p = (const unsigned char *)s;
	size_t i = 0;
	while (i < n) {
		unsigned char c = p[i];
		size_t need;
		uint32_t cp;
		if (c < 0x80) {
			i++;
			continue;
		} else if ((c & 0xe0) == 0xc0) {
			need = 1;
			cp = c & 0x1f;
		} else if ((c & 0xf0) == 0xe0) {
			need = 2;
			cp = c & 0x0f;
		} else if ((c & 0xf8) == 0xf0) {
			need = 3;
			cp = c & 0x07;
		} else
			return false;
		if (n - i - 1 < need)
			return false;
		for (size_t j = 1; j <= need; j++) {
			if ((p[i + j] & 0xc0) != 0x80)
				return false;
			cp = (cp << 6) | (p[i + j] & 0x3f);
		}
		if ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) ||
		    cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
			return false;
		i += need + 1;
	}
	return true;
}

static bool str_eq(rtc_str a, const char *b, size_t bl)
{
	return a.len == bl && memcmp(a.p, b, bl) == 0;
}

static bool fold_eq(const char *a, size_t al, const char *b, size_t bl)
{
	if (al != bl)
		return false;
	for (size_t i = 0; i < al; i++) {
		unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
		if (x >= 'A' && x <= 'Z')
			x = (unsigned char)(x + 32);
		if (y >= 'A' && y <= 'Z')
			y = (unsigned char)(y + 32);
		if (x != y)
			return false;
	}
	return true;
}

static bool name_ok(rtc_str s)
{
	return s.len > 0 && rtc_utf8_valid(s.p, s.len) && !fold_eq(s.p, s.len, "time", 4);
}

/* Injected tags: host, then the configured tags. They exist in every row. */
typedef struct {
	const char *k;
	size_t kl;
} inj;

static unsigned injected(const rtc_config *cfg, inj out[RTC_CFG_MAX_TAGS + 1])
{
	unsigned n = 0;
	out[n++] = (inj){"host", 4};
	for (unsigned i = 0; i < cfg->ntags; i++) {
		if (!strcmp(cfg->tags[i].key, "host")) {
			continue; /* a configured host tag replaces the hostname (see value lookup) */
		}
		out[n++] = (inj){cfg->tags[i].key, strlen(cfg->tags[i].key)};
	}
	return n;
}

static int find_injected(const inj *in, unsigned nin, rtc_str k)
{
	for (unsigned i = 0; i < nin; i++)
		if (fold_eq(k.p, k.len, in[i].k, in[i].kl))
			return (int)i;
	return -1;
}

typedef struct {
	rtc_str name;
	uint8_t is_tag;
	uint8_t type;
} pend;

/* Decide whether a row fits its group; on success commit its new columns. */
static bool admit(rtc_arc_group *g, const rtc_rec_view *v, const inj *in, unsigned nin,
                  rtc_output_counters *k, bool count_nonfinite)
{
	pend p[RTC_REC_MAX_TAGS + RTC_REC_MAX_FIELDS];
	unsigned np = 0, nfields = 0;

	for (unsigned i = 0; i < v->ntags; i++) {
		rtc_str key = v->tag_k[i];
		if (!name_ok(key)) {
			rtc_counter_add(&k->invalid_names, 1);
			return false;
		}
		int j = find_injected(in, nin, key);
		if (j >= 0) {
			if (!str_eq(key, in[j].k, in[j].kl)) {
				rtc_counter_add(&k->invalid_names, 1); /* "Host" vs "host" */
				return false;
			}
			continue; /* merges into the injected column */
		}
		p[np++] = (pend){key, 1, 0};
	}
	for (unsigned i = 0; i < v->nfields; i++) {
		const rtc_field *f = &v->fields[i];
		if (!name_ok(f->key) || find_injected(in, nin, f->key) >= 0) {
			rtc_counter_add(&k->invalid_names, 1);
			return false;
		}
		if (f->type == RTC_F_F64 && !isfinite(f->v.f)) {
			if (count_nonfinite) /* once per record, not per retry */
				rtc_counter_add(&k->fields_nonfinite, 1);
			continue;
		}
		p[np++] = (pend){f->key, 0, f->type};
		nfields++;
	}
	if (nfields == 0)
		return false; /* counted as fields_nonfinite above */

	unsigned add = 0;
	for (unsigned i = 0; i < np; i++) {
		/* Against the other new columns of this row. */
		for (unsigned j = 0; j < i; j++)
			if (fold_eq(p[i].name.p, p[i].name.len, p[j].name.p, p[j].name.len)) {
				rtc_counter_add(&k->invalid_names, 1);
				return false;
			}
		/* Against the group's columns. */
		bool found = false;
		for (unsigned c = 0; c < g->ncols; c++) {
			rtc_arc_col *col = &g->cols[c];
			if (!fold_eq(p[i].name.p, p[i].name.len, col->name.p, col->name.len))
				continue;
			if (col->name.len != p[i].name.len ||
			    memcmp(col->name.p, p[i].name.p, p[i].name.len)) {
				rtc_counter_add(&k->invalid_names, 1);
				return false;
			}
			if (col->is_tag != p[i].is_tag || (!col->is_tag && col->type != p[i].type)) {
				rtc_counter_add(&k->type_conflicts, 1);
				return false;
			}
			found = true;
			break;
		}
		if (!found) {
			p[i].type |= 0x80; /* mark new */
			add++;
		}
	}
	if (g->ncols + add > RTC_ARC_MAX_COLS - nin) {
		rtc_counter_add(&k->columns_over_limit, 1);
		return false;
	}
	for (unsigned i = 0; i < np; i++)
		if (p[i].type & 0x80)
			g->cols[g->ncols++] =
			    (rtc_arc_col){p[i].name, p[i].is_tag, (uint8_t)(p[i].type & 0x7f)};
	return true;
}

static const char *tag_value(const rtc_rec_view *v, const char *k, size_t kl, size_t *len)
{
	for (unsigned i = 0; i < v->ntags; i++)
		if (str_eq(v->tag_k[i], k, kl)) {
			*len = v->tag_v[i].len;
			return v->tag_v[i].p;
		}
	return NULL;
}

static const rtc_field *field_value(const rtc_rec_view *v, rtc_str k)
{
	for (unsigned i = 0; i < v->nfields; i++)
		if (str_eq(v->fields[i].key, k.p, k.len))
			return &v->fields[i];
	return NULL;
}

size_t rtc_arc_encode(rtc_arc_enc *e, const rtc_config *cfg, const char *hostname, const rtc_ring_rec *recs,
                      const uint8_t *done, uint8_t *flags, size_t start, size_t n, uint8_t *verdict,
                      rtc_output_counters *k, uint8_t *out, size_t cap)
{
	inj in[RTC_CFG_MAX_TAGS + 1];
	unsigned nin = injected(cfg, in);
	const char *cfg_host = NULL;
	for (unsigned i = 0; i < cfg->ntags; i++)
		if (!strcmp(cfg->tags[i].key, "host"))
			cfg_host = cfg->tags[i].val;

	e->ngroups = 0;
	size_t end = start + n;
	rtc_rec_view v;

	/* Pass 1: validate rows, build groups and their columns. */
	for (size_t i = 0; i < end && i < e->max_recs; i++) {
		e->row_group[i] = -1;
		if (i < start) {
			verdict[i] = RTC_V_SKIP;
			continue;
		}
		verdict[i] = RTC_V_SKIP;
		if (done[i])
			continue;
		if (!rtc_rec_decode(recs[i].payload, recs[i].len, &v) ||
		    !rtc_valid_arc_name(v.measurement.p, v.measurement.len, 128)) {
			rtc_counter_add(&k->invalid_names, 1);
			verdict[i] = RTC_V_REJECTED;
			continue;
		}
		uint32_t g;
		for (g = 0; g < e->ngroups; g++)
			if (str_eq(e->groups[g].m, v.measurement.p, v.measurement.len))
				break;
		if (g == e->ngroups) {
			if (e->ngroups == e->max_groups)
				continue; /* stays for the next request */
			rtc_arc_group *ng = &e->groups[e->ngroups++];
			ng->m = v.measurement;
			ng->ncols = 0;
			ng->nrows = 0;
		}
		bool first_time = !(flags[i] & RTC_ARC_FLAG_SEEN);
		flags[i] |= RTC_ARC_FLAG_SEEN;
		if (!admit(&e->groups[g], &v, in, nin, k, first_time)) {
			verdict[i] = RTC_V_REJECTED;
			continue;
		}
		e->groups[g].nrows++;
		e->row_group[i] = (int32_t)g;
		verdict[i] = RTC_V_SENT;
	}

	uint32_t live = 0;
	for (uint32_t g = 0; g < e->ngroups; g++)
		live += e->groups[g].nrows > 0;
	if (live == 0)
		return 0;

	/* Pass 2: encode. */
	rtc_mp w;
	rtc_mp_init(&w, out, cap);
	rtc_mp_array(&w, live);
	for (uint32_t g = 0; g < e->ngroups; g++) {
		rtc_arc_group *grp = &e->groups[g];
		if (grp->nrows == 0)
			continue;
		unsigned ntagcols = 0;
		for (unsigned c = 0; c < grp->ncols; c++)
			ntagcols += grp->cols[c].is_tag;

		rtc_mp_map(&w, 3);
		rtc_mp_cstr(&w, "m");
		rtc_mp_str(&w, grp->m.p, grp->m.len);
		rtc_mp_cstr(&w, "columns");
		rtc_mp_map(&w, 1 + nin + grp->ncols);

		rtc_mp_cstr(&w, "time");
		rtc_mp_array(&w, grp->nrows);
		for (size_t i = start; i < end; i++)
			if (e->row_group[i] == (int32_t)g)
				rtc_mp_int(&w, (int64_t)recs[i].ts_ns);

		for (unsigned t = 0; t < nin; t++) {
			rtc_mp_str(&w, in[t].k, in[t].kl);
			rtc_mp_array(&w, grp->nrows);
			const char *cfg_val = NULL;
			if (t == 0)
				cfg_val = cfg_host;
			else
				for (unsigned q = 0; q < cfg->ntags; q++)
					if (!strcmp(cfg->tags[q].key, in[t].k))
						cfg_val = cfg->tags[q].val;
			for (size_t i = start; i < end; i++) {
				if (e->row_group[i] != (int32_t)g)
					continue;
				if (cfg_val) { /* configured tags win, like rtcollector */
					rtc_mp_cstr(&w, cfg_val);
					continue;
				}
				size_t vl = 0;
				const char *rv = NULL;
				if (rtc_rec_decode(recs[i].payload, recs[i].len, &v))
					rv = tag_value(&v, in[t].k, in[t].kl, &vl);
				if (rv)
					rtc_mp_str(&w, rv, vl);
				else
					rtc_mp_cstr(&w, hostname); /* t == 0: the host tag */
			}
		}

		for (unsigned c = 0; c < grp->ncols; c++) {
			rtc_arc_col *col = &grp->cols[c];
			rtc_mp_str(&w, col->name.p, col->name.len);
			rtc_mp_array(&w, grp->nrows);
			for (size_t i = start; i < end; i++) {
				if (e->row_group[i] != (int32_t)g)
					continue;
				rtc_rec_decode(recs[i].payload, recs[i].len, &v);
				if (col->is_tag) {
					size_t vl;
					const char *tv = tag_value(&v, col->name.p, col->name.len, &vl);
					if (tv)
						rtc_mp_str(&w, tv, vl);
					else
						rtc_mp_nil(&w);
					continue;
				}
				const rtc_field *f = field_value(&v, col->name);
				if (!f || (f->type == RTC_F_F64 && !isfinite(f->v.f))) {
					rtc_mp_nil(&w);
					continue;
				}
				switch (f->type) {
				case RTC_F_I64:
					rtc_mp_int(&w, f->v.i);
					break;
				case RTC_F_F64:
					rtc_mp_f64(&w, f->v.f);
					break;
				case RTC_F_BOOL:
					rtc_mp_bool(&w, f->v.b);
					break;
				case RTC_F_STR:
					rtc_mp_str(&w, f->v.s.p, f->v.s.len);
					break;
				}
			}
		}

		rtc_mp_cstr(&w, "tag_keys");
		rtc_mp_array(&w, nin + ntagcols);
		for (unsigned t = 0; t < nin; t++)
			rtc_mp_str(&w, in[t].k, in[t].kl);
		for (unsigned c = 0; c < grp->ncols; c++)
			if (grp->cols[c].is_tag)
				rtc_mp_str(&w, grp->cols[c].name.p, grp->cols[c].name.len);
	}
	return rtc_mp_ok(&w) ? w.len : SIZE_MAX;
}

const rtc_str *rtc_arc_record_measurement(const rtc_arc_enc *e, size_t rec)
{
	if (rec >= e->max_recs || e->row_group[rec] < 0)
		return NULL;
	return &e->groups[e->row_group[rec]].m;
}
