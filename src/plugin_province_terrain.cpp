// province_terrain (engine tier): script changes a province's terrain category at runtime
// (NOTES.md Part P).
//
//   set_province_terrain = { province = 1234 terrain = marsh }                 # one or several
//   set_province_terrain = { province = 1234 if_terrain = plains terrain = marsh }  # only if plains now
//   42 = { set_province_terrain = { if_terrain = plains terrain = marsh } }    # a state: if_terrain needed
//   reset_province_terrain = { province = 1234 }   # or a state: back to map/definition.csv's
//   province_has_terrain = { province = 1234 terrain = marsh }   # triggers (with the guard)
//   state_has_terrain = { terrain = plains }                     # state scope or state = 42
//
// The change happens inside the effect (main thread, serial code: the same tick on every machine)
// and updates what the game derived from the terrain (masks, battles, supply). Storage: a global
// flag per changed province, hoi4_ptr_<province>_<category>; the categories live in the static map
// data, which outlives a game, so before every game entry the originals come back and after it the
// save's flags are applied. Without the loader the flags are inert and saves stay vanilla.
#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "native_script.h"

namespace {

const char* const kFeature = "province_terrain";
const char kPrefix[] = "hoi4_ptr_";
const size_t kPrefixLen = sizeof kPrefix - 1;

// Main thread only (effects, lifecycle).
std::map<int, const void*> g_original;  // province -> its map/definition.csv category (first change this session)
volatile bool g_ready = false;           // this game's self-check passed (triggers read it)

// set_province_terrain / reset_province_terrain parameters.
enum { kProvince, kState, kIfTerrain, kTerrain };

const char* TerrainName(const void* t) { return t ? Engine::TokenName(Engine::TerrainToken(t)) : ""; }

// --- Flags ------------------------------------------------------------------------------------

// "hoi4_ptr_<id>_<category>" -> id and category name. False for other flags.
bool ParseFlag(int token, int* id, std::string* name) {
    const char* s = Engine::TokenName(token);
    if (strncmp(s, kPrefix, kPrefixLen) != 0) return false;
    char* end = nullptr;
    long v = strtol(s + kPrefixLen, &end, 10);
    if (end == s + kPrefixLen || *end != '_' || !end[1] || v <= 0 || v > 10000000) return false;
    *id = (int)v;
    *name = end + 1;
    return true;
}

std::vector<int> FlagTokensOf(int id) {
    struct Ctx {
        int id;
        std::vector<int> tokens;
    } ctx{id, {}};
    Engine::ForEachGlobalFlag(
        [](const Engine::FlagEntry& f, void* c) {
            auto* x = (Ctx*)c;
            int pid = 0;
            std::string name;
            if (ParseFlag(f.token, &pid, &name) && pid == x->id) x->tokens.push_back(f.token);
        },
        &ctx);
    return ctx.tokens;
}

// The province's flags say `terrain` (none when it is the original).
void StoreFlag(int id, const void* terrain) {
    for (int t : FlagTokensOf(id)) Engine::ClearGlobalFlag(t);
    auto it = g_original.find(id);
    if (it != g_original.end() && it->second == terrain) return;
    std::string flag = kPrefix + std::to_string(id) + "_" + TerrainName(terrain);
    Engine::SetGlobalFlag(Engine::Intern(flag.c_str()));
}

// --- Changes ----------------------------------------------------------------------------------

// Provinces restored before a game entry: the game built their states' masks before that, so they
// are refreshed once the game is entered.
std::vector<int> g_restored;

void RestoreAll() {
    for (const auto& o : g_original)
        if (Engine::SetMapTerrain(o.first, o.second)) g_restored.push_back(o.first);
    g_original.clear();
}

void SwitchOff(const char* why) {
    g_ready = false;
    RestoreAll();  // the map data outlives the game: back to map/definition.csv's
    FeatureDeactivate(kFeature, why);
}

// The game's terrain of the province becomes `terrain` (recording the original first).
bool Change(int id, const void* terrain, int* battles = nullptr) {
    const void* cur = Engine::ProvinceTerrain(id);
    if (!cur) return false;
    g_original.emplace(id, cur);  // keeps the first (the original)
    if (cur == terrain) return true;
    if (!Engine::SetProvinceTerrain(id, terrain, battles)) {
        SwitchOff("a terrain change failed (see the line above)");
        return false;
    }
    return true;
}

bool Ready(const char* command) {
    if (!g_ready) return false;
    if (!IsMainThread()) {
        Log("%s: ERROR called off the main thread; nothing changed", command);
        return false;
    }
    return true;
}

// The provinces a use names: the `province` values, else the state's (written, else the state
// scope's). *stateId: the state used (0 for provinces). False (logged) if neither is there.
bool Targets(const Args& a, const Scope& s, const char* command, std::vector<int>* out, int* stateId) {
    out->clear();
    *stateId = 0;
    if (a.count(kProvince)) {
        for (size_t k = 0; k < a.count(kProvince); ++k) out->push_back(a.at(kProvince, k));
        return true;
    }
    int state = a.has(kState) ? a.i(kState) : s.stateId();
    if (state <= 0) {
        Log("%s: ERROR needs `province`, `state` or a state scope; nothing changed", command);
        return false;
    }
    if (!Engine::StateProvinceIds(state, out) || out->empty()) {
        Log("%s: ERROR state %d does not exist or has no provinces; nothing changed", command, state);
        return false;
    }
    *stateId = state;
    return true;
}

void ExecSet(const Args& a, const Scope& s) {
    const char* cmd = "set_province_terrain";
    if (!Ready(cmd)) return;
    const void* to = Engine::TerrainByToken(a.i(kTerrain));
    if (!to) {
        Log("%s: ERROR no terrain category '%s' (common/terrain); nothing changed", cmd, a.key(kTerrain));
        return;
    }
    if (Engine::TerrainIsWater(to)) {
        Log("%s: ERROR '%s' is a water category (is_water); only land categories; nothing changed", cmd, a.key(kTerrain));
        return;
    }
    std::vector<const void*> only;
    for (size_t k = 0; k < a.count(kIfTerrain); ++k) {
        int token = a.at(kIfTerrain, k);
        if (const void* t = Engine::TerrainByToken(token))
            only.push_back(t);
        else
            Log("%s: if_terrain '%s' is not a terrain category (common/terrain); it matches no province", cmd,
                Engine::TokenName(token));
    }
    std::vector<int> ids;
    int state = 0;
    if (!Targets(a, s, cmd, &ids, &state)) return;
    if (state && !a.count(kIfTerrain)) {
        Log("%s: ERROR a state needs if_terrain (which of its provinces); nothing changed in state %d", cmd, state);
        return;
    }
    int changed = 0, missing = 0, water = 0, battles = 0;
    for (int id : ids) {
        const void* cur = Engine::ProvinceTerrain(id);
        if (!cur) {
            ++missing;
            continue;
        }
        if (Engine::TerrainIsWater(cur)) {
            ++water;
            continue;
        }
        if (a.count(kIfTerrain) && std::find(only.begin(), only.end(), cur) == only.end()) continue;
        if (cur == to) continue;
        if (!Change(id, to, &battles)) return;
        StoreFlag(id, to);
        ++changed;
    }
    // Only what changed: an every_state loop would otherwise log a line per state.
    if (changed && state)
        Log("%s: state %d: %d province(s) -> %s, %d battle(s) switched", cmd, state, changed, TerrainName(to), battles);
    else if (changed)
        Log("%s: %d of %zu province(s) -> %s, %d battle(s) switched", cmd, changed, ids.size(), TerrainName(to), battles);
    if (missing) Log("%s: %d province id(s) do not exist; skipped", cmd, missing);
    if (water && !state) Log("%s: %d water province(s) (sea, lake) skipped", cmd, water);
}

void ExecReset(const Args& a, const Scope& s) {
    const char* cmd = "reset_province_terrain";
    if (!Ready(cmd)) return;
    std::vector<int> ids;
    int state = 0;
    if (!Targets(a, s, cmd, &ids, &state)) return;
    int reset = 0;
    for (int id : ids) {
        auto it = g_original.find(id);
        bool flagged = !FlagTokensOf(id).empty();
        if (it == g_original.end() && !flagged) continue;
        if (it != g_original.end() && Engine::ProvinceTerrain(id) != it->second) {
            if (!Change(id, it->second)) return;
            ++reset;
        }
        for (int t : FlagTokensOf(id)) Engine::ClearGlobalFlag(t);
    }
    if (reset && state)
        Log("%s: state %d: %d province(s) back to their original terrain", cmd, state, reset);
    else if (reset)
        Log("%s: %d of %zu province(s) back to their original terrain", cmd, reset, ids.size());
}

// --- Triggers (hot path: reads only, no allocation, no logging) ------------------------------

bool EvalProvinceHas(const Args& a, const Scope&) {
    if (!g_ready) return false;
    const void* t = Engine::TerrainByToken(a.i(1));
    return t && Engine::ProvinceTerrain(a.i(0)) == t;
}

bool EvalStateHas(const Args& a, const Scope&) {
    if (!g_ready) return false;
    const void* t = Engine::TerrainByToken(a.i(1));
    return t && Engine::StateHasTerrain(a.i(0), t);
}

// --- Tooltips ---------------------------------------------------------------------------------

std::string TerrainShown(const char* name) { return native_localize_key(name); }

std::string StateShown(int id) {
    std::string key = "STATE_" + std::to_string(id);
    std::string s = native_localize_key(key.c_str());
    return s == key ? "state " + std::to_string(id) : s;
}

std::string ProvincesShown(const Args& a) {
    std::string s;
    for (size_t k = 0; k < a.count(kProvince); ++k) s += (k ? ", " : "") + std::to_string(a.at(kProvince, k));
    return s;
}

std::string IfShown(const Args& a) {
    std::string s;
    size_t n = a.count(kIfTerrain);
    for (size_t k = 0; k < n; ++k) {
        if (k) s += k + 1 == n ? " or " : ", ";
        s += TerrainShown(Engine::TokenName(a.at(kIfTerrain, k)));
    }
    return s;
}

const LocText kSetText{"HOI4_LOADER_SET_PROVINCE_TERRAIN", "The terrain of province $PROVINCE|Y$ becomes $TERRAIN|Y$."};
const LocText kSetIfText{"HOI4_LOADER_SET_PROVINCE_TERRAIN_IF",
                         "The terrain of province $PROVINCE|Y$ becomes $TERRAIN|Y$ where it is $IF|Y$."};
const LocText kSetStateText{"HOI4_LOADER_SET_STATE_TERRAIN", "The $IF|Y$ provinces of $STATE|Y$ become $TERRAIN|Y$."};
const LocText kSetScopeText{"HOI4_LOADER_SET_STATE_TERRAIN_IN_SCOPE", "Its $IF|Y$ provinces become $TERRAIN|Y$."};
const LocText kResetText{"HOI4_LOADER_RESET_PROVINCE_TERRAIN", "Province $PROVINCE|Y$ gets its original terrain back."};
const LocText kResetStateText{"HOI4_LOADER_RESET_STATE_TERRAIN", "The provinces of $STATE|Y$ get their original terrain back."};
const LocText kResetScopeText{"HOI4_LOADER_RESET_STATE_TERRAIN_IN_SCOPE", "Its provinces get their original terrain back."};

void TooltipSet(const Args& a, const Scope& s, TooltipText& out) {
    std::string to = TerrainShown(a.key(kTerrain));
    if (a.count(kProvince)) {
        if (a.count(kIfTerrain))
            out.add(native_localize(kSetIfText, {{"PROVINCE", ProvincesShown(a)}, {"TERRAIN", to}, {"IF", IfShown(a)}}).c_str());
        else
            out.add(native_localize(kSetText, {{"PROVINCE", ProvincesShown(a)}, {"TERRAIN", to}}).c_str());
        return;
    }
    if (a.has(kState))
        out.add(native_localize(kSetStateText, {{"STATE", StateShown(a.i(kState))}, {"TERRAIN", to}, {"IF", IfShown(a)}}).c_str());
    else
        out.add(native_localize(kSetScopeText, {{"TERRAIN", to}, {"IF", IfShown(a)}}).c_str());
    (void)s;
}

void TooltipReset(const Args& a, const Scope& s, TooltipText& out) {
    if (a.count(kProvince))
        out.add(native_localize(kResetText, {{"PROVINCE", ProvincesShown(a)}}).c_str());
    else if (a.has(kState))
        out.add(native_localize(kResetStateText, {{"STATE", StateShown(a.i(kState))}}).c_str());
    else
        out.add(native_localize(kResetScopeText).c_str());
    (void)s;
}

// "Province 6487 has Jungle terrain", "Brandenburg has Plains terrain" (a province of it), in a
// state scope "Has Plains terrain".
void TooltipProvinceHas(const Args& a, const Scope&, bool negated, TooltipText& out) {
    out.add("Province ").add(a.i(0)).add(negated ? " does NOT have " : " has ").add(TerrainShown(a.key(1)).c_str()).add(" terrain");
}

void TooltipStateHas(const Args& a, const Scope&, bool negated, TooltipText& out) {
    bool named = a.has(0) && !a.fromScope(0);
    if (named) out.add(StateShown(a.i(0)).c_str()).add(" ");
    out.add(negated ? (named ? "has no " : "Has no ") : (named ? "has " : "Has ")).add(TerrainShown(a.key(1)).c_str()).add(" terrain");
}

// --- Lifecycle --------------------------------------------------------------------------------

// Before every game entry: the originals back (the map data outlives the previous game).
void OnEntering() {
    g_ready = false;
    RestoreAll();
}

// After it: check this game's layout, refresh what the game built from the previous game's terrain
// (the restored provinces' state and country masks, battles, supply), then apply the save's flags
// (the latest flag per province).
void OnEntered(bool loaded) {
    std::vector<int> restored;
    restored.swap(g_restored);
    if (!FeatureActive(kFeature)) return;
    std::string problem;
    if (!Engine::TerrainSelfCheck(restored, &problem)) {
        SwitchOff(("self-check failed: " + problem).c_str());
        return;
    }
    for (int id : restored)
        if (!Engine::SetProvinceTerrain(id, Engine::ProvinceTerrain(id))) {
            SwitchOff("refreshing a restored province failed (see the line above)");
            return;
        }
    if (!restored.empty()) Log("province_terrain: %zu province(s) restored to map/definition.csv's terrain", restored.size());
    struct Latest {
        int date;
        std::string name;
    };
    std::map<int, Latest> latest;
    Engine::ForEachGlobalFlag(
        [](const Engine::FlagEntry& f, void* c) {
            auto* m = (std::map<int, Latest>*)c;
            int id = 0;
            std::string name;
            if (!ParseFlag(f.token, &id, &name)) return;
            auto it = m->find(id);
            if (it == m->end() || f.setDate >= it->second.date) (*m)[id] = Latest{f.setDate, name};
        },
        &latest);
    g_ready = true;
    int applied = 0;
    for (const auto& l : latest) {
        const void* t = Engine::TerrainByToken(Engine::Intern(l.second.name.c_str()));
        const void* cur = Engine::ProvinceTerrain(l.first);
        if (!t || Engine::TerrainIsWater(t) || !cur || Engine::TerrainIsWater(cur)) {
            Log("province_terrain: flag for province %d: '%s' is not a land category or the province is not land; ignored",
                l.first, l.second.name.c_str());
            continue;
        }
        if (!Change(l.first, t)) return;
        ++applied;
    }
    Log("province_terrain: %s: %d province(s) changed by saved flags", loaded ? "save loaded" : "new game", applied);
}

}  // namespace

bool ProvinceTerrainInit() {
    if (!Engine::HaveTerrain() || !Engine::HaveTokens() || !Engine::HaveFlags()) {
        Log("%s: required engine functions or fields missing (terrain %d, tokens %d, flags %d)", kFeature, Engine::HaveTerrain(),
            Engine::HaveTokens(), Engine::HaveFlags());
        return false;
    }
    OnGameEntering(&OnEntering);
    OnGameEntered(&OnEntered);

    const std::vector<ParamSpec> setParams = {{"province", ParamType::Int, false, ScopeDefault::None, nullptr, false, true},
                                              {"state", ParamType::Int, false},
                                              {"if_terrain", ParamType::Token, false, ScopeDefault::None, nullptr, false, true},
                                              {"terrain", ParamType::Token, true}};
    const std::vector<ParamSpec> resetParams = {{"province", ParamType::Int, false, ScopeDefault::None, nullptr, false, true},
                                                {"state", ParamType::Int, false}};
    register_native_effect({"set_province_terrain",
                            "Change provinces' terrain category (hoi4_loader): `province` (several allowed), or the "
                            "provinces of a state (`state = N`, else the state scope) that have one of the `if_terrain` "
                            "categories (required for a state; with `province` it filters too). `terrain`: a land "
                            "category from common/terrain. Water provinces and categories are refused. Movement, combat "
                            "(running battles too), supply, attrition and has_terrain follow at once; the map's look "
                            "does not change. Stored as global flag hoi4_ptr_<province>_<category>: survives save and "
                            "load; without the loader the provinces keep map/definition.csv's terrain.",
                            Form::Block, setParams, ScopeMask::Any, &ExecSet, &TooltipSet,
                            {kSetText, kSetIfText, kSetStateText, kSetScopeText}},
                           kFeature);
    register_native_effect({"reset_province_terrain",
                            "Give provinces their original terrain back (hoi4_loader): `province` (several allowed), or "
                            "every changed province of a state (`state = N`, else the state scope). Clears their "
                            "hoi4_ptr flags.",
                            Form::Block, resetParams, ScopeMask::Any, &ExecReset, &TooltipReset,
                            {kResetText, kResetStateText, kResetScopeText}},
                           kFeature);
    register_native_trigger({"province_has_terrain",
                             "True if the province's terrain category is `terrain` now (changed or not). Guard it with "
                             "hoi4_loader_feature_province_terrain = yes in the same AND block.",
                             Form::Block, {{"province", ParamType::Int, true}, {"terrain", ParamType::Token, true}}, ScopeMask::Any,
                             &EvalProvinceHas, &TooltipProvinceHas},
                            kFeature);
    register_native_trigger({"state_has_terrain",
                             "True if any province of the state has the terrain category `terrain`. In a state scope "
                             "`state` may be left out; without it and outside one it is false (TRUE under NOT). Guard "
                             "it with hoi4_loader_feature_province_terrain = yes in the same AND block.",
                             Form::Block,
                             {{"state", ParamType::Int, false, ScopeDefault::State}, {"terrain", ParamType::Token, true}},
                             ScopeMask::Any, &EvalStateHas, &TooltipStateHas},
                            kFeature);
    return true;
}
