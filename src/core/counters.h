/* Self-metric counters.
 *
 * Every counter has exactly one writing thread, so an increment is a relaxed
 * load and store, never a read-modify-write: no LL/SC loop on aarch64, no
 * lock, bounded on every path. Other threads only read.
 */
#ifndef RTC_COUNTERS_H
#define RTC_COUNTERS_H

#include <stdatomic.h>
#include <stdint.h>

_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "64-bit atomics must be lock-free");

typedef struct {
	_Atomic uint64_t v;
} rtc_counter;

static inline void rtc_counter_add(rtc_counter *c, uint64_t n)
{
	uint64_t cur = atomic_load_explicit(&c->v, memory_order_relaxed);
	atomic_store_explicit(&c->v, cur + n, memory_order_relaxed);
}

static inline void rtc_counter_set(rtc_counter *c, uint64_t n)
{
	atomic_store_explicit(&c->v, n, memory_order_relaxed);
}

static inline uint64_t rtc_counter_get(const rtc_counter *c)
{
	return atomic_load_explicit(&c->v, memory_order_relaxed);
}

/* Written by the collection thread. */
typedef struct {
	rtc_counter records_gathered;
	rtc_counter records_dropped_newest; /* ring full inside a claim */
	rtc_counter records_oversize;
	rtc_counter records_clock_invalid;
	rtc_counter records_invalid; /* an input built a record that does not fit */
	rtc_counter rounds;
	rtc_counter rounds_skipped;
	rtc_counter gather_ns_last;
	rtc_counter gather_errors;
	rtc_counter procfs_denied;      /* 1: no PROCMGR_AID_XPROCESS_QUERY, thread counts off */
	rtc_counter processes_overflow; /* scans stopped at max_processes */
	rtc_counter
	    processes_query_errors;    /* a process could not be queried (not exited, not EPERM-at-init) */
	rtc_counter priority_fallback; /* a thread could not get its configured priority */
	/* /dev/rtcollector lines, converted on the collection thread */
	rtc_counter lp_lines;
	rtc_counter lp_malformed;
	rtc_counter lp_bad_timestamp;
	rtc_counter lp_too_large; /* a line whose record exceeds max_record_bytes */
	rtc_counter lp_uint_dropped;
	rtc_counter rmgr_drain_dropped; /* writes overwritten before the drain read them */
	/* Written by the main thread at start or by an rmgr thread, never by
	 * two at once (the collection thread only reads them): */
	rtc_counter rmgr_unavailable;   /* 1: /dev/rtcollector could not be registered */
	rtc_counter rmgr_thread_deaths; /* rmgr handler threads that exited on an error */
} rtc_collect_counters;

/* Written by one resource manager thread (/dev/rtcollector); one set per thread. */
typedef struct {
	rtc_counter writes;
	rtc_counter bytes;
	rtc_counter dropped; /* ring full inside a claim: newest dropped */
	rtc_counter oversize;
	rtc_counter errors;        /* a write's data could not be read (EFAULT) */
	rtc_counter msgreads;      /* writes not fully in the receive buffer (MsgRead used) */
	rtc_counter max_prio_seen; /* highest writer priority seen */
} rtc_rmgr_counters;

/* Written by the sender thread, one set per output. */
typedef struct {
	rtc_counter records_sent;
	rtc_counter records_dropped;        /* evicted from the ring before this output read them */
	rtc_counter records_rejected;       /* refused by the destination or pre-validation */
	rtc_counter points_dropped_partial; /* Arc "partial write: N points dropped" */
	rtc_counter records_outcome_unknown;
	rtc_counter requests;
	rtc_counter request_errors;
	rtc_counter retries;
	rtc_counter fields_nonfinite;
	rtc_counter type_conflicts;
	rtc_counter columns_over_limit;
	rtc_counter invalid_names;
	rtc_counter last_status;
} rtc_output_counters;

#endif
