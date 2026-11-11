// equipment_conversion (engine tier, needs lifecycle; NOTES.md Part J): equipment of any earlier
// generation converts into the generation a production line makes ("Convert from stockpile"), for
// every equipment type and mod, without can_convert_from entries.
//
// - At load, after the engine builds a type's convert-from list, the loader appends every earlier
//   generation of that type (its `parent =` chain within the archetype) that the list lacks. All of
//   the engine's conversion tests, the production line's sources and naval refits read that list.
// - Cost: the engine's own conversion cost, times factor^(g-1) for a source g generations back
//   (one generation back: unchanged). Only for the pairs the loader added, unless a rule says the
//   pairs the game or a mod already allows get it too.
// - Naval types: off unless a rule allows them (the loader's naval pairs are refused in
//   CanConvert, so neither stockpile conversion nor refits use them).
// Rules come from global flags a mod sets from game rules at on_startup (hoi4_loader_conversion_rules).
#include <windows.h>

#include <atomic>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"

namespace {

const char* const kFeature = "equipment_conversion";

// --- Rules -----------------------------------------------------------------------------------
// hoi4_loader_conversion_factor_<percent> (0..100; the most recently set one counts; default 75),
// hoi4_loader_conversion_existing_loader_cost, hoi4_loader_conversion_naval, hoi4_loader_conversion_off.
const int kDefaultFactor = 75;
const char kFactorPrefix[] = "hoi4_loader_conversion_factor_";
std::atomic<int> g_factor{kDefaultFactor};
std::atomic<bool> g_existingLoaderCost{false}, g_naval{false}, g_ruleOff{false};
int g_tokExisting = 0, g_tokNaval = 0, g_tokOff = 0;

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
// type -> list index of its first loader entry (entries from there on are the loader's).
SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<const void*, int> g_firstAdded;
std::atomic<long> g_typesExtended{0}, g_pairsAdded{0}, g_navalPairs{0}, g_builds{0};
std::atomic<bool> g_anyAdded{false};

void Record(const void* type, int first) {
    AcquireSRWLockExclusive(&g_lock);
    g_firstAdded[type] = first;
    ReleaseSRWLockExclusive(&g_lock);
}

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

void Extend(void* type) {
    if (Engine::TypeIsArchetype(type)) return;
    const void* arch = Engine::TypeArchetype(type);
    const void* add[64];
    int n = 0;
    for (const void* p = Engine::TypeParent(type); p && n < 64; p = Engine::TypeParent(p)) {
        if (Engine::TypeArchetype(p) != arch) break;  // a generation chain stays in its archetype
        if (!InList(type, p)) add[n++] = p;
    }
    if (!n) return;
    int first = Engine::ConvertListCount(type);
    if (!Engine::ConvertListAppend(type, add, n)) {
        Log("equipment_conversion: could not extend the list of %s", Engine::TypeName(type));
        return;
    }
    Record(type, first);
    g_typesExtended.fetch_add(1);
    g_pairsAdded.fetch_add(n);
    if (Engine::TypeIsNaval(type)) g_navalPairs.fetch_add(n);
    g_anyAdded.store(true);
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
        Extend(type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault("extending a convert-from list");
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
    if (g_off.load(std::memory_order_relaxed) || g_ruleOff.load(std::memory_order_relaxed)) return true;
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
    bool active = !g_off.load(std::memory_order_relaxed) && !g_ruleOff.load(std::memory_order_relaxed) &&
                  g_anyAdded.load(std::memory_order_relaxed);
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
    if (!g_off.load(std::memory_order_relaxed)) {
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
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) && !g_off.load()) {
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
    if (g_off.load(std::memory_order_relaxed) || !out || !line) return;
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
        bool existing = false, naval = false, off = false;
        std::vector<std::string> roles;  // flag suffixes <TAG>_<type>
    } c;
    Engine::ForEachGlobalFlag(
        [](const Engine::FlagEntry& f, void* p) {
            Ctx& c = *(Ctx*)p;
            if (f.token == g_tokExisting) c.existing = true;
            else if (f.token == g_tokNaval) c.naval = true;
            else if (f.token == g_tokOff) c.off = true;
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
    changed |= g_existingLoaderCost.exchange(c.existing) != c.existing;
    changed |= g_naval.exchange(c.naval) != c.naval;
    changed |= g_ruleOff.exchange(c.off) != c.off;
    if (changed)
        Log("equipment_conversion: rules: %s, factor %d%% per generation after the first%s, already allowed pairs: %s cost, "
            "naval: %s",
            c.off ? "OFF" : "on", factor, c.factor < 0 ? " (default)" : "", c.existing ? "the loader's" : "the game's",
            c.naval ? "allowed" : "not allowed");
}

int g_lastDay = INT_MIN;
void Frame() {
    int hours = Engine::GameDateHours();
    if (hours <= 0) return;
    int day = hours / 24;
    if (day == g_lastDay) return;
    g_lastDay = day;
    ReadRules();
}

void InternTokens() {
    g_tokExisting = Engine::Intern("hoi4_loader_conversion_existing_loader_cost");
    g_tokNaval = Engine::Intern("hoi4_loader_conversion_naval");
    g_tokOff = Engine::Intern("hoi4_loader_conversion_off");
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
    g_lastDay = INT_MIN;
    const char* why = "";
    g_singlePlayer.store(SessionAllowsLocalWrites(&why));
    ReadRules();
    if (g_off.load()) return;
    if (!g_statsLogged) {
        g_statsLogged = true;
        Log("equipment_conversion: %ld lists built, %ld types extended, %ld pairs added (%ld naval)", g_builds.load(),
            g_typesExtended.load(), g_pairsAdded.load(), g_navalPairs.load());
        if (!g_builds.load()) Log("equipment_conversion: WARNING: the list builder never ran (hooked too late?)");
    }
    long checked = 0;
    if (!SafeSelfCheck(&checked, &why)) {
        g_off.store(true);
        FeatureDeactivate(kFeature, why);
        return;
    }
    Log("equipment_conversion: self-check passed (%ld extended lists)", checked);
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
    if (!HookQueue(Engine::pConvertListBuild, (void*)&BuildDetour, (void**)&g_buildOrig, "convert_list_build") ||
        !HookQueue(Engine::pCanConvert, (void*)&CanConvertDetour, (void**)&g_canOrig, "can_convert") ||
        !HookQueue(Engine::pConversionCost, (void*)&CostDetour, (void**)&g_costOrig, "conversion_cost")) {
        g_off.store(true);  // hooks queued before the failure stay vanilla
        return false;
    }
    // The role mode is optional: without its signatures the rest works as before.
    if (Engine::HaveRoleMode()) {
        bool ok = HookQueue(Engine::pLineRebuild, (void*)&RebuildDetour, (void**)&g_rebuildOrig, "line_rebuild_sources") &&
                  HookQueue(Engine::pConvertClick, (void*)&ClickDetour, (void**)&g_clickOrig, "convert_click") &&
                  HookQueue(Engine::pConvertTooltip, (void*)&TooltipDetour, (void**)&g_tooltipOrig, "convert_tooltip");
        if (!ok) {
            g_off.store(true);
            return false;
        }
    } else {
        Log("equipment_conversion: the role mode (Shift+click on convert) is unavailable");
    }
    OnLexerReady(&InternTokens);
    OnGameEntered(&OnEntered);
    OnMainThreadFrame(&Frame);
    return true;
}
