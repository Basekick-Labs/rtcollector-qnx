#!/usr/bin/env bash
# The collector's overhead on a 1 kHz control loop. Run on the host next to
# the target (Atila):
#
#   scripts/rtc-bench.sh -T ssh:root@pi4 [-s S0a,S0b,Cplus,S1,...] [-n 5] [-t 60]
#                        [-m precise] [-c 3] [--arc URL] [--mqtt URL] [--seed N] [-o DIR]
#   scripts/rtc-bench.sh -T vm -t 5 -n 1     # dry run on the QEMU VM: no timing claims
#
# Deploys rtcollector, rtc_loop, rtc_hog, rtc_writer and rtc-batch.sh, checks
# the target's clock, starts the whole batch detached on the target (in a
# randomised order, every scenario -n times), waits without a live session,
# pulls the results and prints the pooled table (rtc-bench-report.py).
# Scenarios: see rtc-batch.sh and the README ("Overhead").
set -uo pipefail
cd "$(dirname "$0")/.."
TARGET=vm SCEN="S0a,S0b,Cplus,S1,S2,S3,S3m,S3h,S4,S4o,S6r,S6b" RUNS=5 SECS=60 MECH=precise CPU=3
ARC="" MQTT="" SEED=$RANDOM OUT=""
while [ $# -gt 0 ]; do
	case $1 in
	-T) TARGET=$2; shift ;;
	-s) SCEN=$2; shift ;;
	-n) RUNS=$2; shift ;;
	-t) SECS=$2; shift ;;
	-m) MECH=$2; shift ;;
	-c) CPU=$2; shift ;;
	--arc) ARC=$2; shift ;;
	--mqtt) MQTT=$2; shift ;;
	--seed) SEED=$2; shift ;;
	-o) OUT=$2; shift ;;
	*) sed -n '2,14p' "$0"; exit 2 ;;
	esac
	shift
done
TDIR=/tmp/rtc-bench
OUT=${OUT:-bench-results/$(date -u +%Y%m%dT%H%M%SZ)-${TARGET//[:@\/]/_}}
mkdir -p "$OUT"

case $TARGET in
vm)
	B=build-qnx-x86_64
	HOSTIP=10.0.2.2
	SER() { python3 -I scripts/qnx-serial.py --timeout "${2:-90}" -- "$1" | tr -d '\r'; }
	run() { SER "$1" "${2:-90}"; }
	;;
ssh:*)
	B=build-qnx-aarch64le
	DEST=${TARGET#ssh:}
	HOSTIP=$(ip route get "$(getent hosts "${DEST#*@}" | awk '{print $1}' | head -1)" 2>/dev/null |
		sed -n 's/.* src \([0-9.]*\).*/\1/p')
	run() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$DEST" "$1"; }
	;;
*) echo "-T vm or -T ssh:user@host"; exit 2 ;;
esac
ARC=${ARC:-http://$HOSTIP:8100}
MQTT=${MQTT:-mqtt://$HOSTIP:18830}
for f in rtcollector rtc_loop rtc_hog rtc_writer; do
	[ -x "$B/$f" ] || { echo "no $B/$f: make qnx ARCH=${B#build-qnx-}"; exit 2; }
done
cp scripts/rtc-batch.sh "$B/"

# The target's clock: rows are stamped with CLOCK_REALTIME (no RTC on a Pi).
t_target=$(run "date -u +%s" | tr -dc '0-9' | tail -c 10)
skew=$(($(date -u +%s) - ${t_target:-0}))
if [ "${skew#-}" -gt 1 ]; then
	echo "target clock is ${skew} s off the host; setting it from the host"
	run "date -u $(date -u +%Y%m%d%H%M.%S)" >/dev/null
	t_target=$(run "date -u +%s" | tr -dc '0-9' | tail -c 10)
	skew=$(($(date -u +%s) - ${t_target:-0}))
	[ "${skew#-}" -le 1 ] || { echo "cannot set the target clock (off by $skew s)"; exit 1; }
fi

# Randomised order, every scenario RUNS times.
ORDERED=$(python3 -c 'import random, sys
s = sys.argv[1].split(",") * int(sys.argv[2]); random.Random(int(sys.argv[3])).shuffle(s); print(" ".join(s))' "$SCEN" "$RUNS" "$SEED")
n=$(echo "$ORDERED" | wc -w)
eta=$((n * (SECS + 9)))
{
	echo "target=$TARGET arch=${B#build-qnx-} loop_cpu=$CPU mech=$MECH secs=$SECS runs=$RUNS seed=$SEED"
	echo "arc=$ARC mqtt=$MQTT clock_skew_s=$skew collector=$( (git rev-parse --short HEAD 2>/dev/null || echo nogit) | tr -d '\n') cksum=$(cksum <"$B/rtcollector" | awk '{print $1}')"
	echo "order: $ORDERED"
} | tee "$OUT/batch.txt"
echo "$TDIR/run $SECS $ARC $MQTT $CPU $MECH $ORDERED" >"$B/rtc-batch.args"

# Deploy.
want=$(cksum <"$B/rtcollector" | awk '{print $1}')
case $TARGET in
vm)
	PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
	(cd "$B" && exec python3 -m http.server "$PORT" --bind 127.0.0.1) >/dev/null 2>&1 &
	HTTP=$!
	trap 'kill $HTTP 2>/dev/null' EXIT
	sleep 1
	got=$(run "mkdir -p $TDIR/bin && cd $TDIR/bin && for f in rtcollector rtc_loop rtc_hog rtc_writer rtc-batch.sh rtc-batch.args; do rm -f \$f; curl -fsS -O http://$HOSTIP:$PORT/\$f || exit 1; done; chmod 755 *; cksum < rtcollector" | awk '{print $1}' | tail -1)
	;;
*)
	run "mkdir -p $TDIR/bin" && scp -q "$B"/{rtcollector,rtc_loop,rtc_hog,rtc_writer,rtc-batch.sh,rtc-batch.args} "$DEST:$TDIR/bin/" &&
		run "chmod 755 $TDIR/bin/*"
	got=$(run "cksum < $TDIR/bin/rtcollector" | awk '{print $1}')
	;;
esac
[ "$got" = "$want" ] || { echo "deploy failed (cksum $got vs $want)"; exit 1; }
NCPU=$(run "pidin info | grep -c '^Processor'" | tr -dc '0-9')
[ "${CPU}" -lt "${NCPU:-0}" ] || { echo "the target has ${NCPU:-?} CPUs; -c $CPU is not one of them"; exit 1; }

echo "about $((eta / 60)) min; results in $OUT"
# Detached, stdin from /dev/null. The line must not end in "&": the console
# helper appends "; echo ...", and "&;" is a syntax error. Over ssh, nohup
# keeps the batch alive when the session closes.
NOHUP=""
[ "$TARGET" = vm ] || NOHUP=nohup
run "rm -rf $TDIR/run; $NOHUP sh $TDIR/bin/rtc-batch.sh @$TDIR/bin/rtc-batch.args > $TDIR/batch.log 2>&1 < /dev/null & echo started" >/dev/null
sleep 5
run "test -s $TDIR/batch.log || test -d $TDIR/run && echo STARTED" 30 | grep -q STARTED ||
	{ echo "the batch did not start on the target"; exit 1; }

# Wait without holding a session open.
sleep "$eta"
for _ in $(seq 120); do
	run "test -f $TDIR/run/done && echo DONE" 30 | grep -q DONE && break
	sleep 30
done

# Pull.
case $TARGET in
vm)
	UP=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
	python3 -c 'import http.server, os, sys
root = sys.argv[2]
class H(http.server.BaseHTTPRequestHandler):
    def do_PUT(self):
        p = os.path.normpath(os.path.join(root, self.path.lstrip("/")))
        if not p.startswith(os.path.abspath(root)): self.send_response(403); self.end_headers(); return
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(self.rfile.read(int(self.headers["Content-Length"])))
        self.send_response(201); self.end_headers()
    def log_message(self, *a): pass
http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()' "$UP" "$(cd "$OUT" && pwd)" &
	PUTS=$!
	trap 'kill $HTTP $PUTS 2>/dev/null' EXIT
	sleep 1
	run "cd $TDIR/run && for f in */* done; do curl -fsS -T \$f http://$HOSTIP:$UP/\$f || echo FAILED \$f; done; curl -fsS -T $TDIR/batch.log http://$HOSTIP:$UP/batch.log" 600 | grep FAILED
	;;
*)
	scp -q -r "$DEST:$TDIR/run/." "$OUT/" && scp -q "$DEST:$TDIR/batch.log" "$OUT/"
	;;
esac
[ -f "$OUT/done" ] || echo "warning: the batch did not finish (no done file); reporting what arrived"
python3 scripts/rtc-bench-report.py "$OUT" --csv "$OUT/summary.csv" | tee "$OUT/report.md"
