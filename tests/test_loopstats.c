/* control_loop's statistics: buckets, percentiles, pooling, the release rule. */
#include "t.h"

#include "../examples/loopstats.h"

#include <stdlib.h>

static void buckets_and_edges(void)
{
	CHECK_EQ_U(ls_lower(0), 0);
	CHECK_EQ_U(ls_upper(0), 1000);
	CHECK_EQ_U(ls_lower(1999), 1999000);
	CHECK_EQ_U(ls_lower(2000), 2000000);
	CHECK_EQ_U(ls_upper(2000), 2100000);
	CHECK_EQ_U(ls_lower(LS_BUCKETS - 1), 100000000);
	static ls_hist h;
	ls_init(&h);
	ls_add(&h, 999);       /* bucket 0 */
	ls_add(&h, 1000);      /* bucket 1 */
	ls_add(&h, 2099999);   /* first coarse bucket */
	ls_add(&h, 500000000); /* overflow */
	CHECK_EQ_U(h.b[0], 1);
	CHECK_EQ_U(h.b[1], 1);
	CHECK_EQ_U(h.b[2000], 1);
	CHECK_EQ_U(h.b[LS_BUCKETS - 1], 1);
	CHECK_EQ_U(h.n, 4);
	CHECK_EQ_U(h.max_ns, 500000000);
}

static void quantiles_never_understate(void)
{
	static ls_hist h;
	ls_init(&h);
	CHECK_EQ_U(ls_quantile(&h, 0.99), 0);
	for (int i = 1; i <= 1000; i++)
		ls_add(&h, (uint64_t)i * 1000 + 500); /* 1.5 us .. 1000.5 us */
	CHECK_EQ_U(ls_quantile(&h, 0.5), 501000);     /* 500th value 500.5 us, bucket [500,501) */
	CHECK_EQ_U(ls_quantile(&h, 0.99), 991000);    /* 990th */
	CHECK_EQ_U(ls_quantile(&h, 0.999), 1000000);
	CHECK_EQ_U(ls_quantile(&h, 1.0), 1000500); /* capped at the exact max */
	ls_add(&h, 7000000000ull);                 /* overflow: the max itself */
	CHECK_EQ_U(ls_quantile(&h, 1.0), 7000000000ull);
	CHECK_EQ_U(ls_above(&h, 991000), 11); /* 991.5 .. 1000.5 us, and 7 s */
	CHECK_EQ_U(ls_above(&h, 20000), 982);
	/* A fractional rank rounds up: the median of 3 values is the 2nd. */
	ls_init(&h);
	ls_add(&h, 1500);
	ls_add(&h, 2500);
	ls_add(&h, 3500);
	CHECK_EQ_U(ls_quantile(&h, 0.5), 3000);
}

static void merge_pools_runs(void)
{
	static ls_hist a, b;
	ls_init(&a);
	ls_init(&b);
	for (int i = 0; i < 99; i++)
		ls_add(&a, 10000);
	ls_add(&b, 300000);
	ls_merge(&a, &b);
	CHECK_EQ_U(a.n, 100);
	CHECK_EQ_U(a.max_ns, 300000);
	CHECK_EQ_U(ls_quantile(&a, 0.99), 11000);
	CHECK_EQ_U(ls_quantile(&a, 1.0), 300000);
}

static void release_rule_skips_missed_releases(void)
{
	uint64_t skipped = 0;
	bool over;
	/* On time: the next release. */
	CHECK_EQ_U(ls_next_release(0, 1000, 5, 5500, &skipped, &over), 6);
	CHECK(!over && skipped == 0);
	/* Ended exactly at the next release: it is not in the future. */
	CHECK_EQ_U(ls_next_release(0, 1000, 5, 6000, &skipped, &over), 7);
	CHECK(over && skipped == 1);
	/* Ran 3.5 periods long: releases 6, 7, 8 are skipped, 9 is next. */
	skipped = 0;
	CHECK_EQ_U(ls_next_release(0, 1000, 5, 8500, &skipped, &over), 9);
	CHECK(over && skipped == 3);
	/* With an offset t0. */
	skipped = 0;
	CHECK_EQ_U(ls_next_release(100, 1000, 0, 1099, &skipped, &over), 1);
	CHECK(!over);
}

const t_case loopstats_cases[] = {
    {"loopstats", "buckets_and_edges", buckets_and_edges},
    {"loopstats", "quantiles_never_understate", quantiles_never_understate},
    {"loopstats", "merge_pools_runs", merge_pools_runs},
    {"loopstats", "release_rule_skips_missed_releases", release_rule_skips_missed_releases},
    {0},
};
