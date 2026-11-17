// map_adjacency (engine tier): script closes and opens map connections at runtime (NOTES.md Part R).
//
//   set_province_adjacency = { province = 42 neighbour = 43 passable = no }   # yes opens it again
//   reset_province_adjacency = { province = 42 neighbour = 43 }               # back to the map's
//   province_adjacency_passable = { province = 42 neighbour = 43 }            # trigger
//   set_state_impassable = { state = 514 impassable = yes }                   # or in a state scope
//   reset_state_impassable = { state = 514 }                                  # back to history/states'
//   (vanilla's state trigger impassable = yes reads the flag)
//
// A border is closed as the game's own impassable borders (map/adjacencies.csv), in both
// directions; a state as the game's own impassable states (history/states). The game reads both
// live, so movement, attacks, supply and the AI follow at once. Storage: global flags
// hoi4_adj_<low>_<high>_<0|1> and hoi4_imp_<state>_<0|1> (1 = passable / impassable). The bytes live
// in the static map and state data, which outlives a game: before every game entry the originals
// come back, after it the save's flags are applied. Without the loader the flags are inert.
#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "native_script.h"

namespace {

const char* const kFeature = "map_adjacency";
const char kAdjPrefix[] = "hoi4_adj_";
const char kImpPrefix[] = "hoi4_imp_";

// Main thread only (effects, lifecycle).
std::map<std::pair<int, int>, int> g_origBorder;  // (a, b) -> a's entry type for b at the first change
std::map<int, bool> g_origState;                  // state -> its impassable flag at the first change
volatile bool g_ready = false;                    // this game's self-check passed (the trigger reads it)

enum { kProvince, kNeighbour, kPassable };
enum { kState, kImpassable };

// --- Flags ------------------------------------------------------------------------------------

// "<prefix><n>_<m>_<v>" (two numbers) or "<prefix><n>_<v>" (one): the numbers and v. False for
// other flags.
bool ParseFlag(int token, const char* prefix, int numbers, int out[3]) {
    const char* s = Engine::TokenName(token);
    size_t len = strlen(prefix);
    if (strncmp(s, prefix, len) != 0) return false;
    const char* p = s + len;
    for (int k = 0; k <= numbers; ++k) {
        char* end = nullptr;
        long v = strtol(p, &end, 10);
        if (end == p || v < 0 || v > 10000000) return false;
        out[k] = (int)v;
        if (k < numbers) {
            if (*end != '_') return false;
            p = end + 1;
        } else if (*end) {
            return false;
        }
    }
    return out[numbers] <= 1;
}

std::pair<int, int> Pair(int a, int b) { return a < b ? std::make_pair(a, b) : std::make_pair(b, a); }

void ClearFlags(const char* prefix, int numbers, int a, int b) {
    struct Ctx {
        const char* prefix;
        int numbers, a, b;
        std::vector<int> tokens;
    } ctx{prefix, numbers, a, b, {}};
    Engine::ForEachGlobalFlag(
        [](const Engine::FlagEntry& f, void* c) {
            auto* x = (Ctx*)c;
            int v[3];
            if (ParseFlag(f.token, x->prefix, x->numbers, v) && v[0] == x->a && (x->numbers == 1 || v[1] == x->b))
                x->tokens.push_back(f.token);
        },
        &ctx);
    for (int t : ctx.tokens) Engine::ClearGlobalFlag(t);
}

void SetFlag(const std::string& name) { Engine::SetGlobalFlag(Engine::Intern(name.c_str())); }

// --- Changes ----------------------------------------------------------------------------------

bool Impassable(int type) { return type == Engine::ImpassableBorderType(); }

// Both entries' types now; false if a and b are not neighbours both ways.
bool Border(int a, int b, int* ab, int* ba) {
    *ab = Engine::BorderType(a, b);
    *ba = Engine::BorderType(b, a);
    return *ab >= 0 && *ba >= 0;
}

bool Passable(int a, int b) {
    int ab, ba;
    return Border(a, b, &ab, &ba) && !Impassable(ab) && !Impassable(ba);
}

// The original type of a's entry for b (recorded at the first change).
int OriginalType(int a, int b) {
    auto it = g_origBorder.find({a, b});
    return it != g_origBorder.end() ? it->second : Engine::BorderType(a, b);
}

// Both directions to their passable (or closed) type: closed = the impassable type; open = the
// original if that was passable, else a plain border (0).
void ApplyBorder(int a, int b, bool passable) {
    for (auto d : {std::make_pair(a, b), std::make_pair(b, a)}) {
        int cur = Engine::BorderType(d.first, d.second);
        g_origBorder.emplace(d, cur);
        int orig = OriginalType(d.first, d.second);
        int to = passable ? (Impassable(orig) ? 0 : orig) : Engine::ImpassableBorderType();
        if (cur != to) Engine::SetBorderType(d.first, d.second, to);
    }
}

void ApplyState(int state, bool impassable) {
    int cur = Engine::StateImpassable(state);
    if (cur < 0) return;
    g_origState.emplace(state, cur == 1);
    if ((cur == 1) != impassable) Engine::SetStateImpassable(state, impassable);
}

void RestoreAll() {
    for (const auto& o : g_origBorder)
        if (Engine::BorderType(o.first.first, o.first.second) != o.second) Engine::SetBorderType(o.first.first, o.first.second, o.second);
    for (const auto& o : g_origState)
        if ((Engine::StateImpassable(o.first) == 1) != o.second) Engine::SetStateImpassable(o.first, o.second);
    g_origBorder.clear();
    g_origState.clear();
}

bool Ready(const char* command) {
    if (!g_ready) return false;
    if (!IsMainThread()) {
        Log("%s: ERROR called off the main thread; nothing changed", command);
        return false;
    }
    return true;
}

// The two provinces of a use; false (logged) if they are no neighbours.
bool Provinces(const Args& a, const char* command, int* p, int* q) {
    *p = a.i(kProvince);
    *q = a.i(kNeighbour);
    int ab, ba;
    if (*p == *q || !Border(*p, *q, &ab, &ba)) {
        Log("%s: ERROR provinces %d and %d are not neighbours on the map; nothing changed", command, *p, *q);
        return false;
    }
    return true;
}

void ExecSetBorder(const Args& a, const Scope&) {
    const char* cmd = "set_province_adjacency";
    if (!Ready(cmd)) return;
    int p, q;
    if (!Provinces(a, cmd, &p, &q)) return;
    bool passable = a.b(kPassable);
    if (Passable(p, q) == passable) return;
    ApplyBorder(p, q, passable);
    std::pair<int, int> k = Pair(p, q);
    ClearFlags(kAdjPrefix, 2, k.first, k.second);
    bool origPassable = !Impassable(OriginalType(p, q)) && !Impassable(OriginalType(q, p));
    if (passable != origPassable)
        SetFlag(kAdjPrefix + std::to_string(k.first) + "_" + std::to_string(k.second) + (passable ? "_1" : "_0"));
    Log("%s: the border %d-%d is %s", cmd, p, q, passable ? "open" : "closed");
}

void ExecResetBorder(const Args& a, const Scope&) {
    const char* cmd = "reset_province_adjacency";
    if (!Ready(cmd)) return;
    int p, q;
    if (!Provinces(a, cmd, &p, &q)) return;
    bool changed = false;
    for (auto d : {std::make_pair(p, q), std::make_pair(q, p)}) {
        auto it = g_origBorder.find(d);
        if (it != g_origBorder.end() && Engine::BorderType(d.first, d.second) != it->second) {
            Engine::SetBorderType(d.first, d.second, it->second);
            changed = true;
        }
    }
    std::pair<int, int> k = Pair(p, q);
    ClearFlags(kAdjPrefix, 2, k.first, k.second);
    if (changed) Log("%s: the border %d-%d is back to the map's", cmd, p, q);
}

int StateOf(const Args& a, const Scope& s) { return a.has(kState) ? a.i(kState) : s.stateId(); }

void ExecSetState(const Args& a, const Scope& s) {
    const char* cmd = "set_state_impassable";
    if (!Ready(cmd)) return;
    int state = StateOf(a, s);
    int cur = Engine::StateImpassable(state);
    if (cur < 0) {
        Log("%s: ERROR state %d does not exist (or no state given outside a state scope); nothing changed", cmd, state);
        return;
    }
    bool to = a.b(kImpassable);
    if ((cur == 1) == to) return;
    ApplyState(state, to);
    ClearFlags(kImpPrefix, 1, state, 0);
    if (to != g_origState[state]) SetFlag(kImpPrefix + std::to_string(state) + (to ? "_1" : "_0"));
    Log("%s: state %d is %s", cmd, state, to ? "impassable" : "passable");
}

void ExecResetState(const Args& a, const Scope& s) {
    const char* cmd = "reset_state_impassable";
    if (!Ready(cmd)) return;
    int state = StateOf(a, s);
    if (Engine::StateImpassable(state) < 0) {
        Log("%s: ERROR state %d does not exist (or no state given outside a state scope); nothing changed", cmd, state);
        return;
    }
    auto it = g_origState.find(state);
    if (it != g_origState.end() && (Engine::StateImpassable(state) == 1) != it->second) {
        Engine::SetStateImpassable(state, it->second);
        Log("%s: state %d is back to history/states' (%s)", cmd, state, it->second ? "impassable" : "passable");
    }
    ClearFlags(kImpPrefix, 1, state, 0);
}

// --- Trigger (hot path: reads only) -----------------------------------------------------------

bool EvalPassable(const Args& a, const Scope&) { return g_ready && Passable(a.i(kProvince), a.i(kNeighbour)); }

// --- Tooltips ---------------------------------------------------------------------------------

std::string StateShown(int id) {
    std::string key = "STATE_" + std::to_string(id);
    std::string s = native_localize_key(key.c_str());
    return s == key ? "state " + std::to_string(id) : s;
}

const LocText kCloseText{"HOI4_LOADER_CLOSE_BORDER", "The border between provinces $A|Y$ and $B|Y$ becomes impassable."};
const LocText kOpenText{"HOI4_LOADER_OPEN_BORDER", "The border between provinces $A|Y$ and $B|Y$ becomes passable."};
const LocText kResetBorderText{"HOI4_LOADER_RESET_BORDER", "The border between provinces $A|Y$ and $B|Y$ is restored."};
const LocText kStateCloseText{"HOI4_LOADER_STATE_IMPASSABLE", "$STATE|Y$ becomes impassable."};
const LocText kStateOpenText{"HOI4_LOADER_STATE_PASSABLE", "$STATE|Y$ becomes passable."};
const LocText kStateResetText{"HOI4_LOADER_STATE_IMPASSABLE_RESET", "$STATE|Y$ gets its original passability back."};

void TooltipSetBorder(const Args& a, const Scope&, TooltipText& out) {
    out.add(native_localize(a.b(kPassable) ? kOpenText : kCloseText,
                            {{"A", std::to_string(a.i(kProvince))}, {"B", std::to_string(a.i(kNeighbour))}})
                .c_str());
}
void TooltipResetBorder(const Args& a, const Scope&, TooltipText& out) {
    out.add(native_localize(kResetBorderText, {{"A", std::to_string(a.i(kProvince))}, {"B", std::to_string(a.i(kNeighbour))}}).c_str());
}
void TooltipSetState(const Args& a, const Scope& s, TooltipText& out) {
    out.add(native_localize(a.b(kImpassable) ? kStateCloseText : kStateOpenText, {{"STATE", StateShown(StateOf(a, s))}}).c_str());
}
void TooltipResetState(const Args& a, const Scope& s, TooltipText& out) {
    out.add(native_localize(kStateResetText, {{"STATE", StateShown(StateOf(a, s))}}).c_str());
}
void TooltipPassable(const Args& a, const Scope&, bool negated, TooltipText& out) {
    out.add("The border between provinces ").add(a.i(kProvince)).add(" and ").add(a.i(kNeighbour)).add(negated ? " is NOT passable" : " is passable");
}

// --- Lifecycle --------------------------------------------------------------------------------

void SwitchOff(const char* why) {
    g_ready = false;
    RestoreAll();
    FeatureDeactivate(kFeature, why);
}

void OnEntering() {
    g_ready = false;
    RestoreAll();  // the map and state data outlive the previous game
}

// After the entry: the save's flags (the latest per border / state).
void OnEntered(bool loaded) {
    if (!FeatureActive(kFeature)) return;
    std::string problem;
    if (!Engine::AdjacencySelfCheck(&problem)) {
        SwitchOff(("self-check failed: " + problem).c_str());
        return;
    }
    struct Latest {
        std::map<std::pair<int, int>, std::pair<int, bool>> borders;  // -> (date, passable)
        std::map<int, std::pair<int, bool>> states;                     // -> (date, impassable)
    } latest;
    Engine::ForEachGlobalFlag(
        [](const Engine::FlagEntry& f, void* c) {
            auto* l = (Latest*)c;
            int v[3];
            if (ParseFlag(f.token, kAdjPrefix, 2, v)) {
                auto& e = l->borders[{v[0], v[1]}];
                if (f.setDate >= e.first) e = {f.setDate, v[2] == 1};
            } else if (ParseFlag(f.token, kImpPrefix, 1, v)) {
                auto& e = l->states[v[0]];
                if (f.setDate >= e.first) e = {f.setDate, v[1] == 1};
            }
        },
        &latest);
    g_ready = true;
    int borders = 0, states = 0;
    for (const auto& b : latest.borders) {
        int ab, ba;
        if (!Border(b.first.first, b.first.second, &ab, &ba)) {
            Log("map_adjacency: flag for %d-%d: not neighbours on this map; ignored", b.first.first, b.first.second);
            continue;
        }
        ApplyBorder(b.first.first, b.first.second, b.second.second);
        ++borders;
    }
    for (const auto& s : latest.states) {
        if (Engine::StateImpassable(s.first) < 0) {
            Log("map_adjacency: flag for state %d: no such state; ignored", s.first);
            continue;
        }
        ApplyState(s.first, s.second.second);
        ++states;
    }
    Log("map_adjacency: %s: %d border(s) and %d state(s) changed by saved flags", loaded ? "save loaded" : "new game", borders, states);
}

}  // namespace

bool MapAdjacencyInit() {
    if (!Engine::HaveAdjacency() || !Engine::HaveTokens() || !Engine::HaveFlags()) {
        Log("%s: required engine functions or fields missing (adjacency %d, tokens %d, flags %d)", kFeature, Engine::HaveAdjacency(),
            Engine::HaveTokens(), Engine::HaveFlags());
        return false;
    }
    OnGameEntering(&OnEntering);
    OnGameEntered(&OnEntered);

    const std::vector<ParamSpec> border = {{"province", ParamType::Int, true}, {"neighbour", ParamType::Int, true}};
    std::vector<ParamSpec> setBorder = border;
    setBorder.push_back({"passable", ParamType::Bool, true});
    const std::vector<ParamSpec> state = {{"state", ParamType::Int, false, ScopeDefault::State}};
    std::vector<ParamSpec> setState = state;
    setState.push_back({"impassable", ParamType::Bool, true});

    register_native_effect({"set_province_adjacency",
                            "Close (passable = no) or open (passable = yes) the border between two neighbouring "
                            "provinces, both directions (hoi4_loader). Closed works like map/adjacencies.csv's "
                            "impassable: no movement, attacks or supply across. Opening a border the map made impassable "
                            "makes it a plain border. Stored as global flag hoi4_adj_<low>_<high>_<0|1>: survives save "
                            "and load; without the loader the map's borders stay.",
                            Form::Block, setBorder, ScopeMask::Any, &ExecSetBorder, &TooltipSetBorder, {kCloseText, kOpenText}},
                           kFeature);
    register_native_effect({"reset_province_adjacency",
                            "Give the border between two provinces the map's type back (hoi4_loader); clears its "
                            "hoi4_adj flags.",
                            Form::Block, border, ScopeMask::Any, &ExecResetBorder, &TooltipResetBorder, {kResetBorderText}},
                           kFeature);
    register_native_trigger({"province_adjacency_passable",
                             "True if the two provinces are neighbours and the border between them is not impassable "
                             "(the map's or a changed one). Guard it with hoi4_loader_feature_map_adjacency = yes in the "
                             "same AND block.",
                             Form::Block, border, ScopeMask::Any, &EvalPassable, &TooltipPassable},
                            kFeature);
    register_native_effect({"set_state_impassable",
                            "Make a state impassable (impassable = yes) or passable (no), as history/states' impassable "
                            "(hoi4_loader). `state = N`, else the state scope. Vanilla's impassable trigger follows. Stored "
                            "as global flag hoi4_imp_<state>_<0|1>: survives save and load; without the loader the states "
                            "keep history/states'.",
                            Form::Block, setState, ScopeMask::Any, &ExecSetState, &TooltipSetState, {kStateCloseText, kStateOpenText}},
                           kFeature);
    register_native_effect({"reset_state_impassable",
                            "Give a state history/states' impassable value back (hoi4_loader); clears its hoi4_imp "
                            "flags. `state = N`, else the state scope.",
                            Form::Block, state, ScopeMask::Any, &ExecResetState, &TooltipResetState, {kStateResetText}},
                           kFeature);
    return true;
}
