/* MQTT 3.1.1 packets, the subset a QoS 1 publisher needs: CONNECT, PUBLISH
 * (QoS 1), PINGREQ and DISCONNECT out; CONNACK, PUBACK and PINGRESP in.
 * Encoding writes into caller buffers and never allocates. Decoding is
 * incremental (bytes may arrive one at a time or several packets at once)
 * and refuses anything else the broker could send: we never subscribe and
 * use a clean session, so any other packet is a protocol error. */
#ifndef RTC_MQTT_H
#define RTC_MQTT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	RTC_MQTT_CONNECT = 1,
	RTC_MQTT_CONNACK = 2,
	RTC_MQTT_PUBLISH = 3,
	RTC_MQTT_PUBACK = 4,
	RTC_MQTT_PINGREQ = 12,
	RTC_MQTT_PINGRESP = 13,
	RTC_MQTT_DISCONNECT = 14,
};

/* Largest remaining length (4 length bytes). */
#define RTC_MQTT_MAX_REMAINING 268435455u
/* A PUBLISH fixed header plus topic length and packet id, over the topic. */
#define RTC_MQTT_PUBLISH_OVERHEAD 9u

/* Remaining-length encoding. Writes 1-4 bytes; returns how many (0 if v is
 * over RTC_MQTT_MAX_REMAINING). */
size_t rtc_mqtt_varint(uint8_t out[4], uint32_t v);
/* Decodes a remaining length from p[0..n): bytes used, 0 when more bytes are
 * needed, -1 when malformed (a fifth length byte). */
int rtc_mqtt_varint_decode(const uint8_t *p, size_t n, uint32_t *v);

typedef struct {
	const char *client_id;
	const char *username; /* NULL or "": none */
	const char *password; /* NULL or "": none; needs a username */
	uint16_t keepalive_s;
	const char *will_topic; /* NULL or "": no will */
	const char *will_payload;
	bool will_retain; /* the will is QoS 1 */
} rtc_mqtt_connect_opts;

/* A clean-session CONNECT. Returns its length, or 0 if it does not fit in cap
 * or a string is over 65535 bytes. */
size_t rtc_mqtt_connect(uint8_t *out, size_t cap, const rtc_mqtt_connect_opts *o);

/* A QoS 1 PUBLISH header (fixed header, topic, packet id) for a payload of
 * payload_len bytes, written right-aligned so it ends at hdr + cap: the
 * header starts at the returned offset. Returns cap (and writes nothing)
 * when it does not fit or the packet would exceed RTC_MQTT_MAX_REMAINING. */
size_t rtc_mqtt_publish_header(uint8_t *hdr, size_t cap, const char *topic, size_t topic_len, uint16_t id,
                               bool retain, size_t payload_len);
/* Bytes of the whole PUBLISH packet for this topic and payload length. */
size_t rtc_mqtt_publish_size(size_t topic_len, size_t payload_len);

/* PINGREQ and DISCONNECT: two bytes. */
void rtc_mqtt_pingreq(uint8_t out[2]);
void rtc_mqtt_disconnect(uint8_t out[2]);

/* Packet ids: 1..65535, never 0. */
uint16_t rtc_mqtt_next_id(uint16_t *last);

typedef struct {
	uint8_t type; /* RTC_MQTT_CONNACK, _PUBACK or _PINGRESP */
	bool session_present;
	uint8_t code; /* CONNACK return code */
	uint16_t id;  /* PUBACK packet id */
} rtc_mqtt_pkt;

typedef struct {
	uint8_t buf[5 + 2]; /* fixed header (1 + up to 4 length bytes) + body */
	size_t len;
} rtc_mqtt_rx;

void rtc_mqtt_rx_init(rtc_mqtt_rx *rx);
/* Feeds bytes. Returns 1 when a packet was decoded into out (*used: bytes
 * of p consumed, through that packet's end), 0 when all of p was consumed
 * with no complete packet, -1 on a protocol error (the connection must be
 * closed). Call again with the rest of p after a 1. */
int rtc_mqtt_rx_feed(rtc_mqtt_rx *rx, const uint8_t *p, size_t n, size_t *used, rtc_mqtt_pkt *out);

/* CONNACK return code as text. */
const char *rtc_mqtt_connack_str(uint8_t code);

#endif
