// region_names (engine tier): runtime names for strategic regions.
//
// Storage is a global-flag protocol, so names persist with the save and are inert without the
// loader (see NOTES.md, Part B2):
//     hoi4_srn_<region id>_<localisation key>     e.g. hoi4_srn_42_MY_LOC_KEY
// The most recently set flag for a region wins; no flag means the original name.
//
// Script can use the flags directly (works without the loader, no parse errors), or the native
// effects this plugin registers, which only manage those flags:
//     set_strategic_region_name = { region = 42 name = MY_LOC_KEY }
//     reset_strategic_region_name = { region = 42 }
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

namespace {

const char kPrefix[] = "hoi4_srn_";
const size_t kPrefixLen = sizeof(kPrefix) - 1;
const size_t kMaxKeyLen = 100;   // flag names up to ~114 chars; see NOTES.md (flag names)
const int kMaxRegions = 4096;

// CEffect virtual slots (NOTES.md B1 #3). Verified at init against the CEffect base vtable:
// each must be overridden by the classes that use it.
const size_t kSlotReadItem = 4, kSlotTooltip = 7, kSlotExecute = 13;
// CRenameProvinceEffect fields filled by its block reader: `id` (int), `name` (std::string).
const size_t kFieldId = 0x58, kFieldName = 0x60;

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

// --- Native effects -------------------------------------------------------------------------
int g_tokRegion = 0, g_tokId = 0;
void** g_setVtable = nullptr;    // clone of CRenameProvinceEffect's vtable, [-1] = its RTTI locator
void** g_resetVtable = nullptr;

using ReadItem_t = void(__fastcall*)(void* self, void* parser, int token);
using Tooltip_t = EngineString*(__fastcall*)(void* self, EngineString* out, void* ctx);
using Create_t = void*(__fastcall*)(void* entry);

void __fastcall ReadItem(void* self, void* parser, int token) {
    // `region = N` is read exactly like set_province_name's `id = N`.
    auto orig = (ReadItem_t)Engine::pRenameProvinceEffectVtable[kSlotReadItem];
    orig(self, parser, token == g_tokRegion ? g_tokId : token);
}

EngineString* __fastcall Tooltip(void*, EngineString* out, void*) {
    // No tooltip line (see README): empty std::string.
    memset(out, 0, sizeof *out);
    out->cap = 15;
    return out;
}

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

// SEH wrappers: the effect object layout is not signature-verified, so a bad read must not
// take the game down. No C++ objects with destructors in these functions.
void SafeSet(void* self) {
    __try {
        int id = *(int*)((uint8_t*)self + kFieldId);
        const EngineString* key = (const EngineString*)((uint8_t*)self + kFieldName);
        if (key->size > 4096) {
            Log("set_strategic_region_name: ERROR unreadable name; nothing changed");
            return;
        }
        DoSet(id, key->c_str());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("set_strategic_region_name: ERROR exception 0x%08lX; nothing changed", GetExceptionCode());
    }
}

void SafeReset(void* self) {
    __try {
        DoReset(*(int*)((uint8_t*)self + kFieldId));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("reset_strategic_region_name: ERROR exception 0x%08lX; nothing changed", GetExceptionCode());
    }
}

void __fastcall SetExecute(void* self, void*, void*, void*) { SafeSet(self); }
void __fastcall ResetExecute(void* self, void*, void*, void*) { SafeReset(self); }

void** CloneVtable(void* readItem, void* tooltip, void* execute) {
    size_t n = Engine::renameProvinceEffectVtableSlots;
    auto** block = new void*[n + 1];
    block[0] = Engine::pRenameProvinceEffectVtable[-1];  // RTTI complete object locator
    memcpy(block + 1, Engine::pRenameProvinceEffectVtable, n * sizeof(void*));
    void** vt = block + 1;
    vt[kSlotReadItem] = readItem;
    vt[kSlotTooltip] = tooltip;
    vt[kSlotExecute] = execute;
    return vt;
}

void* __fastcall EntryDestroy(void* self, unsigned) { return self; }  // static objects

void* CreateWith(void* entry, void** vtable) {
    auto create = (Create_t)Engine::pRenameProvinceEntryVtable[1];
    void* obj = create(entry);
    if (obj) *(void***)obj = vtable;
    return obj;
}
void* __fastcall EntryCreateSet(void* entry) { return CreateWith(entry, g_setVtable); }
void* __fastcall EntryCreateReset(void* entry) { return CreateWith(entry, g_resetVtable); }

void* g_setEntryVtable[2] = {(void*)&EntryDestroy, (void*)&EntryCreateSet};
void* g_resetEntryVtable[2] = {(void*)&EntryDestroy, (void*)&EntryCreateReset};
Engine::EffectEntry g_setEntry = {g_setEntryVtable,
                                  "set_strategic_region_name = { region = <strategic region id> name = <localisation key> }"
                                  " - Rename a strategic region (hoi4_loader)"};
Engine::EffectEntry g_resetEntry = {g_resetEntryVtable,
                                    "reset_strategic_region_name = { region = <strategic region id> }"
                                    " - Restore a strategic region's original name (hoi4_loader)"};
bool g_nativeEffects = false;

void RegisterNativeEffects() {
    if (!g_nativeEffects) return;
    g_tokRegion = Engine::Intern("region");
    g_tokId = Engine::Intern("id");
    int tokSet = Engine::Intern("set_strategic_region_name");
    int tokReset = Engine::Intern("reset_strategic_region_name");
    bool ok = g_tokRegion && g_tokId && tokSet && tokReset && Engine::RegisterEffect(tokSet, &g_setEntry) &&
              Engine::RegisterEffect(tokReset, &g_resetEntry);
    Log("region_names: native effects %s (tokens region=0x%x id=0x%x set=0x%x reset=0x%x)",
        ok ? "registered" : "NOT registered", g_tokRegion, g_tokId, tokSet, tokReset);
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

    // Native effects are optional: without them the flag protocol still works.
    g_nativeEffects = Engine::HaveRegistry() &&
                      SlotsLookRight(Engine::pRenameProvinceEffectVtable, Engine::renameProvinceEffectVtableSlots,
                                     {kSlotReadItem, kSlotTooltip, kSlotExecute}, base, baseSlots, "set_province_name");
    if (g_nativeEffects) {
        g_setVtable = CloneVtable((void*)&ReadItem, (void*)&Tooltip, (void*)&SetExecute);
        g_resetVtable = CloneVtable((void*)&ReadItem, (void*)&Tooltip, (void*)&ResetExecute);
    } else {
        Log("region_names: native effects unavailable; use the hoi4_srn_<id>_<key> flags");
    }

    if (!HookQueue(Engine::pRegionGetNameKey, (void*)&GetNameKeyDetour, (void**)&g_origGetNameKey, "region_get_name_key") ||
        !HookQueue(setVt[kSlotExecute], (void*)&SetFlagExecDetour, (void**)&g_origSetFlagExec, "set_global_flag_execute") ||
        !HookQueue(clrVt[kSlotExecute], (void*)&ClearFlagExecDetour, (void**)&g_origClearFlagExec, "clr_global_flag_execute"))
        return false;
    OnLexerReady(&RegisterNativeEffects);
    OnGameEntered(&OnEntered);
    return true;
}
