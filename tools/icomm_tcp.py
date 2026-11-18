"""Parse iComm TCP :20000 traffic (Supervisor <-> iComm) from a pcap via tshark.

Message envelope (observed): 78 56 | len u16 LE (bytes after this field) | type u16 LE | body
Usage: python icomm_tcp.py capture.pcapng [--all]
"""
import collections
import struct
import subprocess
import sys

TSHARK = r"C:\Program Files\Wireshark\tshark.exe"


def streams(pcap):
    out = subprocess.run([TSHARK, "-r", pcap, "-Y", "tcp.len>0", "-T", "fields",
                          "-e", "frame.time_relative", "-e", "tcp.srcport", "-e", "tcp.dstport",
                          "-e", "tcp.payload"], capture_output=True, text=True, check=True).stdout
    segs = collections.defaultdict(list)
    for line in out.splitlines():
        t, sp, dp, pl = line.split("\t")
        segs[(int(sp), int(dp))].append((float(t), bytes.fromhex(pl)))
    return segs


def messages(chunks):
    buf, times = bytearray(), []
    for t, data in chunks:
        buf += data
        times.append((len(buf), t))
    i = 0
    while i + 6 <= len(buf):
        if buf[i:i + 2] != b"\x78\x56":
            j = buf.find(b"\x78\x56", i + 1)
            if j < 0:
                break
            print(f"  !! resync skipped {j - i} bytes", file=sys.stderr)
            i = j
            continue
        n = struct.unpack_from("<H", buf, i + 2)[0]
        end = i + 4 + n
        if end > len(buf):
            break
        t = next((tt for pos, tt in times if pos >= end), times[-1][1])
        yield t, bytes(buf[i:end])
        i = end


def ascii_runs(b, minlen=3):
    out, cur = [], bytearray()
    for c in b:
        if 32 <= c < 127:
            cur.append(c)
        else:
            if len(cur) >= minlen:
                out.append(cur.decode())
            cur = bytearray()
    if len(cur) >= minlen:
        out.append(cur.decode())
    return out


def main():
    pcap = sys.argv[1]
    show_all = "--all" in sys.argv
    for (sp, dp), chunks in streams(pcap).items():
        direction = "iComm->client" if sp == 20000 else "client->iComm"
        msgs = list(messages(chunks))
        types = collections.Counter(struct.unpack_from("<H", m, 4)[0] for _, m in msgs)
        print(f"== {direction} {sp}->{dp}: {len(msgs)} msgs, types {dict(types)}")
        seen = collections.Counter()
        for t, m in msgs:
            typ = struct.unpack_from("<H", m, 4)[0]
            seen[typ] += 1
            if not show_all and seen[typ] > 3:
                continue
            print(f"  t={t:7.2f} type={typ:#06x} len={len(m):4d} {' | '.join(ascii_runs(m))}")
            print("     ", m.hex(" "))


if __name__ == "__main__":
    main()
