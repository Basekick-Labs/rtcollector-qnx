#!/usr/bin/env bash
# On-target checks for the MQTT output: VM collector -> mosquitto -> Arc's
# MQTT subscription -> SQL. Run on the host that runs the VM (see
# qnx-validate.sh), with:
#   - mosquitto installed (its own service disabled); this script runs its
#     own broker on port 18830 from $RTC_MQTT_RIG (default ~/rtc-mqtt), with
#     persistence and a password file (user rtc);
#   - Arc on port 8100 with MQTT enabled (ARC_MQTT_ENABLED=true and an
#     ARC_ENCRYPTION_KEY), subscribed to rtcollector/+/+ into database mqtt
#     (created here when missing).
# Exit 0 only if every check passed.
set -uo pipefail
cd "$(dirname "$0")/.."
B=build-qnx-x86_64
RIG=${RTC_MQTT_RIG:-$HOME/rtc-mqtt}
ARC=${RTC_ARC:-http://127.0.0.1:8100}
SER() { python3 -I scripts/qnx-serial.py --timeout 150 -- "$@" | tr -d "\r"; }
fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }
for f in rtcollector rtc_writer; do [ -x "$B/$f" ] || { echo "no $B/$f: run make qnx ARCH=x86_64"; exit 2; }; done
command -v mosquitto >/dev/null || { echo "mosquitto is not installed"; exit 2; }
ID=v$(date +%s | tail -c 6) # measurement prefix: fresh per run

# --- broker ------------------------------------------------------------------
mkdir -p "$RIG/data"
[ -f "$RIG/passwd" ] || { mosquitto_passwd -b -c "$RIG/passwd" rtc rtcpass && chmod 600 "$RIG/passwd"; }
broker_stop() {
	[ -f "$RIG/broker.pid" ] || return 0
	local p; p=$(cat "$RIG/broker.pid")
	kill "$p" 2>/dev/null
	for _ in $(seq 50); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
	rm -f "$RIG/broker.pid"
}
# broker_start [extra conf lines]
broker_start() {
	broker_stop
	{ printf 'listener 18830 0.0.0.0\nallow_anonymous false\npassword_file %s/passwd\n' "$RIG"
	  printf 'persistence true\npersistence_location %s/data/\nlog_dest stderr\n' "$RIG"
	  [ -n "${1:-}" ] && printf '%s\n' "$1"; } >"$RIG/run.conf"
	mosquitto -c "$RIG/run.conf" >>"$RIG/broker.log" 2>&1 &
	echo $! >"$RIG/broker.pid"
	for _ in $(seq 50); do mosquitto_pub -p 18830 -u rtc -P rtcpass -t rig/ping -m 1 2>/dev/null && return 0; sleep 0.1; done
	echo "broker did not start"; tail -5 "$RIG/broker.log"; exit 2
}
retained() { mosquitto_sub -p 18830 -u rtc -P rtcpass -t "$1" -C 1 -W 3 2>/dev/null; }
broker_start
trap 'broker_start; kill $HTTP $HUNG 2>/dev/null || true' EXIT

# --- Arc ---------------------------------------------------------------------
q() { curl -s -X POST "$ARC/api/v1/query" -H 'Content-Type: application/json' -d "{\"sql\":\"$1\"}" |
	python3 -c 'import json,sys
d=json.load(sys.stdin); r=(d.get("data") or [[None]])[0]
print(" ".join(str(x) for x in r) if d.get("success") else "ERR " + str(d.get("error"))[:200])'; }
# wait_rows TABLE N: polls until count(*) >= N (Arc buffers before it writes)
wait_rows() { local n; for _ in $(seq 40); do n=$(q "SELECT count(*) FROM $1" | tr -dc '0-9'); [ "${n:-0}" -ge "$2" ] && break; sleep 0.5; done; echo "${n:-0}"; }
curl -fsS "$ARC/api/v1/mqtt/health" >/dev/null || { echo "Arc MQTT API not reachable at $ARC"; exit 2; }
if ! curl -s "$ARC/api/v1/mqtt/subscriptions" | grep -q '"rtcollector/+/+"'; then
	curl -s -X POST "$ARC/api/v1/mqtt/subscriptions" -H 'Content-Type: application/json' -d '{"name":"rtc","broker":"tcp://127.0.0.1:18830","client_id":"arc-rtc-sub","topics":["rtcollector/+/+"],"qos":1,"database":"mqtt","username":"rtc","password":"rtcpass","auto_start":true}' >/dev/null
fi
SUB=$(curl -s "$ARC/api/v1/mqtt/subscriptions" | python3 -c 'import json,sys; print([s["id"] for s in json.load(sys.stdin)["subscriptions"] if "rtcollector/+/+" in s["topics"]][0])')
curl -s -X POST "$ARC/api/v1/mqtt/subscriptions/$SUB/start" >/dev/null

# --- configs and deploy --------------------------------------------------------
M='url: "mqtt://10.0.2.2:18830", username: rtc, password: rtcpass'
cat >"$B/mq.yml" <<EOF
interval: 1
flush_interval: 1
inputs: [rmgr]
outputs:
  - mqtt: {$M}
EOF
cat >"$B/mq-json.yml" <<EOF
interval: 1
flush_interval: 1
inputs: [rmgr]
outputs:
  - mqtt: {$M, format: json, time_precision: ms, keepalive: 0, status_topic: '', client_id: rtcvmjson}
EOF
cat >"$B/mq-badpw.yml" <<EOF
interval: 1
flush_interval: 1
inputs: [rmgr]
outputs:
  - mqtt: {url: "mqtt://10.0.2.2:18830", username: rtcwrong, password: secondpw}
EOF
cat >"$B/mq-hung.yml" <<EOF
interval: 1
flush_interval: 1
shutdown_timeout: 2
inputs: [rmgr]
outputs:
  - arc: {url: "http://10.0.2.2:8100", database: mqtt}
  - mqtt: {url: "mqtt://10.0.2.2:18831", timeout: 30}
EOF
cat >"$B/mq-all.yml" <<EOF
interval: 1
flush_interval: 1
inputs: [cpu, mem, trace, rmgr]
outputs:
  - arc: {url: "http://10.0.2.2:8100", database: rtc_http}
  - mqtt: {$M}
EOF
cat >"$B/mq-small.yml" <<EOF
interval: 1
flush_interval: 1
max_record_bytes: 512
buffer_bytes: 4MiB
inputs: [rmgr]
outputs:
  - mqtt: {$M, max_message_bytes: 4KiB, batch_bytes: 1MiB}
EOF
cat >"$B/mq-bigmsg.yml" <<EOF
interval: 1
flush_interval: 1
inputs: [rmgr]
outputs:
  - mqtt: {$M, max_message_bytes: 1MiB, batch_bytes: 64KiB}
EOF
# One row of about 3 KB (one write to /dev/rtcollector).
python3 -c 'import sys; print(sys.argv[1] + ",mode=big s=\"" + "x" * 3000 + "\"")' "${ID}b" >"$B/big.lp"
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
(cd "$B" && exec python3 -m http.server "$PORT" --bind 127.0.0.1) >/tmp/rtc-validate-http.log 2>&1 &
HTTP=$!
# A listener that accepts and never answers.
python3 -c 'import socket,time
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("0.0.0.0",18831)); s.listen(8)
c=[]
while True: c.append(s.accept()[0])' &
HUNG=$!
for _ in $(seq 10); do curl -fsS -o /dev/null "http://127.0.0.1:$PORT/mq.yml" 2>/dev/null && break; sleep 0.3; done
want=$(cksum <"$B/rtcollector" | awk '{print $1}')
got=$(SER "cd /tmp && slay -f -s SIGKILL rtcollector >/dev/null 2>&1; rm -f rtcollector rtc_writer && for f in rtcollector rtc_writer mq.yml mq-json.yml mq-badpw.yml mq-hung.yml mq-all.yml mq-small.yml mq-bigmsg.yml big.lp; do curl -fsS -O http://10.0.2.2:$PORT/\$f || exit 1; done; chmod 755 rtcollector rtc_writer; cksum < rtcollector" | awk '{print $1}' | tail -1)
[ "$got" = "$want" ] && ok "deployed (cksum $want)" || { bad "deploy: cksum $got vs $want"; exit 1; }
HOST=$(SER "hostname" | tail -1)
# run CONFIG SERIES N [writer args] [seconds after]: collector, writer, clean stop; prints the log.
run() { SER "cd /tmp; ./rtcollector --config $1 > /tmp/mq.out 2>&1 & P=\$!; sleep 2; ./rtc_writer -n $3 -m publish -s $2 ${4:-}; sleep ${5:-3}; kill \$P; sleep 3; cat /tmp/mq.out"; }

# 1. Clean run, msgpack: every row once; presence online while running, offline after.
mosquitto_sub -p 18830 -u rtc -P rtcpass -t "rtcollector/$HOST/${ID}a" -C 1 -W 20 -v >/tmp/rtc-mq-sub.out 2>/dev/null &
SUBP=$!
out=$(SER "cd /tmp; ./rtcollector --config mq.yml > /tmp/mq.out 2>&1 & P=\$!; sleep 2; ./rtc_writer -n 300 -m publish -s ${ID}a; sleep 3; echo STOP; kill \$P; sleep 3; cat /tmp/mq.out")
wait $SUBP
n=$(wait_rows "mqtt.${ID}a" 300); d=$(q "SELECT count(DISTINCT seq) FROM mqtt.${ID}a")
[ "$n" = 300 ] && [ "$d" = 300 ] && ok "msgpack: 300 rows written, 300 in Arc, 300 distinct" || { bad "msgpack: rows=$n distinct=$d"; echo "$out" | tail -5; }
grep -q "^rtcollector/$HOST/${ID}a " /tmp/rtc-mq-sub.out && ok "topic rtcollector/$HOST/${ID}a (seen by mosquitto_sub)" || bad "topic not seen"
[ "$(retained "rtcollector/$HOST")" = offline ] && ok "presence: retained offline after a clean stop" || bad "presence after clean stop: $(retained "rtcollector/$HOST")"
ty=$(q "SELECT typeof(seq) FROM mqtt.${ID}a LIMIT 1")
echo "     column seq from msgpack: $ty"

# 2. JSON, ms precision, keepalive 0, no presence, a set client id.
mosquitto_sub -p 18830 -u rtc -P rtcpass -t "rtcollector/$HOST/${ID}j" -C 1 -W 20 >/tmp/rtc-mq-json.out 2>/dev/null &
SUBP=$!
out=$(run mq-json.yml "${ID}j" 100)
wait $SUBP
n=$(wait_rows "mqtt.${ID}j" 100)
[ "$n" = 100 ] && ok "json: 100 rows in Arc" || { bad "json: rows=$n"; echo "$out" | tail -5; }
head -c 160 /tmp/rtc-mq-json.out | grep -q '^\[{"m":"'"${ID}j"'","t":1[0-9]\{12\},' && ok "json payload: $(head -c 100 /tmp/rtc-mq-json.out)..." || bad "json payload: $(head -c 160 /tmp/rtc-mq-json.out)"
age=$(q "SELECT CAST(epoch_ms(now()) - epoch_ms(max(time)) AS BIGINT) FROM mqtt.${ID}j" | tr -dc '0-9-')
[ -n "$age" ] && [ "$age" -ge 0 ] && [ "$age" -lt 120000 ] && ok "ms timestamps land at the right time (${age} ms ago)" || bad "json time: age $age ms"
echo "$out" | grep -q 'as client rtcvmjson' && ! echo "$out" | grep -q 'presence on' && ok "client_id set, no presence" || bad "client id / presence: $(echo "$out" | grep 'as client')"
# JSON into a measurement first written from msgpack (Int64 seq): what does Arc do?
out=$(run mq-json.yml "${ID}a" 50)
n=$(wait_rows "mqtt.${ID}a" 350)
echo "     json into the msgpack measurement: rows $n (want 350), seq types: $(q "SELECT string_agg(DISTINCT typeof(seq), ',') FROM mqtt.${ID}a")"

# 3. Broker killed mid-run and restarted: nothing lost, duplicates possible.
( sleep 3; broker_stop; sleep 3; broker_start ) &
KP=$!
out=$(run mq.yml "${ID}k" 400 "-u 10000" 12)
wait $KP
n=$(wait_rows "mqtt.${ID}k" 400); d=$(q "SELECT count(DISTINCT seq) FROM mqtt.${ID}k")
[ "$d" = 400 ] && [ "${n:-0}" -ge 400 ] && ok "broker restart: 400 distinct of 400, $((n - 400)) duplicates" || { bad "broker restart: rows=$n distinct=$d"; echo "$out" | tail -8; }

# 4. Wrong password: one error, data kept, resumes once the broker accepts it.
out=$(SER "cd /tmp; ./rtcollector --config mq-badpw.yml > /tmp/mq.out 2>&1 & P=\$!; sleep 2; ./rtc_writer -n 50 -m publish -s ${ID}p; sleep 5; echo \$P > /tmp/mq.pid")
mosquitto_passwd -b "$RIG/passwd" rtcwrong secondpw && kill -HUP "$(cat "$RIG/broker.pid")"
out=$(SER "cd /tmp; sleep 12; kill \$(cat /tmp/mq.pid); sleep 3; cat /tmp/mq.out")
mosquitto_passwd -D "$RIG/passwd" rtcwrong && kill -HUP "$(cat "$RIG/broker.pid")"
n=$(wait_rows "mqtt.${ID}p" 50); e=$(echo "$out" | grep -c 'refused the connection')
[ "$n" = 50 ] && [ "$e" = 1 ] && ok "wrong password: one error logged, 50/50 rows after the fix" || { bad "wrong password: rows=$n errors=$e"; echo "$out" | tail -6; }

# 5. max_packet_size below a message: the ceiling halves and the data arrives;
#    a row above it alone is dropped after 3 closes.
broker_start "max_packet_size 2048"
out=$(SER "cd /tmp; ./rtcollector --config mq.yml > /tmp/mq.out 2>&1 & P=\$!; sleep 2; ./rtc_writer -n 300 -m publish -s ${ID}c; cat big.lp > /dev/rtcollector; sleep 12; kill \$P; sleep 3; cat /tmp/mq.out")
broker_start
n=$(wait_rows "mqtt.${ID}c" 300); d=$(q "SELECT count(DISTINCT seq) FROM mqtt.${ID}c")
echo "$out" | grep -q 'sending messages of at most' && [ "$d" = 300 ] && ok "packet limit: ceiling lowered, 300/300 distinct ($n rows)" || { bad "packet limit: rows=$n distinct=$d"; echo "$out" | grep mqtt | tail -5; }
echo "$out" | grep -q 'closed the connection 3 times on a' && ok "one row over the limit: dropped after 3 closes" || { bad "one-row exit"; echo "$out" | grep mqtt | tail -5; }

# 6. kill -9: the broker publishes the will.
out=$(SER "cd /tmp; ./rtcollector --config mq.yml > /tmp/mq.out 2>&1 & echo \$! > /tmp/mq.pid; sleep 2; ./rtc_writer -n 5 -m publish -s ${ID}w; sleep 3")
on=$(retained "rtcollector/$HOST")
SER "kill -9 \$(cat /tmp/mq.pid)" >/dev/null
sleep 2
off=$(retained "rtcollector/$HOST")
[ "$on" = online ] && [ "$off" = offline ] && ok "presence: online while running, will offline after kill -9" || bad "presence: running=$on after kill -9=$off"

# 7. A broker that accepts and never answers: stop and --once within the
#    budget, and the Arc output keeps sending meanwhile.
out=$(SER "cd /tmp; ./rtcollector --config mq-hung.yml > /tmp/mq.out 2>&1 & P=\$!; sleep 2; ./rtc_writer -n 100 -m publish -s ${ID}h; sleep 3; a=\$(date +%s); kill \$P; while kill -0 \$P 2>/dev/null; do sleep 0.1; done; b=\$(date +%s); echo stop_s=\$((b - a)); a=\$(date +%s); ./rtcollector --config mq-hung.yml --once > /dev/null 2>&1; echo once_rc=\$?; b=\$(date +%s); echo once_s=\$((b - a))")
s=$(echo "$out" | sed -n 's/^stop_s=//p'); os=$(echo "$out" | sed -n 's/^once_s=//p')
n=$(wait_rows "mqtt.${ID}h" 100)
[ "${s:-9}" -le 3 ] && [ "${os:-9}" -le 4 ] && [ "$n" = 100 ] && ok "hung broker: stop ${s}s, --once ${os}s (shutdown_timeout 2), Arc output 100/100" || { bad "hung broker: stop=$s once=$os arc rows=$n"; echo "$out"; }

# 8. arc + mqtt + trace + rmgr together.
out=$(run mq-all.yml "${ID}t" 200)
n=$(wait_rows "mqtt.${ID}t" 200); h=$(wait_rows "rtc_http.${ID}t" 200)
ns=$(q "SELECT count(*) FROM mqtt.sched" | tr -dc '0-9')
[ "$n" = 200 ] && [ "$h" = 200 ] && [ "${ns:-0}" -gt 0 ] && ok "arc + mqtt + trace + rmgr: 200/200 over each output, sched rows over mqtt" || { bad "together: mqtt=$n http=$h sched=$ns"; echo "$out" | tail -5; }

# 9. Non-root: sockets need no ability.
out=$(SER "cd /tmp; on -u rtuser ./rtcollector --config mq.yml --once > /tmp/mq.out 2>&1; echo rc=\$?; cat /tmp/mq.out | tail -3")
echo "$out" | grep -q '^rc=0' && ok "non-root --once over mqtt" || { bad "non-root"; echo "$out"; }

# 10. Non-default sizes: 4KiB messages with a 1MiB batch, 1MiB messages with a 64KiB batch.
out=$(run mq-small.yml "${ID}s" 500)
n=$(wait_rows "mqtt.${ID}s" 500); d=$(q "SELECT count(DISTINCT seq) FROM mqtt.${ID}s")
[ "$d" = 500 ] && ok "max_message_bytes 4KiB, batch 1MiB: 500/500 ($n rows)" || { bad "small messages: rows=$n distinct=$d"; echo "$out" | tail -5; }
out=$(run mq-bigmsg.yml "${ID}m" 2000)
n=$(wait_rows "mqtt.${ID}m" 2000); d=$(q "SELECT count(DISTINCT seq) FROM mqtt.${ID}m")
[ "$d" = 2000 ] && ok "max_message_bytes 1MiB, batch 64KiB: 2000/2000 ($n rows)" || { bad "big messages: rows=$n distinct=$d"; echo "$out" | tail -5; }

exit $fail
