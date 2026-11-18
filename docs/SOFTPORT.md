# SoftPort (GEN2VPRT.exe v5.40D): every Net command it sends

Static analysis only (Ghidra via `rea` + `objdump -M intel`). Binary sha256 `9bd9bba6…de4a4`.
Addresses are VAs in GEN2VPRT.exe. Tags: **[RE]** read from the code, **[GUESS]** inferred.

## 1. How SoftPort sends

SoftPort never touches the serial port. Every command goes to **iComm as TCP message type 7**
(the "raw Net request" already seen in captures, PROTOCOL.md §4). iComm builds the `22 DD` serial frame.

| Function | Role |
|---|---|
| `FUN_0045c164(ctx, dev, drv, cmd, buf, len, reply**, hwnd)` | **send + wait for reply**. On an iComm "lost connection" result (-2001..-2999) it reconnects (`0045ab37`) and retries once. 73 call sites. |
| `FUN_0045c278(ctx, dev, drv, cmd, buf, len, hwnd)` | **send only**: builds and writes the type-7 message (called by 45c164, and directly at 5 sites) |
| `FUN_0045c4b8(ctx, reply**, hwnd)` | wait for the type-8 reply (called by 45c164, `0042d712`, and directly in `0044ffc4`) |
| `FUN_0042d6c5` / `0042d6ed` / `0042d712` | thin wrappers for 45c164 / 45c278 / 45c4b8 (only 42d6c5 is used: `0044de43`, 0/0x86) |
| `FUN_0045a7d9` | writes any iComm message. Its other callers send iComm session messages (types 0x06, 0x08, 0x09, 0x0C, 0x0D, 0x16, 0x18, 0x22, 0x48: introduce/authenticate client, connection list, device list, "Forcing device info check"…), **not Net commands** |

`ctx` = `*(0x00501eb4)` (iComm client context). `dev` = SoftPort device struct:
`+0xC9` u16 firmware version (0x0500 = 5.00), `+0xD9` u16 device type, `+0xDC` u8 session handle,
`+0xE1` u16 net address, `+0x0F` cached 0xAE-byte system-info record.

**Argument order verified:** arg3 is **drv** and arg4 is **cmd**. At `0x4248db` the code pushes `6` then `2`,
so arg3 = 2 and arg4 = 6, which is 2/6 (write record). The same rule gives 1/2 next to "Updating DDC Flash RAM",
0x0A/1 with a 16-byte payload (logon), and 0x0C/2 next to "Reading version info" (config-file chunk). All of these match the known commands.

### Type-7 message built by `0045c278` [RE]
`u16 type = 7`, `u32 invoke` (`ctx+0xCD`, incremented), then this 32-byte struct, then `len` data bytes
(the message carries at least one data byte):

| off | size | source | meaning |
|---|---|---|---|
| 0x00 | u16 | `ctx+0xFD` | retry override (default -1; set to 1 for the DEVICEINFO probe, 0 for an address-changing sysinfo write) [GUESS: retries] |
| 0x02 | u16 | `ctx+0x101` | **timeout override** in seconds (default -1 = iComm default; 0 = don't wait) |
| 0x04 | u16 | `ctx+0xBD` | iComm **ConnectionId** (innotech.ini / connection chooser) |
| 0x06 | u16 | `dev+0xDC` | **session handle** returned by logon 0x0A/1 (0 = none) |
| 0x08 | char[16] | `sprintf("%d", dev+0xE1)` | device net address as ASCII |
| 0x18 | u16 | `dev+0xC9`, or 0x0300 if 0 | **device firmware version**. Its high byte is the "flag 03/05" seen in captures (3 = unknown/default, 5 = fw 5.00). iComm probably uses it to choose 33CC or 22DD framing [GUESS] |
| 0x1A | u16 | arg4 | **cmd** |
| 0x1C | u16 | arg3 | **drv** |
| 0x1E | u16 | arg6 | len |
| 0x20 | len | arg5 | payload |

## 2. Command table (every distinct drv/cmd)

"Wide" = device types 7/9/0x0A/0x0B, where seq/index fields are u16 (u8 otherwise). "Newer" = types 5,6,8,0x0C–0x0E,0x11,0x12,0x15–0x17.

### Legacy DDC family
| drv/cmd | Meaning | Call site(s) → function | Identifying strings | Payload sent | Timeout |
|---|---|---|---|---|---|
| 1/2 | save to Flash RAM | 42b7a4 → 42b72a (via 42b70f) | "Updating DDC Flash RAM ...", "Error saving data to Flash RAM" | – | **0x0F** |
| 1/0x0F | write config data (type 0x11 only) | 46649e → 46629e | "Writing data to the device ..." | u32 offset + data chunk | – |
| 1/0x10 | read config data (type 0x11 only) | 466777 → 4665f4 | "Reading data from the device ..." | u32 offset + u16 count | – |
| 2/1 | pause block processing | 423c9b → 423a40 | "Pausing block processing on Controller ...", "Error pausing block processing" | – | – |
| 2/2 | resume / restart block processing | 4240eb, 4241f6 → 423a40 | "Resuming normal Controller processing ...", "The DDC is now processing the new configuration" | – | **10** at 4241f6 (remote restart) |
| 2/3 | initialise blocks | 424057 → 423a40 | "Initialising blocks ...", "Error initialising blocks" | – | – |
| 2/4 | initialise block table / remote-module table | 42b99a → 42b8bd | "Initialising DDC block information...", "Error clearing block table", "Invalid device address in configuration" | 0x3C bytes = 15 × u32, entry `addr-1` = module bytes `[0x14]<<24 \| [0x15]<<16 \| [0x16]<<8 \| [0x17]` | **0x0F** (set by 423a40) |
| 2/5 | read block record | 426e55 → 426da6; 42791d → 427874 | "Block data len = %d", "Type %d, len=%d…" | seq (u8/u16 wide) | – |
| 2/6 | write block record | 4248e8 → 424320 | "Error transferring block", "Block type %d, fdl %d" | see §3.2 | – |
| 2/8 | block count | 427315 → 426fca | "Getting number of blocks ..." | – | – |
| 3/1 | clear log data | 42a0aa → 42a023 | "Clearing log data ...", "Error clearing log data" | – | **0x0F** |
| 5/1 | read viewport display | 40b175 → 40ae51 | "Error reading display" | – | – |
| 5/2 | **keypress** (send-only) | 40b0fb → 40ae51; 40b630 → 40b451 | "Error sending keypress", "Error ending session on the DDC" | 1 byte key code (§5) | **0** (no reply awaited) |
| 5/3 | comms test frame (send-only) | 42b64a → 42b557 | "Sending comms test frame %d ...", "There was an error attempting to send a test frame" | 0x80 bytes, u32 counter first [GUESS on content] | – |
| 6/1 | begin watch transfer (clear watches) | 424b32 → 424918 | "Sending %d watches to DDC ..." | – | – |
| 6/2 | read one watch | 4291f3, 429247 → 428de1 | "Reading %d watches ..." | index (u16 wide / u8) | – |
| 6/3 | write one watch | 424f02 → 424918 | – | index (u8/u16) + watch record (0x3D B fw≥3.20, else 0x28; −1 on wide) | – |
| 6/4 | read watch info | 428f4e → 428de1 | "Reading Watch Info ..." | 1 byte (=1) | – |
| 6/5 | write watch page | 425197 → 424918 | – | u8 page + page data | – |
| 7/1 | read access codes | 4295a5 → 429575 | "Reading Access Codes ..." | – | – |
| 7/2 | write access codes | 425275 → 4251de | "Sending Access Codes to DDC ...", "Error transferring access codes" | 0x50 = 16 × {4 ASCII digits, u8 level} | – |
| 9/1 | read system info | 426917, 42a278, 42a482, 42affe, 467ac1, 467ccb | "Cannot get system information", "Could not read back system info" | – | – |
| 9/2 | write system info | 42aed1, 42af62 → 42ae71 | "Error transferring new system info to controller", "Could not transfer new info", "System info check failed" | 0xAE-byte record (§3.1) | **0** with retry 0 if the address changes and fw < 6.00 |
| 9/4 | read password / 2nd system block | 41d8c4, 4221a9, 4269d7, 42d3ec | "Error retrieving system information", "Checking password on controller ...", "Cannot read MAXIM password" | – (reply[0..5] = password on newer types) | – |
| 9/5 | write controller password | 426ab8 → 4266f7 | "Sending new controller password ...", "Error changing controller password" | 8 bytes | – |
| 9/6 | **reset device** | 467094 → 466fab (via 466f90) | "Could not reset device", "Error resetting DDC", "The device will reset within 15 seconds" | len 0 | – |
| 9/0x0A | SSG: list available subsystem devices | 42a8f8 → 42a560 | "Retrieving information from SSG #%s about available subsystem devices..." | – | – |
| 0x0A/1 | logon | 42d4ba → 42d25d | "Unable to retrieve session information" | 16 B: 8 B password + 8 B username (`ctx+0x14`); reply[0] = session handle → `dev+0xDC` | – |
| 0x0A/2 | session info | 42a227, 42a434, 467a70, 467c7d | "Cannot get session information" | – | – |
| 0x0A/3 | logoff | 42d5dc → 42d505; 42d6aa → 42d67d | "Ending session with Controller ..." | – | – |
| 0x0C/1 | clear auxiliary data | 42538c → 4252a5 | "Clearing auxilliary data ..." | – | **10** |
| 0x0C/2 | read aux chunk (= compressed config file) | 4271bc → 426fca; 427e98, 428074 → 427cd1 | "Reading version info from DDC ...", "Reading data from DDC ...", "Decompressing data ..." | chunk index (u16 wide / u8); chunk = 256 B (0x108 on some types) | – |
| 0x0C/3 | write aux chunk | 426146 → 4252a5 | "Sending auxiliary data ...", "Auxilliary data OK ..." | u8 chunk index + ≤256 B | – |

### Newer family (drv 0, cmd ≥ 0x80) and DEVICEINFO
| drv/cmd | Meaning | Call site(s) → function | Strings | Payload | Timeout |
|---|---|---|---|---|---|
| 0/0x80 | pause block processing | 466908 → 4668c3 | "Pausing block processing on the device ..." | – | – |
| 0/0x81 | restart and resume block processing | 4502d3 → 44ffc4 | "Restarting and resuming block processing on Controller ..." | 4 B | – |
| 0/0x82 | write config data | 46655b → 46629e | "Writing data to the device ...", "Error sending transfer data" | u16 offset + data | – |
| 0/0x83 | read config data | 4667a1 → 4665f4 | "Reading data from the device ..." | u16 offset | – |
| 0/0x84 | read system info | 4268da, 42a3a2, 44942e, 4501bc, 466105, 466232, 467beb | "Error getting system info" | – | -1 restored at 4501bc |
| 0/0x85 | config transfer complete | 466ab5 → 466a74 | "Sending Transfer Completion message ..." | – | – |
| 0/0x86 | read block outputs | 44de43 (via 42d6c5) → 44dcb2 | "Error reading block outputs for updating Initial Values" | n + n × seq (u16 on types 6/0x15) | – |
| 0/0x8A | reset device | 46706a → 466fab | (as 9/6) | – | – |
| 0/0x8B | read block (MAXIM) | 42bf0b → 42beb3 | "Block data len = %d" | u16 seq | – |
| 0/0x8C | write block (MAXIM) | 42be83 → 42bb88 | "Transferring block details to MAXIM...", "Error sending block to MAXIM" | packed block | – |
| 0/0x8E | write system info | 45009a (send+wait), 450130 (send-only) → 44ffc4 | "Error writing system info", "Could not transfer new info" | 0x37 B | **0** on the send-only (address-change) path |
| 0/0x96 | write aux chunk | 466c95 → 466ae7 | "Writing data to the device ...", "Error opening auxiliary file" | u16 offset + data | – |
| 0/0x97 | read aux chunk | 466e7f → 466d96 | "Reading auxiliary data from the device ..." | u16 offset | – |
| 0/0x98 | aux transfer complete | 466d36 → 466ae7 | "Sending auxiliary transfer completion message ..." | – | – |
| 0/0x9F | start config transfer | 466a10 → 4668c3 | "Starting config transfer to the device ...", "Error starting config transfer to the device" | 4 B = `dev+0x40` (sysinfo[0x31], config ID) | – |
| 0/0xA6 | VAVMAX calibration | 418912 → 4188c3 | "Error calibrating VAVMAX" | – | – |
| 0/0xFF | DEVICEINFO probe | 42a1c1 → 42a102; 467a0a → 46794b | "AllowDeviceInfo" (innotech.ini [MISC]) | – | **2**, retry 1 |

`46794b` is a duplicate of the `42a102` probe. Its sites are listed in the tables.
Not used by SoftPort: 2/7, 2/9 (MVO), 2/0x0A, 2/0x0B, 3/4, 3/5, 9/7–9/9, 0/0x91.

## 3. Config download: `FUN_00423a40` [RE]

| # | Step | drv/cmd | Payload / notes |
|---|---|---|---|
| 1 | `42a102` probe | [0/0xFF, retry 1, timeout 2, only if `AllowDeviceInfo`] → 0x0A/2 → 9/1 (newer types: 0/0x84) | "Reading device System Info ..." |
| 2 | `42cf6f` | – | local checks only (primary device, capacity, block order, firmware feature limits) |
| 3 | compare cached sysinfo with config (name, two timestamps, byte 0x29, address) | – | if anything differs, the new sysinfo is prepared and the **needs-reset** flag is set. A DDC address of 0 aborts |
| 4 | `42d23b`→`42d25d` logon | 0x0A/1 (16 B) | newer types with fw ≥ 4.20 use 9/4 plus a local password compare instead |
| 5 | pause | **2/1** | "Pausing block processing on Controller ..." |
| 6 | `42d72b` | – (iComm message type 0x18, not a Net frame) | tells iComm about the device [GUESS: suspends polling / forces device check] |
| 7 | `42a102` re-read | 0x0A/2, 9/1 | "Error getting device info" |
| 8 | `426211` new system info | **9/2** (0xAE B) then 9/1 read-back + memcmp | "Transferring system info ...", "Error transferring new config info" |
| 9 | `42b8bd` | **2/4**, timeout **0x0F** | 60-byte remote-module table |
| 10 | loop `424320` per block | **2/6** | "Transferring %d block%s ..." |
| 11 | `4252a5` aux data | **0x0C/1** (timeout 10), then **0x0C/3** × N | compressed source config (`_AUXW` scratch file), ≤ 0xFFFF B (0x31800 on wide types) |
| 12 | `424918` watches | **6/1**, **6/3** × watches, **6/5** × pages | |
| 13 | `4251de` | **7/2** (80 B) | access codes |
| 14 | `42a023` | **3/1**, timeout **0x0F** | clear log data |
| 15 | init | **2/3** | "Initialising blocks ..." |
| 16 | `42b70f` | **1/2**, timeout **0x0F** | save to Flash RAM |
| 17 | fw < 3.00 only | **2/2** | "Resuming normal Controller processing ..." |
| 18 | `42d4f7` | **0x0A/3** | logoff |
| 19 | fw ≥ 3.01, setting 0x1B76 on, user confirms the warning | **2/2**, timeout **10** | **remote restart**, sent after logoff. Otherwise (fw ≥ 3.00): "You must reset the DDC…" |
| 20 | needs-reset flag set | **9/6** (0/0x8A on newer types) via `466f90` | else `42d72b` (iComm type 0x18) |

Any failure jumps to 4242ad: "Error transferring configuration to the Controller", then logoff.

### 3.1 9/2 system-info record (fw > 1.03) [RE, field names partly GUESS]
`[0x00..0x14]` name (21 B) · `[0x15]` DDC address · `[0x16..0x18]` I/O module bitmasks · `[0x19]` 1 (fw < 3.20) or a config byte ·
`[0x1A]` flag · `[0x1B..0x21]` timestamp A (u32 in a 7-byte slot) · `[0x22..0x25]` timestamp B (u32) · `[0x26]` byte · `[0x29]` byte ·
`[0x2A..0xA9]` 128 B block (`[0x2A..0x2D]` = 0, `[0x2E]` u16 0x0528, `[0x30]` byte, `[0x31..0x34]` config ID, `[0x35..]` site string) ·
`[0xAA..0xAD]` clock u32. Fw ≤ 1.03 uses 7-byte date structs (sec, min, hour, mday, mon+1, year, 0) instead of the u32s.
Read-back compare ignores `[0x1F]`, `[0x26]`, `[0xAA..0xAD]`, `[0xB0]`, `[0xB5..0xB7]`.

### 3.2 2/6 block record [RE]
`seq` (u8; u16 wide) · name 21 B · type code u8 (table at 0x509057) · 2 B (`blk+0x17`) · 2 B (`blk+0x1B`) · u8 #links · u8 #outputs ·
u16 parameter length · per link: source seq (u8/u16) + output# u8 (+1 pad on wide) + 4 B value (left 0xFF) · per output: 4 B zero ·
parameter bytes (fw-dependent fix-ups). The buffer is prefilled with 0xFF and is at most 0x800 bytes. This matches the PROTOCOL.md capture (`05` + 51 B).

## 4. Config read (upload from controller): `FUN_00426fca` [RE]
Logon 0x0A/1 → **0x0C/2** chunk 0 ("Reading version info from DDC ...", which checks for "No configuration file exists") → **2/8** →
`42a102` (0x0A/2, 9/1) → logoff → may copy from cache (`cfgcache`) → logon → `427874` **2/5** × N ("Reading %d blocks from DDC ...") →
`427cd1` **0x0C/2** × N ("Reading data from DDC ...", then decompress) → `428de1` **6/4**, **6/2** × N (watches) → `429575` **7/1** (access codes) →
logoff 0x0A/3. Failure: "Error reading configuration from the Controller" (0x42783a). Success: "Configuration read successfully".
The newer family reads with 0/0x84, then 0/0x83 (or 1/0x10 on type 0x11) and 0/0x97 (aux).

## 5. Virtual keypad / viewport ("Software Viewport Tool") [RE]
- **5/2** keypress, sent with `0045c278` (no reply awaited, timeout 0). Payload 1 byte = key code from the keypad hit-test `0040c5e6`:
  0x01–0x08 = right-hand column of 8 keys (top to bottom); 0x09/0x0A = two left keys (upper/lower);
  0x0B up, 0x0C down, 0x0D left, 0x0E right (arrow cluster); 0x10 / 0x0F = two keys right of the arrows (upper/lower).
  **0x10 is sent automatically to end the viewport session** (`40b475`) [GUESS: Esc/Exit].
- **5/1** read display (polled after each key). Reply parsed by `0040cead`: `[0..0x4F]` 80 chars LCD text, `[0x50]` cursor position,
  `[0x51]` bit0 cursor on, bit1 indicator, `[0x52..0x53]` u16 LED/status bits (ignored on newer types).

## 6. Device reset [RE]
`466f90`/`466fab`: **9/6**, empty payload, reply expected. Types 5,6,8,0x0C,0x0D,0x0E,0x11,0x12,0x15,0x16,0x17 use **0/0x8A** instead.
The UI path ("Network device reset", fw ≥ 3.11) logs on (0x0A/1), sends 9/6, then logs off (0x0A/3). The DDC resets within ~15 s.
The config-download "remote reset" is a different command: **2/2 with a 10 s timeout** (step 19).

## 7. Newer-family config download (`4660d8`, no direct caller, reached indirectly)
0/0x84 → `4668c3`: **0/0x80** pause, **0/0x9F** start (u32 config ID) → `46629e`: **0/0x82** × N (u16 offset + data; type 0x11: **1/0x0F**, u32 offset;
chunk 0x1FE on type 6 > 4 KiB) → `466a74`: **0/0x85** complete. Aux: **0/0x96** × N then **0/0x98**. System info: **0/0x8E** (55 B) → 0/0x84 read-back → **0/0x81** restart.

## Uncertain
- `+0x00` (`ctx+0xFD`) as a *retry count* is a guess; the timeout meaning of `+0x02` rests on the 0x0F/10/2/0 values and the "0 = send-only" use.
- The meaning of iComm message type 0x18 (`42d72b` / `45cbc8` / `45cee4`) was not decoded. It is an iComm session message, not a Net frame.
- Several 9/2 sysinfo field names (bitmasks, the bytes at 0x19/0x1A/0x26/0x29, the 128-byte block) are inferred from the copy sources only.
- 5/3 test-frame contents and the 0/0x8C, 0/0x8E, 0/0x81 payload layouts were not decoded beyond their sizes.
- The key labels (F1–F8, Esc…) are inferred from keypad geometry; the bitmap resource names were not checked.
