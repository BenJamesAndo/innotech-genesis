"""Dump one TCP connection of an iComm :20000 capture as iComm messages (both directions, in time order).

python gen2xtra_decode.py capture.pcapng CLIENT_PORT [max_messages]
Prints: time, direction, type, length, and the body as hex + printable text, so new message types
(e.g. Gen2Xtra's log reads) can be studied.
"""
import struct
import sys

from icomm_tcp import streams

pcap, port = sys.argv[1], int(sys.argv[2])
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 10**9
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
            m = bytes(buf[:4 + n])
            del buf[:4 + n]
            msgs.append((t, tag, m))
        if buf and buf[:2] != b"\x78\x56":
            msgs.append((t, tag + " RAW", bytes(buf)))
            buf.clear()
msgs.sort(key=lambda x: x[0])
for t, tag, m in msgs[:limit]:
    typ = struct.unpack_from("<H", m, 4)[0] if len(m) >= 6 else -1
    body = m[6:]
    txt = "".join(chr(c) if 32 <= c < 127 else "." for c in body[:96])
    print(f"{t:9.3f} {tag} type={typ:#06x} len={len(m):5d} {body[:64].hex(' ')} | {txt}")
print(f"# {len(msgs)} messages")
