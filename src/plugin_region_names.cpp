// region_names (engine tier): runtime names for strategic regions.
//
// Storage is a global-flag protocol, so names persist with the save and are inert without the
// loader (see NOTES.md, Part B2):
//     hoi4_srn_<region id>_<localisation key>     e.g. hoi4_srn_42_MY_LOC_KEY
// The most recently set flag for a region wins; no flag means the original name.
//
// Script can use the flags directly (works without the loader, no parse errors), or the native
// commands this plugin registers through the native script API, which only manage those flags:
//     set_strategic_region_name = { region = 42 name = MY_LOC_KEY }
//     reset_strategic_region_name = { region = 42 }
//     strategic_region_has_custom_name = { region = 42 }   (trigger)
//
// Display: CStrategicRegion::GetNameKey() is hooked and answers from a lock-free table. The
// engine's own name override (region+0x38, saved natively) is never written, so saves stay
// vanilla and show original names without the loader.
//
// Threading (NOTES.md, "Threading model"): the flag hooks may run on any thread, so they only
// mark the table dirty. The rescan (flags -> table) runs on the main thread at the next frame
// start, or directly on game entry. Display-only state: one frame of delay is fine.
#include <atomic>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "native_script.h"

namespace {

const char kPrefix[] = "hoi4_srn_";
const size_t kPrefixLen = sizeof(kPrefix) - 1;
const size_t kMaxKeyLen = 100;   // flag names up to ~114 chars; see NOTES.md (flag names)
const int kMaxRegions = 4096;

// Guards everything except the display table and the dirty bits: rescans, caches, flag edits.
// Rescans and effects all run on the main thread; this is insurance, not a hot path.
std::recursive_mutex g_lock;

// --- Display table (read from any thread) ------------------------------------------------------
std::atomic<bool> g_enabled{false};                       // self-check passed for this game
std::atomic<const EngineString*> g_names[kMaxRegions];    // null = original name
std::string g_current[kMaxRegions];                       // key behind g_names (game thread)

using GetNameKey_t = const EngineString*(__fastcall*)(void* region);
GetNameKey_t g_origGetNameKey = nullptr;

const EngineString* __fastcall GetNameKeyDetour(void* region) {
    if (g_enabled.load(std::memory_order_acquire)) {
        int id = Engine::RegionTemplateId(region);
        if (id > 0 && id < kMaxRegions) {
            const EngineString* s = g_names[id].load(std::memory_order_acquire);
            if (s) return s;
        }
    }
    return g_origGetNameKey(region);
}

// --- Flag parsing ---------------------------------------------------------------------------
bool ValidKey(const char* k, size_t n) {
    if (n == 0 || n > kMaxKeyLen) return false;
    for (size_t i = 0; i < n; ++i)
        if (!(isalnum((unsigned char)k[i]) || k[i] == '_' || k[i] == '.' || k[i] == '-')) return false;
    return true;
}

struct Parsed {
    bool ours = false;   // starts with the prefix
    bool valid = false;  // well-formed: digits, '_', valid key
    int region = 0;
    std::string key;
};
std::unordered_map<int, Parsed> g_tokenCache;  // game thread only

const Parsed& ParseToken(int token) {
    auto it = g_tokenCache.find(token);
    if (it != g_tokenCache.end()) return it->second;
    Parsed p;
    const char* name = Engine::TokenName(token);
    if (strncmp(name, kPrefix, kPrefixLen) == 0) {
        p.ours = true;
        const char* s = name + kPrefixLen;
        int id = 0;
        size_t digits = 0;
        while (isdigit((unsigned char)s[digits]) && digits < 6) id = id * 10 + (s[digits++] - '0');
        const char* key = s + digits + 1;
        if (digits > 0 && s[digits] == '_' && ValidKey(key, strlen(key))) {
            p.valid = true;
            p.region = id;
            p.key = key;
        } else {
            Log("region_names: ignoring malformed flag '%s' (expected %s<region id>_<loc key>, key chars A-Z a-z 0-9 _ . -, "
                "max %zu)", name, kPrefix, kMaxKeyLen);
        }
    }
    return g_tokenCache.emplace(token, std::move(p)).first->second;
}

// --- Localisation (cached) ------------------------------------------------------------------
// GetNameKey's callers display its result as text (in-game test 2026-10-13: returning a key
// showed the raw key), so the table holds localised text. Strings are never freed: readers on
// other threads may still hold them. One per distinct key.
std::unordered_map<std::string, const EngineString*> g_locCache;

const EngineString* DisplayName(const std::string& key) {
    auto it = g_locCache.find(key);
    if (it != g_locCache.end()) return it->second;
    const EngineString* text = Engine::LocalizeKey(key.c_str());
    if (text)
        Log("region_names: localisation key '%s' found: \"%s\"", key.c_str(), text->c_str());
    else
        Log("region_names: localisation key '%s' NOT found (region keeps its name)", key.c_str());
    return g_locCache.emplace(key, text).first->second;
}

bool KeyExists(const std::string& key) { return DisplayName(key) != nullptr; }

// --- Rescan: flags -> display table ---------------------------------------------------------
struct Candidate {
    int date = 0;
    const Parsed* p = nullptr;
    int count = 0;
};

bool SelfCheck();

// Visibility for the filtered change detection, logged and reset once per game entered.
std::atomic<long> g_ignoredFlagChanges{0};  // global flag set/clear without the hoi4_srn_ prefix
std::atomic<long> g_offMainFlagChanges{0};  // any flag set/clear seen off the main thread
long g_quietRescans = 0;                    // rescans that changed nothing (main thread)

void Rescan(const char* why) {
    std::lock_guard<std::recursive_mutex> guard(g_lock);
    if (!SelfCheck()) return;
    int regionCount = Engine::RegionCount();
    std::vector<Candidate> best((size_t)regionCount);
    struct Ctx {
        std::vector<Candidate>* best;
        int regionCount;
    } ctx{&best, regionCount};
    Engine::ForEachGlobalFlag(
        [](const Engine::FlagEntry& f, void* c) {
            auto* x = (Ctx*)c;
            const Parsed& p = ParseToken(f.token);
            if (!p.ours || !p.valid) return;
            if (p.region < 1 || p.region >= x->regionCount) {
                static int logged = 0;
                if (logged++ < 20) Log("region_names: flag for unknown region %d ignored (valid 1..%d)", p.region, x->regionCount - 1);
                return;
            }
            Candidate& b = (*x->best)[(size_t)p.region];
            ++b.count;
            // Later set date wins; on a tie the larger key wins (deterministic on every client).
            if (!b.p || f.setDate > b.date || (f.setDate == b.date && p.key > b.p->key)) {
                b.date = f.setDate;
                b.p = &p;
            }
        },
        &ctx);

    int changes = 0;
    for (int id = 1; id < regionCount && id < kMaxRegions; ++id) {
        const Candidate& b = best[(size_t)id];
        std::string want = b.p ? b.p->key : std::string();
        if (b.count > 1) Log("region_names: region %d has %d hoi4_srn flags; using '%s'", id, b.count, want.c_str());
        if (!want.empty() && !KeyExists(want)) want.clear();
        if (want == g_current[id]) continue;
        g_current[id] = want;
        g_names[id].store(want.empty() ? nullptr : DisplayName(want), std::memory_order_release);
        Log("region_names: region %d -> %s", id, want.empty() ? "original name" : want.c_str());
        ++changes;
    }
    if (changes) {
        Engine::MarkMapNamesDirty();
        Log("region_names: rescan (%s): %d change(s)", why, changes);
    } else {
        ++g_quietRescans;
    }
}

// --- Self-check: region ids from the template must match the region array -------------------
int g_checkedFor = -1;  // region count the check ran for; -1 = never, -2 = failed

bool SelfCheck() {
    int count = Engine::RegionCount();
    if (g_checkedFor == -2) return false;
    if (count > 0 && g_checkedFor == count) return true;
    if (count <= 1) return false;  // no game yet
    if (count > kMaxRegions) {
        Log("region_names: %d regions exceed the supported %d", count, kMaxRegions);
        g_checkedFor = -2;
    } else {
        for (int id = 1; id < count; ++id) {
            int tid = Engine::RegionTemplateId(Engine::RegionById(id));
            if (tid != id) {
                Log("region_names: self-check failed: region slot %d has template id %d", id, tid);
                g_checkedFor = -2;
                break;
            }
        }
    }
    if (g_checkedFor == -2) {
        g_enabled.store(false);
        FeatureDeactivate("region_names", "engine data layout does not match");
        return false;
    }
    g_checkedFor = count;
    g_enabled.store(true, std::memory_order_release);
    Log("region_names: self-check passed (%d regions)", count - 1);
    // Diagnostic: what the engine itself returns (key or display text) for one region.
    if (void* r = Engine::RegionById(count > 42 ? 42 : 1)) {
        const EngineString* s = g_origGetNameKey(r);
        Log("region_names: engine name of region %d: \"%s\"", Engine::RegionTemplateId(r), s ? s->c_str() : "(null)");
    }
    return true;
}

// --- Change detection: the engine's flag set / clear functions -------------------------------
// Every set_global_flag / clr_global_flag (and country/state flag) ends in these two with the
// resolved token. They may run on any thread, so they only mark the table dirty: a pointer
// compare against the global container, a prefix compare on the engine's own token string, an
// atomic OR. No allocation, no reading of the container. Large mods set flags constantly.
using FlagSet_t = void(__fastcall*)(void* flags, int token, short value, short days);
using FlagClear_t = void(__fastcall*)(void* flags, int token);
FlagSet_t g_origFlagSet = nullptr;
FlagClear_t g_origFlagClear = nullptr;

// Why a rescan is pending; consumed by the next rescan.
enum : unsigned { kDirtyFlagSet = 1, kDirtyFlagClear = 2, kDirtySetEffect = 4, kDirtyResetEffect = 8 };
std::atomic<unsigned> g_dirty{0};
std::atomic<void*> g_globalFlags{nullptr};  // global flag container of the current game (set on game entry)
thread_local int g_ownEdit = 0;             // DoSet/DoReset edit flags themselves and mark their own reason

void OnFlagChanged(void* flags, int token, unsigned reason) {
    if (!IsMainThread() && MainThreadQueueAvailable()) g_offMainFlagChanges.fetch_add(1, std::memory_order_relaxed);
    if (g_ownEdit || flags == nullptr || flags != g_globalFlags.load(std::memory_order_relaxed)) return;
    if (strncmp(Engine::TokenName(token), kPrefix, kPrefixLen) != 0) {
        g_ignoredFlagChanges.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_dirty.fetch_or(reason, std::memory_order_release);
}

void __fastcall FlagSetDetour(void* flags, int token, short value, short days) {
    g_origFlagSet(flags, token, value, days);
    OnFlagChanged(flags, token, kDirtyFlagSet);
}

void __fastcall FlagClearDetour(void* flags, int token) {
    g_origFlagClear(flags, token);
    OnFlagChanged(flags, token, kDirtyFlagClear);
}

// Main thread, every frame (lifecycle safe point): one atomic exchange unless something changed.
void OnFrame() {
    unsigned why = g_dirty.exchange(0, std::memory_order_acquire);
    if (!why) return;
    std::string reason;
    auto add = [&](unsigned bit, const char* text) {
        if (!(why & bit)) return;
        if (!reason.empty()) reason += ", ";
        reason += text;
    };
    add(kDirtyFlagSet, "hoi4_srn flag set");
    add(kDirtyFlagClear, "hoi4_srn flag cleared");
    add(kDirtySetEffect, "set_strategic_region_name");
    add(kDirtyResetEffect, "reset_strategic_region_name");
    Rescan(reason.c_str());
}

// Diagnostic, once per game entered: every hoi4_srn flag with the fields that decide its life.
// days > 0 means the engine deletes the flag when its daily countdown reaches 0.
void LogRegionFlags() {
    int total = 0, ours = 0;
    struct Ctx {
        int* total;
        int* ours;
    } ctx{&total, &ours};
    Engine::ForEachGlobalFlag(
        [](const Engine::FlagEntry& f, void* c) {
            auto* x = (Ctx*)c;
            ++*x->total;
            const char* name = Engine::TokenName(f.token);
            if (strncmp(name, kPrefix, kPrefixLen) != 0) return;
            ++*x->ours;
            Log("region_names: flag %s: value %d, days %d%s, set date %d", name, f.value, f.days,
                f.days > 0 ? " (EXPIRES: the engine counts it down daily)" : "", f.setDate);
        },
        &ctx);
    Log("region_names: %d hoi4_srn flag(s) among %d global flags", ours, total);
}

// --- Native commands (registered through the native script API) ----------------------------

std::string RegionFlagName(int id, const std::string& key) { return kPrefix + std::to_string(id) + "_" + key; }

// Clears every hoi4_srn flag of one region. Returns how many were cleared.
int ClearRegionFlags(int id) {
    std::vector<int> tokens;
    struct Ctx {
        int id;
        std::vector<int>* tokens;
    } ctx{id, &tokens};
    Engine::ForEachGlobalFlag(
        [](const Engine::FlagEntry& f, void* c) {
            auto* x = (Ctx*)c;
            const Parsed& p = ParseToken(f.token);
            if (p.ours && p.valid && p.region == x->id) x->tokens->push_back(f.token);
        },
        &ctx);
    for (int t : tokens) Engine::ClearGlobalFlag(t);
    return (int)tokens.size();
}

void DoSet(int id, const char* key) {
    std::lock_guard<std::recursive_mutex> guard(g_lock);
    size_t n = strlen(key);
    if (Engine::RegionById(id) == nullptr) {
        Log("set_strategic_region_name: ERROR region %d does not exist (valid 1..%d); nothing changed", id,
            Engine::RegionCount() - 1);
        return;
    }
    if (!ValidKey(key, n)) {
        Log("set_strategic_region_name: ERROR name '%s' is not a usable key (A-Z a-z 0-9 _ . -, 1..%zu chars); nothing changed",
            key, kMaxKeyLen);
        return;
    }
    if (!KeyExists(key)) {
        Log("set_strategic_region_name: ERROR localisation key '%s' is missing; region %d keeps its name", key, id);
        return;
    }
    ++g_ownEdit;
    int cleared = ClearRegionFlags(id);
    int token = Engine::Intern(RegionFlagName(id, key).c_str());
    Engine::SetGlobalFlag(token);
    --g_ownEdit;
    Log("set_strategic_region_name: region %d -> %s (%d old flag(s) cleared)", id, key, cleared);
    g_dirty.fetch_or(kDirtySetEffect, std::memory_order_release);  // shown from the next frame
}

void DoReset(int id) {
    std::lock_guard<std::recursive_mutex> guard(g_lock);
    if (Engine::RegionById(id) == nullptr) {
        Log("reset_strategic_region_name: ERROR region %d does not exist (valid 1..%d)", id, Engine::RegionCount() - 1);
        return;
    }
    ++g_ownEdit;
    int cleared = ClearRegionFlags(id);
    --g_ownEdit;
    Log("reset_strategic_region_name: region %d -> original name (%d flag(s) cleared)", id, cleared);
    g_dirty.fetch_or(kDirtyResetEffect, std::memory_order_release);  // shown from the next frame
}

void ExecSet(const Args& a, const Scope&) { DoSet(a.i(0), a.key(1)); }
void ExecReset(const Args& a, const Scope&) { DoReset(a.i(0)); }

// Hot path (AI and UI, possibly several threads): atomics only, no locks, no allocation.
bool EvalHasCustomName(const Args& a, const Scope&) {
    if (!g_enabled.load(std::memory_order_acquire)) return false;
    int id = a.i(0);
    return id > 0 && id < kMaxRegions && g_names[id].load(std::memory_order_acquire) != nullptr;
}

// Requirement line, e.g. "Strategic region Loader Test Sea A (42) has a custom name".
void TooltipHasCustomName(const Args& a, const Scope&, bool negated, TooltipText& out) {
    int id = a.i(0);
    void* region = Engine::RegionById(id);
    const EngineString* name = region ? GetNameKeyDetour(region) : nullptr;  // what the UI shows now
    out.add("Strategic region ");
    if (name)
        out.add(name->c_str()).add(" (").add(id).add(")");
    else
        out.add(id);
    out.add(negated ? " does NOT have a custom name" : " has a custom name");
}


void OnEntered(bool loaded) {
    {
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        long ignored = g_ignoredFlagChanges.exchange(0);
        long offMain = g_offMainFlagChanges.exchange(0);
        Log("region_names: since the last game entry: %ld global flag change(s) ignored (not hoi4_srn_), "
            "%ld rescan(s) without change, %ld flag change(s) off the main thread", ignored, g_quietRescans, offMain);
        g_quietRescans = 0;
        if (g_checkedFor != -2) {
            g_checkedFor = -1;  // new game state: check again
            g_enabled.store(false);
        }
        // Main thread (game entered): the container of this game; the rescan below covers anything pending.
        g_globalFlags.store(Engine::GlobalFlags(), std::memory_order_relaxed);
        g_dirty.store(0, std::memory_order_relaxed);
    }
    Rescan(loaded ? "save loaded" : "new game");
    if (g_checkedFor > 0) LogRegionFlags();
}

}  // namespace

bool RegionNamesInit() {
    if (!Engine::HaveTokens() || !Engine::HaveFlags() || !Engine::HaveRegions() || !Engine::HaveLocalize()) {
        Log("region_names: required engine functions missing (tokens %d, flags %d, regions %d, localize %d)",
            Engine::HaveTokens(), Engine::HaveFlags(), Engine::HaveRegions(), Engine::HaveLocalize());
        return false;
    }
    if (!MainThreadQueueAvailable()) {
        Log("region_names: main-thread queue unavailable (flag changes could not be applied safely)");
        return false;
    }
    if (!HookQueue(Engine::pRegionGetNameKey, (void*)&GetNameKeyDetour, (void**)&g_origGetNameKey, "region_get_name_key") ||
        !HookQueue(Engine::pFlagSet, (void*)&FlagSetDetour, (void**)&g_origFlagSet, "flag_set") ||
        !HookQueue(Engine::pFlagClear, (void*)&FlagClearDetour, (void**)&g_origFlagClear, "flag_clear"))
        return false;
    OnGameEntered(&OnEntered);
    OnMainThreadFrame(&OnFrame);

    // Native commands. If one is refused the feature stays active (the flag protocol works) and
    // the guard trigger hoi4_loader_feature_region_names says no.
    const char* kFeature = "region_names";
    register_native_effect({"set_strategic_region_name",
                            "Rename a strategic region to a localisation key (hoi4_loader). Stored as global flag "
                            "hoi4_srn_<region>_<key>: survives save and load; without the loader every region shows "
                            "its original name. Invalid regions or missing keys change nothing and are logged. "
                            "Inside a strategic region scope `region` may be left out.",
                            Form::Block,
                            {{"region", ParamType::Int, false, ScopeDefault::StrategicRegion}, {"name", ParamType::Key, true}},
                            ScopeMask::Any, &ExecSet, nullptr},
                           kFeature);
    register_native_effect({"reset_strategic_region_name",
                            "Restore a strategic region's original name (hoi4_loader): clears its hoi4_srn flags. "
                            "Inside a strategic region scope `region` may be left out.",
                            Form::Block, {{"region", ParamType::Int, false, ScopeDefault::StrategicRegion}}, ScopeMask::Any,
                            &ExecReset, nullptr},
                           kFeature);
    register_native_trigger({"strategic_region_has_custom_name",
                             "True if the strategic region currently shows a name provided by hoi4_loader. A rename "
                             "shows from the next frame, so in the same effect block that renamed it this is still "
                             "the old state. Inside a strategic region scope `region` may be left out; without "
                             "`region` and outside such a scope it is false (TRUE under NOT). Guard it with "
                             "hoi4_loader_feature_region_names = yes in the same AND block.",
                             Form::Block, {{"region", ParamType::Int, false, ScopeDefault::StrategicRegion}}, ScopeMask::Any,
                             &EvalHasCustomName, &TooltipHasCustomName},
                            kFeature);
    return true;
}
