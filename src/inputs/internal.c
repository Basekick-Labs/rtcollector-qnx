/* internal: the collector's own counters, like rtcollector's internal input.
 *
 *   internal_collector  records gathered and lost at the source, rounds
 *   internal_output     per output (tag output=<name>): sent, dropped, rejected,
 *                       retries, validation drops, last HTTP status
 *
 * Counters are cumulative since start. Values above INT64_MAX are clamped:
 * Arc refuses unsigned 64-bit integers above that range.
 */
#include "../core/collector.h"

#include <stdint.h>

static int64_t clamp(uint64_t v)
{
	return v > INT64_MAX ? INT64_MAX : (int64_t)v;
}

#define F(b, name, ctr) rtc_rb_i64(b, name, clamp(rtc_counter_get(ctr)))

static int gather(rtc_collector *c)
{
	rtc_rec_builder b;
	const rtc_collect_counters *k = &c->counters;
	rtc_rb_init(&b, c->scratch, c->cfg.max_record_bytes, "internal_collector");
	F(&b, "records_gathered", &k->records_gathered);
	F(&b, "records_dropped_newest", &k->records_dropped_newest);
	F(&b, "records_oversize", &k->records_oversize);
	F(&b, "records_clock_invalid", &k->records_clock_invalid);
	F(&b, "records_invalid", &k->records_invalid);
	F(&b, "rounds", &k->rounds);
	F(&b, "rounds_skipped", &k->rounds_skipped);
	F(&b, "gather_ns", &k->gather_ns_last);
	F(&b, "gather_errors", &k->gather_errors);
	F(&b, "procfs_denied", &k->procfs_denied);
	F(&b, "processes_overflow", &k->processes_overflow);
	F(&b, "processes_query_errors", &k->processes_query_errors);
	F(&b, "priority_fallback", &k->priority_fallback);
	F(&b, "rmgr_unavailable", &k->rmgr_unavailable);
	F(&b, "rmgr_thread_deaths", &k->rmgr_thread_deaths);
	rtc_rb_i64(&b, "buffer_bytes", clamp(c->ring.cap));
	rtc_emit(c, &b);

	if (atomic_load(&c->lp_ndrains)) { /* /dev/rtcollector is running */
		rtc_rmgr_counters sum;
		rtc_rmgr_sum(c, &sum);
		const rtc_rmgr_counters *r = &sum;
		rtc_rb_init(&b, c->scratch, c->cfg.max_record_bytes, "internal_rmgr");
		F(&b, "writes", &r->writes);
		F(&b, "bytes", &r->bytes);
		F(&b, "dropped", &r->dropped);
		F(&b, "drain_dropped", &k->rmgr_drain_dropped);
		F(&b, "oversize", &r->oversize);
		F(&b, "errors", &r->errors);
		F(&b, "msgreads", &r->msgreads);
		F(&b, "lines", &k->lp_lines);
		F(&b, "malformed", &k->lp_malformed);
		F(&b, "bad_timestamp", &k->lp_bad_timestamp);
		F(&b, "too_large", &k->lp_too_large);
		F(&b, "uint_dropped", &k->lp_uint_dropped);
		F(&b, "max_writer_prio", &r->max_prio_seen);
		rtc_emit(c, &b);
	}

	for (unsigned i = 0; i < c->noutputs; i++) {
		const rtc_output *o = c->outputs[i];
		const rtc_output_counters *q = &o->counters;
		rtc_rb_init(&b, c->scratch, c->cfg.max_record_bytes, "internal_output");
		rtc_rb_tag(&b, "output", o->name);
		F(&b, "records_sent", &q->records_sent);
		F(&b, "records_dropped", &q->records_dropped);
		F(&b, "records_rejected", &q->records_rejected);
		F(&b, "points_dropped_partial", &q->points_dropped_partial);
		F(&b, "records_outcome_unknown", &q->records_outcome_unknown);
		F(&b, "requests", &q->requests);
		F(&b, "request_errors", &q->request_errors);
		F(&b, "retries", &q->retries);
		F(&b, "fields_nonfinite", &q->fields_nonfinite);
		F(&b, "type_conflicts", &q->type_conflicts);
		F(&b, "columns_over_limit", &q->columns_over_limit);
		F(&b, "invalid_names", &q->invalid_names);
		F(&b, "last_status", &q->last_status);
		rtc_emit(c, &b);
	}
	return 0;
}

const rtc_input_def rtc_input_internal = {.name = "internal", .gather = gather};
