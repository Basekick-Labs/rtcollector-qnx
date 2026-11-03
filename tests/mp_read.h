/* Test-only MessagePack reader: decodes into a tree of nodes from a pool. */
#ifndef RTC_MP_READ_H
#define RTC_MP_READ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum mp_type { MP_NIL, MP_BOOL, MP_INT, MP_F64, MP_STR, MP_ARR, MP_MAP };

typedef struct mp_node {
	enum mp_type t;
	int64_t i;
	double f;
	bool b;
	const char *s;
	uint32_t n;            /* str length, array length, map pairs */
	struct mp_node **kids; /* arrays: n; maps: 2n (k, v, k, v...) */
} mp_node;

/* Decodes buf fully; returns NULL on malformed input or trailing bytes. */
mp_node *mp_parse(const uint8_t *buf, size_t len);
void mp_free_all(void);

mp_node *mp_get(const mp_node *map, const char *key);
bool mp_str_is(const mp_node *n, const char *s);

#endif
