#!/usr/bin/env bash
# On-target checks for /dev/rtcollector (see qnx-validate.sh for the rig).
set -uo pipefail
cd "$(dirname "$0")/.."
B=build-qnx-x86_64
SER() { python3 -I scripts/qnx-serial.py --timeout 120 -- "$@" | tr -d "\r"; }
fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }
for f in rtcollector rtc_writer; do [ -x "$B/$f" ] || { echo "no $B/$f: run make qnx ARCH=x86_64"; exit 2; }; done

PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
printf 'interval: 1\nflush_interval: 1\nrmgr_mode: 0666\ninputs: [rmgr]\noutputs:\n  - stdout: {}\n' >"$B/rmgr.yml"
printf 'interval: 1\nflush_interval: 1\nrmgr_path: /dev/rtc-nonroot\ninputs: [rmgr]\noutputs:\n  - stdout: {}\n' >"$B/rmgr-nr.yml"
(cd "$B" && exec python3 -m http.server "$PORT" --bind 127.0.0.1) >/tmp/rtc-validate-http.log 2>&1 &
HTTP=$!
trap 'kill $HTTP 2>/dev/null || true' EXIT
for _ in 1 2 3 4 5 6 7 8 9 10; do curl -fsS -o /dev/null "http://127.0.0.1:$PORT/rmgr.yml" 2>/dev/null && break; sleep 0.3; done
want=$(cksum <"$B/rtcollector" | awk '{print $1}')
got=$(SER "cd /tmp && rm -f rtcollector rtc_writer && for f in rtcollector rtc_writer rmgr.yml rmgr-nr.yml; do curl -fsS -O http://10.0.2.2:$PORT/\$f || exit 1; done; chmod 755 rtcollector rtc_writer; cksum < rtcollector" | awk '{print $1}' | tail -1)
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
out=$(SER "./rtcollector --config rmgr.yml > /tmp/rm2.out 2>&1 & P=\$!; sleep 1.5; kill \$P; sleep 0.5; grep -c 'already registered' /tmp/rm2.out; ./rtc_writer -n 5 -s after2; sleep 1.5; grep -c '^after2,' /tmp/rm.out")
[ "$(echo "$out" | grep -E '^[0-9]+$' | tr '\n' ' ')" = "1 5 " ] && ok "second instance refused; first unaffected" || { bad "second instance"; echo "$out"; }

# Non-root: registering a path needs PROCMGR_AID_PATHSPACE; the collector runs on.
out=$(SER "on -u rtuser ./rtcollector --config rmgr-nr.yml > /tmp/nr.out 2>&1 & P=\$!; sleep 1.5; kill \$P; sleep 0.5; grep -c 'needs root or PROCMGR_AID_PATHSPACE' /tmp/nr.out; grep -c 'rmgr_unavailable=1i' /tmp/nr.out; ls /dev/rtc-nonroot 2>&1 | grep -c 'No such'")
[ "$(echo "$out" | grep -E '^[0-9]+$' | tr '\n' ' ')" = "1 1 1 " ] && ok "non-root: unavailable, counted, collector runs" || { bad "non-root"; echo "$out"; }

# Priority inversion on one CPU: a prio-50 writer while a prio-5 writer floods
# and a prio-30 hog runs, all pinned with the collector to CPU 0.
out=$(SER "slay -f rtcollector >/dev/null 2>&1; sleep 0.5; on -C 0 ./rtcollector --config rmgr.yml > /tmp/rm.out 2>&1 & sleep 1; on -C 0 -p 5 ./rtc_writer -n 200000 -s low > /dev/null & L=\$!; on -C 0 -p 30 sh -c 'while :; do :; done' & H=\$!; sleep 0.5; on -C 0 -p 50 ./rtc_writer -n 2000 -u 500 -t -s high; kill \$H \$L; cat /dev/rtcollector")
echo "$out" | grep -E 'writes=2000'
max=$(echo "$out" | sed -n 's/.*writes=2000 failed=0 .*max_us=\([0-9.]*\).*/\1/p')
prio=$(echo "$out" | sed -n 's/.*max_prio_seen=\([0-9]*\).*/\1/p')
[ -n "$max" ] && awk -v m="$max" 'BEGIN{exit !(m < 5000)}' && ok "prio-50 writer under inversion load: max ${max} us (< 5 ms), no failures" || bad "inversion: max ${max:-?} us"
[ "${prio:-0}" -ge 50 ] && ok "handler ran at the writer's priority (max_prio_seen=$prio)" || bad "max_prio_seen=$prio"

# After the collector exits, writers get EBADF (they must reopen); the path is gone.
out=$(SER "./rtc_writer -n 30 -u 100000 -s late & W=\$!; sleep 1; slay -f rtcollector; wait \$W; echo w_rc=\$?; ls /dev/rtcollector 2>&1 | grep -c 'No such'")
echo "$out" | grep -q 'w_rc=1' && echo "$out" | grep -E '^1$' >/dev/null && ok "after exit: writes fail (EBADF), path removed" || { bad "after exit"; echo "$out"; }
exit $fail
