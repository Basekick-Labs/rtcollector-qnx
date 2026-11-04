#include "row.h"

#include <math.h>
#include <string.h>

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

static const char *cfg_tag(const rtc_config *cfg, const char *k)
{
	for (unsigned i = 0; i < cfg->ntags; i++)
		if (!strcmp(cfg->tags[i].key, k))
			return cfg->tags[i].val;
	return NULL;
}

unsigned rtc_row_injected(const rtc_config *cfg, rtc_row_inj out[RTC_CFG_MAX_TAGS + 1])
{
	unsigned n = 0;
	out[n++] = (rtc_row_inj){"host", 4, cfg_tag(cfg, "host")};
	for (unsigned i = 0; i < cfg->ntags; i++) {
		if (!strcmp(cfg->tags[i].key, "host"))
			continue; /* the host entry above carries it */
		out[n++] = (rtc_row_inj){cfg->tags[i].key, strlen(cfg->tags[i].key), cfg->tags[i].val};
	}
	return n;
}

const char *rtc_row_cfg_host(const rtc_config *cfg, const char *hostname)
{
	const char *h = cfg_tag(cfg, "host");
	return h ? h : hostname;
}

const char *rtc_row_tag(const rtc_rec_view *v, const char *k, size_t kl, size_t *len)
{
	for (unsigned i = 0; i < v->ntags; i++)
		if (str_eq(v->tag_k[i], k, kl)) {
			*len = v->tag_v[i].len;
			return v->tag_v[i].p;
		}
	return NULL;
}

const char *rtc_row_inj_value(const rtc_row_inj *in, const rtc_rec_view *v, const char *hostname, size_t *len)
{
	if (in->v) { /* configured tags win, like rtcollector */
		*len = strlen(in->v);
		return in->v;
	}
	const char *rv = rtc_row_tag(v, in->k, in->kl, len);
	if (rv)
		return rv;
	*len = strlen(hostname); /* only "host" has no configured value */
	return hostname;
}

bool rtc_row_is_injected(const rtc_row_inj *in, unsigned nin, rtc_str key)
{
	for (unsigned i = 0; i < nin; i++)
		if (str_eq(key, in[i].k, in[i].kl))
			return true;
	return false;
}

static int find_injected(const rtc_row_inj *in, unsigned nin, rtc_str k)
{
	for (unsigned i = 0; i < nin; i++)
		if (fold_eq(k.p, k.len, in[i].k, in[i].kl))
			return (int)i;
	return -1;
}

bool rtc_row_admit(rtc_row_cols *g, const rtc_rec_view *v, const rtc_row_inj *in, unsigned nin,
                   rtc_output_counters *k, bool count_nonfinite)
{
	rtc_row_col p[RTC_REC_MAX_TAGS + RTC_REC_MAX_FIELDS];
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
		p[np++] = (rtc_row_col){key, 1, 0};
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
		p[np++] = (rtc_row_col){f->key, 0, f->type};
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
			rtc_row_col *col = &g->cols[c];
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
	if (g->ncols + add > RTC_ROW_MAX_COLS - nin) {
		rtc_counter_add(&k->columns_over_limit, 1);
		return false;
	}
	for (unsigned i = 0; i < np; i++)
		if (p[i].type & 0x80)
			g->cols[g->ncols++] =
			    (rtc_row_col){p[i].name, p[i].is_tag, (uint8_t)(p[i].type & 0x7f)};
	return true;
}
