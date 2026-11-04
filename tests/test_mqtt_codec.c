/* MQTT 3.1.1 codec: spec vectors and the incremental decoder. */
#include "t.h"

#include "../src/core/mqtt.h"

#include <string.h>

static bool bytes_are(const uint8_t *p, size_t n, const uint8_t *want, size_t wn)
{
	if (n != wn || memcmp(p, want, n)) {
		fprintf(stderr, "     got:");
		for (size_t i = 0; i < n; i++)
			fprintf(stderr, " %02x", p[i]);
		fprintf(stderr, "\n");
		return false;
	}
	return true;
}

static void remaining_length(void)
{
	static const struct {
		uint32_t v;
		uint8_t b[4];
		size_t n;
	} vec[] = {
	    {0, {0x00}, 1},
	    {127, {0x7f}, 1},
	    {128, {0x80, 0x01}, 2},
	    {16383, {0xff, 0x7f}, 2},
	    {16384, {0x80, 0x80, 0x01}, 3},
	    {2097151, {0xff, 0xff, 0x7f}, 3},
	    {2097152, {0x80, 0x80, 0x80, 0x01}, 4},
	    {268435455, {0xff, 0xff, 0xff, 0x7f}, 4},
	};
	for (size_t i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
		uint8_t b[4];
		size_t n = rtc_mqtt_varint(b, vec[i].v);
		CHECK(bytes_are(b, n, vec[i].b, vec[i].n));
		uint32_t v = 0;
		CHECK(rtc_mqtt_varint_decode(b, n, &v) == (int)n && v == vec[i].v);
		CHECK(rtc_mqtt_varint_decode(b, n - 1, &v) == 0); /* needs more */
		/* The size helper agrees with the encoder at each boundary. */
		if (vec[i].v >= 4)
			CHECK_EQ_U(rtc_mqtt_publish_size(0, vec[i].v - 4), 1 + vec[i].n + vec[i].v);
	}
	uint8_t b[4];
	CHECK(rtc_mqtt_varint(b, 268435456u) == 0);
	static const uint8_t five[] = {0xff, 0xff, 0xff, 0xff, 0x7f};
	uint32_t v;
	CHECK(rtc_mqtt_varint_decode(five, sizeof(five), &v) == -1);
}

static void connect_packets(void)
{
	uint8_t out[256];
	rtc_mqtt_connect_opts o = {.client_id = "rtc", .keepalive_s = 60};
	static const uint8_t plain[] = {0x10, 0x0f, 0x00, 0x04, 'M',  'Q', 'T', 'T', 0x04,
	                                0x02, 0x00, 0x3c, 0x00, 0x03, 'r', 't', 'c'};
	size_t n = rtc_mqtt_connect(out, sizeof(out), &o);
	CHECK(bytes_are(out, n, plain, sizeof(plain)));

	o = (rtc_mqtt_connect_opts){.client_id = "c",
	                            .username = "u",
	                            .password = "pw",
	                            .keepalive_s = 0,
	                            .will_topic = "w/t",
	                            .will_payload = "offline",
	                            .will_retain = true};
	static const uint8_t full[] = {0x10, 0x22, 0x00, 0x04, 'M',  'Q',  'T',  'T', 0x04, 0xee, 0x00, 0x00,
	                               0x00, 0x01, 'c',  0x00, 0x03, 'w',  '/',  't', 0x00, 0x07, 'o',  'f',
	                               'f',  'l',  'i',  'n',  'e',  0x00, 0x01, 'u', 0x00, 0x02, 'p',  'w'};
	n = rtc_mqtt_connect(out, sizeof(out), &o);
	CHECK(bytes_are(out, n, full, sizeof(full)));
	o.will_retain = false;
	n = rtc_mqtt_connect(out, sizeof(out), &o);
	CHECK(n == sizeof(full) && out[9] == 0xce);

	o = (rtc_mqtt_connect_opts){.client_id = "c", .password = "pw"};
	CHECK(rtc_mqtt_connect(out, sizeof(out), &o) == 0); /* password needs a username */
	o = (rtc_mqtt_connect_opts){.client_id = "rtc"};
	CHECK(rtc_mqtt_connect(out, sizeof(plain) - 1, &o) == 0); /* does not fit */
	CHECK(rtc_mqtt_connect(out, sizeof(plain), &o) == sizeof(plain));
}

static void publish_header(void)
{
	uint8_t hdr[64];
	size_t at = rtc_mqtt_publish_header(hdr, sizeof(hdr), "a/b", 3, 1, false, 3);
	static const uint8_t want[] = {0x32, 0x0a, 0x00, 0x03, 'a', '/', 'b', 0x00, 0x01};
	CHECK(bytes_are(hdr + at, sizeof(hdr) - at, want, sizeof(want)));
	CHECK_EQ_U(rtc_mqtt_publish_size(3, 3), sizeof(want) + 3);
	at = rtc_mqtt_publish_header(hdr, sizeof(hdr), "a/b", 3, 0x1234, true, 200);
	static const uint8_t ret[] = {0x33, 0xcf, 0x01, 0x00, 0x03, 'a', '/', 'b', 0x12, 0x34};
	CHECK(bytes_are(hdr + at, sizeof(hdr) - at, ret, sizeof(ret)));
	CHECK(rtc_mqtt_publish_header(hdr, 8, "a/b", 3, 1, false, 3) == 8);   /* no room */
	CHECK(rtc_mqtt_publish_header(hdr, 64, "a/b", 3, 0, false, 3) == 64); /* id 0 */
	CHECK(rtc_mqtt_publish_header(hdr, 64, "a/b", 3, 1, false, RTC_MQTT_MAX_REMAINING) == 64);

	uint8_t two[2];
	rtc_mqtt_pingreq(two);
	CHECK(two[0] == 0xc0 && two[1] == 0);
	rtc_mqtt_disconnect(two);
	CHECK(two[0] == 0xe0 && two[1] == 0);
}

static void packet_ids_wrap_and_skip_zero(void)
{
	uint16_t last = 0;
	CHECK(rtc_mqtt_next_id(&last) == 1);
	last = 65534;
	CHECK(rtc_mqtt_next_id(&last) == 65535);
	CHECK(rtc_mqtt_next_id(&last) == 1);
	CHECK(rtc_mqtt_next_id(&last) == 2);
}

static void decoder(void)
{
	rtc_mqtt_rx rx;
	rtc_mqtt_pkt pk;
	size_t used;

	/* Byte by byte. */
	static const uint8_t connack[] = {0x20, 0x02, 0x00, 0x05};
	rtc_mqtt_rx_init(&rx);
	for (size_t i = 0; i < 3; i++)
		CHECK(rtc_mqtt_rx_feed(&rx, connack + i, 1, &used, &pk) == 0 && used == 1);
	CHECK(rtc_mqtt_rx_feed(&rx, connack + 3, 1, &used, &pk) == 1 && used == 1);
	CHECK(pk.type == RTC_MQTT_CONNACK && pk.code == 5 && !pk.session_present);

	static const uint8_t sp[] = {0x20, 0x02, 0x01, 0x00};
	CHECK(rtc_mqtt_rx_feed(&rx, sp, 4, &used, &pk) == 1 && pk.session_present && pk.code == 0);

	/* Several packets in one read. */
	static const uint8_t three[] = {0x40, 0x02, 0x00, 0x07, 0xd0, 0x00, 0x40, 0x02, 0xff, 0xff};
	size_t at = 0;
	CHECK(rtc_mqtt_rx_feed(&rx, three, sizeof(three), &used, &pk) == 1);
	CHECK(pk.type == RTC_MQTT_PUBACK && pk.id == 7 && used == 4);
	at += used;
	CHECK(rtc_mqtt_rx_feed(&rx, three + at, sizeof(three) - at, &used, &pk) == 1);
	CHECK(pk.type == RTC_MQTT_PINGRESP && used == 2);
	at += used;
	CHECK(rtc_mqtt_rx_feed(&rx, three + at, sizeof(three) - at, &used, &pk) == 1);
	CHECK(pk.type == RTC_MQTT_PUBACK && pk.id == 65535 && at + used == sizeof(three));

	/* Garbage: every other type, flags, lengths. */
	static const uint8_t bad[][6] = {
	    {0x30, 0x00},                   /* a PUBLISH: we never subscribe */
	    {0x21, 0x02, 0x00, 0x00},       /* CONNACK with flag bits */
	    {0x20, 0x03, 0x00, 0x00, 0x00}, /* CONNACK too long */
	    {0x20, 0x02, 0x02, 0x00},       /* reserved acknowledge flags */
	    {0xd0, 0x01, 0x00},             /* PINGRESP with a body */
	    {0x20, 0x80, 0x80, 0x80, 0x80}, /* five length bytes */
	    {0x00},
	    {0x90, 0x03}, /* SUBACK */
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		rtc_mqtt_rx_init(&rx);
		CHECK(rtc_mqtt_rx_feed(&rx, bad[i], sizeof(bad[i]), &used, &pk) == -1);
	}
	CHECK(!strcmp(rtc_mqtt_connack_str(4), "bad user name or password"));
}

const t_case mqtt_codec_cases[] = {
    {"mqtt_codec", "remaining_length", remaining_length},
    {"mqtt_codec", "connect_packets", connect_packets},
    {"mqtt_codec", "publish_header", publish_header},
    {"mqtt_codec", "packet_ids_wrap_and_skip_zero", packet_ids_wrap_and_skip_zero},
    {"mqtt_codec", "decoder", decoder},
    {0},
};
