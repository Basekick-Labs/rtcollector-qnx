#include "mqtt.h"

#include <stdint.h>
#include <string.h>

size_t rtc_mqtt_varint(uint8_t out[4], uint32_t v)
{
	if (v > RTC_MQTT_MAX_REMAINING)
		return 0;
	size_t n = 0;
	do {
		uint8_t b = v & 0x7f;
		v >>= 7;
		out[n++] = v ? (uint8_t)(b | 0x80) : b;
	} while (v);
	return n;
}

int rtc_mqtt_varint_decode(const uint8_t *p, size_t n, uint32_t *v)
{
	uint32_t x = 0;
	for (size_t i = 0; i < 4; i++) {
		if (i == n)
			return 0;
		x |= (uint32_t)(p[i] & 0x7f) << (7 * i);
		if (!(p[i] & 0x80)) {
			*v = x;
			return (int)i + 1;
		}
	}
	return -1;
}

typedef struct {
	uint8_t *p;
	size_t cap, len;
	bool failed;
} wbuf;

static void put(wbuf *w, const void *p, size_t n)
{
	if (w->failed || n > w->cap - w->len) {
		w->failed = true;
		return;
	}
	if (w->p) /* NULL: measuring only */
		memcpy(w->p + w->len, p, n);
	w->len += n;
}

static void put_u16(wbuf *w, uint16_t v)
{
	uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
	put(w, b, 2);
}

static void put_str(wbuf *w, const char *s)
{
	size_t n = strlen(s);
	if (n > 65535) {
		w->failed = true;
		return;
	}
	put_u16(w, (uint16_t)n);
	put(w, s, n);
}

static bool set(const char *s)
{
	return s && s[0];
}

static void connect_body(wbuf *w, const rtc_mqtt_connect_opts *o)
{
	bool user = set(o->username), pass = set(o->password), will = set(o->will_topic);
	static const uint8_t proto[] = {0, 4, 'M', 'Q', 'T', 'T', 4};
	put(w, proto, sizeof(proto));
	uint8_t flags = 0x02; /* clean session */
	if (will)
		flags |= 0x04 | (1u << 3) | (o->will_retain ? 0x20 : 0); /* will, QoS 1 */
	if (user)
		flags |= 0x80;
	if (pass)
		flags |= 0x40;
	put(w, &flags, 1);
	put_u16(w, o->keepalive_s);
	put_str(w, o->client_id ? o->client_id : "");
	if (will) {
		put_str(w, o->will_topic);
		put_str(w, o->will_payload ? o->will_payload : "");
	}
	if (user)
		put_str(w, o->username);
	if (pass)
		put_str(w, o->password);
}

size_t rtc_mqtt_connect(uint8_t *out, size_t cap, const rtc_mqtt_connect_opts *o)
{
	if (set(o->password) && !set(o->username))
		return 0; /* 3.1.1 3.1.2.9 */
	wbuf m = {NULL, SIZE_MAX, 0, false};
	connect_body(&m, o);
	uint8_t vb[4];
	size_t vn = m.len <= RTC_MQTT_MAX_REMAINING ? rtc_mqtt_varint(vb, (uint32_t)m.len) : 0;
	if (m.failed || !vn || cap < 1 + vn || m.len > cap - 1 - vn)
		return 0;
	out[0] = RTC_MQTT_CONNECT << 4;
	memcpy(out + 1, vb, vn);
	wbuf w = {out + 1 + vn, m.len, 0, false};
	connect_body(&w, o);
	return 1 + vn + w.len;
}

size_t rtc_mqtt_publish_size(size_t topic_len, size_t payload_len)
{
	size_t rl = 2 + topic_len + 2 + payload_len;
	size_t vn = rl < 128 ? 1 : rl < 16384 ? 2 : rl < 2097152 ? 3 : 4;
	return 1 + vn + rl;
}

size_t rtc_mqtt_publish_header(uint8_t *hdr, size_t cap, const char *topic, size_t topic_len, uint16_t id,
                               bool retain, size_t payload_len)
{
	if (topic_len > 65535 || id == 0)
		return cap;
	size_t rl = 2 + topic_len + 2 + payload_len;
	if (rl > RTC_MQTT_MAX_REMAINING)
		return cap;
	uint8_t vb[4];
	size_t vn = rtc_mqtt_varint(vb, (uint32_t)rl);
	size_t hl = 1 + vn + 2 + topic_len + 2;
	if (hl > cap)
		return cap;
	uint8_t *p = hdr + cap - hl;
	p[0] = (uint8_t)((RTC_MQTT_PUBLISH << 4) | (1u << 1) | (retain ? 1 : 0)); /* QoS 1 */
	memcpy(p + 1, vb, vn);
	p[1 + vn] = (uint8_t)(topic_len >> 8);
	p[2 + vn] = (uint8_t)topic_len;
	memcpy(p + 3 + vn, topic, topic_len);
	p[3 + vn + topic_len] = (uint8_t)(id >> 8);
	p[4 + vn + topic_len] = (uint8_t)id;
	return cap - hl;
}

void rtc_mqtt_pingreq(uint8_t out[2])
{
	out[0] = RTC_MQTT_PINGREQ << 4;
	out[1] = 0;
}

void rtc_mqtt_disconnect(uint8_t out[2])
{
	out[0] = RTC_MQTT_DISCONNECT << 4;
	out[1] = 0;
}

uint16_t rtc_mqtt_next_id(uint16_t *last)
{
	*last = *last == 65535 ? 1 : (uint16_t)(*last + 1);
	return *last;
}

void rtc_mqtt_rx_init(rtc_mqtt_rx *rx)
{
	rx->len = 0;
}

/* The packets we accept, by first byte, and their remaining length. */
static int expected_len(uint8_t b0)
{
	switch (b0) {
	case RTC_MQTT_CONNACK << 4:
	case RTC_MQTT_PUBACK << 4:
		return 2;
	case RTC_MQTT_PINGRESP << 4:
		return 0;
	default:
		return -1; /* another type, or reserved flag bits set */
	}
}

int rtc_mqtt_rx_feed(rtc_mqtt_rx *rx, const uint8_t *p, size_t n, size_t *used, rtc_mqtt_pkt *out)
{
	size_t i = 0;
	while (i < n) {
		if (rx->len == sizeof(rx->buf))
			return -1; /* cannot happen with the lengths below */
		rx->buf[rx->len++] = p[i++];
		int want = expected_len(rx->buf[0]);
		if (want < 0)
			return -1;
		if (rx->len == 1)
			continue;
		uint32_t rl;
		int vn = rtc_mqtt_varint_decode(rx->buf + 1, rx->len - 1, &rl);
		if (vn < 0)
			return -1;
		if (vn == 0)
			continue;
		if (rl != (uint32_t)want)
			return -1;
		if (rx->len < 1 + (size_t)vn + rl)
			continue;
		const uint8_t *b = rx->buf + 1 + vn;
		memset(out, 0, sizeof(*out));
		out->type = rx->buf[0] >> 4;
		if (out->type == RTC_MQTT_CONNACK) {
			if (b[0] & 0xfe)
				return -1; /* reserved acknowledge flags */
			out->session_present = b[0] & 1;
			out->code = b[1];
		} else if (out->type == RTC_MQTT_PUBACK) {
			out->id = (uint16_t)(b[0] << 8 | b[1]);
		}
		rx->len = 0;
		*used = i;
		return 1;
	}
	*used = i;
	return 0;
}

const char *rtc_mqtt_connack_str(uint8_t code)
{
	switch (code) {
	case 0:
		return "accepted";
	case 1:
		return "unacceptable protocol version";
	case 2:
		return "client identifier rejected";
	case 3:
		return "server unavailable";
	case 4:
		return "bad user name or password";
	case 5:
		return "not authorized";
	default:
		return "unknown return code";
	}
}
