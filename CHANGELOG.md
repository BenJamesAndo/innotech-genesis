# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.0.0] - 2026-11-18

### Added

- ESPHome external component `innotech_genesis` for Innotech Genesis II controllers.
- Serial (RS485 Net) bus master: frame codec with CRC-16/ARC, controller status polling, batched output reads (MVO).
- iComm TCP client: subscriptions (advise) like Supervisor, and raw bus commands passed through iComm (message type 7), so writes, overrides and scans use one code path on serial and TCP.
- `transport: serial | icomm | auto`; auto falls back to iComm when no controller answers on the bus and re-checks the bus every 10 minutes.
- Climate platform: one thermostat per zone with current temperature, setpoint, and heating / cooling / fan / off action from the zone heaters, CHW valve and supply fan.
- Off mode that forces the AHU supply fan output off (manual override), and Auto to release it.
- `force_on` button platform for forcing a supply fan on.
- Sensors for zone cooling and heating demand (PID loop Direct / Reverse outputs), with `icomm_field` for named iComm outputs.
- Binary sensors for controller online state, fan status, after hours latches and fire alarm inputs.
- Number platform for User Variable blocks (setpoints, timers).
- Block scan button that logs every block of a controller.
- TCP bridge on port 10001 so iComm and Supervisor can reach the bus through the ESP.
- `icomm_name` option for the name shown in iComm's Current Applications (default adds the end of the MAC address).
- Generic release firmware for the LilyGO T-RSS3 (`esphome/innotech-genesis.yaml`), with Wi-Fi set up from a hotspot.
- Full worked example config (`esphome/example-full.yaml`).
- Protocol notes (`docs/PROTOCOL.md`): serial framing, command table, block record layout, iComm TCP messages, raw passthrough, log extraction.
- Python tools for the frame codec, capture decoding, an iComm client, raw requests, a TCP relay and Gen2Xtract log decoding.
- GitHub Actions workflow that builds the firmware and attaches it to each release.

[Unreleased]: https://github.com/BenJamesAndo/innotech-genesis/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/BenJamesAndo/innotech-genesis/releases/tag/v1.0.0
