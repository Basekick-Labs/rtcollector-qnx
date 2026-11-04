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
printf 'interval: 2\nflush_interval: 2\ntrace_max_bytes: 64KiB\ntrace_max_threads: 16\ninputs: [trace]\noutputs:\n  - stdout: {}\n' >"$B/trace-small.yml"
(cd "$B" && exec python3 -m http.server "$PORT" --bind 127.0.0.1) >/tmp/rtc-validate-http.log 2>&1 &
HTTP=$!
trap 'kill $HTTP 2>/dev/null || true' EXIT
for _ in 1 2 3 4 5 6 7 8 9 10; do curl -fsS -o /dev/null "http://127.0.0.1:$PORT/trace.yml" 2>/dev/null && break; sleep 0.3; done
want=$(cksum <"$B/rtcollector" | awk '{print $1}')
got=$(SER "cd /tmp && slay -f -s SIGKILL rtcollector >/dev/null 2>&1; rm -f rtcollector && for f in rtcollector rtc_writer rtc_hog trace.yml trace-small.yml; do curl -fsS -O http://10.0.2.2:$PORT/\$f || exit 1; done; chmod 755 rtcollector rtc_writer rtc_hog; cksum < rtcollector" | awk '{print $1}' | tail -1)
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
echo "$out" | grep -q 'process=rtc_hog,thread=' && ok "names: processes started after the first interval are named" || bad "names"
echo "$out" | grep -q '^cpu_sched,.*context_switches=[1-9]' && echo "$out" | grep -q '^irq,.*count=[1-9]' && ok "cpu_sched and irq rows" || bad "cpu/irq rows"
mx=$(echo "$out" | grep '^sched' | sed -n 's/.*latency_max_ns=\([0-9]*\)i.*/\1/p' | sort -n | tail -1)
[ "${mx:-0}" -lt 1000000000 ] && ok "no implausible latency (max ${mx} ns)" || bad "implausible latency ${mx}"
[ "$(echo "$out" | tail -1)" = "0" ] && ok "no trace files left in /dev/shmem" || bad "trace files left: $(echo "$out" | tail -1)"

# Another tracer: tracelogger asks for the trace; we yield, it succeeds, we resume.
out=$(SER "cd /tmp; ./rtcollector --config trace.yml > /tmp/ty.out 2>&1 & P=\$!; sleep 2; tracelogger -s 1 -f /dev/shmem/ext.kev > /dev/null 2>&1; echo tl_rc=\$?; ls -la /dev/shmem/ext.kev | awk '{print \"ext_bytes=\" \$5}'; rm -f /dev/shmem/ext.kev; sleep 4; kill \$P; sleep 1; grep ^internal_trace /tmp/ty.out | tail -1")
echo "$out" | grep -q 'tl_rc=0' && ok "external tracelogger ran while the collector traced (it yielded)" || { bad "external tracelogger"; echo "$out"; }
y=$(field "$(echo "$out" | grep ^internal_trace)" yielded); s=$(field "$(echo "$out" | grep ^internal_trace)" sessions)
[ "${y:-0}" -ge 1 ] && [ "${s:-0}" -ge 2 ] && ok "yielded $y, resumed (sessions $s)" || bad "yield/resume: yielded=${y:-?} sessions=${s:-?}"

# Bounds: 64 KiB per interval and a 16-thread table, under a busy system.
out=$(SER "cd /tmp; ./rtcollector --config trace-small.yml > /tmp/ts.out 2>&1 & P=\$!; sleep 1; for i in 1 2 3 4 5 6 7 8; do ./rtc_hog -b 1 -s 1 -t 3 & done; sleep 4; kill \$P; sleep 1; grep ^internal_trace /tmp/ts.out | tail -1")
bd=$(field "$out" bytes_dropped); to=$(field "$out" threads_overflow)
[ "${bd:-0}" -gt 0 ] && ok "byte cap: bytes_dropped=$bd" || bad "byte cap: bytes_dropped=${bd:-?}"
[ "${to:-0}" -gt 0 ] && ok "thread table bound: threads_overflow=$to" || bad "threads_overflow=${to:-?}"

# Starvation: four prio-30 hogs, one per CPU, above the logger (15): the
# kernel drops events; they are counted and no garbage latency comes out.
out=$(SER "cd /tmp; ./rtcollector --config trace.yml > /tmp/tdrop.out 2>&1 & P=\$!; sleep 1; for c in 0 1 2 3; do on -C \$c -p 30 ./rtc_hog -b 2500 -s 1 -t 3 & done; sleep 5; kill \$P; sleep 2; grep ^internal_trace /tmp/tdrop.out | tail -1; grep '^sched' /tmp/tdrop.out | sed -n 's/.*latency_max_ns=\([0-9]*\)i.*/\1/p' | sort -n | tail -1")
kd=$(field "$(echo "$out" | grep ^internal_trace)" kernel_dropped); mx=$(echo "$out" | tail -1)
[ "${kd:-0}" -gt 0 ] && ok "starved logger: kernel drops counted ($kd)" || bad "kernel_dropped=${kd:-?} (expected drops)"
[ "${mx:-0}" -lt 3000000000 ] && ok "after drops: no garbage latency (max ${mx} ns)" || bad "garbage latency after drops: $mx"

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

# kill -9 leaves the trace facility usable.
out=$(SER "cd /tmp; ./rtcollector --config trace.yml > /dev/null 2>&1 & P=\$!; sleep 2; kill -9 \$P; sleep 1; tracelogger -s 1 -f /dev/shmem/ext.kev > /dev/null 2>&1; echo tl_rc=\$?; rm -f /dev/shmem/ext.kev /dev/shmem/rtcollector-trace-*")
echo "$out" | grep -q 'tl_rc=0' && ok "after kill -9: tracelogger works" || { bad "after kill -9"; echo "$out"; }

left=$(SER "pidin -p rtcollector -f a | grep -c rtcollector" | grep -E '^[0-9]+$' | tail -1)
[ "${left:-0}" -eq 0 ] && ok "no collector left running" || { bad "collectors left: $left"; SER "slay -f -s SIGKILL rtcollector"; }
core=$(SER "ls /data/var/dumper 2>/dev/null | grep -c core" | grep -E '^[0-9]+$' | tail -1)
[ "${core:-0}" -eq 0 ] && ok "no core dumps" || { bad "core dumps: $core"; SER "ls -la /data/var/dumper"; }
exit $fail
