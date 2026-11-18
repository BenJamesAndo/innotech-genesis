# Genesis II config file format (.g4c / .G2C, version 0x509)

Supervisor 5.40 caches each controller's stored program as `C:\Program Files\Innotech\cfgcache\<n>.g4c`.
It is the controller's "auxiliary data" (drv 0x0C, read in 256 B chunks, decompressed), i.e. the Gen2Config
source file that was downloaded with the program. Decoder: `tools/g4c_decode.py FILE [--json out.json] [--raw]`.

These files contain the site name, the programmer's name and the panel **access codes**, so don't publish them.
The decoder never prints the access codes.

Sources: GEN2VPRT.exe (SoftPort 5.40D, which contains Gen2Config's file code), checked against two real files and live block reads. Reader chain `FUN_0043662e` →
`004362f0` (preamble) → `0043cf02` header → `0043f438` blocks → `00440a92` nodes → `00441c0f` labels →
`004426cd` lines → `004431d9` texts → `00443e8f` watch pages → `0042fb29` modules (ver > 0x314) →
`0043b226` (payload length check). Older files (ver < 0x30B, fixed 0x288-byte header) use a different
reader (`004368ee`…) and aren't covered.

Tags: **[RE]** read from the reader code, **[LIVE]** checked against live 2/5 / 9/1 reads, **[GUESS]** inferred.

All integers little-endian. Most sections and entries carry their own length, and the reader seeks past
any unread bytes, so later versions can append fields.

## Preamble (5 + 4 bytes) [RE]
| Off | Type | Value |
|---|---|---|
| 0x00 | u16 | magic: `0x28FC` (these files), also `0x72D9` / `0x772E` accepted |
| 0x02 | u16 | version `0x0509` (reader accepts up to 0x509) |
| 0x04 | u8 | flags, `0x48` here (old writer used `0x41`), meaning unknown |
| 0x05 | u32 | payload length from 0x05 to EOF |

## Header section (0x09) [RE]
`u16 section length` (0x401, counted from 0x09), then (offsets for these files; strings are fixed-size):

| Off | Field |
|---|---|
| 0x0B, 0x0D | u16 unknown (0) |
| 0x0F | u16 **block count** |
| 0x11 | u16 **node count** |
| 0x13 | u16 **label count** |
| 0x15 | u16 **line count** |
| 0x17 | u16 **text count** |
| 0x19 | u16 unknown (0) |
| 0x1B | site name: u16 size (0x32) + 50 B |
| 0x4F | author: u16 size (0x32) + 50 B |
| 0x83 | description: u16 size (0x64) + 100 B |
| 0xE9 | u32 unix time, config created [GUESS] |
| 0xED | u32 unix time the file was written (Supervisor cache time) [LIVE: matches when the cache was refreshed] |
| 0xF1 / 0xF3 | u16 0x0E, u16 0x38 (system-info version/size, set as constants by the writer) |
| 0xF5 | device name: u16 size (0x15) + 21 B (junk after the NUL is preserved) |
| 0x10C | u32 **ConfigKey** = SYSTEM_READ 9/1 `[0x31]` config ID = iComm `System~ConfigKey` [LIVE]. It's the unix time of the last config download |
| 0x110, 0x114 | u32 unknown (0) |
| 0x118–0x120 | u16, u16, u16, u8, u16: unknown |
| 0x121 | u8 DDC address [LIVE] |
| 0x122 | **access codes**: u16 count (16), u16 size (4), u16 stride (5), then count × stride (4 ASCII digits + 1 byte). Sensitive: the decoder skips them |
| 0x178 | watch page titles: u16 count (8), u16 size (32), entries (`\r\n` = line break on the display) |
| 0x27C | F-key labels: u16 count (12), u16 size (32), entries |
| 0x400 | u8, u8 unknown |
| 0x402 | u16 0x0528 (same as SYSTEM_READ `[0x2E]`) |
| 0x404, 0x406 | u16 unknown (ver ≥ 0x504) |
| 0x408 | u16 DDC address again (ver ≥ 0x507) |

The cfgcache filename number is **not** the ConfigKey. It's a Supervisor-side id.

## Blocks section [RE]
`u16 section length` (includes itself), `u16 terminal entry size` (8), then one node per block:

| Off | Type | Field |
|---|---|---|
| +0 | u16 | node length (includes itself) |
| +2 | u16 | block sequence number (= the 2/5 index) |
| +4 | u16 + 21 B | name |
| +0x1B | u16, u16 | diagram x, y |
| +0x1F | u16 | Gen2Config block type (table below) |
| +0x21 | u16 | n_in |
| +0x23 | u16 | n_out |
| +0x25 | u16 | parameter size |
| +0x27 | u16 | unknown (0) |
| +0x29 | u16 | display attribute: low byte comes from the type template (`2A`/`1C`), high byte varies. Unknown |
| +0x2B | u16 | terminal count (the reader rejects anything other than n_in + n_out) |
| +0x2D | 8 B × count | terminals: `u16 pin` (low = row on the symbol, high = side: 2 input/left, 1 output/right), `u16` (1 on inputs, 0 on outputs), `u16 terminal index` (inputs first, then outputs), `u16 watched` (1 = a watch record follows this terminal's node) |
| … | psize B | parameter area (see "vs the live record") |

### Block types (Gen2Config table, GEN2VPRT.exe @0x50909C, 0x5C per type) [RE]
The file type id equals the BlocksLibrary.xml DDC Id **except 22/23**. Gen2Config has 22 = Except Schedule
and 23 = Weekly Schedule. BlocksLibrary uses the controller numbering (22 WEEKLY, 23 YEARLY). The controller
record type byte is `LibraryId + 1` [LIVE: Weekly → 0x17, Except → 0x18; all others = type + 1].

0 Analogue Input · 1 Digital Input · 2 Analogue Output · 3 Digital Output · 4 PID Loop · 5 Comparator ·
6 Hysteresis · 7 Recipe · 8 Universal Curve · 9 Calculator · 10 Psychrometric · 11 Selector ·
12 Multiplexor · 13 DeMultiplexor · 14 Lookup · 15 Delay · 16 Counter · 17 Timer · 18 AND · 19 OR ·
20 NOT · 21 XOR · 22 Except Schedule · 23 Weekly Schedule · 24 Latch · 25 Alarm · 26 Log · 27 Analyse ·
28 Global Input · 29 User Variable · 30 Accumulator · 31 Switch · 32 Pulse Counter · 33 Global Output ·
34 Optimum Start · 35 Daily Schedule · 36 Random · 37 Encoder · 38 Decoder · 39 Logic · 40 Net Input.
Input and output terminal names per type are in `BLOCK_TYPES` in the decoder; they come from the same table.

## Nodes section (wiring points) [RE]
`u16 section length`, `u16 node size` (10), `u16 watch record size` (61), then `node count` entries. Ids are **1-based**:

| Field | Meaning |
|---|---|
| u16 kind | 0 = output terminal, 1 = input terminal, 2 = junction/bend, 3 = label pin |
| u16 x, u16 y | position on the diagram |
| u16 a | kind 0/1: block seq · kind 3: label id (1-based) · kind 2: 0 |
| u16 b | kind 0/1: terminal index (inputs first, so output k = n_in + k) |

If the terminal is flagged `watched`, a 61-byte **watch record** follows the node:
`[0]` u8 1 · `[1]` u16 block seq · `[3]` label[11] ("Temp", "Request") · `[14]` u8 1 = analogue, 0 = digital ·
`[15]` on-text[10] · `[25]` off-text[10] · `[35]` units[4] (`DgC` = °C, `%`, `hr`) · `[39]` u8 decimals ·
`[40]` description[21]. Fields after `[0..2]` are [GUESS] from content, but every sample fits.

## Labels section (named connectors) [RE]
`u16 section length`, `u16 entry size` (35); entries: `u32` (0), `u32` COLORREF, `u16 0x15 + name[21]`,
`u16 width`, `u16 height`.

## Lines section [RE]
`u16 section length`, `u16 entry size` (4); entries: `u16 node A`, `u16 node B` (1-based node ids).

## Texts section [RE]
`u16 section length`; per text: `u16 entry length`, `u16 n + text`, `u16 x`, `u16 y`, `u16 n + font name`,
`u16 size`, `u32 COLORREF`, `u16 flags` (bits 0..2 are three style booleans, probably bold/italic/underline [GUESS]).

## Watch pages section [RE]
`u16 section length`, `u16 entry size` (3), `u16 pages` (9), `u16 lines per page` (32); entries:
`u16 node id` (0 = empty line; it points at an output-terminal node and its watch record), `u8 flag`
(1 on User Variable lines, so probably "adjustable" [GUESS]). Pages 1–8 use the header titles. Page 9 has no
title (in both files it lists the zone temperatures) [GUESS: summary/default page].

## Modules section (ver > 0x314), partly decoded
`u16 version marker` (0xABD3), `u16 count`, then variable-length module records: `u16 n + description`
("Genesis II Direct Digital Controller (LEM)"), about 14 u16 fields, more version-gated u16 fields,
`u16 n + short name` ("GENESIS II CONTROLLER"), and so on. The decoder only lists the strings.

## How input links are encoded
**The file doesn't store links.** They are implied by the wiring, the same way Gen2Config derives
them before a download:
1. Build nets: union-find over node ids joined by every line.
2. Join every label pin (kind 3) that has the same label id. Named labels are off-page connectors.
3. In each net, the single **output-terminal** node (kind 0, block `a`, output `b − n_in`) drives every
   **input-terminal** node (kind 1, block `a`, input `b`). An input with no net, or a net without an output,
   is unconnected.

[LIVE] For all 38 blocks checked, every input matched the controller's link table, including links through
labels and the 8-input Selector. No net in either file has two outputs.

## File vs the live 2/5 block record
| Record | From the file |
|---|---|
| `[0x00..0x14]` name | block name (21 B) |
| `[0x15]` type | `LibraryId + 1` (Gen2 type + 1, but 22→0x18, 23→0x17) |
| `[0x16..0x19]` | x, y (u16 each). The diagram position is stored in the controller too |
| `[0x1A]` / `[0x1B]` | n_in / n_out |
| `[0x1C..0x1D]` | parameter size. Can be shorter than the file's (Timer 5 vs 10, Log 6 vs 7) |
| `[0x1E..]` 6 B × n_in | link: `u8 source seq`, `u8 source output (1-based)`, `f32` (constant/last value, not in the file). `00 00` = unconnected |
| 4 B × n_out | current output values (live only) |
| params | = file parameter area, except **Log**: record = file[1:] (the file's first byte is a per-block 0,1,2… index, probably a storage channel [GUESS]); **Timer**: record = file[:5] |

BlocksLibrary.xml offsets apply to the record's parameter layout. The decoder applies them to the file
area after the Log shift.

Schedules (parameter area = 32 × `u16 on, u16 off`):
- Weekly: minutes since Sunday 00:00 (1800 = Mon 06:00). Every period seen fits.
- Except Schedule: 10-minute units, 144 per day, `day = (month−1)·32 + (day−1)`. `State` u32 @128,
  `Year` u8 @132 (+2000). It matches a real site's public holidays for the year exactly.
- Daily: assumed to be minutes since midnight [GUESS, none in these files].

## Cross-check
Checked against two controllers (104 and 131 blocks, counts match 2/8) with read-only 2/5 and 9/1 reads:
38 blocks across 20 types (DI, AI, AO, DO, UV, PID, Selector, Weekly, Except, Latch, Timer, Delay, Log,
Alarm, AND/OR/NOT, Global In/Out, Hysteresis, Universal Curve). Name, type, position, terminal counts and
**links matched 38/38**. Parameters matched except where someone had changed them on the controller after
the download (a weekly schedule edited from the keypad, a reassigned output terminal). So the file is the
program as downloaded, not the live values.

## Still unknown
Preamble flag byte, a few header words (0x0B, 0x0D, 0x19, 0x110–0x120, 0x400–0x407), block +0x27 and the
display attribute, the meaning of the label `u32` (always 0), the modules section layout, the Daily
schedule time unit, and the Analogue Input parameter bytes 42–151. They look like a sensor
conversion table of floats and aren't described in BlocksLibrary. The decoder keeps them in `params_raw`.
