#!/usr/bin/env bash
# On-target checks for /dev/rtcollector (see qnx-validate.sh for the rig).
set -uo pipefail
cd "$(dirname "$0")/.."
B=build-qnx-x86_64
SER() { python3 -I scripts/qnx-serial.py --timeout 120 -- "$@" | tr -d "\r"; }
fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }
for f in rtcollector rtc_writer rtc_hog; do [ -x "$B/$f" ] || { echo "no $B/$f: run make qnx ARCH=x86_64"; exit 2; }; done

PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
printf 'interval: 1\nflush_interval: 1\nrmgr_mode: 0666\ninputs: [rmgr]\noutputs:\n  - stdout: {}\n' >"$B/rmgr.yml"
printf 'interval: 1\nflush_interval: 1\nshutdown_timeout: 5\nrmgr_mode: 0666\nrmgr_buffer_bytes: 64KiB\ninputs: [rmgr]\noutputs:\n  - arc: {url: "http://10.0.2.2:9", timeout: 2}\n' >"$B/rmgr-down.yml"
printf 'interval: 1\nflush_interval: 1\nrmgr_path: /dev/rtc-nonroot\ninputs: [rmgr]\noutputs:\n  - stdout: {}\n' >"$B/rmgr-nr.yml"
(cd "$B" && exec python3 -m http.server "$PORT" --bind 127.0.0.1) >/tmp/rtc-validate-http.log 2>&1 &
HTTP=$!
trap 'kill $HTTP 2>/dev/null || true' EXIT
for _ in 1 2 3 4 5 6 7 8 9 10; do curl -fsS -o /dev/null "http://127.0.0.1:$PORT/rmgr.yml" 2>/dev/null && break; sleep 0.3; done
want=$(cksum <"$B/rtcollector" | awk '{print $1}')
got=$(SER "cd /tmp && rm -f rtcollector rtc_writer rtc_hog && for f in rtcollector rtc_writer rtc_hog rmgr.yml rmgr-nr.yml rmgr-down.yml; do curl -fsS -O http://10.0.2.2:$PORT/\$f || exit 1; done; chmod 755 rtcollector rtc_writer rtc_hog; cksum < rtcollector" | awk '{print $1}' | tail -1)
[ "$got" = "$want" ] && ok "deployed (cksum $want)" || { bad "deploy: cksum $got vs $want"; exit 1; }

# The collector, in the background, for the whole run.
SER "slay -s SIGKILL rtcollector >/dev/null 2>&1; sleep 0.5; rm -f /tmp/rm.out; ./rtcollector --config rmgr.yml > /tmp/rm.out 2>&1 & echo \$! > /tmp/rm.pid; sleep 1; ls -l /dev/rtcollector" | grep -q '^crw-rw-rw-' && ok "device registered, mode 0666" || bad "device not registered"

now=$(date +%s)000000000
past=$((now - 3600 * 1000000000))
out=$(SER "./rtc_writer -n 100 -m publish -s idiom; ./rtc_writer -n 100 -m snprintf -s idiom; ./rtc_writer -n 10 -m snprintf -s mixed -T $past; ./rtc_writer -n 10 -m publish -s mixed; ./rtc_writer -n 3 -m pwrite -s pw; ./rtc_writer -n 3 -m zero; ./rtc_writer -n 2 -m dprintf -s dp; ./rtc_writer -n 1 -m big; sleep 2; grep -c '^idiom,' /tmp/rm.out; grep -c '^pw,' /tmp/rm.out; grep -c '^dp,' /tmp/rm.out; grep '^mixed,' /tmp/rm.out | awk '{print \$NF}'")
echo "$out" | grep -q 'errno=240' && ok "oversize write refused with EMSGSIZE" || { bad "oversize write"; echo "$out"; }
counts=$(echo "$out" | grep -E '^[0-9]+$' | head -3 | tr '\n' ' ')
[ "$counts" = "200 3 0 " ] && ok "rtc_publish + snprintf: 200/200 rows; pwrite 3/3; dprintf 0 rows (documented)" || bad "rows idiom/pwrite/dprintf: $counts"
mixed=$(echo "$out" | grep -E '^[0-9]{19}$')
old=$(echo "$mixed" | awk -v p="$past" '$1 >= p && $1 < p + 1000000000' | wc -l)
fresh=$(echo "$mixed" | awk -v n="$now" '$1 >= n - 60000000000' | wc -l)
[ "$old" -eq 10 ] && [ "$fresh" -eq 10 ] && ok "explicit and receive timestamps kept apart (10 + 10)" || bad "mixed timestamps: $old explicit, $fresh receive"

st=$(SER "cat /dev/rtcollector; echo cat_rc=\$?")
echo "$st" | grep -q '^rtcollector writes=' && echo "$st" | grep -q '^cat_rc=0' && [ "$(echo "$st" | grep -c '^rtcollector')" -eq 1 ] &&
	ok "cat: one status line, EOF" || { bad "cat status"; echo "$st"; }
echo "$st" | grep -q 'oversize=1 ' && ok "status: oversize counted" || bad "status oversize: $st"
mal=$(echo "$st" | sed -n 's/.*malformed=\([0-9]*\).*/\1/p')
[ "${mal:-0}" -ge 2 ] && ok "dprintf fragments counted as malformed ($mal)" || bad "malformed count $mal"

# A second collector on the same path refuses it; the first keeps working.
out=$(SER "./rtcollector --config rmgr.yml > /tmp/rm2.out 2>&1 & P=\$!; sleep 1.5; kill \$P; sleep 0.5; grep -c 'another rtcollector already serves this path' /tmp/rm2.out; ./rtc_writer -n 5 -s after2; sleep 1.5; grep -c '^after2,' /tmp/rm.out")
[ "$(echo "$out" | grep -E '^[0-9]+$' | tr '\n' ' ')" = "1 5 " ] && ok "second instance refused; first unaffected" || { bad "second instance"; echo "$out"; }

# Two collectors started at the same moment: exactly one gets the device.
out=$(SER "slay -s SIGKILL rtcollector >/dev/null 2>&1; sleep 0.5; ./rtcollector --config rmgr.yml > /tmp/ra.out 2>&1 & A=\$!; ./rtcollector --config rmgr.yml > /tmp/rb.out 2>&1 & B=\$!; sleep 1.5; cat /tmp/ra.out /tmp/rb.out | grep -c ' ready ('; cat /tmp/ra.out /tmp/rb.out | grep -c 'another rtcollector already serves this path'; kill \$A \$B; sleep 1")
[ "$(echo "$out" | grep -E '^[0-9]+$' | tr '\n' ' ')" = "1 1 " ] && ok "simultaneous start: one owner, one refused" || { bad "simultaneous start"; echo "$out"; }

# Non-root: registering a path needs PROCMGR_AID_PATHSPACE; the collector runs on.
out=$(SER "on -u rtuser ./rtcollector --config rmgr-nr.yml > /tmp/nr.out 2>&1 & P=\$!; sleep 1.5; kill \$P; sleep 0.5; grep -c 'needs root or PROCMGR_AID_PATHSPACE' /tmp/nr.out; grep -c 'rmgr_unavailable=1i' /tmp/nr.out; ls /dev/rtc-nonroot 2>&1 | grep -c 'No such'")
[ "$(echo "$out" | grep -E '^[0-9]+$' | tr '\n' ' ')" = "1 1 1 " ] && ok "non-root: unavailable, counted, collector runs" || { bad "non-root"; echo "$out"; }

# Priority inversion on one CPU: a prio-50 writer while a prio-5 writer floods
# and a prio-30 hog runs 20 ms busy / 5 ms idle (so the low writer does write,
# and its handler would be preempted under inheritance), all pinned with the
# collector to CPU 0. Five trials; every max must stay under 1 ms.
worst=0
results=0
for t in 1 2 3 4 5; do
	out=$(SER "slay -s SIGKILL rtcollector >/dev/null 2>&1; sleep 0.5; on -C 0 ./rtcollector --config rmgr.yml > /tmp/rm.out 2>&1 & sleep 1; on -C 0 -p 5 ./rtc_writer -n 400000 -s low > /tmp/low.out & L=\$!; on -C 0 -p 30 ./rtc_hog -b 20 -s 5 -t 6 & H=\$!; sleep 0.3; on -C 0 -p 50 ./rtc_writer -n 4000 -u 500 -t -s high; kill \$H \$L 2>/dev/null; sleep 0.3; cat /dev/rtcollector; pidin -p rtcollector -F '%b %p %J' | grep RECEIVE")
	max=$(echo "$out" | sed -n 's/.*writes=4000 failed=0 .*max_us=\([0-9.]*\).*/\1/p')
	low=$(echo "$out" | sed -n 's/^rtcollector writes=\([0-9]*\).*/\1/p')
	echo "     trial $t: high max ${max:-?} us; device writes ${low:-?} (high 4000 + low)"
	[ -n "$max" ] || { bad "inversion trial $t: no result"; echo "$out"; continue; }
	results=$((results + 1))
	awk -v m="$max" -v w="$worst" 'BEGIN{exit !(m > w)}' && worst=$max
	[ "${low:-0}" -gt 4100 ] || bad "inversion trial $t: the low writer did not write (${low:-0})"
done
[ "$results" -eq 5 ] && awk -v m="$worst" 'BEGIN{exit !(m < 1000)}' && ok "prio-50 writer, low writer flooding + duty-cycle hog, 5 trials: worst max ${worst} us (< 1 ms)" || bad "inversion: worst max ${worst} us"
echo "$out" | grep -q ' 63r RECEIVE' && ok "handler thread waits at its fixed priority 63" || { bad "handler priority"; echo "$out" | tail -3; }
echo "$out" | grep -q 'max_writer_prio=50 ' && ok "status: highest writer priority 50" || bad "max_writer_prio"

# Arc unreachable, small device buffer: the writer never errors, drops are
# counted; shutdown does not hold writers for shutdown_timeout.
out=$(SER "slay -s SIGKILL rtcollector >/dev/null 2>&1; sleep 0.5; ./rtcollector --config rmgr-down.yml > /tmp/down.out 2>&1 & P=\$!; sleep 1; ./rtc_writer -n 50000 -s flood; cat /dev/rtcollector; ./rtc_writer -n 40 -u 50000 -t -s late & W=\$!; sleep 0.5; kill \$P; wait \$W; echo w_rc=\$?; sleep 6; grep -c totals /tmp/down.out")
echo "$out" | grep -q '^writes=50000 failed=0' && ok "flood with Arc down: 50000 writes, none failed" || { bad "flood writes"; echo "$out"; }
drop=$(echo "$out" | sed -n 's/^rtcollector .*dropped=\([0-9]*\).*/\1/p')
[ "${drop:-0}" -gt 0 ] && ok "flood with Arc down: drops counted ($drop)" || bad "flood drops: ${drop:-?}"
lmax=$(echo "$out" | sed -n 's/.*writes=40 failed=[0-9]* .*max_us=\([0-9.]*\).*/\1/p')
awk -v m="${lmax:-99999999}" 'BEGIN{exit !(m < 1000000)}' && ok "shutdown with Arc down: writers fail at once (max write ${lmax} us), not after shutdown_timeout" || { bad "shutdown blocked writers: max ${lmax:-?} us"; echo "$out"; }
echo "$out" | grep -qE '^1$' && ok "shutdown logs the device totals" || bad "no totals line at shutdown"

# After the collector exits, writers get EBADF (they must reopen); the path is gone.
out=$(SER "slay -s SIGKILL rtcollector >/dev/null 2>&1; sleep 0.5; ./rtcollector --config rmgr.yml > /tmp/rm.out 2>&1 & sleep 1; ./rtc_writer -n 30 -u 100000 -s late & W=\$!; sleep 1; slay -f rtcollector; wait \$W; echo w_rc=\$?; ls /dev/rtcollector 2>&1 | grep -c 'No such'")
echo "$out" | grep -q 'w_rc=1' && echo "$out" | grep -E '^1$' >/dev/null && ok "after exit: writes fail (EBADF), path removed" || { bad "after exit"; echo "$out"; }
core=$(SER "ls /data/var/dumper 2>/dev/null | grep -c core" | grep -E '^[0-9]+$' | tail -1)
[ "${core:-0}" -eq 0 ] && ok "no core dumps" || { bad "core dumps on the VM: $core"; SER "ls -la /data/var/dumper"; }
exit $fail
