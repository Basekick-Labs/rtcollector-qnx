#include "loopstats.h"

#include <string.h>

void ls_init(ls_hist *h)
{
	memset(h, 0, sizeof(*h));
}

static unsigned bucket(uint64_t ns)
{
	uint64_t us = ns / 1000u;
	if (us < LS_FINE)
		return (unsigned)us;
	uint64_t c = (us - LS_FINE) / 100u;
	return c < LS_COARSE ? LS_FINE + (unsigned)c : LS_BUCKETS - 1;
}

uint64_t ls_lower(unsigned i)
{
	if (i < LS_FINE)
		return (uint64_t)i * 1000u;
	if (i < LS_FINE + LS_COARSE)
		return (LS_FINE + (uint64_t)(i - LS_FINE) * 100u) * 1000u;
	return (LS_FINE + (uint64_t)LS_COARSE * 100u) * 1000u;
}

uint64_t ls_upper(unsigned i)
{
	return i + 1 < LS_BUCKETS ? ls_lower(i + 1) : UINT64_MAX;
}

void ls_add(ls_hist *h, uint64_t ns)
{
	h->b[bucket(ns)]++;
	h->n++;
	if (ns > h->max_ns)
		h->max_ns = ns;
}

void ls_merge(ls_hist *dst, const ls_hist *src)
{
	for (unsigned i = 0; i < LS_BUCKETS; i++)
		dst->b[i] += src->b[i];
	dst->n += src->n;
	if (src->max_ns > dst->max_ns)
		dst->max_ns = src->max_ns;
}

uint64_t ls_quantile(const ls_hist *h, double q)
{
	if (h->n == 0)
		return 0;
	uint64_t rank = (uint64_t)(q * (double)h->n);
	if ((double)rank < q * (double)h->n)
		rank++; /* ceil */
	if (rank == 0)
		rank = 1;
	uint64_t seen = 0;
	for (unsigned i = 0; i < LS_BUCKETS; i++) {
		seen += h->b[i];
		if (seen >= rank) {
			uint64_t up = ls_upper(i); /* every value in bucket i is below up, none above max */
			return up < h->max_ns ? up : h->max_ns;
		}
	}
	return h->max_ns;
}

uint64_t ls_above(const ls_hist *h, uint64_t ns)
{
	uint64_t n = 0;
	for (unsigned i = 0; i < LS_BUCKETS; i++)
		if (ls_lower(i) >= ns)
			n += h->b[i];
	return n;
}

uint64_t ls_next_release(uint64_t t0, uint64_t period, uint64_t k, uint64_t now, uint64_t *skipped,
                         bool *overrun)
{
	uint64_t next = k + 1;
	*overrun = false;
	if (t0 + next * period <= now) {
		uint64_t first_future = (now - t0) / period + 1;
		*skipped += first_future - next;
		*overrun = true;
		next = first_future;
	}
	return next;
}
