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

// CEffect Execute slot (NOTES.md B1 #3), verified at init against the CEffect base vtable.
const size_t kSlotExecute = 13;

// Guards everything except the display table: rescans, caches, flag edits. Recursive because the
// native effects edit flags and then rescan. Effects normally run on one game thread; this is
// insurance, not a hot path.
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
    if (changes) Engine::MarkMapNamesDirty();
    Log("region_names: rescan (%s): %d change(s)", why, changes);
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

// --- Change detection: set_global_flag / clr_global_flag ------------------------------------
using Execute_t = void(__fastcall*)(void*, void*, void*, void*);
Execute_t g_origSetFlagExec = nullptr, g_origClearFlagExec = nullptr;

void __fastcall SetFlagExecDetour(void* a, void* b, void* c, void* d) {
    g_origSetFlagExec(a, b, c, d);
    if (g_checkedFor > 0) Rescan("set_global_flag");
}

void __fastcall ClearFlagExecDetour(void* a, void* b, void* c, void* d) {
    g_origClearFlagExec(a, b, c, d);
    if (g_checkedFor > 0) Rescan("clr_global_flag");
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
    int cleared = ClearRegionFlags(id);
    int token = Engine::Intern(RegionFlagName(id, key).c_str());
    Engine::SetGlobalFlag(token);
    Log("set_strategic_region_name: region %d -> %s (%d old flag(s) cleared)", id, key, cleared);
    Rescan("set_strategic_region_name");
}

void DoReset(int id) {
    std::lock_guard<std::recursive_mutex> guard(g_lock);
    if (Engine::RegionById(id) == nullptr) {
        Log("reset_strategic_region_name: ERROR region %d does not exist (valid 1..%d)", id, Engine::RegionCount() - 1);
        return;
    }
    int cleared = ClearRegionFlags(id);
    Log("reset_strategic_region_name: region %d -> original name (%d flag(s) cleared)", id, cleared);
    Rescan("reset_strategic_region_name");
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
        if (g_checkedFor != -2) {
            g_checkedFor = -1;  // new game state: check again
            g_enabled.store(false);
        }
    }
    Rescan(loaded ? "save loaded" : "new game");
}

// The slots we rely on must be overridden (differ from CEffect's own implementation).
bool SlotsLookRight(void** vt, size_t slots, std::initializer_list<size_t> want, void** base, size_t baseSlots,
                    const char* what) {
    for (size_t s : want) {
        if (s >= slots || s >= baseSlots || vt[s] == base[s]) {
            Log("region_names: %s slot %zu is not an override; engine ABI changed", what, s);
            return false;
        }
    }
    return true;
}

}  // namespace

bool RegionNamesInit() {
    if (!Engine::HaveTokens() || !Engine::HaveFlags() || !Engine::HaveRegions() || !Engine::HaveLocalize()) {
        Log("region_names: required engine functions missing (tokens %d, flags %d, regions %d, localize %d)",
            Engine::HaveTokens(), Engine::HaveFlags(), Engine::HaveRegions(), Engine::HaveLocalize());
        return false;
    }
    size_t baseSlots = 0, setSlots = 0, clrSlots = 0;
    void** base = Engine::FindVtableByRtti(".?AVCEffect@@", &baseSlots);
    void** setVt = Engine::FindVtableByRtti(".?AVCSetGlobalFlagEffect@@", &setSlots);
    void** clrVt = Engine::FindVtableByRtti(".?AVCClearGlobalFlagEffect@@", &clrSlots);
    if (!base || !setVt || !clrVt || !SlotsLookRight(setVt, setSlots, {kSlotExecute}, base, baseSlots, "set_global_flag") ||
        !SlotsLookRight(clrVt, clrSlots, {kSlotExecute}, base, baseSlots, "clr_global_flag"))
        return false;
    if (!HookQueue(Engine::pRegionGetNameKey, (void*)&GetNameKeyDetour, (void**)&g_origGetNameKey, "region_get_name_key") ||
        !HookQueue(setVt[kSlotExecute], (void*)&SetFlagExecDetour, (void**)&g_origSetFlagExec, "set_global_flag_execute") ||
        !HookQueue(clrVt[kSlotExecute], (void*)&ClearFlagExecDetour, (void**)&g_origClearFlagExec, "clr_global_flag_execute"))
        return false;
    OnGameEntered(&OnEntered);

    // Native commands. If one is refused the feature stays active (the flag protocol works) and
    // the guard trigger hoi4_loader_feature_region_names says no.
    const char* kFeature = "region_names";
    register_native_effect({"set_strategic_region_name",
                            "Rename a strategic region to a localisation key (hoi4_loader). Stored as global flag "
                            "hoi4_srn_<region>_<key>: survives save and load; without the loader every region shows "
                            "its original name. Invalid regions or missing keys change nothing and are logged.",
                            Form::Block,
                            {{"region", ParamType::Int, true}, {"name", ParamType::Key, true}},
                            ScopeMask::Any, &ExecSet, nullptr},
                           kFeature);
    register_native_effect({"reset_strategic_region_name",
                            "Restore a strategic region's original name (hoi4_loader): clears its hoi4_srn flags.",
                            Form::Block, {{"region", ParamType::Int, true}}, ScopeMask::Any, &ExecReset, nullptr},
                           kFeature);
    register_native_trigger({"strategic_region_has_custom_name",
                             "True if the strategic region currently shows a name provided by hoi4_loader. Guard it "
                             "with hoi4_loader_feature_region_names = yes in the same AND block.",
                             Form::Block, {{"region", ParamType::Int, true}}, ScopeMask::Any, &EvalHasCustomName,
                             &TooltipHasCustomName},
                            kFeature);
    return true;
}
