#!/usr/bin/env bash
# On-target checks for the kernel trace input (see qnx-validate.sh for the rig).
set -uo pipefail
cd "$(dirname "$0")/.."
B=build-qnx-x86_64
SER() { python3 -I scripts/qnx-serial.py --timeout 150 -- "$@" | tr -d "\r"; }
fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }
for f in rtcollector rtc_writer rtc_hog; do [ -x "$B/$f" ] || { echo "no $B/$f: run make qnx ARCH=x86_64"; exit 2; }; done

PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
printf 'interval: 2\nflush_interval: 2\ninputs: [trace]\noutputs:\n  - stdout: {}\n' >"$B/trace.yml"
printf 'interval: 2\nflush_interval: 2\ntrace_max_bytes: 64MiB\ninputs: [trace]\noutputs:\n  - stdout: {}\n' >"$B/trace-big.yml"
printf 'interval: 2\nflush_interval: 2\ninputs: [trace, rmgr]\noutputs:\n  - stdout: {}\n' >"$B/trace-rmgr.yml"
printf 'interval: 1\nflush_interval: 1\ntrace_interval: 3\ninputs: [trace]\noutputs:\n  - stdout: {}\n' >"$B/trace-slow.yml"
printf 'interval: 2\nflush_interval: 2\ntrace_max_threads: 16\ninputs: [trace]\noutputs:\n  - stdout: {}\n' >"$B/trace-threads.yml"
printf 'interval: 2\nflush_interval: 2\ntrace_max_bytes: 64KiB\ninputs: [trace]\noutputs:\n  - stdout: {}\n' >"$B/trace-small.yml"
(cd "$B" && exec python3 -m http.server "$PORT" --bind 127.0.0.1) >/tmp/rtc-validate-http.log 2>&1 &
HTTP=$!
trap 'kill $HTTP 2>/dev/null || true' EXIT
for _ in 1 2 3 4 5 6 7 8 9 10; do curl -fsS -o /dev/null "http://127.0.0.1:$PORT/trace.yml" 2>/dev/null && break; sleep 0.3; done
# A copy that looks for a libtracelog that is not there: an image without it.
python3 -I -c 'import sys; d=open(sys.argv[1],"rb").read(); assert d.count(b"libtracelog.so.1\0")==1; open(sys.argv[2],"wb").write(d.replace(b"libtracelog.so.1\0", b"libtracelog.so.9\0"))' "$B/rtcollector" "$B/rtcollector-nolib" || { echo "cannot patch the library name"; exit 2; }
want=$(cksum <"$B/rtcollector" | awk '{print $1}')
got=$(SER "cd /tmp && slay -f -s SIGKILL rtcollector >/dev/null 2>&1; rm -f rtcollector && for f in rtcollector rtcollector-nolib rtc_writer rtc_hog trace.yml trace-small.yml trace-threads.yml trace-slow.yml trace-rmgr.yml trace-big.yml; do curl -fsS -O http://10.0.2.2:$PORT/\$f || exit 1; done; chmod 755 rtcollector rtcollector-nolib rtc_writer rtc_hog; cksum < rtcollector" | awk '{print $1}' | tail -1)
[ "$got" = "$want" ] && ok "deployed (cksum $want)" || { bad "deploy: cksum $got vs $want"; exit 1; }

# Known workload on CPU 1: prio-30 hog 5 ms busy / 5 ms idle, prio-40 thread
# (1 ms busy / 4 ms idle), prio-20 victim. The victim waits out the hog's
# bursts (p99 about 5-6 ms); the prio-40 thread preempts (p99 tiny).
out=$(SER "cd /tmp; ./rtcollector --config trace.yml > /tmp/tr.out 2>&1 & P=\$!; sleep 1; on -C 1 -p 30 ./rtc_hog -b 5 -s 5 -t 6 & on -C 1 -p 40 ./rtc_hog -b 1 -s 4 -t 6 & on -C 1 -p 20 ./rtc_hog -b 1 -s 1 -t 6 & sleep 7; kill \$P; sleep 2; grep '^sched,.*process=rtc_hog' /tmp/tr.out; grep ^cpu_sched /tmp/tr.out | head -1; grep ^irq /tmp/tr.out | head -1; grep ^internal_trace /tmp/tr.out | tail -1; ls /dev/shmem | grep -c rtcollector-trace")
field() { echo "$1" | sed -n "s/.*[ ,]$2=\([0-9-]*\)i.*/\1/p"; }
p40=$(echo "$out" | grep 'priority=40i' | tail -1); p20=$(echo "$out" | grep 'priority=20i' | tail -1)
v99=$(field "$p20" latency_p99_ns); h99=$(field "$p40" latency_p99_ns)
[ -n "$v99" ] && [ "$v99" -ge 4500000 ] && [ "$v99" -le 9000000 ] && ok "prio-20 victim p99 ${v99} ns (the hog's 5 ms burst)" || { bad "victim p99 ${v99:-?}"; echo "$out" | head -8; }
[ -n "$h99" ] && [ "$h99" -le 200000 ] && ok "prio-40 thread p99 ${h99} ns (preempts)" || bad "prio-40 p99 ${h99:-?}"
echo "$out" | grep -q 'process=rtc_hog,thread=hog,' && ok "names: processes and threads started after the first interval are named" || bad "names"
nv=$(echo "$out" | grep -c 'priority=20i'); pe=$(field "$(echo "$out" | grep ^internal_trace)" parse_errors)
[ "$nv" -ge 2 ] && [ "${pe:-1}" = "0" ] && ok "every interval parses (victim rows in $nv intervals, parse_errors 0)" || bad "later intervals: victim rows $nv, parse_errors ${pe:-?}"
echo "$out" | grep -q '^cpu_sched,.*context_switches=[1-9]' && echo "$out" | grep -q '^irq,.*count=[1-9]' && ok "cpu_sched and irq rows" || bad "cpu/irq rows"
mx=$(echo "$out" | grep '^sched' | sed -n 's/.*latency_max_ns=\([0-9]*\)i.*/\1/p' | sort -n | tail -1)
[ "${mx:-0}" -lt 1000000000 ] && ok "no implausible latency (max ${mx} ns)" || bad "implausible latency ${mx}"
[ "$(echo "$out" | tail -1)" = "0" ] && ok "no trace files left in /dev/shmem" || bad "trace files left: $(echo "$out" | tail -1)"

# Another tracer: tracelogger asks for the trace; we yield, it succeeds, we resume.
out=$(SER "cd /tmp; ./rtcollector --config trace.yml > /tmp/ty.out 2>&1 & P=\$!; on -C 1 -p 30 ./rtc_hog -b 5 -s 5 -t 9 & on -C 1 -p 20 ./rtc_hog -b 1 -s 1 -t 9 & sleep 2; tracelogger -s 1 -f /dev/shmem/ext.kev > /dev/null 2>&1; echo tl_rc=\$?; ls -la /dev/shmem/ext.kev | awk '{print \"ext_bytes=\" \$5}'; rm -f /dev/shmem/ext.kev; sleep 4; kill \$P; sleep 1; grep ^internal_trace /tmp/ty.out | tail -1; grep '^sched' /tmp/ty.out | sed -n 's/.*latency_max_ns=\\([0-9]*\\)i.*/\\1/p' | sort -n | tail -1")
echo "$out" | grep -q 'tl_rc=0' && ok "external tracelogger ran while the collector traced (it yielded)" || { bad "external tracelogger"; echo "$out"; }
y=$(field "$(echo "$out" | grep ^internal_trace)" yielded); s=$(field "$(echo "$out" | grep ^internal_trace)" sessions)
[ "${y:-0}" -ge 1 ] && [ "${s:-0}" -ge 2 ] && ok "yielded $y, resumed (sessions $s)" || bad "yield/resume: yielded=${y:-?} sessions=${s:-?}"
mx=$(echo "$out" | tail -1)
[ "${mx:-0}" -lt 500000000 ] && ok "the yield gap is not a latency (max ${mx} ns)" || bad "latency across the yield: $mx"

# Byte cap: 64 KiB per interval under a busy system; the bytes dropped at
# the cap are a gap, not a wait.
out=$(SER "cd /tmp; ./rtcollector --config trace-small.yml > /tmp/ts.out 2>&1 & P=\$!; sleep 1; for i in 1 2 3 4 5 6 7 8; do ./rtc_hog -b 1 -s 1 -t 3 & done; sleep 4; kill \$P; sleep 1; grep ^internal_trace /tmp/ts.out | tail -1; grep '^sched' /tmp/ts.out | sed -n 's/.*latency_max_ns=\\([0-9]*\\)i.*/\\1/p' | sort -n | tail -1")
bd=$(field "$out" bytes_dropped)
[ "${bd:-0}" -gt 0 ] && ok "byte cap: bytes_dropped=$bd" || bad "byte cap: bytes_dropped=${bd:-?}"
mx=$(echo "$out" | tail -1)
[ "${mx:-0}" -lt 500000000 ] && ok "bytes dropped at the cap are not a latency (max ${mx} ns)" || bad "latency across the cap: $mx"

# Thread table bound: 16 threads, more are counted.
out=$(SER "cd /tmp; ./rtcollector --config trace-threads.yml > /tmp/tt.out 2>&1 & sleep 1; for i in 1 2 3 4 5 6 7 8; do ./rtc_hog -b 1 -s 1 -t 3 & done; sleep 4; slay -f rtcollector; sleep 1; grep ^internal_trace /tmp/tt.out | tail -1")
to=$(field "$out" threads_overflow)
[ "${to:-0}" -gt 0 ] && ok "thread table bound: threads_overflow=$to" || bad "threads_overflow=${to:-?}"

# trace_interval longer than interval (non-default): one set of rows per
# trace interval, none repeated in the rounds between, none lost.
out=$(SER "cd /tmp; ./rtcollector --config trace-slow.yml > /tmp/tsl.out 2>&1 & sleep 10; slay -f rtcollector; sleep 1; grep ^cpu_sched,.*cpu=cpu0 /tmp/tsl.out | wc -l; grep ^internal_trace /tmp/tsl.out | tail -1")
n=$(echo "$out" | grep -E '^ *[0-9]+$' | tail -1 | tr -d ' '); sl=$(field "$(echo "$out" | grep ^internal_trace)" snapshots_lost)
[ -n "$n" ] && [ "$n" -ge 2 ] && [ "$n" -le 4 ] && [ "${sl:-1}" = "0" ] && ok "trace_interval 3 s at interval 1 s: $n intervals in 10 s, none lost" || { bad "trace_interval: rows=$n snapshots_lost=${sl:-?}"; echo "$out"; }

# trace and rmgr together: published lines all arrive, and the device's
# handler threads (priority 63) show up in the trace rows.
out=$(SER "cd /tmp; ./rtcollector --config trace-rmgr.yml > /tmp/trm.out 2>&1 & sleep 1; ./rtc_writer -n 200 -m publish -s both; sleep 4; slay -f rtcollector; sleep 1; grep -c '^both,' /tmp/trm.out; grep -c '^sched,.*process=rtcollector,.*priority=63i' /tmp/trm.out")
nb=$(echo "$out" | grep -E '^[0-9]+$' | tail -2 | head -1); nh=$(echo "$out" | grep -E '^[0-9]+$' | tail -1)
[ "$nb" = "200" ] && [ "${nh:-0}" -ge 1 ] && ok "trace + rmgr: 200/200 published lines, device threads in sched rows" || { bad "trace + rmgr: lines=$nb handler rows=$nh"; echo "$out" | tail -3; }

# Unpinned: wakers and wakees on different CPUs, whose kernel buffers reach
# the file in any order. Out-of-order events stay rare and no false latency.
out=$(SER "cd /tmp; ./rtcollector --config trace.yml > /tmp/tu.out 2>&1 & sleep 1; for i in 1 2 3 4 5 6; do on -p 20 ./rtc_hog -b 1 -s 1 -t 6 & done; on -p 30 ./rtc_hog -b 3 -s 2 -t 6 & sleep 7; slay -f rtcollector; sleep 1; grep ^internal_trace /tmp/tu.out | tail -1; grep '^sched' /tmp/tu.out | sed -n 's/.*latency_max_ns=\\([0-9]*\\)i.*/\\1/p' | sort -n | tail -1")
ev=$(field "$(echo "$out" | grep ^internal_trace)" events); oo=$(field "$(echo "$out" | grep ^internal_trace)" out_of_order); mx=$(echo "$out" | tail -1)
[ "${ev:-0}" -gt 0 ] && [ $(( ${oo:-0} * 100 )) -lt "$ev" ] && [ "${mx:-0}" -lt 100000000 ] && ok "unpinned: out_of_order ${oo}/${ev} events, max latency ${mx} ns" || { bad "unpinned: out_of_order=${oo:-?} events=${ev:-?} max=${mx:-?}"; }

# Starvation: four prio-30 hogs busy 2.5 s at a time (50 ms pauses, enough
# for every waiter to run), one per CPU, above the logger (15): the
# kernel drops events; they are counted and no garbage latency comes out.
out=$(SER "cd /tmp; ./rtcollector --config trace.yml > /tmp/tdrop.out 2>&1 & P=\$!; sleep 1; for c in 0 1 2 3; do on -C \$c -p 30 ./rtc_hog -b 2500 -s 50 -t 3 & done; sleep 5; kill \$P; sleep 2; grep ^internal_trace /tmp/tdrop.out | tail -1; grep '^sched' /tmp/tdrop.out | sed -n 's/.*latency_max_ns=\([0-9]*\)i.*/\1/p' | sort -n | tail -1")
kd=$(field "$(echo "$out" | grep ^internal_trace)" kernel_dropped); mx=$(echo "$out" | tail -1)
[ "${kd:-0}" -gt 0 ] && ok "starved logger: kernel drops counted ($kd)" || bad "kernel_dropped=${kd:-?} (expected drops)"
# A real wait here is at most one 2.5 s burst; a stale READY carried across
# a drop shows as more.
[ "${mx:-0}" -lt 2600000000 ] && ok "after drops: no latency longer than a burst (max ${mx} ns)" || bad "garbage latency after drops: $mx"

# Non-root: unavailable, counted, collector runs.
out=$(SER "cd /tmp; on -u rtuser ./rtcollector --config trace.yml > /tmp/tn.out 2>&1 & sleep 3; slay -f rtcollector; sleep 1; grep 'needs root or PROCMGR_AID_TRACE' /tmp/tn.out; grep ^internal_trace /tmp/tn.out | tail -1")
echo "$out" | grep -q 'warn trace: kernel tracing needs root' && echo "$out" | grep -q '^internal_trace.*unavailable=1i' && ok "non-root: unavailable, counted, collector runs" || { bad "non-root"; echo "$out"; }

# Non-root granted the trace ability: tracing works.
out=$(SER "cd /tmp; on -u rtuser -A trace,allow,nonroot ./rtcollector --config trace.yml > /tmp/ta.out 2>&1 & on -p 20 ./rtc_hog -b 1 -s 1 -t 4 & sleep 5; slay -f rtcollector; sleep 1; grep -c '^sched,' /tmp/ta.out; grep ^internal_trace /tmp/ta.out | tail -1")
echo "$out" | grep -q '^internal_trace.*unavailable=0i' && [ "$(echo "$out" | grep -E '^[0-9]+$' | tail -1)" -gt 0 ] && ok "non-root with PROCMGR_AID_TRACE: traces" || { bad "non-root with ability"; echo "$out"; }

# --once gathers one round and exits; it must not start a trace session.
out=$(SER "cd /tmp; ./rtcollector --config trace.yml --once 2>&1 | grep ^internal_trace; echo rc=\$?; ls /dev/shmem | grep -c rtcollector-trace")
echo "$out" | grep -q '^internal_trace.*sessions=0i' && [ "$(echo "$out" | tail -1)" = "0" ] && ok "--once: no trace session, no files" || { bad "--once"; echo "$out"; }

# A second collector cannot get the trace monitor: it says so and counts it.
out=$(SER "cd /tmp; ./rtcollector --config trace.yml > /dev/null 2>&1 & sleep 1; ./rtcollector --config trace.yml > /tmp/t2.out 2>&1 & sleep 3; slay -f rtcollector; sleep 1; grep 'tracing disabled' /tmp/t2.out; grep ^internal_trace /tmp/t2.out | tail -1; ls /dev/shmem | grep -c rtcollector-trace")
echo "$out" | grep -q 'another process monitors' && echo "$out" | grep -q '^internal_trace.*unavailable=1i' && [ "$(echo "$out" | tail -1)" = "0" ] && ok "second collector: tracing disabled, counted, no files left" || { bad "second collector"; echo "$out"; }

# No libtracelog on the image: the collector runs, the input says why.
out=$(SER "cd /tmp; ./rtcollector-nolib --config trace.yml > /tmp/tl.out 2>&1 & sleep 3; slay -f rtcollector-nolib; sleep 1; grep 'cannot load libtracelog' /tmp/tl.out; grep ^internal_trace /tmp/tl.out | tail -1; grep -c ^internal_collector /tmp/tl.out")
echo "$out" | grep -q 'cannot load libtracelog.so.9' && echo "$out" | grep -q '^internal_trace.*unavailable=1i' && [ "$(echo "$out" | tail -1)" -ge 1 ] && ok "no libtracelog: collector runs, trace unavailable" || { bad "no libtracelog"; echo "$out"; }

# kill -9 leaves the trace facility usable.
# Its files are removed by the next collector's start.
out=$(SER "cd /tmp; ./rtcollector --config trace.yml > /dev/null 2>&1 & P=\$!; sleep 2; kill -9 \$P; sleep 1; tracelogger -s 1 -f /dev/shmem/ext.kev > /dev/null 2>&1; echo tl_rc=\$?; rm -f /dev/shmem/ext.kev; echo left=\$(ls /dev/shmem | grep -c rtcollector-trace); ./rtcollector --config trace.yml > /tmp/tk.out 2>&1 & sleep 2; slay -f rtcollector; sleep 1; grep -c 'trace files left by collectors' /tmp/tk.out; ls /dev/shmem | grep -c rtcollector-trace")
echo "$out" | grep -q 'tl_rc=0' && ok "after kill -9: tracelogger works" || { bad "after kill -9"; echo "$out"; }
echo "$out" | grep -qE 'left=[1-9]' && [ "$(echo "$out" | grep -E '^[0-9]+$' | tail -2 | head -1)" = "1" ] && [ "$(echo "$out" | tail -1)" = "0" ] && ok "after kill -9: the next start removes the files left behind" || { bad "stale files after kill -9"; echo "$out" | tail -4; }

# Shutdown is bounded with the parser (priority 8) starved: threads at 9 busy
# without pause on
# every CPU and full 64 MiB files queued, then SIGTERM.
out=$(SER "cd /tmp; ./rtcollector --config trace-big.yml > /tmp/tb.out 2>&1 & P=\$!; for c in 0 1 2 3; do on -C \$c -p 9 ./rtc_hog -b 1000 -s 0 -t 40 & done; sleep 8; t0=\$(date +%s); kill \$P; while kill -0 \$P 2>/dev/null; do sleep 0.2; done; echo took=\$(( \$(date +%s) - t0 )); slay -f rtc_hog; sleep 1; echo stuck=\$(grep -c 'parser did not stop within' /tmp/tb.out); echo files=\$(ls /dev/shmem | grep -c rtcollector-trace); ./rtcollector --config trace.yml > /dev/null 2>&1 & sleep 2; slay -f rtcollector; sleep 1; echo after=\$(ls /dev/shmem | grep -c rtcollector-trace)")
tk=$(echo "$out" | sed -n 's/^took=\([0-9]*\)/\1/p' | tail -1)
sk=$(echo "$out" | sed -n 's/^stuck=\([0-9]*\)/\1/p' | tail -1); fl=$(echo "$out" | sed -n 's/^files=\([0-9]*\)/\1/p' | tail -1); af=$(echo "$out" | sed -n 's/^after=\([0-9]*\)/\1/p' | tail -1)
# The parser is starved mid-parse: stop() gives up on it after 2 s (stuck=1),
# its file stays until the next start removes it.
[ -n "$tk" ] && [ "$tk" -le 10 ] && [ "$sk" = "1" ] && [ "${fl:-9}" -le 1 ] && [ "$af" = "0" ] && ok "SIGTERM with a starved parser: exited in ${tk} s, parser left behind, ${fl} file removed by the next start" || { bad "shutdown: took=${tk:-?} stuck=${sk:-?} files=${fl:-?} after=${af:-?}"; echo "$out" | tail -4; }

left=$(SER "pidin -p rtcollector -f a | grep -c rtcollector" | grep -E '^[0-9]+$' | tail -1)
[ "${left:-0}" -eq 0 ] && ok "no collector left running" || { bad "collectors left: $left"; SER "slay -f -s SIGKILL rtcollector"; }
core=$(SER "ls /data/var/dumper 2>/dev/null | grep -c core" | grep -E '^[0-9]+$' | tail -1)
[ "${core:-0}" -eq 0 ] && ok "no core dumps" || { bad "core dumps: $core"; SER "ls -la /data/var/dumper"; }
exit $fail
