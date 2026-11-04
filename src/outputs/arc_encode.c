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

static bool str_eq(rtc_str a, const char *b, size_t bl)
{
	return a.len == bl && memcmp(a.p, b, bl) == 0;
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
	rtc_row_inj in[RTC_CFG_MAX_TAGS + 1];
	unsigned nin = rtc_row_injected(cfg, in);

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
			ng->s.ncols = 0;
			ng->nrows = 0;
		}
		bool first_time = !(flags[i] & RTC_ARC_FLAG_SEEN);
		flags[i] |= RTC_ARC_FLAG_SEEN;
		if (!rtc_row_admit(&e->groups[g].s, &v, in, nin, k, first_time)) {
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
		for (unsigned c = 0; c < grp->s.ncols; c++)
			ntagcols += grp->s.cols[c].is_tag;

		rtc_mp_map(&w, 3);
		rtc_mp_cstr(&w, "m");
		rtc_mp_str(&w, grp->m.p, grp->m.len);
		rtc_mp_cstr(&w, "columns");
		rtc_mp_map(&w, 1 + nin + grp->s.ncols);

		rtc_mp_cstr(&w, "time");
		rtc_mp_array(&w, grp->nrows);
		for (size_t i = start; i < end; i++)
			if (e->row_group[i] == (int32_t)g)
				rtc_mp_int(&w, (int64_t)recs[i].ts_ns);

		for (unsigned t = 0; t < nin; t++) {
			rtc_mp_str(&w, in[t].k, in[t].kl);
			rtc_mp_array(&w, grp->nrows);
			for (size_t i = start; i < end; i++) {
				if (e->row_group[i] != (int32_t)g)
					continue;
				rtc_rec_decode(recs[i].payload, recs[i].len, &v); /* admitted: decodes */
				size_t vl;
				const char *val = rtc_row_inj_value(&in[t], &v, hostname, &vl);
				rtc_mp_str(&w, val, vl);
			}
		}

		for (unsigned c = 0; c < grp->s.ncols; c++) {
			rtc_row_col *col = &grp->s.cols[c];
			rtc_mp_str(&w, col->name.p, col->name.len);
			rtc_mp_array(&w, grp->nrows);
			for (size_t i = start; i < end; i++) {
				if (e->row_group[i] != (int32_t)g)
					continue;
				rtc_rec_decode(recs[i].payload, recs[i].len, &v);
				if (col->is_tag) {
					size_t vl;
					const char *tv = rtc_row_tag(&v, col->name.p, col->name.len, &vl);
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
		for (unsigned c = 0; c < grp->s.ncols; c++)
			if (grp->s.cols[c].is_tag)
				rtc_mp_str(&w, grp->s.cols[c].name.p, grp->s.cols[c].name.len);
	}
	return rtc_mp_ok(&w) ? w.len : SIZE_MAX;
}

const rtc_str *rtc_arc_record_measurement(const rtc_arc_enc *e, size_t rec)
{
	if (rec >= e->max_recs || e->row_group[rec] < 0)
		return NULL;
	return &e->groups[e->row_group[rec]].m;
}
