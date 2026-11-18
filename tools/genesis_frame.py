"""Innotech Genesis II RS485 "Net" framing, as implemented by iComm 1.30 (icomm.exe).

Recovered from icomm.exe (FrameTx FUN_0049effc, FrameRx FUN_0049f310, CRC FUN_004a2fa4):

    FF FF FF FF  22 DD  <hdr 9 bytes>  <crc16 LE>  <data N bytes>  <crc16 LE>  FF FF

  - firmware >= 3.00 uses sync 22 DD and a 9-byte header; older firmware uses 33 CC
    and drops header byte 0 (8-byte header).
  - CRC-16/ARC: poly 0xA001 (reflected 0x8005), init 0x0000, no final xor. Computed
    separately over the header and over the data; running the CRC over block+crc gives 0.
  - header: [0] seq  [1] dst addr  [2] drv  [3] src addr (FF = master)  [4] 0
            [5] flag  [6] cmd (reply: status, 0 = OK)  [7..8] data length LE

Usage:
    python genesis_frame.py capture.bin        # decode a raw byte capture
    python genesis_frame.py --hex "ff ff ..."  # decode hex text
"""
import argparse
import sys
from dataclasses import dataclass

SYNC_NEW = b"\x22\xdd"
SYNC_OLD = b"\x33\xcc"


def crc16_arc(data: bytes, crc: int = 0) -> int:
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


@dataclass
class Frame:
    offset: int
    seq: int
    dst: int
    drv: int
    src: int
    b4: int
    flag: int
    cmd: int
    data: bytes
    hdr_ok: bool
    data_ok: bool

    def describe(self) -> str:
        crc = "ok" if self.hdr_ok and self.data_ok else f"BAD(hdr={self.hdr_ok},data={self.data_ok})"
        return (f"@{self.offset:06x} seq={self.seq:3d} {self.src:02x}->{self.dst:02x} "
                f"drv={self.drv:02x} cmd={self.cmd:02x} flag={self.flag:02x} b4={self.b4:02x} "
                f"len={len(self.data):4d} crc={crc}  {self.data.hex(' ')}")


def build(seq: int, dst: int, drv: int, cmd: int, data: bytes = b"", src: int = 0xFF, flag: int = 0) -> bytes:
    hdr = bytes([seq & 0xFF, dst, drv, src, 0, flag, cmd]) + len(data).to_bytes(2, "little")
    return (b"\xff" * 4 + SYNC_NEW + hdr + crc16_arc(hdr).to_bytes(2, "little")
            + data + crc16_arc(data).to_bytes(2, "little") + b"\xff\xff")


def parse(buf: bytes):
    """Yield every new-style (22 DD) frame found in a byte stream."""
    i = 0
    while True:
        i = buf.find(SYNC_NEW, i)
        if i < 0 or i + 2 + 11 > len(buf):
            return
        h = buf[i + 2:i + 13]
        hdr_ok = crc16_arc(h) == 0
        n = int.from_bytes(h[7:9], "little")
        if not hdr_ok or i + 13 + n + 2 > len(buf):
            i += 1
            continue
        d = buf[i + 13:i + 13 + n + 2]
        yield Frame(i, h[0], h[1], h[2], h[3], h[4], h[5], h[6], d[:-2], hdr_ok, crc16_arc(d) == 0)
        i += 13 + n + 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", nargs="?")
    ap.add_argument("--hex")
    a = ap.parse_args()
    if a.hex:
        buf = bytes.fromhex(a.hex.replace(" ", "").replace("\n", ""))
    elif a.file:
        buf = open(a.file, "rb").read()
    else:
        f = build(7, 4, 0x05, 0x03, (123).to_bytes(4, "little"))
        print("self-test frame:", f.hex(" "))
        buf = f
    for fr in parse(buf):
        print(fr.describe())


if __name__ == "__main__":
    sys.exit(main())
