/* A scripted HTTP server standing in for Arc in tests. Like Arc, it detects
 * compression from the body's magic bytes; it decompresses zstd and keeps the
 * MessagePack bodies it received. */
#ifndef RTC_FAKE_ARC_H
#define RTC_FAKE_ARC_H

#include <stddef.h>
#include <stdint.h>

#define FAKE_MAX_REQ 64

typedef struct {
	int status;
	const char *body;
} fake_reply;

typedef struct fake_arc fake_arc;

/* Starts on 127.0.0.1 with an ephemeral port. Replies are consumed in order;
 * after the script runs out every request gets 204. */
fake_arc *fake_arc_start(const fake_reply *script, size_t n);
int fake_arc_port(const fake_arc *f);
void fake_arc_stop(fake_arc *f);

size_t fake_arc_requests(fake_arc *f);
/* Decompressed body of request i (copied), its length, and whether the raw
 * body started with the zstd magic. */
const uint8_t *fake_arc_body(fake_arc *f, size_t i, size_t *len, int *was_zstd);
/* Header value of request i ("" if absent). */
const char *fake_arc_header(fake_arc *f, size_t i, const char *name);

#endif
