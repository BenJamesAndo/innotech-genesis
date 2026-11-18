# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.1.0] - 2026-11-19

### Added

- "iComm server" and "Connection mode" settings in the release firmware, so the iComm server IP and serial / iComm / auto can be set from Home Assistant or the web page without rebuilding. Backed by new `set_icomm_server()` and `set_transport_mode()` methods. An empty iComm server goes back to the YAML's `icomm_hosts`.
- Release firmware shows "Using iComm server" and "iComm connected", and the status LED shows the iComm states again.

### Fixed

- A write or override attempted while the iComm connection was down could leave the write queue stuck, so later setpoint changes and overrides were silently ignored until a reboot.
- In iComm mode, controllers now show offline as soon as the iComm connection drops for any reason (send failure, silence, framing), not only when the server closes it.
- A forced `icomm` connection mode could switch itself back to serial if a late serial reply arrived; only `auto` switches now.
- A force read before its ForceValue arrived could briefly show Off for a forced-on fan.
- After a reboot, setpoints from the controller were ignored for 15 seconds even though nothing had been written.
- iComm updates with a value type it doesn't know no longer misread the rest of the batch.
- The iComm client socket is always reserved, so a server set at runtime has one even when `icomm_hosts` is empty.
- `max_points_per_message` is limited to 30, the size the request buffer holds.
- GPIO3 strapping pin warning (the T-RSS3 wires RS485 TX to GPIO3).
- Build workflow uses Node 24 versions of the GitHub actions and a pinned Ubuntu runner.

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

[Unreleased]: https://github.com/BenJamesAndo/innotech-genesis/compare/v1.1.0...HEAD
[1.1.0]: https://github.com/BenJamesAndo/innotech-genesis/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/BenJamesAndo/innotech-genesis/releases/tag/v1.0.0
