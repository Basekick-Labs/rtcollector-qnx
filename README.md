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
| `shutdown_timeout` | `5` | Seconds allowed for the final send on shutdown; a request still running then is cut off, and whatever was not sent is counted as dropped |
| `collect_priority` | `10` | QNX: priority of the collection thread (SCHED_RR, 1–253). Keep it below your real-time threads. Above 63 needs root or `PROCMGR_AID_PRIORITY`; otherwise the thread keeps the inherited priority and `priority_fallback` is counted |
| `send_priority` | `9` | QNX: priority of the sender thread. With `--once` everything runs on the main thread and neither priority applies |
| `processes_every` | `6` | Rounds between process scans (a scan costs about 50 µs per process) |
| `max_processes` | `4096` | A scan stops here; `processes_overflow` is counted |
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
