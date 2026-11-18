"""Decode iComm type-7 (raw Net request) / type-8 (raw reply) pairs from a capture.

python gen2xtra_raw.py capture.pcapng CLIENT_PORT [--all]

Type 7 body after the u32 invoke id is a C struct copy (padding bytes are uninitialised garbage):
fixed 32 bytes, the last 7 of which are  flag u8 | cmd u16 | drv u16 | len u16,  then max(len, 1) payload bytes.
Type 8 reply body: u32 invoke | u16 result | u16 data length | data (the controller's reply data).
"""
import collections
import struct
import sys

from icomm_tcp import streams

pcap, port = sys.argv[1], int(sys.argv[2])
show_all = "--all" in sys.argv
segs = streams(pcap)
msgs = []
for (sp, dp), chunks in segs.items():
    if port not in (sp, dp):
        continue
    tag = "C>S" if sp == port else "S>C"
    buf = bytearray()
    for t, data in chunks:
        buf += data
        while len(buf) >= 4 and buf[:2] == b"\x78\x56":
            n = struct.unpack_from("<H", buf, 2)[0]
            if len(buf) < 4 + n:
                break
            msgs.append((t, tag, bytes(buf[:4 + n])))
            del buf[:4 + n]
msgs.sort(key=lambda x: x[0])

reqs = {}
summary = collections.OrderedDict()
for t, tag, m in msgs:
    typ = struct.unpack_from("<H", m, 4)[0]
    body = m[6:]
    inv = struct.unpack_from("<I", body, 0)[0]
    if tag == "C>S" and typ == 7:
        s = body[4:]
        flag = s[25]
        cmd, drv, ln = struct.unpack_from("<HHH", s, 26)
        payload = s[32:32 + ln]
        head = s[:25]
        reqs[inv] = (t, drv, cmd, payload, head, flag)
    elif tag == "S>C" and typ == 8 and inv in reqs:
        t0, drv, cmd, payload, head, flag = reqs.pop(inv)
        result, dlen = struct.unpack_from("<HH", body, 4)
        data = body[8:8 + dlen]
        key = (drv, cmd)
        summary.setdefault(key, []).append((t0, payload, result, data, head, flag))
        if show_all:
            print(f"{t0:8.3f} drv={drv:#04x} cmd={cmd:#04x} flag={flag:#04x} pl={payload.hex(' ')} -> "
                  f"res={result} {dlen}B {data[:48].hex(' ')}")

for (drv, cmd), items in summary.items():
    print(f"\n== drv {drv:#04x} cmd {cmd:#04x}: {len(items)} requests")
    for t0, payload, result, data, head, flag in items[:4]:
        txt = "".join(chr(c) if 32 <= c < 127 else "." for c in data[:60])
        print(f"  {t0:8.3f} flag={flag:#04x} head={head[:12].hex(' ')} pl=[{payload.hex(' ')}] -> res={result} "
              f"{len(data)}B {data[:40].hex(' ')} |{txt}")
    if len(items) > 4:
        pls = [i[1].hex(' ') for i in items]
        print(f"  ... payloads: first {pls[4:8]} last {pls[-3:]}")
