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

static size_t home(const rtc_sched *s, uint32_t pid, uint32_t tid)
{
	return ((size_t)pid * 2654435761u + tid) % s->max_threads;
}

/* Open addressing on (pid, tid), linear probing, no tombstones (slot_free
 * shifts entries back), so a probe ends at the first empty slot. Returns
 * NULL when the table is full. */
static rtc_sched_thread *find(rtc_sched *s, uint32_t pid, uint32_t tid, bool create)
{
	size_t n = s->max_threads, h = home(s, pid, tid);
	for (size_t i = 0; i < n; i++) {
		rtc_sched_thread *t = &s->threads[(h + i) % n];
		if (t->used && t->pid == pid && t->tid == tid)
			return t;
		if (!t->used) {
			if (!create)
				return NULL;
			if (s->nthreads + 1 >= n) { /* keep one slot empty: probes end */
				s->threads_overflow++;
				return NULL;
			}
			memset(t, 0, sizeof(*t));
			t->used = true;
			t->pid = pid;
			t->tid = tid;
			t->state = RTC_EV_BLOCKED;
			t->max_prio = -1;
			s->nthreads++;
			return t;
		}
	}
	return NULL; /* not reached: one slot is always empty */
}

/* Empties slot i and moves later entries of its probe run back into the
 * hole (backward-shift deletion), so lookups never need tombstones. */
static void slot_free(rtc_sched *s, size_t i)
{
	size_t n = s->max_threads, j = i;
	s->nthreads--;
	for (;;) {
		s->threads[i].used = false;
		for (;;) {
			j = (j + 1) % n;
			if (!s->threads[j].used)
				return;
			size_t k = home(s, s->threads[j].pid, s->threads[j].tid);
			/* Entry j stays if its home lies cyclically in (i, j]. */
			if (i <= j ? (i < k && k <= j) : (i < k || k <= j))
				continue;
			break;
		}
		s->threads[i] = s->threads[j];
		i = j;
	}
}

void rtc_sched_event_in(rtc_sched *s, const rtc_sched_event *e)
{
	s->events++;
	if (e->kind == RTC_EV_DEAD) {
		rtc_sched_thread *t = find(s, e->pid, e->tid, false);
		if (t) {
			if (t->wakeups > 0 && t->pid != 1)
				s->exited_unreported++;
			if (t->name_t > e->t_ns) {
				/* Named after this exit: the slot is already the next
				 * thread with this pid/tid (tids are reused at once).
				 * End the old one's interval, keep the slot and name. */
				t->wakeups = t->preemptions = t->run_ns = 0;
				t->max_prio = -1;
				memset(&t->lat, 0, sizeof(t->lat));
				t->ready_at = t->run_since = 0;
				t->state = RTC_EV_BLOCKED;
				if (e->t_ns > t->last_t)
					t->last_t = e->t_ns;
				return;
			}
			slot_free(s, (size_t)(t - s->threads));
		}
		return;
	}
	rtc_sched_thread *t = find(s, e->pid, e->tid, true);
	if (!t)
		return;
	/* An event older than the thread's newest (it missed the watermark of
	 * its file): ignore it, and forget the pending wait it may have ended,
	 * or the next RUNNING reads as a wait that did not happen. */
	if (e->t_ns < t->last_t) {
		s->out_of_order++;
		t->ready_at = t->run_since = 0;
		t->state = RTC_EV_BLOCKED;
		return;
	}
	t->last_t = e->t_ns;
	t->seen = true;
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
		else
			s->ready_repeats++; /* in time order the kernel never repeats it */
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

void rtc_sched_thread_name(rtc_sched *s, uint32_t pid, uint32_t tid, const char *name, uint64_t t_ns)
{
	rtc_sched_thread *t = find(s, pid, tid, true);
	if (t) {
		copy_name(t->name, name);
		t->name_t = t_ns;
	}
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

/* Adds up to k rows, highest p99 first, from the reportable threads on one
 * side of min_prio. */
static void pick(rtc_sched *s, int min_prio, bool high, size_t k, rtc_sched_snapshot *out)
{
	for (size_t n = 0; n < k && out->nrows < out->max_rows; n++) {
		rtc_sched_thread *best = NULL;
		for (size_t i = 0; i < s->max_threads; i++) {
			rtc_sched_thread *t = &s->threads[i];
			if (!reportable(t) || t->picked || (t->max_prio >= min_prio) != high)
				continue;
			if (!best || t->p99 > best->p99)
				best = t;
		}
		if (!best)
			return;
		best->picked = true;
		fill_row(s, best, &out->rows[out->nrows++]);
	}
}

void rtc_sched_take(rtc_sched *s, unsigned ncpu, int min_prio, size_t top, rtc_sched_snapshot *out)
{
	out->nrows = 0;
	out->threads_omitted = s->exited_unreported;
	s->exited_unreported = 0;
	size_t high = 0, others = 0;
	for (size_t i = 0; i < s->max_threads; i++) {
		rtc_sched_thread *t = &s->threads[i];
		t->picked = false;
		if (!reportable(t))
			continue;
		t->p99 = rtc_hist_quantile(&t->lat, 0.99);
		if (t->max_prio >= min_prio)
			high++;
		else
			others++;
	}
	/* Pass 1: every thread at or above min_prio (the worst p99 first when
	 * they do not all fit). */
	if (high <= out->max_rows) {
		for (size_t i = 0; i < s->max_threads; i++) {
			rtc_sched_thread *t = &s->threads[i];
			if (reportable(t) && t->max_prio >= min_prio) {
				t->picked = true;
				fill_row(s, t, &out->rows[out->nrows++]);
			}
		}
	} else {
		pick(s, min_prio, true, out->max_rows, out);
	}
	size_t first_other = out->nrows;
	/* Pass 2: the top `top` others by p99. */
	pick(s, min_prio, false, top, out);
	out->threads_omitted += (high - first_other) + (others - (out->nrows - first_other));

	out->ncpu = ncpu < RTC_SCHED_MAX_CPUS ? ncpu : RTC_SCHED_MAX_CPUS;
	memcpy(out->cpu_switches, s->cpu_switches, sizeof(out->cpu_switches));
	memcpy(out->cpu_interrupts, s->cpu_interrupts, sizeof(out->cpu_interrupts));
	out->nvec = s->nvec;
	memcpy(out->vec_id, s->vec_id, sizeof(out->vec_id));
	memcpy(out->vec_count, s->vec_count, sizeof(out->vec_count));

	/* Reset the interval counters; keep state, names and vectors seen. */
	for (size_t i = 0; i < s->max_threads; i++) {
		rtc_sched_thread *t = &s->threads[i];
		if (!t->used)
			continue;
		t->idle = t->seen ? 0 : t->idle + 1;
		t->seen = false;
		t->wakeups = t->preemptions = t->run_ns = 0;
		t->max_prio = -1;
		if (t->lat.n)
			memset(&t->lat, 0, sizeof(t->lat));
	}
	/* With the table three-quarters full, evict (down to half full) threads idle for two
	 * intervals that are not ready or running: a lost DEAD event must not
	 * hold a slot for ever. Not sooner: an evicted thread loses its name
	 * (names come only at creation and session start). A READY thread is
	 * kept, its wait is what this input reports. A freed slot can receive a
	 * later entry, so each slot is checked until it holds a keeper. */
	size_t cap = s->max_threads - 1;
	bool pressure = s->nthreads * 4 >= cap * 3; /* then down to half full */
	for (size_t i = 0; pressure && i < s->max_threads && s->nthreads * 2 > cap; i++) {
		rtc_sched_thread *t = &s->threads[i];
		while (t->used && t->idle >= 2 && t->state == RTC_EV_BLOCKED) {
			slot_free(s, i);
			s->threads_evicted++;
		}
	}
	memset(s->cpu_switches, 0, sizeof(s->cpu_switches));
	memset(s->cpu_interrupts, 0, sizeof(s->cpu_interrupts));
	memset(s->vec_count, 0, sizeof(s->vec_count));
}

/* --- events of a file, in time order ---------------------------------------- */

int rtc_sched_batch_init(rtc_sched_batch *b, size_t cap)
{
	memset(b, 0, sizeof(*b));
	if (cap == 0)
		cap = 1;
	b->ev = calloc(cap, sizeof(*b->ev));
	if (!b->ev)
		return -1;
	b->cap = cap;
	return 0;
}

void rtc_sched_batch_free(rtc_sched_batch *b)
{
	free(b->ev);
	b->ev = NULL;
	b->cap = b->n = 0;
}

static uint8_t cpu_of(unsigned cpu)
{
	return cpu < RTC_SCHED_MAX_CPUS ? (uint8_t)cpu : RTC_SCHED_NO_CPU;
}

/* A CPU reached t: its watermark moves, and a gap waiting for its next
 * event takes this time (and closes). */
static void cpu_reached(rtc_sched_batch *b, unsigned cpu, uint64_t t)
{
	if (cpu >= RTC_SCHED_MAX_CPUS)
		return;
	uint64_t bit = 1ull << cpu;
	if (b->known & bit) {
		if (t < b->last[cpu])
			b->cpu_backwards++; /* the premise of the watermark failed: counted */
		else
			b->last[cpu] = t;
	} else {
		b->last[cpu] = t;
	}
	b->known |= bit;
	b->present |= bit;
	if (b->gap_open & bit) {
		rtc_sched_qev *g = &b->ev[b->gap_idx[cpu]];
		if (t > g->t_ns)
			g->t_ns = t;
		g->tid = 0; /* closed */
		b->gap_open &= ~bit;
	}
}

static int qev_rank(uint8_t kind)
{
	/* At equal times: a gap's reset first, then READY (a READY and the
	 * RUNNING it caused can share a timestamp on two CPUs), then the rest. */
	return kind == RTC_SCHED_GAP ? 0 : kind == RTC_EV_READY ? 1 : 2;
}

static int qev_cmp(const void *pa, const void *pb)
{
	const rtc_sched_qev *a = pa, *b = pb;
	if (a->t_ns != b->t_ns)
		return a->t_ns < b->t_ns ? -1 : 1;
	int ra = qev_rank(a->kind), rb = qev_rank(b->kind);
	if (ra != rb)
		return ra < rb ? -1 : 1;
	if (a->seq != b->seq)
		return a->seq < b->seq ? -1 : 1;
	return 0;
}

static void forget_tracking(rtc_sched_batch *b)
{
	b->known = b->present = b->gap_open = 0;
	memset(b->absent, 0, sizeof(b->absent));
}

static rtc_sched_qev *batch_slot(rtc_sched *s, rtc_sched_batch *b)
{
	if (b->n == b->cap) {
		/* More events than one file can hold (see the sizing in trace.c):
		 * apply what is safe, drop the rest (counted). */
		b->overflow++;
		rtc_sched_batch_apply(s, b, false, NULL);
	}
	rtc_sched_qev *q = &b->ev[b->n++];
	q->seq = b->seq++;
	return q;
}

void rtc_sched_batch_add(rtc_sched *s, rtc_sched_batch *b, const rtc_sched_event *e)
{
	rtc_sched_qev *q = batch_slot(s, b);
	q->t_ns = e->t_ns;
	q->pid = e->pid;
	q->tid = e->tid;
	q->cpu = cpu_of(e->cpu);
	q->kind = (uint8_t)e->kind;
	q->prio = (int16_t)(e->prio < -1 ? -1 : e->prio > 32767 ? 32767 : e->prio);
	cpu_reached(b, e->cpu, e->t_ns);
}

void rtc_sched_batch_cpu_time(rtc_sched_batch *b, unsigned cpu, uint64_t t_ns)
{
	cpu_reached(b, cpu, t_ns);
}

void rtc_sched_batch_gap(rtc_sched *s, rtc_sched_batch *b, unsigned cpu, uint64_t t_ns, uint64_t n)
{
	uint64_t bit = cpu < RTC_SCHED_MAX_CPUS ? 1ull << cpu : 0;
	if (b->gap_open & bit) {
		/* A second marker before any event of its CPU: one reset covers
		 * both. */
		rtc_sched_qev *g = &b->ev[b->gap_idx[cpu]];
		uint64_t sum = (uint64_t)g->pid + n;
		g->pid = sum > UINT32_MAX ? UINT32_MAX : (uint32_t)sum;
		if (t_ns > g->t_ns)
			g->t_ns = t_ns;
		return;
	}
	rtc_sched_qev *q = batch_slot(s, b);
	q->t_ns = t_ns;
	q->pid = n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
	q->cpu = cpu_of(cpu);
	q->kind = RTC_SCHED_GAP;
	q->prio = -1;
	q->tid = bit ? 1 : 0; /* open: waits for its CPU's next event, holding the watermark */
	if (bit) {
		b->gap_open |= bit;
		b->gap_idx[cpu] = b->n - 1;
	}
}

/* Positions of the open gaps after the entries moved. */
static void find_open_gaps(rtc_sched_batch *b)
{
	b->gap_open = 0;
	for (size_t j = 0; j < b->n; j++) {
		const rtc_sched_qev *q = &b->ev[j];
		if (q->kind == RTC_SCHED_GAP && q->tid == 1 && q->cpu != RTC_SCHED_NO_CPU) {
			b->gap_open |= 1ull << q->cpu;
			b->gap_idx[q->cpu] = j;
		}
	}
}

static void apply_one(rtc_sched *s, const rtc_sched_qev *q)
{
	if (q->kind == RTC_SCHED_GAP) {
		rtc_sched_dropped(s, q->pid);
		return;
	}
	rtc_sched_event e = {.cpu = q->cpu == RTC_SCHED_NO_CPU ? RTC_SCHED_MAX_CPUS : q->cpu,
	                     .t_ns = q->t_ns,
	                     .pid = q->pid,
	                     .tid = q->tid,
	                     .kind = (enum rtc_sched_kind)q->kind,
	                     .prio = q->prio};
	rtc_sched_event_in(s, &e);
}

void rtc_sched_batch_apply(rtc_sched *s, rtc_sched_batch *b, bool carry, const _Atomic bool *stop)
{
	if (stop && *stop) {
		b->n = 0;
		forget_tracking(b);
		return;
	}
	if (b->n > 1)
		qsort(b->ev, b->n, sizeof(*b->ev), qev_cmp);
	if (b->gap_open)
		find_open_gaps(b);

	/* The watermark: the newest time every CPU has reached. A CPU that had
	 * events before but none since the last apply holds it at its last
	 * time once; twice in a row it is released (it went quiet), and the
	 * pending waits are forgotten since its events may still be missing. */
	bool reset = false;
	uint64_t w = UINT64_MAX, newest = 0;
	for (unsigned c = 0; c < RTC_SCHED_MAX_CPUS; c++) {
		uint64_t bit = 1ull << c;
		if (!(b->known & bit))
			continue;
		if (b->present & bit) {
			b->absent[c] = 0;
		} else if (++b->absent[c] >= 2) {
			b->known &= ~bit;
			b->absent[c] = 0;
			b->cpu_silent++;
			reset = true;
			continue;
		}
		if (b->last[c] < w)
			w = b->last[c];
		if (b->last[c] > newest)
			newest = b->last[c];
	}
	/* A gap still waiting for its CPU's next event: its reset must come
	 * before everything after it, so nothing from its time on is applied. */
	for (unsigned c = 0; c < RTC_SCHED_MAX_CPUS; c++)
		if ((b->gap_open & (1ull << c)) && b->ev[b->gap_idx[c]].t_ns < w)
			w = b->ev[b->gap_idx[c]].t_ns;
	if (reset)
		rtc_sched_dropped(s, 0);
	b->lag_ns = (w == UINT64_MAX || newest < w) ? 0 : newest - w;

	size_t i = 0;
	for (; i < b->n && b->ev[i].t_ns < w; i++) {
		if (stop && (i & 4095) == 4095 && *stop) {
			b->n = 0;
			forget_tracking(b);
			return;
		}
		apply_one(s, &b->ev[i]);
	}
	size_t k = b->n - i;
	if (carry) {
		/* Kept for the next file, still in order. */
		memmove(b->ev, b->ev + i, k * sizeof(*b->ev));
		b->n = k;
		for (size_t j = 0; j < k; j++)
			b->ev[j].seq = (uint32_t)j;
		find_open_gaps(b);
		b->seq = (uint32_t)k;
		b->present = 0;
	} else {
		b->discarded += k;
		if (k)
			rtc_sched_dropped(s, 0);
		b->n = 0;
		b->seq = 0;
		forget_tracking(b);
	}
}

void rtc_sched_batch_clear(rtc_sched *s, rtc_sched_batch *b)
{
	if (b->n) {
		b->discarded += b->n;
		rtc_sched_dropped(s, 0);
	}
	b->n = 0;
	b->seq = 0;
	forget_tracking(b);
}
