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
    army = img.vtable('.?AVCArmy@@')
    ok = bool(army) and len(army[1]) > 39 and all(img.match_at(army[1][i], codes['kCodeArmyRatio']) for i in (34, 39))
    results.append(('kCodeArmyRatio (CArmy slots 34, 39)', ok, hex(army[0]) if army else '-'))
    ok = (bool(army) and len(army[1]) > 35 and img.match_at(army[1][35], codes['kCodeArmyFightingStrength']) and
          struct.unpack_from('<i', img.d, img.va2off(army[1][35]) + 0x1a)[0] == 34 * 8)
    results.append(('kCodeArmyFightingStrength (CArmy slot 35 -> slot 34)', ok, hex(army[0]) if army else '-'))
    avail = img.vtable('.?AVCNumOfAvailableMilitaryFactoriesTrigger@@')
    results.append(('kCodeAvailableMilitaryFactories (slot 23)',
                    bool(avail) and img.match_at(avail[1][23], codes['kCodeAvailableMilitaryFactories']), '-'))
    mil = img.vtable('.?AVCMilitaryProductionLine@@')
    nav = img.vtable('.?AVCNavalProductionLine@@')
    results.append(('kCodeLineEquipment (military line slot 11)',
                    bool(mil) and img.match_at(mil[1][11], codes['kCodeLineEquipment']), '-'))
    results.append(('kCodeNavalLineEquipment (naval line slot 11)',
                    bool(nav) and img.match_at(nav[1][11], codes['kCodeNavalLineEquipment']), '-'))
    text_va, text = cs.text_section(img.d)
    refs, _ = cs.anchor_refs(img.d, text_va, text, 'production.daily_serial')
    ok = bool(refs) and len(refs) == 1 and img.match_at(refs[0] + 0x13, codes['kCodeProductionDaily'])
    results.append(('kCodeProductionDaily (lea + 0x13)', ok, hex(refs[0]) if refs else '-'))
    # kCodeArmyDailyLoop right after the "army.daily" anchor's lea (+0x14).
    text_va, text = cs.text_section(img.d)
    refs, _ = cs.anchor_refs(img.d, text_va, text, 'army.daily')
    ok = bool(refs) and len(refs) == 1 and img.match_at(refs[0] + 0x14, codes['kCodeArmyDailyLoop'])
    results.append(('kCodeArmyDailyLoop (army.daily lea + 0x14)', ok, hex(refs[0]) if refs else '-'))
    results += check_region_scope(img, codes, text_va, text)
    results.append(check_trigger_registry(img, codes, text_va, text))
    results += check_create_army(img, codes, text_va, text)
    return results


def find_in(img, va, length, pattern):
    """First match of pattern in [va, va+length), as engine SigFindIn; None if none."""
    off = img.va2off(va)
    m = cs.compile_pattern(pattern).search(img.d, off, off + length)
    return va + (m.start() - off) if m else None


def i32(img, va):
    return struct.unpack_from('<i', img.d, img.va2off(va))[0]


def check_create_army(img, codes, text_va, text):
    """create_army (NOTES.md G1): the same pieces engine_army.cpp resolves, with its cross-checks."""
    results = []
    sigs, _ = cs.load_signatures(pathlib.Path(__file__).parent.parent / 'src' / 'signatures.h')
    sigs = dict(sigs)
    mgr = [m.start() for m in cs.compile_pattern(sigs['kSigBorderWarManager']).finditer(text)]
    army = [m.start() for m in cs.compile_pattern(sigs['kSigBorderWarArmy']).finditer(text)]
    if len(mgr) != 1 or len(army) != 1:
        return [('create_army border war signatures', False, '%d/%d matches' % (len(mgr), len(army)))]
    mgr_disp = i32(img, text_va + mgr[0] + 3)
    army_va = text_va + army[0]
    create = rel32(img, army_va + 0x1b)
    sl = find_in(img, army_va + 0x1b, 0x40, codes['kCodeBorderWarSetLeader'])
    set_leader = rel32(img, sl + 9) if sl else 0
    refresh = i32(img, sl + 0x16) if sl else -1
    results.append(('kCodeBorderWarSetLeader (manager +0x%x, refresh slot +0x%x)' % (mgr_disp, refresh), bool(sl), hex(set_leader)))

    order = img.vtable('.?AVCOrderGroupCommand@@')
    ok = bool(order) and img.match_at(order[1][10], codes['kCodeOrderGroupExecute'])
    resolve = rel32(img, order[1][10] + 0x31) if ok else 0
    oc = find_in(img, order[1][10], 0x100, codes['kCodeOrderGroupCreate']) if ok else None
    ok = ok and bool(oc) and rel32(img, oc + 0x11) == create
    results.append(('kCodeOrderGroupExecute / Create (same create_army)', ok, hex(create)))

    setcmd = img.vtable('.?AVCSetArmyLeaderCommand@@')
    ok = bool(setcmd) and img.match_at(setcmd[1][9], codes['kCodeSetLeaderCheck'])
    ok = ok and rel32(img, setcmd[1][9] + 0x2a) == resolve
    can_change = rel32(img, setcmd[1][9] + 0x6d) if ok else 0
    ok = ok and calls(img, setcmd[1][10], 0x300, set_leader)
    results.append(('kCodeSetLeaderCheck (resolver, can change; execute calls set_leader)', ok, hex(can_change)))

    ok = bool(set_leader) and img.match_at(set_leader, codes['kCodeSetLeaderStart'])
    name = img.d[img.va2off(set_leader + 0x51)] if ok else -1
    cap = img.d[img.va2off(set_leader + 0x4c)] if ok else -1
    lead = i32(img, set_leader + 0x76) if ok else -1
    results.append(('kCodeSetLeaderStart (name +0x%x, army leader +0x%x)' % (name, lead), ok and cap == name + 0x18, hex(set_leader)))

    isarmy = img.vtable('.?AVCIsArmyLeader@@')
    ok = bool(isarmy) and img.match_at(isarmy[1][22], codes['kCodeIsArmyLeaderEval'])
    role_fn = rel32(img, isarmy[1][22] + 0x34) if ok else 0
    role_type = i32(img, isarmy[1][22] + 0x48) if ok else -1
    results.append(('kCodeIsArmyLeaderEval (role type +0x%x)' % role_type, ok, hex(role_fn)))

    leading = img.vtable('.?AVCIsLeadingArmy@@')
    ev = leading[1][22] if leading else 0
    ok = bool(leading) and img.match_at(ev, codes['kCodeIsLeadingArmyEval'])
    ok = ok and i32(img, ev + 0x18) == role_type and rel32(img, ev + 0x3e) == resolve
    led = i32(img, ev + 0x27) if ok else -1
    ag = img.d[img.va2off(ev + 0x61)] if ok else -1
    results.append(('kCodeIsLeadingArmyEval (led group +0x%x, army group flag +0x%x)' % (led, ag), ok, hex(ev)))

    # can_change_leader's refusal fields (only for create_army's reason text; not fatal there).
    ok = bool(can_change) and img.match_at(can_change, codes['kCodeCanChangeStart'])
    dep = find_in(img, can_change + 0x40, 0x100, codes['kCodeCanChangeDeploying']) if ok else None
    wd = find_in(img, dep + 0x11, 0xc0, codes['kCodeCanChangeWithdrawing']) if dep else None
    war = i32(img, can_change + 0x2d) if ok else -1
    d1, d2 = (i32(img, dep + 2), i32(img, dep + 0xb)) if dep else (-1, -1)
    w = i32(img, wd + 2) if wd else -1
    ok = bool(wd) and img.d[img.va2off(can_change + 0x1c)] == ag and i32(img, can_change + 0x3b) == lead
    ok = ok and 0x40 < war < 0x230 and len({d1, d2, w}) == 3 and max(d1, d2, w) < 0x230
    results.append(('kCodeCanChangeStart / Deploying / Withdrawing (border war +0x%x, deploying +0x%x/+0x%x, '
                    'withdrawing +0x%x)' % (war, d1, d2, w), ok, hex(can_change)))

    cap_vt = img.vtable('.?AVCIsGeneralCapturedTrigger@@')
    ok = bool(cap_vt) and img.match_at(cap_vt[1][22], codes['kCodeIsCapturedEval'])
    slot = i32(img, cap_vt[1][22] + 0x27) if ok else -1
    results.append(('kCodeIsCapturedEval (captured slot +0x%x)' % slot, ok, hex(cap_vt[1][22]) if cap_vt else '-'))

    rec = img.vtable('.?AVCRecruitCharacterEffect@@')
    look = find_in(img, rec[1][13], 0x200, codes['kCodeCharacterLookup']) if rec else None
    chk = find_in(img, rec[1][13], 0x400, codes['kCodeCharacterOwnerCheck']) if rec else None
    getter = rel32(img, chk + 3) if chk else 0
    ok = bool(look and chk) and img.match_at(getter, codes['kCodeCharacterOwnerGetter'])
    mgr_off = i32(img, look + 0xe) if look else -1
    owner = img.d[img.va2off(getter + 3)] if ok else -1
    results.append(('kCodeCharacterLookup / OwnerCheck (characters +0x%x, owner +0x%x)' % (mgr_off, owner), ok,
                    hex(rel32(img, look + 0x12)) if look else '-'))

    # Characters of a country by token (has_character's lookup; works after loading a save).
    has = img.vtable('.?AVCHasCharacterTrigger@@')
    ok = bool(has) and img.match_at(has[1][22], codes['kCodeHasCharacterEval'])
    lookup = None
    if ok:
        target = rel32(img, has[1][22] + 0x38)
        for i in range(0x9b - 5):
            if img.d[img.va2off(target + i)] == 0xe8:
                lookup = find_in(img, rel32(img, target + i), 0x100, codes['kCodeCountryCharacterLookup'])
                if lookup:
                    break
    results.append(('kCodeHasCharacterEval / CountryCharacterLookup', bool(lookup),
                    '%s %s' % (hex(rel32(img, lookup + 0x10)), hex(rel32(img, lookup + 0x1f))) if lookup else '-'))

    ren = img.vtable('.?AVCSetOrderGroupNameCommand@@')
    ok = bool(ren) and img.match_at(ren[1][10], codes['kCodeSetArmyName']) and rel32(img, ren[1][10] + 0x1c) == resolve
    disp = i32(img, ren[1][10] + 0x3c) if ok else -1
    results.append(('kCodeSetArmyName (name +0x%x)' % disp, ok, hex(rel32(img, ren[1][10] + 0x58)) if ok else '-'))
    return results


def check_trigger_registry(img, codes, text_va, text):
    """The trigger registry's map offset, read from the trigger Register (has_global_flag stub's jmp)."""
    _, anchors = cs.load_signatures(pathlib.Path(__file__).parent.parent / 'src' / 'signatures.h')
    refs, _ = cs.anchor_refs(img.d, text_va, text, dict(anchors)['kAnchorGlobalFlagTriggerHelp'])
    if not refs or len(refs) != 1 or img.d[img.va2off(refs[0] + 0x29)] != 0xE9:
        return ('kCodeTriggerRegisterMap (trigger Register + 0x1f)', False, 'has_global_flag stub not found')
    reg = rel32(img, refs[0] + 0x29)
    ok = img.match_at(reg + 0x1f, codes['kCodeTriggerRegisterMap'])
    disp = img.d[img.va2off(reg + 0x22)] if ok else -1
    return ('kCodeTriggerRegisterMap (trigger Register + 0x1f)', ok and disp == 0x18, '%s map +0x%x' % (hex(reg), disp))


def rel32(img, insn):
    """Target of the rel32 at insn+1 (call / jmp)."""
    return insn + 5 + struct.unpack_from('<i', img.d, img.va2off(insn) + 1)[0]


def decode_key_chain(img, va, factory):
    """The vanilla item handler's compare chain, as engine_scope.cpp DecodeKeyChain reads it:
    (keys, {key: handler}) or None."""
    keys, handlers, p = [], {}, va
    for _ in range(48):
        b = img.d[img.va2off(p):img.va2off(p) + 8]
        if b[:3] in (b'\x48\x8b\xc2', b'\x48\x8b\xda'):
            p += 3
        elif b[:2] == b'\x40\x53':
            p += 2
        elif b[:3] == b'\x48\x83\xec':
            p += 4
        elif b[:3] == b'\x41\x81\xf8':
            key = struct.unpack_from('<I', b, 3)[0]
            p += 7
            b = img.d[img.va2off(p):img.va2off(p) + 6]
            if b[0] == 0x74:
                handlers[key] = p + 2 + struct.unpack_from('<b', b, 1)[0]
                p += 2
            elif b[:2] == b'\x0f\x84':
                handlers[key] = p + 6 + struct.unpack_from('<i', b, 2)[0]
                p += 6
            elif b[0] == 0x75:
                handlers[key] = p + 2
                p = p + 2 + struct.unpack_from('<b', b, 1)[0]
            elif b[:2] == b'\x0f\x85':
                handlers[key] = p + 6
                p = p + 6 + struct.unpack_from('<i', b, 2)[0]
            else:
                return None
            keys.append(key)
        elif b[:2] == b'\x41\xb9':
            if struct.unpack_from('<I', b, 2)[0] != 2:
                return None
            p += 6
            b = img.d[img.va2off(p):img.va2off(p) + 8]
            if b[:3] == b'\x48\x83\xc4':
                p += 4
                b = img.d[img.va2off(p):img.va2off(p) + 8]
            if b[:1] == b'\x5b':
                p += 1
                b = img.d[img.va2off(p):img.va2off(p) + 8]
            return (keys, handlers) if b[0] == 0xe9 and rel32(img, p) == factory and keys else None
        else:
            return None
    return None


def calls(img, va, length, target):
    d, o = img.d, img.va2off(va)
    return any(d[o + i] == 0xe8 and rel32(img, va + i) == target for i in range(length - 5))


def check_region_scope(img, codes, text_va, text):
    """region_scope (NOTES.md F5): the vanilla state iterators the region iterators are cloned from."""
    results = []
    trig, eff = img.vtable('.?AVCTrigger@@')[1], img.vtable('.?AVCEffect@@')[1]
    any_vt = img.vtable('.?AVCAnyStateTrigger@@')
    every_vt = img.vtable('.?AVCEveryStateEffect@@')
    random_vt = img.vtable('.?AVCRandomStateEffect@@')
    entries = [img.vtable('.?AV?$CTriggerEntry@VCAnyStateTrigger@@@@'), img.vtable('.?AV?$CEffectEntry@VCEveryStateEffect@@@@'),
               img.vtable('.?AV?$CEffectEntry@VCRandomStateEffect@@@@'), img.vtable('.?AVCAndTrigger@@')]
    if not (any_vt and every_vt and random_vt and all(entries)):
        return [('region_scope rtti', False, 'an iterator class or entry vtable is missing')]
    for name, vt, slot, factory, limit in (('any_state', any_vt, 6, trig[6], False), ('every_state', every_vt, 4, eff[4], True),
                                           ('random_state', random_vt, 4, eff[4], True)):
        chain = decode_key_chain(img, vt[1][slot], factory)
        ok = chain is not None
        where = 'keys ' + ','.join(hex(k) for k in chain[0]) if chain else hex(vt[1][slot])
        if ok and limit:
            lim = [k for k, h in chain[1].items() if img.match_at(h, codes['kCodeTargetEffectLimit'])]
            ok = len(lim) == 1 and len(vt[1]) >= 29 and img.match_at(vt[1][13], codes['kCodeTargetEffectExecute'])
            where += ' limit ' + (hex(lim[0]) if len(lim) == 1 else '?')
        results.append(('%s item keys / limit / execute' % name, ok, where))
    ev = any_vt[1][22]
    ok = img.match_at(ev, codes['kCodeAnyStateEvaluate'])
    copy = rel32(img, ev + 0x26) if ok else 0
    ok = ok and img.match_at(copy, '48 89 4C 24 08 53 48 83 EC 20 48 8B D9 48 8D 05 ?? ?? ?? ?? 48 89 01 33 C9 89 4B 08 '
                                   'C7 43 0C 01 00 00 00 C7 43 10 9E BA A6 5E')
    # The scope destructor (player_scope's) and the script value evaluation (CIntTrigger's).
    sigs, _ = cs.load_signatures(pathlib.Path(__file__).parent.parent / 'src' / 'signatures.h')
    ps = [m.start() for m in cs.compile_pattern(dict(sigs)['kSigPlayerScope']).finditer(text)]
    dtor = rel32(img, text_va + ps[0] + 0x45) if len(ps) == 1 else 0
    num = img.vtable('.?AVCNumOfFactoriesTrigger@@')[1][22]
    sve = rel32(img, num + 0x30)
    ok = bool(ok and dtor and calls(img, ev, 0x400, dtor) and calls(img, ev, 0x400, sve))
    results.append(('kCodeAnyStateEvaluate (copy ctor, dtor, script value)', ok, hex(copy)))
    dt = every_vt[1][0]
    ok = img.match_at(dt, codes['kCodeScalarDeletingDtor'])
    results.append(('kCodeScalarDeletingDtor (every_state slot 0)', ok, hex(rel32(img, dt + 0x21)) if ok else hex(dt)))
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
