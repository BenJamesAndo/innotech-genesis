"""Send one raw Net request through iComm (message type 7) and print the type-8 reply. READ-ONLY use only.

python icomm_raw.py DEV DRV CMD [payload_hex]     e.g.  python icomm_raw.py 3 0 ff
Struct after the invoke id (32 bytes, from the Gen2Xtract capture):
  [0..3] u32 FFFFFFFF  [4..5] u16 connection id  [6..7] u16 session (0 = none)
  [8..9] device address as ASCII text ("4\\0")   [10..13] u32 1   [14..24] padding
  [25] flag (03 before logon, 05 after)  [26] u16 cmd  [28] u16 drv  [30] u16 len  then payload (min 1 byte)
"""
import os
import socket
import struct
import sys
import time

HOST, PORT = os.environ.get("ICOMM_HOST", "192.168.1.10"), 20000  # set ICOMM_HOST to your iComm server


def frame(mtype, body):
    return b"\x78\x56" + struct.pack("<HH", len(body) + 2, mtype) + body


def recv_msg(s, buf):
    while True:
        if len(buf) >= 4:
            n = struct.unpack_from("<H", buf, 2)[0]
            if len(buf) >= 4 + n:
                m = bytes(buf[:4 + n])
                del buf[:4 + n]
                return struct.unpack_from("<H", m, 4)[0], m[6:]
        chunk = s.recv(65536)
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk


def raw_struct(dev, drv, cmd, payload, conn=1, flag=0x03):
    head = bytearray(32)
    struct.pack_into("<IHH", head, 0, 0xFFFFFFFF, conn, 0)
    d = str(dev).encode()
    head[8:8 + len(d)] = d
    struct.pack_into("<I", head, 10, 1)
    head[25] = flag
    struct.pack_into("<HHH", head, 26, cmd, drv, len(payload))
    return bytes(head) + (payload if payload else b"\x00")


def request(s, buf, inv, dev, drv, cmd, payload=b""):
    t0 = time.perf_counter()
    s.sendall(frame(7, struct.pack("<I", inv) + raw_struct(dev, drv, cmd, payload)))
    while True:
        typ, body = recv_msg(s, buf)
        if typ == 8 and struct.unpack_from("<I", body, 0)[0] == inv:
            res, dl = struct.unpack_from("<HH", body, 4)
            return res, body[8:8 + dl], time.perf_counter() - t0


def connect():
    s = socket.create_connection((HOST, PORT), timeout=15)
    buf = bytearray()
    hello = struct.pack("<IH", 1, 10) + b"raw probe".ljust(32, b"\0") + b"".ljust(32, b"\0")
    s.sendall(frame(0x11, hello))
    recv_msg(s, buf)
    return s, buf


if __name__ == "__main__":
    dev, drv, cmd = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
    pl = bytes.fromhex(sys.argv[4]) if len(sys.argv) > 4 else b""
    s, buf = connect()
    res, data, dt = request(s, buf, 2, dev, drv, cmd, pl)
    txt = "".join(chr(c) if 32 <= c < 127 else "." for c in data[:80])
    print(f"dev {dev} drv {drv:#04x} cmd {cmd:#04x}: result {res} in {dt*1000:.0f} ms, {len(data)} B {data[:48].hex(' ')} |{txt}")
