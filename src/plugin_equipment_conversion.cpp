// equipment_conversion (engine tier, needs lifecycle; NOTES.md Part J): tools a mod switches on
// with global flags. Without them the game behaves exactly as vanilla: the hooks pass through and
// no conversion list is changed.
//
// - hoi4_loader_conversion_generations: equipment of any earlier generation converts into the
//   generation a production line makes ("Convert from stockpile"), for every equipment type and mod,
//   without can_convert_from entries. At load the loader only plans each type's earlier generations
//   (its `parent =` chain within the archetype); it appends them to the game's convert-from list at
//   a main-thread safe point once the flag is set, and removes them again when it is gone.
// - Cost (with generations on): the engine's own conversion cost, times factor^(g-1) for a source g
//   generations back; hoi4_loader_conversion_factor_<percent> (default 75); only the loader's pairs
//   unless hoi4_loader_conversion_existing_loader_cost.
// - hoi4_loader_conversion_naval: the loader's naval pairs are refused in CanConvert without it.
// - hoi4_loader_conversion_role_mode: Shift+click on a line's convert button toggles "same role
//   only"; the tooltip shows the mode and the designs it leaves out.
// The separate mod "HoI4Mod Loader: Equipment Conversion" (its own repository) sets them from game rules.
#include <windows.h>

#include <atomic>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "MinHook.h"
#include "engine.h"
#include "lifecycle.h"
#include "loader.h"

namespace {

const char* const kFeature = "equipment_conversion";

// --- The flags a mod sets (read on the main thread into atomics) ------------------------------
// hoi4_loader_conversion_generations, hoi4_loader_conversion_role_mode (the two opt-ins),
// hoi4_loader_conversion_factor_<percent> (0..100; the most recently set one counts; default 75),
// hoi4_loader_conversion_existing_loader_cost, hoi4_loader_conversion_naval.
const int kDefaultFactor = 75;
const char kFactorPrefix[] = "hoi4_loader_conversion_factor_";
std::atomic<int> g_factor{kDefaultFactor};
std::atomic<bool> g_generations{false}, g_roleMode{false}, g_existingLoaderCost{false}, g_naval{false};
int g_tokGenerations = 0, g_tokRoleMode = 0, g_tokExisting = 0, g_tokNaval = 0;

// Fault or failed self-check: vanilla behaviour from then on (the loader's pairs are refused and
// costs are left alone). Not switched back on in this session.
std::atomic<bool> g_off{false};
volatile LONG g_faultLogged = 0;

void DeactivateOnMain(void* why) { FeatureDeactivate(kFeature, (const char*)why); }

void Fault(const char* where) {
    g_off.store(true);
    if (InterlockedExchange(&g_faultLogged, 1)) return;
    Log("ERROR: equipment_conversion: fault in %s; vanilla conversion from now on", where);
    if (!RunOnMainThread(&DeactivateOnMain, (void*)"a fault in a hook")) Log("equipment_conversion: switched off (no main-thread queue)");
}

// --- The loader's pairs ----------------------------------------------------------------------
// Planned at load (type -> its earlier generations); applied (appended to the game's lists) only
// while hoi4_loader_conversion_generations is set. While applied: type -> list index of its first
// loader entry (entries from there on are the loader's).
SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<void*, std::vector<const void*>> g_planned;
std::unordered_map<const void*, int> g_firstAdded;
std::atomic<long> g_plannedPairs{0}, g_plannedNaval{0}, g_builds{0};
std::atomic<bool> g_anyAdded{false};  // the lists hold loader entries right now

int FirstAdded(const void* type) {
    AcquireSRWLockShared(&g_lock);
    auto it = g_firstAdded.find(type);
    int first = it == g_firstAdded.end() ? -1 : it->second;
    ReleaseSRWLockShared(&g_lock);
    return first;
}

// Did the loader add `source` to `target`'s list (and not the game)?
bool LoaderAdded(const void* target, const void* source) {
    int first = FirstAdded(target);
    if (first < 0) return false;
    int n = Engine::ConvertListCount(target);
    for (int i = 0; i < n; ++i)
        if (Engine::ConvertListAt(target, i) == source) return i >= first;
    return false;
}

bool InList(const void* type, const void* t) {
    int n = Engine::ConvertListCount(type);
    for (int i = 0; i < n; ++i)
        if (Engine::ConvertListAt(type, i) == t) return true;
    return false;
}

// At load (read only): the type's earlier generations its list lacks.
void Plan(void* type) {
    if (Engine::TypeIsArchetype(type)) return;
    const void* arch = Engine::TypeArchetype(type);
    std::vector<const void*> add;
    for (const void* p = Engine::TypeParent(type); p && add.size() < 64; p = Engine::TypeParent(p)) {
        if (Engine::TypeArchetype(p) != arch) break;  // a generation chain stays in its archetype
        if (!InList(type, p)) add.push_back(p);
    }
    AcquireSRWLockExclusive(&g_lock);
    auto old = g_planned.find(type);
    if (old != g_planned.end()) g_plannedPairs.fetch_sub((long)old->second.size());
    if (add.empty()) {
        if (old != g_planned.end()) g_planned.erase(old);
    } else {
        g_plannedPairs.fetch_add((long)add.size());
        if (Engine::TypeIsNaval(type)) g_plannedNaval.fetch_add((long)add.size());
        g_planned[type] = std::move(add);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// Main thread, safe point only (no conversion test runs): append the planned pairs, or remove them.
void Apply(bool on, long* types, long* pairs) {
    AcquireSRWLockExclusive(&g_lock);
    if (on && g_firstAdded.empty()) {
        for (auto& e : g_planned) {
            const void* add[64];
            int n = 0;
            for (const void* p : e.second)
                if (n < 64 && !InList(e.first, p)) add[n++] = p;
            int first = Engine::ConvertListCount(e.first);
            if (!n || !Engine::ConvertListAppend(e.first, add, n)) continue;
            g_firstAdded[e.first] = first;
            ++*types;
            *pairs += n;
        }
        g_anyAdded.store(!g_firstAdded.empty());
    } else if (!on && !g_firstAdded.empty()) {
        g_anyAdded.store(false);
        for (const auto& e : g_firstAdded) {
            if (Engine::ConvertListTruncate((void*)e.first, e.second)) ++*types;
        }
        g_firstAdded.clear();
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool SafeApply(bool on) {
    long types = 0, pairs = 0;
    __try {
        Apply(on, &types, &pairs);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault(on ? "adding the earlier generations" : "removing the earlier generations");
        return false;
    }
    if (on && types) Log("equipment_conversion: earlier generations ON: %ld pairs added to %ld types' lists", pairs, types);
    if (!on && types) Log("equipment_conversion: earlier generations OFF: the loader's entries removed from %ld lists", types);
    return true;
}

void RememberType(const void* type);  // role mode, below

// --- Detours ---------------------------------------------------------------------------------
using Build_t = void(__fastcall*)(void* type, void* database);
using CanConvert_t = bool(__fastcall*)(const void* target, const void* source);
using Cost_t = int64_t*(__fastcall*)(const void* source, int64_t* out, const void* target, const void* org, int64_t* breakdown);
Build_t g_buildOrig = nullptr;
CanConvert_t g_canOrig = nullptr;
Cost_t g_costOrig = nullptr;

void __fastcall BuildDetour(void* type, void* database) {
    g_buildOrig(type, database);
    g_builds.fetch_add(1, std::memory_order_relaxed);
    if (g_off.load(std::memory_order_relaxed)) return;
    __try {
        RememberType(type);
        Plan(type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault("planning a convert-from list");
    }
}

// Thread evidence (nothing here touches graphics, but the hooks run wherever the engine calls them).
std::atomic<bool> g_canThreadLogged{false}, g_costThreadLogged{false};
void LogThread(std::atomic<bool>& done, const char* what) {
    if (done.exchange(true)) return;
    Log("equipment_conversion: first %s on thread %lu (%s)", what, GetCurrentThreadId(),
        IsMainThread() ? "the main thread" : "not the main thread");
}

// True if the engine said yes only because of a loader pair that is switched off now.
bool Refused(const void* target, const void* source) {
    const void* t = Engine::DesignType(target);
    const void* s = Engine::DesignType(source);
    if (!t || !s || !LoaderAdded(t, s)) return false;
    const void* root = Engine::DesignRoot(target);
    if (root && root == Engine::DesignRoot(source)) return false;  // the engine's other reason
    // Off, or the mod's flag gone but the entries not removed yet (that happens at the next safe point).
    if (g_off.load(std::memory_order_relaxed) || !g_generations.load(std::memory_order_relaxed)) return true;
    return !g_naval.load(std::memory_order_relaxed) && Engine::TypeIsNaval(t);
}

// --- Role mode (Shift+click on a line's convert button) -------------------------------------
// Per country and target equipment type: "same role only" (a source design converts only if its
// unit-type bits equal the line's design's: fighters into fighters, CAS into CAS). Stored as the
// global flag hoi4_loader_conversion_role_<TAG>_<type> (saved; harmless without the loader).
const char kRolePrefix[] = "hoi4_loader_conversion_role_";
SRWLOCK g_modeLock = SRWLOCK_INIT;
std::unordered_map<uint64_t, bool> g_roleOnly;              // (tag << 32 | type index) -> on
std::unordered_map<std::string, const void*> g_typeByName;  // every type the list builder saw
std::unordered_map<const void*, uint32_t> g_typeIndex;
std::atomic<bool> g_singlePlayer{false};

uint64_t ModeKey(int tag, const void* type) {
    AcquireSRWLockShared(&g_modeLock);
    auto it = g_typeIndex.find(type);
    uint64_t key = it == g_typeIndex.end() ? 0 : ((uint64_t)(uint32_t)tag << 32 | it->second);
    ReleaseSRWLockShared(&g_modeLock);
    return key;
}

bool RoleOnly(int tag, const void* type) {
    uint64_t key = ModeKey(tag, type);
    if (!key) return false;
    AcquireSRWLockShared(&g_modeLock);
    auto it = g_roleOnly.find(key);
    bool on = it != g_roleOnly.end() && it->second;
    ReleaseSRWLockShared(&g_modeLock);
    return on;
}

void RememberType(const void* type) {
    AcquireSRWLockExclusive(&g_modeLock);
    if (g_typeIndex.find(type) == g_typeIndex.end()) {
        uint32_t index = (uint32_t)g_typeIndex.size() + 1;
        g_typeIndex[type] = index;
        g_typeByName[Engine::TypeName(type)] = type;
    }
    ReleaseSRWLockExclusive(&g_modeLock);
}

// Set by the line rebuild hook while the engine rebuilds one line's sources on this thread.
const int kMaxRefused = 48;
struct RebuildCtx {
    bool active;
    const void* target;
    uint64_t role;
    int tag;
    int refusedCount;                     // designs the stockpile walk would have used
    const void* refused[kMaxRefused];
};
thread_local RebuildCtx t_rebuild = {};

// Per line: the designs its last rebuild left out for role (the tooltip lists them).
struct RefusedList {
    const void* target;
    std::vector<const void*> designs;
};
SRWLOCK g_refusedLock = SRWLOCK_INIT;
std::unordered_map<const void*, RefusedList> g_refused;

bool RoleRefused(const void* target, const void* source) {
    if (!t_rebuild.active || target != t_rebuild.target || Engine::DesignRole(source) == t_rebuild.role) return false;
    RebuildCtx& c = t_rebuild;
    if (Engine::HaveSkippedList() && c.refusedCount < kMaxRefused && Engine::StockpileUses(source, c.tag)) {
        bool seen = false;
        for (int i = 0; i < c.refusedCount && !seen; ++i) seen = c.refused[i] == source;
        if (!seen) c.refused[c.refusedCount++] = source;
    }
    return true;
}

void PublishRefused(const void* line, const RebuildCtx& c) {
    AcquireSRWLockExclusive(&g_refusedLock);
    if (c.active) {
        RefusedList& r = g_refused[line];
        r.target = c.target;
        r.designs.assign(c.refused, c.refused + c.refusedCount);
    } else {
        g_refused.erase(line);
    }
    ReleaseSRWLockExclusive(&g_refusedLock);
}

std::vector<const void*> RefusedFor(const void* line, const void* target) {
    std::vector<const void*> out;
    AcquireSRWLockShared(&g_refusedLock);
    auto it = g_refused.find(line);
    if (it != g_refused.end() && it->second.target == target) out = it->second.designs;
    ReleaseSRWLockShared(&g_refusedLock);
    return out;
}

bool __fastcall CanConvertDetour(const void* target, const void* source) {
    bool yes = g_canOrig(target, source);
    if (!yes) return false;
    if (t_rebuild.active) {
        __try {
            if (RoleRefused(target, source)) return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Fault("the role mode");
        }
    }
    if (!g_anyAdded.load(std::memory_order_relaxed)) return yes;
    LogThread(g_canThreadLogged, "CanConvert with the loader's pairs");
    __try {
        return !Refused(target, source);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault("CanConvert");
        return yes;
    }
}

// factor^(g-1), applied one generation at a time (no overflow; fixed point stays fixed point).
int64_t Scale(int64_t v, int factor, int g) {
    for (int i = 1; i < g && v; ++i) v = v * factor / 100;
    return v;
}

void Adjust(const void* source, int64_t* out, const void* target, int64_t* breakdown, const int64_t* before) {
    const void* t = Engine::DesignType(target);
    const void* s = Engine::DesignType(source);
    int g = t && s ? Engine::GenerationsBack(t, s) : 0;
    if (g < 2) return;  // one generation back (or not on the chain): the engine's cost
    if (!LoaderAdded(t, s) && !g_existingLoaderCost.load(std::memory_order_relaxed)) return;
    int factor = g_factor.load(std::memory_order_relaxed);
    if (factor == 100) return;
    out[0] = Scale(out[0], factor, g);
    // The tooltip's breakdown: the IC parts (0, 2, 4, 6) were added to; 1, 3, 5, 7 are modifiers.
    if (breakdown)
        for (int i = 0; i < 8; i += 2) breakdown[i] = before[i] + Scale(breakdown[i] - before[i], factor, g);
}

bool Snapshot(const int64_t* breakdown, int64_t* before) {
    __try {
        memcpy(before, breakdown, 8 * sizeof(int64_t));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int64_t* __fastcall CostDetour(const void* source, int64_t* out, const void* target, const void* org, int64_t* breakdown) {
    int64_t before[8] = {};
    bool active = !g_off.load(std::memory_order_relaxed) && g_generations.load(std::memory_order_relaxed);
    if (active && breakdown && !Snapshot(breakdown, before)) active = false;
    int64_t* r = g_costOrig(source, out, target, org, breakdown);
    if (!active || !out) return r;
    LogThread(g_costThreadLogged, "conversion cost");
    __try {
        Adjust(source, out, target, breakdown, before);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault("the conversion cost");
    }
    return r;
}

using Rebuild_t = void(__fastcall*)(void* line, const int* tag);
using Click_t = void(__fastcall*)(void* item);
using Tooltip_t = void(__fastcall*)(void* line, void* arg, EngineString* out);
Rebuild_t g_rebuildOrig = nullptr;
Click_t g_clickOrig = nullptr;
Tooltip_t g_tooltipOrig = nullptr;
std::atomic<bool> g_roleLogged{false};

void PrepareRebuild(void* line, const int* tag, RebuildCtx* c) {
    const void* target = Engine::LineTarget(line);
    const void* type = Engine::DesignType(target);
    if (!type || !tag || !RoleOnly(*tag, type)) return;
    c->active = true;
    c->target = target;
    c->role = Engine::DesignRole(target);
    c->tag = *tag;
    if (!g_roleLogged.exchange(true))
        Log("equipment_conversion: first rebuild in \"same role only\" mode (%s, thread %lu%s)", Engine::TypeName(type),
            GetCurrentThreadId(), IsMainThread() ? ", the main thread" : "");
}

void PublishSafe(const void* line, const RebuildCtx* c) {
    __try {
        PublishRefused(line, *c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault("recording the left-out designs");
    }
}

void __fastcall RebuildDetour(void* line, const int* tag) {
    RebuildCtx c = {};
    if (!g_off.load(std::memory_order_relaxed) && g_roleMode.load(std::memory_order_relaxed)) {
        __try {
            PrepareRebuild(line, tag, &c);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Fault("the line rebuild");
            c = {};
        }
    }
    RebuildCtx saved = t_rebuild;
    t_rebuild = c;
    g_rebuildOrig(line, tag);
    RebuildCtx done = t_rebuild;
    t_rebuild = saved;
    PublishSafe(line, &done);
}

std::string RoleFlagName(int tag, const void* type) {
    return std::string(kRolePrefix) + Engine::CountryTagName(tag) + "_" + Engine::TypeName(type);
}

// At the next safe point: flip the player's mode for this type (flag and table).
void ToggleOnMain(void* typePtr) {
    const void* type = typePtr;
    int tag = Engine::PlayerCountryTag();
    uint64_t key = tag > 0 ? ModeKey(tag, type) : 0;
    if (!key) return;
    bool on = !RoleOnly(tag, type);
    int token = Engine::Intern(RoleFlagName(tag, type).c_str());
    bool written = on ? Engine::SetGlobalFlag(token) : Engine::ClearGlobalFlag(token);
    AcquireSRWLockExclusive(&g_modeLock);
    g_roleOnly[key] = on;
    ReleaseSRWLockExclusive(&g_modeLock);
    Log("equipment_conversion: %s lines of %s: %s%s", Engine::TypeName(type), Engine::CountryTagName(tag).c_str(),
        on ? "same role only" : "all sources", written ? "" : " (flag NOT written)");
}

std::atomic<bool> g_mpLogged{false};
bool HandleShiftClick(void* item) {
    void* line = Engine::ItemLine(item);
    const void* target = line ? Engine::LineTarget(line) : nullptr;
    const void* type = Engine::DesignType(target);
    if (!type) return false;
    if (!g_singlePlayer.load()) {
        if (!g_mpLogged.exchange(true)) Log("equipment_conversion: Shift+click on convert: single player only");
        return true;  // swallowed: no vanilla toggle either
    }
    return RunOnMainThread(&ToggleOnMain, (void*)type);
}

void __fastcall ClickDetour(void* item) {
    // Without the mod's opt-in, Shift+click is vanilla's click.
    if (g_roleMode.load() && !g_off.load() && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
        __try {
            if (HandleShiftClick(item)) return;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Fault("the convert click");
        }
    }
    g_clickOrig(item);
}

const char* const kModeAllKey = "HOI4_LOADER_CONVERSION_MODE_ALL";
const char* const kModeRoleKey = "HOI4_LOADER_CONVERSION_MODE_ROLE";
const char kModeAllText[] = "Sources: designs of every role. Shift+click: the same role only.";
const char kModeRoleText[] = "Sources: designs of the same role only. Shift+click: every role.";

// "Not converted (another role):" and one greyed line per design the last rebuild left out. The
// colours come from the localisation (HOI4_LOADER_CONVERSION_LEFT_OUT / _ENTRY with $VARIANT$), so
// the game formats them; without those keys plain text.
const char kLeftOutKey[] = "HOI4_LOADER_CONVERSION_LEFT_OUT";
const char kLeftOutEntryKey[] = "HOI4_LOADER_CONVERSION_LEFT_OUT_ENTRY";
void AppendLeftOut(void* line, EngineString* out) {
    std::vector<const void*> designs = RefusedFor(line, Engine::LineTarget(line));
    if (designs.empty()) return;
    const EngineString* head = Engine::LocalizeKey(kLeftOutKey);
    std::string text = "\n";
    text += head ? head->c_str() : "Not converted (another role):";
    for (const void* d : designs) {
        std::string name = Engine::DesignName(d), entry;
        Engine::LocalizeArg arg{"VARIANT", name.c_str()};
        text += "\n";
        text += Engine::LocalizeWithArgs(kLeftOutEntryKey, &arg, 1, &entry) ? entry : "   - " + name;
    }
    Engine::AppendEngineString(out, text.data(), text.size());
}

void AppendMode(void* line, EngineString* out) {
    const void* type = Engine::DesignType(Engine::LineTarget(line));
    int tag = Engine::PlayerCountryTag();
    if (!type || tag <= 0 || !out->size) return;
    bool on = RoleOnly(tag, type);
    const EngineString* loc = Engine::HaveLocalize() ? Engine::LocalizeKey(on ? kModeRoleKey : kModeAllKey) : nullptr;
    const char* text = loc ? loc->c_str() : on ? kModeRoleText : kModeAllText;
    Engine::AppendEngineString(out, "\n", 1);
    Engine::AppendEngineString(out, text, strlen(text));
    if (on && Engine::HaveSkippedList()) AppendLeftOut(line, out);
}

void __fastcall TooltipDetour(void* line, void* arg, EngineString* out) {
    g_tooltipOrig(line, arg, out);
    if (g_off.load(std::memory_order_relaxed) || !g_roleMode.load(std::memory_order_relaxed) || !out || !line) return;
    __try {
        AppendMode(line, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault("the convert tooltip");
    }
}

// --- Rules (main thread) ---------------------------------------------------------------------
void ReadRules() {
    struct Ctx {
        int factor = -1, factorDate = INT_MIN;
        bool generations = false, roleMode = false, existing = false, naval = false;
        std::vector<std::string> roles;  // flag suffixes <TAG>_<type>
    } c;
    Engine::ForEachGlobalFlag(
        [](const Engine::FlagEntry& f, void* p) {
            Ctx& c = *(Ctx*)p;
            if (f.token == g_tokGenerations) c.generations = true;
            else if (f.token == g_tokRoleMode) c.roleMode = true;
            else if (f.token == g_tokExisting) c.existing = true;
            else if (f.token == g_tokNaval) c.naval = true;
            else {
                const char* name = Engine::TokenName(f.token);
                if (strncmp(name, kRolePrefix, sizeof kRolePrefix - 1) == 0) {
                    c.roles.push_back(name + sizeof kRolePrefix - 1);
                    return;
                }
                if (strncmp(name, kFactorPrefix, sizeof kFactorPrefix - 1) != 0) return;
                char* end = nullptr;
                long v = strtol(name + sizeof kFactorPrefix - 1, &end, 10);
                if (end && !*end && v >= 0 && v <= 100 && f.setDate >= c.factorDate) {
                    c.factor = (int)v;
                    c.factorDate = f.setDate;
                }
            }
        },
        &c);
    // Role modes: the flags are the truth (a toggle writes its flag).
    std::unordered_map<uint64_t, bool> roles;
    for (const std::string& s : c.roles) {
        if (s.size() < 5 || s[3] != '_') continue;
        int tag = Engine::TagFromText(s.substr(0, 3).c_str());
        AcquireSRWLockShared(&g_modeLock);
        auto it = g_typeByName.find(s.substr(4));
        const void* type = it == g_typeByName.end() ? nullptr : it->second;
        ReleaseSRWLockShared(&g_modeLock);
        uint64_t key = tag > 0 && type ? ModeKey(tag, type) : 0;
        if (key) roles[key] = true;
    }
    AcquireSRWLockExclusive(&g_modeLock);
    bool rolesChanged = roles != g_roleOnly;
    g_roleOnly.swap(roles);
    ReleaseSRWLockExclusive(&g_modeLock);
    if (rolesChanged) Log("equipment_conversion: %zu \"same role only\" mode(s) from flags", c.roles.size());
    int factor = c.factor < 0 ? kDefaultFactor : c.factor;
    bool changed = g_factor.exchange(factor) != factor;
    changed |= g_generations.exchange(c.generations) != c.generations;
    changed |= g_roleMode.exchange(c.roleMode) != c.roleMode;
    changed |= g_existingLoaderCost.exchange(c.existing) != c.existing;
    changed |= g_naval.exchange(c.naval) != c.naval;
    if (changed)
        Log("equipment_conversion: flags: earlier generations %s, factor %d%% per generation after the first%s, already "
            "allowed pairs: %s cost, naval: %s, role mode %s",
            c.generations ? "ON" : "off", factor, c.factor < 0 ? " (default)" : "", c.existing ? "the loader's" : "the game's",
            c.naval ? "allowed" : "not allowed", c.roleMode ? "ON" : "off");
}

// Self-check of the applied lists (defined below).
bool SafeSelfCheck(long* checked, const char** why);

// Main thread, at a safe point or on game entry: bring the lists in line with the flag.
void Sync() {
    bool want = g_generations.load() && !g_off.load();
    bool applied = g_anyAdded.load();
    if (want == applied) return;
    if (!SafeApply(want) || !want) return;
    long checked = 0;
    const char* why = "";
    if (!SafeSelfCheck(&checked, &why)) {
        g_off.store(true);
        SafeApply(false);
        FeatureDeactivate(kFeature, why);
        return;
    }
    Log("equipment_conversion: self-check passed (%ld extended lists)", checked);
}

// The flags are read every 30 frames in a game: a mod's on_startup sets them right after a new game
// starts, before the first day passes.
int g_frames = 0;
void Frame() {
    if (Engine::GameDateHours() <= 0 || ++g_frames % 30) return;
    ReadRules();
    Sync();
}

void InternTokens() {
    g_tokGenerations = Engine::Intern("hoi4_loader_conversion_generations");
    g_tokRoleMode = Engine::Intern("hoi4_loader_conversion_role_mode");
    g_tokExisting = Engine::Intern("hoi4_loader_conversion_existing_loader_cost");
    g_tokNaval = Engine::Intern("hoi4_loader_conversion_naval");
}

// Every loader entry must be an earlier generation of its type; otherwise the layout is not what
// the resolver saw, and the feature switches off.
bool SelfCheck(long* checked, const char** why) {
    AcquireSRWLockShared(&g_lock);
    bool ok = true;
    for (const auto& e : g_firstAdded) {
        int n = Engine::ConvertListCount(e.first);
        if (n < e.second || n > 10000) {
            ok = false;
            *why = "a list is shorter than when the loader extended it";
            break;
        }
        for (int i = e.second; i < n && ok; ++i)
            if (Engine::GenerationsBack(e.first, Engine::ConvertListAt(e.first, i)) <= 0) {
                ok = false;
                *why = "a loader entry is not an earlier generation";
            }
        ++*checked;
        if (!ok) break;
    }
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

bool SafeSelfCheck(long* checked, const char** why) {
    __try {
        return SelfCheck(checked, why);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "the check faulted";
        return false;
    }
}

bool g_statsLogged = false;
void OnEntered(bool) {
    const char* why = "";
    g_singlePlayer.store(SessionAllowsLocalWrites(&why));
    if (!g_statsLogged) {
        g_statsLogged = true;
        AcquireSRWLockShared(&g_lock);
        size_t types = g_planned.size();
        ReleaseSRWLockShared(&g_lock);
        Log("equipment_conversion: %ld lists built; %zu types have earlier generations to add (%ld pairs, %ld naval), "
            "used only while a mod sets hoi4_loader_conversion_generations",
            g_builds.load(), types, g_plannedPairs.load(), g_plannedNaval.load());
        if (!g_builds.load()) Log("equipment_conversion: WARNING: the list builder never ran (hooked too late?)");
    }
    ReadRules();
    Sync();
}

}  // namespace

bool EquipmentConversionInit() {
    if (!Engine::HaveConversion() || !Engine::HaveFlags() || !Engine::HaveTokens()) {
        Log("equipment_conversion: required engine functions missing (conversion %d, flags %d, tokens %d)", Engine::HaveConversion(),
            Engine::HaveFlags(), Engine::HaveTokens());
        return false;
    }
    if (!MainThreadQueueAvailable()) {
        Log("equipment_conversion: needs the main-thread frame hook (rules and fail-safe)");
        return false;
    }
    // All hooks or none: on a failure the ones queued so far are removed again (nothing is applied
    // before every feature has initialised).
    struct Hook {
        void* target;
        void* detour;
        void** orig;
        const char* name;
    };
    std::vector<Hook> hooks = {
        {Engine::pConvertListBuild, (void*)&BuildDetour, (void**)&g_buildOrig, "convert_list_build"},
        {Engine::pCanConvert, (void*)&CanConvertDetour, (void**)&g_canOrig, "can_convert"},
        {Engine::pConversionCost, (void*)&CostDetour, (void**)&g_costOrig, "conversion_cost"},
    };
    // The role mode is optional: without its signatures the rest works as before.
    if (Engine::HaveRoleMode()) {
        hooks.push_back({Engine::pLineRebuild, (void*)&RebuildDetour, (void**)&g_rebuildOrig, "line_rebuild_sources"});
        hooks.push_back({Engine::pConvertClick, (void*)&ClickDetour, (void**)&g_clickOrig, "convert_click"});
        hooks.push_back({Engine::pConvertTooltip, (void*)&TooltipDetour, (void**)&g_tooltipOrig, "convert_tooltip"});
    } else {
        Log("equipment_conversion: the role mode (Shift+click on convert) is unavailable");
    }
    for (size_t i = 0; i < hooks.size(); ++i) {
        if (HookQueue(hooks[i].target, hooks[i].detour, hooks[i].orig, hooks[i].name)) continue;
        for (size_t j = 0; j < i; ++j) MH_RemoveHook(hooks[j].target);
        g_off.store(true);
        return false;
    }
    OnLexerReady(&InternTokens);
    OnGameEntered(&OnEntered);
    OnMainThreadFrame(&Frame);
    return true;
}
