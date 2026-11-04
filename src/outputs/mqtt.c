/* mqtt: rows to an MQTT 3.1.1 broker, QoS 1, one message in flight.
 *
 * One PUBLISH per measurement: an array of {"m", "t", "tags", "fields"} rows
 * in MessagePack (Arc's format) or JSON, on the topic template with {host}
 * (the configured host, never a record's) and {measurement}.
 *
 * Nothing is in flight when send() returns: it publishes one message, waits
 * for its PUBACK and decides every record of the message itself.
 *
 *   PUBACK                               sent
 *   connect, CONNACK or write failure    kept, retried with backoff
 *   closed or timed out after the whole  kept and sent again (records_resent):
 *     PUBLISH was written                  at least once, duplicates possible
 *   shutdown deadline after the whole    outcome unknown, as for Arc
 *     PUBLISH was written
 *   shutdown deadline before that        kept, then counted as dropped
 *
 * MQTT has no "too large" answer: a broker closes the connection. After a
 * close during or after a PUBLISH of S bytes, messages are cut at S/2 bytes,
 * growing back after 32 acknowledged messages. A message of one row closed 3
 * times in a row, each time on a fresh connection, is refused (the broker's
 * packet limit, an ACL or another client with our id).
 *
 * Presence (status_topic, not under --once): the CONNECT's will is a
 * retained "offline"; after CONNACK a retained "online"; at a clean stop a
 * retained "offline" then DISCONNECT. Out of time, the connection is just
 * closed and the broker publishes the will.
 */
#include "../core/collector.h"
#include "../core/log.h"
#include "../core/msgpack.h"
#include "../core/mqtt.h"
#include "row.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* macOS: SO_NOSIGPIPE on the socket instead */
#endif

#define N         "mqtt"
#define MAX_ADDRS 8
/* Room for the PUBLISH header in front of the payload: fixed header (5),
 * topic length (2), topic, packet id (2). */
#define HDR_ROOM       (5 + 2 + RTC_MQTT_TOPIC_MAX + 2)
#define CEILING_MIN    64u /* below any row: messages can go down to one row */
#define GROW_AFTER     32u
#define ONE_ROW_CLOSES 3u

enum { FLAG_SEEN = 0x01, FLAG_RESEND = 0x02 };

/* Results of the socket helpers. */
enum { IO_OK = 0, IO_ERR = -1, IO_LATE = -2, IO_TIMEOUT = -3, IO_CLOSED = -4 };

typedef struct {
	int fd;
	bool connected;
	bool fresh; /* no data PUBLISH acknowledged on this connection yet */
	struct sockaddr_storage addr[MAX_ADDRS];
	socklen_t addr_len[MAX_ADDRS];
	unsigned naddr;
	bool numeric; /* the host is an address: never resolved again */

	rtc_mqtt_rx rx;
	uint16_t last_id;
	uint64_t last_tx_ns;
	uint64_t ping_sent_ns; /* 0: no PINGREQ outstanding */

	const char *host; /* the configured host: {host} */
	char client_id[128];
	bool presence;
	char status_topic[RTC_MQTT_TOPIC_MAX + 1];
	uint8_t connect_pkt[3072];
	size_t connect_len;

	uint8_t *msg; /* HDR_ROOM, then the payload */
	size_t payload_cap;
	uint8_t *in_msg; /* per record: in the message being sent */
	rtc_row_cols cols;
	char topic[RTC_MQTT_TOPIC_MAX + 1];

	size_t ceiling; /* learned packet limit, max_message_bytes when none */
	unsigned acks_since_cut;
	bool ceiling_logged;
	uint64_t one_row_seq;
	unsigned one_row_closes;

	int last_logged; /* last connection problem logged (an IO_ or CONNACK code) */
	bool auth_logged;
	bool connected_logged;
} mqtt_impl;

/* --- time and sockets --------------------------------------------------- */

static bool out_of_time(rtc_output *o, uint64_t deadline_ns, uint64_t now)
{
	uint64_t abort_after = atomic_load(&o->collector->abort_after_ns);
	return (abort_after && now >= abort_after) || (deadline_ns && now >= deadline_ns);
}

/* Waits for fd to be ready for ev, in slices of at most 50 ms so the shutdown
 * deadline (abort_after_ns) is seen even when it is set during the wait. */
static int wait_io(rtc_output *o, int fd, short ev, uint64_t until, uint64_t deadline_ns)
{
	for (;;) {
		uint64_t now = rtc_mono_ns();
		if (out_of_time(o, deadline_ns, now))
			return IO_LATE;
		if (now >= until)
			return IO_TIMEOUT;
		uint64_t left = (until - now) / 1000000u;
		struct pollfd p = {.fd = fd, .events = ev};
		int r = poll(&p, 1, left > 50 ? 50 : (int)left + 1);
		if (r > 0)
			return IO_OK; /* also on POLLERR/POLLHUP: the next call reports it */
		if (r < 0 && errno != EINTR)
			return IO_ERR;
	}
}

static void close_conn(mqtt_impl *m)
{
	if (m->fd >= 0)
		close(m->fd);
	m->fd = -1;
	m->connected = false;
	m->ping_sent_ns = 0;
	rtc_mqtt_rx_init(&m->rx);
}

/* Writes all of p. *written: bytes that went out (a partial packet when this
 * fails). */
static int send_all(rtc_output *o, const uint8_t *p, size_t n, uint64_t until, uint64_t deadline_ns,
                    size_t *written)
{
	mqtt_impl *m = o->impl;
	size_t done = 0;
	int r = IO_OK;
	while (done < n) {
		ssize_t w = send(m->fd, p + done, n - done, MSG_NOSIGNAL);
		if (w > 0) {
			done += (size_t)w;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			r = wait_io(o, m->fd, POLLOUT, until, deadline_ns);
			if (r != IO_OK)
				break;
			continue;
		}
		r = (errno == EPIPE || errno == ECONNRESET) ? IO_CLOSED : IO_ERR;
		break;
	}
	if (written)
		*written = done;
	if (done == n) {
		m->last_tx_ns = rtc_mono_ns();
		return IO_OK;
	}
	return r;
}

/* Reads until a packet of type want (and, for a PUBACK, id) arrives. A
 * PINGRESP on the way is taken; anything else is a protocol error. */
static int wait_packet(rtc_output *o, uint8_t want, uint16_t id, uint64_t until, uint64_t deadline_ns,
                       rtc_mqtt_pkt *got)
{
	mqtt_impl *m = o->impl;
	for (;;) {
		uint8_t buf[64];
		ssize_t n = recv(m->fd, buf, sizeof(buf), 0);
		if (n == 0)
			return IO_CLOSED;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				int r = wait_io(o, m->fd, POLLIN, until, deadline_ns);
				if (r != IO_OK)
					return r;
				continue;
			}
			return errno == ECONNRESET ? IO_CLOSED : IO_ERR;
		}
		bool found = false;
		for (size_t at = 0; at < (size_t)n;) {
			size_t used;
			rtc_mqtt_pkt pk;
			int r = rtc_mqtt_rx_feed(&m->rx, buf + at, (size_t)n - at, &used, &pk);
			at += used;
			if (r < 0) {
				RTC_WARN(o->name, "protocol error from the broker; reconnecting");
				return IO_ERR;
			}
			if (r == 0)
				break;
			if (pk.type == RTC_MQTT_PINGRESP) {
				m->ping_sent_ns = 0;
			} else if (!found && pk.type == want && (want != RTC_MQTT_PUBACK || pk.id == id)) {
				*got = pk;
				found = true;
			} else {
				RTC_WARN(o->name,
				         "unexpected packet %u (id %u) from the broker; reconnecting",
				         pk.type, pk.id);
				if (!found)
					return IO_ERR;
				close_conn(m); /* what we waited for did arrive */
				break;
			}
		}
		if (found)
			return IO_OK;
	}
}

static void log_problem(rtc_output *o, int code, const char *fmt, const char *detail);

static void resolve(rtc_output *o)
{
	mqtt_impl *m = o->impl;
	const rtc_mqtt_cfg *q = &o->collector->cfg.mqtt;
	struct addrinfo hints = {
	    .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICHOST | AI_NUMERICSERV};
	struct addrinfo *res = NULL;
	int e = getaddrinfo(q->host, q->port, &hints, &res);
	m->numeric = e == 0;
	if (e != 0) {
		hints.ai_flags = AI_NUMERICSERV;
		e = getaddrinfo(q->host, q->port, &hints, &res);
	}
	m->naddr = 0;
	if (e != 0) {
		log_problem(o, 6000, "cannot resolve the host of %s: %s; retrying", gai_strerror(e));
		return;
	}
	for (struct addrinfo *a = res; a && m->naddr < MAX_ADDRS; a = a->ai_next)
		if (a->ai_addrlen <= sizeof(m->addr[0])) {
			memcpy(&m->addr[m->naddr], a->ai_addr, a->ai_addrlen);
			m->addr_len[m->naddr++] = (socklen_t)a->ai_addrlen;
		}
	freeaddrinfo(res);
}

/* A non-blocking connect to each address in turn. A refused connect shows
 * as writable with no POLLERR on io-sock: SO_ERROR is always read. */
static int tcp_connect(rtc_output *o, uint64_t until, uint64_t deadline_ns)
{
	mqtt_impl *m = o->impl;
	int last = IO_ERR;
	for (unsigned i = 0; i < m->naddr; i++) {
		/* Each address gets its share of what is left, so one that drops
		 * packets (IPv6 behind a firewall) leaves time for the next. */
		uint64_t now = rtc_mono_ns();
		uint64_t share = now < until ? now + (until - now) / (m->naddr - i) : until;
		int fd = socket(m->addr[i].ss_family, SOCK_STREAM, 0);
		if (fd < 0)
			continue;
		int fl = fcntl(fd, F_GETFL);
		if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
			close(fd);
			continue;
		}
		int one = 1;
		(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
		(void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
		int r = connect(fd, (struct sockaddr *)&m->addr[i], m->addr_len[i]);
		if (r != 0 && errno == EINPROGRESS) {
			r = wait_io(o, fd, POLLOUT, share, deadline_ns);
			if (r == IO_OK) {
				int err = 0;
				socklen_t l = sizeof(err);
				if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) != 0 || err != 0)
					r = IO_ERR;
			}
		} else if (r != 0)
			r = IO_ERR;
		if (r == IO_OK) {
			m->fd = fd;
			return IO_OK;
		}
		close(fd);
		last = r;
		if (r == IO_LATE)
			break;
	}
	return last;
}

/* PUBLISH of a small retained message (presence) and its PUBACK. */
static int publish_status(rtc_output *o, const char *payload, uint64_t until, uint64_t deadline_ns)
{
	mqtt_impl *m = o->impl;
	uint8_t pkt[HDR_ROOM + 16];
	size_t pl = strlen(payload);
	uint16_t id = rtc_mqtt_next_id(&m->last_id);
	size_t at =
	    rtc_mqtt_publish_header(pkt, HDR_ROOM, m->status_topic, strlen(m->status_topic), id, true, pl);
	if (at == HDR_ROOM)
		return IO_ERR;
	memcpy(pkt + HDR_ROOM, payload, pl);
	int r = send_all(o, pkt + at, HDR_ROOM - at + pl, until, deadline_ns, NULL);
	rtc_mqtt_pkt ack;
	return r == IO_OK ? wait_packet(o, RTC_MQTT_PUBACK, id, until, deadline_ns, &ack) : r;
}

static void log_problem(rtc_output *o, int code, const char *fmt, const char *detail)
{
	mqtt_impl *m = o->impl;
	if (m->last_logged == code)
		return;
	m->last_logged = code;
	RTC_WARN(o->name, fmt, o->collector->cfg.mqtt.url, detail);
}

static const char *io_str(int r)
{
	switch (r) {
	case IO_TIMEOUT:
		return "timed out";
	case IO_CLOSED:
		return "connection closed";
	case IO_LATE:
		return "out of time";
	default:
		return strerror(errno);
	}
}

/* Connects, sends CONNECT, waits for CONNACK, publishes presence. */
static int connect_broker(rtc_output *o, uint64_t deadline_ns)
{
	mqtt_impl *m = o->impl;
	rtc_collector *c = o->collector;
	const rtc_mqtt_cfg *q = &c->cfg.mqtt;
	uint64_t until = rtc_mono_ns() + (uint64_t)q->timeout_ms * 1000000u;
	/* Never during the final flush. A lookup that is running when shutdown
	 * starts is not cut short: getaddrinfo cannot be (README). */
	if (m->naddr == 0 && atomic_load(&c->abort_after_ns) == 0)
		resolve(o);
	if (m->naddr == 0)
		return IO_ERR;
	int r = tcp_connect(o, until, deadline_ns);
	if (r != IO_OK) {
		if (r != IO_LATE) {
			log_problem(o, 1000 + r, "cannot connect to %s: %s; retrying", io_str(r));
			if (!m->numeric && atomic_load(&c->abort_after_ns) == 0)
				m->naddr = 0; /* resolve again next time */
		}
		return r;
	}
	rtc_mqtt_rx_init(&m->rx);
	rtc_mqtt_pkt ack;
	r = send_all(o, m->connect_pkt, m->connect_len, until, deadline_ns, NULL);
	if (r == IO_OK)
		r = wait_packet(o, RTC_MQTT_CONNACK, 0, until, deadline_ns, &ack);
	if (r != IO_OK) {
		if (r != IO_LATE)
			log_problem(o, 2000 + r, "no CONNACK from %s: %s; retrying", io_str(r));
		close_conn(m);
		return r;
	}
	rtc_counter_set(&o->counters.last_status, ack.code);
	if (ack.code != 0) {
		if (!m->auth_logged) {
			m->auth_logged = true;
			RTC_ERROR(o->name,
			          "%s refused the connection: %s (%u)%s; keeping the data and retrying",
			          q->url, rtc_mqtt_connack_str(ack.code), ack.code,
			          ack.code == 4 || ack.code == 5 ? "; check username and password" : "");
		}
		close_conn(m);
		return IO_ERR;
	}
	if (ack.session_present) { /* we asked for a clean session */
		RTC_WARN(o->name, "the broker resumed a session we did not ask for; reconnecting");
		close_conn(m);
		return IO_ERR;
	}
	m->connected = true;
	m->fresh = true;
	m->auth_logged = false;
	if (m->presence) {
		r = publish_status(o, "online", until, deadline_ns);
		if (r != IO_OK) {
			if (r != IO_LATE && m->last_logged != 3000 + r) {
				m->last_logged = 3000 + r;
				RTC_ERROR(o->name,
				          "%s does not take the presence message on %s (%s): no data is sent "
				          "until it does. Allow that topic, or set status_topic: '' to run "
				          "without presence",
				          q->url, m->status_topic, io_str(r));
			}
			close_conn(m);
			return r;
		}
	}
	m->last_logged = 0;
	if (!m->connected_logged) {
		m->connected_logged = true;
		RTC_INFO(o->name, "connected to %s as %s", q->url, m->client_id);
	}
	return IO_OK;
}

/* --- rows ----------------------------------------------------------------- */

typedef struct {
	uint8_t *p;
	size_t cap, len;
	bool failed;
} jw;

static void jput(jw *w, const char *s, size_t n)
{
	if (w->failed || n > w->cap - w->len) {
		w->failed = true;
		return;
	}
	memcpy(w->p + w->len, s, n);
	w->len += n;
}

static void jputs(jw *w, const char *s)
{
	jput(w, s, strlen(s));
}

/* A JSON string (RFC 8259): quotes, backslash and control characters
 * escaped. Values are UTF-8 (checked before). */
static void jstr(jw *w, const char *s, size_t n)
{
	jput(w, "\"", 1);
	size_t run = 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char ch = (unsigned char)s[i];
		if (ch >= 0x20 && ch != '"' && ch != '\\')
			continue;
		jput(w, s + run, i - run);
		char e[8];
		if (ch == '"' || ch == '\\')
			snprintf(e, sizeof(e), "\\%c", ch);
		else
			snprintf(e, sizeof(e), "\\u%04x", ch);
		jputs(w, e);
		run = i + 1;
	}
	jput(w, s + run, n - run);
	jput(w, "\"", 1);
}

/* A float that stays a float for typed readers: 3 is printed 3.0. */
static void jf64(jw *w, double v)
{
	char b[40];
	int n = snprintf(b, sizeof(b), "%.17g", v);
	if (n > 0 && !strpbrk(b, ".eE"))
		snprintf(b + n, sizeof(b) - (size_t)n, ".0");
	jputs(w, b);
}

static bool values_utf8(const rtc_rec_view *v)
{
	for (unsigned i = 0; i < v->ntags; i++)
		if (!rtc_utf8_valid(v->tag_v[i].p, v->tag_v[i].len))
			return false;
	for (unsigned i = 0; i < v->nfields; i++)
		if (v->fields[i].type == RTC_F_STR &&
		    !rtc_utf8_valid(v->fields[i].v.s.p, v->fields[i].v.s.len))
			return false;
	return true;
}

static bool finite_field(const rtc_field *f)
{
	return f->type != RTC_F_F64 || isfinite(f->v.f);
}

/* One row at out[0..cap). Returns its length, or 0 when it does not fit. */
static size_t encode_row(rtc_output *o, const rtc_rec_view *v, uint64_t ts_ns, const rtc_row_inj *in,
                         unsigned nin, uint8_t *out, size_t cap)
{
	const rtc_config *cfg = &o->collector->cfg;
	int64_t t = (int64_t)(ts_ns / cfg->mqtt.time_div);
	unsigned ntag = nin, nfield = 0;
	for (unsigned i = 0; i < v->ntags; i++)
		ntag += !rtc_row_is_injected(in, nin, v->tag_k[i]);
	for (unsigned i = 0; i < v->nfields; i++)
		nfield += finite_field(&v->fields[i]);

	if (!cfg->mqtt.json) {
		rtc_mp w;
		rtc_mp_init(&w, out, cap);
		rtc_mp_map(&w, 4);
		rtc_mp_cstr(&w, "m");
		rtc_mp_str(&w, v->measurement.p, v->measurement.len);
		rtc_mp_cstr(&w, "t");
		rtc_mp_int(&w, t);
		rtc_mp_cstr(&w, "tags");
		rtc_mp_map(&w, ntag);
		for (unsigned i = 0; i < nin; i++) {
			size_t vl;
			const char *val = rtc_row_inj_value(&in[i], v, cfg->hostname, &vl);
			rtc_mp_str(&w, in[i].k, in[i].kl);
			rtc_mp_str(&w, val, vl);
		}
		for (unsigned i = 0; i < v->ntags; i++)
			if (!rtc_row_is_injected(in, nin, v->tag_k[i])) {
				rtc_mp_str(&w, v->tag_k[i].p, v->tag_k[i].len);
				rtc_mp_str(&w, v->tag_v[i].p, v->tag_v[i].len);
			}
		rtc_mp_cstr(&w, "fields");
		rtc_mp_map(&w, nfield);
		for (unsigned i = 0; i < v->nfields; i++) {
			const rtc_field *f = &v->fields[i];
			if (!finite_field(f))
				continue;
			rtc_mp_str(&w, f->key.p, f->key.len);
			switch (f->type) {
			case RTC_F_I64:
				rtc_mp_int(&w, f->v.i);
				break;
			case RTC_F_F64:
				rtc_mp_f64(&w, f->v.f);
				break;
			case RTC_F_BOOL:
				rtc_mp_bool(&w, f->v.b);
				break;
			default:
				rtc_mp_str(&w, f->v.s.p, f->v.s.len);
			}
		}
		return rtc_mp_ok(&w) ? w.len : 0;
	}

	jw w = {out, cap, 0, false};
	jputs(&w, "{\"m\":");
	jstr(&w, v->measurement.p, v->measurement.len);
	char num[32];
	snprintf(num, sizeof(num), ",\"t\":%" PRId64 ",\"tags\":{", t);
	jputs(&w, num);
	bool first = true;
	for (unsigned i = 0; i < nin; i++) {
		size_t vl;
		const char *val = rtc_row_inj_value(&in[i], v, cfg->hostname, &vl);
		if (!first)
			jputs(&w, ",");
		first = false;
		jstr(&w, in[i].k, in[i].kl);
		jputs(&w, ":");
		jstr(&w, val, vl);
	}
	for (unsigned i = 0; i < v->ntags; i++)
		if (!rtc_row_is_injected(in, nin, v->tag_k[i])) {
			jputs(&w, ",");
			jstr(&w, v->tag_k[i].p, v->tag_k[i].len);
			jputs(&w, ":");
			jstr(&w, v->tag_v[i].p, v->tag_v[i].len);
		}
	jputs(&w, "},\"fields\":{");
	first = true;
	for (unsigned i = 0; i < v->nfields; i++) {
		const rtc_field *f = &v->fields[i];
		if (!finite_field(f))
			continue;
		if (!first)
			jputs(&w, ",");
		first = false;
		jstr(&w, f->key.p, f->key.len);
		jputs(&w, ":");
		switch (f->type) {
		case RTC_F_I64:
			snprintf(num, sizeof(num), "%" PRId64, f->v.i);
			jputs(&w, num);
			break;
		case RTC_F_F64:
			jf64(&w, f->v.f);
			break;
		case RTC_F_BOOL:
			jputs(&w, f->v.b ? "true" : "false");
			break;
		default:
			jstr(&w, f->v.s.p, f->v.s.len);
		}
	}
	jputs(&w, "}}");
	return w.failed ? 0 : w.len;
}

static size_t remaining(const rtc_output *o)
{
	size_t n = 0;
	for (size_t i = 0; i < o->nrecs; i++)
		n += !o->done[i];
	return n;
}

static void reject(rtc_output *o, size_t i)
{
	o->done[i] = 1;
	rtc_counter_add(&o->counters.records_rejected, 1);
}

/* Builds the next message: rows of the first unsent record's measurement,
 * up to the learned ceiling. Returns the payload length (at msg + HDR_ROOM)
 * and the row count in *nrows; 0 rows when nothing is left to send. */
static size_t build_message(rtc_output *o, size_t *nrows)
{
	mqtt_impl *m = o->impl;
	rtc_collector *c = o->collector;
	const rtc_mqtt_cfg *q = &c->cfg.mqtt;
	rtc_row_inj in[RTC_CFG_MAX_TAGS + 1];
	unsigned nin = rtc_row_injected(&c->cfg, in);
	uint8_t *pay = m->msg + HDR_ROOM;
	*nrows = 0;
	memset(m->in_msg, 0, o->nrecs);

	rtc_rec_view v;
	size_t first = 0;
	for (; first < o->nrecs; first++) {
		if (o->done[first])
			continue;
		if (rtc_rec_decode(o->recs[first].payload, o->recs[first].len, &v) &&
		    rtc_valid_arc_name(v.measurement.p, v.measurement.len, 128))
			break;
		rtc_counter_add(&o->counters.invalid_names, 1);
		reject(o, first);
	}
	if (first == o->nrecs)
		return 0;
	char mname[129];
	memcpy(mname, v.measurement.p, v.measurement.len);
	mname[v.measurement.len] = '\0';
	size_t ml = v.measurement.len;
	if (rtc_mqtt_topic_expand(q->topic, m->host, mname, ml, m->topic, sizeof(m->topic)) < 0) {
		reject(o, first); /* validated at load: cannot happen */
		return 0;
	}
	/* The payload room under the hard limit and under the learned ceiling,
	 * with the largest PUBLISH header for this topic. */
	size_t ovh = RTC_MQTT_PUBLISH_OVERHEAD + strlen(m->topic);
	size_t hard = q->max_message_bytes - ovh; /* > 0: checked at load */
	size_t soft = m->ceiling > ovh ? m->ceiling - ovh : 0;
	if (soft > hard)
		soft = hard;

	size_t head = q->json ? 1 : 5; /* '[' or an array32 header, filled at the end */
	size_t len = head;
	m->cols.ncols = 0;
	for (size_t i = first; i < o->nrecs; i++) {
		if (o->done[i])
			continue;
		if (!rtc_rec_decode(o->recs[i].payload, o->recs[i].len, &v)) {
			rtc_counter_add(&o->counters.invalid_names, 1);
			reject(o, i);
			continue;
		}
		if (v.measurement.len != ml || memcmp(v.measurement.p, mname, ml))
			continue; /* another message */
		bool first_time = !(o->flags[i] & FLAG_SEEN);
		o->flags[i] |= FLAG_SEEN;
		if (!rtc_row_admit(&m->cols, &v, in, nin, &o->counters, first_time) ||
		    (q->json && !values_utf8(&v))) {
			reject(o, i);
			continue;
		}
		size_t sep = q->json && *nrows ? 1 : 0;
		size_t need = len + sep + (q->json ? 1 : 0); /* JSON: keep a byte for ']' */
		size_t rl = need < hard
		                ? encode_row(o, &v, o->recs[i].ts_ns, in, nin, pay + len + sep, hard - need)
		                : 0;
		if (rl == 0 || (*nrows && need + rl > soft)) {
			if (*nrows)
				break; /* the next message */
			/* Alone and over max_message_bytes: it can never be sent. */
			RTC_WARN(o->name, "a %.*s row does not fit in max_message_bytes (%zu); dropped",
			         (int)ml, mname, q->max_message_bytes);
			reject(o, i);
			continue;
		}
		if (sep)
			pay[len] = ',';
		len += sep + rl;
		m->in_msg[i] = 1;
		(*nrows)++;
	}
	if (*nrows == 0)
		return 0;
	if (q->json) {
		pay[0] = '[';
		pay[len++] = ']';
	} else {
		uint32_t n = (uint32_t)*nrows;
		pay[0] = 0xdd; /* array32 */
		pay[1] = (uint8_t)(n >> 24);
		pay[2] = (uint8_t)(n >> 16);
		pay[3] = (uint8_t)(n >> 8);
		pay[4] = (uint8_t)n;
	}
	return len;
}

/* A close during or after a PUBLISH of size bytes: the broker may refuse
 * messages that large. True when the ceiling went down (at most about 20
 * times from max_message_bytes to one row). */
static bool learn_ceiling(rtc_output *o, size_t size)
{
	mqtt_impl *m = o->impl;
	size_t cut = size / 2 < CEILING_MIN ? CEILING_MIN : size / 2;
	m->acks_since_cut = 0;
	if (cut >= m->ceiling)
		return false;
	m->ceiling = cut;
	if (!m->ceiling_logged) {
		m->ceiling_logged = true;
		RTC_WARN(o->name,
		         "the broker closed the connection after a %zu-byte PUBLISH; sending messages of "
		         "at most %zu bytes. Check the broker's packet size limit, its ACL for %s and that "
		         "no other client uses the id %s",
		         size, cut, m->topic, m->client_id);
	}
	return true;
}

/* --- the output ------------------------------------------------------------ */

static enum rtc_send_outcome send_batch(rtc_output *o, uint64_t deadline_ns)
{
	mqtt_impl *m = o->impl;
	const rtc_mqtt_cfg *q = &o->collector->cfg.mqtt;
	size_t nrows;
	size_t plen = build_message(o, &nrows);
	if (nrows == 0)
		return remaining(o) ? RTC_SEND_AGAIN : RTC_SEND_DONE;
	if (out_of_time(o, deadline_ns, rtc_mono_ns()))
		return RTC_SEND_RETRY; /* not started: kept, the caller accounts for the rest */

	if (!m->connected) {
		int r = connect_broker(o, deadline_ns);
		if (r != IO_OK) {
			if (r != IO_LATE)
				rtc_counter_add(&o->counters.request_errors, 1);
			return RTC_SEND_RETRY; /* kept */
		}
	}
	bool fresh = m->fresh;
	uint16_t id = rtc_mqtt_next_id(&m->last_id);
	size_t at = rtc_mqtt_publish_header(m->msg, HDR_ROOM, m->topic, strlen(m->topic), id, false, plen);
	if (at == HDR_ROOM) { /* cannot happen: topic and size are bounded at load */
		for (size_t i = 0; i < o->nrecs; i++)
			if (m->in_msg[i])
				reject(o, i);
		return remaining(o) ? RTC_SEND_AGAIN : RTC_SEND_DONE;
	}
	size_t size = HDR_ROOM - at + plen;
	uint64_t until = rtc_mono_ns() + (uint64_t)q->timeout_ms * 1000000u;
	size_t written = 0;
	rtc_counter_add(&o->counters.requests, 1);
	int r = send_all(o, m->msg + at, size, until, deadline_ns, &written);
	rtc_mqtt_pkt ack;
	if (r == IO_OK) {
		for (size_t i = 0; i < o->nrecs; i++)
			if (m->in_msg[i] && (o->flags[i] & FLAG_RESEND)) {
				o->flags[i] &= (uint8_t)~FLAG_RESEND;
				rtc_counter_add(&o->counters.records_resent, 1);
			}
		r = wait_packet(o, RTC_MQTT_PUBACK, id, until, deadline_ns, &ack);
		if (r == IO_OK) {
			for (size_t i = 0; i < o->nrecs; i++)
				if (m->in_msg[i])
					o->done[i] = 1;
			rtc_counter_add(&o->counters.records_sent, nrows);
			m->fresh = false;
			m->one_row_closes = 0;
			if (m->ceiling < q->max_message_bytes && ++m->acks_since_cut >= GROW_AFTER) {
				m->acks_since_cut = 0;
				m->ceiling = m->ceiling > q->max_message_bytes / 2 ? q->max_message_bytes
				                                                   : m->ceiling * 2;
			}
			return remaining(o) ? RTC_SEND_AGAIN : RTC_SEND_DONE;
		}
		if (r == IO_LATE) {
			/* Cut off by the shutdown budget after the whole message went
			 * out: the broker may have it. */
			close_conn(m);
			for (size_t i = 0; i < o->nrecs; i++)
				if (m->in_msg[i])
					o->done[i] = 1;
			rtc_counter_add(&o->counters.records_outcome_unknown, nrows);
			RTC_WARN(o->name,
			         "shutdown: a message was cut off; %zu records may or may not be stored",
			         nrows);
			return remaining(o) ? RTC_SEND_AGAIN : RTC_SEND_DONE;
		}
		/* No PUBACK: send again on a new connection (a late PUBACK must
		 * never reach a later message). */
		for (size_t i = 0; i < o->nrecs; i++)
			if (m->in_msg[i])
				o->flags[i] |= FLAG_RESEND;
	}
	close_conn(m);
	if (r == IO_LATE)
		return RTC_SEND_RETRY; /* not all written: kept, dropped at shutdown */
	rtc_counter_add(&o->counters.request_errors, 1);
	log_problem(o, 4000 + r, "publish to %s failed: %s; retrying", io_str(r));
	if (r == IO_CLOSED && written > 0) {
		bool cut = learn_ceiling(o, size);
		size_t one = SIZE_MAX;
		for (size_t i = 0; i < o->nrecs && nrows == 1; i++)
			if (m->in_msg[i])
				one = i;
		if (one != SIZE_MAX && fresh) {
			if (m->one_row_seq != o->recs[one].seq)
				m->one_row_closes = 0;
			m->one_row_seq = o->recs[one].seq;
			if (++m->one_row_closes >= ONE_ROW_CLOSES) {
				m->one_row_closes = 0;
				o->flags[one] &= (uint8_t)~FLAG_RESEND;
				reject(o, one);
				RTC_ERROR(
				    o->name,
				    "the broker closed the connection %u times on a %zu-byte message of one "
				    "row on %s; inferring that it refuses it (packet size limit or ACL) and "
				    "dropping the row",
				    ONE_ROW_CLOSES, size, m->topic);
				return remaining(o) ? RTC_SEND_AGAIN : RTC_SEND_DONE;
			}
		} else
			m->one_row_closes = 0;
		if (cut)
			return RTC_SEND_AGAIN; /* smaller messages now, without a backoff */
	}
	return RTC_SEND_RETRY;
}

/* Keepalive while there is nothing to send. Never blocks. */
static void idle(rtc_output *o, uint64_t now)
{
	mqtt_impl *m = o->impl;
	const rtc_mqtt_cfg *q = &o->collector->cfg.mqtt;
	if (!m->connected)
		return;
	for (;;) {
		uint8_t buf[64];
		ssize_t n = recv(m->fd, buf, sizeof(buf), 0);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
			log_problem(o, 5000, "%s closed the connection: %s; reconnecting when there is data",
			            n == 0 ? "end of stream" : strerror(errno));
			close_conn(m);
			return;
		}
		if (n < 0)
			break;
		for (size_t at = 0; at < (size_t)n;) {
			size_t used;
			rtc_mqtt_pkt pk;
			int r = rtc_mqtt_rx_feed(&m->rx, buf + at, (size_t)n - at, &used, &pk);
			at += used;
			if (r < 0 || (r == 1 && pk.type != RTC_MQTT_PINGRESP)) {
				RTC_WARN(o->name, "unexpected data from the broker while idle; reconnecting");
				close_conn(m);
				return;
			}
			if (r == 0)
				break;
			m->ping_sent_ns = 0;
		}
	}
	if (m->ping_sent_ns && now - m->ping_sent_ns > (uint64_t)q->timeout_ms * 1000000u) {
		log_problem(o, 5001, "%s: no PINGRESP (%s); reconnecting when there is data", "timed out");
		close_conn(m);
		return;
	}
	if (q->keepalive_s && !m->ping_sent_ns &&
	    now - m->last_tx_ns >= (uint64_t)q->keepalive_s * 500000000u) {
		uint8_t ping[2];
		rtc_mqtt_pingreq(ping);
		ssize_t w = send(m->fd, ping, 2, MSG_NOSIGNAL);
		if (w == 2) {
			m->ping_sent_ns = now;
			m->last_tx_ns = now;
		} else if (w >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
			log_problem(o, 5002, "cannot send PINGREQ to %s: %s; reconnecting when there is data",
			            w >= 0 ? "partial write" : strerror(errno));
			close_conn(m); /* a partial PINGREQ breaks the stream */
		}
	}
}

/* After the final flush: retained offline, then DISCONNECT. */
static void finish(rtc_output *o, uint64_t deadline_ns)
{
	mqtt_impl *m = o->impl;
	if (!m->connected)
		return;
	uint64_t now = rtc_mono_ns();
	if (!out_of_time(o, deadline_ns, now)) {
		uint64_t until = now + (uint64_t)o->collector->cfg.mqtt.timeout_ms * 1000000u;
		if (!m->presence || publish_status(o, "offline", until, deadline_ns) == IO_OK) {
			uint8_t d[2];
			rtc_mqtt_disconnect(d);
			(void)send_all(o, d, 2, until, deadline_ns, NULL);
		}
	}
	close_conn(m); /* without DISCONNECT the broker publishes the will */
}

static void destroy(rtc_output *o)
{
	mqtt_impl *m = o->impl;
	if (m) {
		if (m->connected && !m->presence) { /* --once: nothing to announce */
			uint8_t d[2];
			rtc_mqtt_disconnect(d);
			(void)send(m->fd, d, 2, MSG_NOSIGNAL);
		}
		close_conn(m);
		free(m->msg);
		free(m->in_msg);
		free(m);
	}
	rtc_output_free_batch(o);
	free(o);
}

static uint32_t fnv1a(const char *s)
{
	uint32_t h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (unsigned char)*s) * 16777619u;
	return h;
}

static uint32_t random32(void)
{
	uint32_t r = 0;
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		ssize_t n = read(fd, &r, sizeof(r));
		close(fd);
		if (n == (ssize_t)sizeof(r))
			return r;
	}
	uint64_t t = rtc_now_ns() ^ rtc_mono_ns();
	return (uint32_t)(t ^ (t >> 32)) ^ (uint32_t)getpid() * 2654435761u;
}

int rtc_output_mqtt_create(rtc_collector *c, unsigned idx, rtc_output **out)
{
	const rtc_mqtt_cfg *q = &c->cfg.mqtt;
	rtc_output *o = calloc(1, sizeof(*o));
	mqtt_impl *m = calloc(1, sizeof(*m));
	if (!o || !m) {
		free(o);
		free(m);
		return -1;
	}
	m->fd = -1;
	o->name = N;
	o->idx = idx;
	o->collector = c;
	o->impl = m;
	o->send = send_batch;
	o->idle = idle;
	o->finish = finish;
	o->destroy = destroy;
	if (rtc_output_alloc_batch(o, q->batch_bytes, c->cfg.max_record_bytes) != 0)
		goto fail;
	m->payload_cap = q->max_message_bytes;
	m->msg = malloc(HDR_ROOM + m->payload_cap);
	m->in_msg = calloc(o->max_recs, 1);
	if (!m->msg || !m->in_msg)
		goto fail;
	m->ceiling = q->max_message_bytes;
	m->host = rtc_row_cfg_host(&c->cfg, c->cfg.hostname);
	rtc_mqtt_rx_init(&m->rx);

	if (q->client_id[0])
		snprintf(m->client_id, sizeof(m->client_id), "%s", q->client_id);
	else
		snprintf(m->client_id, sizeof(m->client_id), "rtc%08" PRIx32 "%08" PRIx32, fnv1a(m->host),
		         random32());
	m->presence = q->status_topic[0] && !c->cfg.once;
	if (m->presence && rtc_mqtt_topic_expand(q->status_topic, m->host, NULL, 0, m->status_topic,
	                                         sizeof(m->status_topic)) < 0)
		goto fail; /* validated at load */
	rtc_mqtt_connect_opts co = {.client_id = m->client_id,
	                            .username = q->username,
	                            .password = q->password,
	                            .keepalive_s = (uint16_t)q->keepalive_s,
	                            .will_topic = m->presence ? m->status_topic : NULL,
	                            .will_payload = "offline",
	                            .will_retain = true};
	m->connect_len = rtc_mqtt_connect(m->connect_pkt, sizeof(m->connect_pkt), &co);
	if (!m->connect_len)
		goto fail;
	resolve(o);
	if (q->password[0])
		RTC_WARN(N, "the password goes to %s in clear text (no TLS in this build)", q->url);
	RTC_INFO(N, "publishing to %s as client %s%s%s", q->url, m->client_id,
	         m->presence ? ", presence on " : "", m->presence ? m->status_topic : "");
	*out = o;
	return 0;
fail:
	RTC_ERROR(N, "cannot set up the MQTT output");
	destroy(o);
	return -1;
}
