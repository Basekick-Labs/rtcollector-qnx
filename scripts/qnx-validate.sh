#!/usr/bin/env bash
# On-target validation of the QNX inputs. Run on the host that runs the VM
# (scripts/qemu-vm.sh). Serves build-qnx-x86_64/ over HTTP on a free port,
# pulls the binary onto the VM, checks it arrived intact (cksum), and checks
# each input against QNX's own tools. Exit 0 only if every check passed.
set -uo pipefail
cd "$(dirname "$0")/.."
B=build-qnx-x86_64
SER() { python3 -I scripts/qnx-serial.py --timeout 90 -- "$@"; }
fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

[ -x "$B/rtcollector" ] || { echo "no $B/rtcollector: run make qnx ARCH=x86_64"; exit 2; }
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
printf 'interval: 1\nflush_interval: 1\nprocesses_every: 1\noutputs:\n  - stdout: {}\ninputs: [cpu, mem, processes]\n' >"$B/validate.yml"
printf 'interval: 1\nflush_interval: 1\ncollect_priority: 12\nsend_priority: 8\noutputs:\n  - stdout: {}\ninputs: [cpu]\n' >"$B/prio.yml"
(cd "$B" && exec python3 -m http.server "$PORT" --bind 127.0.0.1) >/tmp/rtc-validate-http.log 2>&1 &
HTTP=$!
trap 'kill $HTTP 2>/dev/null || true' EXIT
for _ in 1 2 3 4 5 6 7 8 9 10; do
	curl -fsS -o /dev/null "http://127.0.0.1:$PORT/validate.yml" 2>/dev/null && break
	sleep 0.3
done
kill -0 $HTTP 2>/dev/null || { echo "HTTP server did not start on port $PORT"; cat /tmp/rtc-validate-http.log; exit 2; }

want=$(cksum <"$B/rtcollector" | awk '{print $1}')
got=$(SER "cd /tmp && rm -f rtcollector validate.yml prio.yml && curl -fsS -O http://10.0.2.2:$PORT/rtcollector && curl -fsS -O http://10.0.2.2:$PORT/validate.yml && curl -fsS -O http://10.0.2.2:$PORT/prio.yml && chmod 755 rtcollector && cksum < rtcollector" | awk '{print $1}' | tail -1)
[ "$got" = "$want" ] && ok "deployed (cksum $want)" || { bad "deployed binary cksum $got, host $want"; exit 1; }

ncpu=$(SER "pidin info | grep -c '^Processor'" | tr -dc '0-9')
[ "${ncpu:-0}" -ge 1 ] || { bad "cannot read the VM's CPU count"; exit 1; }
echo "VM has $ncpu CPUs"

# cpu: a hog pinned to core k; exactly ncpu+1 rows; k busy, others idle, total = mean.
for k in $(seq 0 $((ncpu - 1))); do
	out=$(SER "on -C $k sh -c 'while :; do :; done' & H=\$!; sleep 1; ./rtcollector --config validate.yml --once | grep '^cpu,'; kill \$H")
	verdict=$(echo "$out" | python3 -c '
import re, sys
k, n = int(sys.argv[1]), int(sys.argv[2])
rows = {}
for line in sys.stdin:
    m = re.match(r"cpu,.*cpu=(cpu[\w-]+) .*usage_percent=([0-9.eE+-]+)", line)
    if m: rows[m.group(1)] = float(m.group(2))
cores = [rows.get(f"cpu{i}") for i in range(n)]
if len(rows) != n + 1 or None in cores or "cpu-total" not in rows:
    print(f"FAIL rows {sorted(rows)} (want cpu0..cpu{n-1} + cpu-total)"); sys.exit()
others = [u for i, u in enumerate(cores) if i != k]
mean = sum(cores) / n
total = rows["cpu-total"]
if cores[k] > 90 and all(u < 10 for u in others) and abs(total - mean) < 0.5:
    print(f"ok cpu{k} {cores[k]:.0f}% others max {max(others, default=0):.1f}% total {total:.1f}%")
else:
    print(f"FAIL cpu{k} {cores[k]:.0f}% others {others} total {total:.1f} mean {mean:.1f}")
' "$k" "$ncpu")
	case "$verdict" in ok*) ok "${verdict#ok }" ;; *) bad "${verdict#FAIL }"; echo "$out" ;; esac
done

# mem and processes against pidin (root).
out=$(SER "./rtcollector --config validate.yml --once | grep -E '^(mem|processes)'; pidin info | head -1; pidin info | grep Processes")
echo "$out" | grep -E '^(mem|processes)'
num() { echo "$out" | sed -n "s/.*$1=\([0-9]*\)i.*/\1/p" | head -1; }
close() { [ -n "$1" ] && [ -n "$2" ] && [ $(($1 > $2 ? $1 - $2 : $2 - $1)) -le "$3" ]; }
free=$(( $(num free_bytes) / 1048576 )); total=$(( $(num total_bytes) / 1048576 ))
pfree=$(echo "$out" | sed -n 's/.*FreeMem:\([0-9]*\)MB\/\([0-9]*\)MB.*/\1/p'); ptotal=$(echo "$out" | sed -n 's/.*FreeMem:[0-9]*MB\/\([0-9]*\)MB.*/\1/p')
close "$free" "$pfree" $((pfree / 50 + 1)) && ok "mem free ${free}MB vs pidin ${pfree}MB" || bad "mem free ${free}MB vs pidin ${pfree}MB"
close "$total" "$ptotal" 1 && ok "mem total ${total}MB vs pidin ${ptotal}MB" || bad "mem total ${total}MB vs pidin ${ptotal}MB"
procs=$(num processes); threads=$(num threads)
pprocs=$(echo "$out" | sed -n 's/Processes: \([0-9]*\),.*/\1/p'); pthreads=$(echo "$out" | sed -n 's/.*Threads: \([0-9]*\).*/\1/p')
close "$procs" "$pprocs" 3 && ok "processes $procs vs pidin $pprocs" || bad "processes $procs vs pidin $pprocs"
close "$threads" "$pthreads" 12 && ok "threads $threads vs pidin $pthreads" || bad "threads $threads vs pidin $pthreads"

# Non-root: cpu works, processes without threads, procfs_denied=1, startup succeeds.
out=$(SER "on -u rtuser ./rtcollector --config validate.yml --once; echo rc=\$?")
echo "$out" | grep -q '^cpu,.*cpu=cpu-total' && echo "$out" | grep -q '^processes,' && ! echo "$out" | grep -q 'threads=' &&
	echo "$out" | grep -q 'procfs_denied=1i' && echo "$out" | grep -q '^rc=0' &&
	ok "non-root: cpu ok, processes without threads, procfs_denied=1" || { bad "non-root run"; echo "$out"; }

# Priorities as configured (root), seen from outside with pidin.
out=$(SER "./rtcollector --config prio.yml >/dev/null 2>&1 & P=\$!; sleep 2; pidin -p \$P -F '%b %p %J' ; kill \$P")
echo "$out" | grep -q ' 12r ' && echo "$out" | grep -q ' 8r ' && ok "thread priorities 12r/8r" || { bad "thread priorities"; echo "$out"; }

exit $fail
