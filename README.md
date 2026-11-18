# Innotech Genesis

ESPHome firmware and protocol notes for Innotech Genesis II BMS controllers (the DDCs that run a lot of older commercial air conditioning). It started as packet captures of the Supervisor software talking to iComm, and it's grown into an ESP32 that talks to the controllers directly, so you don't need the old Windows PC running iComm anymore.

Innotech don't support this gear or offer the software anymore, so hopefully this helps someone else keep theirs going.

## What it can do

- Each zone as a thermostat in Home Assistant: current temp, setpoint, and whether it's heating, cooling, just running the fan or off
- Turn an AHU off by forcing its supply fan off (instead of blanking its weekly schedule), and back to Auto to hand it back to the schedule
- A separate "fan force on" button per AHU, for when you really need it running
- Cooling and heating demand per zone, chilled water valve %, fan status, after hours and fire alarm inputs
- Scan a controller and log every block (number, type, name, values), which is how you find the block numbers for your site
- Talk to the bus directly over RS485, or through an existing iComm server over TCP like Supervisor does, or switch between the two automatically
- Supervisor can connect straight to the ESP as if it were an iComm server (port 20000), so you don't need the Windows PC to use Supervisor either
- A TCP bridge on port 10001 so an existing iComm server can use the ESP as its connection to the bus (a TCP/IP connection in iComm pointing at the ESP)

## Hardware

I'm using a [LilyGO T-RSS3](https://github.com/Xinyuan-LilyGO/T-RSS3) (ESP32-S3 with isolated RS485 and RS232). It runs off 7 to 24 V DC.

- **RS485:** A/B/GND to the controllers' NET +/-/SCN terminals. Don't leave another bus master on the same bus (like an iComm server's CONV232 converter) unless you set `polling: false`.
- **RS232:** a controller's front DB9 port also works, but you only see that one controller. While something is plugged into the front port, that controller stops talking on the RS485 Net, so an iComm server or anything else on the Net will see it as Dead.

## Getting started

1. Download `innotech-genesis-t-rss3.factory.bin` from the [latest release](../../releases/latest) and flash it over USB with [ESPHome Web](https://web.esphome.io).
2. It starts a hotspot called `innotech-genesis-xxxxxx`. Join it and pick your Wi-Fi.
3. Home Assistant should discover it. Add it and you'll see which controllers are online.
   If you've got an iComm server, put its IP in the "iComm server" setting and pick a "Connection mode". Auto uses the serial bus when a controller answers on it and falls back to iComm.
4. Press the "Scan controller N blocks" buttons and read the block list in the ESPHome log.
5. Copy [`esphome/example-full.yaml`](esphome/example-full.yaml), change the block numbers to match your scan, and build it in the ESPHome dashboard. Pull the component straight from this repo:

```yaml
external_components:
  - source: github://BenJamesAndo/innotech-genesis@main
    components: [innotech_genesis]
```

To use Supervisor with it, go to Communicate, Comms settings, Advanced and put in the ESP's IP as the server. Supervisor only talks to one server at a time, so with more than one ESP you either switch between them or add each ESP to your iComm server as a TCP/IP connection on port 10001.

If you've got more than one, give each a name for iComm's Current Applications list with `icomm_name: "ESPHome DDC1 plant room"`. Otherwise it uses the device name plus the end of the MAC address.

## How it talks to the controllers

The controllers speak a framed serial protocol on their RS485 "Net" at 9600 baud: `FF FF FF FF 22 DD`, a 9 byte header, CRC-16/ARC, the data, another CRC. Reading and writing a block is the same on serial and through iComm, because iComm will pass raw bus commands straight through (message type 7). That's what Gen2Xtract uses to pull logs.

Live values come from polling on serial, or from iComm's subscriptions over TCP.

A few things I found along the way:

- Over iComm, field writes rewrite the whole block from iComm's cached copy, which can quietly change other settings. The ESP reads and patches the actual block instead.
- Don't send save-to-flash through iComm. The controller goes quiet while it saves and iComm marks it Dead for about 3 minutes.
- A digital output's `Forced` / `ForceValue` fields are the manual override. Supervisor has a dialog for it, but only if the output is on one of your watch pages.
- Analogue outputs (valves, dampers) can be forced the same way. The force value is in the output's own units, so on a 0 to 1 output you send 0.4 for 40%, not 40. I found that out the hard way.
- Each controller keeps a copy of the Gen2Config file it was programmed with, and Supervisor caches it on the PC. It has the whole program in it: every block, where it sits on the drawing and how it's wired. The block links aren't stored as a table, they come from the wiring lines and labels. Heads up that it also has the panel access codes in it, so don't post yours online.
- SoftPort (in Innotech's free Utilities download) turned out to have Gen2Config's config and download code inside it, which is where most of the new command list came from.

All the details are in [`docs/PROTOCOL.md`](docs/PROTOCOL.md), with the block types in [`docs/BLOCKS.md`](docs/BLOCKS.md), everything SoftPort sends in [`docs/SOFTPORT.md`](docs/SOFTPORT.md) and the config file format in [`docs/CONFIG_FILE.md`](docs/CONFIG_FILE.md).

## Tools

[`tools/`](tools) has the Python scripts I used for the reverse engineering: the frame codec, decoders for serial port monitor exports and pcaps, a small iComm client, a raw request tool, a TCP relay for testing, decoders for Gen2Xtract's log downloads, a decoder for the config files (`g4c_decode.py`), a viewer that draws them like Gen2Config does (`g4c_view.py`, handy since Gen2Config isn't available anymore) and a tool to force or release an analogue output (`ao_force.py`). Set `ICOMM_HOST` (and `ICOMM_PASSWORD` for the web one) to point them at your iComm server.

## Node-RED (the original approach)

Before the ESPHome firmware I controlled it from Node-RED by replaying captured Supervisor packets to iComm on TCP 20000: setpoints were built from a captured header plus the temperature as a little endian double, and on/off was done by writing or blanking the weekly schedule. The flow is still in [`Node-RED flows/`](Node-RED%20flows) and exposes the AC to Home Assistant with `node-red-contrib-ha-mqtt`. It needs `node-red-contrib-string-to-hex` as well.

<img width="547" height="356" alt="image" src="https://github.com/user-attachments/assets/997872ff-0cf2-488a-8a7d-82bd1c7f2be2" />

AC controls within Home Assistant

<img width="1071" height="209" alt="image" src="https://github.com/user-attachments/assets/d5acc9c7-5aec-4847-a57a-4d1659a912c8" />

Node-RED integrating with iComm Control Centre

## Disclaimer

This is unofficial and reverse engineered. Use at your own risk, especially the force on button, which overrides the controller's own schedule, after hours and fire alarm logic. I'm not affiliated with Innotech.
