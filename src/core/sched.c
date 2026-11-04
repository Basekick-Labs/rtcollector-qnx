#include "sched.h"

#include <stdlib.h>
#include <string.h>

unsigned rtc_hist_bucket(uint64_t v)
{
	if (v < RTC_HIST_SUB)
		return (unsigned)v; /* 0..7 exact */
	unsigned msb = 63u - (unsigned)__builtin_clzll(v);
	if (msb >= RTC_HIST_MAX_LOG)
		return RTC_HIST_BUCKETS - 1;
	/* 8 linear sub-buckets inside [2^msb, 2^(msb+1)) */
	unsigned sub = (unsigned)((v >> (msb - 3)) & (RTC_HIST_SUB - 1));
	return (msb - 2) * RTC_HIST_SUB + sub;
}

uint64_t rtc_hist_bucket_hi(unsigned idx)
{
	if (idx < RTC_HIST_SUB)
		return idx;
	unsigned msb = idx / RTC_HIST_SUB + 2, sub = idx % RTC_HIST_SUB;
	uint64_t lo = (1ull << msb) + ((uint64_t)sub << (msb - 3));
	return lo + (1ull << (msb - 3)) - 1;
}

void rtc_hist_add(rtc_hist *h, uint64_t v)
{
	h->b[rtc_hist_bucket(v)]++;
	h->n++;
	if (v > h->max)
		h->max = v;
}

uint64_t rtc_hist_quantile(const rtc_hist *h, double q)
{
	if (h->n == 0)
		return 0;
	if (q >= 1.0)
		return h->max;
	uint64_t rank = (uint64_t)(q * (double)h->n);
	if (rank >= h->n)
		rank = h->n - 1;
	uint64_t seen = 0;
	for (unsigned i = 0; i < RTC_HIST_BUCKETS; i++) {
		seen += h->b[i];
		if (seen > rank) {
			uint64_t hi = rtc_hist_bucket_hi(i);
			return hi < h->max ? hi : h->max;
		}
	}
	return h->max;
}

int rtc_sched_init(rtc_sched *s, size_t max_threads)
{
	memset(s, 0, sizeof(*s));
	/* One slot more than the threads it holds: find() keeps one empty so a
	 * miss ends its probe early, and max_threads stays the real capacity. */
	s->threads = calloc(max_threads + 1, sizeof(*s->threads));
	s->max_procs = max_threads;
	s->procs = calloc(s->max_procs, sizeof(*s->procs));
	if (!s->threads || !s->procs)
		return -1;
	s->max_threads = max_threads + 1; /* table slots */
	return 0;
}

void rtc_sched_free(rtc_sched *s)
{
	free(s->threads);
	free(s->procs);
	s->threads = NULL;
	s->procs = NULL;
}

/* Open addressing on (pid, tid). Returns NULL when the table is full. */
static rtc_sched_thread *find(rtc_sched *s, uint32_t pid, uint32_t tid, bool create)
{
	size_t n = s->max_threads, h = ((size_t)pid * 2654435761u + tid) % n;
	rtc_sched_thread *free_slot = NULL;
	for (size_t i = 0; i < n; i++) {
		rtc_sched_thread *t = &s->threads[(h + i) % n];
		if (t->used && t->pid == pid && t->tid == tid)
			return t;
		if (!t->used) {
			if (!free_slot)
				free_slot = t;
			if (t->pid == 0 && t->tid == 0)
				break; /* never used: the key is not further along */
		}
	}
	if (!create)
		return NULL;
	if (!free_slot || s->nthreads + 1 >= n) { /* keep one slot empty: probes end */
		s->threads_overflow++;
		return NULL;
	}
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->used = true;
	free_slot->pid = pid;
	free_slot->tid = tid;
	free_slot->state = RTC_EV_BLOCKED;
	free_slot->max_prio = -1;
	s->nthreads++;
	return free_slot;
}

void rtc_sched_event_in(rtc_sched *s, const rtc_sched_event *e)
{
	s->events++;
	if (e->kind == RTC_EV_DEAD) {
		rtc_sched_thread *t = find(s, e->pid, e->tid, false);
		if (t) {
			/* Tombstone: keep pid/tid non-zero so probes continue past it. */
			t->used = false;
			s->nthreads--;
		}
		return;
	}
	rtc_sched_thread *t = find(s, e->pid, e->tid, true);
	if (!t)
		return;
	/* Per-CPU kernel buffers are flushed independently, so at a file
	 * rotation an older event can arrive after a newer one of the same
	 * thread. Applying it would leave a stale READY behind (a bogus latency
	 * of seconds later on): ignore it, and count it. */
	if (e->t_ns < t->last_t) {
		s->out_of_order++;
		return;
	}
	t->last_t = e->t_ns;
	if (e->prio > t->max_prio)
		t->max_prio = e->prio;
	switch (e->kind) {
	case RTC_EV_READY:
		if (t->state == RTC_EV_RUNNING) {
			t->preemptions++;
			if (t->run_since && e->t_ns >= t->run_since)
				t->run_ns += e->t_ns - t->run_since;
			t->run_since = 0;
		}
		if (t->state != RTC_EV_READY)
			t->ready_at = e->t_ns; /* the first READY counts, repeats do not move it */
		break;
	case RTC_EV_RUNNING:
		if (e->cpu < RTC_SCHED_MAX_CPUS)
			s->cpu_switches[e->cpu]++;
		if (t->state == RTC_EV_READY && t->ready_at && e->t_ns >= t->ready_at) {
			rtc_hist_add(&t->lat, e->t_ns - t->ready_at);
			t->wakeups++;
		}
		t->ready_at = 0;
		t->run_since = e->t_ns;
		break;
	default:
		if (t->state == RTC_EV_RUNNING && t->run_since && e->t_ns >= t->run_since)
			t->run_ns += e->t_ns - t->run_since;
		t->run_since = 0;
		t->ready_at = 0;
		break;
	}
	t->state = e->kind;
}

void rtc_sched_dropped(rtc_sched *s, uint64_t n)
{
	s->drops += n;
	for (size_t i = 0; i < s->max_threads; i++) {
		rtc_sched_thread *t = &s->threads[i];
		if (!t->used)
			continue;
		t->ready_at = 0;
		t->run_since = 0;
		t->state = RTC_EV_BLOCKED;
	}
}

static void copy_name(char *dst, const char *src)
{
	/* Keep the last path component of process names ("proc/boot/devb-eide"). */
	const char *slash = strrchr(src, '/');
	if (slash && slash[1])
		src = slash + 1;
	size_t n = strnlen(src, RTC_SCHED_NAME - 1);
	memcpy(dst, src, n);
	dst[n] = '\0';
}

void rtc_sched_thread_name(rtc_sched *s, uint32_t pid, uint32_t tid, const char *name)
{
	rtc_sched_thread *t = find(s, pid, tid, true);
	if (t)
		copy_name(t->name, name);
}

void rtc_sched_process_name(rtc_sched *s, uint32_t pid, const char *name)
{
	for (size_t i = 0; i < s->nprocs; i++)
		if (s->procs[i].pid == pid) {
			copy_name(s->procs[i].name, name);
			return;
		}
	if (s->nprocs == s->max_procs) {
		/* Full: reuse the oldest slot (process names are only labels). */
		memmove(s->procs, s->procs + 1, (s->max_procs - 1) * sizeof(*s->procs));
		s->nprocs--;
	}
	s->procs[s->nprocs].pid = pid;
	copy_name(s->procs[s->nprocs].name, name);
	s->nprocs++;
}

void rtc_sched_interrupt(rtc_sched *s, unsigned cpu, uint32_t vector)
{
	if (cpu < RTC_SCHED_MAX_CPUS)
		s->cpu_interrupts[cpu]++;
	for (unsigned i = 0; i < s->nvec; i++)
		if (s->vec_id[i] == vector) {
			s->vec_count[i]++;
			return;
		}
	if (s->nvec == RTC_SCHED_MAX_VECTORS) {
		s->vectors_overflow++;
		return;
	}
	s->vec_id[s->nvec] = vector;
	s->vec_count[s->nvec++] = 1;
}

int rtc_sched_snapshot_init(rtc_sched_snapshot *o, size_t max_rows)
{
	memset(o, 0, sizeof(*o));
	o->rows = calloc(max_rows ? max_rows : 1, sizeof(*o->rows));
	o->max_rows = max_rows;
	return o->rows ? 0 : -1;
}

void rtc_sched_snapshot_free(rtc_sched_snapshot *o)
{
	free(o->rows);
	o->rows = NULL;
}

static const char *proc_name(const rtc_sched *s, uint32_t pid)
{
	for (size_t i = 0; i < s->nprocs; i++)
		if (s->procs[i].pid == pid)
			return s->procs[i].name;
	return "";
}

static void fill_row(const rtc_sched *s, const rtc_sched_thread *t, rtc_sched_row *r)
{
	r->pid = t->pid;
	r->tid = t->tid;
	copy_name(r->process, proc_name(s, t->pid));
	copy_name(r->thread, t->name);
	r->wakeups = t->wakeups;
	r->preemptions = t->preemptions;
	r->run_ns = t->run_ns;
	r->p50_ns = rtc_hist_quantile(&t->lat, 0.50);
	r->p99_ns = rtc_hist_quantile(&t->lat, 0.99);
	r->max_ns = t->lat.max;
	r->prio = t->max_prio;
}

static bool reportable(const rtc_sched_thread *t)
{
	return t->used && t->wakeups > 0 && t->pid != 1; /* pid 1 = procnto (idle threads) */
}

void rtc_sched_take(rtc_sched *s, unsigned ncpu, int min_prio, size_t top, rtc_sched_snapshot *out)
{
	out->nrows = 0;
	out->threads_omitted = 0;
	/* Pass 1: every thread at or above min_prio. */
	for (size_t i = 0; i < s->max_threads; i++) {
		const rtc_sched_thread *t = &s->threads[i];
		if (!reportable(t) || t->max_prio < min_prio)
			continue;
		if (out->nrows < out->max_rows)
			fill_row(s, t, &out->rows[out->nrows++]);
		else
			out->threads_omitted++;
	}
	/* Pass 2: the top `top` others by p99 (selection by repeated maximum:
	 * top is small). */
	size_t first_other = out->nrows;
	for (size_t k = 0; k < top && out->nrows < out->max_rows; k++) {
		const rtc_sched_thread *best = NULL;
		uint64_t best_p99 = 0;
		for (size_t i = 0; i < s->max_threads; i++) {
			const rtc_sched_thread *t = &s->threads[i];
			if (!reportable(t) || t->max_prio >= min_prio)
				continue;
			bool taken = false;
			for (size_t j = first_other; j < out->nrows; j++)
				if (out->rows[j].pid == t->pid && out->rows[j].tid == t->tid)
					taken = true;
			if (taken)
				continue;
			uint64_t p99 = rtc_hist_quantile(&t->lat, 0.99);
			if (!best || p99 > best_p99) {
				best = t;
				best_p99 = p99;
			}
		}
		if (!best)
			break;
		fill_row(s, best, &out->rows[out->nrows++]);
	}
	size_t others = 0;
	for (size_t i = 0; i < s->max_threads; i++)
		if (reportable(&s->threads[i]) && s->threads[i].max_prio < min_prio)
			others++;
	out->threads_omitted += others - (out->nrows - first_other);

	out->ncpu = ncpu < RTC_SCHED_MAX_CPUS ? ncpu : RTC_SCHED_MAX_CPUS;
	memcpy(out->cpu_switches, s->cpu_switches, sizeof(out->cpu_switches));
	memcpy(out->cpu_interrupts, s->cpu_interrupts, sizeof(out->cpu_interrupts));
	out->nvec = s->nvec;
	memcpy(out->vec_id, s->vec_id, sizeof(out->vec_id));
	memcpy(out->vec_count, s->vec_count, sizeof(out->vec_count));

	/* Reset the interval counters; keep state, names and vectors seen. */
	for (size_t i = 0; i < s->max_threads; i++) {
		rtc_sched_thread *t = &s->threads[i];
		t->wakeups = t->preemptions = t->run_ns = 0;
		t->max_prio = -1;
		memset(&t->lat, 0, sizeof(t->lat));
	}
	memset(s->cpu_switches, 0, sizeof(s->cpu_switches));
	memset(s->cpu_interrupts, 0, sizeof(s->cpu_interrupts));
	memset(s->vec_count, 0, sizeof(s->vec_count));
}
