#include "fake_broker.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct fake_broker {
	int lfd, port;
	pthread_t thr;
	_Atomic bool stop;
	_Atomic bool paused; /* stop reading (a broker that fell behind) */
	_Atomic bool drop;   /* close the current connection */
	pthread_mutex_t mu;
	fb_script s;
	unsigned conns, disconnects, pings, data_pubs;
	bool connected;
	fb_pub pubs[FB_MAX_PUB];
	size_t npubs;
	uint8_t connect[4096];
	size_t connect_len;
};

/* Reads exactly n bytes; false on EOF, error or stop. */
static bool read_n(fake_broker *b, int fd, uint8_t *p, size_t n)
{
	while (n) {
		if (atomic_load(&b->stop) || atomic_exchange(&b->drop, false))
			return false;
		if (atomic_load(&b->paused)) {
			struct timespec ts = {0, 10000000};
			nanosleep(&ts, NULL);
			continue;
		}
		struct pollfd pf = {fd, POLLIN, 0};
		int r = poll(&pf, 1, 50);
		if (r <= 0)
			continue;
		ssize_t k = read(fd, p, n);
		if (k <= 0)
			return false;
		p += k;
		n -= (size_t)k;
	}
	return true;
}

static void reply(int fd, const uint8_t *p, size_t n)
{
	(void)!write(fd, p, n);
}

static void handle(fake_broker *b, int fd)
{
	static uint8_t body[1 << 21];
	pthread_mutex_lock(&b->mu);
	unsigned conn = ++b->conns;
	b->connected = true;
	pthread_mutex_unlock(&b->mu);
	bool silent = false;
	for (;;) {
		uint8_t h;
		if (!read_n(b, fd, &h, 1))
			break;
		uint32_t rl = 0;
		int shift = 0;
		uint8_t vb;
		bool ok = true;
		for (int i = 0; i < 4; i++) {
			if (!(ok = read_n(b, fd, &vb, 1)))
				break;
			rl |= (uint32_t)(vb & 0x7f) << shift;
			shift += 7;
			if (!(vb & 0x80))
				break;
		}
		if (!ok)
			break;
		uint8_t type = h >> 4;
		size_t size = 1 + (size_t)(shift / 7) + rl;
		if (type == 3 && b->s.max_packet && size > b->s.max_packet)
			break; /* like mosquitto: refuse by the header */
		pthread_mutex_lock(&b->mu);
		enum fb_action act = FB_ACK;
		pthread_mutex_unlock(&b->mu);
		if (rl > sizeof(body))
			break;
		if (!read_n(b, fd, body, rl))
			break;
		if (type == 1) { /* CONNECT */
			pthread_mutex_lock(&b->mu);
			b->connect_len = rl < sizeof(b->connect) ? rl : sizeof(b->connect);
			memcpy(b->connect, body, b->connect_len);
			uint8_t code = conn - 1 < b->s.nconnack ? b->s.connack[conn - 1] : 0;
			bool no = b->s.no_connack;
			bool sp = b->s.session_present;
			pthread_mutex_unlock(&b->mu);
			if (!no) {
				uint8_t ack[4] = {0x20, 2, sp ? 1 : 0, code};
				reply(fd, ack, 4);
			}
			if (code)
				break;
		} else if (type == 3) { /* PUBLISH */
			uint16_t tl = (uint16_t)(body[0] << 8 | body[1]);
			fb_pub p = {0};
			snprintf(p.topic, sizeof(p.topic), "%.*s", (int)tl, (const char *)body + 2);
			p.flags = h & 0x0f;
			p.retain = h & 1;
			p.id = (uint16_t)(body[2 + tl] << 8 | body[3 + tl]);
			p.len = rl - 4 - tl;
			p.payload = malloc(p.len + 1);
			memcpy(p.payload, body + 4 + tl, p.len);
			p.payload[p.len] = 0;
			p.conn = conn;
			pthread_mutex_lock(&b->mu);
			bool presence = b->s.status_topic && !strcmp(p.topic, b->s.status_topic);
			if (!presence) {
				unsigned k = b->data_pubs++;
				act = k < b->s.naction ? b->s.action[k] : FB_ACK;
			}
			if (b->npubs < FB_MAX_PUB)
				b->pubs[b->npubs++] = p;
			else
				free(p.payload);
			pthread_mutex_unlock(&b->mu);
			if (act == FB_CLOSE || act == FB_CLOSE_HDR)
				break;
			if (act == FB_SILENT)
				silent = true;
			if (!silent) {
				uint8_t ack[4] = {0x40, 2, (uint8_t)(p.id >> 8), (uint8_t)p.id};
				reply(fd, ack, 4);
			}
		} else if (type == 12) { /* PINGREQ */
			pthread_mutex_lock(&b->mu);
			b->pings++;
			bool no = b->s.no_pingresp;
			pthread_mutex_unlock(&b->mu);
			if (!no && !silent) {
				uint8_t r[2] = {0xd0, 0};
				reply(fd, r, 2);
			}
		} else if (type == 14) { /* DISCONNECT */
			pthread_mutex_lock(&b->mu);
			b->disconnects++;
			pthread_mutex_unlock(&b->mu);
			break;
		} else
			break;
	}
	close(fd);
	pthread_mutex_lock(&b->mu);
	b->connected = false;
	pthread_mutex_unlock(&b->mu);
}

static void *run(void *arg)
{
	fake_broker *b = arg;
	while (!atomic_load(&b->stop)) {
		struct pollfd p = {b->lfd, POLLIN, 0};
		if (poll(&p, 1, 50) <= 0)
			continue;
		int fd = accept(b->lfd, NULL, NULL);
		if (fd >= 0)
			handle(b, fd);
	}
	return NULL;
}

fake_broker *fake_broker_start(const fb_script *s)
{
	fake_broker *b = calloc(1, sizeof(*b));
	if (!b)
		return NULL;
	b->s = *s;
	pthread_mutex_init(&b->mu, NULL);
	b->lfd = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1, small = 4096;
	setsockopt(b->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	/* Small receive buffers (inherited by accepted sockets): a paused broker
	 * stops a large PUBLISH part-way. */
	setsockopt(b->lfd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	socklen_t al = sizeof(a);
	if (bind(b->lfd, (struct sockaddr *)&a, sizeof(a)) || listen(b->lfd, 4) ||
	    getsockname(b->lfd, (struct sockaddr *)&a, &al)) {
		close(b->lfd);
		free(b);
		return NULL;
	}
	b->port = ntohs(a.sin_port);
	pthread_create(&b->thr, NULL, run, b);
	return b;
}

int fake_broker_port(const fake_broker *b)
{
	return b->port;
}

void fake_broker_stop(fake_broker *b)
{
	if (!b)
		return;
	atomic_store(&b->stop, true);
	pthread_join(b->thr, NULL);
	close(b->lfd);
	for (size_t i = 0; i < b->npubs; i++)
		free(b->pubs[i].payload);
	pthread_mutex_destroy(&b->mu);
	free(b);
}

#define GET(field)                                                                                           \
	pthread_mutex_lock(&b->mu);                                                                          \
	unsigned v = b->field;                                                                               \
	pthread_mutex_unlock(&b->mu);                                                                        \
	return v

unsigned fb_connects(fake_broker *b)
{
	GET(conns);
}

unsigned fb_disconnects(fake_broker *b)
{
	GET(disconnects);
}

unsigned fb_pings(fake_broker *b)
{
	GET(pings);
}

size_t fb_npubs(fake_broker *b)
{
	pthread_mutex_lock(&b->mu);
	size_t n = b->npubs;
	pthread_mutex_unlock(&b->mu);
	return n;
}

bool fb_pub_get(fake_broker *b, size_t i, fb_pub *out)
{
	pthread_mutex_lock(&b->mu);
	bool ok = i < b->npubs;
	if (ok)
		*out = b->pubs[i];
	pthread_mutex_unlock(&b->mu);
	return ok;
}

size_t fb_last_connect(fake_broker *b, uint8_t *out, size_t cap)
{
	pthread_mutex_lock(&b->mu);
	size_t n = b->connect_len < cap ? b->connect_len : cap;
	memcpy(out, b->connect, n);
	pthread_mutex_unlock(&b->mu);
	return n;
}

bool fb_connected(fake_broker *b)
{
	pthread_mutex_lock(&b->mu);
	bool c = b->connected;
	pthread_mutex_unlock(&b->mu);
	return c;
}

void fake_broker_pause_reads(fake_broker *b, bool on)
{
	atomic_store(&b->paused, on);
}

void fake_broker_drop_client(fake_broker *b)
{
	atomic_store(&b->drop, true);
}
