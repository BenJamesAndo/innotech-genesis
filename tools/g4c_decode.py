"""Decode an Innotech Genesis II controller configuration file.

Handles Supervisor's cfgcache `<ConfigKey>.g4c` files and Gen2Config `.G2C` files of the same
generation (magic 0x28FC / 0x72D9 / 0x772E, version >= 0x30B, <= 0x509). Format: docs/CONFIG_FILE.md.

    python g4c_decode.py FILE [--json out.json] [--raw]

Access codes are recognised but never printed or exported.
"""
import argparse
import json
import os
import struct
import sys
import xml.etree.ElementTree as ET

# Gen2Config's built-in block type table (GEN2VPRT.exe @0x50909C, 0x5C per type):
# type id -> (name, param bytes, input terminal names, output terminal names).
# Type id = BlocksLibrary DDC Id; the controller's record type byte is id + 1.
BLOCK_TYPES = {
    0: ('Analogue Input', 177, [], ['VALUE', 'REQUEST', 'FAIL']),
    1: ('Digital Input', 2, [], ['STATE']),
    2: ('Analogue Output', 35, ['INPUT', 'ENABLE', 'FAULT'], ['OUTPUT']),
    3: ('Digital Output', 6, ['INPUT', 'ENABLE', 'FAULT'], ['OUTPUT']),
    4: ('PID Loop', 36, ['INPUT', 'SET POINT', 'ENABLE'], ['DIRECT', 'REVERSE']),
    5: ('Comparator', 8, ['INPUT A', 'INPUT B', 'EXT TOL'], ['A > B', 'A < B', 'A = B']),
    6: ('Hysteresis', 8, ['INPUT', 'EXT OFF', 'EXT ON'], ['OUTPUT']),
    7: ('Recipe', 81, ['ENABLE', 'RESTART', 'STEP', 'HOLDING'], ['VALUE', 'STAGE', 'PROGRESS', 'DONE']),
    8: ('Universal Curve', 89, ['INPUT'], ['OUTPUT']),
    9: ('Calculator', 1, ['INPUT A', 'INPUT B'], ['OUTPUT']),
    10: ('Psychrometric', 5, ['DRY BULB', 'INPUT B'], ['OUTPUT']),
    11: ('Selector', 1, ['INPUT'] * 8, ['MAXIMUM', 'MINIMUM', 'AVERAGE']),
    12: ('Multiplexor', 1, ['INPUT %d' % i for i in range(8)] + ['CONTROL'], ['OUTPUT']),
    13: ('DeMultiplexor', 1, ['INPUT', 'CONTROL'], ['OUTPUT %d' % i for i in range(8)]),
    14: ('Lookup', 128, ['ENTRY'], ['VALUE']),
    15: ('Delay', 10, ['INPUT', 'ONDELAY', 'OFFDELAY'], ['OUTPUT']),
    16: ('Counter', 9, ['CNT LIM', 'UP PULSE', 'DN PULSE', 'RESET'], ['STARTED', 'REACHED', 'COUNT']),
    17: ('Timer', 10, ['EXT LIM', 'ENABLE', 'RESET'], ['RUNNING', 'DONE', 'TIME']),
    18: ('AND', 1, ['INPUT', 'INPUT'], ['OUTPUT']),
    19: ('OR', 1, ['INPUT', 'INPUT'], ['OUTPUT']),
    20: ('NOT', 1, ['INPUT'], ['OUTPUT']),
    21: ('XOR', 1, ['INPUT 1', 'INPUT 2'], ['OUTPUT']),
    22: ('Except Schedule', 133, ['VALUE', 'OVERRIDE'], ['OUTPUT', 'ACTIVE']),
    23: ('Weekly Schedule', 128, ['VALUE', 'OVERRIDE'], ['OUTPUT']),
    24: ('Latch', 6, ['EXT LIM', 'INPUT', 'RESET'], ['OUTPUT']),
    25: ('Alarm', 118, ['TRIGGER', 'LISTEN', 'RESET'], ['OUTPUT']),
    26: ('Log', 7, ['INPUT', 'ENABLE', 'TRIGGER'], []),
    27: ('Analyse', 6, ['INPUT', 'ENABLE', 'RESET', 'EXT INT'], ['MAXIMUM', 'MINIMUM', 'AVERAGE']),
    28: ('Global Input', 5, [], ['VALUE', 'FAULT']),
    29: ('User Variable', 17, [], ['OUTPUT']),
    30: ('Accumulator', 6, ['EN/VALUE', 'RESET'], ['ACCUM VALUE']),
    31: ('Switch', 1, ['INPUT A', 'INPUT B', 'CONTROL'], ['OUTPUT']),
    32: ('Pulse Counter', 4, [], ['PULSES']),
    33: ('Global Output', 1, ['INPUT'], []),
    34: ('Optimum Start', 22, ['SCHEDULED TIME', 'INPUT', 'SETPOINT'], ['START TIME']),
    35: ('Daily Schedule', 128, ['VALUE', 'OVERRIDE'], ['OUTPUT']),
    36: ('Random', 8, ['TRIGGER'], ['OUTPUT']),
    37: ('Encoder', 1, ['INPUT 1', 'INPUT 2', 'INPUT 3', 'INPUT 4'], ['OUTPUT']),
    38: ('Decoder', 1, ['INPUT'], ['OUTPUT 1', 'OUTPUT 2', 'OUTPUT 3', 'OUTPUT 4']),
    39: ('Logic', 2, ['INPUT A', 'INPUT B', 'INPUT C', 'INPUT D'], ['OUTPUT']),
    40: ('Net Input', 4, [], ['OUTPUT', 'FAULT']),
}
# BlocksLibrary.xml has WEEKLY=22 / YEARLY=23, the reverse of Gen2Config (22 = Except Schedule,
# 133-byte params incl. State/Year; 23 = Weekly, 128 bytes). Map Gen2 type -> library Id.
LIB_ID = {22: 23, 23: 22}
# Bytes at the start of the file's parameter area that Gen2Config does not send to the controller
# (confirmed for Log against live 2/5 records: record params = file params[1:]).
FILE_PARAM_PREFIX = {26: ('StorageChannel', 1)}


def record_type(gen2_type):
    """Controller record type byte ([0x15] of a 2/5 record) for a Gen2Config block type."""
    return LIB_ID.get(gen2_type, gen2_type) + 1

NODE_KINDS = {0: 'output', 1: 'input', 2: 'junction', 3: 'label'}
SCHEDULE_TYPES = (22, 23, 35)
HERE = os.path.dirname(os.path.abspath(__file__))


class Reader:
    def __init__(self, data, pos=0):
        self.d, self.p = data, pos

    def u8(self):
        v = self.d[self.p]; self.p += 1; return v

    def u16(self):
        v = struct.unpack_from('<H', self.d, self.p)[0]; self.p += 2; return v

    def u32(self):
        v = struct.unpack_from('<I', self.d, self.p)[0]; self.p += 4; return v

    def raw(self, n):
        v = self.d[self.p:self.p + n]; self.p += n; return v

    def pstr(self):
        """u16 length + fixed-size NUL-padded buffer."""
        n = self.u16()
        return cstr(self.raw(n))


def colorref(c):
    """Windows COLORREF (0x00BBGGRR) -> #rrggbb."""
    return '#%02x%02x%02x' % (c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF)


def cstr(b):
    return b.split(b'\0', 1)[0].decode('latin1')


def load_library(path):
    """Parameter layout per library type id. Uses an iComm BlocksLibrary.xml if one is given,
    otherwise the built-in table in _block_params.py (same data)."""
    lib = {}
    if not path or not os.path.exists(path):
        from _block_params import BLOCK_PARAMS
        return {bid: [dict(name=n, kind=k, off=o, type=t, mask=m) for n, k, o, t, m in pts]
                for bid, pts in BLOCK_PARAMS.items()}
    for b in ET.parse(path).getroot().findall('Block'):
        bid = int(b.get('Id'))
        if bid >= 50:
            continue
        lib[bid] = [dict(name=p.get('Name'), kind=p.get('Kind'), off=int(p.get('Offset')),
                         type=p.get('Type'), mask=p.get('Mask')) for p in b.findall('point')]
    return lib


def week_time(v):
    """Weekly schedule entry: minutes since Sunday 00:00 (fits every period seen: Mon-Fri
    06:00-18:00 style blocks, Sunday service times)."""
    days = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat']
    d, m = divmod(v, 1440)
    return '%s %02d:%02d' % (days[d] if d < 7 else 'd%d' % d, m // 60, m % 60)


def day_time(v):
    """Daily schedule entry: minutes since midnight [GUESS, by analogy with Weekly]."""
    return '%02d:%02d' % divmod(v, 60)


def except_time(v):
    """Except (holiday) schedule entry: 10-minute units, 144 per day, day = (month-1)*32 + (day-1).
    Fits a real site's public holidays exactly."""
    day, slot = divmod(v, 144)
    mon, dom = divmod(day, 32)
    return '%02d-%02d %02d:%02d' % (mon + 1, dom + 1, slot // 6, slot % 6 * 10)


def decode_params(btype, data, lib):
    """Named parameter values using BlocksLibrary INPUT points (offsets within the param area)."""
    out = {}
    if btype in FILE_PARAM_PREFIX:
        name, n = FILE_PARAM_PREFIX[btype]
        out[name] = data[0] if n == 1 else data[:n].hex()
        data = data[n:]
    if btype in SCHEDULE_TYPES:
        pairs = []
        for i in range(32):
            on, off = struct.unpack_from('<HH', data, i * 4)
            if on or off:
                f = {22: except_time, 23: week_time, 35: day_time}[btype]
                pairs.append({'on': on, 'off': off, 'on_text': f(on), 'off_text': f(off)})
        out['Periods'] = pairs
        if btype == 22 and len(data) >= 133:
            out['State'] = struct.unpack_from('<I', data, 128)[0]
            out['Year'] = 2000 + data[132]
        return out
    points = [p for p in lib.get(LIB_ID.get(btype, btype), []) if p['kind'] == 'INPUT']
    offs = sorted({p['off'] for p in points} | {len(data)})
    for p in points:
        o, t = p['off'], p['type']
        nxt = min(x for x in offs if x > o) if any(x > o for x in offs) else len(data)
        try:
            if t == 'byte':
                v = data[o]
            elif t == 'short':
                v = struct.unpack_from('<h', data, o)[0]
            elif t == 'long':
                v = struct.unpack_from('<i', data, o)[0]
            elif t in ('single', 'float', 'psingle', 'pfloat', 'sfloat', 'psfloat'):
                v = round(struct.unpack_from('<f', data, o)[0], 6)
            elif t == 'double':
                v = struct.unpack_from('<d', data, o)[0]
            elif t == 'string':
                v = cstr(data[o:nxt])
            elif t == 'bit':
                v = bool(data[o] & int(p['mask'] or 1))
            elif t == 'data':
                chunk = data[o:nxt]
                v = [round(x, 6) for x in struct.unpack('<%df' % (len(chunk) // 4), chunk[:len(chunk) // 4 * 4])]
            else:
                v = data[o:nxt].hex()
        except (IndexError, struct.error):
            continue
        out[p['name']] = v
    return out


def parse(data, lib):
    r = Reader(data)
    cfg = {'magic': r.u16(), 'version': r.u16(), 'flags': r.u8(), 'total_length': r.u32()}
    if cfg['magic'] not in (0x28FC, 0x72D9, 0x772E) or not 0x30B <= cfg['version'] < 0x50A:
        raise ValueError('unsupported file: magic %#x version %#x' % (cfg['magic'], cfg['version']))
    ver = cfg['version']

    # ---- header section (FUN_0043cf02) -------------------------------------------------------
    hstart = r.p
    hlen = r.u16()
    h = {}
    (h['unk07'], h['unk05'], n_blocks, n_nodes, n_labels, n_lines, n_texts, h['unk13']) = \
        [r.u16() for _ in range(8)]
    h['site'] = r.pstr()
    h['author'] = r.pstr()
    h['description'] = r.pstr()
    h['created'] = r.u32()          # unix time [GUESS: when the config was first created]
    h['saved'] = r.u32()            # unix time the file was written (Supervisor cache time)
    h['sysinfo_ver'] = r.u16()      # writer sets 0x0E
    h['sysinfo_len'] = r.u16()      # writer sets 0x38
    h['device_name'] = r.pstr()
    h['config_key'] = r.u32()       # = SYSTEM_READ 9/1 [0x31] config ID / iComm System~ConfigKey (unix time of download)
    h['unk102'] = r.u32()
    h['unk106'] = r.u32()
    h['unk10a'], h['unk10c'], h['unk10e'] = r.u16(), r.u16(), r.u16()
    h['unk110'] = r.u8()
    h['unk111'] = r.u16()
    h['address'] = r.u8()
    # access codes: count, size, stride; never decoded into the output
    n_codes, csize, cstride = r.u16(), r.u16(), r.u16()
    r.p += n_codes * cstride
    h['access_codes'] = '%d entries (not shown)' % n_codes
    n_pages, psize = r.u16(), r.u16()
    h['page_titles'] = [cstr(r.raw(psize)) for _ in range(n_pages)]
    n_keys, ksize = r.u16(), r.u16()
    h['fkey_labels'] = [cstr(r.raw(ksize)) for _ in range(n_keys)]
    rest = hstart + hlen - r.p
    if rest >= 2:
        h['unk3e4'], h['unk3e5'] = r.u8(), r.u8()
    if rest >= 4:
        h['gen_version'] = r.u16()
    if rest >= 8:
        h['unk3e8'], h['unk3ea'] = r.u16(), r.u16()
    if rest >= 10:
        h['address2'] = r.u16()
    r.p = hstart + hlen
    cfg['header'] = h

    # ---- blocks (FUN_0043f438) ---------------------------------------------------------------
    sec = r.p
    seclen, tsize = r.u16(), r.u16()
    blocks = {}
    for _ in range(n_blocks):
        start = r.p
        nlen = r.u16()
        b = {'seq': r.u16()}
        b['name'] = r.pstr()
        b['x'], b['y'] = r.u16(), r.u16()
        b['type'], b['n_in'], b['n_out'], psz, b['unk39'], b['attr'] = [r.u16() for _ in range(6)]
        nterm = r.u16()
        terms = []
        for _ in range(nterm):
            q = r.p
            pos, side, idx, watched = struct.unpack_from('<4H', data, q)
            r.p = q + tsize
            terms.append({'index': idx, 'pin_row': pos & 0xFF, 'pin_side': pos >> 8,
                          'kind': 'input' if (pos >> 8) == 2 else 'output', 'watched': bool(watched)})
        b['params_raw'] = r.raw(psz)
        r.p = start + nlen
        tdef = BLOCK_TYPES.get(b['type'], ('type %d' % b['type'], None, [], []))
        b['type_name'] = tdef[0]
        b['record_type'] = record_type(b['type'])
        b['input_names'] = (tdef[2] + ['IN%d' % i for i in range(len(tdef[2]), b['n_in'])])[:b['n_in']]
        b['output_names'] = (tdef[3] + ['OUT%d' % i for i in range(len(tdef[3]), b['n_out'])])[:b['n_out']]
        b['terminals'] = terms
        b['params'] = decode_params(b['type'], b['params_raw'], lib)
        blocks[b['seq']] = b
    r.p = sec + seclen

    # ---- nodes: terminals, junctions, label pins (FUN_00440a92) --------------------------------
    sec = r.p
    seclen, nsize, wsize = r.u16(), r.u16(), r.u16()
    nodes = []
    for i in range(n_nodes):
        q = r.p
        kind, x, y, a, b_ = struct.unpack_from('<5H', data, q)
        r.p = q + nsize
        n = {'id': i + 1, 'kind': NODE_KINDS.get(kind, kind), 'x': x, 'y': y}
        if kind in (0, 1):
            n['block'], n['terminal'] = a, b_
            blk = blocks.get(a)
            t = blk['terminals'][b_] if blk and b_ < len(blk['terminals']) else None
            if t and t['watched']:
                w = r.raw(wsize)
                n['watch'] = {'flag': w[0], 'block': struct.unpack_from('<H', w, 1)[0],
                              'label': cstr(w[3:14]), 'analog': w[14], 'on_text': cstr(w[15:25]),
                              'off_text': cstr(w[25:35]), 'units': cstr(w[35:39]), 'decimals': w[39],
                              'description': cstr(w[40:61])}
        elif kind == 3:
            n['label'] = a
        nodes.append(n)
    r.p = sec + seclen

    # ---- connector labels (FUN_00441c0f) -----------------------------------------------------
    sec = r.p
    seclen, lsize = r.u16(), r.u16()
    labels = []
    for i in range(n_labels):
        q = r.p
        a, colour = struct.unpack_from('<II', data, q)
        rr = Reader(data, q + 8)
        name = rr.pstr()
        w, hgt = rr.u16(), rr.u16()
        labels.append({'id': i + 1, 'name': name, 'colour': colorref(colour),
                       'unk0': a, 'width': w, 'height': hgt})
        r.p = q + lsize
    r.p = sec + seclen

    # ---- lines (FUN_004426cd): pairs of 1-based node ids -------------------------------------
    sec = r.p
    seclen, esize = r.u16(), r.u16()
    lines = []
    for _ in range(n_lines):
        a, b_ = struct.unpack_from('<HH', data, r.p)
        r.p += esize
        lines.append((a, b_))
    r.p = sec + seclen

    # ---- free text (FUN_004431d9) ------------------------------------------------------------
    sec = r.p
    seclen = r.u16()
    texts = []
    for _ in range(n_texts):
        q = r.p
        elen = r.u16()
        t = {'text': cstr(r.raw(r.u16()))}
        t['x'], t['y'] = r.u16(), r.u16()
        t['font'] = cstr(r.raw(r.u16()))
        t['size'] = r.u16()
        t['colour'] = colorref(r.u32())
        t['flags'] = r.u16()
        texts.append(t)
        r.p = q + elen
    r.p = sec + seclen

    # ---- watch pages (FUN_00443e8f) ----------------------------------------------------------
    sec = r.p
    seclen, esize, n_wp, n_wl = r.u16(), r.u16(), r.u16(), r.u16()
    pages = []
    for pg in range(n_wp):
        ent = []
        for ln in range(n_wl):
            nid, flag = struct.unpack_from('<HB', data, r.p)
            r.p += esize
            if nid:
                ent.append({'line': ln + 1, 'node': nid, 'adjustable': flag})
        title = h['page_titles'][pg] if pg < len(h['page_titles']) else ''
        pages.append({'page': pg + 1, 'title': title, 'lines': ent})
    r.p = sec + seclen

    # ---- hardware module definitions (FUN_0042fb29, partially decoded) -----------------------
    modules = []
    if ver > 0x314 and r.p + 4 <= len(data):
        mver, cnt = r.u16(), r.u16()
        tail = data[r.p:]
        strings, cur = [], b''
        for ch in tail:
            if 32 <= ch < 127:
                cur += bytes([ch])
            else:
                if len(cur) >= 4:
                    strings.append(cur.decode())
                cur = b''
        modules = {'version': mver, 'count': cnt, 'strings': strings}

    cfg.update(blocks=blocks, nodes=nodes, labels=labels, lines=lines, texts=texts,
               watch_pages=pages, modules=modules)
    cfg['links'], cfg['nets'] = resolve_links(blocks, nodes, lines)
    return cfg


def resolve_links(blocks, nodes, lines):
    """Wires are a graph of nodes joined by lines; label pins with the same label id are joined
    too. Each net with one output terminal drives every input terminal on it."""
    parent = list(range(len(nodes) + 1))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    def union(a, b):
        parent[find(a)] = find(b)

    for a, b in lines:
        if 0 < a <= len(nodes) and 0 < b <= len(nodes):
            union(a, b)
    first_label = {}
    for n in nodes:
        if n['kind'] == 'label':
            if n['label'] in first_label:
                union(n['id'], first_label[n['label']])
            else:
                first_label[n['label']] = n['id']
    nets = {}
    for n in nodes:
        nets.setdefault(find(n['id']), []).append(n)
    links = []
    net_list = []
    for root, members in nets.items():
        outs = [m for m in members if m['kind'] == 'output']
        ins = [m for m in members if m['kind'] == 'input']
        labs = sorted({m['label'] for m in members if m['kind'] == 'label'})
        if len(members) > 1:
            net_list.append({'net': root, 'nodes': [m['id'] for m in members], 'labels': labs})
        for i in ins:
            dst = blocks.get(i['block'])
            dst_in = i['terminal']
            if not outs:
                continue
            for o in outs:
                src = blocks.get(o['block'])
                src_out = o['terminal'] - src['n_in'] if src else o['terminal']
                links.append({'src': o['block'], 'src_out': src_out,
                              'src_out_name': src['output_names'][src_out] if src and 0 <= src_out < len(src['output_names']) else '?',
                              'dst': i['block'], 'dst_in': dst_in,
                              'dst_in_name': dst['input_names'][dst_in] if dst and dst_in < len(dst['input_names']) else '?',
                              'net': root, 'labels': labs, 'conflict': len(outs) > 1})
    links.sort(key=lambda l: (l['dst'], l['dst_in']))
    return links, net_list


def fmt_ts(t):
    import datetime
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime('%Y-%m-%d %H:%M UTC') if t else '-'


def report(cfg, out=sys.stdout, raw=False):
    h = cfg['header']
    blocks, nodes = cfg['blocks'], cfg['nodes']
    p = lambda *a: print(*a, file=out)
    p('File: magic %#06x version %#06x, %d bytes payload' % (cfg['magic'], cfg['version'], cfg['total_length']))
    p('Site: %r  Author: %r' % (h['site'], h['author']))
    p('Description: %r' % h['description'])
    p('Device: %r  address %d  (sysinfo ver %#x len %#x, Genesis ver %#x)' % (
        h['device_name'], h['address'], h['sysinfo_ver'], h['sysinfo_len'], h.get('gen_version', 0)))
    p('ConfigKey %#010x = %d (downloaded %s); created %s; file saved %s' % (
        h['config_key'], h['config_key'], fmt_ts(h['config_key']), fmt_ts(h['created']), fmt_ts(h['saved'])))
    p('Access codes: %s' % h['access_codes'])
    p('F-key labels: %s' % ', '.join(repr(k.replace('\r\n', ' / ').replace('\r', '')) for k in h['fkey_labels'] if k))
    p('Counts: %d blocks, %d nodes, %d lines, %d labels, %d texts' % (
        len(blocks), len(nodes), len(cfg['lines']), len(cfg['labels']), len(cfg['texts'])))

    p('\n== Watch pages')
    for pg in cfg['watch_pages']:
        if not pg['lines']:
            continue
        p('Page %d %r' % (pg['page'], pg['title'].replace('\r\n', ' / ') or '(untitled)'))
        for ln in pg['lines']:
            n = nodes[ln['node'] - 1]
            b = blocks.get(n.get('block'))
            w = n.get('watch', {})
            out_name = '?'
            if b:
                oi = n['terminal'] - b['n_in']
                out_name = b['output_names'][oi] if 0 <= oi < len(b['output_names']) else 'T%d' % n['terminal']
            p('  %2d %-21s %-10s %s.%s  %s%s' % (
                ln['line'], w.get('description', ''), w.get('label', ''), n.get('block'),
                out_name, b['name'] if b else '?', '  [adjustable]' if ln['adjustable'] else ''))

    p('\n== Blocks')
    by_dst = {}
    for l in cfg['links']:
        by_dst.setdefault((l['dst'], l['dst_in']), []).append(l)
    for seq in sorted(blocks):
        b = blocks[seq]
        p('%3d %-21s %-16s @(%d,%d)' % (seq, repr(b['name']), b['type_name'], b['x'], b['y']))
        for i, nm in enumerate(b['input_names']):
            ls = by_dst.get((seq, i))
            if ls:
                src = ', '.join('%d %s.%s%s' % (l['src'], blocks[l['src']]['name'] if l['src'] in blocks else '?',
                                                l['src_out_name'], ' (CONFLICT)' if l['conflict'] else '') for l in ls)
                lab = ' via label ' + ','.join(cfg['labels'][x - 1]['name'] for x in ls[0]['labels']
                                                  if 0 < x <= len(cfg['labels'])) if ls[0]['labels'] else ''
                p('      in %-10s <- %s%s' % (nm, src, lab))
            else:
                p('      in %-10s <- (unconnected)' % nm)
        if b['output_names']:
            p('      out: %s' % ', '.join(b['output_names']))
        for k, v in b['params'].items():
            if k == 'Periods':
                for per in v:
                    p('      period %s -> %s' % (per['on_text'], per['off_text']))
            else:
                p('      %s = %r' % (k, v))
        if raw:
            p('      raw: %s' % b['params_raw'].hex())

    if cfg['texts']:
        p('\n== Text')
        for t in cfg['texts']:
            p('  @(%d,%d) %r' % (t['x'], t['y'], t['text']))
    if cfg['modules']:
        p('\n== Hardware modules (partial): %s' % '; '.join(cfg['modules']['strings']))


def to_json(cfg):
    h = dict(cfg['header'])
    blocks = []
    for seq in sorted(cfg['blocks']):
        b = dict(cfg['blocks'][seq])
        b['params_raw'] = b['params_raw'].hex()
        blocks.append(b)
    return {
        'format': {'magic': cfg['magic'], 'version': cfg['version'], 'total_length': cfg['total_length']},
        'header': h,
        'blocks': blocks,
        'links': [{k: l[k] for k in ('src', 'src_out', 'src_out_name', 'dst', 'dst_in', 'dst_in_name', 'labels', 'conflict')}
                  for l in cfg['links']],
        'wires': {'nodes': cfg['nodes'], 'lines': cfg['lines'], 'labels': cfg['labels']},
        'texts': cfg['texts'],
        'watch_pages': cfg['watch_pages'],
        'modules': cfg['modules'],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('file')
    ap.add_argument('--json', help='write JSON (blocks with x/y, links as edges, raw wire geometry)')
    ap.add_argument('--raw', action='store_true', help='also print raw parameter bytes')
    ap.add_argument('--library', default=None)
    a = ap.parse_args()
    data = open(a.file, 'rb').read()
    cfg = parse(data, load_library(a.library))
    report(cfg, raw=a.raw)
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as f:
            json.dump(to_json(cfg), f, indent=1)
        print('\nJSON written to %s' % a.json)


if __name__ == '__main__':
    main()
