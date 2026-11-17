#!/bin/sh
# Starts rtcollector on a QNX 8.0 target, once the clock is set.
#
#   ./rtcollector-start.sh [config]      (default: rtcollector.yml next to it)
#
# A Raspberry Pi has no battery-backed clock: it boots in 1970, and the
# collector drops every row stamped before its build date (counted as
# records_clock_invalid). The QNX quickstart image sets the clock with
# ntpdate only when the board gets a new DHCP lease, not on every boot. Two
# ntpdate runs at once from 1970 add their steps (the clock lands decades
# ahead), so this waits for that one first and sets the clock only if it is
# still unset and no ntpdate is running.
#
# NTP servers: RTC_NTP_SERVERS (default pool.ntp.org).
DIR=$(cd "$(dirname "$0")" && pwd)
CONF=${1:-$DIR/rtcollector.yml}
SERVERS=${RTC_NTP_SERVERS:-0.pool.ntp.org 1.pool.ntp.org 2.pool.ntp.org}

clock_set() { [ "$(date +%Y)" -ge 2020 ]; }
ntpdate_running() { pidin -F '%N' 2>/dev/null | grep -q '^ntpdate'; }

if ! clock_set; then
	echo "rtcollector-start: the clock is not set; waiting for the network time"
	i=0
	# Up to 30 s for the image's own ntpdate, then ours, retried for 5 min.
	while ! clock_set && [ $i -lt 6 ]; do
		sleep 5
		i=$((i + 1))
	done
	i=0
	while ! clock_set && [ $i -lt 30 ]; do
		if ! ntpdate_running; then
			# shellcheck disable=SC2086
			ntpdate -u -b $SERVERS >/dev/null 2>&1
		fi
		clock_set || sleep 10
		i=$((i + 1))
	done
	if clock_set; then
		echo "rtcollector-start: clock set: $(date -u)"
	else
		echo "rtcollector-start: the clock is still not set; rows are dropped until it is"
	fi
fi

exec "$DIR/rtcollector" --config "$CONF"
