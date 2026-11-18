# Genesis II block types

Every Genesis II block type, with its input terminals, outputs and parameter layout. Put together from
Gen2Config's type table (inside SoftPort) and the point definitions iComm 4.4 uses, and checked against
live block records. Used by `tools/g4c_decode.py` (`tools/_block_params.py` holds the same parameter table).

- **Type** is Gen2Config's block type id (the `.g4c`/`.G2C` file uses it). **Record** is the type byte at
  `[0x15]` of a 2/5 block record. It's the controller type id + 1, where the controller numbering swaps
  22 and 23 compared to Gen2Config (Gen2Config 22 = Except Schedule, 23 = Weekly; record bytes 0x18 / 0x17).
- Parameter offsets are from the start of the record's parameter area:
  `param_base = 0x1E + 6 * n_inputs + 4 * n_outputs` (see PROTOCOL.md).
- `f32` = little-endian float, `u8`/`i16`/`i32` integers, `bytes` = a table (schedule periods, curve points).
- Values are in the block's own units. A forced output's `ForceValue` is in **output units**: an Analogue
  Output scaled Y1 = 0, Y2 = 1 needs `0.4` for 40 %, not `40`.

| Type | Record | Block | Inputs | Outputs | Parameters |
|---|---|---|---|---|---|
| 0 | 0x01 | Analogue Input | – | VALUE, REQUEST, FAIL | SensorName @0 string, Conversion @41 u8, CalM @152 f32, CalC @156 f32, LimitMin @160 f32, LimitMax @164 f32, Alpha @168 f32 |
| 1 | 0x02 | Digital Input | – | STATE | OnValue @0 u8 |
| 2 | 0x03 | Analogue Output | INPUT, ENABLE, FAULT | OUTPUT | Forced @0 u8, ForceValue @1 f32, X1 @5 f32, X2 @9 f32, Y1 @17 f32, Y2 @21 f32, DisableValue @25 f32, FaultValue @29 f32, Type @33 u8 |
| 3 | 0x04 | Digital Output | INPUT, ENABLE, FAULT | OUTPUT | DisableValue @0 u8, FaultValue @1 u8, TerminalType @2 u8, Type @3 u8, Forced @4 u8, ForceValue @5 u8 |
| 4 | 0x05 | PID Loop | INPUT, SET POINT, ENABLE | DIRECT, REVERSE | SetPoint @0 f32, DeadBand @4 f32, DPropBand @8 f32, DIConst @12 f32, DDConst @16 f32, RPropBand @20 f32, RIConst @24 f32, RDConst @28 f32, Alpha @32 f32 |
| 5 | 0x06 | Comparator | INPUT A, INPUT B, EXT TOL | A > B, A < B, A = B | Tolerance @0 f32, Diff @4 f32 |
| 6 | 0x07 | Hysteresis | INPUT, EXT OFF, EXT ON | OUTPUT | OffValue @0 f32, OnValue @4 f32 |
| 7 | 0x08 | Recipe | ENABLE, RESTART, STEP, HOLDING | VALUE, STAGE, PROGRESS, DONE | NumPoints @80 u8, Point @0 bytes, Interval @40 bytes |
| 8 | 0x09 | Universal Curve | INPUT | OUTPUT | NumPoints @80 u8, FunctX @0 bytes, FunctY @40 bytes |
| 9 | 0x0A | Calculator | INPUT A, INPUT B | OUTPUT | Type @0 u8 |
| 10 | 0x0B | Psychrometric | DRY BULB, INPUT B | OUTPUT | FuncType @0 u8, BaromPress @1 f32 |
| 11 | 0x0C | Selector | INPUT, INPUT, INPUT, INPUT, INPUT, INPUT, INPUT, INPUT | MAXIMUM, MINIMUM, AVERAGE | – |
| 12 | 0x0D | Multiplexor | INPUT 0, INPUT 1, INPUT 2, INPUT 3, INPUT 4, INPUT 5, INPUT 6, INPUT 7, CONTROL | OUTPUT | – |
| 13 | 0x0E | DeMultiplexor | INPUT, CONTROL | OUTPUT 0, OUTPUT 1, OUTPUT 2, OUTPUT 3, OUTPUT 4, OUTPUT 5, OUTPUT 6, OUTPUT 7 | – |
| 14 | 0x0F | Lookup | ENTRY | VALUE | Values @0 bytes |
| 15 | 0x10 | Delay | INPUT, ONDELAY, OFFDELAY | OUTPUT | DelayToOn @0 f32, DelayToOnType @4 u8, DelayToOff @5 f32, DelayToOffType @9 u8 |
| 16 | 0x11 | Counter | CNT LIM, UP PULSE, DN PULSE, RESET | STARTED, REACHED, COUNT | CountLimit @0 i32, StoragePos @4 u8, InitialValue @5 i32 |
| 17 | 0x12 | Timer | EXT LIM, ENABLE, RESET | RUNNING, DONE, TIME | TimerValue @0 f32, TimeType @4 u8, StoragePos @5 u8, InitialValue @6 i32 |
| 18 | 0x13 | AND | INPUT, INPUT | OUTPUT | – |
| 19 | 0x14 | OR | INPUT, INPUT | OUTPUT | – |
| 20 | 0x15 | NOT | INPUT | OUTPUT | – |
| 21 | 0x16 | XOR | INPUT 1, INPUT 2 | OUTPUT | – |
| 22 | 0x18 | Except Schedule | VALUE, OVERRIDE | OUTPUT, ACTIVE | On @0 bytes, Off @2 bytes, State @128 i32, Year @132 u8 |
| 23 | 0x17 | Weekly Schedule | VALUE, OVERRIDE | OUTPUT | On @0 bytes, Off @2 bytes |
| 24 | 0x19 | Latch | EXT LIM, INPUT, RESET | OUTPUT | Mode @0 u8, TimerValue @1 f32, TimerType @5 u8 |
| 25 | 0x1A | Alarm | TRIGGER, LISTEN, RESET | OUTPUT | Message @0 string, Latch @21 u8, OutputPrinter @22 bit, OutputDisplay @22 bit, OutputModem @22 bit, OutputSpeaker @22 bit, OutputPager @22 bit, OutputLog @22 bit, DelayToOn @26 i32, DelayUnit @30 u8 |
| 26 | 0x1B | Log | INPUT, ENABLE, TRIGGER | – | LogType @0 bit, Interval @1 f32, TimeType @5 u8 |
| 27 | 0x1C | Analyse | INPUT, ENABLE, RESET, EXT INT | MAXIMUM, MINIMUM, AVERAGE | Interval @0 f32, TimeType @4 u8, Method @5 u8 |
| 28 | 0x1D | Global Input | – | VALUE, FAULT | FailType @0 u8, FailValue @1 f32 |
| 29 | 0x1E | User Variable | – | OUTPUT | Type @0 u8, Value @1 f32, MinVal @5 f32, MaxVal @9 f32, Pulse @13 i32 |
| 30 | 0x1F | Accumulator | EN/VALUE, RESET | ACCUM VALUE | TimeType @0 u8, Channel @1 u8, InitialVal @2 f32 |
| 31 | 0x20 | Switch | INPUT A, INPUT B, CONTROL | OUTPUT | – |
| 32 | 0x21 | Pulse Counter | – | PULSES | Alpha @0 f32 |
| 33 | 0x22 | Global Output | INPUT | – | – |
| 34 | 0x23 | Optimum Start | SCHEDULED TIME, INPUT, SETPOINT | START TIME | SetPoint @0 f32, Tolerance @4 f32, CoolingConst @8 f32, HeatingConst @12 f32, MaxEarlyStart @16 i16, Alpha @18 f32 |
| 35 | 0x24 | Daily Schedule | VALUE, OVERRIDE | OUTPUT | On @0 bytes, Off @2 bytes |
| 36 | 0x25 | Random | TRIGGER | OUTPUT | Minimum @0 i32, Maximum @4 i32 |
| 37 | 0x26 | Encoder | INPUT 1, INPUT 2, INPUT 3, INPUT 4 | OUTPUT | – |
| 38 | 0x27 | Decoder | INPUT | OUTPUT 1, OUTPUT 2, OUTPUT 3, OUTPUT 4 | – |
| 39 | 0x28 | Logic | INPUT A, INPUT B, INPUT C, INPUT D | OUTPUT | Type @0 u8, Inverted @1 u8 |
| 40 | 0x29 | Net Input | – | OUTPUT, FAULT | StartupValue @0 f32 |

Schedules store 32 periods of `u16 on, u16 off`:
- **Weekly:** minutes since Sunday 00:00 (1800 = Monday 06:00).
- **Except (holiday) schedule:** 10-minute units, 144 per day, `day = (month - 1) * 32 + (day - 1)`;
  `State` u32 @128 and `Year` u8 @132 (+2000).
- **Daily:** presumably minutes since midnight (not seen yet).

Analogue Input bytes 42-151 hold the sensor conversion table (floats), not described here.
