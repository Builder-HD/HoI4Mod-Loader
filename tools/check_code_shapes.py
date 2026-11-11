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
    # Railway construction lines (railway_queue): the output getter (the slot kSigRailLineDailyOutput
    # calls; 14 on the checked build), finished slot 25, cancel slot 27 (the loader takes the cancel
    # slot from kSigRailLineDailyHead; 27 on the checked build).
    rail = img.vtable('.?AVCRailwayProductionLine@@')
    sigs, _ = cs.load_signatures(pathlib.Path(__file__).parent.parent / 'src' / 'signatures.h')
    out = [text_va + m.start() for m in cs.compile_pattern(dict(sigs)['kSigRailLineDailyOutput']).finditer(text)]
    out_slot = img.d[img.va2off(out[0] + 0xf)] // 8 if len(out) == 1 else 14
    for slot, code in ((out_slot, 'kCodeRailLineOutput'), (25, 'kCodeRailLineFinished'), (27, 'kCodeRailLineCancel')):
        ok = bool(rail) and len(rail[1]) > slot and img.match_at(rail[1][slot], codes[code])
        results.append(('%s (railway line slot %d)' % (code, slot), ok, hex(rail[1][slot]) if ok else '-'))
    results += check_create_army(img, codes, text_va, text)
    results += check_add_to_army(img, codes, text_va, text)
    results += check_time_control(img, codes, text_va, text)
    return results


def check_time_control(img, codes, text_va, text):
    """time_control (NOTES.md Part K): the pieces engine_time.cpp resolves, with its cross-checks."""
    sigs, _ = cs.load_signatures(pathlib.Path(__file__).parent.parent / 'src' / 'signatures.h')
    sigs = dict(sigs)
    at = {}
    for name in ('kSigConsoleSpeedPause', 'kSigConsoleSpeedSet', 'kSigSetGameSpeed', 'kSigIncreaseSpeedTail',
                 'kSigTopBarUpdateDate', 'kSigTopBarPauseClick', 'kSigTopBarHideWindow'):
        m = [x.start() for x in cs.compile_pattern(sigs[name]).finditer(text)]
        if len(m) != 1:
            return [('time_control signature %s' % name, False, '%d matches' % len(m))]
        at[name] = text_va + m[0]
    cp, cset, ss, inc = (at['kSigConsoleSpeedPause'], at['kSigConsoleSpeedSet'], at['kSigSetGameSpeed'],
                         at['kSigIncreaseSpeedTail'])
    ud, pc = at['kSigTopBarUpdateDate'], at['kSigTopBarPauseClick']

    def u8(va):
        return img.d[img.va2off(va)]

    def rip(insn):
        return insn + 7 + i32(img, insn + 3)

    results = []
    idlers = {rip(cp + 0x13), rip(cp + 0x2f), rip(cset + 4), rip(cset + 0x22)}
    toggle, paused = i32(img, cp + 0x40), i32(img, cp + 0x1f)
    speeds, top = u8(cp + 0xc), i32(img, ss + 0x14)
    field = i32(img, ss + 0x1f)
    request = rel32(img, cset + 0x29)
    ok = (len(idlers) == 1 and toggle == i32(img, cset + 0x15) == i32(img, pc + 0x17) and top == speeds - 1 and
          field == i32(img, inc + 0xc) and rel32(img, inc + 0x27) == ss and img.match_at(request, codes['kCodeGameSpeedRequest']))
    results.append(('time_control speed sites (%d speeds, field +0x%x)' % (speeds, field), ok, hex(min(idlers))))
    idler = img.vtable('.?AVCInGameIdler@@')
    ok = bool(idler) and len(idler[1]) > max(toggle, paused) // 8
    tfn = idler[1][toggle // 8] if ok else 0
    pfn = idler[1][paused // 8] if ok else 0
    ok = (ok and img.match_at(tfn, codes['kCodeIdlerTogglePause']) and img.match_at(pfn, codes['kCodeIdlerIsPaused']) and
          i32(img, request + 0x1d) == i32(img, tfn + 0x1e))
    results.append(('kCodeIdlerTogglePause / IsPaused (slots %d / %d, paused +0x%x)' %
                    (toggle // 8, paused // 8, i32(img, pfn + 3) if ok else -1), ok, hex(tfn)))
    tail = find_in(img, ud, 0x180, codes['kCodeTopBarDateTail'])
    fmt = rel32(img, tail + 0xa) if tail else 0
    ok = bool(tail) and img.match_at(fmt, codes['kCodeGameDateFormat'])
    base = i32(img, fmt + 0x21) if ok else -1
    hours = i32(img, tail + 3) + u8(fmt + 0x1a) if ok else -1
    results.append(('kCodeTopBarDateTail / GameDateFormat (date +0x%x, base %d)' % (hours, base),
                    ok and base > 0 and base % 8760 == 0, hex(fmt)))
    refresh = rel32(img, pc + 0x23)
    hide = find_in(img, refresh, 0x200, codes['kCodeWidgetHide'])
    show = find_in(img, refresh, 0x200, codes['kCodeWidgetShow'])
    ok = bool(hide and show) and u8(show + 2) == u8(hide + 0xb) and u8(show + 3) == (~u8(hide + 0xc)) & 0xff
    results.append(('kCodeWidgetHide / Show (slots %d / %d)' % (i32(img, hide + 5) // 8 if hide else -1,
                                                                u8(show + 0x18) // 8 if show else -1), ok, hex(refresh)))
    m = [x.start() for x in cs.compile_pattern(sigs['kSigTopBarHideWindow']).finditer(text)]
    hw = text_va + m[0] if len(m) == 1 else 0
    ok = bool(hw and hide) and img.match_at(refresh, codes['kCodeTopBarPauseRefresh'])
    if ok:
        name_va = rip(refresh + 0x18)
        name = img.d[img.va2off(name_va):img.va2off(name_va) + 20]
        widget = u8(hw + 3)
        ok = (name == b'time_control_window\0' and u8(refresh + 0x14) == 19 and widget == u8(hw + 7) and
              i32(img, hw + 0xa) == i32(img, hide + 5) and i32(img, hw + 0x10) == widget + u8(hide + 0xb) and
              u8(hw + 0x14) == u8(hide + 0xc) and u8(hw + 0x18) == u8(refresh + 0x2a))
    results.append(('kSigTopBarHideWindow / kCodeTopBarPauseRefresh (find slot %d, widget +0x%x)' %
                    (i32(img, refresh + 0x43) // 8 if ok else -1, u8(hw + 3) if hw else -1), ok, hex(hw)))
    bar = img.vtable('.?AVCTopBar@@')
    ok = bool(bar) and img.match_at(bar[1][0], codes['kCodeScalarDeletingDtor'])
    results.append(('kCodeScalarDeletingDtor (CTopBar slot 0; DateText +0x%x)' % i32(img, ud + 0x10), ok,
                    hex(bar[1][0]) if bar else '-'))
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
    # Up to 1.19.3 one shape from the start; 1.19.4.2 restructured it (kCodeSetLeaderCheck1194 inside).
    check1194 = None
    if setcmd and img.match_at(setcmd[1][9], codes['kCodeSetLeaderCheck']):
        shape, resolve_at, can_at = 'kCodeSetLeaderCheck', setcmd[1][9] + 0x2a, setcmd[1][9] + 0x6d
    else:
        check1194 = find_in(img, setcmd[1][9], 0x200, codes['kCodeSetLeaderCheck1194']) if setcmd else None
        shape = 'kCodeSetLeaderCheck1194'
        resolve_at, can_at = (check1194 + 0x37, check1194 + 0x16) if check1194 else (0, 0)
    ok = bool(resolve_at) and rel32(img, resolve_at) == resolve
    can_change = rel32(img, can_at) if ok else 0
    ok = ok and calls(img, setcmd[1][10], 0x300, set_leader)
    results.append(('%s (resolver, can change; execute calls set_leader)' % shape, ok, hex(can_change)))

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
    if check1194:
        ok = ok and i32(img, check1194 + 0x27) == led  # the restructured check's led-group field
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


def check_add_to_army(img, codes, text_va, text):
    """add_to_army (NOTES.md G4): the same pieces engine_army.cpp ResolveJoin resolves, with its cross-checks."""
    sigs, _ = cs.load_signatures(pathlib.Path(__file__).parent.parent / 'src' / 'signatures.h')
    sigs = dict(sigs)
    army = [m.start() for m in cs.compile_pattern(sigs['kSigBorderWarArmy']).finditer(text)]
    land = [m.start() for m in cs.compile_pattern(sigs['kSigStateLandUnits']).finditer(text)]
    if len(army) != 1 or len(land) != 1:
        return [('add_to_army: create_army / land unit signatures', False, '%d/%d matches' % (len(army), len(land)))]
    create = rel32(img, text_va + army[0] + 0x1b)
    owner = i32(img, text_va + land[0] + 0x38)
    results = []
    ms = find_in(img, create, 0x400, codes['kCodeCreateArmyMove'])
    move = rel32(img, ms + 0x12) if ms else 0
    cs_ = find_in(img, ms, 0x100, codes['kCodeCreateArmyCleanup']) if ms else None
    cleanup = rel32(img, cs_ + 6) if cs_ else 0
    assign = img.vtable('.?AVCOrderAssignCommand@@')
    ok = bool(move and cleanup and assign) and calls(img, assign[1][10], 0x1400, move) and calls(img, assign[1][10], 0x1400, cleanup)
    results.append(('kCodeCreateArmyMove / Cleanup (COrderAssignCommand calls the same)', ok, '%s %s' % (hex(move), hex(cleanup))))

    tag = find_in(img, move, 0x100, codes['kCodeArmyMoveTag']) if move else None
    mem = find_in(img, move, 0x140, codes['kCodeArmyMoveMembers']) if move else None
    ua = find_in(img, move, 0x200, codes['kCodeArmyMoveUnitArmy']) if move else None
    u8 = lambda va: img.d[img.va2off(va)]
    ok = bool(tag and mem and ua) and bool(cleanup) and img.match_at(cleanup, codes['kCodeArmyCleanup'])
    if ok:
        unit_army, lst, cnt = i32(img, ua + 3), u8(mem + 0x24), u8(mem + 0xf)
        ok = (i32(img, tag + 2) == owner and unit_army == i32(img, ua + 0x35) == i32(img, ua + 0x41) and
              lst == u8(ua + 0x25) and cnt == lst + 0xc and cnt == u8(cleanup + 0xb))
        detail = 'unit army +0x%x, entry +0x%x, army tag +0x%x, members +0x%x' % (unit_army, i32(img, mem + 6), u8(tag + 0x17), lst)
    else:
        detail = '-'
    results.append(('kCodeArmyMoveTag / Members / UnitArmy, kCodeArmyCleanup (%s)' % detail, ok, hex(move)))

    destroy = img.vtable('.?AVCDestroyUnitEffect@@')
    getter = rel32(img, destroy[1][13] + 9) if destroy and u8(destroy[1][13] + 9) == 0xe8 else 0
    resolve = rel32(img, img.vtable('.?AVCOrderGroupCommand@@')[1][10] + 0x31)
    ok = bool(getter) and img.match_at(getter, codes['kCodeScopeDivision'])
    ok = ok and u8(getter + 7) == 0x70 and rel32(img, getter + 0x13) == resolve and u8(getter + 0x20) == 0xf0
    results.append(('kCodeScopeDivision (destroy_unit slot 13 + 9; scope +0x70, resolver, -0x10)', ok, hex(getter)))
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
