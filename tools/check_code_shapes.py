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


CONST_ITEM = re.compile(r'\b(k\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\b')


def load_constants(header):
    """The numeric `static const ... kName = value, ...;` constants of signatures.h, by name, so the
    offsets here come from the same place the loader reads them."""
    consts, stmt = {}, ''
    for line in header.read_text(encoding='utf-8').splitlines():
        if re.match(r'^\s*static const ', line) or stmt:
            stmt += line
            if ';' in line:
                for name, value in CONST_ITEM.findall(stmt):
                    consts[name] = int(value, 0)
                stmt = ''
    return consts


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
    # Province building removal: the item's destructor (slot 0) and, inside the update the
    # signature finds, the picture's enable test.
    # Offsets, ranges and slots as the resolver uses them (engine_province_buildings.cpp).
    k = load_constants(pathlib.Path(__file__).parent.parent / 'src' / 'signatures.h')
    pb = img.vtable('.?AVCProvinceBuildingItem@@')
    ok = bool(pb) and img.match_at(pb[1][0], codes['kCodePbDtor'])
    results.append(('kCodePbDtor (province building item slot 0)', ok, hex(pb[1][0]) if pb else '-'))
    sigs, _ = cs.load_signatures(pathlib.Path(__file__).parent.parent / 'src' / 'signatures.h')
    upd = [text_va + m.start() for m in cs.compile_pattern(dict(sigs)['kSigPbUpdate']).finditer(text)]
    rng = k['kCodePbUpdateEnableRange']
    ok = len(upd) == 1 and any(img.match_at(upd[0] + i, codes['kCodePbUpdateEnable']) for i in range(rng))
    results.append(('kCodePbUpdateEnable (in kSigPbUpdate + 0x%x)' % rng, ok, hex(upd[0]) if upd else '-'))
    # The confirmation dialog: constructor (from kSigConfirmRemoveOpen's call), RTTI vtable slots
    # (dtor, valid, accept), and the state id field the constructor writes read by valid and accept.
    opn = [text_va + m.start() for m in cs.compile_pattern(dict(sigs)['kSigConfirmRemoveOpen']).finditer(text)]
    dlg = img.vtable('.?AVCConfirmRemoveBuildingLevel@@')
    ctor = rel32(img, opn[0] + k['kSigConfirmCtorCall']) if len(opn) == 1 else 0
    ok = bool(ctor) and img.match_at(ctor, codes['kCodeConfirmCtor'])
    state = struct.unpack_from('<I', img.d, img.va2off(ctor + k['kCodeConfirmStateDisp']))[0] if ok else -1
    lea = ctor + k['kCodeConfirmCtorVtableLea']
    ok = ok and bool(dlg) and lea + 7 + struct.unpack_from('<i', img.d, img.va2off(lea + 3))[0] == dlg[0]
    results.append(('kCodeConfirmCtor (state id +0x%x, vtable)' % state, ok, hex(ctor)))

    def reads_state(fn):
        for i in range(k['kCodeConfirmReadRange']):
            if img.match_at(fn + i, codes['kCodeConfirmReadState']) and \
                    struct.unpack_from('<I', img.d, img.va2off(fn + i + 3))[0] == state:
                return True
        return False

    for slot, code, read in ((k['kConfirmDtorSlot'], 'kCodeConfirmDtor', False),
                             (k['kConfirmValidSlot'], 'kCodeConfirmValid', True),
                             (k['kConfirmAcceptSlot'], 'kCodeConfirmAccept', True)):
        fn = dlg[1][slot] if dlg and len(dlg[1]) > slot else 0
        ok = bool(fn) and img.match_at(fn, codes[code]) and (not read or reads_state(fn))
        results.append(('%s (confirm dialog slot %d%s)' % (code, slot, ', reads the state id' if read else ''), ok,
                        hex(fn)))
    results += check_create_army(img, codes, text_va, text)
    results += check_add_to_army(img, codes, text_va, text)
    results += check_air_scenarios(img, codes, text_va, text)
    results += check_time_control(img, codes, text_va, text)
    results += check_equipment_conversion(img, codes, text_va, text)
    results += check_equipment_designers(img, codes, text_va, text)
    results += check_production_tabs(img, codes, text_va, text)
    results += check_province_terrain(img, codes, text_va, text)
    results += check_engine_on_actions(img, codes, text_va, text)
    results += check_map_adjacency(img, codes, text_va, text)
    results += check_globe(img, codes, text_va, text)
    results += check_large_map(img, codes, text_va, text)
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


def check_equipment_conversion(img, codes, text_va, text):
    """equipment_conversion (NOTES.md Part J): the shapes engine_conversion.cpp finds inside its SIG
    functions, with the same cross-checks."""
    sigs, _ = cs.load_signatures(pathlib.Path(__file__).parent.parent / 'src' / 'signatures.h')
    sigs = dict(sigs)
    found = {}
    for name in ('kSigConvertListBuild', 'kSigEquipPostLoad', 'kSigCanConvert', 'kSigEquipIsNaval', 'kSigConversionCost'):
        m = [x.start() for x in cs.compile_pattern(sigs[name]).finditer(text)]
        if len(m) != 1:
            return [('equipment_conversion %s' % name, False, '%d matches' % len(m))]
        found[name] = text_va + m[0]
    build, post, can, cost = (found[n] for n in ('kSigConvertListBuild', 'kSigEquipPostLoad', 'kSigCanConvert', 'kSigConversionCost'))
    results = []
    grow = find_in(img, build, 0x100, codes['kCodeConvListGrow'])
    count = i32(img, build + 0x2f)
    lst = i32(img, grow + 0x2a) if grow else -1
    ok = bool(grow) and i32(img, grow + 0x1f) == count and i32(img, grow + 0x39) == i32(img, grow + 3) and \
        i32(img, grow + 0x43) == lst and i32(img, grow + 0x4d) == lst
    results.append(('kCodeConvListGrow (list +0x%x, count +0x%x)' % (lst, count), ok, hex(grow or build)))
    refs, _ = cs.anchor_refs(img.d, text_va, text, ' in can_convert_from list for type ')
    ok = len(refs) == 1 and build <= refs[0] < build + 0x500 and img.match_at(refs[0], codes['kCodeConvListTypeName'])
    results.append(('kCodeConvListTypeName (anchor lea in the builder)', ok, hex(refs[0]) if refs else '-'))
    test = find_in(img, can, 0x100, codes['kCodeCanConvertList'])
    dtype = i32(img, can + 0xf)
    ok = bool(test) and i32(img, test + 3) == count and i32(img, test + 0x17) == lst and i32(img, test + 0xc) == dtype
    results.append(('kCodeCanConvertList (design type +0x%x)' % dtype, ok, hex(test or can)))
    depth = find_in(img, post, 0x300, codes['kCodePostLoadDepth'])
    ok = bool(depth) and i32(img, post + 0x1c) == i32(img, post + 0x44) and calls(img, post, 0x300, build)
    results.append(('kCodePostLoadDepth (depth +0x%x, calls the builder)' % (i32(img, depth + 0xe) if depth else -1), ok,
                    hex(depth or post)))
    chassis = find_in(img, cost, 0x400, codes['kCodeConvCostChassis'])
    ok = bool(chassis) and i32(img, chassis + 0x1e) == dtype
    role = i32(img, chassis + 0xd) if chassis else -1
    results.append(('kCodeConvCostChassis (same design type, role +0x%x)' % role, ok, hex(chassis or cost)))
    # The role mode: line rebuild, convert click, convert tooltip.
    found = {}
    for name in ('kSigLineRebuildSources', 'kSigConvertClick', 'kSigConvertTooltip'):
        m = [x.start() for x in cs.compile_pattern(sigs[name]).finditer(text)]
        if len(m) != 1:
            return results + [('equipment_conversion role mode %s' % name, False, '%d matches' % len(m))]
        found[name] = text_va + m[0]
    rebuild, click, tip = found['kSigLineRebuildSources'], found['kSigConvertClick'], found['kSigConvertTooltip']
    target = i32(img, rebuild + 0x2b)
    ok = i32(img, rebuild + 0x3b) == dtype and i32(img, rebuild + 0x4b) == count and i32(img, tip + 0x39) == target
    d, o = img.d, img.va2off(rebuild)
    reaches = any(d[o + i] == 0xe8 and calls(img, rel32(img, rebuild + i), 0x100, can) for i in range(0x100 - 5))
    results.append(('line rebuild (target +0x%x, reaches CanConvert)' % target, ok and reaches, hex(rebuild)))
    ok = i32(img, click + 0x15) == i32(img, click + 0x2a)
    results.append(('convert click (item line +0x%x)' % i32(img, click + 0x15), ok, hex(click)))
    refs, _ = cs.anchor_refs(img.d, text_va, text, 'PRODUCTION_LINE_CONVERT_HEADER')
    app = find_in(img, tip, 0x300, codes['kCodeConvTipAppend'])
    ok = len(refs) == 1 and tip <= refs[0] < tip + 0x100 and bool(app)
    results.append(('kCodeConvTipAppend (convert tooltip)', ok, hex(rel32(img, app + 0x19)) if app else '-'))
    # The left-out list: the stockpile walk's skip rule and the design name function.
    walk = next((rel32(img, rebuild + i) for i in range(0x100 - 5)
                 if d[o + i] == 0xe8 and calls(img, rel32(img, rebuild + i), 0x100, can)), 0)
    skip = find_in(img, walk, 0x100, codes['kCodeStockpileSkip']) if walk else None
    results.append(('kCodeStockpileSkip (obsolete +0x%x, owner +0x%x)' % (i32(img, skip + 2) if skip else -1,
                                                                        img.d[img.va2off(skip + 0xc)] if skip else -1),
                    bool(skip), hex(skip or walk)))
    refs, _ = cs.anchor_refs(img.d, text_va, text, 'PRODUCTION_LINE_CONVERT_ORDER_OF_CONVERSION_ENTRY')
    lea = refs[0] if len(refs) == 1 else 0
    before = find_in(img, lea - 0x180, 0x180, codes['kCodeConvVariantName']) if lea else None
    after = find_in(img, lea, 0x200, codes['kCodeConvVariantName']) if lea else None
    ok = bool(before and after) and rel32(img, before + 7) == rel32(img, after + 7)
    results.append(('kCodeConvVariantName (same name function twice)', ok, hex(rel32(img, before + 7)) if ok else '-'))
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


def check_air_scenarios(img, codes, text_va, text):
    """air_scenarios (NOTES.md Part L): the same pieces engine_air.cpp resolves, with its cross-checks."""
    sigs, _ = cs.load_signatures(pathlib.Path(__file__).parent.parent / 'src' / 'signatures.h')
    sigs = dict(sigs)
    t = [m.start() for m in cs.compile_pattern(sigs['kSigAirTemplates']).finditer(text)]
    s = [m.start() for m in cs.compile_pattern(sigs['kSigAirSpawn']).finditer(text)]
    if len(t) != 1 or len(s) != 1:
        return [('air_scenarios signatures', False, '%d/%d matches' % (len(t), len(s)))]
    u8 = lambda va: img.d[img.va2off(va)]
    u32 = lambda va: struct.unpack_from('<I', img.d, img.va2off(va))[0]
    t_va, s_va = text_va + t[0], text_va + s[0]
    templates, slots = u8(t_va + 3), u8(t_va + 0x35)
    loader = rel32(img, t_va + 0x20)
    results = []
    head = find_in(img, loader, 0x100, codes['kCodeAirLoaderHead'])
    look = find_in(img, loader, 0x200, codes['kCodeAirLoaderLookup'])
    ok = bool(head and look)
    names = []
    if ok:
        entity, attackers, defenders = u8(head + 6), u8(head + 0xa), u8(head + 0x14)
        lea = head + 0x23
        base = lea + 7 + struct.unpack_from('<i', img.d, img.va2off(lea) + 3)[0]
        table = base + u32(head + 0x2e)
        for i in range(slots):
            p = struct.unpack_from('<Q', img.d, img.va2off(table + 8 * i))[0]
            o = img.va2off(p)
            names.append(img.d[o:img.d.index(b'\0', o)].decode('ascii', 'replace'))
        ok = base == 0x140000000 and u8(look + 0x13) == entity and attackers + 0x18 == defenders and all(names)
    results.append(('kCodeAirLoaderHead / Lookup (%d slots: %s ... %s)' % (slots, names[0] if names else '-', names[-1] if names else '-'),
                    ok, hex(loader)))
    inst, start = rel32(img, s_va + 0xa), rel32(img, s_va + 0x3c)
    ic = find_in(img, inst, 0x60, codes['kCodeAirInstance'])
    ok = bool(ic) and img.match_at(start, codes['kCodeAirStart']) and u8(ic + 0xd) == templates and         u8(start + 0x39) == u8(ic + 0x24) and (not head or u8(start + 0x4b) == u8(head + 6))
    results.append(('kCodeAirInstance / kCodeAirStart (instance template +0x%x)' % (u8(ic + 0x24) if ic else -1), ok, hex(start)))
    attach = None
    for i in range(0x600 - 5):
        if u8(start + i) == 0xe8 and img.match_at(rel32(img, start + i), codes['kCodeAirAttach']):
            attach = rel32(img, start + i)
            break
    creator = rel32(img, attach + 0x20) if attach else 0
    model = rel32(img, attach + 0x2f) if attach else 0
    mt = find_in(img, model, 0x400, codes['kCodeAirModelType']) if model else None
    ok = bool(attach and mt) and img.match_at(creator, codes['kCodeAirCreator']) and u8(creator + 6) == u8(creator + 0xb)
    results.append(('kCodeAirAttach / Creator / ModelType (design type +0x%x, type name +0x%x)' %
                    (u32(mt + 3) if mt else -1, u8(mt + 0xa) if mt else -1), ok, hex(attach or 0)))
    return results


def check_equipment_designers(img, codes, text_va, text):
    """equipment_designers (NOTES.md Part N): the same pieces engine_designers.cpp resolves, with its cross-checks."""
    hdr = pathlib.Path(__file__).parent.parent / 'src' / 'signatures.h'
    sigs, anchors = cs.load_signatures(hdr)
    sigs, anchors, k = dict(sigs), dict(anchors), load_constants(hdr)
    u8 = lambda va: img.d[img.va2off(va)]
    u32 = lambda va: struct.unpack_from('<I', img.d, img.va2off(va))[0]

    def one(name):
        m = [text_va + x.start() for x in cs.compile_pattern(sigs[name]).finditer(text)]
        return m[0] if len(m) == 1 else None

    def lea(name):
        refs, _ = cs.anchor_refs(img.d, text_va, text, anchors[name])
        return refs[0] if refs and len(refs) == 1 else None

    results = []
    can = one('kSigEqdCanOpen')
    designable = rel32(img, can + k['kSigEqdCanOpenDesignableCall']) if can else 0
    ok = bool(can) and img.match_at(designable, codes['kCodeEqdDesignable'])
    mask = rel32(img, designable + k['kCodeEqdDesignableMaskCall']) if ok else 0
    ok = ok and img.match_at(mask, codes['kCodeEqdDlcMask'])
    bit = u8(mask + k['kCodeEqdDlcTankBit']) if ok else 0
    u64 = lambda va: struct.unpack_from('<Q', img.d, img.va2off(va))[0]
    ship = u64(mask + k['kCodeEqdDlcShipMask']) if ok else 0
    plane = u64(mask + k['kCodeEqdDlcPlaneMask']) if ok else 0
    ok = ok and bit != 0 and bit & (bit - 1) == 0 and ship != 0 and plane != 0 and not (ship & plane) and not ((ship | plane) & bit)
    results.append(('kCodeEqdDesignable / DlcMask (ship 0x%x, tank 0x%x, plane 0x%x)' % (ship, bit, plane), ok, hex(designable)))

    opn = one('kSigEqdOpenVariant')
    t = find_in(img, opn, 0x300, codes['kCodeEqdTitle']) if opn else None
    fn = rel32(img, t + k['kCodeEqdTitleTextCall']) if t else 0
    anchor = lea('kAnchorEqdTitle')
    ok = bool(fn) and img.match_at(fn, codes['kCodeEqdTitleText']) and bool(anchor) and fn <= anchor < fn + 0x40
    found = []
    for jump in ('kCodeEqdTitleShipJump', 'kCodeEqdTitleTankJump', 'kCodeEqdTitlePlaneJump') if ok else ():
        j = fn + k[jump]
        at = j + 2 + struct.unpack_from('<b', img.d, img.va2off(j + 1))[0]
        key = '?'
        if img.d[img.va2off(at):img.va2off(at) + 3] == b'\x48\x8d\x15':
            o = img.va2off(at + 7 + i32(img, at + 3))
            key = img.d[o:img.d.index(b'\0', o)].decode('ascii', 'replace')
        found.append(key)
    ok = ok and found == ['SHIP_DESIGNER_TITLE', 'TANK_DESIGNER_TITLE', 'PLANE_DESIGNER_TITLE']
    results.append(('kCodeEqdTitle / TitleText (kind +0x%x, title +0x%x, kinds 0-2 = %s)' %
                    (u32(t + 2) if t else -1, u32(t + 0xe) if t else -1, '/'.join(x.split('_')[0] for x in found) or '?'), ok, hex(t or 0)))

    ref = one('kSigEqdRefresh')
    assign = rel32(img, ref + k['kSigEqdRefreshAssignCall']) if ref else 0
    ok = bool(ref) and img.match_at(assign, codes['kCodeEqdSlotAssign']) and \
        u8(assign + k['kCodeEqdSlotCountDisp']) == (u8(designable + k['kCodeEqdDesignableCountDisp']) if designable else -1)
    tok = find_in(img, assign, 0x100, codes['kCodeEqdSlotToken']) if ok else None
    hf = find_in(img, assign, 0x400, codes['kCodeEqdSlotHideFixed']) if ok else None
    hc = find_in(img, assign, 0x400, codes['kCodeEqdSlotHideCustom']) if ok else None
    ok = ok and bool(tok and hf and hc) and rel32(img, hf + 4) == rel32(img, hc + 4)
    ok = ok and img.match_at(rel32(img, hf + 4), codes['kCodeEqdSlotClear'])
    results.append(('kCodeEqdSlotAssign / Token / Hide (archetype +0x%x, view %d)' %
                    (u32(assign + 0x31) if assign else -1, struct.unpack_from('<b', img.d, img.va2off(ref + 0x26))[0] if ref else 0),
                    ok, hex(assign)))

    cu, fx = one('kSigEqdSlotsCustom'), one('kSigEqdSlotsFixed')
    creator = rel32(img, cu + k['kSigEqdSlotsCreatorCall']) if cu else 0
    ok = bool(cu and fx) and rel32(img, fx + k['kSigEqdSlotsCreatorCall']) == creator and \
        u32(cu + k['kSigEqdSlotsContainerDisp']) == u32(fx + k['kSigEqdSlotsContainerDisp']) and bool(hc and hf) and \
        u32(hc + k['kCodeEqdSlotCountVecDisp']) == u32(cu + k['kSigEqdSlotsVecDisp']) + 0xc and \
        u32(hf + k['kCodeEqdSlotCountVecDisp']) == u32(fx + k['kSigEqdSlotsVecDisp']) + 0xc
    look = find_in(img, creator, 0x200, codes['kCodeEqdSlotLookup']) if ok else None
    place = find_in(img, creator, 0x400, codes['kCodeEqdSlotPlace']) if ok else None
    ok = ok and bool(look and place) and u32(look + 6) % 8 == 0
    ok = ok and img.match_at(rel32(img, place + k['kCodeEqdSetPositionCall']), codes['kCodeEqdSetPosition'])
    new = find_in(img, creator, 0x400, codes['kCodeEqdSlotNew']) if ok else None
    gui = find_in(img, rel32(img, new + k['kCodeEqdSlotCtorCall']), 0x200, codes['kCodeEqdSlotGuiBase']) if new else None
    ok = ok and bool(gui) and u32(gui + 3) == u32(rel32(img, place + 0x12) + 3)
    results.append(('kCodeEqdSlotLookup / Place (windows +0x%x/+0x%x, position +0x%x)' %
                    (u32(cu + 7) if cu else -1, u32(fx + 7) if fx else -1, u32(place + 3) if place else -1), ok, hex(creator)))

    roles = lea('kAnchorEqdRoles')
    store = find_in(img, roles, 0x80, codes['kCodeEqdRolesStore']) if roles else None
    ok = bool(store) and u32(store + k['kCodeEqdRootFindSlot']) % 8 == 0
    results.append(('kCodeEqdRolesStore (roles +0x%x, root +0x%x)' %
                    (u32(store + k['kCodeEqdRolesDisp']) if store else -1, u32(store + k['kCodeEqdRootDisp']) if store else -1),
                    ok, hex(roles or 0)))
    # `hide`: the root's lookup slots (calls whose result the window keeps), the roles lookup among them.
    slots = []
    pat = cs.compile_pattern(codes['kCodeEqdRootFind'])
    if roles:
        lo = img.va2off(roles - 0x800)
        for m in pat.finditer(img.d, lo, lo + 0x2800):
            call = roles - 0x800 + (m.start() - lo) + k['kCodeEqdRootFindCall']
            modrm = u8(call + 1)
            slot = u8(call + 2) if modrm == 0x50 else u32(call + 2) if modrm == 0x90 else 0
            after = call + (3 if modrm == 0x50 else 6)
            if slot and slot % 8 == 0 and img.d[img.va2off(after):img.va2off(after) + 3] == b'\x48\x89\x86' and slot not in slots:
                slots.append(slot)
    ok = bool(store) and u32(store + k['kCodeEqdRootFindSlot']) in slots
    # The container lookup (plane_role): rcx = the same root, then the slot.
    pr = lea('kAnchorEqdPlaneRole')
    root = u32(store + k['kCodeEqdRootDisp']) if store else -1
    cont = u32(pr + k['kCodeEqdRootFindContainerSlot']) if pr and img.match_at(pr, codes['kCodeEqdRootFindContainer']) else 0
    ok = ok and bool(cont) and img.d[img.va2off(pr - 10):img.va2off(pr - 7)] == b'\x48\x8b\x8e' and u32(pr - 7) == root and \
        img.d[img.va2off(pr - 3):img.va2off(pr)] == b'\x48\x8b\x01'
    results.append(('kCodeEqdRootFind / Container (lookup slots %s, containers %s)' % ([hex(s) for s in slots], hex(cont)), ok, hex(roles or 0)))
    buttons = one('kSigEqdButtons')
    ok = bool(buttons and store) and u32(buttons + k['kSigEqdButtonsRootDisp']) == u32(store + k['kCodeEqdRootDisp'])
    results.append(('kSigEqdButtons (root +0x%x)' % (u32(buttons + k['kSigEqdButtonsRootDisp']) if buttons else -1), ok, hex(buttons or 0)))

    # Stats: the three arrays (their entries name stats of the game's loc key table), the fill call.
    st, nm = one('kSigEqdStats'), one('kSigEqdStatName')
    keys = 0
    if nm:
        lea_at = nm + k['kSigEqdStatNameTableLea']
        table = lea_at + 7 + i32(img, lea_at + 3)
        while keys < 256:
            ptr = struct.unpack_from('<Q', img.d, img.va2off(table + 8 * keys))[0]
            o = img.va2off(ptr) if ptr >> 32 == 1 else None
            if o is None or img.d[o:o + 5] != b'STAT_':
                break
            keys += 1
    ok = bool(st and nm) and keys >= 8 and u8(st + k['kSigEqdStatsFillCall']) == 0xE8
    arrays = []
    for g in range(3) if ok else []:
        b_at, e_at = st + k['kSigEqdStatsLea%d' % (2 * g)], st + k['kSigEqdStatsLea%d' % (2 * g + 1)]
        if img.d[img.va2off(b_at):img.va2off(b_at) + 3] != b'\x48\x8d\x05' or img.d[img.va2off(e_at):img.va2off(e_at) + 3] != b'\x48\x8d\x05':
            ok = False
            break
        b, e = b_at + 7 + i32(img, b_at + 3), e_at + 7 + i32(img, e_at + 3)
        ids = [struct.unpack_from('<i', img.d, img.va2off(a))[0] for a in range(b, e, 8)]
        kinds = [struct.unpack_from('<b', img.d, img.va2off(a) + 4)[0] for a in range(b, e, 8)]
        ok = ok and 0 < len(ids) <= 64 and all(kd != 0 or 0 <= i < keys for i, kd in zip(ids, kinds))
        arrays.append(len(ids))
    results.append(('kSigEqdStats / StatName (%d stat keys, rows %s)' % (keys, arrays), ok, hex(st or 0)))
    # The view markers for the Fill hook: dispatcher and comparison panels reach the same fillers; the
    # plane filler and the comparison's plane branch call Fill three times each.
    disp, cmpf = one('kSigEqdStatsDispatch'), one('kSigEqdStatsRef')
    fill = rel32(img, st + k['kSigEqdStatsFillCall']) if st else 0
    call = lambda base, off: rel32(img, base + off) if base and u8(base + off) == 0xE8 else 0
    ok = bool(disp and cmpf and st) and call(disp, k['kSigEqdStatsDispatchTankCall']) == st and \
        call(cmpf, k['kSigEqdStatsRefTankCall']) == st and \
        call(disp, k['kSigEqdStatsDispatchShipCall']) == call(cmpf, k['kSigEqdStatsRefShipCall']) != 0 and \
        u32(disp + k['kSigEqdStatsDispatchKindDisp']) == (u32(t + 2) if t else -1)
    pc = find_in(img, disp, 0x300, codes['kCodeEqdStatsPlane']) if ok else None
    plane_fn = rel32(img, pc + k['kCodeEqdStatsPlaneCall']) if pc else 0

    def fills(lo, n):
        return sum(1 for a in range(lo, lo + n) if u8(a) == 0xE8 and rel32(img, a) == fill) if lo else 0
    counts = [fills(plane_fn, 0x180), fills(call(disp, k['kSigEqdStatsDispatchShipCall']), 0x400), fills(st, 0x180), fills(cmpf, 0x300)]
    ok = ok and counts == [3, 3, 3, 3]
    results.append(('kSigEqdStatsDispatch / Ref / plane filler (Fill calls plane/ship/tank/ref %s)' % counts, ok,
                    '%s %s' % (hex(disp or 0), hex(plane_fn))))

    # Roles: the dropdown builder's entry and label functions, the type mask it compares, the base hop.
    build = one('kSigEqdRoleBuild')
    entry = rel32(img, build + k['kSigEqdRoleBuildEntryCall']) if build and u8(build + k['kSigEqdRoleBuildEntryCall']) == 0xE8 else 0
    ok = bool(entry) and img.match_at(entry, codes['kCodeEqdRoleEntry']) and u8(entry + k['kCodeEqdRoleEntryLabelCall']) == 0xE8
    label = rel32(img, entry + k['kCodeEqdRoleEntryLabelCall']) if ok else 0
    ok = ok and img.match_at(label, codes['kCodeEqdRoleLabel'])
    cmpm = find_in(img, build - 0x200, 0x200, codes['kCodeEqdRoleTypeMask']) if ok else None
    basem = find_in(img, build - 0x400, 0x400, codes['kCodeEqdRoleBase']) if ok else None
    mask_disp = u32(build + k['kSigEqdRoleBuildMaskDisp']) if build else -1
    design_type = u32(pick + k['kSigEqdPickTypeDisp']) if (pick := one('kSigEqdPickWindow')) else -1
    ok = ok and bool(cmpm and basem) and u32(cmpm + k['kCodeEqdRoleTypeMaskDesignDisp']) == mask_disp and \
        u32(basem + k['kCodeEqdRoleBaseTypeDisp']) == design_type
    types = rel32(img, entry + k['kCodeEqdRoleEntryTypesCall']) if ok and u8(entry + k['kCodeEqdRoleEntryTypesCall']) == 0xE8 else 0
    ok = ok and bool(types) and img.match_at(types, codes['kCodeEqdDesignTypes']) and \
        rel32(img, types + k['kCodeEqdDesignTypesJmp']) == rel32(img, types + k['kCodeEqdDesignTypesCall']) and \
        u32(types + k['kCodeEqdDesignTypesBaseDisp']) == u32(basem + k['kCodeEqdRoleBaseDisp']) and \
        u32(types + k['kCodeEqdDesignTypesTypeDisp']) == design_type
    results.append(('kSigEqdRoleBuild / Entry / Label / TypeMask / Base (design mask +0x%x, type mask +0x%x, base +0x%x)' %
                    (mask_disp, u32(cmpm + 3) if cmpm else -1, u32(basem + k['kCodeEqdRoleBaseDisp']) if basem else -1),
                    ok, '%s %s' % (hex(entry), hex(label))))

    # Experience: pool offsets and tests, the window's line, the tooltip masks, the commands' tests.
    pick, pool, shown, tip = one('kSigEqdPickWindow'), one('kSigEqdPool'), one('kSigEqdXpShown'), one('kSigEqdTooltip')
    army = find_in(img, pool, 0x100, codes['kCodeEqdPoolArmy']) if pool else None
    naval = rel32(img, pick + 0x23) if pick else 0
    air = rel32(img, pick + 0x68) if pick and u8(pick + 0x68) == 0xE8 else 0
    ok = bool(pool and shown and tip and army and air) and rel32(img, pool + k['kSigEqdPoolNavalCall']) == naval and \
        rel32(img, pool + k['kSigEqdPoolAirCall']) == air and u8(shown + k['kSigEqdXpArmyDisp']) == u8(army + 3)
    masks = []
    m = tip
    for _ in range(3) if ok else []:
        m = find_in(img, m, 0x300, codes['kCodeEqdTooltipMask'])
        if not m:
            ok = False
            break
        masks.append(struct.unpack_from('<Q', img.d, img.va2off(m + 2))[0])
        m += 10
    ok = ok and len(masks) == 3 and not (masks[0] & masks[1] or masks[0] & masks[2] or masks[1] & masks[2])
    rets = []
    for cls, shapes in (('.?AVCCreateEquipmentVariantCommand@@', ('kCodeEqdCreateAir', 'kCodeEqdCreateNaval', 'kCodeEqdCreateCall')),
                        ('.?AVCUpdateEquipmentVariantCommand@@', ('kCodeEqdUpdateTest', 'kCodeEqdUpdateTest', 'kCodeEqdUpdateCall'))):
        vt = img.vtable(cls)
        ex = vt[1][10] if vt and len(vt[1]) > 10 else 0
        a = find_in(img, ex, 0x1000, codes[shapes[0]]) if ex else None
        n = (find_in(img, a + 1, 0x200, codes[shapes[1]]) if shapes[0] == shapes[1] else find_in(img, ex, 0x1000, codes[shapes[1]])) if a else None
        c = k[shapes[2]]
        ok = ok and bool(a and n) and rel32(img, a + c) == air and rel32(img, n + c) == naval
        rets.append(hex(a or 0))
    results.append(('kSigEqdPool / XpShown / Tooltip / command tests (pools +0x%x/+0x%x/+0x%x)' %
                    (u8(army + 3) if army else -1, u8(pool + k['kSigEqdPoolNavyDisp']) if pool else -1,
                     u8(pool + k['kSigEqdPoolAirDisp']) if pool else -1), ok, ' '.join(rets)))
    return results


def check_production_tabs(img, codes, text_va, text):
    """production_tabs (NOTES.md Part O): the same pieces engine_production_tabs.cpp resolves, with its cross-checks."""
    hdr = pathlib.Path(__file__).parent.parent / 'src' / 'signatures.h'
    sigs, _ = cs.load_signatures(hdr)
    sigs, k = dict(sigs), load_constants(hdr)
    arrays = {m.group(1): [int(x, 0) for x in m.group(2).split(',')]
              for m in re.finditer(r'\b(kSigPtab\w+)\[4\]\s*=\s*\{([^}]*)\}', hdr.read_text(encoding='utf-8'))}
    u8 = lambda va: img.d[img.va2off(va)]
    u32 = lambda va: struct.unpack_from('<I', img.d, img.va2off(va))[0]
    u64 = lambda va: struct.unpack_from('<Q', img.d, img.va2off(va))[0]

    def one(name):
        m = [text_va + x.start() for x in cs.compile_pattern(sigs[name]).finditer(text)]
        return m[0] if len(m) == 1 else None

    def string(lea):
        o = img.va2off(lea + 7 + i32(img, lea + 3))
        return img.d[o:img.d.index(b'\0', o)].decode('ascii', 'replace')

    results = []
    setup, clicks, states = one('kSigPtabSetup'), one('kSigPtabClicks'), one('kSigPtabStates')
    land, naval = one('kSigPtabListLand'), one('kSigPtabListNaval')
    if not (setup and clicks and states and land and naval):
        return [('production_tabs signatures', False, 'missing')]
    # Setup: root, the two containers, the named lookups.
    r = find_in(img, setup, 0x100, codes['kCodePtabRoot'])
    t = find_in(img, r, 0x100, codes['kCodePtabTop']) if r else None
    slot = u32(r + k['kCodePtabRootSlot']) if r else 0
    ok = bool(r and t) and u32(t + k['kCodePtabTopSlot']) == slot
    names = (string(r + k['kCodePtabRootName']), string(t + k['kCodePtabTopName'])) if ok else ('-', '-')
    results.append(('kCodePtabRoot / Top (%s > %s, slot 0x%x)' % (names[0], names[1], slot), ok, hex(r or 0)))
    found = {}
    for i in range(k['kCodePtabFindRange']):
        c = setup + i
        if img.match_at(c, codes['kCodePtabFindA']):
            found[string(c + 7)] = (u8(c + 6), u32(c + 0x33))
        elif img.match_at(c, codes['kCodePtabFindB']):
            found[string(c + 3)] = (u8(c + 0x2b), u32(c + 0x2f))
        elif img.match_at(c, codes['kCodePtabFindC']):
            found[string(c + 3)] = (u32(c + 0x2b), u32(c + 0x32))
    buttons = ['infantry_button', 'tank_button', 'aircraft_button', 'naval_button']
    ok = all(b in found and b + '_glow' in found for b in buttons) and \
        len({found[b][0] for b in buttons}) == 1 and len({found[b + '_glow'][0] for b in buttons}) == 1
    results.append(('kCodePtabFindA/B/C (%d lookups; button slot 0x%x, icon slot 0x%x)' %
                    (len(found), found[buttons[0]][0] if ok else -1, found[buttons[0] + '_glow'][0] if ok else -1), ok,
                    hex(setup)))
    disp = {found[b][1]: b for b in buttons} if ok else {}
    # Click handlers and button states: each tab once, one Open and one State function.
    masks, opens = {}, set()
    for off in arrays.get('kSigPtabClickThunk', []):
        th = clicks + off
        wide = u8(th + k['kSigPtabThunkMaskOp']) == 0x48
        mask = u64(th + k['kSigPtabThunkMaskOp'] + 2) if wide else u32(th + k['kSigPtabThunkMaskOp'] + 1)
        opens.add(rel32(img, th + (0x24 if wide else 0x1f)))
        b = disp.get(u32(th + k['kSigPtabThunkButtonDisp']), '?')
        masks[b] = (mask, u8(th + k['kSigPtabThunkNaval']) == 0x41)
    ok = len(opens) == 1 and set(masks) == set(buttons) and all(masks[b][1] == (b == 'naval_button') for b in buttons)
    results.append(('kSigPtabClicks (%s)' % ', '.join('%s 0x%x' % (b.split('_')[0], masks[b][0]) for b in buttons if b in masks),
                    ok, hex(clicks)))
    fns, state = set(), {}
    for i in range(4):
        b = disp.get(u32(states + arrays['kSigPtabStateButton'][i]), '?')
        m = arrays['kSigPtabStateMask'][i]
        state[b] = u32(states + m) if i < 2 else u64(states + m)
        fns.add(rel32(img, states + arrays['kSigPtabStateCall'][i]))
        ok = ok and b in found and u32(states + arrays['kSigPtabStateGlow'][i]) == found.get(b + '_glow', (0, -1))[1]
    fn = fns.pop() if len(fns) == 1 else 0
    ok = ok and bool(fn) and set(state) == set(buttons)
    results.append(('kSigPtabStates (state masks %s)' % ', '.join('0x%x' % state[b] for b in buttons if b in state), ok, hex(states)))
    f = find_in(img, fn, 0x80, codes['kCodePtabFactories']) if fn else None
    d = find_in(img, fn, 0x200, codes['kCodePtabVariants']) if fn else None
    s = find_in(img, fn, 0x280, codes['kCodePtabState']) if fn else None
    g = find_in(img, s, 0x80, codes['kCodePtabGlow']) if s else None
    ok = bool(f and d and s and g)
    if ok:
        ok = u32(d + k['kCodePtabCountDisp']) == u32(d + k['kCodePtabDesignsDisp']) + 0xc and \
            u8(g + k['kCodePtabGlowFlags2']) == u8(g + k['kCodePtabGlowFlags']) and \
            (~u8(g + k['kCodePtabGlowClear'])) & 0xff == u8(g + k['kCodePtabGlowSet'])
    results.append(('kCodePtabFactories / Variants / State / Glow (design mask +0x%x, type +0x%x)' %
                    (u32(d + k['kCodePtabMaskDisp']) if d else -1, u32(d + k['kCodePtabTypeDisp']) if d else -1), ok, hex(fn)))
    lt = find_in(img, land, k['kCodePtabMaskTestRange'], codes['kCodePtabMaskTestLand'])
    nt = find_in(img, naval, k['kCodePtabMaskTestRange'], codes['kCodePtabMaskTestNaval'])
    ok = bool(lt and nt and d) and u32(lt + 3) == u32(nt + 3) and \
        u32(lt + k['kCodePtabDesignMaskDisp']) == u32(nt + k['kCodePtabDesignMaskDisp']) == u32(d + k['kCodePtabMaskDisp'])
    results.append(('kCodePtabMaskTestLand / Naval (window mask +0x%x)' % (u32(lt + 3) if lt else -1), ok, hex(lt or 0)))
    tn = one('kSigPtabTypeByName')
    ok = bool(tn) and u32(tn + k['kSigPtabTypeDefaultToken']) == u32(tn + k['kSigPtabTypeDefaultToken2'])
    results.append(('kSigPtabTypeByName (default token 0x%x)' % (u32(tn + k['kSigPtabTypeDefaultToken']) if tn else -1), ok,
                    hex(tn or 0)))
    # visible: the parser's scripted trigger lookup, its valid test, CScriptedTrigger::Evaluate (RTTI slot 22).
    st = one('kSigPtabScriptedTrigger')
    v = find_in(img, st, 0x100, codes['kCodePtabScriptedValid']) if st else None
    sc = img.vtable('.?AVCScriptedTrigger@@')
    slot = k['kScriptedTriggerEvalSlot']
    ev = sc[1][slot] if sc and len(sc[1]) > slot else 0
    call = st + k['kSigPtabScriptedFindCall'] if st else 0
    ok = bool(st and v and ev) and img.match_at(ev, codes['kCodePtabScriptedEval']) and \
        u8(ev + k['kCodePtabScriptedEvalSlot']) % 8 == 0 and u8(call) == 0xE8 and img.is_code(rel32(img, call))
    results.append(('kSigPtabScriptedTrigger / Valid / Eval (find 0x%x, valid +0x%x, template +0x%x, eval slot 0x%x)' %
                    (rel32(img, call) if st else 0, u8(v + k['kCodePtabScriptedValidDisp']) if v else -1,
                     u8(ev + k['kCodePtabScriptedTemplateDisp']) if ok else -1, u8(ev + k['kCodePtabScriptedEvalSlot']) if ok else -1),
                    ok, hex(st or 0)))
    return results


def check_map_adjacency(img, codes, text_va, text):
    """map_adjacency (NOTES.md Part R): the neighbour finder's list layout and the border type compare
    (kSigAdjAttackCheck), the state impassable getter (kSigAdjStateImpassable), as engine_adjacency.cpp
    reads them."""
    hdr = pathlib.Path(__file__).parent.parent / 'src' / 'signatures.h'
    sigs, _ = cs.load_signatures(hdr)
    sigs, k = dict(sigs), load_constants(hdr)
    u8 = lambda va: img.d[img.va2off(va)]
    u32 = lambda va: struct.unpack_from('<I', img.d, img.va2off(va))[0]

    def one(name):
        m = [text_va + x.start() for x in cs.compile_pattern(sigs[name]).finditer(text)]
        return m[0] if len(m) == 1 else None

    at, st = one('kSigAdjAttackCheck'), one('kSigAdjStateImpassable')
    if not (at and st):
        return [('map_adjacency signatures', False, 'missing')]
    r = []
    f = rel32(img, at + k['kSigAdjFindCall'])
    ok = (img.d[img.va2off(f):img.va2off(f) + 3] == bytes([0x48, 0x63, 0x41]) and
          img.d[img.va2off(f) + 0xb:img.va2off(f) + 0xe] == bytes([0x4C, 0x8B, 0x59]) and
          img.d[img.va2off(f) + 0x2a:img.va2off(f) + 0x2d] == bytes([0x48, 0x83, 0xC0]))
    r.append(('kSigAdjAttackCheck (neighbours +0x%x, count +0x%x, id +0x%x, stride 0x%x, type +0x%x == %d)' %
              (u8(f + 0xe), u8(f + 3), u8(f + 0x18), u8(f + 0x2d), u8(at + k['kSigAdjTypeDisp']), u8(at + k['kSigAdjTypeDisp'] + 1)),
              ok and u8(f + 0x2d) >= 0x10, hex(at)))
    g = rel32(img, st + k['kSigAdjStateGetterCall'])
    o = img.va2off(g)
    ok = img.d[o:o + 3] == bytes([0x48, 0x8B, 0x41]) and img.d[o + 4:o + 7] == bytes([0x0F, 0xB6, 0x80]) and img.d[o + 0xb] == 0xC3
    r.append(('kSigAdjStateImpassable (state +0x%x -> flag +0x%x)' % (u8(g + 3), u32(g + 7)), ok, hex(st)))
    # R6: the map lines (updater getter, province refresh thunk) and the resistance refresh.
    mr, rr, sr = one('kSigAdjMapRefresh'), one('kSigAdjResistRefresh'), one('kSigAdjStateResistance')
    if not (mr and rr and sr):
        r.append(('map_adjacency R6 signatures', False, 'missing'))
        return r
    get = img.va2off(rel32(img, mr + k['kSigAdjMapGetterCall']))
    th = img.va2off(rel32(img, mr + k['kSigAdjMapRefreshCall']))
    ok = img.d[get:get + 3] == bytes([0x48, 0x8B, 0x05]) and img.d[get + 7] == 0xC3 and img.d[th:th + 5] == bytes([0x48, 0x8B, 0x49, 0x08, 0xE9])
    r.append(('kSigAdjMapRefresh (getter, refresh thunk)', ok, hex(mr)))
    fn = img.va2off(rel32(img, rr + k['kSigAdjResistRefreshCall']))
    ok = img.d[fn:fn + 8] == bytes([0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x79])
    r.append(('kSigAdjResistRefresh (occupied +0x%x)' % img.d[fn + 8], ok, hex(rr)))
    off = u32(sr + k['kSigAdjStateResistanceDisp'])
    r.append(('kSigAdjStateResistance (state +0x%x)' % off, 0 < off < 0x4000, hex(sr)))
    return r


def check_globe(img, codes, text_va, text):
    """globe (NOTES.md Part S): the map camera's layout as engine_globe.cpp resolves it, with its
    cross-checks (main camera in the visible area and the picking, one map in both clamps, the
    copies' stride, the targets after eye and look-at), and the visible area's first point
    (kCodeGlobeAreaStore, first match from the area function's start)."""
    hdr = pathlib.Path(__file__).parent.parent / 'src' / 'signatures.h'
    sigs, _ = cs.load_signatures(hdr)
    sigs, k = dict(sigs), load_constants(hdr)
    u8 = lambda va: img.d[img.va2off(va)]
    u32 = lambda va: struct.unpack_from('<I', img.d, img.va2off(va))[0]

    def one(name):
        m = [text_va + x.start() for x in cs.compile_pattern(sigs[name]).finditer(text)]
        return m[0] if len(m) == 1 else None

    area, ray, clamp, fr = one('kSigGlobeArea'), one('kSigGlobeScreenRay'), one('kSigGlobeEdgeClamp'), one('kSigGlobeFrustum')
    pick, cc, look = one('kSigGlobePickCams'), one('kSigGlobeCameraClamp'), one('kSigCameraLookAt')
    pan, zoom = one('kSigGlobePanRay'), one('kSigGlobeZoomRay')
    if not (area and ray and clamp and fr and pick and cc and look and pan and zoom):
        return [('globe signatures', False, 'missing')]
    calls_ok = rel32(img, pan + k['kSigGlobePanRayCall']) == ray and rel32(img, zoom + k['kSigGlobeZoomRayCall']) == ray
    store = find_in(img, area, 0x400, codes['kCodeGlobeAreaStore'])
    if not store:
        return [('kCodeGlobeAreaStore (in the visible area function)', False, hex(area))]
    main = u8(area + k['kSigGlobeAreaMainCam'])
    cams = [main, u32(pick + k['kSigGlobePickCopy1']), u32(pick + k['kSigGlobePickCopy2'])]
    flags = [u32(pick + k['kSigGlobePickCopy1Flag']), u32(pick + k['kSigGlobePickCopy2Flag'])]
    eye, lk = u32(cc + k['kSigGlobeClampEyeDisp']), u32(cc + k['kSigGlobeClampLookDisp'])
    eyeT, lookT = u32(look + 21), u32(look + 45)
    pts, planes, pos = u32(store + k['kCodeGlobeAreaStoreDisp']), u32(fr + k['kSigGlobeFrustumPlanesDisp']), u32(ray + k['kSigGlobeRayPosDisp'])
    w, h = u8(cc + k['kSigGlobeClampWidthDisp']), u8(cc + k['kSigGlobeClampHeightDisp'])
    mapA = clamp + k['kSigGlobeEdgeMapEnd'] + i32(img, clamp + k['kSigGlobeEdgeMapRel'])
    mapB = cc + k['kSigGlobeClampMapRel'] + 4 + i32(img, cc + k['kSigGlobeClampMapRel'])
    stride = cams[1] - cams[0]
    ok = (main == u8(pick + k['kSigGlobePickMain']) and mapA == mapB and cams[2] - cams[1] == stride and stride > planes + 0x60 and
          flags[1] == flags[0] + 1 and eye > flags[1] and lk == eye + 12 and eyeT == eye + 0x18 and lookT == lk + 0x18 and
          pts > lk and planes > pos and h == w + 4 and calls_ok)
    return [('kSigGlobe* / kCodeGlobeAreaStore (cameras +0x%x/+0x%x/+0x%x, in use +0x%x/+0x%x, eye +0x%x, look +0x%x, '
             'area +0x%x, planes +0x%x, map +0x%x/+0x%x, camera rays call it: %s)' % (cams[0], cams[1], cams[2], flags[0], flags[1], eye, lk, pts, planes, w, h, calls_ok),
             ok, hex(area))]


def check_large_map(img, codes, text_va, text):
    """large_map (NOTES.md Part T): the map scratch buffer's two sizes are 50 MB and the size field
    lies 8 past the pointer field."""
    hdr = pathlib.Path(__file__).parent.parent / 'src' / 'signatures.h'
    sigs, _ = cs.load_signatures(hdr)
    sigs, k = dict(sigs), load_constants(hdr)
    m = [text_va + x.start() for x in cs.compile_pattern(sigs['kSigMapScratchSize']).finditer(text)]
    if len(m) != 1:
        return [('kSigMapScratchSize', False, 'missing')]
    at = m[0]
    u32 = lambda va: struct.unpack_from('<I', img.d, img.va2off(va))[0]
    size, ptr = u32(at + 2), u32(at + k['kSigMapScratchSizePtrDisp'])
    a, b = u32(at + k['kSigMapScratchSizeImm1']), u32(at + k['kSigMapScratchSizeImm2'])
    m = [text_va + x.start() for x in cs.compile_pattern(sigs['kSigTextureFileLimit']).finditer(text)]
    lim = u32(m[0] + k['kSigTextureFileLimitImm']) if len(m) == 1 else None
    return [('kSigMapScratchSize (size +0x%x, pointer +0x%x, %d / %d bytes)' % (size, ptr, a, b),
             size == ptr + 8 and a == b == 0x3200000, hex(at)),
            ('kSigTextureFileLimit (%s bytes)' % lim, lim == 0x1000000, hex(m[0]) if len(m) == 1 else 'missing')] + [
            ('%s (shifts %s)' % (name, sh), sh == [8, 8], hex(va) if va else 'missing')
            for name, va, sh in [(n, v, [img.d[img.va2off(v + k[n + s])] for s in ('Shift1', 'Shift2')] if v else None)
                                 for n, v in [(n, (lambda mm: mm[0] if len(mm) == 1 else None)(
                                     [text_va + x.start() for x in cs.compile_pattern(sigs[n]).finditer(text)]))
                                     for n in ('kSigTerrainBlocksBuild', 'kSigTerrainBlocksCamera')]]]


def check_province_terrain(img, codes, text_va, text):
    """province_terrain (NOTES.md Part P): the pieces engine_terrain.cpp resolves, with its cross-checks."""
    hdr = pathlib.Path(__file__).parent.parent / 'src' / 'signatures.h'
    sigs, _ = cs.load_signatures(hdr)
    sigs, k = dict(sigs), load_constants(hdr)
    u8 = lambda va: img.d[img.va2off(va)]
    u32 = lambda va: struct.unpack_from('<I', img.d, img.va2off(va))[0]

    def one(name):
        m = [text_va + x.start() for x in cs.compile_pattern(sigs[name]).finditer(text)]
        return m[0] if len(m) == 1 else None

    combat, ps, lk = one('kSigTerrainCombatLocation'), one('kSigTerrainProvinceState'), one('kSigTerrainLookup')
    he, rb = one('kSigTerrainHasEval'), one('kSigTerrainCountryRebuild')
    if not (combat and ps and lk and he and rb):
        return [('province_terrain signatures', False, 'missing')]
    results = []
    mp, tr = u32(combat + k['kSigTerrainCombatMap']), u32(combat + k['kSigTerrainCombatTerrain'])
    ok = u32(ps + k['kSigTerrainStateMap']) == mp and u32(ps + k['kSigTerrainStateTerrain']) == tr and mp < 0x1000 and tr < 0x1000
    results.append(('kSigTerrainCombatLocation / ProvinceState (map +0x%x, terrain +0x%x, state +0x%x, combat +0x%x/+0x%x)' %
                    (mp, tr, u32(ps + k['kSigTerrainStateDisp']), u8(combat + k['kSigTerrainCombatProvince']),
                     u8(combat + k['kSigTerrainCombatCopy'])), ok, hex(combat)))
    getter = rel32(img, lk + k['kSigTerrainDbGetter'])
    ok = u8(lk + k['kSigTerrainDbGetter']) == 0xE8 and img.is_code(getter) and \
        u8(lk + k['kSigTerrainValid']) == u8(he + k['kSigTerrainHasValid'])
    results.append(('kSigTerrainLookup (getter 0x%x, db +0x%x/+0x%x, token +0x%x, valid +0x%x)' %
                    (getter, u8(lk + k['kSigTerrainDbArray']), u8(lk + k['kSigTerrainDbCount']), u8(lk + k['kSigTerrainToken']),
                     u8(lk + k['kSigTerrainValid'])), ok, hex(lk)))
    cm = u32(he + k['kSigTerrainCountryMask'])
    ok = u32(rb + k['kSigTerrainCountryMask2']) == cm and u32(he + k['kSigTerrainBits']) < 0x1000 and \
        u32(rb + k['kSigTerrainStateMask']) < 0x4000
    results.append(('kSigTerrainHasEval / CountryRebuild (bits +0x%x, country mask +0x%x, dirty +0x%x, states +0x%x/+0x%x, '
                    'state mask +0x%x)' % (u32(he + k['kSigTerrainBits']), cm, u32(rb + k['kSigTerrainCountryDirty']),
                                            u32(rb + k['kSigTerrainCountryStates']), u32(rb + k['kSigTerrainCountryStateCount']),
                                            u32(rb + k['kSigTerrainStateMask'])), ok, hex(he)))
    return results


def check_engine_on_actions(img, codes, text_va, text):
    """engine_on_actions (NOTES.md Part Q): the pieces engine_events.cpp resolves, with its cross-checks."""
    hdr = pathlib.Path(__file__).parent.parent / 'src' / 'signatures.h'
    sigs, _ = cs.load_signatures(hdr)
    sigs, k = dict(sigs), load_constants(hdr)
    u8 = lambda va: img.d[img.va2off(va)]
    u32 = lambda va: struct.unpack_from('<I', img.d, img.va2off(va))[0]

    def one(name):
        m = [text_va + x.start() for x in cs.compile_pattern(sigs[name]).finditer(text)]
        return m[0] if len(m) == 1 else None

    names = ['kSigEventScopeState', 'kSigEvConstructionDone', 'kSigEvBuildingLocation', 'kSigEvConstructionPass',
             'kSigEvResearchUpdate', 'kSigEvTechLevel', 'kSigEvTechOwner', 'kSigEvBattleSides', 'kSigEvBattleEnd',
             'kSigEvDivisionKilled', 'kSigEvDeploy', 'kSigEvAddUnit', 'kSigTerrainCombatLocation', 'kSigEvDecisionTaken',
             'kSigEvDecisionName', 'kSigEvControllerSet', 'kSigEvNavalSideResult', 'kSigEvLeaderCountry', 'kSigEvShipSunk',
             'kSigEvFocusComplete', 'kSigEvFocusName']
    at = {n: one(n) for n in names}
    missing = [n for n in names if not at[n]]
    if missing:
        return [('engine_on_actions signatures', False, 'missing ' + ', '.join(missing))]
    r = []
    p = at['kSigEvConstructionDone']
    r.append(('kSigEvConstructionDone (country +0x%x, type +0x%x, tag +0x%x)' % (u32(p + k['kSigEvConsCountryDisp']),
              u32(p + k['kSigEvConsTypeDisp']), u8(p + k['kSigEvConsTagImm'])), u8(p + k['kSigEvConsTagImm']) < 0x40, hex(p)))
    p = at['kSigEvBuildingLocation']
    r.append(('kSigEvBuildingLocation (location +0x%x)' % u32(p + k['kSigEvLocationDisp']),
              u32(p + k['kSigEvLocationDisp']) == u32(p + k['kSigEvLocationDisp2']), hex(p)))
    p = at['kSigEvTechLevel']
    r.append(('kSigEvTechLevel (template +0x%x, level +0x%x)' % (u32(p + k['kSigEvTechTemplateDisp']), u32(p + k['kSigEvTechLevelDisp'])),
              u32(p + k['kSigEvTechLevelDisp']) == u32(p + k['kSigEvTechLevelDisp2']), hex(p)))
    p, loc = at['kSigEvBattleSides'], at['kSigTerrainCombatLocation']
    adjust = -struct.unpack('<b', bytes([u8(p + k['kSigEvSideBaseDisp'])]))[0]
    side = u8(p + k['kSigEvSideDisp']) + adjust
    prov = u8(loc + k['kSigTerrainCombatProvince'])
    r.append(('kSigEvBattleSides (sides +0x%x/+0x%x, units +0x%x/+0x%x, province +0x%x)' % (side, side + 8, u8(p + k['kSigEvSideUnitsDisp']),
              u8(p + k['kSigEvSideCountDisp']), prov), adjust > 0 and prov == side + 0x10, hex(p)))
    p = at['kSigEvDecisionName']
    lit = p + k['kSigEvDecisionDescRel'] + 4 + i32(img, p + k['kSigEvDecisionDescRel'])
    o = img.va2off(lit)
    r.append(('kSigEvDecisionName (name +0x%x, "_desc" literal)' % u32(p + k['kSigEvDecisionNameDisp']),
              img.d[o:o + 5] == b'_desc' and img.d[o + 5] == 0, hex(p)))
    p = at['kSigEvNavalSideResult']
    r.append(('kSigEvNavalSideResult / LeaderCountry (leaders +0x%x/+0x%x, leader tag +0x%x)' % (u32(p + k['kSigEvNavalLeadersDisp']),
              u32(p + k['kSigEvNavalLeaderCountDisp']), u32(at['kSigEvLeaderCountry'] + k['kSigEvLeaderTagDisp'])),
              u32(p + k['kSigEvNavalLeaderCountDisp']) == u32(p + k['kSigEvNavalLeadersDisp']) + 0xc, hex(p)))
    p, q = at['kSigEvFocusComplete'], at['kSigEvFocusName']
    r.append(('kSigEvFocusComplete / FocusName (country +0x%x, focus id +0x%x)' % (u8(p + k['kSigEvFocusCountryDisp']),
              u8(q + k['kSigEvFocusNameDisp'])),
              u8(p + k['kSigEvFocusCountryDisp']) < 0x40 and u8(q + k['kSigEvFocusNameDisp']) < 0x80, hex(p)))
    p = at['kSigEventScopeState']
    r.append(('kSigEventScopeState (state id +0x%x)' % u8(p + k['kSigEventScopeStateIdDisp']),
              u8(p + k['kSigEventScopeSetCountryCall']) == 0xE8 and u8(p + k['kSigEventScopeSetStateCall']) == 0xE8, hex(p)))
    return r


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
