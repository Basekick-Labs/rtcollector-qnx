#define _GNU_SOURCE /* memmem, strcasestr */
#include "fake_arc.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zstd.h>

typedef struct {
	uint8_t *body;
	size_t len;
	int zstd;
	char headers[2048];
} req;

struct fake_arc {
	int lfd, port;
	pthread_t thr;
	_Atomic bool stop;
	pthread_mutex_t mu;
	fake_reply script[FAKE_MAX_REQ];
	size_t nscript, next;
	req reqs[FAKE_MAX_REQ];
	size_t nreq;
};

static ssize_t read_some(int fd, char *buf, size_t cap, _Atomic bool *stop)
{
	for (;;) {
		struct pollfd p = {fd, POLLIN, 0};
		int r = poll(&p, 1, 100);
		if (atomic_load(stop))
			return -1;
		if (r > 0)
			return read(fd, buf, cap);
	}
}

static void handle_conn(fake_arc *f, int fd)
{
	static char buf[1 << 22];
	size_t have = 0;
	for (;;) {
		char *eoh;
		while (!(eoh = memmem(buf, have, "\r\n\r\n", 4))) {
			ssize_t n = read_some(fd, buf + have, sizeof(buf) - have, &f->stop);
			if (n <= 0)
				return;
			have += (size_t)n;
		}
		size_t hlen = (size_t)(eoh - buf) + 4;
		size_t clen = 0;
		char *cl = strcasestr(buf, "Content-Length:");
		if (cl && cl < eoh)
			clen = strtoul(cl + 15, NULL, 10);
		while (have < hlen + clen) {
			ssize_t n = read_some(fd, buf + have, sizeof(buf) - have, &f->stop);
			if (n <= 0)
				return;
			have += (size_t)n;
		}
		pthread_mutex_lock(&f->mu);
		fake_reply rep = {204, ""};
		if (f->next < f->nscript)
			rep = f->script[f->next++];
		if (f->nreq < FAKE_MAX_REQ) {
			req *r = &f->reqs[f->nreq++];
			size_t hl = hlen < sizeof(r->headers) - 1 ? hlen : sizeof(r->headers) - 1;
			memcpy(r->headers, buf, hl);
			r->headers[hl] = 0;
			const uint8_t *b = (const uint8_t *)buf + hlen;
			r->zstd = clen >= 4 && b[0] == 0x28 && b[1] == 0xb5 && b[2] == 0x2f && b[3] == 0xfd;
			if (r->zstd) {
				unsigned long long sz = ZSTD_getFrameContentSize(b, clen);
				if (sz != ZSTD_CONTENTSIZE_ERROR && sz != ZSTD_CONTENTSIZE_UNKNOWN) {
					r->body = malloc(sz ? sz : 1);
					r->len = ZSTD_decompress(r->body, sz, b, clen);
					if (ZSTD_isError(r->len))
						r->len = 0;
				}
			} else {
				r->body = malloc(clen ? clen : 1);
				memcpy(r->body, b, clen);
				r->len = clen;
			}
		}
		pthread_mutex_unlock(&f->mu);

		char out[4096];
		int n = snprintf(
		    out, sizeof(out),
		    "HTTP/1.1 %d X\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
		    rep.status, strlen(rep.body), rep.body);
		if (write(fd, out, (size_t)n) != n)
			return;
		memmove(buf, buf + hlen + clen, have - hlen - clen);
		have -= hlen + clen;
	}
}

static void *serve(void *arg)
{
	fake_arc *f = arg;
	while (!atomic_load(&f->stop)) {
		struct pollfd p = {f->lfd, POLLIN, 0};
		if (poll(&p, 1, 100) <= 0)
			continue;
		int fd = accept(f->lfd, NULL, NULL);
		if (fd < 0)
			continue;
		handle_conn(f, fd);
		close(fd);
	}
	return NULL;
}

fake_arc *fake_arc_start(const fake_reply *script, size_t n)
{
	fake_arc *f = calloc(1, sizeof(*f));
	if (!f)
		return NULL;
	pthread_mutex_init(&f->mu, NULL);
	memcpy(f->script, script, n * sizeof(*script));
	f->nscript = n;
	f->lfd = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(f->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(f->lfd, (struct sockaddr *)&a, sizeof(a)) || listen(f->lfd, 8)) {
		close(f->lfd);
		free(f);
		return NULL;
	}
	socklen_t al = sizeof(a);
	getsockname(f->lfd, (struct sockaddr *)&a, &al);
	f->port = ntohs(a.sin_port);
	pthread_create(&f->thr, NULL, serve, f);
	return f;
}

int fake_arc_port(const fake_arc *f)
{
	return f->port;
}

void fake_arc_stop(fake_arc *f)
{
	atomic_store(&f->stop, true);
	pthread_join(f->thr, NULL);
	close(f->lfd);
	for (size_t i = 0; i < f->nreq; i++)
		free(f->reqs[i].body);
	pthread_mutex_destroy(&f->mu);
	free(f);
}

size_t fake_arc_requests(fake_arc *f)
{
	pthread_mutex_lock(&f->mu);
	size_t n = f->nreq;
	pthread_mutex_unlock(&f->mu);
	return n;
}

const uint8_t *fake_arc_body(fake_arc *f, size_t i, size_t *len, int *was_zstd)
{
	pthread_mutex_lock(&f->mu);
	const uint8_t *b = i < f->nreq ? f->reqs[i].body : NULL;
	*len = i < f->nreq ? f->reqs[i].len : 0;
	if (was_zstd)
		*was_zstd = i < f->nreq ? f->reqs[i].zstd : 0;
	pthread_mutex_unlock(&f->mu);
	return b;
}

const char *fake_arc_header(fake_arc *f, size_t i, const char *name)
{
	static char val[512];
	val[0] = 0;
	pthread_mutex_lock(&f->mu);
	if (i < f->nreq) {
		const char *h = strcasestr(f->reqs[i].headers, name);
		if (h) {
			h += strlen(name);
			while (*h == ':' || *h == ' ')
				h++;
			size_t n = strcspn(h, "\r\n");
			if (n >= sizeof(val))
				n = sizeof(val) - 1;
			memcpy(val, h, n);
			val[n] = 0;
		}
	}
	pthread_mutex_unlock(&f->mu);
	return val;
}
