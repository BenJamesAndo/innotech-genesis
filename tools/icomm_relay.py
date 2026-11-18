"""TCP relay for testing: lets a client on another network (e.g. an ESP on the bench) reach an iComm server through this PC.
Logs every 78 56 message in both directions (type + length + hex) to icomm_relay.log.

python icomm_relay.py [listen_port=20000] [target=192.168.1.10:20000]
"""
import asyncio
import datetime
import struct
import sys

LISTEN = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
TARGET = sys.argv[2] if len(sys.argv) > 2 else "192.168.1.10:20000"
THOST, TPORT = TARGET.split(":")
LOG = open("icomm_relay.log", "a", buffering=1)


def log(line):
    LOG.write(f"{datetime.datetime.now():%H:%M:%S.%f} {line}\n")


async def pump(reader, writer, tag):
    buf = bytearray()
    try:
        while data := await reader.read(65536):
            writer.write(data)
            await writer.drain()
            buf += data
            while len(buf) >= 4 and buf[:2] == b"\x78\x56":
                n = struct.unpack_from("<H", buf, 2)[0]
                if len(buf) < 4 + n:
                    break
                m = bytes(buf[:4 + n])
                del buf[:4 + n]
                log(f"{tag} type={struct.unpack_from('<H', m, 4)[0]:#06x} len={len(m)} {m.hex(' ')}")
            if buf and buf[:2] != b"\x78\x56":
                log(f"{tag} raw {bytes(buf).hex(' ')}")
                buf.clear()
    except (ConnectionError, OSError):
        pass
    finally:
        writer.close()


async def handle(cr, cw):
    peer = cw.get_extra_info("peername")
    log(f"client {peer} connected")
    try:
        sr, sw = await asyncio.open_connection(THOST, int(TPORT))
    except OSError as e:
        log(f"upstream connect failed: {e}")
        cw.close()
        return
    await asyncio.gather(pump(cr, sw, "C>S"), pump(sr, cw, "S>C"))
    log(f"client {peer} closed")


async def main():
    srv = await asyncio.start_server(handle, "0.0.0.0", LISTEN)
    log(f"listening on {LISTEN} -> {TARGET}")
    async with srv:
        await srv.serve_forever()


asyncio.run(main())
