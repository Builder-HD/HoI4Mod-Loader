"""Check every CODE(...) shape in src/signatures.h at the addresses the loader reaches through RTTI.

Usage:  python tools/check_code_shapes.py <hoi4.exe copy> [<another copy> ...]

CODE patterns are not scanned for uniqueness (check_signatures.py does that for SIG); the loader
matches them at one known address each, found through an RTTI vtable. This tool resolves the same
addresses offline and checks the same patterns, so a new game build can be vetted on a copy.
Run it on COPIES of the executable. Exit code 1 if anything fails in any file.
"""
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import check_signatures as cs  # noqa: E402  (pe_sections, compile_pattern)

CODE_LINE = re.compile(r'^\s*CODE\(\s*(\w+)\s*,\s*"([0-9A-Fa-f? ]+)"\s*\)')


class Image:
    def __init__(self, data):
        self.d = data
        self.base, secs = cs.pe_sections(data)
        self.secs = {name.decode(): (va, rs, ra) for name, va, rs, ra in secs}

    def off2rva(self, off):
        for va, rs, ra in self.secs.values():
            if ra <= off < ra + rs:
                return off - ra + va
        return None

    def va2off(self, va):
        rva = va - self.base
        for sva, rs, ra in self.secs.values():
            if sva <= rva < sva + rs:
                return rva - sva + ra
        return None

    def is_code(self, va):
        tva, trs, _ = self.secs['.text']
        return self.base + tva <= va < self.base + tva + trs

    def vtable(self, name):
        """Complete-object vtable (this-offset 0) of an RTTI class: (address, list of slots)."""
        d = self.d
        pos = d.find(name.encode() + b'\0')
        if pos < 0:
            return None
        td_rva = self.off2rva(pos) - 0x10
        rva, rrs, rra = self.secs['.rdata']
        rdata = d[rra:rra + rrs]
        needle = struct.pack('<I', td_rva)
        i = rdata.find(needle)
        while i >= 0:
            col_off = rra + i - 12
            sig, offset, _, td = struct.unpack_from('<IIII', d, col_off)
            self_rva = struct.unpack_from('<I', d, col_off + 20)[0]
            if sig == 1 and td == td_rva and offset == 0 and self_rva == self.off2rva(col_off):
                col_va = self.base + self.off2rva(col_off)
                p = rdata.find(struct.pack('<Q', col_va))
                if p >= 0:
                    vt_va = self.base + self.off2rva(rra + p) + 8
                    slots, o = [], rra + p + 8
                    while True:
                        (fp,) = struct.unpack_from('<Q', d, o)
                        if not self.is_code(fp):
                            return vt_va, slots
                        slots.append(fp)
                        o += 8
            i = rdata.find(needle, i + 1)
        return None

    def match_at(self, va, pattern):
        off = self.va2off(va)
        if off is None:
            return False
        return cs.compile_pattern(pattern).match(self.d, off) is not None


def load_codes(header):
    codes = {}
    for line in header.read_text(encoding='utf-8').splitlines():
        m = CODE_LINE.match(line)
        if m:
            codes[m.group(1)] = m.group(2)
    return codes


def check(path, codes):
    img = Image(pathlib.Path(path).read_bytes())
    count = img.vtable('.?AVCNumOfFactoriesTrigger@@')
    ratio = img.vtable('.?AVCHasStabilityTrigger@@')
    base = img.vtable('.?AVCTrigger@@')
    entries = [img.vtable('.?AV?$CTriggerEntry@VCNumOfFactoriesTrigger@@@@'),
               img.vtable('.?AV?$CTriggerEntry@VCHasStabilityTrigger@@@@')]
    if not (count and ratio and base and all(entries)):
        return [('rtti', False, 'a template class or entry vtable is missing')]
    results = []
    for name, entry, cls in (('count', entries[0], count), ('ratio', entries[1], ratio)):
        factory = entry[1][1]
        ok = img.match_at(factory, codes['kCodeTriggerFactory'])
        size = 0
        if ok:  # the factory's lea (+0x25, rel32 at +0x28) must build exactly the RTTI class
            lea = factory + 0x25
            rel = struct.unpack_from('<i', img.d, img.va2off(lea) + 3)[0]
            ok = lea + 7 + rel == cls[0]
            size = struct.unpack_from('<I', img.d, img.va2off(factory) + 7)[0]
        results.append(('kCodeTriggerFactory (%s, size 0x%x)' % (name, size), ok, hex(factory)))
    base, count, ratio = base[1], count[1], ratio[1]
    results.append(('kCodeIntCompare (count slot 22)', img.match_at(count[22], codes['kCodeIntCompare']), hex(count[22])))
    results.append(('kCodeNumericReadClause (slot 5, shared)',
                    img.match_at(count[5], codes['kCodeNumericReadClause']) and count[5] == ratio[5], hex(count[5])))
    results.append(('kCodeValueReadValue (ratio slot 4)', img.match_at(ratio[4], codes['kCodeValueReadValue']), hex(ratio[4])))
    block = any(img.match_at(base[5] + i, codes['kCodeBlockValueCheck']) for i in range(0x80))
    results.append(('kCodeBlockValueCheck (CTrigger slot 5)', block, hex(base[5])))
    results.append(('slot counts (count >= 24, ratio >= 26)', len(count) >= 24 and len(ratio) >= 26,
                    '%d/%d' % (len(count), len(ratio))))
    # kCodeArmyDailyLoop right after the "army.daily" anchor's lea (+0x14).
    text_va, text = cs.text_section(img.d)
    refs, _ = cs.anchor_refs(img.d, text_va, text, 'army.daily')
    ok = bool(refs) and len(refs) == 1 and img.match_at(refs[0] + 0x14, codes['kCodeArmyDailyLoop'])
    results.append(('kCodeArmyDailyLoop (army.daily lea + 0x14)', ok, hex(refs[0]) if refs else '-'))
    return results


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    codes = load_codes(pathlib.Path(__file__).parent.parent / 'src' / 'signatures.h')
    rc = 0
    for path in sys.argv[1:]:
        print(path)
        for name, ok, where in check(path, codes):
            print('  %-4s %-45s %s' % ('ok' if ok else 'FAIL', name, where))
            rc |= not ok
    return rc


if __name__ == '__main__':
    sys.exit(main())
