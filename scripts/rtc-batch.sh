#!/bin/sh
# On-target half of the overhead measurement (see rtc-bench.sh, which copies
# this to the target with the binaries and starts it detached).
#
#   sh rtc-batch.sh DIR SECONDS ARC_URL MQTT_URL LOOP_CPU MECH SCENARIO...
#   sh rtc-batch.sh @ARGS_FILE     the same arguments, one line (a serial
#                                  console drops lines past ~255 bytes)
#
# Runs each scenario in the order given (the host randomises it), one
# rtc_loop run each, and writes DIR/NN-SCENARIO/{summary,hist.csv,env,
# collector.log}. DIR/done is written last. No output is needed while it
# runs: nothing has to stay connected to the target.
set -u
case ${1:-} in @*) set -- $(cat "${1#@}") ;; esac
DIR=$1 SECS=$2 ARC=$3 MQTT=$4 CPU=$5 MECH=$6
shift 6
NCPU=$(pidin info | grep -c '^Processor')
BIN=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$DIR"
rm -f "$DIR/done"

# Collector configurations: the defaults but interval 1 (busier than the
# default 10 s), trace_interval 1 (one sched row per second) unless EXTRA
# says otherwise.
conf() { # conf NAME INPUTS OUTPUT [extra yaml]
	{
		printf 'interval: 1\nflush_interval: 1\ninputs: [%s]\noutputs:\n' "$2"
		printf '  - %s\n' "$3"
		printf '%s\n' "${4:-trace_interval: 1}"
	} >"$DIR/$1.yml"
}
A="arc: {url: \"$ARC\", database: rtc_bench}"
conf c1 "cpu, mem, processes" "$A"
conf c2 "cpu, mem, processes, trace" "$A"
conf c2d "cpu, mem, processes, trace" "$A" "trace_interval: 10"
conf c3 "cpu, mem, processes, trace, rmgr" "$A"
conf c3q "cpu, mem, processes, trace, rmgr" "mqtt: {url: \"$MQTT\", username: rtc, password: rtcpass}"
conf c6r "cpu, mem, processes, trace, rmgr" "arc: {url: \"http://127.0.0.1:1\", database: rtc_bench}"
conf c6b "cpu, mem, processes, trace, rmgr" "arc: {url: \"http://10.255.255.1:8100\", database: rtc_bench, timeout: 5}"
conf cneg "cpu, mem, processes, trace, rmgr" "$A" "trace_interval: 1
collect_priority: 60"

others() { # every CPU but the loop's
	i=0
	while [ "$i" -lt "$NCPU" ]; do
		[ "$i" != "$CPU" ] && echo "$i"
		i=$((i + 1))
	done
}

n=0
for s in "$@"; do
	n=$((n + 1))
	R=$DIR/$(printf '%02d' "$n")-$s
	mkdir -p "$R"
	cfg="" rate=0 pids=""
	case $s in
	S0a | S0b) ;;
	Cplus) # positive control: 20 us at prio 60 on the loop's CPU every 100 ms
		"$BIN/rtc_hog" -p 60 -c "$CPU" -u 20 -U 100000 -t $((SECS + 5)) &
		pids="$pids $!" ;;
	S1) cfg=c1 ;;
	S2) cfg=c2 ;;
	S2d) cfg=c2d ;;
	S3) cfg=c3 rate=10 ;;
	S3q) cfg=c3q rate=10 ;;
	S3m) cfg=c3 rate=10 ;;
	S3h)
		cfg=c3 rate=10
		for c in $(others); do
			"$BIN/rtc_hog" -p 10 -c "$c" -b 1000 -s 0 -t $((SECS + 5)) &
			pids="$pids $!"
		done ;;
	S4 | S4o) # flood of 4 KiB writes at prio 10, on the loop's CPU or another
		cfg=c3 rate=10 fc=$CPU
		[ "$s" = S4o ] && fc=$(others | head -1) ;;
	S6r) cfg=c6r rate=10 ;;
	S6b) cfg=c6b rate=10 ;;
	Sneg) cfg=cneg rate=10 ;;
	*) echo "unknown scenario $s" >"$R/summary"; continue ;;
	esac
	{
		echo "scenario=$s order=$n secs=$SECS cpu=$CPU mech=$MECH"
		date -u +%Y-%m-%dT%H:%M:%SZ
		uname -a
		pidin info 2>/dev/null | head -4
		pidin -F '%a %b %N %p %J' 2>/dev/null | grep -iE 'io-sock|devnp|devc-|wpa' | head -20
		pidin irqs 2>/dev/null | head -40
	} >"$R/env" 2>&1
	cp=""
	if [ -n "$cfg" ]; then
		if [ "$s" = S3m ]; then
			on -C 0 "$BIN/rtcollector" --config "$DIR/$cfg.yml" >"$R/collector.log" 2>&1 &
		else
			"$BIN/rtcollector" --config "$DIR/$cfg.yml" >"$R/collector.log" 2>&1 &
		fi
		cp=$!
		sleep 3 # past startup: measured on its own, not inside a run
		case $s in S4 | S4o) # needs the device, so after the collector
			"$BIN/rtc_writer" -m max -n 100000000 -P 10 -c "$fc" -s bench_flood >/dev/null 2>&1 &
			pids="$pids $!" ;;
		esac
	fi
	"$BIN/rtc_loop" -m "$MECH" -c "$CPU" -t "$SECS" -r "$rate" -H "$R/hist.csv" >"$R/summary" 2>"$R/loop.err"
	echo "rc=$?" >>"$R/summary"
	for p in $pids; do kill "$p" 2>/dev/null; done
	if [ -n "$cp" ]; then
		kill "$cp" 2>/dev/null
		wait "$cp" 2>/dev/null
	fi
	wait 2>/dev/null
	sleep 2
done
date -u +%Y-%m-%dT%H:%M:%SZ >"$DIR/done"
