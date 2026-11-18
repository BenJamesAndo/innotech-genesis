"""Decode HHD Serial Port Monitor text/CSV exports (IRP view) into Genesis frames.

Writes = IRP_MJ_WRITE DOWN rows (host -> bus); reads = IRP_MJ_READ UP rows (bus -> host).
Usage: python spm_decode.py "<export>.txt|.csv" [--raw]
"""
import csv
import importlib.util
import os
import struct
import sys

_spec = importlib.util.spec_from_file_location("gf", os.path.join(os.path.dirname(__file__), "genesis_frame.py"))
gf = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gf)


def rows(path):
    raw = open(path, "rb").read()
    text = raw.decode("utf-16") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else raw.decode("latin-1")
    delim = ";" if text.splitlines()[0].count(";") > text.splitlines()[0].count("\t") else "\t"
    for r in csv.reader(text.splitlines()[1:], delimiter=delim):
        if len(r) >= 6:
            yield r[1], r[2], r[3], r[5]


def events(path):
    """Merge consecutive same-direction chunks; yield (time, 'TX'|'RX', bytes)."""
    cur_dir, cur_t, buf = None, None, bytearray()
    for t, func, direction, data in rows(path):
        if func == "IRP_MJ_WRITE" and direction == "DOWN":
            d = "TX"
        elif func == "IRP_MJ_READ" and direction == "UP":
            d = "RX"
        else:
            continue
        b = bytes.fromhex(data.strip()) if data.strip() else b""
        if not b:
            continue
        if d != cur_dir and buf:
            yield cur_t, cur_dir, bytes(buf)
            buf = bytearray()
        if not buf:
            cur_t = t
        cur_dir = d
        buf += b
    if buf:
        yield cur_t, cur_dir, bytes(buf)


def f32s(b):
    return [struct.unpack_from("<f", b, i)[0] for i in range(0, len(b) - 3, 4)]


def main():
    path = sys.argv[1]
    for t, d, b in events(path):
        frames = list(gf.parse(b))
        if not frames:
            if "--raw" in sys.argv:
                print(f"{t} {d} (no frame) {b.hex(' ')}")
            continue
        for fr in frames:
            extra = ""
            if fr.drv == 2 and fr.cmd == 9 and d == "TX":
                n = fr.data[0]
                extra = " pts=" + ",".join(f"{fr.data[1+2*i]}.{fr.data[2+2*i]}" for i in range(n))
            elif d == "RX" and fr.data and len(fr.data) == 1 + 4 * fr.data[0]:
                extra = " vals=" + ",".join(f"{v:.3g}" for v in f32s(fr.data[1:]))
            print(f"{t} {d} {fr.describe()[:220]}{extra}")


if __name__ == "__main__":
    main()
