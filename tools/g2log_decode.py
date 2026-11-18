"""Decode Genesis II log data from a capture of Gen2Xtract talking to iComm (TCP 20000).

python g2log_decode.py capture.pcapng TCP_STREAM_INDEX      (find the index with: tshark -r cap -q -z conv,tcp)

Log read = raw Net drv 0x03 cmd 0x04 (iComm message type 7), payload [u32 record pointer][u32 time].
Reply data: u32 next pointer | u32 time | u16 records scanned | u16 records returned | returned records.
Each reply decodes on its own, walking from its end (newest first). A record is its payload followed by a
type byte (high nibble = kind, low nibble = payload length):
  0x35  channel u8 + f32 value   (a logged sample; samples are stored on change)
  0x74  u32 unix time             (time marker)
  0x55  u32 unix time + u8        (time marker + flag byte)
"""
import collections
import datetime
import struct
import subprocess
import sys

TSHARK = r"C:\Program Files\Wireshark\tshark.exe"


def follow(pcap, stream):
    """Reassembled bytes of one TCP stream: (client->server, server->client)."""
    out = subprocess.run([TSHARK, "-r", pcap, "-q", "-z", f"follow,tcp,raw,{stream}"],
                         capture_output=True, text=True, check=True).stdout
    c2s, s2c = bytearray(), bytearray()
    for line in out.splitlines():
        hexpart = line.strip()
        if not hexpart or any(ch not in "0123456789abcdef" for ch in hexpart):
            continue
        (s2c if line.startswith("\t") else c2s).extend(bytes.fromhex(hexpart))
    return bytes(c2s), bytes(s2c)


def msgs(b):
    i, out = 0, []
    while i + 4 <= len(b):
        n = struct.unpack_from("<H", b, i + 2)[0]
        out.append(b[i:i + 4 + n])
        i += 4 + n
    return out


def log_replies(c2s, s2c):
    """Yield the data of every reply to a drv 3 / cmd 4 log read, in request order (newest first)."""
    replies = {}
    for m in msgs(s2c):
        if struct.unpack_from("<H", m, 4)[0] == 8:
            replies[struct.unpack_from("<I", m, 6)[0]] = m[14:]
    for m in msgs(c2s):
        if struct.unpack_from("<H", m, 4)[0] != 7:
            continue
        inv = struct.unpack_from("<I", m, 6)[0]
        cmd, drv, _ln = struct.unpack_from("<HHH", m, 10 + 26)
        if (drv, cmd) == (3, 4) and inv in replies:
            yield replies[inv]


def walk(body):
    """Yield records from the end (newest) of one reply's record bytes to its start."""
    i = len(body)
    while i > 0:
        t = body[i - 1]
        n = t & 0x0F
        start = i - 1 - n
        if start < 0:
            return
        p = body[start:i - 1]
        if t == 0x35:
            yield ("value", p[0], struct.unpack_from("<f", p, 1)[0])
        elif t in (0x74, 0x55):
            yield ("time", struct.unpack_from("<I", p, 0)[0], t, p[4:].hex())
        else:
            yield ("other", t, p.hex())
        i = start


if __name__ == "__main__":
    c2s, s2c = follow(sys.argv[1], int(sys.argv[2]))
    per = collections.defaultdict(list)
    kinds = collections.Counter()
    times = []
    for d in log_replies(c2s, s2c):
        if len(d) < 12:
            continue
        _nxt, _tm, _scanned, returned = struct.unpack_from("<IIHH", d, 0)
        if returned == 0:
            continue  # phase 1: scanning back to the start time
        for r in walk(d[12:]):
            kinds[r[0]] += 1
            if r[0] == "value":
                per[r[1]].append(r[2])
            elif r[0] == "time":
                times.append(r[1])
    print("records:", dict(kinds))
    if times:
        f = lambda u: datetime.datetime.fromtimestamp(u).strftime("%d/%m %H:%M:%S")
        print("time markers:", len(times), "from", f(min(times)), "to", f(max(times)))
    for ch in sorted(per):
        v = per[ch]
        print(f"channel {ch}: {len(v)} samples, newest {[round(x, 2) for x in v[:5]]}")
