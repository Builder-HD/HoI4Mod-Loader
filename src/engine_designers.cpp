// Engine access for equipment_designers (NOTES.md Part N): which equipment may open a designer
// window (ship, tank or plane), the window's title, role dropdown, slot windows and stats. UI only
// (but `xp`): the save goes through the game's own variant commands, which don't look at the window.
#include <windows.h>
#include <intrin.h>
#include <string.h>

#include <string>
#include <vector>

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

struct StatRange {
    const uint64_t* begin;
    const uint64_t* end;
};
struct StatContext {  // b's 16 bytes and a flag, as the game passes them to Fill
    uint8_t b[16];
    uint8_t one;
    uint8_t pad[7];
};
using Fill_t = void(__fastcall*)(void* panel, const StatRange* range, void* a, const StatContext* ctx, int extra);
using Pool_t = int64_t*(__fastcall*)(void* container, int64_t* out, const void* type);
using Copy_t = void(__fastcall*)(int64_t* out, const void* pool);
using Tooltip_t = void(__fastcall*)(void* a, uint64_t mask, void* c, void* d, void* out);
using Session_t = void*(__fastcall*)(void* session);
using Container_t = void*(__fastcall*)(void* country);
using ElementFind_t = void*(__fastcall*)(void* root, const void* name, int flags);
using XpIcon_t = const char*(__fastcall*)(uint64_t mask);

// Hooked functions and their originals.
uint8_t* g_canOpenFn = nullptr;
CanOpen_t g_canOpenOrig = nullptr;
uint8_t* g_openFn = nullptr;
ViewFn_t g_openOrig = nullptr;
uint8_t* g_refreshFn = nullptr;
ViewFn_t g_refreshOrig = nullptr;
uint8_t* g_statsFn = nullptr;  // the tank filler: not hooked, gives Fill and the checks
uint8_t* g_fillFn = nullptr;
Fill_t g_fillOrig = nullptr;
uint8_t* g_markFn[3] = {};  // the stats dispatcher, the comparison panels, the plane filler: mark the view
ViewFn_t g_markOrig[3] = {};
uint8_t* g_poolFn = nullptr;
Pool_t g_poolOrig = nullptr;
uint8_t* g_isAirFn = nullptr;
TypeTest_t g_isAirOrig = nullptr;
uint8_t* g_isNavalFn = nullptr;
TypeTest_t g_isNavalOrig = nullptr;
uint8_t* g_tooltipFn = nullptr;
Tooltip_t g_tooltipOrig = nullptr;
uint8_t* g_xpIconFn = nullptr;
XpIcon_t g_xpIconOrig = nullptr;
uint8_t* g_buttonsFn = nullptr;
ViewFn_t g_buttonsOrig = nullptr;
using RoleEntry_t = void(__fastcall*)(void* ctx, uint64_t mask);
using RoleLabel_t = EngineString*(__fastcall*)(EngineString* out, uint64_t mask, int token);
uint8_t* g_roleEntryFn = nullptr;
RoleEntry_t g_roleEntryOrig = nullptr;
uint8_t* g_roleLabelFn = nullptr;
RoleLabel_t g_roleLabelOrig = nullptr;
using TypeMask_t = uint64_t(__fastcall*)(const void* type);
TypeMask_t g_typeMaskFn = nullptr;         // a type's mask as roles compare it
uint32_t g_typeMask = 0, g_viewMask = 0;  // (the builder's comparison, checks only); the edited design's mask in the view
uint32_t g_typeBase = 0;                   // a duplicate_archetypes copy -> the type it was made from
const uint64_t kRoleBits = 0x7e000000;
DesignerRoles_t g_rolesFor = nullptr;
DesignerRoleName_t g_roleName = nullptr;
DesignerRefreshed_t g_afterButtons = nullptr;

// Stats.
Fill_t g_fill = nullptr;
StatRange g_vanillaStats[3] = {};  // the tank window's lists (checks only)
// The view whose panels Fill is filling now, and how many it filled (main thread).
struct FillMark {
    void* view;
    int calls;
    DWORD thread;
};
FillMark g_mark = {};
const char* const* g_statKeys = nullptr;  // the game's id -> loc key table
int g_statCount = 0;

// Experience.
Copy_t g_copy = nullptr;
uint32_t g_pool[3] = {0, 0, 0};       // army, navy, air: container offsets
uint64_t g_poolMask[3] = {0, 0, 0};   // the tooltip's army, navy and air masks
void** g_session = nullptr;           // the global the window asks for the player's country
uint32_t g_countrySlot = 0, g_xpText = 0;
Container_t g_container_ = nullptr;
const uint8_t* g_airRet[2] = {nullptr, nullptr};    // create, update: after their IsAir call
const uint8_t* g_navalRet[2] = {nullptr, nullptr};  // after their IsNaval call
volatile LONG g_shownXp = -1;  // the tank window shows a listed design paying from this pool (UI thread)
void* volatile g_shownView = nullptr;

uint8_t* g_pickWindow = nullptr;

// Window elements.
uint32_t g_root = 0, g_rootFindSlot = 0;
uint32_t g_findSlots[8] = {};  // the root's lookup slots (one per element class)
int g_findSlotCount = 0;
void* g_findRoot = nullptr;       // the root the placeholders below belong to
void* g_findMissing[8] = {};      // per slot: what a missing name gives
int g_containerSlot = -1;         // the slot that finds container windows (their element part is at +g_guiBase)

// Called.
TypeTest_t g_designable = nullptr, g_landA = nullptr, g_landB = nullptr, g_navalTest = nullptr, g_airTest = nullptr;
Layout_t g_layout = nullptr;
DlcMask_t g_dlcMask = nullptr;
uint64_t g_windowMask[3] = {0, 0, 0};  // per window kind: its DLC's bits in the DLC designer mask
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

DesignerFind_t g_find = nullptr;
DesignerStats_t g_statsFor = nullptr;
DesignerXp_t g_xpFor = nullptr;
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
    g_windowMask[kDesignerShip] = Rd<uint64_t>(mask, kCodeEqdDlcShipMask);
    g_windowMask[kDesignerTank] = Rd<uint8_t>(mask, kCodeEqdDlcTankBit);
    g_windowMask[kDesignerPlane] = Rd<uint64_t>(mask, kCodeEqdDlcPlaneMask);
    uint64_t tankBit = g_windowMask[kDesignerTank];
    if (!tankBit || (tankBit & (tankBit - 1))) return Fail("the tank designer's DLC bit is not a single bit");
    if (!g_windowMask[kDesignerShip] || !g_windowMask[kDesignerPlane] || (g_windowMask[kDesignerShip] & g_windowMask[kDesignerPlane]) ||
        ((g_windowMask[kDesignerShip] | g_windowMask[kDesignerPlane]) & tankBit))
        return Fail("the ship and plane designers' DLC masks are implausible");

    if (!(p = SigFindUnique("eqd_pick_window", kSigEqdPickWindow))) return false;
    g_pickWindow = p;
    uint32_t designType = Rd<uint32_t>(p, kSigEqdPickTypeDisp);
    if (EquipmentTypeOffset() && designType != EquipmentTypeOffset())
        return Fail("the window picker's design type differs from production_data's");
    g_landA = (TypeTest_t)SigFollowRel32(p + kSigEqdPickLandACall);
    g_landB = (TypeTest_t)SigFollowRel32(p + kSigEqdPickLandBCall);
    // The naval test decides first (+0x23), the air test last (+0x68); land between.
    if (p[0x23] != 0xE8 || p[0x68] != 0xE8) return Fail("the window picker's naval and air tests moved");
    g_navalTest = (TypeTest_t)SigFollowRel32(p + 0x23);
    g_airTest = (TypeTest_t)SigFollowRel32(p + 0x68);

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
    // The kinds: 0 ship, 1 tank, 2 plane (the constants in engine.h).
    const size_t jumps[3] = {kCodeEqdTitleShipJump, kCodeEqdTitleTankJump, kCodeEqdTitlePlaneJump};
    const char* const keys[3] = {"SHIP_DESIGNER_TITLE", "TANK_DESIGNER_TITLE", "PLANE_DESIGNER_TITLE"};
    for (int k = 0; k < 3; ++k) {
        const char* key = RipString(text + jumps[k] + 2 + (int8_t)text[jumps[k] + 1]);
        if (!key || strcmp(key, keys[k]) != 0) return Fail("the window kinds are not ship, tank, plane");
    }

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
    g_root = Rd<uint32_t>(store, kCodeEqdRootDisp);
    g_rootFindSlot = Rd<uint32_t>(store, kCodeEqdRootFindSlot);
    if (g_rootFindSlot % 8 || g_rootFindSlot >= 0x800) return Fail("the window's find slot is implausible");
    // The root's lookup slots, from setup's calls around the roles anchor.
    for (uint8_t* c = roles - 0x800; c && c < roles + 0x2000 && g_findSlotCount < 8; ++c) {
        if (!SigMatchAt(c, kCodeEqdRootFind)) continue;
        uint8_t* call = c + kCodeEqdRootFindCall;
        uint32_t slot = call[1] == 0x50 ? call[2] : call[1] == 0x90 ? Rd<uint32_t>(call, 2) : 0;
        uint8_t* after = call + (call[1] == 0x50 ? 3 : 6);
        // A lookup: the window keeps what it returns (mov [rsi+disp32], rax).
        if (!slot || slot % 8 || slot >= 0x800 || after[0] != 0x48 || after[1] != 0x89 || after[2] != 0x86) continue;
        bool known = false;
        for (int i = 0; i < g_findSlotCount; ++i) known = known || g_findSlots[i] == slot;
        if (!known) g_findSlots[g_findSlotCount++] = slot;
    }
    bool rolesSlot = false;
    for (int i = 0; i < g_findSlotCount; ++i) rolesSlot = rolesSlot || g_findSlots[i] == g_rootFindSlot;
    if (!rolesSlot) g_findSlotCount = 0;  // `hide` unavailable: the idiom didn't match the roles lookup
    // Container windows (e.g. the plane window's role line): the root's container lookup.
    uint8_t* planeRole = AnchorFindLea("eqd_plane_role", kAnchorEqdPlaneRole);
    if (g_findSlotCount && g_findSlotCount < 8 && planeRole && SigMatchAt(planeRole, kCodeEqdRootFindContainer) && planeRole[-10] == 0x48 &&
        planeRole[-9] == 0x8B && planeRole[-8] == 0x8E && Rd<uint32_t>(planeRole - 7, 0) == g_root && planeRole[-3] == 0x48 &&
        planeRole[-2] == 0x8B && planeRole[-1] == 0x01) {
        uint32_t slot = Rd<uint32_t>(planeRole, kCodeEqdRootFindContainerSlot);
        bool known = false;
        for (int i = 0; i < g_findSlotCount; ++i) known = known || g_findSlots[i] == slot;
        if (slot && slot % 8 == 0 && slot < 0x800 && !known && g_guiBase) {
            g_containerSlot = g_findSlotCount;
            g_findSlots[g_findSlotCount++] = slot;
        }
    } else {
        Log("equipment_designers: the window's container lookup was not found; `hide` finds no container windows");
    }
    // Roles: the dropdown builder's entry and label functions, the masks it compares.
    if (uint8_t* build = SigFindUnique("eqd_role_build", kSigEqdRoleBuild)) {
        uint8_t* entry = build[kSigEqdRoleBuildEntryCall] == 0xE8 ? SigFollowRel32(build + kSigEqdRoleBuildEntryCall) : nullptr;
        uint8_t* label = entry && SigMatchAt(entry, kCodeEqdRoleEntry) && entry[kCodeEqdRoleEntryLabelCall] == 0xE8
                             ? SigFollowRel32(entry + kCodeEqdRoleEntryLabelCall)
                             : nullptr;
        uint8_t* cmp = SigFindIn(build - 0x200, 0x200, kCodeEqdRoleTypeMask);
        uint8_t* base = SigFindIn(build - 0x400, 0x400, kCodeEqdRoleBase);
        uint32_t maskDisp = Rd<uint32_t>(build, kSigEqdRoleBuildMaskDisp);
        if (label && SigMatchAt(label, kCodeEqdRoleLabel) && cmp && Rd<uint32_t>(cmp, kCodeEqdRoleTypeMaskDesignDisp) == maskDisp &&
            designType && g_archetype > designType && base && Rd<uint32_t>(base, kCodeEqdRoleBaseTypeDisp) == designType &&
            TypeArchetypeOffset() && Rd<uint32_t>(base, kCodeEqdRoleBaseArchetypeDisp) == TypeArchetypeOffset() &&
            entry[kCodeEqdRoleEntryTypesCall] == 0xE8) {
            g_typeBase = Rd<uint32_t>(base, kCodeEqdRoleBaseDisp);
            uint8_t* types = SigFollowRel32(entry + kCodeEqdRoleEntryTypesCall);
            uint8_t* maskFn = SigMatchAt(types, kCodeEqdDesignTypes) ? SigFollowRel32(types + kCodeEqdDesignTypesJmp) : nullptr;
            if (!maskFn || SigFollowRel32(types + kCodeEqdDesignTypesCall) != maskFn ||
                Rd<uint32_t>(types, kCodeEqdDesignTypesTypeDisp) != designType || Rd<uint8_t>(types, kCodeEqdDesignTypesCountDisp) != g_typeSlotCount ||
                Rd<uint32_t>(types, kCodeEqdDesignTypesBaseDisp) != g_typeBase)
                entry = nullptr;  // `roles` unavailable
            g_typeMaskFn = (TypeMask_t)maskFn;
            g_roleEntryFn = entry;
            g_roleLabelFn = entry ? label : nullptr;
            g_typeMask = Rd<uint32_t>(cmp, kCodeEqdRoleTypeMaskDisp);
            g_viewMask = g_archetype - designType + maskDisp;  // the design in the view: its archetype, its mask
        } else {
            Log("equipment_designers: the role dropdown's functions have an unexpected shape; `roles` is unavailable");
        }
    }
    if ((g_buttonsFn = SigFindUnique("eqd_buttons", kSigEqdButtons)) != nullptr && Rd<uint32_t>(g_buttonsFn, kSigEqdButtonsRootDisp) != g_root) {
        Log("equipment_designers: the buttons update reads another root; `hide` is unavailable");
        g_buttonsFn = nullptr;
    }
    return true;
}

// Stats, experience: resolved apart, so a mismatch there leaves the designers working without them.
bool ResolveStats() {
    uint8_t* p = SigFindUnique("eqd_stats", kSigEqdStats);
    uint8_t* name = SigFindUnique("eqd_stat_name", kSigEqdStatName);
    if (!p || !name) return false;
    const size_t kSigEqdStatsLeas[6] = {kSigEqdStatsLea0, kSigEqdStatsLea1, kSigEqdStatsLea2, kSigEqdStatsLea3, kSigEqdStatsLea4, kSigEqdStatsLea5};
    for (size_t i = 0; i < 6; ++i)
        if (p[kSigEqdStatsLeas[i]] != 0x48 || p[kSigEqdStatsLeas[i] + 1] != 0x8D || p[kSigEqdStatsLeas[i] + 2] != 0x05)
            return Fail("the stats function's arrays have an unexpected shape");
    for (int g = 0; g < 3; ++g) {
        g_vanillaStats[g].begin = (const uint64_t*)SigRipTarget(p + kSigEqdStatsLeas[2 * g], 3, 7);
        g_vanillaStats[g].end = (const uint64_t*)SigRipTarget(p + kSigEqdStatsLeas[2 * g + 1], 3, 7);
        size_t n = (size_t)(g_vanillaStats[g].end - g_vanillaStats[g].begin);
        if (g_vanillaStats[g].end < g_vanillaStats[g].begin || n == 0 || n > 64) return Fail("the stats arrays are implausible");
    }
    if (p[kSigEqdStatsFillCall] != 0xE8) return Fail("the stats function's fill call moved");
    g_fill = (Fill_t)SigFollowRel32(p + kSigEqdStatsFillCall);
    // Who marks the view: the dispatcher and the comparison panels (both reach this tank filler and
    // the same ship filler), and the plane filler (from the dispatcher; it calls Fill itself).
    uint8_t* dispatch = SigFindUnique("eqd_stats_dispatch", kSigEqdStatsDispatch);
    uint8_t* ref = SigFindUnique("eqd_stats_ref", kSigEqdStatsRef);
    if (!dispatch || !ref) return false;
    const size_t calls[4] = {kSigEqdStatsDispatchShipCall, kSigEqdStatsDispatchTankCall, kSigEqdStatsRefShipCall, kSigEqdStatsRefTankCall};
    for (size_t i = 0; i < 4; ++i)
        if ((i < 2 ? dispatch : ref)[calls[i]] != 0xE8) return Fail("the stats dispatcher's calls moved");
    if (Rd<uint32_t>(dispatch, kSigEqdStatsDispatchKindDisp) != g_kind || SigFollowRel32(dispatch + kSigEqdStatsDispatchTankCall) != p ||
        SigFollowRel32(ref + kSigEqdStatsRefTankCall) != p ||
        SigFollowRel32(dispatch + kSigEqdStatsDispatchShipCall) != SigFollowRel32(ref + kSigEqdStatsRefShipCall))
        return Fail("the stats dispatcher and the comparison panels don't reach the same fillers");
    uint8_t* planeCall = SigFindIn(dispatch, 0x300, kCodeEqdStatsPlane);
    uint8_t* plane = planeCall ? SigFollowRel32(planeCall + kCodeEqdStatsPlaneCall) : nullptr;
    int planeFills = 0;
    for (uint8_t* c = plane; c && c < plane + 0x180; ++c)
        if (c[0] == 0xE8 && SigFollowRel32(c) == (uint8_t*)g_fill) ++planeFills;
    if (planeFills != 3) return Fail("the plane window's stats don't fill three panels");
    g_markFn[0] = dispatch;
    g_markFn[1] = ref;
    g_markFn[2] = plane;
    g_statKeys = (const char* const*)SigRipTarget(name + kSigEqdStatNameTableLea, 3, 7);
    uint8_t* rdata = nullptr;
    size_t rsize = 0;
    if (!ImageSection(".rdata", &rdata, &rsize)) return false;
    g_statCount = 0;
    while (g_statCount < 256) {
        const char* k = g_statKeys[g_statCount];
        if ((const uint8_t*)k < rdata || (const uint8_t*)k + 6 > rdata + rsize || strncmp(k, "STAT_", 5) != 0) break;
        ++g_statCount;
    }
    if (g_statCount < 8) return Fail("the stat loc key table is implausible");
    for (int g = 0; g < 3; ++g)  // the game's own entries must be stats of that table
        for (const uint64_t* e = g_vanillaStats[g].begin; e < g_vanillaStats[g].end; ++e)
            if ((int8_t)(*e >> 32) == 0 && (int)(uint32_t)*e >= g_statCount) return Fail("a stats array names an unknown stat");
    g_statsFn = p;
    g_fillFn = (uint8_t*)g_fill;
    return true;
}

bool ResolveXp(uint8_t* pickWindow) {
    uint8_t* pool = SigFindUnique("eqd_pool", kSigEqdPool);
    uint8_t* shown = SigFindUnique("eqd_xp_shown", kSigEqdXpShown);
    uint8_t* tip = SigFindUnique("eqd_tooltip", kSigEqdTooltip);
    if (!pool || !shown || !tip) return false;
    uint8_t* army = SigFindIn(pool, 0x100, kCodeEqdPoolArmy);
    if (!army) return Fail("the pool's land branch was not found");
    g_pool[0] = Rd<uint8_t>(army, kCodeEqdPoolArmyDisp);
    g_pool[1] = Rd<uint8_t>(pool, kSigEqdPoolNavyDisp);
    g_pool[2] = Rd<uint8_t>(pool, kSigEqdPoolAirDisp);
    g_copy = (Copy_t)SigFollowRel32(army + kCodeEqdPoolCopyCall);
    // IsNaval / IsAir: the window picker's calls (+0x23, +0x68) must be the pool's.
    g_isNavalFn = SigFollowRel32(pickWindow + 0x23);
    g_isAirFn = pickWindow[0x68] == 0xE8 ? SigFollowRel32(pickWindow + 0x68) : nullptr;
    if (!g_isAirFn || SigFollowRel32(pool + kSigEqdPoolNavalCall) != g_isNavalFn || SigFollowRel32(pool + kSigEqdPoolAirCall) != g_isAirFn)
        return Fail("the pool's naval and air tests are not the window picker's");
    if (Rd<uint8_t>(shown, kSigEqdXpArmyDisp) != g_pool[0]) return Fail("the window's experience line reads another pool");
    g_session = (void**)SigRipTarget(shown, kSigEqdXpSessionRel, kSigEqdXpSessionRel + 4);
    g_countrySlot = Rd<uint32_t>(shown, kSigEqdXpCountrySlot);
    g_container_ = (Container_t)SigFollowRel32(shown + kSigEqdXpContainerCall);
    g_xpText = Rd<uint32_t>(shown, kSigEqdXpTextDisp);
    uint8_t* m = tip;
    for (int i = 0; i < 3; ++i) {
        m = SigFindIn(m, 0x300, kCodeEqdTooltipMask);
        if (!m) return Fail("the tooltip's masks were not found");
        g_poolMask[i] = Rd<uint64_t>(m, 2);
        m += 10;
    }
    if (!g_poolMask[0] || !g_poolMask[1] || !g_poolMask[2] || (g_poolMask[0] & g_poolMask[1]) || (g_poolMask[0] & g_poolMask[2]) ||
        (g_poolMask[1] & g_poolMask[2]))
        return Fail("the tooltip's masks are implausible");
    // The icon name function: its three names must be the experience icons.
    uint8_t* icon = SigFindUnique("eqd_xp_icon", kSigEqdXpIcon);
    if (!icon) return false;
    int names = 0;
    for (uint8_t* c = icon; c < icon + 0x120; ++c)
        if ((c[0] == 0x48 || c[0] == 0x4C) && c[1] == 0x8D && (c[2] & 0xC7) == 0x05) {
            const char* s = (const char*)SigRipTarget(c, 3, 7);
            uint8_t* rdata = nullptr;
            size_t rsize = 0;
            if (ImageSection(".rdata", &rdata, &rsize) && (const uint8_t*)s >= rdata && (const uint8_t*)s + 16 < rdata + rsize &&
                (!strcmp(s, "army_experience") || !strcmp(s, "navy_experience") || !strcmp(s, "air_experience")))
                ++names;
        }
    if (names < 3) return Fail("the experience icon function has an unexpected shape");
    g_xpIconFn = icon;
    // The commands' execute: their IsAir / IsNaval calls.
    const char* const classes[2] = {".?AVCCreateEquipmentVariantCommand@@", ".?AVCUpdateEquipmentVariantCommand@@"};
    for (int k = 0; k < 2; ++k) {
        size_t slots = 0;
        void** vt = FindVtableByRtti(classes[k], &slots);
        uint8_t* exec = vt && slots > 10 ? (uint8_t*)vt[10] : nullptr;
        if (!exec) return Fail("a variant command's execute was not found");
        uint8_t *air = nullptr, *naval = nullptr;
        size_t call = 0;
        if (k == 0) {
            air = SigFindIn(exec, 0x1000, kCodeEqdCreateAir);
            naval = SigFindIn(exec, 0x1000, kCodeEqdCreateNaval);
            call = kCodeEqdCreateCall;
        } else {
            air = SigFindIn(exec, 0x1000, kCodeEqdUpdateTest);
            naval = air ? SigFindIn(air + 1, 0x200, kCodeEqdUpdateTest) : nullptr;
            call = kCodeEqdUpdateCall;
        }
        if (!air || !naval || SigFollowRel32(air + call) != g_isAirFn || SigFollowRel32(naval + call) != g_isNavalFn || naval < air)
            return Fail("a variant command's experience tests have an unexpected shape");
        g_airRet[k] = air + call + 5;
        g_navalRet[k] = naval + call + 5;
    }
    g_poolFn = pool;
    g_tooltipFn = tip;
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
        int kind = type ? g_find(type) : -1;
        if (kind < 0 || TypeSlotCount(type) <= 0 || !DesignerWindowUnlocked(kind) || DesignerWindowOf(type) != kind) return false;
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

void AfterButtons(void* view) {
    __try {
        g_afterButtons(view, Rd<const void*>(view, g_archetype));
    } __except (FaultFilter(GetExceptionInformation())) {
        LogFaultOnce("buttons hook");
    }
}
void __fastcall ButtonsDetour(void* view, void* b, void* c, void* d) {
    g_buttonsOrig(view, b, c, d);
    if (!g_off && g_afterButtons && view) AfterButtons(view);
}

// Stats: while a marked function fills a view's panels, Fill's n-th call is panel n (base, combat,
// misc); a listed archetype with that panel in `stats` gets the definition's rows.
bool CustomRows(void* view, int panel, StatRange* r) {
    __try {
        const void* arch = Rd<const void*>(view, g_archetype);
        int n = 0;
        uint64_t mask = g_viewMask ? Rd<uint64_t>(view, g_viewMask) : 0;
        const uint64_t* e = arch ? g_statsFor(Rd<int>(view, g_kind), arch, mask, panel, &n) : nullptr;
        if (!e) return false;
        *r = StatRange{e, e + n};
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        LogFaultOnce("stats hook");
        return false;
    }
}
void __fastcall FillDetour(void* panel, const StatRange* range, void* a, const StatContext* ctx, int extra) {
    if (!g_off && g_statsFor && g_mark.view && g_mark.thread == GetCurrentThreadId()) {
        int n = g_mark.calls++;
        StatRange r;
        if (n < 3 && CustomRows(g_mark.view, n, &r)) {
            g_fillOrig(panel, &r, a, ctx, extra);
            return;
        }
    }
    g_fillOrig(panel, range, a, ctx, extra);
}
template <int I>
void __fastcall MarkDetour(void* view, void* b, void* c, void* d) {
    FillMark saved = g_mark;
    g_mark = FillMark{view, 0, GetCurrentThreadId()};
    g_markOrig[I](view, b, c, d);
    g_mark = saved;
}

// Experience: the definition's pool for listed archetypes, wherever the game asks for the pool, and
// in the two commands' deduction tests (by return address; every other caller gets the game's answer).
int XpSafe(const void* type) {
    __try {
        return type ? g_xpFor(type) : -1;
    } __except (FaultFilter(GetExceptionInformation())) {
        LogFaultOnce("experience hook");
        return -1;
    }
}
int64_t* __fastcall PoolDetour(void* container, int64_t* out, const void* type) {
    if (!g_off && g_xpFor && container && out) {
        int x = XpSafe(type);
        if (x >= 0 && x < 3) {
            g_copy(out, (const uint8_t*)container + g_pool[x]);
            return out;
        }
    }
    return g_poolOrig(container, out, type);
}
bool __fastcall IsAirDetour(const void* type) {
    const uint8_t* ra = (const uint8_t*)_ReturnAddress();
    if ((ra == g_airRet[0] || ra == g_airRet[1]) && !g_off && g_xpFor) {
        int x = XpSafe(type);
        if (x >= 0) return x == 2;
    }
    return g_isAirOrig(type);
}
bool __fastcall IsNavalDetour(const void* type) {
    const uint8_t* ra = (const uint8_t*)_ReturnAddress();
    if ((ra == g_navalRet[0] || ra == g_navalRet[1]) && !g_off && g_xpFor) {
        int x = XpSafe(type);
        if (x >= 0) return x == 1;
    }
    return g_isNavalOrig(type);
}
// Roles: the dropdown builder's first call (the design's mask without role bits) lists the
// definition's roles instead, each through the game's own entry; its role-bit calls are skipped.
const uint64_t* RolesSafe(void* ctx, int* n) {
    __try {
        void* view = Rd<void*>(ctx, 0);
        const void* arch = view && DesignerKind(view) == kDesignerTank ? Rd<const void*>(view, g_archetype) : nullptr;
        return arch ? g_rolesFor(arch, n) : nullptr;
    } __except (FaultFilter(GetExceptionInformation())) {
        LogFaultOnce("role list hook");
        return nullptr;
    }
}
void __fastcall RoleEntryDetour(void* ctx, uint64_t mask) {
    int n = 0;
    const uint64_t* roles = !g_off && g_rolesFor && ctx ? RolesSafe(ctx, &n) : nullptr;
    if (!roles) {
        g_roleEntryOrig(ctx, mask);
        return;
    }
    if (mask & kRoleBits) return;  // the game's fixed roles
    for (int i = 0; i < n; ++i) g_roleEntryOrig(ctx, roles[i]);
}
const char* RoleNameSafe(uint64_t mask, int token) {
    __try {
        return g_roleName(token, mask);
    } __except (FaultFilter(GetExceptionInformation())) {
        LogFaultOnce("role name hook");
        return nullptr;
    }
}
EngineString* __fastcall RoleLabelDetour(EngineString* out, uint64_t mask, int token) {
    const char* key = !g_off && g_roleName ? RoleNameSafe(mask, token) : nullptr;
    if (!key) return g_roleLabelOrig(out, mask, token);
    ConstructEngineString(out, key, strlen(key));
    return out;
}

// While the tank window is open on a listed design with `xp`, the cost texts (tooltip and icon,
// called only by the designer and its module picker) get one bit of that pool's mask.
bool ShownPoolMask(uint64_t* mask) {
    LONG x = g_shownXp;
    void* view = g_shownView;
    if (x < 0 || x > 2 || !view || g_off) return false;
    __try {
        void* root = Rd<void*>(view, g_root);
        if (!root || !ElementVisible(root)) return false;
    } __except (FaultFilter(GetExceptionInformation())) {
        LogFaultOnce("experience text hook");
        return false;
    }
    *mask = g_poolMask[x] & (0 - g_poolMask[x]);
    return true;
}
void __fastcall TooltipDetour(void* a, uint64_t mask, void* c, void* d, void* out) {
    ShownPoolMask(&mask);
    g_tooltipOrig(a, mask, c, d, out);
}
const char* __fastcall XpIconDetour(uint64_t mask) {
    ShownPoolMask(&mask);
    return g_xpIconOrig(mask);
}

}  // namespace

void ResolveDesigners() {
    if (!ResolveAll()) {
        g_canOpenFn = nullptr;
        return;
    }
    if (!ResolveStats()) g_statsFn = nullptr;
    if (!ResolveXp(g_pickWindow)) g_poolFn = nullptr;
    Log("equipment_designers: element lookups %d (root +0x%x)", g_findSlotCount, g_root);
    Log("equipment_designers: stats %s (%d stat keys, fill %s), experience %s (pools +0x%x/+0x%x/+0x%x, icon %s), buttons %s",
        g_statsFn ? SigDescribe(g_statsFn).c_str() : "unavailable", g_statCount, g_fill ? SigDescribe((void*)g_fill).c_str() : "-",
        g_poolFn ? SigDescribe(g_poolFn).c_str() : "unavailable", g_pool[0], g_pool[1], g_pool[2],
        g_xpIconFn ? SigDescribe(g_xpIconFn).c_str() : "-", g_buttonsFn ? SigDescribe(g_buttonsFn).c_str() : "unavailable");
    Log("equipment_designers: can-open %s, open %s, refresh %s (view %d), kind +0x%x, title +0x%x, archetype +0x%x, roles "
        "+0x%x, slot windows +0x%x/+0x%x (slot +0x%x, element +0x%x, GUI object +0x%x), positions +0x%x (find slot +0x%x, position +0x%x), type slots +0x%x/+0x%x "
        "(name +0x%x), DLC masks ship 0x%llx tank 0x%llx plane 0x%llx",
        SigDescribe(g_canOpenFn).c_str(), SigDescribe(g_openFn).c_str(), SigDescribe(g_refreshFn).c_str(), g_viewDelta, g_kind,
        g_title, g_archetype, g_roles, g_vec[0], g_vec[1], g_slotOfWindow, g_windowElement, g_guiBase, g_container, g_lookupSlot, g_position, g_typeSlots, g_typeSlotCount,
        g_slotToken, (unsigned long long)g_windowMask[0], (unsigned long long)g_windowMask[1], (unsigned long long)g_windowMask[2]);
}

bool HaveDesigners() {
    return g_canOpenFn && g_designable && g_layout && g_dlcMask && g_windowMask[1] && g_landA && g_landB && g_navalTest && g_airTest &&
           g_openFn && g_setText &&
           g_kind && g_title && g_refreshFn && g_archetype && g_typeSlots && g_typeSlotCount && g_slotToken && g_slotOfWindow && g_windowElement && g_guiBase &&
           g_vec[0] && g_vec[1] && g_container && g_lookupSlot && g_position && g_setPosition && g_isMissing && g_roles &&
           HaveTokens() && HaveProduction() && TypeArchetypeOffset() && HaveElementVisibility();
}

bool HaveDesignerButtons() { return g_buttonsFn != nullptr && g_findSlotCount > 0; }
bool HaveDesignerStats() { return g_statsFn && g_fillFn && g_markFn[0] && g_markFn[1] && g_markFn[2] && g_statKeys && g_statCount; }
bool HaveDesignerXp() {
    return g_poolFn && g_copy && g_isAirFn && g_isNavalFn && g_tooltipFn && g_xpIconFn && g_session && g_container_ && g_xpText && g_airRet[0] &&
           g_airRet[1] && g_navalRet[0] && g_navalRet[1];
}

bool HaveDesignerRoles() { return g_roleEntryFn && g_roleLabelFn && g_typeMaskFn && g_viewMask && g_typeBase; }

uint64_t DesignerTypeMask(const void* type) { return type && g_typeMaskFn ? g_typeMaskFn(type) : 0; }

bool HookDesignerRoles(DesignerRoles_t rolesFor, DesignerRoleName_t roleName) {
    if (!HaveDesignerRoles()) return false;
    if (!HookQueue(g_roleEntryFn, (void*)&RoleEntryDetour, (void**)&g_roleEntryOrig, "eqd_role_entry")) return false;
    if (!HookQueue(g_roleLabelFn, (void*)&RoleLabelDetour, (void**)&g_roleLabelOrig, "eqd_role_label")) {
        HookUnqueue(g_roleEntryFn);
        return false;
    }
    g_rolesFor = rolesFor;
    g_roleName = roleName;
    return true;
}

bool HookDesignerExtras(DesignerStats_t statsFor, DesignerXp_t xpFor, DesignerRefreshed_t afterButtons) {
    struct One {
        uint8_t* target;
        void* detour;
        void** orig;
        const char* name;
    };
    std::vector<One> hooks;
    if (statsFor && HaveDesignerStats()) {
        hooks.push_back({g_fillFn, (void*)&FillDetour, (void**)&g_fillOrig, "eqd_stats_fill"});
        hooks.push_back({g_markFn[0], (void*)&MarkDetour<0>, (void**)&g_markOrig[0], "eqd_stats_dispatch"});
        hooks.push_back({g_markFn[1], (void*)&MarkDetour<1>, (void**)&g_markOrig[1], "eqd_stats_ref"});
        hooks.push_back({g_markFn[2], (void*)&MarkDetour<2>, (void**)&g_markOrig[2], "eqd_stats_plane"});
    }
    if (xpFor && HaveDesignerXp()) {
        hooks.push_back({g_poolFn, (void*)&PoolDetour, (void**)&g_poolOrig, "eqd_pool"});
        hooks.push_back({g_isAirFn, (void*)&IsAirDetour, (void**)&g_isAirOrig, "eqd_is_air"});
        hooks.push_back({g_isNavalFn, (void*)&IsNavalDetour, (void**)&g_isNavalOrig, "eqd_is_naval"});
        hooks.push_back({g_tooltipFn, (void*)&TooltipDetour, (void**)&g_tooltipOrig, "eqd_tooltip"});
        hooks.push_back({g_xpIconFn, (void*)&XpIconDetour, (void**)&g_xpIconOrig, "eqd_xp_icon"});
    }
    if (afterButtons && g_buttonsFn) hooks.push_back({g_buttonsFn, (void*)&ButtonsDetour, (void**)&g_buttonsOrig, "eqd_buttons"});
    for (size_t i = 0; i < hooks.size(); ++i)
        if (!HookQueue(hooks[i].target, hooks[i].detour, hooks[i].orig, hooks[i].name)) {
            for (size_t j = 0; j < i; ++j) HookUnqueue(hooks[j].target);
            return false;
        }
    if (statsFor && HaveDesignerStats()) g_statsFor = statsFor;
    if (xpFor && HaveDesignerXp()) g_xpFor = xpFor;
    if (afterButtons && g_buttonsFn) g_afterButtons = afterButtons;
    return true;
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

const void* DesignerBaseArchetype(const void* type) {
    const void* a = TypeArchetypeOf(type);
    const void* base = a && g_typeBase ? Rd<const void*>(a, g_typeBase) : nullptr;  // as the role dropdown reads it
    return base ? TypeArchetypeOf(base) : a;
}

int DesignerWindowOf(const void* type) {
    if (!type) return -1;
    if (g_navalTest(type)) return kDesignerShip;
    if (g_landA(type) || g_landB(type)) return kDesignerTank;
    if (g_airTest(type)) return kDesignerPlane;
    return -1;
}
bool DesignerWindowUnlocked(int kind) { return kind >= 0 && kind < 3 && (g_dlcMask() & g_windowMask[kind]) != 0; }
bool DesignerDesignable(const void* type) { return type && g_designable(type); }

int TypeSlotCount(const void* type) {
    int n = type ? Rd<int>(type, g_typeSlotCount) : 0;
    return n > 0 && n < 256 && Rd<const void*>(type, g_typeSlots) ? n : 0;
}
const char* TypeSlotName(const void* type, int i) {
    if (i < 0 || i >= TypeSlotCount(type)) return "";
    const uint8_t* slot = Rd<const uint8_t*>(type, g_typeSlots) + kSlotStride * (size_t)i;
    return TokenName(Rd<int>(slot, g_slotToken));
}

int DesignerKind(const void* view) {
    int k = view ? Rd<int>(view, g_kind) : -1;
    return k >= 0 && k < 3 ? k : -1;
}

void DesignerSetTitle(void* view, const EngineString* text) {
    void* e = Rd<void*>(view, g_title);
    if (!e || !text) return;
    // SetText takes the string's buffer (the game passes a temporary): give it a copy, so the
    // caller's text stays intact for the next open (in-game 2026-11-15: blank title on reopening).
    EngineString s;
    ConstructEngineString(&s, text->c_str(), (size_t)text->size);
    g_setText(e, &s);
    FreeEngineString(&s);
}

void DesignerShowRoles(void* view, bool show) {
    if (DesignerKind(view) == kDesignerTank) SetElementVisible(Rd<void*>(view, g_roles), show);
}

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

int StatIdByKey(const char* key) {
    for (int i = 0; key && i < g_statCount; ++i)
        if (strcmp(g_statKeys[i], key) == 0) return i;
    return -1;
}

void DesignerSetShownXp(void* view, int pool) {
    g_shownView = view;
    InterlockedExchange(&g_shownXp, pool >= 0 && pool < 3 ? pool : -1);
}

bool DesignerShowXp(void* view, int pool) {
    static const char* const kKeys[3] = {"DESIGNER_ARMY_EXPERIENCE", "DESIGNER_NAVY_EXPERIENCE", "DESIGNER_AIR_EXPERIENCE"};
    if (pool < 0 || pool > 2 || !HaveDesignerXp() || !*g_session) return false;
    void* session = *g_session;
    void* country = ((Session_t)(*(void***)session)[g_countrySlot / 8])(session);
    void* container = country ? g_container_(country) : nullptr;
    void* element = Rd<void*>(view, g_xpText);
    if (!container || !element) return false;
    int64_t amount = 0;
    g_copy(&amount, (const uint8_t*)container + g_pool[pool]);
    char value[32];
    snprintf(value, sizeof value, "%lld", (long long)(amount / 100000));
    LocalizeArg arg{"VALUE", value};
    std::string text;
    if (!LocalizeWithArgs(kKeys[pool], &arg, 1, &text)) return false;
    EngineString s;
    ConstructEngineString(&s, text.data(), text.size());
    g_setText(element, &s);
    FreeEngineString(&s);
    return true;
}

void* DesignerElement(void* view, const char* name) {
    void* root = view ? Rd<void*>(view, g_root) : nullptr;
    if (!root || !name || !*name || !g_findSlotCount) return nullptr;
    void** vt = *(void***)root;
    if (root != g_findRoot) {  // each slot's placeholder for a name the window can't have
        static const char kNone[] = "hoi4_loader_no_such_element";
        NameView none{kNone, sizeof kNone - 1};
        for (int i = 0; i < g_findSlotCount; ++i) g_findMissing[i] = ((ElementFind_t)vt[g_findSlots[i] / 8])(root, &none, 0);
        g_findRoot = root;
    }
    NameView nv{name, strlen(name)};
    for (int i = 0; i < g_findSlotCount; ++i) {
        void* e = ((ElementFind_t)vt[g_findSlots[i] / 8])(root, &nv, 0);
        // A container window is shown and hidden through its element part, as the game does.
        if (e && e != g_findMissing[i]) return i == g_containerSlot ? (uint8_t*)e + g_guiBase : e;
    }
    return nullptr;
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
