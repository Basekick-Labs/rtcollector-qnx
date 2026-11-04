/* A scripted MQTT 3.1.1 broker standing in for mosquitto in tests. One
 * connection at a time; it records every packet it receives. */
#ifndef RTC_FAKE_BROKER_H
#define RTC_FAKE_BROKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FB_MAX_PUB 256

enum fb_action {
	FB_ACK = 0,   /* PUBACK */
	FB_CLOSE,     /* close after reading the whole PUBLISH */
	FB_SILENT,    /* never answer, keep reading */
	FB_CLOSE_HDR, /* close as soon as the fixed header is read (mosquitto's size limit) */
};

typedef struct {
	/* CONNACK code for connection k (0 after the array, or past nconnack). */
	uint8_t connack[8];
	unsigned nconnack;
	bool session_present;
	bool no_connack; /* accept and never answer CONNECT */
	/* What to do with data PUBLISH k (counted over all connections; presence
	 * messages on status_topic are always acknowledged). FB_ACK after. */
	enum fb_action action[64];
	unsigned naction;
	/* Over this PUBLISH size (0 = none): FB_CLOSE_HDR. */
	size_t max_packet;
	bool no_pingresp;
	const char *status_topic; /* presence topic, not a data publish */
} fb_script;

typedef struct {
	char topic[256];
	uint8_t *payload;
	size_t len;
	bool retain;
	uint8_t flags; /* fixed header low nibble */
	uint16_t id;
	unsigned conn; /* connection number, from 1 */
} fb_pub;

typedef struct fake_broker fake_broker;

fake_broker *fake_broker_start(const fb_script *s);
int fake_broker_port(const fake_broker *b);
void fake_broker_stop(fake_broker *b);

/* Snapshot accessors (thread-safe). */
unsigned fb_connects(fake_broker *b);
unsigned fb_disconnects(fake_broker *b); /* DISCONNECT packets */
unsigned fb_pings(fake_broker *b);
size_t fb_npubs(fake_broker *b); /* every PUBLISH, presence included */
/* PUBLISH i (copied into *out; payload valid until stop). */
bool fb_pub_get(fake_broker *b, size_t i, fb_pub *out);
/* The last CONNECT packet's body (after the fixed header). */
size_t fb_last_connect(fake_broker *b, uint8_t *out, size_t cap);
void fake_broker_pause_reads(fake_broker *b, bool on);
/* Closes the current connection (as a broker restart would). */
void fake_broker_drop_client(fake_broker *b);
/* Is a client connected right now? */
bool fb_connected(fake_broker *b);

#endif
