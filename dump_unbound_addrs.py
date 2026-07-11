#!/usr/bin/env python3
"""Dump the UNBOUND .trace imports (the dead names in trace_unbound.txt) WITH their addresses.

For every by-name .trace import record whose name is in trace_unbound.txt's dead list, print:
  NAME   record_rva   [slot_rvas...]   resolvable_in_dll

- record_rva : the value an UNBOUND slot holds. Calling through the slot jumps here (into the
               hint+name bytes) and faults -- this is exactly the GetApi @ 0xA97DF14 crash.
- slot_rvas  : the .trace slots that point at the record (usually 2 -- copyA ~0xA979xxx, copyB ~0xA97Bxxx).
- resolvable : which sibling DLL (if any) actually exports the name (e.g. GetApi -> tobii_gameintegration_x64.dll).
               Blank = no DLL in the probed dirs exports it (genuinely dead / must be stubbed or emulated).

Usage:  python dump_unbound_addrs.py  [> trace_unbound_addrs.txt]
"""
import struct, glob, os

GAME = r'C:\Users\qstli\Downloads\duniabackup\DuniaDemo_clang_64_dx11.dll'
UNBOUND_TXT = r'C:\Users\qstli\Downloads\UPC_ACHTool\tools\trace_unbound.txt'
OUT_TXT = r'C:\Users\qstli\Downloads\UPC_ACHTool\tools\trace_unbound_addrs.txt'
# Dirs scanned for a DLL that exports each name (resolvability check).
DLL_DIRS = [r'C:\WDL_E3\bin',
            r'C:\Users\qstli\Downloads\duniabackup',
            r'C:\Users\qstli\Downloads\UPC_ACHTool\WDLE3Hook',
            r'C:\Users\qstli\Downloads\UPC_ACHTool\WDLHook']


def load_pe(path):
    return open(path, 'rb').read()


def sections(f):
    pe = struct.unpack_from('<I', f, 0x3C)[0]
    nsec = struct.unpack_from('<H', f, pe + 6)[0]
    optsz = struct.unpack_from('<H', f, pe + 20)[0]
    base = pe + 24 + optsz
    secs = []
    for k in range(nsec):
        o = base + k * 40
        nm = f[o:o + 8].rstrip(b'\x00').decode('latin1')
        vsz, va, rsz, rp = struct.unpack_from('<IIII', f, o + 8)
        secs.append((nm, va, vsz, rp, rsz))
    return secs


def make_r2o(secs):
    def r2o(rva):
        for nm, va, vsz, rp, rsz in secs:
            if va <= rva < va + max(vsz, rsz):
                return rp + (rva - va)
        return None
    return r2o


def read_name(f, r2o, recrva):
    """Interpret recrva as an IMAGE_IMPORT_BY_NAME record: 2-byte hint + ASCII name."""
    o = r2o(recrva)
    if o is None or o + 2 >= len(f):
        return None
    j = o + 2
    while j < len(f) and f[j] != 0 and 32 <= f[j] < 127 and j - (o + 2) < 128:
        j += 1
    if j >= len(f) or f[j] != 0:
        return None
    nm = f[o + 2:j]
    return nm.decode('latin1') if len(nm) >= 2 else None


def exports(path):
    try:
        g = load_pe(path)
    except OSError:
        return []
    if g[:2] != b'MZ':
        return []
    p = struct.unpack_from('<I', g, 0x3C)[0]
    if g[p:p + 4] != b'PE\x00\x00':
        return []
    opt = p + 24
    mag = struct.unpack_from('<H', g, opt)[0]
    dd = opt + (112 if mag == 0x20b else 96)
    er, es = struct.unpack_from('<II', g, dd)
    if not er:
        return []
    secs = sections(g)
    rr = make_r2o(secs)
    eo = rr(er)
    if eo is None:
        return []
    nn = struct.unpack_from('<I', g, eo + 24)[0]
    nr = struct.unpack_from('<I', g, eo + 32)[0]
    no = rr(nr)
    out = []
    for i in range(nn):
        x = struct.unpack_from('<I', g, no + i * 4)[0]
        po = rr(x)
        if po:
            e = g.find(b'\x00', po)
            out.append(g[po:e].decode('latin1', 'replace'))
    return out


def parse_dead_names(path):
    """Pull the plain + mangled names listed under the DEAD section of trace_unbound.txt."""
    names = set()
    grabbing = False
    for line in open(path, encoding='latin1'):
        if '--- DEAD' in line:
            grabbing = True
            continue
        if grabbing:
            s = line.strip()
            if not s or s.endswith('):'):        # skip 'mangled C++ (0):' / 'plain (98):' headers
                continue
            if s.startswith('---'):
                break
            names.add(s.split()[0])
    return names


def main():
    f = load_pe(GAME)
    secs = sections(f)
    r2o = make_r2o(secs)
    trBeg = trEnd = 0
    for nm, va, vsz, rp, rsz in secs:
        if nm == '.trace':
            trBeg, trEnd = va, va + max(vsz, rsz)

    dead = parse_dead_names(UNBOUND_TXT)

    # Map every by-name slot -> record; keep those whose name is in the dead set.
    rec_slots = {}   # recrva -> [slot rvas]
    for a in range(trBeg, trEnd, 8):
        v = struct.unpack_from('<Q', f, r2o(a))[0]
        if trBeg <= v < trEnd:
            nm = read_name(f, r2o, v)
            if nm in dead:
                rec_slots.setdefault(v, []).append(a)

    # Resolvability: name -> first DLL that exports it.
    name2dll = {}
    for d in DLL_DIRS:
        if not os.path.isdir(d):
            continue
        for p in glob.glob(os.path.join(d, '*.dll')):
            for e in exports(p):
                name2dll.setdefault(e, os.path.basename(p))

    entries = []
    for recrva, slist in rec_slots.items():
        nm = read_name(f, r2o, recrva)
        entries.append((nm, recrva, sorted(slist), name2dll.get(nm, '')))
    entries.sort(key=lambda x: (x[3] == '', x[0].lower()))   # resolvable first, then alpha

    with open(OUT_TXT, 'w', encoding='latin1') as out:
        def w(s):
            print(s)
            print(s, file=out)
        w('UNBOUND .trace imports WITH addresses -- %d records, %d slots' %
          (len(entries), sum(len(e[2]) for e in entries)))
        w('cols: NAME  record_rva  [slot_rvas]  resolvable_in_dll')
        w('record_rva = value an unbound slot holds; calling it faults (cf. GetApi @ 0xA97DF14).')
        w('sorted: resolvable-by-a-DLL first, then genuinely dead.')
        w('=' * 100)
        for nm, rec, sl, dll in entries:
            w('%-44s 0x%07X  [%s]  %s' %
              (nm, rec, ' '.join('0x%X' % s for s in sl), dll))
    print('\n-> %s' % OUT_TXT)


if __name__ == '__main__':
    main()
