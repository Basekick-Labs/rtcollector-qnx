#include "mp_read.h"

#include <stdlib.h>
#include <string.h>

static void *pool[1 << 16];
static size_t npool;

static void *alloc(size_t n)
{
	if (npool == sizeof(pool) / sizeof(pool[0]))
		return NULL;
	void *p = calloc(1, n ? n : 1);
	pool[npool++] = p;
	return p;
}

void mp_free_all(void)
{
	for (size_t i = 0; i < npool; i++)
		free(pool[i]);
	npool = 0;
}

typedef struct {
	const uint8_t *p;
	size_t len, off;
	bool bad;
} rd;

static uint64_t be(rd *r, int n)
{
	uint64_t v = 0;
	if (r->len - r->off < (size_t)n) {
		r->bad = true;
		return 0;
	}
	for (int i = 0; i < n; i++)
		v = (v << 8) | r->p[r->off++];
	return v;
}

static mp_node *node(rd *r, int depth);

static mp_node *container(rd *r, mp_node *x, enum mp_type t, uint32_t n, int depth)
{
	x->t = t;
	x->n = n;
	size_t k = t == MP_MAP ? 2u * n : n;
	x->kids = alloc(k * sizeof(mp_node *));
	for (size_t i = 0; i < k && !r->bad; i++)
		x->kids[i] = node(r, depth + 1);
	return x;
}

static mp_node *str(rd *r, mp_node *x, uint32_t n)
{
	x->t = MP_STR;
	x->n = n;
	if (r->len - r->off < n) {
		r->bad = true;
		return x;
	}
	x->s = (const char *)r->p + r->off;
	r->off += n;
	return x;
}

static mp_node *node(rd *r, int depth)
{
	mp_node *x = alloc(sizeof(*x));
	if (!x || depth > 32 || r->off >= r->len) {
		r->bad = true;
		return x;
	}
	uint8_t c = r->p[r->off++];
	if (c <= 0x7f) {
		x->t = MP_INT;
		x->i = c;
	} else if (c >= 0xe0) {
		x->t = MP_INT;
		x->i = (int8_t)c;
	} else if ((c & 0xe0) == 0xa0)
		return str(r, x, c & 0x1f);
	else if ((c & 0xf0) == 0x90)
		return container(r, x, MP_ARR, c & 0x0f, depth);
	else if ((c & 0xf0) == 0x80)
		return container(r, x, MP_MAP, c & 0x0f, depth);
	else
		switch (c) {
		case 0xc0:
			x->t = MP_NIL;
			break;
		case 0xc2:
		case 0xc3:
			x->t = MP_BOOL;
			x->b = c == 0xc3;
			break;
		case 0xd3:
			x->t = MP_INT;
			x->i = (int64_t)be(r, 8);
			break;
		case 0xcb: {
			uint64_t bits = be(r, 8);
			x->t = MP_F64;
			memcpy(&x->f, &bits, 8);
			break;
		}
		case 0xd9:
			return str(r, x, (uint32_t)be(r, 1));
		case 0xda:
			return str(r, x, (uint32_t)be(r, 2));
		case 0xdb:
			return str(r, x, (uint32_t)be(r, 4));
		case 0xdc:
			return container(r, x, MP_ARR, (uint32_t)be(r, 2), depth);
		case 0xdd:
			return container(r, x, MP_ARR, (uint32_t)be(r, 4), depth);
		case 0xde:
			return container(r, x, MP_MAP, (uint32_t)be(r, 2), depth);
		case 0xdf:
			return container(r, x, MP_MAP, (uint32_t)be(r, 4), depth);
		default:
			r->bad = true; /* the encoder never emits anything else */
		}
	return x;
}

mp_node *mp_parse(const uint8_t *buf, size_t len)
{
	rd r = {buf, len, 0, false};
	mp_node *n = node(&r, 0);
	return r.bad || r.off != len ? NULL : n;
}

bool mp_str_is(const mp_node *n, const char *s)
{
	return n && n->t == MP_STR && n->n == strlen(s) && !memcmp(n->s, s, n->n);
}

mp_node *mp_get(const mp_node *map, const char *key)
{
	if (!map || map->t != MP_MAP)
		return NULL;
	for (uint32_t i = 0; i < map->n; i++)
		if (mp_str_is(map->kids[2 * i], key))
			return map->kids[2 * i + 1];
	return NULL;
}
