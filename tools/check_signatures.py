"""Check every SIG(...) and ANCHOR(...) in src/signatures.h against one or more hoi4.exe copies.

Usage:  python tools/check_signatures.py <hoi4.exe copy> [<another copy> ...]

SIG: the pattern must match exactly once in the executable's .text section, the same rule the
loader applies at runtime. ANCHOR: the NUL-terminated string must occur exactly once and be
referenced by exactly one RIP-relative `lea` in .text. Run it on COPIES of the executable.
Exit code 1 if anything fails in any file.
"""
import hashlib
import pathlib
import re
import struct
import sys

SIG_LINE = re.compile(r'^\s*SIG\(\s*(\w+)\s*,\s*"([0-9A-Fa-f? ]+)"\s*\)')
ANCHOR_LINE = re.compile(r'^\s*ANCHOR\(\s*(\w+)\s*,\s*"([^"]*)"\s*\)')
LEA_RIP = re.compile(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]')


def load_signatures(header):
    sigs, anchors = [], []
    for line in header.read_text(encoding='utf-8').splitlines():
        m = SIG_LINE.match(line)
        if m:
            sigs.append((m.group(1), m.group(2)))
        m = ANCHOR_LINE.match(line)
        if m:
            anchors.append((m.group(1), m.group(2)))
    return sigs, anchors


def pe_sections(data):
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    optsz = struct.unpack_from('<H', data, pe + 20)[0]
    base = struct.unpack_from('<Q', data, pe + 24 + 24)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + optsz + i * 40
        vs, va, rs, ra = struct.unpack_from('<IIII', data, o + 8)
        secs.append((data[o:o + 8].rstrip(b'\0'), va, rs, ra))
    return base, secs


def text_section(data):
    base, secs = pe_sections(data)
    for name, va, rs, ra in secs:
        if name == b'.text':
            return base + va, data[ra:ra + rs]
    raise ValueError('no .text section')


def compile_pattern(pattern):
    parts = []
    for tok in pattern.split():
        parts.append(b'.' if tok in ('?', '??') else re.escape(bytes([int(tok, 16)])))
    return re.compile(b''.join(parts), re.S)


def anchor_refs(data, text_va, text, s):
    """(refs, string_count): VAs of `lea r64,[rip+x]` in .text targeting the NUL-terminated s."""
    base, secs = pe_sections(data)
    needle = b'\0' + s.encode('latin-1') + b'\0'
    hits = [m.start() + 1 for m in re.finditer(re.escape(needle), data)]
    if len(hits) != 1:
        return None, len(hits)
    off = hits[0]
    target = next(base + va + off - ra for _, va, rs, ra in secs if ra <= off < ra + rs)
    refs = []
    for m in LEA_RIP.finditer(text):
        p = m.start()
        rel = struct.unpack_from('<i', text, p + 3)[0]
        if text_va + p + 7 + rel == target:
            refs.append(text_va + p)
    return refs, 1


def build_string(data):
    m = re.search(rb'v1\.\d+\.\d+\.\d+\.[0-9a-f]{4}', data)
    return m.group(0).decode() if m else 'unknown version'


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    header = pathlib.Path(__file__).resolve().parent.parent / 'src' / 'signatures.h'
    sigs, anchors = load_signatures(header)
    if not sigs and not anchors:
        print('no SIG(...) / ANCHOR(...) lines found in', header)
        return 2
    failed = False
    for exe in sys.argv[1:]:
        data = pathlib.Path(exe).read_bytes()
        va, text = text_section(data)
        print('%s  [%s, sha256 %s]' % (exe, build_string(data), hashlib.sha256(data).hexdigest()))
        for name, pattern in sigs:
            hits = [va + m.start() for m in compile_pattern(pattern).finditer(text)]
            ok = len(hits) == 1
            failed |= not ok
            where = ' '.join('0x%x' % h for h in hits[:4]) or '-'
            print('  %-4s %-28s %d match(es)  %s' % ('ok' if ok else 'FAIL', name, len(hits), where))
        for name, s in anchors:
            refs, nstr = anchor_refs(data, va, text, s)
            if refs is None:
                failed = True
                print('  FAIL %-28s string found %d time(s), expected 1' % (name, nstr))
                continue
            ok = len(refs) == 1
            failed |= not ok
            where = ' '.join('0x%x' % r for r in refs[:4]) or '-'
            print('  %-4s %-28s %d lea ref(s)  %s' % ('ok' if ok else 'FAIL', name, len(refs), where))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
