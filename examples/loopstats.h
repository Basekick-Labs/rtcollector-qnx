/* Statistics for a periodic real-time loop: a fixed histogram (no
 * allocation after init, constant-time add) and the release rule for
 * overruns. Portable; used by control_loop.c and its host tests.
 *
 * Buckets: 1 us wide up to 2 ms, then 100 us wide up to 100 ms, then one
 * overflow bucket. A value is counted in the bucket that starts at or below
 * it; percentiles report a bucket's upper edge, so they never understate. */
#ifndef RTC_LOOPSTATS_H
#define RTC_LOOPSTATS_H

#include <stdbool.h>
#include <stdint.h>

#define LS_FINE    2000u /* 1 us buckets: [0, 2 ms) */
#define LS_COARSE  980u  /* 100 us buckets: [2 ms, 100 ms) */
#define LS_BUCKETS (LS_FINE + LS_COARSE + 1)

typedef struct {
	uint64_t b[LS_BUCKETS];
	uint64_t n;
	uint64_t max_ns;
} ls_hist;

void ls_init(ls_hist *h);
void ls_add(ls_hist *h, uint64_t ns);
/* Adds every bucket of src into dst (pooling runs). */
void ls_merge(ls_hist *dst, const ls_hist *src);
/* The q-quantile (0 < q <= 1) in ns: the upper edge of the bucket holding
 * the ceil(q * n)-th value, capped at the exact max. 0 when empty. */
uint64_t ls_quantile(const ls_hist *h, double q);
/* Values at or above ns, at bucket granularity: a bucket counts when its
 * lower edge is >= ns (exact for ns on a bucket edge, e.g. whole us). */
uint64_t ls_above(const ls_hist *h, uint64_t ns);
/* Bucket i's lower and upper edge in ns. */
uint64_t ls_lower(unsigned i);
uint64_t ls_upper(unsigned i);

/* Release rule. Releases are t0 + k * period. After a cycle that ended at
 * now, returns the index of the next release that is still in the future
 * (> now), and adds the releases jumped over to *skipped; *overrun is set
 * when at least one was. */
uint64_t ls_next_release(uint64_t t0, uint64_t period, uint64_t k, uint64_t now, uint64_t *skipped,
                         bool *overrun);

#endif
