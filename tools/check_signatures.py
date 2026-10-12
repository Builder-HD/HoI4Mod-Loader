"""Check every SIG(...) in src/signatures.h against one or more hoi4.exe copies.

Usage:  python tools/check_signatures.py <hoi4.exe copy> [<another copy> ...]

Each signature must match exactly once in the executable's .text section, the same rule the
loader applies at runtime. Run it on COPIES of the executable. Exit code 1 if any signature
fails in any file.
"""
import hashlib
import pathlib
import re
import struct
import sys

SIG_LINE = re.compile(r'^\s*SIG\(\s*(\w+)\s*,\s*"([0-9A-Fa-f? ]+)"\s*\)')


def load_signatures(header):
    sigs = []
    for line in header.read_text(encoding='utf-8').splitlines():
        m = SIG_LINE.match(line)
        if m:
            sigs.append((m.group(1), m.group(2)))
    return sigs


def text_section(data):
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    optsz = struct.unpack_from('<H', data, pe + 20)[0]
    base = struct.unpack_from('<Q', data, pe + 24 + 24)[0]
    for i in range(nsec):
        o = pe + 24 + optsz + i * 40
        if data[o:o + 8].rstrip(b'\0') == b'.text':
            vs, va, rs, ra = struct.unpack_from('<IIII', data, o + 8)
            return base + va, data[ra:ra + rs]
    raise ValueError('no .text section')


def compile_pattern(pattern):
    parts = []
    for tok in pattern.split():
        parts.append(b'.' if tok in ('?', '??') else re.escape(bytes([int(tok, 16)])))
    return re.compile(b''.join(parts), re.S)


def build_string(data):
    m = re.search(rb'v1\.\d+\.\d+\.\d+\.[0-9a-f]{4}', data)
    return m.group(0).decode() if m else 'unknown version'


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    header = pathlib.Path(__file__).resolve().parent.parent / 'src' / 'signatures.h'
    sigs = load_signatures(header)
    if not sigs:
        print('no SIG(...) lines found in', header)
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
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
