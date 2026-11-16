#!/usr/bin/env bash
# The collector stops on SIGINT and SIGTERM even when it was started with
# them ignored (a non-interactive shell starts background jobs with SIGINT
# ignored).
#
# The ignored cases pin the fix on macOS (and QNX), where an ignored signal
# is discarded even while blocked. Linux keeps a blocked signal pending
# whether ignored or not, so there they pass with or without the fix.
#
#   tests/test_signals.sh build/rtcollector
set -u
BIN=${1:?usage: test_signals.sh path/to/rtcollector}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
printf 'interval: 1\nflush_interval: 1\noutputs:\n  - stdout: {}\n' >"$T/c.yml"

fail=0
# check NAME SIGNAL MODE: start the collector (MODE ignore: with SIGNAL
# ignored; jobctl: with job control on, so SIGINT is not ignored; plain: a
# background job of this script), wait for it to run, send SIGNAL, expect a
# clean exit in 10 s.
check() {
	local name=$1 sig=$2 mode=$3
	case $mode in
	ignore) (trap '' "$sig" && exec "$BIN" --config "$T/c.yml") >"$T/out" 2>"$T/err" & ;;
	jobctl)
		set -m
		"$BIN" --config "$T/c.yml" >"$T/out" 2>"$T/err" &
		set +m
		;;
	*) "$BIN" --config "$T/c.yml" >"$T/out" 2>"$T/err" & ;;
	esac
	local pid=$!
	local up=0
	for _ in $(seq 50); do
		grep -q 'starting' "$T/err" && { up=1; break; }
		sleep 0.1
	done
	if [ $up = 0 ]; then
		echo "FAIL signals: $name: not started in 5 s"
		cat "$T/err"
		kill -KILL "$pid" 2>/dev/null
		wait "$pid" 2>/dev/null
		fail=1
		return
	fi
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

check "SIGINT, not ignored (job control)" INT jobctl
check "SIGTERM" TERM plain
# A background job of this non-interactive script starts with SIGINT
# ignored: the case seen on the target.
check "SIGINT to a background job" INT plain
check "SIGINT while ignored at start" INT ignore
check "SIGTERM while ignored at start" TERM ignore
exit $fail
