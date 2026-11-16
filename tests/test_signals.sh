#!/usr/bin/env bash
# The collector stops on SIGINT and SIGTERM even when it was started with
# them ignored (a shell starts background jobs with SIGINT ignored).
#
#   tests/test_signals.sh build/rtcollector
set -u
BIN=${1:?usage: test_signals.sh path/to/rtcollector}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
printf 'interval: 1\nflush_interval: 1\noutputs:\n  - stdout: {}\n' >"$T/c.yml"

fail=0
# check NAME SIGNAL IGNORE: start the collector (with SIGNAL ignored if
# IGNORE is 1), wait for it to run, send SIGNAL, expect a clean exit in 10 s.
check() {
	local name=$1 sig=$2 ign=$3
	if [ "$ign" = 1 ]; then
		(trap '' "$sig" && exec "$BIN" --config "$T/c.yml") >"$T/out" 2>"$T/err" &
	else
		"$BIN" --config "$T/c.yml" >"$T/out" 2>"$T/err" &
	fi
	local pid=$!
	for _ in $(seq 50); do
		grep -q 'starting' "$T/err" && break
		sleep 0.1
	done
	kill -s "$sig" "$pid"
	local i=0
	while kill -0 "$pid" 2>/dev/null && [ $i -lt 100 ]; do
		sleep 0.1
		i=$((i + 1))
	done
	if kill -0 "$pid" 2>/dev/null; then
		echo "FAIL signals: $name: still running 10 s after SIG$sig"
		kill -KILL "$pid" 2>/dev/null
		wait "$pid" 2>/dev/null
		fail=1
		return
	fi
	wait "$pid"
	local rc=$?
	if [ $rc -ne 0 ] || ! grep -q 'stopping' "$T/err"; then
		echo "FAIL signals: $name: rc=$rc"
		cat "$T/err"
		fail=1
		return
	fi
	echo "ok   signals: $name"
}

check "SIGINT" INT 0
check "SIGTERM" TERM 0
check "SIGINT while ignored at start" INT 1
check "SIGTERM while ignored at start" TERM 1
exit $fail
