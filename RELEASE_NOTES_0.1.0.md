# rtcollector-qnx 0.1.0

The first release of rtcollector for QNX 8.0: a telemetry collector written in
C that runs on the QNX target and sends what it collects to
[Arc](https://github.com/Basekick-Labs/arc) (columnar MessagePack over HTTP)
or to an MQTT broker. It is built so that it is never the reason a real-time
deadline is missed: fixed buffers allocated at start, and nothing on the path
a measured program waits on that allocates, blocks or logs.

**Get started:** [docs/getting-started.md](https://github.com/Basekick-Labs/rtcollector-qnx/blob/v0.1.0/docs/getting-started.md),
from a blank SD card to data in Arc on a Raspberry Pi 4.

## What it collects

- **`trace`**: from the QNX kernel trace, per thread, the time between
  becoming ready and running (p50, p99, max), wakeups, run time, preemptions
  and priority; context switches and interrupts per CPU; interrupts per
  vector. Events are applied in time order across CPUs, so a thread that
  wakes on one CPU and runs on another is measured correctly.
- **`cpu`**: usage per CPU and in total, from the run time of the kernel's
  idle threads.
- **`mem`**, **`processes`**: memory, process and thread counts.
- **`rmgr`**: `/dev/rtcollector`, where any program on the target publishes
  its own metrics as line protocol, at the cost of one memory copy.
- **`internal`**: the collector's own counters; every dropped record is
  counted.

## Tested on

- Raspberry Pi 4 Model B, QNX 8.0 Quick Start Target Image (`aarch64le`).
- QEMU x86_64, QNX 8.0 (`x86_64`).

## Requirements on the target

`libcurl.so.12`, `libsocket.so.4`, `libm.so.3`, `libc.so.6` (on the QNX 8.0
images). The `trace` input also needs `libtracelog.so.1` (on the images) and
`libtraceparser.so.1`, which you copy from your QNX SDP 8.0 to `/usr/lib`.
Without them the other inputs run and `trace` reports itself unavailable.

## Licence

The source is under the Apache License 2.0. The binaries are built with the
QNX SDP 8.0 and contain no QNX code; the QNX libraries they run with are
licensed to you by QNX under their own terms. rtcollector-qnx is free for
non-commercial use and makes no claim to commercial use of QNX software. QNX
is a trademark of BlackBerry Limited; this project is not affiliated with or
endorsed by BlackBerry or QNX. See NOTICE.
