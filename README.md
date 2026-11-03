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

Build and run instructions for QNX SDP 8.0 (Raspberry Pi 5 and QEMU) land with
the first code. Configuration follows rtcollector's YAML format.

## Licence

[Apache-2.0](LICENSE).
