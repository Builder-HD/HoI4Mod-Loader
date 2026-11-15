// Engine access for production_tabs (NOTES.md Part O): which designs the production view's "add
// production line" tabs list, the mod's own tab buttons and every button's state. UI only: the
// add-line command checks the country and the design, never the tab.
#include <windows.h>
#include <string.h>

#include <functional>
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
template <typename T>
void Wr(void* base, size_t offset, T v) {
    memcpy((uint8_t*)base + offset, &v, sizeof v);
}

struct NameView {
    const char* p;
    size_t n;
};
using Find_t = void*(__fastcall*)(void* container, const NameView* name, int report);
using Root_t = void*(__fastcall*)(void* view);
using Open_t = void(__fastcall*)(void* view, uint64_t mask, uint8_t naval, void* window, void* button);
using List_t = void(__fastcall*)(void* window, const void* designs, int* used);
using States_t = void(__fastcall*)(void* view, void* open);
using Setup_t = void(__fastcall*)(void* view, void* b, void* c, void* d);
using MaskTest_t = bool(__fastcall*)(uint64_t mask);
using ElementFn_t = void(__fastcall*)(void* element);
using Frame_t = void(__fastcall*)(void* element, int frame);

// The game's own design array header: {data, capacity, count} (the lists read data and count).
struct DesignArray {
    void* const* data;
    int capacity;
    int count;
};

// Hooked functions and their originals.
uint8_t* g_setupFn = nullptr;
Setup_t g_setupOrig = nullptr;
uint8_t* g_statesFn = nullptr;
States_t g_statesOrig = nullptr;
uint8_t* g_listFn[2] = {nullptr, nullptr};  // land/air, naval
List_t g_listOrig[2] = {nullptr, nullptr};

// Called.
using TypeByName_t = uint64_t(__fastcall*)(const EngineString* name);
TypeByName_t g_typeByName = nullptr;
int g_typesChecked = 0;  // 0 not yet, 1 passed, -1 failed
Open_t g_open = nullptr;
Root_t g_root = nullptr;
MaskTest_t g_canHave = nullptr, g_hasFactories = nullptr;

// The view's element lookups.
uint32_t g_containerSlot = 0, g_buttonSlot = 0, g_iconSlot = 0;
std::string g_outerName, g_topName;  // the containers setup walks down to the buttons

// The vanilla tabs, by Ptab index (0 infantry, 1 armor, 2 air, 3 naval).
struct Vanilla {
    const char* button;
    uint32_t buttonDisp = 0, glowDisp = 0, windowDisp = 0;
    uint64_t listMask = 0, stateMask = 0;  // the click's (what the list shows), the state's (what lights the button)
    bool naval = false;
};
Vanilla g_vanilla[4] = {{"infantry_button"}, {"tank_button"}, {"aircraft_button"}, {"naval_button"}};

// Fields.
uint32_t g_prod = 0, g_designs = 0, g_count = 0, g_designMask = 0, g_new = 0, g_designType = 0, g_buildable = 0;
uint32_t g_windowMask = 0;
uint32_t g_enableSlot = 0, g_disableSlot = 0, g_frameSlot = 0, g_showSlot = 0, g_hideSlot = 0, g_flags = 0;
int g_openFrame = 0;
uint8_t g_glowBit = 0;
const int kFrameEnabled = 1, kFrameDisabled = 3;  // as the state function passes them (checked in Resolve)
const int kOwnBits = 48;                           // new tab t has mask bit 63 - t; type bits stay below

// Runtime.
bool g_changed[4] = {false, false, false, false};
struct NewTab {
    std::string name, button, glow;
    void* buttonEl = nullptr;  // in g_view
    void* glowEl = nullptr;
    bool missingLogged = false;
};
std::vector<NewTab> g_tabs;
void* g_view = nullptr;  // the production view the new buttons are bound in
PtabMember_t g_member = nullptr;
PtabFault_t g_fault = nullptr;
volatile LONG g_off = 0, g_faultLogged = 0;
bool g_ok = false;
std::vector<void*> g_scratch;  // a list's members (main thread, lists don't nest)

bool Fail(const char* why) {
    Log("production_tabs: %s", why);
    return false;
}

const char* RipString(const uint8_t* lea) {
    if (!lea || (lea[0] != 0x48 && lea[0] != 0x4C) || lea[1] != 0x8D) return nullptr;
    return (const char*)SigRipTarget((uint8_t*)lea, 3, 7);
}

// --- Resolve -------------------------------------------------------------------------------------

struct Lookup {
    const char* name;
    uint32_t slot, disp;
};

bool ResolveSetup(uint8_t* setup, std::vector<Lookup>* found) {
    uint8_t* r = SigFindIn(setup, 0x100, kCodePtabRoot);
    if (!r) return Fail("the production view's setup has no root lookup");
    g_root = (Root_t)SigFollowRel32(r + kCodePtabRootCall);
    g_containerSlot = Rd<uint32_t>(r, kCodePtabRootSlot);
    uint8_t* t = SigFindIn(r, 0x100, kCodePtabTop);
    const char* outer = RipString(r + kCodePtabRootName);
    const char* top = t ? RipString(t + kCodePtabTopName) : nullptr;
    if (!t || !outer || !top || Rd<uint32_t>(t, kCodePtabTopSlot) != g_containerSlot || g_containerSlot % 8 || g_containerSlot >= 0x800)
        return Fail("the production view's container lookups have an unexpected shape");
    g_outerName = outer;
    g_topName = top;
    for (uint8_t* c = setup; c < setup + kCodePtabFindRange; ++c) {
        Lookup l{nullptr, 0, 0};
        if (SigMatchAt(c, kCodePtabFindA))
            l = {(const char*)SigRipTarget(c + 7, 3, 7), c[6], Rd<uint32_t>(c, 0x33)};
        else if (SigMatchAt(c, kCodePtabFindB))
            l = {RipString(c + 3), c[0x2b], Rd<uint32_t>(c, 0x2f)};
        else if (SigMatchAt(c, kCodePtabFindC))
            l = {RipString(c + 3), Rd<uint32_t>(c, 0x2b), Rd<uint32_t>(c, 0x32)};
        if (l.name) found->push_back(l);
    }
    return true;
}

const Lookup* Named(const std::vector<Lookup>& found, const std::string& name) {
    for (const Lookup& l : found)
        if (name == l.name) return &l;
    return nullptr;
}

bool ResolveAll() {
    uint8_t* setup = SigFindUnique("ptab_setup", kSigPtabSetup);
    uint8_t* clicks = SigFindUnique("ptab_clicks", kSigPtabClicks);
    uint8_t* states = SigFindUnique("ptab_states", kSigPtabStates);
    uint8_t* land = SigFindUnique("ptab_list_land", kSigPtabListLand);
    uint8_t* naval = SigFindUnique("ptab_list_naval", kSigPtabListNaval);
    if (!setup || !clicks || !states || !land || !naval) return false;
    if (!EquipmentTypeOffset() || !TypeArchetypeOffset()) return Fail("production_data's type offsets are unresolved");

    // The vanilla buttons and glow icons by name, as setup finds and keeps them.
    std::vector<Lookup> found;
    if (!ResolveSetup(setup, &found)) return false;
    for (int i = 0; i < 4; ++i) {
        Vanilla& v = g_vanilla[i];
        const Lookup* b = Named(found, v.button);
        const Lookup* g = Named(found, std::string(v.button) + "_glow");
        if (!b || !g) return Fail("a vanilla tab button or its glow icon is not looked up in the setup");
        if ((g_buttonSlot && b->slot != g_buttonSlot) || (g_iconSlot && g->slot != g_iconSlot))
            return Fail("the tab buttons are looked up through different slots");
        g_buttonSlot = b->slot;
        g_iconSlot = g->slot;
        v.buttonDisp = b->disp;
        v.glowDisp = g->disp;
    }
    if (g_buttonSlot % 8 || g_iconSlot % 8 || g_buttonSlot >= 0x800 || g_iconSlot >= 0x800) return Fail("the element lookup slots are implausible");

    // The click handlers: each tab's list mask, window and naval flag; one Open function.
    for (size_t k = 0; k < 4; ++k) {
        uint8_t* th = clicks + kSigPtabClickThunk[k];
        uint32_t button = Rd<uint32_t>(th, kSigPtabThunkButtonDisp);
        bool wide = th[kSigPtabThunkMaskOp] == 0x48;
        uint64_t mask = wide ? Rd<uint64_t>(th, kSigPtabThunkMaskOp + 2) : Rd<uint32_t>(th, kSigPtabThunkMaskOp + 1);
        uint8_t* call = th + (wide ? 0x24 : 0x1f);
        Open_t open = (Open_t)SigFollowRel32(call);
        if (g_open && open != g_open) return Fail("the tab buttons open different functions");
        g_open = open;
        Vanilla* v = nullptr;
        for (Vanilla& c : g_vanilla)
            if (c.buttonDisp == button) v = &c;
        if (!v || v->listMask) return Fail("a click handler belongs to no (or a second time to the same) tab button");
        v->listMask = mask;
        v->windowDisp = Rd<uint32_t>(th, kSigPtabThunkWindowDisp);
        v->naval = th[kSigPtabThunkNaval] == 0x41;
    }
    // The state function: each tab's state mask and glow.
    uint8_t* stateFn = nullptr;
    for (size_t k = 0; k < 4; ++k) {
        uint32_t button = Rd<uint32_t>(states, kSigPtabStateButton[k]);
        uint64_t mask = k < 2 ? Rd<uint32_t>(states, kSigPtabStateMask[k]) : Rd<uint64_t>(states, kSigPtabStateMask[k]);
        uint8_t* fn = SigFollowRel32(states + kSigPtabStateCall[k]);
        if (stateFn && fn != stateFn) return Fail("the button states go to different functions");
        stateFn = fn;
        Vanilla* v = nullptr;
        for (Vanilla& c : g_vanilla)
            if (c.buttonDisp == button) v = &c;
        if (!v || v->stateMask || Rd<uint32_t>(states, kSigPtabStateGlow[k]) != v->glowDisp)
            return Fail("a button state belongs to no tab, or reads another glow icon");
        v->stateMask = mask;
    }
    for (int i = 0; i < 4; ++i) {
        const Vanilla& v = g_vanilla[i];
        if (!v.listMask || !v.stateMask || (v.listMask >> kOwnBits) || (v.stateMask >> kOwnBits) || v.naval != (i == 3))
            return Fail("a vanilla tab's masks or window are implausible");
        for (int j = 0; j < i; ++j)
            if (g_vanilla[j].listMask == v.listMask) return Fail("two vanilla tabs have the same mask");
    }
    if (g_vanilla[0].windowDisp != g_vanilla[1].windowDisp || g_vanilla[0].windowDisp != g_vanilla[2].windowDisp ||
        g_vanilla[3].windowDisp == g_vanilla[0].windowDisp)
        return Fail("the land/air and naval windows are not where expected");

    uint8_t* f = SigFindIn(stateFn, 0x80, kCodePtabFactories);
    uint8_t* d = SigFindIn(stateFn, 0x200, kCodePtabVariants);
    uint8_t* s = SigFindIn(stateFn, 0x280, kCodePtabState);
    uint8_t* g = s ? SigFindIn(s, 0x80, kCodePtabGlow) : nullptr;
    if (!f || !d || !s || !g) return Fail("the button state function has an unexpected shape");
    g_canHave = (MaskTest_t)SigFollowRel32(f);
    g_hasFactories = (MaskTest_t)SigFollowRel32(f + kCodePtabFactoriesCall2);
    g_prod = Rd<uint32_t>(d, kCodePtabProdDisp);
    g_designs = Rd<uint32_t>(d, kCodePtabDesignsDisp);
    g_count = Rd<uint32_t>(d, kCodePtabCountDisp);
    g_designMask = Rd<uint32_t>(d, kCodePtabMaskDisp);
    g_new = Rd<uint32_t>(d, kCodePtabNewDisp);
    g_designType = Rd<uint32_t>(d, kCodePtabTypeDisp);
    g_buildable = Rd<uint32_t>(d, kCodePtabBuildableDisp);
    if (g_count != g_designs + 0xc) return Fail("the design array's count is not where expected");
    if (g_designType != EquipmentTypeOffset()) return Fail("the design -> type offset differs from production_data's");
    g_openFrame = (int)Rd<uint32_t>(s, kCodePtabOpenFrame);
    g_enableSlot = Rd<uint32_t>(s, kCodePtabEnableSlot);
    g_disableSlot = Rd<uint32_t>(s, kCodePtabDisableSlot);
    g_frameSlot = Rd<uint32_t>(s, kCodePtabFrameSlot);
    g_flags = g[kCodePtabGlowFlags];
    g_showSlot = g[kCodePtabShowSlot];
    g_hideSlot = Rd<uint32_t>(g, kCodePtabHideSlot);
    g_glowBit = g[kCodePtabGlowSet];
    if (g[kCodePtabGlowFlags2] != g_flags || (uint8_t)~g[kCodePtabGlowClear] != g_glowBit || !g_glowBit || (g_glowBit & (g_glowBit - 1)))
        return Fail("the glow's show / hide flags are inconsistent");
    for (uint32_t slot : {g_enableSlot, g_disableSlot, g_frameSlot, g_showSlot, g_hideSlot})
        if (!slot || slot % 8 || slot >= 0x800) return Fail("an element slot is implausible");

    uint8_t* lt = SigFindIn(land, kCodePtabMaskTestRange, kCodePtabMaskTestLand);
    uint8_t* nt = SigFindIn(naval, kCodePtabMaskTestRange, kCodePtabMaskTestNaval);
    if (!lt || !nt) return Fail("the lists' member test was not found");
    g_windowMask = Rd<uint32_t>(lt, kCodePtabWindowMaskDisp);
    if (Rd<uint32_t>(nt, kCodePtabWindowMaskDisp) != g_windowMask || Rd<uint32_t>(lt, kCodePtabDesignMaskDisp) != g_designMask ||
        Rd<uint32_t>(nt, kCodePtabDesignMaskDisp) != g_designMask)
        return Fail("the two lists test different fields");
    // Optional: type names (add_types / remove_types). Its default token is compared and returned.
    if (uint8_t* tn = SigFindUnique("ptab_type_by_name", kSigPtabTypeByName)) {
        if (Rd<uint32_t>(tn, kSigPtabTypeDefaultToken) == Rd<uint32_t>(tn, kSigPtabTypeDefaultToken2))
            g_typeByName = (TypeByName_t)tn;
        else
            Log("production_tabs: the type name lookup has an unexpected shape; add_types / remove_types unavailable");
    }
    g_setupFn = setup;
    g_statesFn = states;
    g_listFn[0] = land;
    g_listFn[1] = naval;
    return true;
}

// --- Faults --------------------------------------------------------------------------------------

DWORD g_faultCode = 0;
void* g_faultAt = nullptr;
int FaultFilter(EXCEPTION_POINTERS* ep) {
    g_faultCode = ep->ExceptionRecord->ExceptionCode;
    g_faultAt = ep->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}
void LogFaultOnce(const char* where) {
    InterlockedExchange(&g_off, 1);
    if (!InterlockedExchange(&g_faultLogged, 1)) {
        Log("ERROR: production_tabs: fault in the %s (exception 0x%08lX at %s); the game's own tabs from now on", where,
            g_faultCode, DescribeAddress(g_faultAt).c_str());
        if (g_fault) g_fault(where);
    }
}

// --- Designs and members -------------------------------------------------------------------------

bool DesignIsNaval(const void* design) { return (Rd<uint64_t>(design, g_designMask) & g_vanilla[3].listMask) != 0; }

bool Member(int tab, const void* design, uint64_t vanillaMask) {
    const void* type = Rd<const void*>(design, g_designType);
    if (!type) return false;
    uint64_t types = Rd<uint64_t>(design, g_designMask);
    bool vanilla = tab < 4 && (types & vanillaMask) != 0;
    return g_member(tab, PtabArchetypeOf(type), types, vanilla);
}

// The viewing country's designs (as the state function reads them), or false.
bool Designs(void* const** data, int* count) {
    void* country = CountryByTag(PlayerCountryTag());
    const uint8_t* prod = country ? Rd<const uint8_t*>(country, g_prod) : nullptr;
    if (!prod) return false;
    *data = Rd<void* const*>(prod, g_designs);
    *count = Rd<int>(prod, g_count);
    return *data && *count >= 0 && *count < 1000000;
}

int TabOfMask(uint64_t mask) {
    if (mask >> kOwnBits) {
        for (size_t t = 0; t < g_tabs.size(); ++t)
            if (mask >> (63 - t) & 1) return 4 + (int)t;
        return -1;
    }
    for (int i = 0; i < 4; ++i)
        if (g_changed[i] && mask == g_vanilla[i].listMask) return i;
    return -1;
}

// A new tab lists ships (the dockyard window) when its members are all ships.
bool NewTabNaval(int tab, uint64_t* typeMask) {
    void* const* data = nullptr;
    int n = 0;
    bool land = false, ships = false;
    *typeMask = 0;
    if (!Designs(&data, &n)) return false;
    for (int i = 0; i < n; ++i) {
        const void* design = data[i];
        if (!design || !Member(tab, design, 0)) continue;
        *typeMask |= Rd<uint64_t>(design, g_designMask);
        (DesignIsNaval(design) ? ships : land) = true;
    }
    *typeMask &= (1ull << kOwnBits) - 1;
    return ships && !land;
}

// --- The lists -----------------------------------------------------------------------------------

// The members of the window's tab among the game's designs, into g_scratch; -1: not a tab the loader
// changes (the game's own list).
int FilterBody(bool naval, void* window, const DesignArray* designs) {
    int tab = TabOfMask(Rd<uint64_t>(window, g_windowMask));
    if (tab < 0) return -1;
    uint64_t vanillaMask = tab < 4 ? g_vanilla[tab].listMask : 0;
    g_scratch.clear();
    for (int i = 0; i < designs->count; ++i) {
        void* design = designs->data[i];
        if (design && DesignIsNaval(design) == naval && Member(tab, design, vanillaMask)) g_scratch.push_back(design);
    }
    return tab;
}
int FilterSafe(bool naval, void* window, const DesignArray* designs) {
    __try {
        return FilterBody(naval, window, designs);
    } __except (FaultFilter(GetExceptionInformation())) {
        LogFaultOnce("production list filter");
        return -1;
    }
}
void List(int k, void* window, const void* designs, int* used) {
    if (g_off || !g_member || !window || !designs || FilterSafe(k == 1, window, (const DesignArray*)designs) < 0) {
        g_listOrig[k](window, designs, used);
        return;
    }
    // The members, with the window's mask open to every type for this call: the game still applies
    // its other checks (buildable, can_be_produced, outdated, modules).
    DesignArray members{g_scratch.data(), (int)g_scratch.size(), (int)g_scratch.size()};
    uint64_t mask = Rd<uint64_t>(window, g_windowMask);
    Wr<uint64_t>(window, g_windowMask, ~0ull);
    g_listOrig[k](window, &members, used);
    Wr<uint64_t>(window, g_windowMask, mask);
}
void __fastcall ListLandDetour(void* window, const void* designs, int* used) { List(0, window, designs, used); }
void __fastcall ListNavalDetour(void* window, const void* designs, int* used) { List(1, window, designs, used); }

// --- Button states -------------------------------------------------------------------------------

void CallSlot(void* element, uint32_t slot) { ((ElementFn_t)(*(void***)element)[slot / 8])(element); }

// As the game's state function: selected while open; else enabled when the country has a member
// (and the factories), disabled without; the glow while a member is new and buildable.
void ApplyState(void* button, void* glow, void* open, int tab, uint64_t vanillaMask, uint64_t factoryMask) {
    bool any = false, fresh = false;
    void* const* data = nullptr;
    int n = 0;
    if (g_canHave(factoryMask) && g_hasFactories(factoryMask) && Designs(&data, &n))
        for (int i = 0; i < n; ++i) {
            const void* design = data[i];
            if (!design || !Member(tab, design, vanillaMask)) continue;
            any = true;
            const void* type = Rd<const void*>(design, g_designType);
            if (Rd<uint8_t>(design, g_new) && type && Rd<uint8_t>(type, g_buildable)) fresh = true;
        }
    int frame = g_openFrame;
    if (button != open) {
        CallSlot(button, any ? g_enableSlot : g_disableSlot);
        frame = any ? kFrameEnabled : kFrameDisabled;
    }
    ((Frame_t)(*(void***)button)[g_frameSlot / 8])(button, frame);
    if (!glow) return;
    uint8_t flags = Rd<uint8_t>(glow, g_flags);
    if (fresh) {
        Wr<uint8_t>(glow, g_flags, (uint8_t)(flags & ~g_glowBit));
        CallSlot(glow, g_showSlot);
    } else {
        CallSlot(glow, g_hideSlot);
        Wr<uint8_t>(glow, g_flags, (uint8_t)(Rd<uint8_t>(glow, g_flags) | g_glowBit));
    }
}

bool TryBind(void* view, size_t t, bool atSetup);

void StatesBody(void* view, void* open) {
    for (int i = 0; i < 4; ++i) {
        if (!g_changed[i]) continue;
        void* button = Rd<void*>(view, g_vanilla[i].buttonDisp);
        if (button) ApplyState(button, Rd<void*>(view, g_vanilla[i].glowDisp), open, i, g_vanilla[i].stateMask, g_vanilla[i].stateMask);
    }
    if (view != g_view) return;
    for (size_t t = 0; t < g_tabs.size(); ++t) {
        const NewTab& nt = g_tabs[t];
        // A button the setup didn't find is looked up again (cheap; the tab works once it is there).
        if (!nt.buttonEl && !TryBind(view, t, false)) continue;
        uint64_t types = 0;
        bool naval = NewTabNaval(4 + (int)t, &types);
        // The factory check of the land/air tabs (military factories) or of the naval tab (dockyards).
        ApplyState(nt.buttonEl, nt.glowEl, open, 4 + (int)t, 0, g_vanilla[naval ? 3 : 1].stateMask);
    }
}
void StatesSafe(void* view, void* open) {
    __try {
        StatesBody(view, open);
    } __except (FaultFilter(GetExceptionInformation())) {
        LogFaultOnce("tab button states");
    }
}
void __fastcall StatesDetour(void* view, void* open) {
    g_statesOrig(view, open);
    if (!g_off && g_member && view) StatesSafe(view, open);
}

// --- New tab buttons -----------------------------------------------------------------------------

void* FindNamed(void* container, uint32_t slot, const char* name) {
    if (!container || !name || !*name) return nullptr;
    Find_t find = (Find_t)(*(void***)container)[slot / 8];
    static const char kNone[] = "hoi4_loader_no_such_element";
    NameView none{kNone, sizeof kNone - 1};
    void* missing = find(container, &none, 0);  // what a name the container lacks gives (a placeholder)
    NameView nv{name, strlen(name)};
    void* e = find(container, &nv, 0);
    return e && e != missing ? e : nullptr;
}

// Next to the vanilla buttons first, then further out.
void* FindInView(void* view, uint32_t slot, const char* name) {
    void* root = g_root(view);
    void* outer = FindNamed(root, g_containerSlot, g_outerName.c_str());
    void* top = FindNamed(outer, g_containerSlot, g_topName.c_str());
    for (void* c : {top, outer, root})
        if (void* e = FindNamed(c, slot, name)) return e;
    return nullptr;
}

void ClickBody(int t) {
    void* view = g_view;
    const NewTab& nt = g_tabs[(size_t)t];
    if (!view || !nt.buttonEl) return;
    uint64_t types = 0;
    bool naval = NewTabNaval(4 + t, &types);
    uint64_t mask = types | (1ull << (63 - t));  // the tab's own bit keeps its mask apart from every other tab's
    const Vanilla& v = g_vanilla[naval ? 3 : 0];
    g_open(view, mask, naval ? 1 : 0, Rd<void*>(view, v.windowDisp), nt.buttonEl);
}
void ClickSafe(int t) {
    if (g_off || !g_member) return;
    __try {
        ClickBody(t);
    } __except (FaultFilter(GetExceptionInformation())) {
        LogFaultOnce("tab button click");
    }
}

// Finds and binds new tab t's button (and glow) in the view; false if the view has no such button
// yet. At setup a missing button is logged once; later lookups (each buttons update) stay quiet
// until they find it.
bool TryBind(void* view, size_t t, bool atSetup) {
    NewTab& nt = g_tabs[t];
    void* button = FindInView(view, g_buttonSlot, nt.button.c_str());
    if (!button) {
        if (atSetup && !nt.missingLogged)
            Log("production_tabs: %s: no button %s in the production view (add a buttonType of that name to "
                "countryproductionlineview.gui); looked up again when the buttons update, the tab is off until then",
                nt.name.c_str(), nt.button.c_str());
        nt.missingLogged = true;
        return false;
    }
    void* glow = nt.glow.empty() ? nullptr : FindInView(view, g_iconSlot, nt.glow.c_str());
    if (!nt.glow.empty() && !glow)
        Log("production_tabs: %s: the production view has no icon %s; the tab has no glow", nt.name.c_str(), nt.glow.c_str());
    int index = (int)t;
    // The wrapper lives as long as the game: the button keeps a reference to it.
    if (!BindButtonClick(button, [index]() { ClickSafe(index); })) {
        Log("production_tabs: %s: the button could not be bound; the tab is off", nt.name.c_str());
        nt.buttonEl = nullptr;
        nt.button.clear();  // not looked up again
        return false;
    }
    nt.buttonEl = button;
    nt.glowEl = glow;
    Log("production_tabs: %s: button %s bound%s%s", nt.name.c_str(), nt.button.c_str(), glow ? " (with glow)" : "",
        atSetup ? "" : " (found after the view's setup)");
    return true;
}

void SetupBody(void* view) {
    g_view = view;
    for (size_t t = 0; t < g_tabs.size(); ++t) {
        g_tabs[t].buttonEl = nullptr;
        g_tabs[t].glowEl = nullptr;
        g_tabs[t].missingLogged = false;
        if (!g_tabs[t].button.empty()) TryBind(view, t, true);
    }
}
void SetupSafe(void* view) {
    __try {
        SetupBody(view);
    } __except (FaultFilter(GetExceptionInformation())) {
        LogFaultOnce("production view setup");
    }
}
void __fastcall SetupDetour(void* view, void* b, void* c, void* d) {
    g_setupOrig(view, b, c, d);
    if (!g_off && g_member && view && !g_tabs.empty()) SetupSafe(view);
}

}  // namespace

void ResolveProductionTabs() {
    g_ok = ResolveAll();
    if (g_ok)
        Log("Production tabs: masks infantry 0x%llx/0x%llx, armor 0x%llx, air 0x%llx, naval 0x%llx; window mask +0x%x, design mask "
            "+0x%x, designs +0x%x/+0x%x; lookups %s > %s (slot 0x%x), button slot 0x%x, icon slot 0x%x",
            (unsigned long long)g_vanilla[0].listMask, (unsigned long long)g_vanilla[0].stateMask, (unsigned long long)g_vanilla[1].listMask,
            (unsigned long long)g_vanilla[2].listMask, (unsigned long long)g_vanilla[3].listMask, g_windowMask, g_designMask, g_prod,
            g_designs, g_outerName.c_str(), g_topName.c_str(), g_containerSlot, g_buttonSlot, g_iconSlot);
}

bool HaveProductionTabs() { return g_ok && HaveCountries() && HaveButtonClick(); }

namespace {
uint64_t TypeByName(const char* name) {
    EngineString s;
    ConstructEngineString(&s, name, strlen(name));
    uint64_t m = g_typeByName(&s);
    FreeEngineString(&s);
    return m;
}
uint64_t TypeByNameSafe(const char* name, bool* ok) {
    __try {
        *ok = true;
        return TypeByName(name);
    } __except (FaultFilter(GetExceptionInformation())) {
        *ok = false;
        return 0;
    }
}
}  // namespace

bool HavePtabTypes() {
    if (!g_typeByName) return false;
    if (g_typesChecked == 0) {  // the game's own masks: "armor" is exactly the tank tab's; a made-up name has none
        bool ok1 = false, ok2 = false;
        uint64_t armor = TypeByNameSafe("armor", &ok1);
        uint64_t none = TypeByNameSafe("hoi4_loader_no_such_type", &ok2);
        g_typesChecked = ok1 && ok2 && armor == g_vanilla[1].listMask && none == 0 ? 1 : -1;
        if (g_typesChecked < 0)
            Log("production_tabs: the type name lookup failed its check (armor 0x%llx, the tank tab 0x%llx); add_types / "
                "remove_types unavailable",
                (unsigned long long)armor, (unsigned long long)g_vanilla[1].listMask);
    }
    return g_typesChecked > 0;
}

uint64_t PtabTypeMask(const char* name) {
    if (!name || !*name || !HavePtabTypes()) return 0;
    bool ok = false;
    uint64_t m = TypeByNameSafe(name, &ok);
    return ok && !(m >> kOwnBits) ? m : 0;
}

const void* PtabArchetypeOf(const void* type) {
    const void* a = type && TypeArchetypeOffset() ? Rd<const void*>(type, TypeArchetypeOffset()) : nullptr;
    return a ? a : type;
}

bool HookProductionTabs(const bool changed[4], const PtabNewTab* tabs, int count, PtabMember_t member, PtabFault_t fault) {
    if (!HaveProductionTabs() || !member || count < 0 || count > 63 - kOwnBits + 1) return false;
    for (int i = 0; i < 4; ++i) g_changed[i] = changed[i];
    g_tabs.clear();
    for (int t = 0; t < count; ++t) {
        NewTab nt;
        nt.name = tabs[t].name ? tabs[t].name : "";
        nt.button = tabs[t].button ? tabs[t].button : "";
        nt.glow = tabs[t].glow ? tabs[t].glow : "";
        g_tabs.push_back(nt);
    }
    g_member = member;
    g_fault = fault;
    struct Hook {
        uint8_t* target;
        void* detour;
        void** orig;
        const char* name;
    } hooks[] = {
        {g_setupFn, (void*)&SetupDetour, (void**)&g_setupOrig, "ptab_setup"},
        {g_statesFn, (void*)&StatesDetour, (void**)&g_statesOrig, "ptab_states"},
        {g_listFn[0], (void*)&ListLandDetour, (void**)&g_listOrig[0], "ptab_list_land"},
        {g_listFn[1], (void*)&ListNavalDetour, (void**)&g_listOrig[1], "ptab_list_naval"},
    };
    for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; ++i)
        if (!HookQueue(hooks[i].target, hooks[i].detour, hooks[i].orig, hooks[i].name)) {
            for (size_t j = 0; j < i; ++j) HookUnqueue(hooks[j].target);
            g_member = nullptr;
            return false;
        }
    return true;
}

}  // namespace Engine
