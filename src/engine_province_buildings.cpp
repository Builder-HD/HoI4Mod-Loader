// Engine access for province building removal (NOTES.md Part I): a cross on each province
// building entry of the state view (supply hub, naval base, bunkers, ...), as vanilla shows on
// the shared slot buildings (dockyards, factories), removing one level as remove_building does.
//
// Vanilla's cross only exists in state_shared_slot_building_entry, and its command can only name a
// state building. So:
//   - a file patch adds a `remove` button to province_building_entry in countrystateview.gui
//     (vanilla's or a mod's copy, whichever the game loads);
//   - the item's update hook binds that button once per item (a CButtonWrapper, as the shared slot
//     item binds its own cross) and shows it where the player may remove a level, enabled unless
//     the wartime removal cooldown blocks it;
//   - the tooltip hook gives it vanilla's "Remove the building." text (or the cooldown text);
//   - the destructor hook frees the binding before the item goes;
//   - a click goes to the plugin, which removes the level at the next safe point (single player).
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine.h"
#include "lifecycle.h"
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

struct StringView {  // what the window's finders and the lookup read: data, length
    const char* data;
    uint64_t size;
};

using ItemFn_t = void(__fastcall*)(void* item);
using ItemDtor_t = void*(__fastcall*)(void* item, uint32_t flags);
using Tooltip_t = uint64_t(__fastcall*)(void* sub, void* element, EngineString* out);
using ButtonLookup_t = void*(__fastcall*)(void* map, const StringView* name);
using WrapperCtor_t = void*(__fastcall*)(void* out, void* button, std::function<void()>* click, std::function<void()>* other);
using WrapperDtor_t = void(__fastcall*)(void* wrapper);
using BuildingLookup_t = void*(__fastcall*)(void* container, void* type);
using Permission_t = bool(__fastcall*)(void* state, void* building, EngineString* why);
using SetLevel_t = bool(__fastcall*)(void* building, int level, bool notify);
using ElementFn_t = void(__fastcall*)(void* element);
using ElementBool_t = bool(__fastcall*)(void* element);

// The engine's std::function<void()> (MSVC STL) is passed to the wrapper as is: 64 bytes, the
// implementation pointer in the last qword. The wrapper copies it (_Copy) and clears ours.
static_assert(sizeof(std::function<void()>) == 64, "std::function must match the game's MSVC STL layout");

uint8_t* g_updateFn = nullptr;
uint8_t* g_tooltipFn = nullptr;
uint8_t* g_dtorFn = nullptr;
ItemFn_t g_updateOrig = nullptr;
Tooltip_t g_tooltipOrig = nullptr;
ItemDtor_t g_dtorOrig = nullptr;

uint32_t g_itemWindow = 0, g_itemType = 0, g_finderSlotDisp = 0;
uint32_t g_tipSub = 0, g_tipType = 0, g_tipView = 0;
uint32_t g_typeProvinceFlag = 0, g_viewProvince = 0, g_viewState = 0, g_provinceBuildings = 0;
uint32_t g_buttonMap = 0;
ButtonLookup_t g_buttonLookup = nullptr;
uint8_t* g_buttonFinder = nullptr;
WrapperCtor_t g_wrapperCtor = nullptr;
WrapperDtor_t g_wrapperDtor = nullptr;
size_t g_wrapperSize = 0;
BuildingLookup_t g_lookupBuilding = nullptr;
Permission_t g_permission = nullptr;
SetLevel_t g_setLevel = nullptr;
uint32_t g_level = 0;
uint32_t g_dateDisp = 0, g_stateRemovalDate = 0;  // gamestate -> today; state -> last removal date
// The UI refresh vanilla's removal command ends with: ui->vtable[slot]() gets the event.
using UiTarget_t = void*(__fastcall*)(void* ui);
using UiPost_t = void(__fastcall*)(void* target, const int* event);
void** g_ppUi = nullptr;
uint32_t g_uiSlot = 0;
int g_uiEvent = 0;
UiPost_t g_uiPost = nullptr;
uint32_t g_hideDisp = 0, g_showDisp = 0, g_flags = 0;
uint8_t g_hiddenBit = 0;
uint32_t g_isEnabledDisp = 0, g_enableDisp = 0, g_disableDisp = 0;
void** g_vtItem = nullptr;
void** g_vtWindow = nullptr;

ProvinceRemoveAllowed_t g_allowedFn = nullptr;
ProvinceRemoveClick_t g_clickFn = nullptr;

// Vanilla's "Remove Building" dialog (CConfirmRemoveBuildingLevel), opened by the cross as vanilla's
// shared slot cross opens it. Its own OK posts a command for a *state* building, so for the dialogs
// opened here the OK, the validity check and the destructor are hooked: OK runs the province
// removal, validity checks the province building. Optional: without it the cross removes at once.
using EngineAlloc_t = void*(__fastcall*)(size_t size);
using ConfirmCtor_t = void*(__fastcall*)(void* dialog, void* ui, int stateId, int typeIndex, int mode);
using ConfirmOpen_t = void(__fastcall*)(void* manager, void* dialog);
using ConfirmFn_t = uint64_t(__fastcall*)(void* dialog);
using ConfirmDtor_t = void*(__fastcall*)(void* dialog, uint32_t flags);
EngineAlloc_t g_engineAlloc = nullptr;
ConfirmCtor_t g_confirmCtor = nullptr;
ConfirmOpen_t g_confirmOpen = nullptr;
size_t g_confirmSize = 0;
void** g_ppConfirmUi = nullptr;
uint32_t g_viewManager1 = 0, g_viewManager2 = 0, g_stateIdDisp = 0, g_typeIndexDisp = 0;
uint8_t* g_confirmValidFn = nullptr;
uint8_t* g_confirmAcceptFn = nullptr;
uint8_t* g_confirmDtorFn = nullptr;
ConfirmFn_t g_confirmValidOrig = nullptr, g_confirmAcceptOrig = nullptr;
ConfirmDtor_t g_confirmDtorOrig = nullptr;
bool g_confirmHooked = false;

bool HaveConfirm() {
    return g_engineAlloc && g_confirmCtor && g_confirmOpen && g_confirmSize && g_ppConfirmUi && g_viewManager1 &&
           g_viewManager2 && g_stateIdDisp && g_typeIndexDisp && g_confirmValidFn && g_confirmAcceptFn && g_confirmDtorFn;
}

// One per CProvinceBuildingItem the update has seen. button null: the entry has no `remove`
// button (the patch was not applied to the file the game loaded) or the window is not a
// CContainerWindow; such items are left alone.
struct Binding {
    void* button = nullptr;
    void* wrapper = nullptr;  // CButtonWrapper storage (aligned), owned here
};
// UI objects: created, updated and destroyed by the UI, which runs on the main thread. The lock
// only guards against a surprise.
std::unordered_map<void*, Binding> g_bindings;
SRWLOCK g_lock = SRWLOCK_INIT;
bool g_loggedBind = false, g_loggedMissing = false;

void* Window(void* item) { return Rd<void*>(item, g_itemWindow); }
void* Type(void* item) { return Rd<void*>(item, g_itemType); }
void* View(void* item) { return Rd<void*>(item, g_tipSub + g_tipView); }

bool IsProvinceType(void* type) { return type && Rd<int>(type, g_typeProvinceFlag) > 0; }

// What the entry stands for: the province, its building of the entry's type (or null), the state.
struct Target {
    void* province = nullptr;
    void* type = nullptr;
    void* state = nullptr;
    void* building = nullptr;
    int level = 0;
};

Target TargetOf(void* item) {
    Target t;
    void* view = View(item);
    t.type = Type(item);
    if (!view || !IsProvinceType(t.type)) return t;
    t.province = Rd<void*>(view, g_viewProvince);
    t.state = Rd<void*>(view, g_viewState);
    if (!t.province) return t;
    t.building = g_lookupBuilding((uint8_t*)t.province + g_provinceBuildings, t.type);
    t.level = t.building ? Rd<int16_t>(t.building, g_level) : 0;
    return t;
}

bool PlayerControls(void* province) {
    uint32_t off = ProvinceControllerOffset();
    int player = PlayerCountryTag();
    int controller = off && province ? Rd<int>(province, off) : 0;
    return player > 0 && controller > 0 && IsSameCountry(controller, player);
}

// The cross shows where a level could be removed by the local player; it is enabled unless the
// wartime removal cooldown blocks it (the vanilla check, which also explains why in the tooltip).
bool MayShow(const Target& t) {
    return t.building && t.level > 0 && t.state && PlayerControls(t.province) && g_allowedFn && g_allowedFn();
}

void SetVisible(void* element, bool visible) {
    uint8_t flags = Rd<uint8_t>(element, g_flags);
    void** vt = *(void***)element;
    if (visible) {
        flags &= (uint8_t)~g_hiddenBit;
        memcpy((uint8_t*)element + g_flags, &flags, 1);
        ((ElementFn_t)vt[g_showDisp / 8])(element);
    } else {
        ((ElementFn_t)vt[g_hideDisp / 8])(element);
        flags = (uint8_t)(Rd<uint8_t>(element, g_flags) | g_hiddenBit);
        memcpy((uint8_t*)element + g_flags, &flags, 1);
    }
}

void SetEnabled(void* element, bool enabled) {
    void** vt = *(void***)element;
    bool now = ((ElementBool_t)vt[g_isEnabledDisp / 8])(element);
    if (now == enabled) return;
    ((ElementFn_t)vt[(enabled ? g_enableDisp : g_disableDisp) / 8])(element);
}

// --- Fail-safe ---------------------------------------------------------------------------------
// The project's rule: on any error, log it and leave the feature off. A fault inside the loader's
// part of a hook (contained by SEH) switches it off for the rest of the session: the crosses hide,
// clicks do nothing and the tooltip is vanilla's. The plugin is told, to clear the feature's flag.
volatile bool g_enabled = true;
volatile LONG g_faultLogged = 0;
ProvinceRemoveFault_t g_faultFn = nullptr;

void Fault(const char* where) {
    g_enabled = false;
    if (InterlockedExchange(&g_faultLogged, 1)) return;
    Log("ERROR: province_building_removal: fault in the %s; switched off for this session", where);
    if (g_faultFn) g_faultFn(where);
}

// UI hooks touch game state (levels, controllers) and GUI elements: main thread only. The first
// call logs its thread as evidence; a call from another thread leaves the entry as vanilla made it.
// Without the frame hook there is no main-thread test (the plugin needs the queue, so it is there).
volatile LONG g_threadLogged = 0;
bool OnMainThread(const char* what) {
    bool main = !MainThreadQueueAvailable() || IsMainThread();
    if (!InterlockedExchange(&g_threadLogged, 1))
        Log("province_building_removal: first %s on thread %lu (%s)", what, GetCurrentThreadId(),
            main ? "the main thread" : "NOT the main thread: entries are left as vanilla made them");
    return main;
}

bool IsItem(void* item) { return item && g_vtItem && *(void***)item == g_vtItem; }

// --- The confirmation dialog -------------------------------------------------------------------
// The dialogs opened by a province cross, and what they would remove. Erased by the dialog's
// destructor hook. UI objects on the main thread; the lock only guards against a surprise.
struct Pending {
    void* province;
    void* type;
    void* state;
};
std::unordered_map<void*, Pending> g_dialogs;

bool FindDialog(void* dialog, Pending* p) {
    AcquireSRWLockShared(&g_lock);
    auto it = g_dialogs.find(dialog);
    bool known = it != g_dialogs.end();
    if (known) *p = it->second;
    ReleaseSRWLockShared(&g_lock);
    return known;
}

// The removal could still happen now: the province still has a level, the player still controls
// it, and the cooldown allows it (as vanilla's dialog re-checks the state building).
bool PendingStillValid(const Pending& p) {
    if (!g_enabled || !p.province || !p.state || !IsProvinceType(p.type)) return false;
    void* building = g_lookupBuilding((uint8_t*)p.province + g_provinceBuildings, p.type);
    int level = building ? Rd<int16_t>(building, g_level) : 0;
    return level > 0 && PlayerControls(p.province) && g_permission(p.state, building, nullptr);
}

// Opens vanilla's "Remove Building" dialog for this province building (mode 0: remove a level).
// False if it could not be opened; the caller then removes without asking.
bool OpenConfirm(void* item, const Target& t) {
    if (!g_confirmHooked) return false;
    void* view = View(item);
    void* holder = view ? Rd<void*>(view, g_viewManager1) : nullptr;
    void* manager = holder ? Rd<void*>(holder, g_viewManager2) : nullptr;
    void* ui = *g_ppConfirmUi;
    if (!manager || !ui) return false;
    void* dialog = g_engineAlloc(g_confirmSize);  // the engine's allocator: the dialog frees itself
    if (!dialog) return false;
    g_confirmCtor(dialog, ui, Rd<int>(t.state, g_stateIdDisp), Rd<int>(t.type, g_typeIndexDisp), 0);
    AcquireSRWLockExclusive(&g_lock);
    g_dialogs[dialog] = Pending{t.province, t.type, t.state};
    ReleaseSRWLockExclusive(&g_lock);
    g_confirmOpen(manager, dialog);
    return true;
}

// --- Hook bodies (C++ objects allowed; the detours below wrap them in SEH) --------------------
void OnCrossClicked(void* item) {
    if (!g_enabled || !OnMainThread("click")) return;
    AcquireSRWLockShared(&g_lock);
    bool known = g_bindings.count(item) != 0;
    ReleaseSRWLockShared(&g_lock);
    if (!known || !IsItem(item)) return;
    Target t = TargetOf(item);
    if (!MayShow(t) || !g_permission(t.state, t.building, nullptr)) return;
    if (OpenConfirm(item, t)) return;  // OK in the dialog removes (AcceptBody)
    if (g_clickFn) g_clickFn(t.province, t.type, t.state);
}

// -1: not one of ours (vanilla's own); else 1 still valid, 0 not (the dialog closes).
int ValidBody(void* dialog) {
    Pending p;
    if (!FindDialog(dialog, &p)) return -1;
    return PendingStillValid(p) ? 1 : 0;
}

// -1: not one of ours; else the removal is handed to the plugin (deferred) and the dialog closes.
int AcceptBody(void* dialog) {
    Pending p;
    if (!FindDialog(dialog, &p)) return -1;
    if (PendingStillValid(p) && g_clickFn) g_clickFn(p.province, p.type, p.state);
    return 1;
}

void ForgetDialog(void* dialog) {
    AcquireSRWLockExclusive(&g_lock);
    g_dialogs.erase(dialog);
    ReleaseSRWLockExclusive(&g_lock);
}

// Builds the CButtonWrapper for the item's `remove` button (once per item).
Binding Bind(void* item) {
    Binding b;
    void* window = Window(item);
    if (!window || *(void***)window != g_vtWindow) return b;
    StringView name{"remove", 6};
    void* button = g_buttonLookup((uint8_t*)window + g_buttonMap, &name);
    if (!button) {
        if (!g_loggedMissing)
            Log("province_building_removal: province_building_entry has no `remove` button (the countrystateview.gui "
                "patch was not applied to the file the game loaded); no cross");
        g_loggedMissing = true;
        return b;
    }
    void* storage = _aligned_malloc(g_wrapperSize + 0x40, 16);
    if (!storage) return b;
    memset(storage, 0, g_wrapperSize + 0x40);
    std::function<void()> click = [item]() {
        __try {
            OnCrossClicked(item);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Fault("click handler");
        }
    };
    std::function<void()> other;
    g_wrapperCtor(storage, button, &click, &other);
    b.button = button;
    b.wrapper = storage;
    if (!g_loggedBind) Log("province_building_removal: first cross bound (button %p)", button);
    g_loggedBind = true;
    return b;
}

Binding FindOrBind(void* item) {
    AcquireSRWLockShared(&g_lock);
    auto it = g_bindings.find(item);
    bool known = it != g_bindings.end();
    Binding b = known ? it->second : Binding{};
    ReleaseSRWLockShared(&g_lock);
    if (known) return b;
    b = Bind(item);  // outside the lock: the wrapper constructor calls into the GUI
    AcquireSRWLockExclusive(&g_lock);
    g_bindings[item] = b;
    ReleaseSRWLockExclusive(&g_lock);
    return b;
}

Binding Find(void* item) {
    AcquireSRWLockShared(&g_lock);
    auto it = g_bindings.find(item);
    Binding b = it != g_bindings.end() ? it->second : Binding{};
    ReleaseSRWLockShared(&g_lock);
    return b;
}

void UpdateBody(void* item) {
    if (!IsItem(item) || !OnMainThread("entry update")) return;
    if (!g_enabled) {  // switched off: hide a cross that is already there, bind nothing new
        Binding b = Find(item);
        if (b.button) SetVisible(b.button, false);
        return;
    }
    Binding b = FindOrBind(item);
    if (!b.button) return;
    Target t = TargetOf(item);
    bool show = MayShow(t);
    SetVisible(b.button, show);
    if (show) SetEnabled(b.button, g_permission(t.state, t.building, nullptr));
}

// 1: the cross's tooltip was written; 0: not the cross (or switched off), vanilla's tooltip.
int TooltipBody(void* sub, void* element, EngineString* out) {
    void* item = (uint8_t*)sub - g_tipSub;
    if (!g_enabled || !out || !IsItem(item)) return 0;
    Binding b = Find(item);
    if (!b.button || element != b.button) return 0;
    Target t = TargetOf(item);
    if (t.building && t.state && !g_permission(t.state, t.building, out)) return 1;  // the cooldown text
    if (const EngineString* text = LocalizeKey(kProvinceBuildingRemoveTooltipKey)) {
        FreeEngineString(out);
        ConstructEngineString(out, text->c_str(), text->size);
    }
    return 1;
}

void* TakeWrapper(void* item) {
    AcquireSRWLockExclusive(&g_lock);
    auto it = g_bindings.find(item);
    void* wrapper = nullptr;
    if (it != g_bindings.end()) {
        wrapper = it->second.wrapper;
        g_bindings.erase(it);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return wrapper;
}

// --- Detours (no C++ objects, so SEH applies) -------------------------------------------------
void __fastcall UpdateDetour(void* item) {
    g_updateOrig(item);
    __try {
        UpdateBody(item);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault("entry update");
    }
}

uint64_t __fastcall TooltipDetour(void* sub, void* element, EngineString* out) {
    int mine = 0;
    __try {
        mine = TooltipBody(sub, element, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault("tooltip");
        mine = 0;
    }
    return mine ? 1 : g_tooltipOrig(sub, element, out);
}

// The dialog's validity check and OK: the province removal for dialogs opened by a cross, vanilla's
// for every other. A fault answers "not valid" (the dialog closes without removing anything).
uint64_t __fastcall ConfirmValidDetour(void* dialog) {
    int mine = -1;
    __try {
        mine = ValidBody(dialog);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault("confirmation dialog check");
        mine = 0;
    }
    return mine < 0 ? g_confirmValidOrig(dialog) : (uint64_t)mine;
}

uint64_t __fastcall ConfirmAcceptDetour(void* dialog) {
    int mine = -1;
    __try {
        mine = AcceptBody(dialog);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault("confirmation dialog OK");
        mine = 1;
    }
    return mine < 0 ? g_confirmAcceptOrig(dialog) : (uint64_t)mine;
}

void* __fastcall ConfirmDtorDetour(void* dialog, uint32_t flags) {
    __try {
        ForgetDialog(dialog);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault("confirmation dialog destructor");
    }
    return g_confirmDtorOrig(dialog, flags);
}

// Always runs, switched off or not: the wrapper must leave the button before the item's window goes.
void* __fastcall DtorDetour(void* item, uint32_t flags) {
    __try {
        if (void* wrapper = TakeWrapper(item)) {
            g_wrapperDtor(wrapper);
            _aligned_free(wrapper);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault("entry destructor");
    }
    return g_dtorOrig(item, flags);
}

}  // namespace

void ResolveProvinceBuildings() {
    uint8_t* p;
    uint32_t updateType = 0, updateWindow = 0;
    if ((p = SigFindUnique("pb_update", kSigPbUpdate)) != nullptr) {
        g_updateFn = p;
        updateWindow = Rd<uint8_t>(p, kSigPbUpdateWindowDisp);
        g_finderSlotDisp = Rd<uint8_t>(p, kSigPbUpdateFinderSlotDisp);
        updateType = Rd<uint8_t>(p, kSigPbUpdateTypeDisp);
        for (size_t i = 0; i < kCodePbUpdateEnableRange; ++i) {
            if (!SigMatchAt(p + i, kCodePbUpdateEnable)) continue;
            g_isEnabledDisp = Rd<uint32_t>(p + i, kCodePbIsEnabledDisp);
            g_enableDisp = Rd<uint32_t>(p + i, kCodePbEnableDisp);
            g_disableDisp = Rd<uint32_t>(p + i, kCodePbDisableDisp);
            break;
        }
    }
    if ((p = SigFindUnique("pb_tooltip", kSigPbTooltip)) != nullptr) {
        g_tooltipFn = p;
        g_tipType = Rd<uint8_t>(p, kSigPbTipTypeDisp);
        g_tipView = Rd<uint8_t>(p, kSigPbTipViewDisp);
        g_typeProvinceFlag = Rd<uint32_t>(p, kSigPbTipProvinceFlagDisp);
        g_viewProvince = Rd<uint32_t>(p, kSigPbTipProvinceDisp);
        g_provinceBuildings = Rd<uint32_t>(p, kSigPbTipBuildingsDisp);
        g_viewState = Rd<uint32_t>(p, kSigPbTipStateDisp);
        int8_t window = Rd<int8_t>(p, kSigPbTipWindowDisp);
        // The item's vtable and destructor; the destructor names the tooltip sub-object's vtable,
        // whose slot 0 must be the tooltip found above.
        g_vtItem = FindVtableByRtti(".?AVCProvinceBuildingItem@@", nullptr);
        uint8_t* dtor = g_vtItem ? (uint8_t*)g_vtItem[0] : nullptr;
        if (dtor && SigMatchAt(dtor, kCodePbDtor) &&
            (void**)SigRipTarget(dtor + kCodePbDtorVtableLea, 3, 7) == g_vtItem &&
            ((void**)SigRipTarget(dtor + kCodePbDtorTipVtableLea, 3, 7))[0] == (void*)p) {
            g_dtorFn = dtor;
            g_tipSub = Rd<uint8_t>(dtor, kCodePbDtorTipSubDisp);
            if (g_tipSub + g_tipType != updateType || (int)g_tipSub + window != (int)updateWindow) {
                Log("ERROR: province_building_removal: the update and the tooltip read different item fields");
                g_dtorFn = nullptr;
            }
            g_itemType = updateType;
            g_itemWindow = updateWindow;
        } else {
            Log("ERROR: province_building_removal: CProvinceBuildingItem destructor or tooltip vtable not as analysed");
        }
    }
    if ((p = SigFindUnique("pb_button_find", kSigPbButtonFind)) != nullptr) {
        g_buttonFinder = p;
        g_buttonMap = Rd<uint32_t>(p, kSigPbButtonMapDisp);
        g_buttonLookup = (ButtonLookup_t)SigFollowRel32(p + kSigPbButtonLookupCall);
        // The update's finder slot of CContainerWindow must lead here (a forwarding stub just before it).
        g_vtWindow = FindVtableByRtti(".?AVCContainerWindow@@", nullptr);
        uint8_t* slot = g_vtWindow && g_finderSlotDisp ? (uint8_t*)g_vtWindow[g_finderSlotDisp / 8] : nullptr;
        if (!slot || slot >= p || p - slot > 0x40) {
            Log("ERROR: province_building_removal: the window's button finder is not where expected");
            g_buttonLookup = nullptr;
        }
    }
    if ((p = SigFindUnique("button_wrapper_ctor", kSigButtonWrapperCtor)) != nullptr) {
        void** vt = FindVtableByRtti(".?AVCButtonWrapper@@", nullptr);
        if (vt && (void**)SigRipTarget(p + kSigButtonWrapperVtableLea, 3, 7) == vt)
            g_wrapperCtor = (WrapperCtor_t)p;
        else
            Log("ERROR: province_building_removal: the button wrapper constructor stores another vtable");
    }
    if ((p = SigFindUnique("shared_slot_dtor", kSigSharedSlotDtor)) != nullptr) {
        uint8_t* d1 = SigFollowRel32(p + kSigSharedSlotDtorCall1);
        uint8_t* d2 = SigFollowRel32(p + kSigSharedSlotDtorCall2);
        uint32_t second = Rd<uint32_t>(p, kSigSharedSlotSecondDisp), first = Rd<uint8_t>(p, kSigSharedSlotFirstDisp);
        if (d1 == d2 && second > first && second - first >= 0x100 && second - first <= 0x2000) {
            g_wrapperDtor = (WrapperDtor_t)d1;
            g_wrapperSize = second - first;
        } else {
            Log("ERROR: province_building_removal: the shared slot destructor does not look as analysed");
        }
    }
    g_lookupBuilding = (BuildingLookup_t)SigFindUnique("building_lookup", kSigBuildingLookup);
    if ((p = SigFindUnique("remove_permission", kSigRemovePermission)) != nullptr) {
        uint8_t* lea = AnchorFindLea("remove_cooldown", kAnchorRemoveCooldown);
        if (lea && lea > p && lea < p + 0x400)
            g_permission = (Permission_t)p;
        else
            Log("ERROR: province_building_removal: the removal permission check does not use the cooldown text");
    }
    if ((p = SigFindUnique("remove_building_loop", kSigRemoveBuildingLoop)) != nullptr) {
        g_level = Rd<uint8_t>(p, kSigRemoveLevelDisp);
        g_setLevel = (SetLevel_t)SigFollowRel32(p + kSigRemoveSetLevelCall);
    }
    if ((p = SigFindUnique("remove_cooldown_stamp", kSigRemoveCooldownStamp)) != nullptr) {
        // The same gamestate global the rest of the engine access uses.
        void** gs = (void**)SigRipTarget(p, kSigStampGameStateRel, kSigStampGameStateRel + 4);
        if (GameStateSlot() && gs == GameStateSlot()) {
            g_dateDisp = Rd<uint32_t>(p, kSigStampDateDisp);
            g_stateRemovalDate = Rd<uint32_t>(p, kSigStampStateDisp);
        } else {
            Log("ERROR: province_building_removal: the removal command reads another gamestate global");
        }
    }
    if ((p = SigFindUnique("remove_ui_refresh", kSigRemoveUiRefresh)) != nullptr) {
        g_ppUi = (void**)SigRipTarget(p, kSigRemoveUiRel, kSigRemoveUiRel + 4);
        g_uiSlot = Rd<uint32_t>(p, kSigRemoveUiSlotDisp) / 8;
        g_uiEvent = Rd<int>(p, kSigRemoveUiEventDisp);
        g_uiPost = (UiPost_t)SigFollowRel32(p + kSigRemoveUiPostCall);
    }
    // The confirmation dialog: opened as vanilla's shared slot cross opens it; its vtable from RTTI,
    // checked against the constructor; the three hooked slots checked by shape, and the state id
    // field the constructor writes must be the one the validity check and OK read.
    if ((p = SigFindUnique("confirm_remove_open", kSigConfirmRemoveOpen)) != nullptr) {
        uint8_t* ctor = SigFollowRel32(p + kSigConfirmCtorCall);
        size_t slots = 0;
        void** vt = FindVtableByRtti(".?AVCConfirmRemoveBuildingLevel@@", &slots);
        uint32_t stateField = SigMatchAt(ctor, kCodeConfirmCtor) ? Rd<uint32_t>(ctor, kCodeConfirmStateDisp) : 0;
        auto readsState = [&](uint8_t* fn) {
            for (size_t i = 0; i < kCodeConfirmReadRange; ++i)
                if (SigMatchAt(fn + i, kCodeConfirmReadState) && Rd<uint32_t>(fn + i, 3) == stateField) return true;
            return false;
        };
        bool ok = vt && slots > (size_t)kConfirmAcceptSlot && stateField &&
                  (void**)SigRipTarget(ctor + kCodeConfirmCtorVtableLea, 3, 7) == vt &&
                  SigMatchAt((uint8_t*)vt[kConfirmDtorSlot], kCodeConfirmDtor) &&
                  (void**)SigRipTarget((uint8_t*)vt[kConfirmDtorSlot] + kCodeConfirmDtorVtableLea, 3, 7) == vt &&
                  SigMatchAt((uint8_t*)vt[kConfirmValidSlot], kCodeConfirmValid) &&
                  SigMatchAt((uint8_t*)vt[kConfirmAcceptSlot], kCodeConfirmAccept) &&
                  readsState((uint8_t*)vt[kConfirmValidSlot]) && readsState((uint8_t*)vt[kConfirmAcceptSlot]);
        if (ok) {
            g_engineAlloc = (EngineAlloc_t)SigFollowRel32(p + kSigConfirmAllocCall);
            g_confirmCtor = (ConfirmCtor_t)ctor;
            g_confirmOpen = (ConfirmOpen_t)SigFollowRel32(p + kSigConfirmOpenCall);
            g_confirmSize = Rd<uint32_t>(p, kSigConfirmSizeImm);
            g_ppConfirmUi = (void**)SigRipTarget(p + kSigConfirmUiRel - 3, 3, 7);
            g_viewManager1 = Rd<uint32_t>(p, kSigConfirmMgrDisp1);
            g_viewManager2 = Rd<uint32_t>(p, kSigConfirmMgrDisp2);
            g_stateIdDisp = Rd<uint8_t>(p, kSigConfirmStateIdDisp);
            g_typeIndexDisp = Rd<uint32_t>(p, kSigConfirmTypeIndexDisp);
            g_confirmDtorFn = (uint8_t*)vt[kConfirmDtorSlot];
            g_confirmValidFn = (uint8_t*)vt[kConfirmValidSlot];
            g_confirmAcceptFn = (uint8_t*)vt[kConfirmAcceptSlot];
            Log("Province buildings: confirmation dialog 0x%zx bytes, manager view+0x%x->+0x%x, state id +0x%x, type "
                "index +0x%x",
                g_confirmSize, g_viewManager1, g_viewManager2, g_stateIdDisp, g_typeIndexDisp);
        } else {
            Log("province_building_removal: the confirmation dialog does not look as analysed; the cross removes "
                "without asking");
        }
    }
    if ((p = SigFindUnique("gui_hide_show", kSigGuiHideShow)) != nullptr) {
        g_hideDisp = Rd<uint32_t>(p, kSigGuiHideDisp);
        g_flags = Rd<uint8_t>(p, kSigGuiFlagsDisp);
        g_hiddenBit = Rd<uint8_t>(p, kSigGuiHiddenBit);
        g_showDisp = Rd<uint8_t>(p, kSigGuiShowDisp);
    }
    if (HaveProvinceBuildingRemoval())
        Log("Province buildings: item window +0x%x, type +0x%x, tooltip sub +0x%x (view +0x%x), view province +0x%x / "
            "state +0x%x, buildings +0x%x, level +0x%x, button map +0x%x, wrapper 0x%zx bytes, controller +0x%x",
            g_itemWindow, g_itemType, g_tipSub, g_tipView, g_viewProvince, g_viewState, g_provinceBuildings, g_level,
            g_buttonMap, g_wrapperSize, ProvinceControllerOffset());
}

bool HaveProvinceBuildingRemoval() {
    return g_updateFn && g_tooltipFn && g_dtorFn && g_itemWindow && g_itemType && g_tipSub && g_tipView &&
           g_typeProvinceFlag && g_viewProvince && g_viewState && g_provinceBuildings && g_buttonMap && g_buttonLookup &&
           g_wrapperCtor && g_wrapperDtor && g_wrapperSize && g_lookupBuilding && g_permission && g_setLevel && g_level &&
           g_dateDisp && g_stateRemovalDate &&
           g_hideDisp && g_showDisp && g_flags && g_hiddenBit && g_isEnabledDisp && g_enableDisp && g_disableDisp &&
           g_vtItem && g_vtWindow && ProvinceControllerOffset() && HaveCountries();
}

bool HookProvinceBuildingRemoval(ProvinceRemoveAllowed_t allowed, ProvinceRemoveClick_t onClick,
                                 ProvinceRemoveFault_t onFault) {
    if (!HaveProvinceBuildingRemoval() || !allowed || !onClick) return false;
    g_allowedFn = allowed;
    g_clickFn = onClick;
    g_faultFn = onFault;
    if (!HookQueue(g_updateFn, (void*)&UpdateDetour, (void**)&g_updateOrig, "province_building_update") ||
        !HookQueue(g_tooltipFn, (void*)&TooltipDetour, (void**)&g_tooltipOrig, "province_building_tooltip") ||
        !HookQueue(g_dtorFn, (void*)&DtorDetour, (void**)&g_dtorOrig, "province_building_dtor"))
        return false;
    // Optional: the confirmation dialog. All three or none (a dialog opened here must have its OK
    // and validity answered here).
    g_confirmHooked =
        HaveConfirm() &&
        HookQueue(g_confirmDtorFn, (void*)&ConfirmDtorDetour, (void**)&g_confirmDtorOrig, "confirm_remove_dtor") &&
        HookQueue(g_confirmValidFn, (void*)&ConfirmValidDetour, (void**)&g_confirmValidOrig, "confirm_remove_valid") &&
        HookQueue(g_confirmAcceptFn, (void*)&ConfirmAcceptDetour, (void**)&g_confirmAcceptOrig, "confirm_remove_accept");
    Log("province_building_removal: %s", g_confirmHooked ? "the cross asks for confirmation (vanilla's dialog)"
                                                          : "no confirmation dialog; the cross removes at once");
    return true;
}

void DisableProvinceBuildingRemoval() {
    if (g_enabled) Log("province_building_removal: switched off for this session");
    g_enabled = false;
}

static int RemoveBody(void* province, void* type, void* state, std::string* why) {
    if (!g_enabled) {
        *why = "the feature is switched off";
        return -1;
    }
    if (!HaveProvinceBuildingRemoval() || !province || !IsProvinceType(type) || !state) {
        *why = "no province, type or state";
        return -1;
    }
    void* building = g_lookupBuilding((uint8_t*)province + g_provinceBuildings, type);
    int level = building ? Rd<int16_t>(building, g_level) : 0;
    if (level <= 0) {
        *why = "the province has no level of that building";
        return -1;
    }
    if (!PlayerControls(province)) {
        *why = "the player does not control the province";
        return -1;
    }
    if (!g_permission(state, building, nullptr)) {
        *why = "the removal cooldown blocks it";
        return -1;
    }
    void* gs = GameState();
    if (!gs) {
        *why = "no game";
        return -1;
    }
    // As vanilla's removal command: today becomes the state's last removal date (the wartime
    // cooldown counts from it), then one level goes with notify, so listeners and supply follow.
    int today = Rd<int>(gs, g_dateDisp);
    memcpy((uint8_t*)state + g_stateRemovalDate, &today, sizeof today);
    g_setLevel(building, level - 1, true);
    // Then, as the command does for the player's own change, the state view refreshes now rather
    // than at the next tick (the player controls the province here). Optional: without it the
    // change shows once the game runs.
    void* ui = g_ppUi ? *g_ppUi : nullptr;
    void* target = ui && g_uiPost ? ((UiTarget_t)(*(void***)ui)[g_uiSlot])(ui) : nullptr;
    if (target) g_uiPost(target, &g_uiEvent);
    return Rd<int16_t>(building, g_level);
}

int RemoveProvinceBuildingLevel(void* province, void* type, void* state, std::string* why) {
    int level = -1;
    __try {
        level = RemoveBody(province, type, state, why);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault("removal");
        level = -1;
    }
    if (level < 0 && why->empty()) *why = "a fault (see the line above)";
    return level;
}

}  // namespace Engine
