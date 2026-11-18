"""Force / release an Analogue Output block via raw 2/5 read + 2/6 write through iComm (RAM only, no flash save).

python ao_force.py DEV BLOCK            show the AO's force params
python ao_force.py DEV BLOCK VALUE      force to VALUE in OUTPUT units (Y1..Y2, usually 0..1, so 0.5 = 50 %)
python ao_force.py DEV BLOCK release    release the force
AO params (docs/BLOCKS.md): Forced@0 byte, ForceValue@1 single, X1@5, X2@9, Y1@17, Y2@21, DisableValue@25, FaultValue@29, Type@33
"""
import struct
import sys

from icomm_raw import connect, request

AO_TYPE = 0x03  # record type byte (see docs/BLOCKS.md)


def params(rec):
    base = 0x1E + 6 * rec[0x1A] + 4 * rec[0x1B]
    return base


def show(rec):
    b = params(rec)
    f = lambda o: struct.unpack_from("<f", rec, b + o)[0]
    print(f"  '{rec[:20].split(b'\0')[0].decode('latin-1')}' type {rec[0x15]:#04x} Forced={rec[b]} ForceValue={f(1):.2f} "
          f"X1={f(5):.1f} X2={f(9):.1f} Y1={f(17):.1f} Y2={f(21):.1f} Disable={f(25):.1f} Fault={f(29):.1f} Type={rec[b+33]}")


def main():
    dev, blk = sys.argv[1], int(sys.argv[2])
    s, buf = connect()
    res, rec, _ = request(s, buf, 2, dev, 2, 5, bytes([blk]))
    assert res in (0, 1) and len(rec) > 0x22, f"read failed {res}"
    show(rec)
    if len(sys.argv) < 4:
        return
    assert rec[0x15] == AO_TYPE, "not an Analogue Output, refusing"
    rec = bytearray(rec)
    b = params(rec)
    if sys.argv[3] == "release":
        rec[b] = 0
        struct.pack_into("<f", rec, b + 1, 0.0)  # back to the factory ForceValue
    else:
        rec[b] = 1
        struct.pack_into("<f", rec, b + 1, float(sys.argv[3]))
    res, data, dt = request(s, buf, 3, dev, 2, 6, bytes([blk]) + bytes(rec))
    print(f"  write result {res} in {dt*1000:.0f} ms {data[:8].hex(' ')}")
    res, rec, _ = request(s, buf, 4, dev, 2, 5, bytes([blk]))
    show(rec)


if __name__ == "__main__":
    main()
