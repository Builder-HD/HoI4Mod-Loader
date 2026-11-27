// Engine access for ship_domains (NOTES.md U14): where a built ship goes, and the player's commands
// that move ships and task forces between task forces and fleets. A mod's ship domains (a navy in
// space beside the one at sea, say) are kept apart: a ship is only delivered to a naval base, task
// force or fleet of its domain, and the commands that would put ships of two domains together, or a
// fleet at a base of another domain, are refused. The policy (which archetype and province belong to
// which domain) is the plugin's; this file only reads and calls the game.
#include <windows.h>

#include <atomic>
#include <string.h>

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

using Deploy_t = void*(__fastcall*)(uint8_t* target, int count, void* line, void* a4);
using Handle_t = uint8_t*(__fastcall*)(const void* handle);
using TfBase_t = int(__fastcall*)(void* taskForce);
using CountryOf_t = void*(__fastcall*)(const int* tag);
using Bases_t = void*(__fastcall*)(void* country);
using BaseLookup_t = void*(__fastcall*)(void* manager, int province);
using Access_t = bool(__fastcall*)(void* province, const int* tag, const int* tag2, int mode);
using CanUse_t = bool(__fastcall*)(void* base, const int* tag);
using DefaultBase_t = void*(__fastcall*)(uint8_t* target);
using Stockpile_t = void(__fastcall*)(void* stockpile, const void* design, int count);
using ContinentDb_t = void*(__fastcall*)();
using ContinentLookup_t = void*(__fastcall*)(void* db, int token);
using CanExec_t = uint64_t(__fastcall*)(void* cmd, void* a2, void* a3, void* a4);
using Builder_t = void(__fastcall*)(void* view);
using EntryNew_t = void*(__fastcall*)(size_t size);
using EntryCtor_t = void*(__fastcall*)(void* entry, void* gui, void* view);
using EntrySet_t = void(__fastcall*)(void* entry, void* base);
using Sort_t = void(__fastcall*)(void* a, void* b, void* c, void* d, void* e, void* f);
using FleetList_t = void*(__fastcall*)(void* country, uint8_t* out);

// The naval deployment target (CNavalDeployment, built per line and per delivery).
const size_t kTargetTag = 0x8, kTargetTaskForce = 0xc, kTargetTheater = 0x14, kTargetBase = 0x1c, kTargetDesign = 0x20,
             kTargetInfo = 0x28, kInfoIsShip = 0x10;
// Read from the game's code where the layout was found (NOTES.md U14); checked at every use against
// the objects' classes and against each other (a ship's task force holds the ship).
const size_t kTfShips = 0x348, kTfShipCount = 0x354, kShipTaskForce = 0x728, kShipEquipment = 0x40, kEquipData = 0x8,
             kEquipCount = 0x14, kEquipEntry = 0x18, kFleetTaskForces = 0xb8, kFleetTaskForceCount = 0xc4, kFleetBase = 0xd8,
             kBaseProvince = 0x10, kBaseLevel = 0x18, kBasesData = 0x18, kBasesCount = 0x24, kStateDefinition = 0x30;

uint8_t* g_deploy = nullptr;
Deploy_t g_deployOrig = nullptr;
Handle_t g_handle = nullptr;
TfBase_t g_tfBase = nullptr;
CountryOf_t g_countryOf = nullptr;       // the stockpile's (FUN_140c093e0)
CountryOf_t g_countryOfBases = nullptr;  // the default base's (FUN_140c09940)
Bases_t g_bases = nullptr;
BaseLookup_t g_baseLookup = nullptr;
Access_t g_access = nullptr;
CanUse_t g_canUse = nullptr;
DefaultBase_t g_defaultBase = nullptr;
Stockpile_t g_stockpile = nullptr;
uint32_t g_stockpileDisp = 0;
ContinentDb_t g_continentDb = nullptr;
ContinentLookup_t g_continentLookup = nullptr;
uint32_t g_continentDisp = 0;
void** g_vtShip = nullptr;
void** g_vtTaskForce = nullptr;
void** g_vtFleet = nullptr;
ShipDomainHooks g_hooks = {};
// The "Deploy target" list: while it is built, the line's domain (-1 outside); task forces and bases of
// another domain are left out of it (UI only).
uint8_t* g_builder = nullptr;
Builder_t g_builderOrig = nullptr;
uint8_t* g_fleetList = nullptr;
FleetList_t g_fleetListOrig = nullptr;
CanUse_t g_canUseOrig = nullptr;
void** g_vtDeployment = nullptr;
thread_local int t_listDomain = -1;
thread_local void* t_listView = nullptr;
// The list's naval base entries: a domain's own bases added after the game's are sorted.
uint32_t g_viewGui = 0, g_guiObject = 0;     // view -> +a -> +b: the game GUI the entries are made with
uint32_t g_poolData = 0, g_poolCount = 0;    // the window's naval base entries (CPdxArray)
uint32_t g_entrySize = 0;
EntryNew_t g_entryNew = nullptr;
EntryCtor_t g_entryCtor = nullptr;
EntrySet_t g_entrySet = nullptr;
uint8_t* g_sort[2] = {nullptr, nullptr};
Sort_t g_sortOrig[2] = {nullptr, nullptr};
const size_t kLineDeployment = 0x110;  // a naval line's CNavalDeployment (the target command's execute)
std::atomic<bool> g_off{false};       // a fault or a layout surprise: everything passes to the game
std::atomic<int> g_layoutLogs{0};

enum Cmd { kMoveShips, kDetachMerge, kSetFleet, kSetFleetBase, kSetDeployTarget, kReorganize, kCreateFleet, kMergeNavies, kTheaterFleet, kCmdCount };
const char* const kCmdClass[kCmdCount] = {".?AVCMoveShipsCommand@@", ".?AVCNavyDetachShipsAndMergeCommand@@",
                                          ".?AVCSetFleetCommand@@", ".?AVCSetFleetHomeBaseCommand@@",
                                          ".?AVCSetNavalDeploymentTargetCommand@@", ".?AVCReorganizeShipsCommand@@",
                                          ".?AVCCreateFleetCommand@@", ".?AVCMergeNaviesCommand@@",
                                          ".?AVCSetNavyTheaterGroupForCommand@@"};
uint8_t* g_canExec[kCmdCount] = {};
CanExec_t g_canExecOrig[kCmdCount] = {};

void Layout(const char* what) {
    if (g_layoutLogs.fetch_add(1) < 5) Log("ship_domains: %s; that check lets everything through", what);
}

bool IsA(const void* obj, void** vt) {
    return obj && vt && *(void* const*)obj == (void*)vt;
}

int ArchetypeDomain(const void* archetype) { return archetype ? g_hooks.archetypeDomain(archetype) : 0; }

int ProvinceDomainOf(int province) { return province > 0 ? g_hooks.provinceDomain(province) : -1; }

// A ship's domain from the archetypes in its equipment (a hull's): the first with a domain, else 0.
// -1 if unreadable.
int ShipDomain(const uint8_t* ship) {
    if (!IsA(ship, g_vtShip)) return -1;
    const uint8_t* equip = ship + kShipEquipment;
    int n = Rd<int>(equip, kEquipCount);
    const uint8_t* data = Rd<const uint8_t*>(equip, kEquipData);
    if (n < 1 || n > 64 || !data) return -1;
    for (int i = 0; i < n; ++i) {
        int d = ArchetypeDomain(Rd<const void*>(data, (size_t)i * kEquipEntry));
        if (d > 0) return d;
    }
    return 0;
}

// A task force's domain: its ships' (-2 when they differ), -1 when it has none or is unreadable.
int TaskForceDomain(uint8_t* tf) {
    if (!IsA(tf, g_vtTaskForce)) return -1;
    int n = Rd<int>(tf, kTfShipCount);
    uint8_t* const* ships = Rd<uint8_t* const*>(tf, kTfShips);
    if (n < 0 || n > 10000 || (n > 0 && !ships)) {
        Layout("a task force's ship list is not as analysed");
        return -1;
    }
    int domain = -1;
    for (int i = 0; i < n; ++i) {
        uint8_t* ship = ships[i];
        if (!IsA(ship, g_vtShip) || Rd<uint8_t*>(ship, kShipTaskForce) != tf) {
            Layout("a task force's ships are not ships of that task force");
            return -1;
        }
        int d = ShipDomain(ship);
        if (d < 0) return -1;
        if (domain >= 0 && d != domain) return -2;
        domain = d;
    }
    return domain;
}

// A fleet's domain: its task forces' (-2 when they differ), else its home base's; -1 if unknown.
int FleetDomain(uint8_t* fleet) {
    if (!IsA(fleet, g_vtFleet)) return -1;
    int n = Rd<int>(fleet, kFleetTaskForceCount);
    uint8_t* const* tfs = Rd<uint8_t* const*>(fleet, kFleetTaskForces);
    if (n < 0 || n > 10000 || (n > 0 && !tfs)) {
        Layout("a fleet's task force list is not as analysed");
        return -1;
    }
    int domain = -1;
    for (int i = 0; i < n; ++i) {
        int d = TaskForceDomain(tfs[i]);
        if (d == -2) return -2;
        if (d < 0) continue;
        if (domain >= 0 && d != domain) return -2;
        domain = d;
    }
    if (domain >= 0) return domain;
    const void* base = Rd<const void*>(fleet, kFleetBase);
    int province = base ? ProvinceIdOf(base) : 0;
    return province > 0 && ProvinceById(province) == base ? ProvinceDomainOf(province) : -1;
}

bool EmptyHandle(const uint8_t* h) { return Rd<int>(h, 0) == 0 && Rd<int>(h, 4) == 0; }

uint8_t* Lookup(const uint8_t* h) { return EmptyHandle(h) ? nullptr : g_handle(h); }

uint8_t* TaskForceOf(const uint8_t* h) {
    uint8_t* p = Lookup(h);
    return p && p != (uint8_t*)0x10 ? p - 0x10 : nullptr;
}

// A base of a domain is usable by the country that controls its province (and has a naval base there:
// the game's base table). The game's own rule (the province's area the country's) never passes for a
// base on another world.
bool DomainBaseUsable(int id, const int* tag) {
    void* province = ProvinceById(id);
    return province && ProvinceIdOf(province) == id && g_access(province, tag, tag, 1) &&
           IsSameCountry(ProvinceController(id), *tag);
}

// The best naval base of a domain the target's country may use, as the game's default picks one:
// the highest level, the first on a tie. 0 if none.
int BestBase(uint8_t* target, int domain) {
    const int* tag = (const int*)(target + kTargetTag);
    void* country = g_countryOfBases(tag);
    void* list = country ? g_bases(country) : nullptr;
    if (!list) return 0;
    void* const* bases = Rd<void* const*>(list, kBasesData);
    int n = Rd<int>(list, kBasesCount);
    if (n < 0 || n > 100000 || (n > 0 && !bases)) return 0;
    int best = 0, bestLevel = 0;
    if (domain > 0) {
        // A domain's bases by its provinces (the list holds the bases the map started with only).
        const int* ids = nullptr;
        int m = g_hooks.domainProvinces ? g_hooks.domainProvinces(domain, &ids) : 0;
        for (int i = 0; i < m && ids; ++i) {
            void* base = g_baseLookup(list, ids[i]);
            if (!base || Rd<int>(base, kBaseProvince) != ids[i]) continue;
            if (!DomainBaseUsable(ids[i], tag)) continue;
            int level = Rd<int>(base, kBaseLevel);
            if (level > bestLevel) best = ids[i], bestLevel = level;
        }
        return best;
    }
    for (int i = 0; i < n; ++i) {
        void* base = bases[i];
        if (!base || !g_canUse(base, tag)) continue;
        int level = Rd<int>(base, kBaseLevel);
        if (level <= bestLevel) continue;
        int province = Rd<int>(base, kBaseProvince);
        if (ProvinceDomainOf(province) != domain) continue;
        best = province, bestLevel = level;
    }
    return best;
}

enum Action { kKeep, kRedirect, kStock };

// Where the game would deliver and whether that is the ships' domain.
Action Decide(uint8_t* target, int count, int* from, int* to, int* domain) {
    const uint8_t* info = Rd<const uint8_t*>(target, kTargetInfo);
    const void* design = Rd<const void*>(target, kTargetDesign);
    if (count < 1 || !info || !Rd<uint8_t>(info, kInfoIsShip) || !design) return kKeep;  // not ships: the stockpile
    int want = ShipDesignDomain(design);
    *domain = want;
    bool wrong = false;
    int province = 0;
    if (uint8_t* tf = TaskForceOf(target + kTargetTaskForce)) {
        int d = TaskForceDomain(tf);
        if (d == -2 || (d >= 0 && d != want)) wrong = true;
        province = g_tfBase(tf);
    }
    if (province <= 0) province = Rd<int>(target, kTargetBase);
    if (province <= 0) {
        void* base = g_defaultBase(target);
        province = base ? Rd<int>(base, kBaseProvince) : 0;
    }
    *from = province;
    if (!wrong && province > 0 && ProvinceDomainOf(province) == want) return kKeep;
    if (!wrong && province <= 0 && want == 0) return kKeep;  // no base at all: the game's own handling
    *to = BestBase(target, want);
    return *to > 0 ? kRedirect : kStock;
}

Action DecideSafe(uint8_t* target, int count, int* from, int* to, int* domain) {
    __try {
        return Decide(target, count, from, to, domain);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_off = true;
        Log("ERROR: ship_domains: fault while choosing where ships go; ship domains are off for this session");
        return kKeep;
    }
}

void StockSafe(uint8_t* target, int count) {
    __try {
        void* country = g_countryOf((const int*)(target + kTargetTag));
        void* stockpile = country ? Rd<void*>(country, g_stockpileDisp) : nullptr;
        if (stockpile) g_stockpile(stockpile, Rd<const void*>(target, kTargetDesign), count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_off = true;
        Log("ERROR: ship_domains: fault while stockpiling ships; ship domains are off for this session");
    }
}

void NotifySafe(int tag, const void* design, int count, int domain, int from, int to) {
    __try {
        if (g_hooks.delivered) g_hooks.delivered(tag, design, count, domain, from, to);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Every ship that comes into being: to a naval base of its domain (the target pointed there for the
// call and put back after), or, with none, into the stockpile.
void* __fastcall HookDeploy(uint8_t* target, int count, void* line, void* a4) {
    if (g_off || !target || !g_hooks.archetypeDomain || !g_hooks.provinceDomain) return g_deployOrig(target, count, line, a4);
    int from = 0, to = 0, domain = 0;
    Action a = DecideSafe(target, count, &from, &to, &domain);
    if (a == kKeep) return g_deployOrig(target, count, line, a4);
    int tag = Rd<int>(target, kTargetTag);
    const void* design = Rd<const void*>(target, kTargetDesign);
    if (a == kStock) {
        StockSafe(target, count);
        NotifySafe(tag, design, count, domain, from, 0);
        return nullptr;
    }
    uint8_t saved[kTargetDesign - kTargetTaskForce];
    memcpy(saved, target + kTargetTaskForce, sizeof saved);
    memset(target + kTargetTaskForce, 0, kTargetBase - kTargetTaskForce);  // no task force, no theater
    Wr<int>(target, kTargetBase, to);
    void* r = g_deployOrig(target, count, line, a4);
    memcpy(target + kTargetTaskForce, saved, sizeof saved);
    NotifySafe(tag, design, count, domain, from, to);
    return r;
}

// --- The deploy target list: only the line's domain ----------------------------------------------------

// The domain of the ships a list's line makes; -1 if unknown (then nothing is left out). The list
// window holds its naval line's handle at +0x600 (a ship refit line's at +0x608), as its entries'
// click handlers read it to make the deployment target command; checked by the line's class.
const size_t kListLineHandle = 0x600;
int ListDomain(void* view) {
    uint8_t* line = view ? Lookup((const uint8_t*)view + kListLineHandle) : nullptr;
    if (!IsA(line, NavalLineVtable())) return -1;
    const void* design = Rd<const void*>(line, LineEquipmentOffset());
    return design ? ShipDesignDomain(design) : -1;
}

int ListDomainSafe(void* view) {
    __try {
        return ListDomain(view);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_off = true;
        Log("ERROR: ship_domains: fault while reading a deploy target list's line; ship domains are off for this session");
        return -1;
    }
}

void __fastcall HookBuilder(void* view) {
    int domain = g_off || !g_hooks.archetypeDomain ? -1 : ListDomainSafe(view);
    int saved = t_listDomain;
    void* savedView = t_listView;
    t_listDomain = domain, t_listView = view;
    g_builderOrig(view);
    t_listDomain = saved, t_listView = savedView;
}

// After the game's naval base entries are sorted: one more for each base of the line's domain the
// country can use (DomainBaseUsable; the game lists only bases connected to the capital), made as the game makes
// them, appended to the window's entries (the list is filled from them right after).
void AddDomainBases(void* view, int domain) {
    uint8_t* line = Lookup((const uint8_t*)view + kListLineHandle);
    uint8_t* deployment = IsA(line, NavalLineVtable()) ? Rd<uint8_t*>(line, kLineDeployment) : nullptr;
    if (!IsA(deployment, g_vtDeployment)) return;
    const int* tag = (const int*)(deployment + kTargetTag);
    void* country = g_countryOfBases(tag);
    void* manager = country ? g_bases(country) : nullptr;
    void* inner = Rd<void*>(view, g_viewGui);
    void* gui = inner ? Rd<void*>(inner, g_guiObject) : nullptr;
    const int* ids = nullptr;
    int m = g_hooks.domainProvinces ? g_hooks.domainProvinces(domain, &ids) : 0;
    if (!manager || !gui || !ids) return;
    uint8_t* pool = (uint8_t*)view + g_poolData;  // data, capacity +8, count +0xc, allocator +0x10
    for (int i = 0; i < m; ++i) {
        void* base = g_baseLookup(manager, ids[i]);
        if (!base || Rd<int>(base, kBaseProvince) != ids[i]) continue;
        if (!DomainBaseUsable(ids[i], tag)) continue;
        int count = Rd<int>(pool, 0xc), cap = Rd<int>(pool, 8);
        void** data = Rd<void**>(pool, 0);
        if (count < 0 || cap < count || count > 100000) return;
        if (count == cap) {
            void** alloc = Rd<void**>(pool, 0x10);
            if (!alloc) return;
            using Alloc_t = void*(__fastcall*)(void* self, size_t size, size_t align);
            using Free_t = void(__fastcall*)(void* self, void* p);
            int grown = cap < 8 ? 8 : cap * 2;
            void** fresh = (void**)((Alloc_t)(*(void***)alloc)[1])(alloc, (size_t)grown * 8, 8);
            if (!fresh) return;
            if (count) memcpy(fresh, data, (size_t)count * 8);
            if (data) ((Free_t)(*(void***)alloc)[2])(alloc, data);
            Wr<void**>(pool, 0, fresh);
            Wr<int>(pool, 8, grown);
            data = fresh;
        }
        void* entry = g_entryNew(g_entrySize);
        if (!entry) return;
        entry = g_entryCtor(entry, gui, view);
        g_entrySet(entry, base);
        data[count] = entry;
        Wr<int>(pool, 0xc, count + 1);
    }
}

void AddDomainBasesSafe(void* view, int domain) {
    __try {
        AddDomainBases(view, domain);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_off = true;
        Log("ERROR: ship_domains: fault while adding a domain's bases to a deploy target list; ship domains are off for this session");
    }
}

template <int W>
void __fastcall HookSort(void* a, void* b, void* c, void* d, void* e, void* f) {
    g_sortOrig[W](a, b, c, d, e, f);
    void* view = t_listView;
    if (view && t_listDomain > 0 && !g_off && a == Rd<void*>(view, g_poolData)) AddDomainBasesSafe(view, t_listDomain);
}

// The country's task forces for the list: those of another domain out (unknown ones stay).
void FilterTaskForces(uint8_t* out, int want) {
    uint8_t** data = Rd<uint8_t**>(out, 0);
    int n = Rd<int>(out, 0xc), kept = 0;
    if (!data || n <= 0 || n > 100000) return;
    for (int i = 0; i < n; ++i) {
        uint8_t* tf = data[i];
        int d = TaskForceDomain(tf);
        if (d == -1 && IsA(tf, g_vtTaskForce)) {
            uint8_t* fleet = Rd<uint8_t*>(tf, 0x340);
            d = IsA(fleet, g_vtFleet) ? FleetDomain(fleet) : -1;
        }
        if (d == -1 || d == want) data[kept++] = tf;
    }
    Wr<int>(out, 0xc, kept);
}

void FilterSafe(uint8_t* out, int want) {
    __try {
        FilterTaskForces(out, want);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_off = true;
        Log("ERROR: ship_domains: fault while filtering a deploy target list; ship domains are off for this session");
    }
}

void* __fastcall HookFleetList(void* country, uint8_t* out) {
    void* r = g_fleetListOrig(country, out);
    if (t_listDomain >= 0 && out && !g_off) FilterSafe(out, t_listDomain);
    return r;
}

// While a deploy target list is built or a deployment target command is checked for a line of a known
// domain: bases of other domains can't be used, the domain's own by DomainBaseUsable. Everywhere else
// the game's.
bool __fastcall HookCanUse(void* base, const int* tag) {
    bool r = g_canUseOrig(base, tag);
    if (t_listDomain < 0 || g_off || !base) return r;
    int id = Rd<int>(base, kBaseProvince);
    int d = ProvinceDomainOf(id);
    if (d != t_listDomain) return false;
    if (r || d == 0) return r;
    return DomainBaseUsable(id, tag);
}

// --- The commands: refused when they would put two domains together --------------------------------

uint8_t* ShipOf(const uint8_t* h) {
    uint8_t* p = Lookup(h);
    return IsA(p, g_vtShip) ? p : nullptr;
}

// Ships (an array of handles) into a task force: the ships' domain against the task force's (or, if
// it has no ships, its fleet's).
bool ShipsFit(const uint8_t* cmd, size_t arrayAt, size_t countAt, size_t tfAt) {
    uint8_t* tf = TaskForceOf(cmd + tfAt);
    if (!tf) return true;  // a new task force (a split)
    if (!IsA(tf, g_vtTaskForce)) {
        Layout("a move-ships command's task force is not a task force");
        return true;
    }
    int n = Rd<int>(cmd, countAt);
    const uint8_t* arr = Rd<const uint8_t*>(cmd, arrayAt);
    if (n < 1 || n > 10000 || !arr) return true;
    int ships = -1;
    for (int i = 0; i < n; ++i) {
        uint8_t* ship = ShipOf(arr + (size_t)i * 8);
        if (!ship) {
            Layout("a move-ships command's ship is not a ship");
            return true;
        }
        int d = ShipDomain(ship);
        if (d < 0) return true;
        if (ships >= 0 && d != ships) return false;
        ships = d;
    }
    int there = TaskForceDomain(tf);
    if (there == -1) {
        uint8_t* fleet = Rd<uint8_t*>(tf, 0x340);  // the task force's fleet (as its base lookup reads it)
        there = IsA(fleet, g_vtFleet) ? FleetDomain(fleet) : -1;
    }
    return there < 0 ? there != -2 : there == ships;
}

// Task forces (handles at arrayAt, count at countAt) of one domain, and of the fleet's if one is given
// (none: a new fleet).
bool TaskForceListFits(const uint8_t* cmd, uint8_t* fleet, size_t arrayAt, size_t countAt, const char* what) {
    int domain = fleet ? FleetDomain(fleet) : -1;
    if (domain == -2) domain = -1;  // already mixed (from before): no worse
    int n = Rd<int>(cmd, countAt);
    const uint8_t* arr = Rd<const uint8_t*>(cmd, arrayAt);
    if (n < 1 || n > 10000 || !arr) return true;
    for (int i = 0; i < n; ++i) {
        uint8_t* tf = TaskForceOf(arr + (size_t)i * 8);
        if (!tf) continue;
        if (!IsA(tf, g_vtTaskForce)) {
            Layout(what);
            return true;
        }
        int d = TaskForceDomain(tf);
        if (d == -2) return false;
        if (d < 0) continue;
        if (domain >= 0 && d != domain) return false;
        domain = d;
    }
    return true;
}

// Task forces into a fleet (fleet handle +0x28; empty: a new fleet), task force handles +0x30 / +0x3c.
bool TaskForcesFit(const uint8_t* cmd) {
    uint8_t* fleet = Lookup(cmd + 0x28);
    if (fleet && !IsA(fleet, g_vtFleet)) {
        Layout("a set-fleet command's fleet is not a fleet");
        return true;
    }
    return TaskForceListFits(cmd, fleet, 0x30, 0x3c, "a set-fleet command's task force is not a task force");
}

bool BaseFits(const uint8_t* cmd) {
    uint8_t* fleet = Lookup(cmd + 0x28);
    if (!IsA(fleet, g_vtFleet)) {
        if (fleet) Layout("a home-base command's fleet is not a fleet");
        return true;
    }
    int domain = FleetDomain(fleet);
    int province = Rd<int>(cmd, 0x30);
    if (domain < 0 || province <= 0 || province >= ProvinceCount()) return true;
    return ProvinceDomainOf(province) == domain;
}

bool DeployTargetFits(const uint8_t* cmd) {
    uint8_t* line = Lookup(cmd + 0x28);
    if (!IsA(line, NavalLineVtable())) return true;  // not a naval line (a refit line has its own command)
    const void* design = Rd<const void*>(line, LineEquipmentOffset());
    if (!design) return true;
    int want = ShipDesignDomain(design);
    if (uint8_t* tf = TaskForceOf(cmd + 0x30)) {
        if (!IsA(tf, g_vtTaskForce)) {
            Layout("a deployment target command's task force is not a task force");
            return true;
        }
        int d = TaskForceDomain(tf);
        if (d == -1) {
            uint8_t* fleet = Rd<uint8_t*>(tf, 0x340);
            d = IsA(fleet, g_vtFleet) ? FleetDomain(fleet) : -1;
        }
        return d < 0 ? d != -2 : d == want;
    }
    int province = Rd<int>(cmd, 0x40);
    if (province > 0 && province < ProvinceCount()) return ProvinceDomainOf(province) == want;
    return true;  // a theater, or the default: the delivery picks a base of the domain
}

// The fleet window's reorganisation (drag and drop): the fleet (+0x48) and its new task forces, each
// an entry of 0x80 (+0x30, count +0x3c) with its ship handles (+0x58, count +0x64): one domain only.
bool ReorganizeFits(const uint8_t* cmd) {
    int n = Rd<int>(cmd, 0x3c);
    const uint8_t* entries = Rd<const uint8_t*>(cmd, 0x30);
    if (n < 1 || n > 1000 || !entries) return true;
    int domain = -1;
    uint8_t* fleet = Lookup(cmd + 0x48);
    if (IsA(fleet, g_vtFleet)) {
        int d = FleetDomain(fleet);
        if (d >= 0) domain = d;
    }
    for (int i = 0; i < n; ++i) {
        const uint8_t* e = entries + (size_t)i * 0x80;
        int m = Rd<int>(e, 0x64);
        const uint8_t* ships = Rd<const uint8_t*>(e, 0x58);
        if (m < 0 || m > 10000 || (m > 0 && !ships)) {
            Layout("a reorganize command's entries are not as analysed");
            return true;
        }
        for (int j = 0; j < m; ++j) {
            if (EmptyHandle(ships + (size_t)j * 8)) continue;
            uint8_t* ship = ShipOf(ships + (size_t)j * 8);
            if (!ship) {
                Layout("a reorganize command's ship is not a ship");
                return true;
            }
            int d = ShipDomain(ship);
            if (d < 0) return true;
            if (domain >= 0 && d != domain) return false;
            domain = d;
        }
    }
    return true;
}

// A new fleet from selected task forces (handles +0x38, count +0x44) and ships (+0x50, count +0x5c):
// all of one domain.
bool CreateFleetFits(const uint8_t* cmd) {
    int domain = -1;
    int n = Rd<int>(cmd, 0x44);
    const uint8_t* arr = Rd<const uint8_t*>(cmd, 0x38);
    if (n < 0 || n > 10000 || (n > 0 && !arr)) return true;
    for (int i = 0; i < n; ++i) {
        uint8_t* tf = TaskForceOf(arr + (size_t)i * 8);
        if (!tf) continue;
        if (!IsA(tf, g_vtTaskForce)) {
            Layout("a create-fleet command's task force is not a task force");
            return true;
        }
        int d = TaskForceDomain(tf);
        if (d == -2) return false;
        if (d < 0) continue;
        if (domain >= 0 && d != domain) return false;
        domain = d;
    }
    n = Rd<int>(cmd, 0x5c);
    arr = Rd<const uint8_t*>(cmd, 0x50);
    if (n < 0 || n > 10000 || (n > 0 && !arr)) return true;
    for (int i = 0; i < n; ++i) {
        if (EmptyHandle(arr + (size_t)i * 8)) continue;
        uint8_t* ship = ShipOf(arr + (size_t)i * 8);
        if (!ship) {
            Layout("a create-fleet command's ship is not a ship");
            return true;
        }
        int d = ShipDomain(ship);
        if (d < 0) return true;
        if (domain >= 0 && d != domain) return false;
        domain = d;
    }
    return true;
}

// The domain of a deployment target command's line (for the game's own check of a base), -1 unknown.
int DeployCommandDomain(const uint8_t* cmd) {
    uint8_t* line = Lookup(cmd + 0x28);
    if (!IsA(line, NavalLineVtable())) return -1;
    const void* design = Rd<const void*>(line, LineEquipmentOffset());
    return design ? ShipDesignDomain(design) : -1;
}

int DeployCommandDomainSafe(const uint8_t* cmd) {
    __try {
        return DeployCommandDomain(cmd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool FitsSafe(int which, const uint8_t* cmd) {
    __try {
        switch (which) {
            case kMoveShips:
            case kDetachMerge: return ShipsFit(cmd, 0x28, 0x34, 0x40);
            case kSetFleet: return TaskForcesFit(cmd);
            case kSetFleetBase: return BaseFits(cmd);
            case kSetDeployTarget: return DeployTargetFits(cmd);
            case kReorganize: return ReorganizeFits(cmd);
            case kCreateFleet: return CreateFleetFits(cmd);
            case kMergeNavies: return TaskForceListFits(cmd, nullptr, 0x28, 0x34, "a merge command's task force is not a task force");
            // Into a theater: whole fleets (+0x38) as they are; task forces (+0x50 / +0x5c) into a new fleet.
            case kTheaterFleet: return TaskForceListFits(cmd, nullptr, 0x50, 0x5c, "a theater command's task force is not a task force");
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_off = true;
        Log("ERROR: ship_domains: fault while checking a naval command; ship domains are off for this session");
        return true;
    }
}

template <int W>
uint64_t __fastcall HookCanExec(void* cmd, void* a2, void* a3, void* a4) {
    int context = -1;
    if constexpr (W == kSetDeployTarget) {
        // The game's own check of a naval base target: a base of the line's domain counts as usable.
        if (g_canUseOrig && !g_off && g_hooks.archetypeDomain) context = DeployCommandDomainSafe((const uint8_t*)cmd);
    }
    int saved = t_listDomain;
    if (context >= 0) t_listDomain = context;
    uint64_t r = g_canExecOrig[W](cmd, a2, a3, a4);
    t_listDomain = saved;
    if (!(r & 0xff) || g_off || !g_hooks.archetypeDomain || !g_hooks.provinceDomain) return r;
    if (FitsSafe(W, (const uint8_t*)cmd)) return r;
    if (g_hooks.refused) g_hooks.refused(W);
    return r & ~(uint64_t)0xff;
}

void* const kCanExecDetours[kCmdCount] = {(void*)&HookCanExec<0>, (void*)&HookCanExec<1>, (void*)&HookCanExec<2>,
                                          (void*)&HookCanExec<3>, (void*)&HookCanExec<4>, (void*)&HookCanExec<5>,
                                          (void*)&HookCanExec<6>, (void*)&HookCanExec<7>,
                                          (void*)&HookCanExec<8>};

bool Fail(const char* why) {
    Log("ship_domains: %s; ship domains unavailable", why);
    return false;
}

bool ResolveAll() {
    if (!HaveProduction() || !HaveProvinceData() || !LineEquipmentOffset() || !NavalLineVtable())
        return Fail("needs production_data's designs and naval lines and the province data");
    uint8_t* br = SigFindUnique("ship_deploy_branch", kSigShipDeployBranch);
    if (!br) return Fail("where built ships go was not found");
    DWORD64 base = 0;
    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry((DWORD64)br, &base, nullptr);
    if (!f || (f->UnwindData & 1) || ((Rd<uint8_t>((void*)base, f->UnwindData) >> 3) & 4)) return Fail("the deploy function's start is unknown");
    g_deploy = (uint8_t*)(base + f->BeginAddress);
    if (br - g_deploy > 0x400) return Fail("the deploy function's start is too far from its ship branch");
    g_countryOf = (CountryOf_t)SigFollowRel32(br + kSigShipDeployCountry);
    g_stockpileDisp = Rd<uint32_t>(br, kSigShipDeployStockpileDisp);
    g_stockpile = (Stockpile_t)SigFollowRel32(br + kSigShipDeployStockpileAdd);
    g_handle = (Handle_t)SigFollowRel32(br + kSigShipDeployHandle);
    g_tfBase = (TfBase_t)SigFollowRel32(br + kSigShipDeployTfBase);
    uint8_t* def = SigFollowRel32(br + kSigShipDeployDefault);
    if (!SigMatchAt(def, kCodeShipDefaultBase)) return Fail("the default naval base has an unexpected shape");
    g_defaultBase = (DefaultBase_t)def;
    g_countryOfBases = (CountryOf_t)SigFollowRel32(def + kCodeShipBaseCountry);
    g_bases = (Bases_t)SigFollowRel32(def + kCodeShipBaseList);
    g_canUse = (CanUse_t)SigFollowRel32(def + kCodeShipBaseCanUse);
    uint8_t* look = SigFindUnique("ship_base_by_province", kSigShipBaseByProvince);
    if (!look || (Bases_t)SigFollowRel32(look + kSigShipBaseManager) != g_bases)
        return Fail("the naval base lookup by province was not found, or reads another base manager");
    g_baseLookup = (BaseLookup_t)SigFollowRel32(look + kSigShipBaseLookup);
    uint8_t* access = SigFindUnique("ship_base_access", kSigShipBaseAccess);
    if (!access || (CountryOf_t)SigFollowRel32(access + kSigShipBaseAccessCountry) != g_countryOfBases)
        return Fail("the naval base access test was not found, or reads countries another way");
    g_access = (Access_t)SigFollowRel32(access + kSigShipBaseAccessCall);
    uint8_t* areaOf = SigFollowRel32(access + kSigShipBaseAccessArea);
    if (g_stockpileDisp < 0x100 || g_stockpileDisp > 0x10000) return Fail("the country's stockpile field is out of range");
    size_t n = 0;
    void** cont = FindVtableByRtti(".?AVCIsOnContinentTrigger@@", &n);
    uint8_t* read = cont && n > 22 ? SigFindIn((uint8_t*)cont[22], 0x200, kCodeContinentRead) : nullptr;
    if (!read) return Fail("the continent trigger's province read was not found");
    g_continentDisp = Rd<uint32_t>(read, kCodeContinentDisp);
    g_continentDb = (ContinentDb_t)SigFollowRel32(read + kCodeContinentDb);
    g_continentLookup = (ContinentLookup_t)SigFollowRel32(read + kCodeContinentLookup);
    // The deploy target list (optional: without it the list shows everything, the commands still refuse).
    if (uint8_t* list = SigFindUnique("ship_deploy_list", kSigShipDeployList)) {
        DWORD64 lb = 0;
        PRUNTIME_FUNCTION lf = RtlLookupFunctionEntry((DWORD64)list, &lb, nullptr);
        uint8_t* builder = lf && !(lf->UnwindData & 1) && !((Rd<uint8_t>((void*)lb, lf->UnwindData) >> 3) & 4)
                               ? (uint8_t*)(lb + lf->BeginAddress) : nullptr;
        uint8_t* adder = SigFollowRel32(list + kSigShipDeployListFleets);
        uint8_t* call = SigFindIn(adder, 0x100, kCodeShipFleetListCall);
        uint8_t* fleets = call ? SigFollowRel32(call + 8) : nullptr;
        uint8_t* tfCall = fleets && SigMatchAt(fleets, kCodeShipFleetList) ? SigFindIn(fleets, 0x80, kCodeShipFleetTaskForces) : nullptr;
        uint8_t* tfList = tfCall ? SigFollowRel32(tfCall + 3) : nullptr;
        if (builder && list - builder < 0x400 && tfList && SigMatchAt(tfList, kCodeFleetTaskForceList) &&
            Rd<uint32_t>(tfList, 3) == kFleetTaskForces && SigFollowRel32(list + kSigShipDeployListArea) == areaOf) {
            g_builder = builder, g_fleetList = fleets;
            g_viewGui = Rd<uint32_t>(list, kSigShipDeployListView);
            g_guiObject = Rd<uint32_t>(list, kSigShipDeployListLine);
            // The base entries (optional: without them a domain's bases are not offered).
            uint8_t* mk = SigFindIn(builder, 0x1000, kCodeShipListEntryNew);
            uint8_t* set = mk ? SigFindIn(mk, 0x200, kCodeShipListEntrySet) : nullptr;
            uint8_t* small = set ? SigFindIn(set, 0x200, kCodeShipListSortSmall) : nullptr;
            uint8_t* large = small ? SigFindIn(small, 0x200, kCodeShipListSortLarge) : nullptr;
            if (mk && set && small && large && Rd<uint32_t>(set, kCodeShipListEntryData) + 0xc == Rd<uint32_t>(mk, kCodeShipListEntryCount)) {
                g_poolData = Rd<uint32_t>(set, kCodeShipListEntryData);
                g_poolCount = Rd<uint32_t>(mk, kCodeShipListEntryCount);
                g_entrySize = Rd<uint32_t>(mk, kCodeShipListEntrySize);
                g_entryNew = (EntryNew_t)SigFollowRel32(mk + kCodeShipListEntryAlloc);
                g_entryCtor = (EntryCtor_t)SigFollowRel32(mk + kCodeShipListEntryCtor);
                g_entrySet = (EntrySet_t)SigFollowRel32(set + kCodeShipListEntrySetCall);
                g_sort[0] = SigFollowRel32(small + kCodeShipListSortSmallCall);
                g_sort[1] = SigFollowRel32(large + kCodeShipListSortLargeCall);
            } else {
                Log("ship_domains: the deploy target list's base entries are not as analysed; a domain's bases are not offered");
            }
            g_vtDeployment = FindVtableByRtti(".?AVCNavalDeployment@@", &n);
        } else {
            Log("ship_domains: the deploy target list is not as analysed; it shows every target (the commands still refuse)");
        }
    }
    g_vtShip = FindVtableByRtti(".?AVCShip@@", &n);
    g_vtTaskForce = FindVtableByRtti(".?AVCTaskForce@@", &n);
    g_vtFleet = FindVtableByRtti(".?AVCFleet@@", &n);
    if (!g_vtShip || !g_vtTaskForce || !g_vtFleet) return Fail("CShip, CTaskForce or CFleet not found");
    for (int i = 0; i < kCmdCount; ++i) {
        void** vt = FindVtableByRtti(kCmdClass[i], &n);
        g_canExec[i] = vt && n > 9 ? (uint8_t*)vt[9] : nullptr;
        if (!g_canExec[i]) return Fail("a naval command's can-execute was not found");
        if (i == kTheaterFleet && !SigMatchAt(g_canExec[i], kCodeTheaterFleetCanExec))
            return Fail("the theater command's can-execute does not read its task forces as analysed");
        for (int j = 0; j < i; ++j)
            if (g_canExec[j] == g_canExec[i]) return Fail("two naval commands share their can-execute");
    }
    return true;
}

}  // namespace

void ResolveShipDomains() {
    if (!ResolveAll()) {
        g_deploy = nullptr;
        return;
    }
    Log("ship_domains: deploy %s, default base %s, stockpile +0x%x, state continent +0x%x -> +0x%x; can-execute %s / %s / %s / %s / %s",
        DescribeAddress(g_deploy).c_str(), DescribeAddress((void*)g_defaultBase).c_str(), g_stockpileDisp, (unsigned)kStateDefinition,
        g_continentDisp, DescribeAddress(g_canExec[0]).c_str(), DescribeAddress(g_canExec[1]).c_str(),
        DescribeAddress(g_canExec[2]).c_str(), DescribeAddress(g_canExec[3]).c_str(), DescribeAddress(g_canExec[4]).c_str());
}

bool HaveShipDomains() { return g_deploy != nullptr; }

bool HookShipDomains(const ShipDomainHooks& hooks) {
    if (!g_deploy) return false;
    g_hooks = hooks;
    bool ok = HookQueue(g_deploy, (void*)&HookDeploy, (void**)&g_deployOrig, "ship_domains_deploy");
    for (int i = 0; ok && i < kCmdCount; ++i)
        ok = HookQueue(g_canExec[i], kCanExecDetours[i], (void**)&g_canExecOrig[i], kCmdClass[i]);
    if (ok && g_builder && g_fleetList && g_vtDeployment) {
        bool list = HookQueue(g_builder, (void*)&HookBuilder, (void**)&g_builderOrig, "ship_domains_deploy_list") &&
                    HookQueue(g_fleetList, (void*)&HookFleetList, (void**)&g_fleetListOrig, "ship_domains_fleet_list") &&
                    HookQueue((void*)g_canUse, (void*)&HookCanUse, (void**)&g_canUseOrig, "ship_domains_base_use");
        if (!list) {
            HookUnqueue(g_builder), HookUnqueue(g_fleetList), HookUnqueue((void*)g_canUse);
            Log("ship_domains: the deploy target list could not be hooked; it shows every target");
        } else if (g_sort[0] && g_sort[1] && g_sort[0] != g_sort[1] && g_entrySize >= 0x100 && g_entrySize < 0x10000) {
            if (!HookQueue(g_sort[0], (void*)&HookSort<0>, (void**)&g_sortOrig[0], "ship_domains_list_sort_small") ||
                !HookQueue(g_sort[1], (void*)&HookSort<1>, (void**)&g_sortOrig[1], "ship_domains_list_sort_large")) {
                HookUnqueue(g_sort[0]), HookUnqueue(g_sort[1]);
                g_sort[0] = g_sort[1] = nullptr;
                Log("ship_domains: the deploy target list's sort could not be hooked; a domain's bases are not offered");
            }
        }
    }
    if (!ok) {
        HookUnqueue(g_deploy);
        for (int i = 0; i < kCmdCount; ++i)
            if (g_canExec[i]) HookUnqueue(g_canExec[i]);
        if (g_builder) HookUnqueue(g_builder);
        if (g_fleetList) HookUnqueue(g_fleetList);
        HookUnqueue((void*)g_canUse);
    }
    return ok;
}

void* ContinentByToken(int token) {
    if (!g_continentLookup || token <= 0) return nullptr;
    void* db = g_continentDb();
    return db ? g_continentLookup(db, token) : nullptr;
}

void* StateContinent(int stateId) {
    void* state = g_continentDisp ? StateById(stateId) : nullptr;
    void* def = state ? Rd<void*>(state, kStateDefinition) : nullptr;  // as the continent trigger reads it
    return def ? Rd<void*>(def, g_continentDisp) : nullptr;
}

int ShipDesignDomain(const void* design) {
    if (!design || !g_hooks.archetypeDomain) return 0;
    const void* type = Rd<const void*>(design, EquipmentTypeOffset());
    if (!type) return 0;
    const void* arch = Rd<const void*>(type, TypeArchetypeOffset());
    return ArchetypeDomain(arch ? arch : type);
}

}  // namespace Engine
