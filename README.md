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

Pre-alpha. Nothing here runs yet. This repository is Basekick Labs' entry to the
[QNX Global Hackathon](https://qnx.devpost.com/) (submission deadline
2026-12-15). Targets: Raspberry Pi 5 (aarch64) and QEMU (x86_64), QNX SDP 8.0.

## Building

The portable core builds and runs on macOS and Linux today; the QNX 8.0 build
(Raspberry Pi 5, QEMU) lands next.

```sh
# macOS: brew install libyaml zstd        Linux: apt install libyaml-dev libzstd-dev libcurl4-openssl-dev
make                  # build/rtcollector
make test             # unit and integration tests
make test SANITIZE=1  # with AddressSanitizer + UBSan
make tsan             # concurrency tests under ThreadSanitizer
```

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
| `buffer_bytes` | `1MiB` | Size of the buffer between collection and sending. When it is full the oldest records are dropped and counted |
| `max_record_bytes` | `4096` | Largest record an input may produce |
| `hostname` | system hostname | Value of the `host` tag on every row |
| `tags` | none | Tags added to every row; they take precedence over tags an input sets |
| `warn_on_buffer` | `true` | Log when records are dropped because the buffer was full |
| `shutdown_timeout` | `5` | Seconds allowed for the final send on shutdown |
| `once` | `false` | Same as `--once` |
| `outputs.arc.url` | required | Arc base URL |
| `outputs.arc.database` | `default` | Arc database (`x-arc-database`) |
| `outputs.arc.token` | none | Arc API token; `RTC_ARC_TOKEN` overrides it |
| `outputs.arc.batch_bytes` | `256KiB` | Records per request, at most |
| `outputs.arc.timeout` | `10` | Request timeout, seconds |
| `outputs.arc.max_measurements_per_batch` | `64` | Distinct measurements per request |
| `outputs.stdout` | off | Print rows as line protocol, for debugging |

`max_buffer_size` from rtcollector configurations is accepted and ignored:
rtcollector counted single-value series, this collector buffers multi-field
rows.

Data goes to Arc as columnar MessagePack (`POST /api/v1/write/msgpack`,
zstd-compressed), one item per measurement per request. The collector reports
on itself in two measurements, `internal_collector` and `internal_output`:
records gathered, sent, dropped and rejected, retries and the last HTTP status.

## Licence

[Apache-2.0](LICENSE).
