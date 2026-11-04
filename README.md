# rtcollector for QNX

A real-time-safe telemetry collector for QNX 8.0. It gathers kernel trace
events, system metrics and application metrics on the device, without touching
the code you are measuring, and streams them to [Arc](https://github.com/Basekick-Labs/arc)
or any MQTT pipeline.

It is the C successor of [rtcollector](https://github.com/Basekick-Labs/rtcollector),
our Telegraf-style collector for RedisTimeSeries, rebuilt for a real-time
operating system around one rule:

> **The collector must never be the reason a deadline is missed.**

## Status

Pre-alpha. Runs on QNX 8.0 (tested on x86_64 under QEMU; Raspberry Pi next) and on
macOS/Linux for development. This repository is Basekick Labs' entry to the
[QNX Global Hackathon](https://qnx.devpost.com/) (submission deadline
2026-12-15). Targets: Raspberry Pi 5 (aarch64) and QEMU (x86_64), QNX SDP 8.0.

## Building

Host build (macOS, Linux), for development and tests:

```sh
# macOS: brew install libyaml zstd        Linux: apt install libyaml-dev libzstd-dev libcurl4-openssl-dev
make                  # build/rtcollector
make test             # unit and integration tests
make test SANITIZE=1  # with AddressSanitizer + UBSan
make tsan             # concurrency tests under ThreadSanitizer
```

QNX 8.0 build, with QNX SDP 8.0 installed (QNX Everywhere):

```sh
source ~/qnx800/qnxsdp-env.sh
make qnx ARCH=x86_64      # build-qnx-x86_64/rtcollector (QEMU)
make qnx ARCH=aarch64le   # build-qnx-aarch64le/rtcollector (Raspberry Pi)
```

The QNX binary needs `libcurl.so.12` on the target (present on the standard
QNX images); zstd and libyaml are linked in. libyaml 0.2.5 is vendored in
`third_party/libyaml` because the SDP does not ship it.

### Trying it on QEMU

`scripts/qemu-vm.sh build|run|stop` builds a QNX 8.0 x86_64 image with
`mkqnximage` and runs it under QEMU with user networking: from the VM, the
host is `10.0.2.2`. `scripts/qnx-serial.py` runs commands over the VM's serial
console, and `scripts/qnx-validate.sh` checks the inputs against QNX's own
tools (a CPU hog pinned to each core, `pidin` memory and process counts).

## Inputs

| Input | Measurement | What |
|---|---|---|
| `internal` | `internal_collector`, `internal_output` | The collector's own counters. Always on |
| `cpu` (QNX) | `cpu`, tag `cpu` = `cpu0`…, `cpu-total` | `usage_percent`, `idle_percent`, from the run time of procnto's per-CPU idle threads. Needs no privileges. CPUs taken offline at run time are not handled |
| `mem` (QNX) | `mem` | `total_bytes`, `free_bytes`, `used_bytes`, `used_percent` |
| `processes` (QNX) | `processes` | `processes`, and `threads` when the collector may query other processes (root, or `PROCMGR_AID_XPROCESS_QUERY`). Without it, `internal_collector.procfs_denied` is 1 and only the process count is sent |
| `rmgr` (QNX) | what programs write | `/dev/rtcollector`: any program publishes metrics by writing line protocol to it. See below |
| `trace` (QNX) | `sched`, `cpu_sched`, `irq`, `internal_trace` | Scheduling latency per thread, context switches and interrupts, from the kernel trace. Needs the instrumented kernel. See below |

## Publishing from your program: `/dev/rtcollector`

With `rmgr` in `inputs`, the collector registers `/dev/rtcollector`. Any QNX
program publishes a metric by writing one line of
[line protocol](https://docs.influxdata.com/influxdb/v1/write_protocols/line_protocol_tutorial/)
per `write()`:

```c
#include "rtc_publish.h"   /* include/rtc_publish.h: header-only */

int fd = open("/dev/rtcollector", O_WRONLY);
rtc_publish(fd, "control_loop,axis=x jitter_us=%di,ok=true", jitter_us);
```

or, without the header, `snprintf` into a buffer and one `write()`. Shell:
`echo 'pump,id=3 rpm=1450i' > /dev/rtcollector`.

**Do not use `dprintf` or default-buffered stdio.** On QNX, `dprintf` sends
literal text one character per `write()`, and buffered `fprintf` cuts lines
at 1 KiB. The collector does not reassemble lines split across writes; such
fragments are counted as malformed.

What a write costs and what it promises:

- The handler copies the bytes into a preallocated buffer and replies. It
  runs on a small pool of threads (`rmgr_threads`, default 4) at a fixed
  priority (`rmgr_priority`, default 63): set it above every thread that
  writes. Measured on QEMU/KVM, all on one core: a priority-50 writer while a
  priority-5 writer floods and a priority-30 CPU hog runs: p99 about 10 µs,
  worst 43 µs over 5 runs. If the handler threads cannot get `rmgr_priority` (above 63 needs root or `PROCMGR_AID_PRIORITY`), the device is refused rather than served with that promise broken. (The Raspberry Pi measurement comes with the
  jitter harness.)
- Why a pool and a fixed priority, both learned on QNX 8: with priority
  inheritance, a low-priority writer's handler ran at that writer's priority
  and anything in between preempted it; and a low-priority writer preempted
  in the middle of its send keeps the server thread it was being delivered
  to. With one thread, a priority-50 writer waited 20–47 ms behind them.
- While the collector runs, a write waits at most for one handler run per
  writer ahead of it, unless more than `rmgr_threads` lower-priority writers
  are preempted mid-send at the same moment. If the collector is stopped
  (SIGSTOP), writes block. When it shuts down, writes fail at once with
  `EBADF` (reopen the device after it restarts).
- Writes longer than `max_write_bytes` fail with `EMSGSIZE`. Anything else
  succeeds: data lost because a buffer is full is counted, never reported to
  the writer.
- Open the device once and keep it open: opens and closes go through the
  same threads and allocate.
- Lines are parsed on the collection thread, not in `write()`. Fields:
  float, `123i` integer, `123u` unsigned (dropped if above 2^63−1), booleans
  (`t`, `true`, `True`, `TRUE`, `f`, …), `"strings"`. No leading `+`, no NUL
  bytes. An explicit timestamp must be in nanoseconds (others are refused and
  counted); without one, the receive time is used. A tag `host` replaces the
  collector's host tag. A line whose converted record exceeds
  `max_record_bytes` is counted as `too_large`.
- `cat /dev/rtcollector` prints one status line: writes, bytes, dropped,
  oversize, lines, malformed, bad timestamps, too large, the highest writer
  priority seen, and the handler priority and thread count.

Registering a path needs root (or `PROCMGR_AID_PATHSPACE`); without it the
collector runs without `/dev/rtcollector` and `rmgr_unavailable` is 1. A
second collector refuses a path that is already registered. `--once` does not
start it.

## Scheduling latency from the kernel trace: `trace`

How long does each thread wait between becoming ready and running? The
`trace` input answers it from the QNX kernel trace, for every thread, without
touching their code. Every `trace_interval` it sends, per thread that woke up:

| Measurement | Tags | Fields |
|---|---|---|
| `sched` | `process`, `thread`, `pid`, `tid` | `wakeups`, `latency_p50_ns`, `latency_p99_ns`, `latency_max_ns` (ready to running), `run_ns`, `preemptions`, `priority` (highest seen) |
| `cpu_sched` | `cpu` | `context_switches`, `interrupts` |
| `irq` | `vector` | `count` |
| `internal_trace` | | the input's own counters, below |

Latencies come from a histogram with about 12% resolution; `latency_max_ns`
is exact. Rows are every thread at or above `trace_min_priority` (the worst
p99 first if they do not all fit in `trace_max_rows`), then the `trace_top`
others with the highest p99, up to `trace_max_rows`; the rest, and threads
that woke and exited before the rows were taken, are counted in
`threads_omitted`. procnto's idle threads are left out. Rows carry the time
their interval ended.

On a 4-CPU QEMU guest, with a priority-30 thread busy 5 ms out of every 10 on
one CPU: a priority-40 thread on the same CPU shows a p99 of about 16 µs (it
preempts), a priority-20 thread shows about 6 ms (it waits out the burst).
`scripts/qnx-validate-trace.sh` checks this and the cases below on the target.

How it works and what it costs:

- The kernel writes events into its buffers; threads of the collector at
  `trace_priority` (default 15) copy them to a file in `/dev/shmem` and start
  a new file every interval; a parser thread, below the sender, reads each
  finished file. Only the thread, process and interrupt classes are traced.
  Tracing has a cost in the kernel on every event: measure it on your system
  before you keep it on in production. If `trace_priority` cannot be had
  (above 63 needs root or `PROCMGR_AID_PRIORITY`), the threads run at the
  priority they inherit and `priority_fallback` is counted.
- Memory: about 1.3 KB per tracked thread (`trace_max_threads`), and up to
  four files of `trace_max_bytes` in `/dev/shmem` (RAM): the one being
  written, two waiting, one being parsed.
- If real-time threads keep the collector's threads from running, the kernel
  drops events rather than wait (`kernel_dropped`). Bytes over
  `trace_max_bytes` in an interval are dropped (`bytes_dropped`); if the
  parser falls two intervals behind, the oldest file is dropped
  (`files_dropped`). After any of these, after a file that could not be
  parsed completely, and after a yield, the pending
  ready-to-running waits are forgotten: a gap never turns into a false
  latency.
- Each CPU's events reach the file separately, so an event can arrive after a
  later one of the same thread; it is ignored and counted (`out_of_order`).
- A thread whose exit was in dropped events would hold its table slot for
  ever. When the table is three-quarters full, threads with no event for two
  intervals that are not ready or running are dropped from it, down to half
  full (`threads_evicted`); if one runs again, its rows lack the thread name
  (thread names are traced only when a thread is created or a session
  starts). New threads past the table's size are not tracked
  (`threads_overflow` counts their events).
- Files are removed once parsed, and on exit; files left by a collector that
  was killed, or by a parse that shutdown did not wait for, are removed by
  the next collector's start. On shutdown, intervals not yet parsed are
  dropped. Shutdown runs at `collect_priority`, the parser included; the
  input's part takes at most about 7 s (5 s for the trace monitor, 2 s for
  the parser), before `shutdown_timeout`.
- Only one process can trace at a time. When another tool asks for the trace
  (`tracelogger`, for example), the collector stops and gives it the trace
  (`yielded`), and starts again when it is done (`sessions`).
- Tracing needs `procnto-smp-instr` (the kernel QNX 8.0 ships) and root, or
  `PROCMGR_AID_TRACE` (`on -A trace,allow,nonroot`). Without them, without
  `libtracelog.so.1`, when a second collector already traces, or when a trace
  session cannot start for any other reason, the input logs why,
  `internal_trace.unavailable` and `internal_collector.trace_unavailable` are
  1, and the collector runs on. Tracing then stays off until the collector is
  restarted. `--once` does not start tracing.

`internal_trace` fields, all totals since start except `parse_ns`:
`sessions`, `yielded`, `intervals`, `bytes`, `bytes_dropped`,
`files_dropped`, `events`, `kernel_dropped`, `parse_ns` (the last interval's
parse), `parse_errors` (files not read to the end, or without a clock rate),
`threads_overflow`, `out_of_order`, `threads_omitted`, `threads_evicted`,
`vectors_overflow` (interrupt vectors past 64, not reported), `snapshots_lost`
(an interval parsed but not collected), `unavailable`.

## Running

```sh
build/rtcollector --config rtcollector.yml          # run until SIGINT/SIGTERM
build/rtcollector --config rtcollector.yml --once   # one round, one send; exit 0 only if nothing was lost
```

A minimal configuration, in rtcollector's YAML format:

```yaml
interval: 10          # seconds between collection rounds
flush_interval: 10    # seconds between sends (a full batch is sent sooner)
buffer_bytes: 1MiB    # fixed buffer, allocated once at startup
tags:
  site: lab
outputs:
  - arc:
      url: http://localhost:8000
      database: telemetry
      # token: ...   or set RTC_ARC_TOKEN
```

| Key | Default | Meaning |
|---|---|---|
| `interval` | `10` | Seconds between collection rounds |
| `flush_interval` | `interval` | Seconds between sends; a send also happens when a batch is full |
| `buffer_bytes` | `1MiB` | Size of the buffer between collection and sending, allocated once. When it is full the oldest records are dropped; if an output is copying those at that moment, the newest record is dropped instead. Every drop is counted |
| `max_record_bytes` | `4096` | Largest record an input may produce (at least 512) |
| `hostname` | system hostname | Value of the `host` tag on every row |
| `tags` | none | Tags added to every row; they take precedence over tags an input sets |
| `warn_on_buffer` | `true` | Log when records are dropped because the buffer was full |
| `shutdown_timeout` | `5` | Seconds allowed for the final send on shutdown; the outputs flush in parallel, each within this budget. A request still running then is cut off, and whatever was not sent is counted as dropped |
| `collect_priority` | `10` | QNX: priority of the collection thread (SCHED_RR, 1–253). Keep it below your real-time threads. Above 63 needs root or `PROCMGR_AID_PRIORITY`; otherwise the thread keeps the inherited priority and `priority_fallback` is counted |
| `send_priority` | `9` | QNX: priority of the sender threads, one per output, so a destination that hangs never delays another. With `--once` everything runs on the main thread and neither priority applies |
| `processes_every` | `6` | Rounds between process scans (a scan costs about 50 µs per process) |
| `max_processes` | `4096` | A scan stops here; `processes_overflow` is counted |
| `rmgr_path` | `/dev/rtcollector` | Path of the device (input `rmgr`) |
| `rmgr_mode` | `0660` | Its permissions, octal. `0666` lets any user publish (and flood) |
| `rmgr_group` | `0` | Its group id |
| `rmgr_buffer_bytes` | `256KiB` | Buffer between the device and the collection thread, per handler thread (memory = this × `rmgr_threads`) |
| `rmgr_threads` | `4` | Handler threads for the device (1–16) |
| `rmgr_priority` | `63` | Fixed priority of the device's handler threads: above every writer |
| `max_write_bytes` | `4096` | Largest write accepted |
| `rmgr_drain_bytes` | `64KiB` | Bytes converted per handler-thread buffer per collection-thread step (every 20 ms at most) |
| `trace_interval` | `interval` (1 to 3600) | Seconds per trace interval (1–3600). Shorter than `interval`, intervals the collection rounds do not take are lost (`snapshots_lost`) |
| `trace_priority` | `15` | QNX: priority of the threads that copy the kernel's events (1–253). Below it, busy real-time threads make the kernel drop events (counted) |
| `trace_min_priority` | `40` | Threads at or above this priority are always reported |
| `trace_top` | `16` | Lower-priority threads reported: the ones with the highest p99 (0–256) |
| `trace_max_rows` | `64` | `sched` rows per interval, at most (1–4096) |
| `trace_max_threads` | `1024` | Threads tracked at once (16–16384), about 1.3 KB each |
| `trace_max_bytes` | `4MiB` | Trace bytes kept per interval (64KiB–64MiB); up to four times this in RAM |
| `debug` | `false` | Verbose logging (same as `-d`) |
| `inputs` | `[internal]` | Inputs to run; `internal` is always on. Names not available in this build are skipped with a warning |
| `once` | `false` | Same as `--once` |
| `outputs.arc.url` | required | Arc base URL |
| `outputs.arc.database` | `default` | Arc database (`x-arc-database`) |
| `outputs.arc.token` | none | Arc API token; `RTC_ARC_TOKEN` overrides it. rtcollector's `secret:` references are not supported |
| `outputs.arc.ca_file` | none | CA bundle for https (QNX images ship none) |
| `outputs.arc.batch_bytes` | `256KiB` | Bytes of records per request, at most (default capped at half of `buffer_bytes`) |
| `outputs.arc.timeout` | `10` | Request timeout, seconds |
| `outputs.arc.max_measurements_per_batch` | `64` | Distinct measurements per request (1 to 256) |
| `outputs.stdout` | off | Print rows as line protocol, for debugging |
| `outputs.stdout.batch_bytes` | `64KiB` | Bytes of records per write |

`max_buffer_size` from rtcollector configurations is accepted and ignored:
rtcollector counted single-value series, this collector buffers multi-field
rows.

Data goes to Arc as columnar MessagePack (`POST /api/v1/write/msgpack`,
zstd-compressed), one item per measurement per request. The collector reports
on itself in two measurements, `internal_collector` and `internal_output`:
records gathered, sent, dropped and rejected, retries and the last HTTP status.

## Licence

[Apache-2.0](LICENSE).
