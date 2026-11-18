# Innotech Genesis II protocol notes

Working notes from building the ESPHome bridge in `esphome/`. Sources: reverse engineering of iComm 1.30
(`icomm.exe`) and SoftPort 5.40D (`GEN2VPRT.exe`, which carries Gen2Config's config and transfer code), serial
captures of an iComm 2.38 server's COM port, TCP captures of Supervisor 5.40 and Gen2Xtract talking to that
server, and tests against iComm 4.4. The captures themselves aren't published (they're from a live site).
Tool scripts mentioned below are in `tools/`.

Other docs:
- [BLOCKS.md](BLOCKS.md): every block type with its inputs, outputs and parameter offsets.
- [SOFTPORT.md](SOFTPORT.md): every Net command SoftPort sends, including the full config download and read
  sequences, the virtual keypad and device reset.
- [CONFIG_FILE.md](CONFIG_FILE.md): the Gen2Config / Supervisor config file format (`.G2C` / `.g4c`), including
  how block links are stored.

Confidence tags: **[RE]** read from decompiled iComm code, **[CAP]** seen in a capture, **[GUESS]** unconfirmed.

## Layers

```
Supervisor / Node-RED ──TCP :20000 (78 56 envelope, name-based)──► iComm ──serial or TCP──► RS485 Net ──► Genesis II DDCs
```

iComm's help says a connection can be a COM port, a USB converter, a modem, or a **TCP/IP-RS485 converter**,
so an ESP32 that is a transparent RS485↔TCP bridge would work with the existing iComm and Supervisor unchanged.
The native ESPHome firmware needs the **serial (Net) protocol** below; it doesn't need the TCP protocol at all.

## 1. Serial "Net" frame [RE]

```
FF FF FF FF  22 DD  hdr[9]  crcH(LE)  data[N]  crcD(LE)  FF FF
```

| hdr byte | meaning |
|---|---|
| 0 | sequence number (incremented per message; reply must echo it, except drv 0x0E) |
| 1 | destination net address (the DDC, e.g. 1–4) |
| 2 | **Drv** – driver/subsystem number |
| 3 | source address (`FF` = master/PC); the reply swaps 1 and 3 |
| 4 | 0 |
| 5 | flag (caller parameter) |
| 6 | **Cmd**; in a reply this is a status byte, 0 = OK, non-zero = "protocol error" |
| 7–8 | data length N, little-endian |

- CRC is **CRC-16/ARC** (poly 0xA001 reflected, init 0, no xor-out; check value `"123456789"` → `0xBB3D`),
  computed separately over the 9 header bytes and over the N data bytes, appended little-endian.
- Receivers hunt for the 2-byte sync; leading/trailing `FF`s are line padding.
- Firmware below 3.00 uses sync `33 CC` and an 8-byte header (byte 0 omitted). Our controllers report
  version 5.0, so they use `22 DD`.
- Timeout per message is a few seconds; iComm retries on timeout/CRC errors only.
- Codec + parser: `tools/genesis_frame.py`.

### Device families [RE]
`DEVICEINFO` (drv 0 / cmd 0xFF) returns the device type (u16 at reply[2]). Type **1 = Genesis II DDC**,
10 = Genesis II DDC V5, 2 = Genesis I, 7 = Genesis V, 0x12 = MicroMax, 0x15 = Maxim 1010.
- "DDC" types (1,2,3,4,7,9,10,11,0x18) use the **drv 2 / drv 9** commands below.
- Newer types (5,6,8,0xC–0xE,0x11,0x12,0x15–0x17) use drv 0 / cmd 0x8x equivalents.
- "Wide" types (7,9,10,11,0x18) send block sequence numbers as u16 LE, others as u8.
  Firmware 5.0 Genesis II DDCs tested so far report type 1 (u8 seq).

All multi-byte values little-endian. `seq` = block sequence number (1..N).

| Drv/Cmd | Name | Request | Reply |
|---|---|---|---|
| 0/0xFF | DEVICEINFO | – | [2..3] device type u16, [5] options |
| 9/1 | SYSTEM_READ (status) | – | ≥0xBC bytes: [0] name (C string), [0x31] config ID u32, [0xAE] bit0 = running, [0xBA] firmware u16 |
| 2/8 | block count | – | [0] N |
| 2/5 | read block record | seq | record: [0..0x13] name, [0x15] block type code, parameters (see below) |
| 2/6 | write block record | seq + whole record | status |
| 2/0x0A | find block by name | name (21 bytes, NUL padded) | [0] seq (0 = not found) |
| 2/7 | read one output | seq, out#+1 | 4-byte value |
| 2/9 | read many outputs (MVO) | n, then n × (seq, out#+1) | n, then n × 4-byte values (f32; FFFFFFFF = bad point) |
| 2/0x0B | write output directly (Net Input class) | `01`, u16 index (probably seq−1), out#+1, f32 | status |
| 1/2 | save to flash | – (12 s timeout) | status |
| 9/7 | read device table | – | 0x1F1 bytes: [0] enable, 62 × 8-byte entries ([8i+7] addr, [8i+8] kind) |
| 9/8 | write device table | 0x3F bytes | – |
| 9/9 | unknown, sent after device-table ops | – | discarded |
| 5/3 | comms test frame (SoftPort sends 0x80 bytes, u32 counter first) | | – |
| 0/0x84, 0/0x8B, 0/0x8C, 0/0x91, 0/0x86, 0/0x83, 0/0x8E | newer-family equivalents of status, read/write record, find-by-name, MVO, read block outputs, read config data, write system info | | |

Commands found in SoftPort (details and payloads in [SOFTPORT.md](SOFTPORT.md)):

| Drv/Cmd | Name | Notes |
|---|---|---|
| 2/1, 2/2 | pause / resume block processing | 2/2 with a 10 s timeout is the "remote restart" after a download |
| 2/3 | initialise blocks | sent near the end of a download |
| 2/4 | write block table (remote modules) | 60 bytes |
| 3/1 | clear log data | |
| 5/1 | read the display | 80 characters of LCD text, cursor and LED bytes |
| 5/2 | keypress | 1-byte key code; SoftPort's virtual keypad |
| 6/1 … 6/5 | watches and watch pages (clear, read, write) | |
| 7/1, 7/2 | read / write access codes | 16 × (4 ASCII digits + 1 byte) |
| 9/2 | write system info | 0xAE-byte record |
| 9/4, 9/5 | read / write controller password | |
| 9/6 | **reset device** | empty payload; the controller restarts within about 15 s |
| 0x0A/1, 2, 3 | logon, session info, logoff | logon payload 16 B (8 B password + 8 B user name) |
| 0x0C/1, 2, 3 | clear / read / write the config file ("auxiliary data") | 256-byte chunks, compressed; see CONFIG_FILE.md |

**Alive/Dead:** iComm sends DEVICEINFO then SYSTEM_READ. Alive = SYSTEM_READ answered with [0xAE] bit 0 set.
Any failure = Dead (that's what the iComm web "Dead" status means).

### Block record and fields [RE]
Record layout (2/5 reply, 2/6 payload after the seq byte), confirmed against the config file:

| Bytes | Field |
|---|---|
| `[0x00..0x14]` | name (21 B, NUL padded) |
| `[0x15]` | block type (see BLOCKS.md) |
| `[0x16..0x19]` | diagram x, y (u16 each): the block's position on the Gen2Config drawing |
| `[0x1A]` / `[0x1B]` | number of inputs / outputs |
| `[0x1C..0x1D]` | parameter area size |
| 6 B per input | **link**: u8 source block seq, u8 source output (1-based), f32 value. `00 00` = unconnected |
| 4 B per output | current output values |
| parameters | the block's settings, offsets in BLOCKS.md |

- Outputs (section 1, e.g. `OUTPUT`, a sensor's `Value`) are live values read with cmd 7/9.
- Parameters (section 2) live in the block record after the input links:
  `param_base = rec + 0x1E + 6*rec[0x1A] + 4*rec[0x1B]` (7 instead of 6 per link on Wide types), then + field offset.
- Writing a parameter = read record (2/5), patch bytes, write record back (2/6), optionally save to flash (1/2).
  A config download sends every block with this same 2/6 write, so links could be changed the same way.
- Field table in icomm.exe 1.30 at 0x61C2E4 (24-byte entries). Useful entries (Genesis II block types, full list in BLOCKS.md):

| Block type | Field | Offset / type |
|---|---|---|
| User Variable (setpoints) | Type / **Value** / MinVal / MaxVal / Pulse | 0 u8 / **1 f32** / 5 f32 / 9 f32 / 13 i32 |
| PID Loop | SetPoint, DeadBand, DPropBand… (outputs: Direct = cooling demand, Reverse = heating demand) | 0, 4, 8… f32 |
| Digital Output | DisableValue / FaultValue / Type / **Forced** / **ForceValue** | 0 / 1 / 3 / **4** / **5**, u8 |
| Analogue Output | **Forced** / **ForceValue** / X1 / X2 / Y1 / Y2 | **0** u8 / **1** f32 / 5 / 9 / 17 / 21 f32. ForceValue is in output units (Y1..Y2), so 0.4 = 40 % on a 0–1 output. Tested: forcing and releasing a chilled water valve's AO took effect within a poll |
| Weekly Schedule | On / Off | 0 / 2, u16, 32 entries, stride 4 |
| Yearly Schedule | On / Off ×32, State bitmask @128 u32, Year @132 u8 | |
| Analogue Input | CalM / CalC / LimitMin / LimitMax | 152 / 156 / 160 / 164 f32 |

Block type code in a record at [0x15] = iComm's Genesis II type id + 1 (User Variable 0x1E, Digital Output 0x04).

Weekly times are **minutes since Sunday 00:00** (1890 = Monday 07:30), confirmed from config files.
Except (holiday) schedules use 10-minute units and `day = (month-1)*32 + (day-1)` (see BLOCKS.md).

Value encodings: 1 f32, 2 u8, 3/7 u16, 4/9 i32, 5 bitmask in a byte, 6 string, 8 bit in u32, 10 f32×0.01.

### Confirmed on the wire [CAP] (serial port monitor on the iComm server's COM1)
All frames CRC-valid with the framing above. Decoder: `tools/spm_decode.py <export.txt|csv>`.
- **Reply header** differs from the request: `[0] seq, [1] FF (to master), [2] 00, [3] src addr, [4] drv echoed, [5] 0, [6] status (0 = OK), [7..8] len`.
- **DEVICEINFO 0/0xFF** → `21 00 01 00 07 0a 00 05 00 01 01 15 "<controller name>"`: device type `01 00` = **Genesis II DDC → block seq is one byte**.
- **SYSTEM_READ 9/1** → 190 bytes: name @0, config ID @0x31 (`99 46 8c 2e`), ASCII serial-ish `"13054004009 1"` @0x34,
  controller clock as Unix seconds @0xAA (`0x6806ABAF` = 2025-04-21 20:33 local), status @0xAE = `03` (bit 0 = alive), firmware @0xBA = `00 05` (5.00).
- **drv 0x0A cmd 2** → 15 bytes `3f 3f 3f 00… 56 ab 06 68 …` (contains another timestamp; meaning unknown).
- **drv 0 cmd 7 [seq]** is sent by Supervisor for ~14 blocks after a reconnect; the DDC answers status `04` (not supported). Safe to ignore.
- **MVO 2/9 poll** (every ~1 s while Supervisor is open): `14` + 20 × (seq, out#+1); reply `14` + 20 × f32.
  Example poll list: 25.1 26.1 4.1 4.2 4.3 9.1 9.2 9.3 11.1 11.2 11.3 48.1 2.1 21.1 61.1 5.1 63.1 65.1 67.1 53.1
  (4.1 / 9.1 / 11.1 ≈ 24–25 °C zone temps, 5.1 = a zone setpoint, 21.1 = an after-hours timer).
- **Setpoint write** (a zone setpoint User Variable 22.5 → 22.6): `2/6` with `05` + 51-byte record, then `1/2` save to flash.
  No read-before-write; Supervisor reuses the record it already holds. Record layout:
  `[0..19] name, [20] 00, [21] type 0x1E (User Variable), [22..25] ?, [26] #links=0, [27] #4-byte items=1, [28..29] 11 00,
   [30..33] current output f32, [34] Type=01, [35..38] **Value f32**, [39..42] Min 15.0, [43..46] Max 30.0, [47..50] Pulse`.
  The next poll shows 5.1 = the new value.
- **After-hours timer** = block 21 "User Variable", Value 3.8 (hours), written the same way.

## 2. iComm TCP :20000 protocol [CAP]

```
78 56 | len u16 LE (bytes that follow) | type u16 LE | body
```
The old README's "command types" (`48 00`, `3f 00`, `0d 00`) were actually lengths.

| type | dir | body |
|---|---|---|
| 0x0005 | client→iComm | request: `reqId u32`, `"<conn>:<dev>"` (e.g. `1:3`), padding, `"<block>~<field>"` (e.g. `System~ConfigKey`, `@102~OUTPUT` = block seq 102, field OUTPUT) |
| 0x0001 | iComm→client | reply: `reqId u32`, `u16 kind`, value. e.g. ConfigKey → `7c 88 44 48`; error → `15 fc` (0xFC15) |
| 0x0003 | iComm→client | advise batch: header then 14-byte records `handle u32, 01 00, 02 00, value f64 LE` |
| 0x0010 | iComm→client | keepalive, ~10 s |

Blocks are addressed by name or by `@<sequence number>`; fields by name (`OUTPUT`, …).
Parser: `tools/icomm_tcp.py`. Client: `tools/icomm_client.py`.

More message types, from Supervisor and from the ESPHome client:

| type | dir | body |
|---|---|---|
| 0x0011 | client→iComm | hello: u16 version 10, char[32] application name, char[32] host. **Required**, or iComm 2.38 ignores advises. iComm shows the 64 bytes as the name in Current Applications |
| 0x0002 | client→iComm | advise (subscribe): i16 count + 64-byte records (u32 handle chosen by the client, char[16] `conn:dev`, char[32] `@seq~Field`, u32 min interval, f64 deadband). Replied to with type 1; updates then arrive as type 3 |
| 0x0003 | iComm→client | update: u32 0, u16 count, records u32 handle, u16 status, u8 variant type, value (2 = f64, 3 = u16, …). Status 1 OK, 0xF43B bad field, 0xF444 device dead, 0xF440 no value yet |
| 0x0004 | client→iComm | unadvise: count + handles |
| 0x0006 | client→iComm | write: char[16] `conn:dev`, char[32] `@seq~Field`, u8 variant type, value. Send a double (2) even for u16 fields; a u16 variant gets no reply |

Field names: analogue input `Value`, digital input `State`, outputs and User Variables `OUTPUT` (`Value` also works on a
User Variable), PID `Direct` / `Reverse`, Digital Output `Forced` / `ForceValue`.
**Gotcha:** an iComm field write rewrites the whole block record from iComm's own cached copy, so other parameters can
silently change (a PID's unused SetPoint went 22 → 23 when only DeadBand was written). The raw passthrough below avoids it.

## 3. Controller config file

Each controller stores the Gen2Config source file it was programmed with (drv 0x0C "auxiliary data").
Supervisor reads it when it connects and caches it in `C:\Program Files\Innotech\cfgcache\<n>.g4c` (the file
name is Supervisor's own number, not the ConfigKey). It holds the site name, the access codes, F-key labels,
watch pages and every block with its position and wiring. Format and decoder: [CONFIG_FILE.md](CONFIG_FILE.md),
`tools/g4c_decode.py`.

The **ConfigKey** (SYSTEM_READ `[0x31]`, iComm `System~ConfigKey`) is the unix time of the last config download.

## Open questions
- drv 2/cmd 0x0B index meaning; drv 9/cmd 9; SYSTEM_READ fields beyond 0x31/0xAE/0xBA.
- Whether the controller's local DB9 RS232 port uses the same framing as the Net (very likely: iComm can use it as a serial connection).

## 4. Raw Net passthrough + log extraction (Gen2Xtract capture) [CAP]

Captured while `gen2xtra.exe /AUTO /D4 /ALL /CSV /SD<today> /ED<yesterday>` extracted a day of logs; the CSVs it
wrote validate the decode. Tools: `tools/gen2xtra_decode.py`, `tools/gen2xtra_raw.py`, `tools/g2log_decode.py`.

**iComm TCP message types seen (in addition to section 2):**

| type | dir | meaning |
|---|---|---|
| 0x0016 / 0x0017 | C>S / S>C | session/key query; reply `0a 00 02 00 00 00 41 00` + 16 bytes (random-looking) |
| 0x0009 / 0x0012 | C>S / S>C | connection info, sent before every raw request: reply carries "BMS Network", "Serial", "COM1@9600" |
| **0x0007** | C>S | **raw Net request passed straight to the bus.** Body = u32 invoke + a 32-byte C struct (padding is uninitialised garbage) whose last 7 bytes are `flag u8 (03/05), cmd u16, drv u16, len u16`, then max(len,1) payload bytes |
| **0x0008** | S>C | raw reply: u32 invoke, u16 result (1 OK), u16 data length, controller reply data |

So **anything the serial protocol can do works over iComm TCP too** (record reads 2/5, writes 2/6, scans), without
iComm's whole-record rewrite from its cache (see the dead-band SetPoint side effect).

**Raw commands Gen2Xtra used:**

| drv/cmd | payload | reply |
|---|---|---|
| 0x00/0xFF | – | DEVICEINFO (33 B: name, then a second string) |
| 0x0A/0x02 | – | 15 B `"???" … u32 time, session` (logon status) |
| 0x0A/0x01 | 16 B: 8 × 00 + username ("???" from Innotech.Ini) + 00… | 1 B session/handle (6, 7, 8, 9: increments) = **user logon** |
| 0x0A/0x03 | – | empty (logoff) |
| 0x09/0x01 | – | SYSTEM_READ 190 B |
| 0x0C/0x02 | `00` (chunk index?) | 256 B of the controller config file (same format as the cfgcache .g4c) |
| 0x02/0x08 | – | block count (u8) |
| 0x02/0x05 | seq | block record (here the Log and Alarm blocks) |
| **0x03/0x05** | u32 start time, u32 end time (unix), u16 n, n × channel bytes | empty = **set up a log extraction** |
| **0x03/0x04** | u32 record pointer (FFFFFFFF = newest), u32 time | **read/scan log**: u32 next pointer, u32 time, u16 records scanned, u16 records returned, then the returned records |

Log memory: 7 bytes per stored record (pointer steps 1400 per 200 scanned). Phase 1 scans back to the start time
(returned = 0), phase 2 returns the matching records, ~86 per call. Returned records are newest-first when each
reply is walked **from its end**: payload then a type byte (high nibble kind, low nibble payload length):
`0x35` = channel u8 + f32 sample, `0x74` = u32 unix time marker, `0x55` = u32 unix time + u8 (flag 01).
A sample is stored every log interval (30 s channels gave 1439 samples over 12 h, 900 s channels 100, matching the
Nrecs in Gen2Xtract's CSVs). The channel number is the Log block's channel.

**Raw request struct (confirmed):** `[0..3] FFFFFFFF, [4..5] connection id, [6..7] session (0), [8..9] device address as
ASCII text ("3"), [10..13] u32 1, [25] flag 03, [26] cmd u16, [28] drv u16, [30] len u16`, then the payload (min 1 byte).
No logon needed for reads or record writes. Tool: `tools/icomm_raw.py DEV DRV CMD [payload]`.
**Do not send save-to-flash (drv 1 cmd 2) raw through iComm:** no reply ever comes, the controller goes quiet while
saving, iComm marks it Dead and only re-probes about 3 minutes later. Timing via the server:
block read 0.25-0.9 s (up to 4.6 s while busy), record write 0.5-2.7 s.

## 5. iComm 4.4 and the TCP bridge [CAP]

iComm 4.4 (installed with Focus 4.5) runs as a Windows service with a web UI on port 8080 (the "server hosting
port" chosen at install). Supervisor still talks to it on TCP 20000 through its "iComm 1.xx connection over TCP"
listener, so nothing else can use 20000 on that PC.

Its **Innotech TCP** connection type is a TCP client to a raw serial stream (`IP Address`, `Port`, device address
range). Pointed at the ESPHome bridge port (`tcp_port: 10001`), iComm 4.4 found both test controllers online as
"Genesis II DDC 5.0" and read their block tables through the ESP, including when the ESP itself was reaching the
bus through another iComm server (iComm mode). Timeouts and retries are per connection (defaults 5 s, 3 retries).
