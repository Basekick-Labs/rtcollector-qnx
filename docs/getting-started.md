# Getting started: rtcollector on a Raspberry Pi 4 running QNX 8.0

From a blank SD card to scheduling latencies, interrupts and CPU usage of a
QNX system in [Arc](https://github.com/Basekick-Labs/arc), using a release
binary. About an hour, most of it downloading and flashing.

What you get: per-thread scheduling latency (how long each thread waited
between becoming ready and running), interrupts per vector, context switches
per CPU, CPU and memory usage, process counts, and anything your own programs
publish, as rows in Arc you can query with SQL.

## 1. What you need

- A Raspberry Pi 4 and a microSD card of 16 GB or more.
- A computer on the same network as the Pi, with Docker, to run Arc.
- A myQNX account and the QNX Software Center (free, non-commercial use:
  [QNX Everywhere](https://www.qnx.com/developers/docs/qnxeverywhere/index.html)).
- The QNX SDP 8.0, installed with the Software Center, for one file the trace
  input needs (step 5).

## 2. QNX on the Pi

The image is the QNX 8.0 "Quick Start Target Image" for the Raspberry Pi 4,
package `com.qnx.qnx800.quickstart.rpi4` in the QNX Software Center. Its
image comes in three parts (`com.qnx.qnx800.quickstart.rpi4.0`, `.1`, `.2`);
install all of them. Then, in the directory they were installed to:

```sh
cd images/rpi4
sh unpack_rpi4_image.sh        # joins the parts: rpi4/rpi4.img (about 12.5 GB)
```

Write `rpi4/rpi4.img` to the SD card with Raspberry Pi Imager ("Use custom",
and answer "No" to OS customisation), or with `dd`. On macOS, find the card
with `diskutil list` first (writing to the wrong disk erases it):

```sh
diskutil unmountDisk /dev/diskN
sudo dd if=rpi4/rpi4.img of=/dev/rdiskN bs=4m
diskutil mountDisk /dev/diskN
```

Wi-Fi: the card's boot partition (FAT, it mounts as `NO NAME` on macOS) has a
`wpa_supplicant.conf`. Put your network in it with a plain-text editor (not
one that turns quotes into curly quotes):

```
network={
    ssid="YourNetwork"
    key_mgmt=WPA-PSK
    psk="YourPassword"
}
```

Eject the card, put it in the Pi, power it on. After a minute or two:

```sh
ssh qnxuser@qnxpi.local      # password: qnxuser
su                           # password: root; rtcollector runs as root
```

Change these passwords if the Pi is on a network others use (`passwd`).

## 3. The clock

The Pi has no battery-backed clock: QNX starts in 1970. rtcollector drops
every row stamped before its own build date (and counts them in
`internal_collector.records_clock_invalid`), so the clock must be set.

The image sets it with `ntpdate` only when the Pi gets a new DHCP lease, which
is usually the first boot but not later ones. `rtcollector-start.sh` (in the
release) handles this: if the clock is not set, it waits for the image's own
`ntpdate`, then runs `ntpdate` itself, and only then starts the collector.
Check with `date` at any time.

Do not run two `ntpdate`s at the same time from 1970: their steps add up and
the clock lands decades ahead.

## 4. Arc on your computer

```sh
docker run -d --name arc -p 8000:8000 -v arc-data:/app/data ghcr.io/basekick-labs/arc:latest
docker logs arc 2>&1 | grep -A6 "INITIAL ADMIN TOKEN"
```

Arc starts with authentication on and prints an admin token the first time.
Keep it: rtcollector sends it, and you query with it. Note your computer's
IP address on the local network (`ipconfig getifaddr en0` on macOS, `ip -4
addr` on Linux): the QNX image does not resolve `.local` names, so the Pi
reaches Arc by IP. If a firewall is on (macOS application firewall, ufw),
allow port 8000.

## 5. rtcollector on the Pi

Download the release for the Pi from
[the releases page](https://github.com/Basekick-Labs/rtcollector-qnx/releases):
`rtcollector-<version>-qnx8.0-aarch64le.tar.gz` and `SHA256SUMS`. (The bare
binary, `rtcollector-<version>-qnx8.0-aarch64le`, is there too if you only
want the program; the tarball adds `rtcollector-start.sh`, the example
config and the header used below.) On your computer:

```sh
sha256sum -c --ignore-missing SHA256SUMS      # macOS: shasum -a 256 -c --ignore-missing SHA256SUMS
tar -xzf rtcollector-<version>-qnx8.0-aarch64le.tar.gz
scp -r rtcollector-<version>-qnx8.0-aarch64le qnxuser@qnxpi.local:/tmp/
```

On the Pi, as root, put it in a directory only root can change (it runs as
root, at boot too, so `qnxuser`'s home is not the place):

```sh
rm -rf /data/rtc && mv /tmp/rtcollector-<version>-qnx8.0-aarch64le /data/rtc
chown -R root:root /data/rtc && chmod -R go-w /data/rtc
```

(For an upgrade, the same two commands replace it; keep your
`rtcollector.yml` aside first.)

The trace input needs `libtraceparser.so.1`, which the image does not have.
Copy it from your SDP (`~/qnx800` by default):

```sh
scp ~/qnx800/target/qnx/aarch64le/usr/lib/libtraceparser.so.1 qnxuser@qnxpi.local:/tmp/
```

Then on the Pi, as root:

```sh
mv /tmp/libtraceparser.so.1 /usr/lib/ && chown root:root /usr/lib/libtraceparser.so.1
```

Without it, everything else works and the trace input reports itself
unavailable. rtcollector also looks for it next to the `rtcollector` binary,
but only in a directory and file owned by root that nobody else can write;
`/usr/lib` is simpler.

## 6. Configure and run

As root on the Pi, edit `/data/rtc/rtcollector.yml`: put your computer's IP
in `url` (port 8000). Keep the token out of it, in a file only root can read:

```sh
echo 'export RTC_ARC_TOKEN=<the admin token>' > /data/rtc/token.env
chmod 600 /data/rtc/token.env
. /data/rtc/token.env && /data/rtc/rtcollector-start.sh
```

It logs to the terminal and sends every second. Stop it with Ctrl-C (or
`kill`); it sends what it has before it exits.

## 7. See the data

On your computer (the database is `qnx`, as in the example config):

```sh
curl -s http://localhost:8000/api/v1/query \
  -H "Authorization: Bearer <the admin token>" -H "x-arc-database: qnx" \
  -H "Content-Type: application/json" \
  -d '{"sql": "SELECT process, thread, MAX(priority) AS prio, SUM(wakeups) AS wakeups, MAX(latency_max_ns) AS worst_ns FROM sched GROUP BY process, thread ORDER BY worst_ns DESC LIMIT 10"}'
```

Other tables: `irq` (interrupts per vector), `cpu_sched` (context switches
and interrupts per CPU), `cpu`, `mem`, `processes`, and the collector's own
counters in `internal_collector`, `internal_output`, `internal_trace`,
`internal_rmgr`. The README describes every field.

## 8. Publish from your program

Any program on the Pi can write line protocol to `/dev/rtcollector`:

```sh
echo 'demo,host=qnxpi value=1' > /dev/rtcollector
```

The device is root-only by default (`rmgr_mode: 0660`, group 0); set
`rmgr_mode: 0666` in the config to let any local user publish. From C,
`rtc_publish.h` (in the release) has a small helper; compiling needs the QNX
SDP. `rtc_writer` (also in the release) writes test lines.

## 9. Start at boot (optional)

The image runs `/system/etc/startup/post_startup.sh` at the end of boot. As
root, add before its last lines (`echo Process count...` and `exit 0`):

```sh
if [ -f /boot/rtcollector_on ]; then
  ( . /data/rtc/token.env && /data/rtc/rtcollector-start.sh ) > /data/var/log/rtcollector.log 2>&1 &
fi
```

and create the switch with `touch /boot/rtcollector_on`. Delete
`rtcollector_on` from the card's boot partition (from any computer) to turn
it off.

## Troubleshooting

- `trace: cannot load libtraceparser.so.1`: step 5.
- `kernel tracing needs root or PROCMGR_AID_TRACE`: run as root.
- No rows, `records_clock_invalid` growing: the clock is not set (step 3).
- `internal_output.request_errors` growing: Arc's address, port, firewall or
  token.
- The Pi drops off Wi-Fi: on a network with several access points, it may
  roam between them; the log is `slog2info | grep CTRL-EVENT`.

The source is Apache-2.0. The release binaries are built with the QNX SDP 8.0
under QNX's non-commercial licence (QNX Everywhere), and QNX software on your
Pi is licensed to you by QNX under its own terms (see NOTICE). QNX is a
trademark of BlackBerry Limited; this project is not affiliated with or
endorsed by BlackBerry, QNX or Raspberry Pi Ltd.
