#!/usr/bin/env python3
"""The scripts of Gothic 1 as one Daedalus source this project's compiler takes (tools/bake/daedalus.h).

For the test "g1 beasts" (tests/test_core.cpp, G3_G1_SCRIPT=<out>): the real AI of the game on the synthetic world.

    git clone --depth 1 https://github.com/AmProsius/gothic-1-community-patch.git g1cp
    python3 scripts/g1_script_source.py g1cp /tmp/g1.d
    G3_G1_SCRIPT=/tmp/g1.d build/tests/test_core build/tests "g1 beasts"

<g1cp> is the clone: the sources are under scriptbase/_work/Data/Scripts/Content (Gothic.src order), the engine's
functions are declared in .dls/externals/gothic.d. Left out: Story/Startup.d and the test levels' start-ups (the test
gives its own STARTUP_WORLD / INIT_WORLD). Where our compiler is stricter than the game's, the source is bent a little:
umlauts in names, duplicate or loosely typed externals (the synthetic script's own declarations win), NULL, a
`const func`, a no-op comparison used as a statement. Licence of the scripts: non-commercial use only (the mod kit's).
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def read(path):
    return open(path, 'rb').read().decode('cp1252')


def main(clone, out):
    root = os.path.join(clone, 'scriptbase/_work/Data/Scripts/Content')
    ext = read(os.path.join(clone, '.dls/externals/gothic.d'))

    def param_list(text):
        params = []
        for i, p in enumerate(x.strip() for x in text.split(',') if x.strip()):
            w = p.split()
            t = w[1] if w[0].lower() == 'var' and len(w) > 1 else w[0]
            if t.lower() == 'instance':
                t = 'C_NPC'
            if not re.match(r'^(int|float|string|func|c_\w+)$', t, re.I):
                t = 'int'
            params.append('var %s p%d' % (t, i))
        return ', '.join(params)

    # The synthetic script's declarations are the ones the engine is known to take.
    synth = read(os.path.join(HERE, '../tools/bake/synth_script.cpp'))
    known = {}
    for d in re.findall(r'^extern func[^;]*;', synth, re.M):
        m = re.match(r'extern func\s+\w+\s+(\w+)\(', d)
        known[m.group(1).lower()] = ' '.join(d.split())
    decls, seen = [], set()
    for m in re.finditer(r'^\s*func\s+(\w+)\s+(\w+)\s*\(([^)]*)\)', ext, re.I | re.M):
        t, name, params = m.groups()
        if name.lower() in seen:
            continue
        seen.add(name.lower())
        decls.append(known.get(name.lower(), 'extern func %s %s(%s);' % (t.lower(), name, param_list(params))))
    decls.append('extern func void Wld_SpawnNpcRange(var C_NPC p0, var int p1, var int p2, var float p3);')
    decls.append('func void NOFUNC() {};')

    parts = []

    def walk(src, base):
        for line in read(src).splitlines():
            entry = line.strip()
            if not entry or entry.startswith('//'):
                continue
            path = os.path.join(base, entry.replace('\\', '/'))
            rel = os.path.relpath(path, root).lower()
            if rel == 'story/startup.d' or (rel.startswith('ai/test_skripts') and 'startup' in rel):
                continue
            if entry.lower().endswith('.src'):
                walk(path, os.path.dirname(path))
                continue
            parts.append('// ---- %s\n%s' % (rel, read(path)))
            if rel == '_intern/classes.d':
                parts.append('\n'.join(decls))

    walk(os.path.join(root, 'Gothic.src'), root)
    s = '\n'.join(parts)
    for a, b in {'ä': 'ae', 'ö': 'oe', 'ü': 'ue', 'Ä': 'Ae', 'Ö': 'Oe', 'Ü': 'Ue', 'ß': 'ss'}.items():
        s = s.replace(a, b)
    s = ''.join(c if ord(c) < 128 else '_' for c in s)
    s = s.replace('const func PLAYER_PERC_ASSESSMAGIC = B_AssessMagic;', '')
    s = re.sub(r'\bNULL\b', 'self', s)
    s = re.sub(r'^(extern func void Mdl_ApplyOverlayMDSTimed\(.*), var float p2\);', r'\1, var int p2);', s, flags=re.M)
    s = re.sub(r'^(\s*[A-Za-z_0-9.]+(\[[A-Za-z_0-9]+\])?)\s*==\s*([A-Za-z_0-9]+)\s*;', r'\1 = \3;', s, flags=re.M)
    open(out, 'w').write(s)
    print('%d files, %d engine functions -> %s' % (len(parts) - 1, len(decls), out))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('usage: g1_script_source.py <gothic-1-community-patch clone> <out.d>')
    main(sys.argv[1], sys.argv[2])
