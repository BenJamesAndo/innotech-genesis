"""Build a self-contained HTML viewer for one or more Genesis II config files.

    python g4c_view.py FILE.g4c [FILE2.g4c ...] [-o view.html]

Each file is decoded with g4c_decode.py and embedded in g4c_view.html, so the output opens in any browser
with no server. The page draws each controller's program the way it sits on the Gen2Config drawing
(blocks, wires, labels, headings); click a block for its links and parameters. Access codes are never
included. The output does contain the site name and author from the file, so keep it private.
"""
import argparse
import json
import os

from g4c_decode import load_library, parse, to_json

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('files', nargs='+')
    ap.add_argument('-o', '--out', default='g4c_view_out.html')
    a = ap.parse_args()
    lib = load_library(None)
    configs = []
    for f in a.files:
        cfg = to_json(parse(open(f, 'rb').read(), lib))
        cfg['header'].pop('access_codes', None)
        configs.append(cfg)
    configs.sort(key=lambda c: c['header'].get('address', 0))
    page = open(os.path.join(HERE, 'g4c_view.html'), encoding='utf-8').read()
    data = json.dumps(configs, separators=(',', ':')).replace('</', '<\\/')
    page = page.replace('const CONFIGS = /*CONFIGS*/[];', 'const CONFIGS = %s;' % data, 1)
    with open(a.out, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write(page)
    print('wrote %s (%d controller%s)' % (a.out, len(configs), '' if len(configs) == 1 else 's'))


if __name__ == '__main__':
    main()
