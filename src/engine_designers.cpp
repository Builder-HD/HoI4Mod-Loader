// Engine access for equipment_designers (NOTES.md Part N): which equipment may open a designer
// window, the window's title, role dropdown and slot windows. UI only: the save goes through the
// game's own variant commands, which don't look at the window.
#include <windows.h>
#include <string.h>

#include <string>

#include "engine.h"
#include "loader.h"
#include "signatures.h"

namespace Engine {

namespace {

template <typename T>
T Rd(const void* base, size_t offset) {
    T v;
    memcpy(&v, (const uint8_t*)base + offset, sizeof v);
    return v;
}

using CanOpen_t = bool(__fastcall*)(void* ctx, const void* type);
using TypeTest_t = bool(__fastcall*)(const void* type);
using Layout_t = bool(__fastcall*)(void* ctx, const void* type);
using DlcMask_t = uint64_t(__fastcall*)();
using ViewFn_t = void(__fastcall*)(void* a, void* b, void* c, void* d);
using SetText_t = void(__fastcall*)(void* element, const EngineString* text);
struct NameView {
    const char* p;
    size_t n;
};
using Find_t = void*(__fastcall*)(void* container, const NameView* name, int flags);
using IsMissing_t = bool(__fastcall*)(const void* found);
using SetPosition_t = void(__fastcall*)(void* window, const int64_t* position);

// Hooked functions and their originals.
uint8_t* g_canOpenFn = nullptr;
CanOpen_t g_canOpenOrig = nullptr;
uint8_t* g_openFn = nullptr;
ViewFn_t g_openOrig = nullptr;
uint8_t* g_refreshFn = nullptr;
ViewFn_t g_refreshOrig = nullptr;

// Called.
TypeTest_t g_designable = nullptr, g_landA = nullptr, g_landB = nullptr;
Layout_t g_layout = nullptr;
DlcMask_t g_dlcMask = nullptr;
uint64_t g_tankBit = 0;
SetText_t g_setText = nullptr;
IsMissing_t g_isMissing = nullptr;
SetPosition_t g_setPosition = nullptr;

// Fields.
uint32_t g_typeSlots = 0, g_typeSlotCount = 0, g_slotToken = 0;  // CEquipmentType -> slots (0x50 each)
const size_t kSlotStride = 0x50;
uint32_t g_kind = 0, g_title = 0, g_archetype = 0, g_roles = 0, g_container = 0, g_lookupSlot = 0, g_position = 0;
uint32_t g_vec[2] = {0, 0};  // custom, fixed slot window vectors {data, cap, count}
uint32_t g_slotOfWindow = 0, g_windowElement = 0;  // slot window -> its slot (null: locked), its window
uint32_t g_guiBase = 0;                            // that window -> its GUI object
int g_viewDelta = 0;         // refresh argument -> view
const int kTankKind = 1;     // TitleText: kind 1 is the tank designer (checked in Resolve)

DesignerFind_t g_find = nullptr;
DesignerOpened_t g_opened = nullptr;
DesignerRefreshed_t g_refreshed = nullptr;
DesignerFault_t g_fault = nullptr;
volatile LONG g_off = 0, g_faultLogged = 0;

bool Fail(const char* why) {
    Log("equipment_designers: %s", why);
    return false;
}

const char* RipString(uint8_t* lea) {
    if (!lea || lea[0] != 0x48 || lea[1] != 0x8D) return nullptr;
    return (const char*)SigRipTarget(lea, 3, 7);
}

bool ResolveAll() {
    uint8_t* p = SigFindUnique("eqd_can_open", kSigEqdCanOpen);
    if (!p) return false;
    g_canOpenFn = p;
    uint8_t* designable = SigFollowRel32(p + kSigEqdCanOpenDesignableCall);
    g_layout = (Layout_t)SigFollowRel32(p + kSigEqdCanOpenLayoutCall);
    if (!SigMatchAt(designable, kCodeEqdDesignable)) return Fail("the designable test has an unexpected shape");
    g_designable = (TypeTest_t)designable;
    uint8_t* mask = SigFollowRel32(designable + kCodeEqdDesignableMaskCall);
    if (!SigMatchAt(mask, kCodeEqdDlcMask)) return Fail("the DLC designer mask has an unexpected shape");
    g_dlcMask = (DlcMask_t)mask;
    g_tankBit = Rd<uint8_t>(mask, kCodeEqdDlcTankBit);
    if (!g_tankBit || (g_tankBit & (g_tankBit - 1))) return Fail("the tank designer's DLC bit is not a single bit");

    if (!(p = SigFindUnique("eqd_pick_window", kSigEqdPickWindow))) return false;
    uint32_t designType = Rd<uint32_t>(p, kSigEqdPickTypeDisp);
    if (EquipmentTypeOffset() && designType != EquipmentTypeOffset())
        return Fail("the window picker's design type differs from production_data's");
    g_landA = (TypeTest_t)SigFollowRel32(p + kSigEqdPickLandACall);
    g_landB = (TypeTest_t)SigFollowRel32(p + kSigEqdPickLandBCall);

    if (!(g_openFn = SigFindUnique("eqd_open", kSigEqdOpenVariant))) return false;
    uint8_t* t = SigFindIn(g_openFn, 0x300, kCodeEqdTitle);
    if (!t) return Fail("the title code in Open was not found");
    g_kind = Rd<uint32_t>(t, kCodeEqdKindDisp);
    g_title = Rd<uint32_t>(t, kCodeEqdTitleDisp);
    g_setText = (SetText_t)SigFollowRel32(t + kCodeEqdSetTextCall);
    uint8_t* text = SigFollowRel32(t + kCodeEqdTitleTextCall);
    uint8_t* anchor = AnchorFindLea("eqd_title", kAnchorEqdTitle);
    if (!SigMatchAt(text, kCodeEqdTitleText) || !anchor || anchor < text || anchor >= text + 0x40)
        return Fail("the title text function has an unexpected shape");
    uint8_t* tank = text + kCodeEqdTitleTankJump + 2 + (int8_t)text[kCodeEqdTitleTankJump + 1];
    const char* tankKey = RipString(tank);
    if (!tankKey || strcmp(tankKey, "TANK_DESIGNER_TITLE") != 0) return Fail("window kind 1 is not the tank designer");

    if (!(g_refreshFn = SigFindUnique("eqd_refresh", kSigEqdRefresh))) return false;
    g_viewDelta = (int8_t)g_refreshFn[kSigEqdRefreshViewDisp];
    uint8_t* assign = SigFollowRel32(g_refreshFn + kSigEqdRefreshAssignCall);
    if (!SigMatchAt(assign, kCodeEqdSlotAssign)) return Fail("the slot assignment has an unexpected shape");
    g_archetype = Rd<uint32_t>(assign, kCodeEqdArchetypeDisp);
    g_typeSlots = Rd<uint8_t>(assign, kCodeEqdSlotsDisp);
    g_typeSlotCount = Rd<uint8_t>(assign, kCodeEqdSlotCountDisp);
    if (g_typeSlotCount != Rd<uint8_t>(designable, kCodeEqdDesignableCountDisp))
        return Fail("the slot count offsets disagree");
    uint8_t* tok = SigFindIn(assign, 0x100, kCodeEqdSlotToken);
    uint8_t* hideFixed = SigFindIn(assign, 0x400, kCodeEqdSlotHideFixed);
    uint8_t* hideCustom = SigFindIn(assign, 0x400, kCodeEqdSlotHideCustom);
    if (!tok || !hideFixed || !hideCustom) return Fail("the slot assignment's loop has an unexpected shape");
    g_slotToken = Rd<uint8_t>(tok, kCodeEqdSlotTokenDisp);
    uint8_t* clear = SigFollowRel32(hideFixed + kCodeEqdSlotHideCall);
    if (clear != SigFollowRel32(hideCustom + kCodeEqdSlotHideCall) || !SigMatchAt(clear, kCodeEqdSlotClear))
        return Fail("the slot window clear calls disagree or have an unexpected shape");
    g_slotOfWindow = Rd<uint32_t>(clear, kCodeEqdSlotClearDisp);

    uint8_t* custom = SigFindUnique("eqd_slots_custom", kSigEqdSlotsCustom);
    uint8_t* fixed = SigFindUnique("eqd_slots_fixed", kSigEqdSlotsFixed);
    if (!custom || !fixed) return false;
    g_vec[0] = Rd<uint32_t>(custom, kSigEqdSlotsVecDisp);
    g_vec[1] = Rd<uint32_t>(fixed, kSigEqdSlotsVecDisp);
    g_container = Rd<uint32_t>(custom, kSigEqdSlotsContainerDisp);
    uint8_t* creator = SigFollowRel32(custom + kSigEqdSlotsCreatorCall);
    if (Rd<uint32_t>(fixed, kSigEqdSlotsContainerDisp) != g_container || SigFollowRel32(fixed + kSigEqdSlotsCreatorCall) != creator)
        return Fail("the two slot window groups are set up differently");
    if (Rd<uint32_t>(hideCustom, kCodeEqdSlotCountVecDisp) != g_vec[0] + 0xc || Rd<uint32_t>(hideFixed, kCodeEqdSlotCountVecDisp) != g_vec[1] + 0xc)
        return Fail("the slot window counts are not where the vectors are");
    uint8_t* look = SigFindIn(creator, 0x200, kCodeEqdSlotLookup);
    uint8_t* place = SigFindIn(creator, 0x400, kCodeEqdSlotPlace);
    if (!look || !place) return Fail("the slot window creator has an unexpected shape");
    g_lookupSlot = Rd<uint32_t>(look, kCodeEqdLookupSlotDisp);
    g_position = Rd<uint32_t>(place, kCodeEqdPositionDisp);
    g_setPosition = (SetPosition_t)SigFollowRel32(place + kCodeEqdSetPositionCall);
    g_isMissing = (IsMissing_t)SigFollowRel32(place + kCodeEqdIsMissingCall);
    if (!SigMatchAt((uint8_t*)g_setPosition, kCodeEqdSetPosition)) return Fail("the slot window's position setter has an unexpected shape");
    g_windowElement = Rd<uint32_t>((uint8_t*)g_setPosition, kCodeEqdSlotElementDisp);
    uint8_t* create = SigFindIn(creator, 0x400, kCodeEqdSlotNew);
    uint8_t* gui = create ? SigFindIn(SigFollowRel32(create + kCodeEqdSlotCtorCall), 0x200, kCodeEqdSlotGuiBase) : nullptr;
    if (!gui || Rd<uint32_t>(gui, kCodeEqdSlotGuiElementDisp) != g_windowElement)
        return Fail("the slot window's constructor has an unexpected shape");
    g_guiBase = Rd<uint8_t>(gui, kCodeEqdSlotGuiBaseImm);
    if (g_lookupSlot % 8 || g_lookupSlot >= 0x800) return Fail("the container's find slot is implausible");

    uint8_t* roles = AnchorFindLea("eqd_roles", kAnchorEqdRoles);
    uint8_t* store = roles ? SigFindIn(roles, 0x80, kCodeEqdRolesStore) : nullptr;
    if (!store) return Fail("the role dropdown's member was not found");
    g_roles = Rd<uint32_t>(store, kCodeEqdRolesDisp);
    return true;
}

DWORD g_faultCode = 0;
void* g_faultAt = nullptr;
void* g_faultStack[12] = {};
USHORT g_faultFrames = 0;
int FaultFilter(EXCEPTION_POINTERS* ep) {
    g_faultCode = ep->ExceptionRecord->ExceptionCode;
    g_faultAt = ep->ExceptionRecord->ExceptionAddress;
    // The filter runs on top of the faulting frames: they show where it came from.
    g_faultFrames = RtlCaptureStackBackTrace(1, 12, g_faultStack, nullptr);
    return EXCEPTION_EXECUTE_HANDLER;
}

void LogFaultOnce(const char* where) {
    InterlockedExchange(&g_off, 1);
    if (!InterlockedExchange(&g_faultLogged, 1)) {
        Log("ERROR: equipment_designers: fault in the %s (exception 0x%08lX at %s); the game's own designer behaviour from now on",
            where, g_faultCode, DescribeAddress(g_faultAt).c_str());
        for (USHORT i = 0; i < g_faultFrames; ++i) Log("  frame %u: %s", i, DescribeAddress(g_faultStack[i]).c_str());
        if (g_fault) g_fault(where);
    }
}

void* SlotWindow(const void* view, bool fixed, int i) {
    uint32_t v = g_vec[fixed ? 1 : 0];
    void** data = Rd<void**>(view, v);
    int count = Rd<int>(view, v + 0xc);
    return data && i >= 0 && i < count ? data[i] : nullptr;
}

// --- Hooks ---------------------------------------------------------------------------------------

bool ExtraAllowed(void* ctx, const void* type) {
    __try {
        if (!type || !g_find(type)) return false;
        if (TypeSlotCount(type) <= 0 || !TankDesignerUnlocked() || !DesignerLandType(type)) return false;
        return g_layout(ctx, type);
    } __except (FaultFilter(GetExceptionInformation())) {
        LogFaultOnce("designer check");
        return false;
    }
}
bool __fastcall CanOpenDetour(void* ctx, const void* type) {
    bool r = g_canOpenOrig(ctx, type);
    if (r || g_off || !g_find) return r;
    return ExtraAllowed(ctx, type);
}

void AfterOpen(void* view) {
    __try {
        if (g_opened) g_opened(view, Rd<const void*>(view, g_archetype));
    } __except (FaultFilter(GetExceptionInformation())) {
        LogFaultOnce("designer open hook");
    }
}
void __fastcall OpenDetour(void* view, void* variant, void* c, void* d) {
    g_openOrig(view, variant, c, d);
    if (!g_off && view) AfterOpen(view);
}

void AfterRefresh(void* view) {
    __try {
        if (g_refreshed) g_refreshed(view, Rd<const void*>(view, g_archetype));
    } __except (FaultFilter(GetExceptionInformation())) {
        LogFaultOnce("designer refresh hook");
    }
}
void __fastcall RefreshDetour(void* a, void* b, void* c, void* d) {
    g_refreshOrig(a, b, c, d);
    if (!g_off && a) AfterRefresh((uint8_t*)a + g_viewDelta);
}

}  // namespace

void ResolveDesigners() {
    if (!ResolveAll()) {
        g_canOpenFn = nullptr;
        return;
    }
    Log("equipment_designers: can-open %s, open %s, refresh %s (view %d), kind +0x%x, title +0x%x, archetype +0x%x, roles "
        "+0x%x, slot windows +0x%x/+0x%x (slot +0x%x, element +0x%x, GUI object +0x%x), positions +0x%x (find slot +0x%x, position +0x%x), type slots +0x%x/+0x%x "
        "(name +0x%x), tank DLC bit 0x%llx",
        SigDescribe(g_canOpenFn).c_str(), SigDescribe(g_openFn).c_str(), SigDescribe(g_refreshFn).c_str(), g_viewDelta, g_kind,
        g_title, g_archetype, g_roles, g_vec[0], g_vec[1], g_slotOfWindow, g_windowElement, g_guiBase, g_container, g_lookupSlot, g_position, g_typeSlots, g_typeSlotCount,
        g_slotToken, (unsigned long long)g_tankBit);
}

bool HaveDesigners() {
    return g_canOpenFn && g_designable && g_layout && g_dlcMask && g_tankBit && g_landA && g_landB && g_openFn && g_setText &&
           g_kind && g_title && g_refreshFn && g_archetype && g_typeSlots && g_typeSlotCount && g_slotToken && g_slotOfWindow && g_windowElement && g_guiBase &&
           g_vec[0] && g_vec[1] && g_container && g_lookupSlot && g_position && g_setPosition && g_isMissing && g_roles &&
           HaveTokens() && HaveProduction() && TypeArchetypeOffset() && HaveElementVisibility();
}

bool HookDesigners(DesignerFind_t find, DesignerOpened_t opened, DesignerRefreshed_t refreshed, DesignerFault_t fault) {
    g_find = find;
    g_opened = opened;
    g_refreshed = refreshed;
    g_fault = fault;
    if (!HookQueue(g_canOpenFn, (void*)&CanOpenDetour, (void**)&g_canOpenOrig, "eqd_can_open")) return false;
    if (!HookQueue(g_openFn, (void*)&OpenDetour, (void**)&g_openOrig, "eqd_open")) {
        HookUnqueue(g_canOpenFn);
        return false;
    }
    if (!HookQueue(g_refreshFn, (void*)&RefreshDetour, (void**)&g_refreshOrig, "eqd_refresh")) {
        HookUnqueue(g_canOpenFn);
        HookUnqueue(g_openFn);
        return false;
    }
    return true;
}

const void* TypeArchetypeOf(const void* type) {
    const void* a = type ? Rd<const void*>(type, TypeArchetypeOffset()) : nullptr;
    return a ? a : type;
}

bool DesignerLandType(const void* type) { return type && (g_landA(type) || g_landB(type)); }
bool TankDesignerUnlocked() { return (g_dlcMask() & g_tankBit) != 0; }

int TypeSlotCount(const void* type) {
    int n = type ? Rd<int>(type, g_typeSlotCount) : 0;
    return n > 0 && n < 256 && Rd<const void*>(type, g_typeSlots) ? n : 0;
}
const char* TypeSlotName(const void* type, int i) {
    if (i < 0 || i >= TypeSlotCount(type)) return "";
    const uint8_t* slot = Rd<const uint8_t*>(type, g_typeSlots) + kSlotStride * (size_t)i;
    return TokenName(Rd<int>(slot, g_slotToken));
}

bool IsTankDesigner(const void* view) { return view && Rd<int>(view, g_kind) == kTankKind; }

void DesignerSetTitle(void* view, const EngineString* text) {
    void* e = Rd<void*>(view, g_title);
    if (e && text) g_setText(e, text);
}

void DesignerShowRoles(void* view, bool show) { SetElementVisible(Rd<void*>(view, g_roles), show); }

int DesignerSlotWindows(const void* view, bool fixed) {
    int n = Rd<int>(view, g_vec[fixed ? 1 : 0] + 0xc);
    return n > 0 && n <= 64 ? n : 0;
}

void DesignerPlaceSlot(void* view, bool fixed, int i, int x, int y) {
    void* w = SlotWindow(view, fixed, i);
    if (!w) return;
    int64_t pos = (int64_t)(((uint64_t)(uint32_t)y << 32) | (uint32_t)x);
    g_setPosition(w, &pos);
}

bool DesignerResetSlot(void* view, bool fixed, int i) {
    void* w = SlotWindow(view, fixed, i);
    void* container = Rd<void*>(view, g_container);
    if (!w || !container) return false;
    char name[64];
    int n = snprintf(name, sizeof name, fixed ? "pos_fixed_module_slot_window_%d" : "pos_custom_module_slot_window_%d", i);
    if (n <= 0 || n >= (int)sizeof name) return false;
    NameView nv{name, (size_t)n};
    Find_t find = (Find_t)(*(void***)container)[g_lookupSlot / 8];
    void* found = find(container, &nv, 0);
    if (!found || g_isMissing(found)) return false;
    int64_t pos = Rd<int64_t>(found, g_position);
    g_setPosition(w, &pos);
    return true;
}

bool DesignerSlotAssigned(const void* view, bool fixed, int i) {
    const void* w = SlotWindow(view, fixed, i);
    return w && Rd<const void*>(w, g_slotOfWindow) != nullptr;
}

void DesignerShowSlot(void* view, bool fixed, int i, bool show) {
    void* w = SlotWindow(view, fixed, i);
    uint8_t* element = w ? Rd<uint8_t*>(w, g_windowElement) : nullptr;
    if (element) SetElementVisible(element + g_guiBase, show);
}

}  // namespace Engine
