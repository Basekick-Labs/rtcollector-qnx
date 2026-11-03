#!/usr/bin/env bash
# On-target validation of the QNX inputs, run from the host that runs the VM
# (see qemu-vm.sh). Serves build-qnx-x86_64/ over HTTP, pulls the binary onto
# the VM, and checks each input against QNX's own tools.
set -euo pipefail
cd "$(dirname "$0")/.."
SER="python3 -I scripts/qnx-serial.py --timeout 90 --"
B=build-qnx-x86_64
printf 'interval: 1\nflush_interval: 1\nprocesses_every: 1\noutputs:\n  - stdout: {}\ninputs: [cpu, mem, processes]\n' >$B/validate.yml
(cd $B && exec python3 -m http.server 28080 --bind 127.0.0.1 >/dev/null 2>&1) &
HTTP=$!
trap 'kill $HTTP 2>/dev/null' EXIT
sleep 1
$SER "cd /tmp && rm -f rtcollector validate.yml && curl -s -O http://10.0.2.2:28080/rtcollector && curl -s -O http://10.0.2.2:28080/validate.yml && chmod +x rtcollector && echo deployed"
fail=0
ncpu=$($SER "pidin info | grep -c '^Processor'" | tr -dc '0-9')
echo "VM has $ncpu CPUs"
for k in $(seq 0 $((ncpu - 1))); do
	out=$($SER "on -C $k sh -c 'while :; do :; done' & H=\$!; sleep 1; ./rtcollector --config validate.yml --once | grep '^cpu,'; kill \$H")
	busy=$(echo "$out" | awk -v k="cpu$k" -F'[ ,=]' '$0 ~ "cpu="k" " {for (i=1;i<=NF;i++) if ($i=="usage_percent") print int($(i+1))}')
	others=$(echo "$out" | awk -v k="cpu$k" -F'[ ,=]' '$0 !~ "cpu="k" " && $0 !~ "cpu-total" {for (i=1;i<=NF;i++) if ($i=="usage_percent") if ($(i+1) > m) m=$(i+1)} END {print int(m)}')
	if [ "${busy:-0}" -gt 90 ] && [ "${others:-100}" -lt 10 ]; then echo "ok   cpu$k pinned hog: $busy% (others max $others%)"; else echo "FAIL cpu$k pinned hog: $busy% (others max $others%)"; echo "$out"; fail=1; fi
done
out=$($SER "./rtcollector --config validate.yml --once | grep -E '^(mem|processes)'; pidin info | head -1; pidin info | grep Processes")
echo "$out"
free=$(echo "$out" | awk -F'free_bytes=' '/^mem/ {split($2,a,"i"); print int(a[1]/1048576)}')
pfree=$(echo "$out" | sed -n 's/.*FreeMem:\([0-9]*\)MB.*/\1/p')
if [ -n "$free" ] && [ -n "$pfree" ] && [ $(( free > pfree ? free - pfree : pfree - free )) -le $(( pfree / 50 + 1 )) ]; then echo "ok   mem free ${free}MB vs pidin ${pfree}MB"; else echo "FAIL mem free ${free}MB vs pidin ${pfree}MB"; fail=1; fi
procs=$(echo "$out" | awk -F'processes=' '/^processes/ {split($2,a,"i"); print a[1]}')
pprocs=$(echo "$out" | sed -n 's/Processes: \([0-9]*\),.*/\1/p')
if [ -n "$procs" ] && [ -n "$pprocs" ] && [ $(( procs > pprocs ? procs - pprocs : pprocs - procs )) -le 5 ]; then echo "ok   processes $procs vs pidin $pprocs"; else echo "FAIL processes $procs vs pidin $pprocs"; fail=1; fi
exit $fail
