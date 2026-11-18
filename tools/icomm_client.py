"""Minimal iComm TCP :20000 client (the protocol Supervisor speaks).

Envelope: 78 56 | len u16 (bytes after this field) | type u16 | body
  type 5  read   : reqId u32 | "<conn>:<dev>" char[16] | "<block>~<field>" char[32]
  type 6  write  : reqId u32 | "<conn>:<dev>" char[16] | "<block>~<field>" char[32] | dtype u8 | value
  type 1  reply  : reqId u32 | echoed request type u16 | result u16 (1 = OK, else error e.g. 0xFC15) | value…
  type 16 keepalive from iComm (~10 s)

Usage:
  python icomm_client.py read  1:4 "AHU 3 SUPPLY FAN~OUTPUT" ["@2~VALUE" ...]
  python icomm_client.py write 1:4 "@2~VALUE" 22.5
"""
import os
import socket
import struct
import sys
import time

HOST, PORT = os.environ.get("ICOMM_HOST", "192.168.1.10"), 20000  # set ICOMM_HOST to your iComm server


def pad(s: str, n: int) -> bytes:
    b = s.encode("latin-1")
    if len(b) >= n:
        raise ValueError(f"{s!r} longer than {n - 1}")
    return b + b"\0" * (n - len(b))


class IComm:
    def __init__(self, host=HOST, port=PORT, timeout=15):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.settimeout(timeout)
        self.req = int(time.time()) & 0xFFFF
        self.buf = bytearray()

    def send(self, mtype: int, body: bytes):
        self.s.sendall(b"\x78\x56" + struct.pack("<HH", len(body) + 2, mtype) + body)

    def recv_msg(self):
        while True:
            if len(self.buf) >= 4:
                assert self.buf[:2] == b"\x78\x56", self.buf[:16].hex()
                n = struct.unpack_from("<H", self.buf, 2)[0]
                if len(self.buf) >= 4 + n:
                    m = bytes(self.buf[:4 + n])
                    del self.buf[:4 + n]
                    return struct.unpack_from("<H", m, 4)[0], m[6:]
            chunk = self.s.recv(65536)
            if not chunk:
                raise ConnectionError("closed")
            self.buf += chunk

    def request(self, mtype: int, body_after_id: bytes):
        self.req += 1
        rid = self.req
        t0 = time.perf_counter()
        self.send(mtype, struct.pack("<I", rid) + body_after_id)
        while True:
            typ, body = self.recv_msg()
            if typ == 1 and struct.unpack_from("<I", body)[0] == rid:
                return body[4:], time.perf_counter() - t0
            # ignore keepalives / other traffic

    def read(self, dev: str, point: str):
        return self.request(5, pad(dev, 16) + pad(point, 32))

    def write(self, dev: str, point: str, value: float, dtype: int = 2):
        return self.request(6, pad(dev, 16) + pad(point, 32) + bytes([dtype]) + struct.pack("<d", value))


def show(point, body, dt):
    echo, result = struct.unpack_from("<HH", body)
    val = body[4:]
    dec = ""
    if len(val) == 8:
        dec = f" = {struct.unpack('<d', val)[0]!r}"
    elif len(val) == 4:
        dec = f" = {struct.unpack('<i', val)[0]} / {struct.unpack('<f', val)[0]!r}"
    print(f"{point:32s} {dt*1000:7.0f} ms  echo={echo} result={result:#06x} raw={val.hex(' ')}{dec}")


if __name__ == "__main__":
    op, dev = sys.argv[1], sys.argv[2]
    c = IComm()
    if op == "read":
        for p in sys.argv[3:]:
            show(p, *c.read(dev, p))
    elif op == "write":
        p, v = sys.argv[3], float(sys.argv[4])
        show(p, *c.write(dev, p, v))
