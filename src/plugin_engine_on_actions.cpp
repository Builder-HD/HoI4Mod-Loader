// engine_on_actions (engine tier): on_actions for moments vanilla fires none for (NOTES.md Part Q).
//
//   on_actions = {
//       on_construction_finished = { effect = { ... } }  # ROOT = the country, FROM = the state
//       on_technology_researched = { ... }               # ROOT = the country
//       on_battle_started = { ... }                       # ROOT = each country in it, FROM = the other side's first
//       on_battle_won = { ... }   on_battle_lost = { ... } # land battles, per country of each side
//       on_division_trained = { ... }                     # ROOT = the country
//       on_division_destroyed = { ... }                   # ROOT = the country (destroyed in battle)
//   }
//
// They behave like vanilla's on_actions (events, random_events, effect; fired at the moment, only
// while a game runs). Data the scopes can't carry: constructed_building = <type>,
// researched_technology = <technology> (triggers), constructed_province and battle_province
// (values, also game variables). A name the game's executable uses itself is left to the game.
#include <windows.h>

#include <string.h>

#include <string>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "native_script.h"

namespace {

const char* const kFeature = "engine_on_actions";
using Kind = Engine::EngineEventKind;
const int kKinds = (int)Kind::Count;
const char* const kNames[kKinds] = {"on_construction_finished", "on_technology_researched", "on_battle_started",
                                    "on_battle_won",            "on_battle_lost",           "on_division_trained",
                                    "on_division_destroyed",    "on_naval_battle_won",      "on_naval_battle_lost",
                                    "on_ship_sunk",             "on_decision_taken",        "on_province_captured"};

bool g_vanillaOwns[kKinds] = {};
void* g_lists[kKinds] = {};
bool g_looked = false;
const Engine::EngineEvent* g_current = nullptr;  // the event being fired (main thread)

// Does the executable name this on_action itself (a later game version firing it)?
bool ExeNames(const char* name) {
    size_t n = strlen(name) + 1;  // with the terminating NUL
    for (const char* sec : {".rdata", ".data"}) {
        uint8_t* begin = nullptr;
        size_t size = 0;
        if (!ImageSection(sec, &begin, &size)) continue;
        for (size_t i = 1; i + n <= size; ++i)
            if (begin[i] == (uint8_t)name[0] && begin[i - 1] == 0 && memcmp(begin + i, name, n) == 0) return true;
    }
    return false;
}

void Sink(const Engine::EngineEvent& ev) {
    int k = (int)ev.kind;
    if (k < 0 || k >= kKinds || !g_lists[k]) return;
    g_current = &ev;
    Engine::FireOnActionList(g_lists[k], ev.tag, ev.fromTag, ev.kind == Kind::Construction ? ev.state : 0);
    g_current = nullptr;
}

void OnEntering() { Engine::SetEngineEventsRunning(false); }

void OnEntered(bool) {
    if (!FeatureActive(kFeature)) return;
    if (!g_looked) {  // the on_action database is loaded once per session
        g_looked = true;
        std::string defined;
        for (int k = 0; k < kKinds; ++k) {
            if (g_vanillaOwns[k] || !Engine::HaveEngineEvent((Kind)k)) continue;
            g_lists[k] = Engine::OnActionListByName(kNames[k]);
            if (g_lists[k]) defined += std::string(defined.empty() ? "" : ", ") + kNames[k];
        }
        Log("%s: defined by scripts: %s", kFeature, defined.empty() ? "none (nothing will fire)" : defined.c_str());
    }
    Engine::SetEngineEventsRunning(true);
}

// --- Data commands (main thread, while the event's on_action runs; else false / 0) ------------

const Engine::EngineEvent* Current(Kind a, Kind b = Kind::Count) {
    if (!IsMainThread() || !g_current) return nullptr;
    return g_current->kind == a || g_current->kind == b ? g_current : nullptr;
}

bool EvalBuilding(const Args& a, const Scope&) {
    const Engine::EngineEvent* ev = Current(Kind::Construction);
    return ev && ev->token == a.i(0);
}
bool EvalTechnology(const Args& a, const Scope&) {
    const Engine::EngineEvent* ev = Current(Kind::Technology);
    return ev && ev->token == a.i(0);
}
bool ValueConstructedProvince(const Args&, const Scope&, int64_t* out) {
    const Engine::EngineEvent* ev = Current(Kind::Construction);
    *out = ev ? ev->province : 0;
    return true;
}
bool ValueBattleProvince(const Args&, const Scope&, int64_t* out) {
    const Engine::EngineEvent* ev = Current(Kind::BattleStarted, Kind::BattleWon);
    if (!ev) ev = Current(Kind::BattleLost);
    *out = ev ? ev->province : 0;
    return true;
}

bool EvalDecision(const Args& a, const Scope&) {
    const Engine::EngineEvent* ev = Current(Kind::DecisionTaken);
    return ev && strcmp(ev->name, a.key(0)) == 0;
}
bool ValueCapturedProvince(const Args&, const Scope&, int64_t* out) {
    const Engine::EngineEvent* ev = Current(Kind::ProvinceCaptured);
    *out = ev ? ev->province : 0;
    return true;
}
void TooltipDecision(const Args& a, const Scope&, bool negated, TooltipText& out) {
    out.add(negated ? "The decision taken is NOT " : "The decision taken is ").add(native_localize_key(a.key(0)).c_str());
}

void TooltipBuilding(const Args& a, const Scope&, bool negated, TooltipText& out) {
    out.add(negated ? "The finished building is NOT " : "The finished building is ").add(native_localize_key(a.key(0)).c_str());
}
void TooltipTechnology(const Args& a, const Scope&, bool negated, TooltipText& out) {
    out.add(negated ? "The researched technology is NOT " : "The researched technology is ")
        .add(native_localize_key(a.key(0)).c_str());
}

}  // namespace

bool EngineOnActionsInit() {
    if (!Engine::HaveOnActions()) {
        Log("%s: on_action functions missing", kFeature);
        return false;
    }
    int available = 0;
    for (int k = 0; k < kKinds; ++k) {
        g_vanillaOwns[k] = ExeNames(kNames[k]);
        bool have = Engine::HaveEngineEvent((Kind)k);
        if (g_vanillaOwns[k])
            Log("%s: the game names %s itself; left to the game", kFeature, kNames[k]);
        else if (!have)
            Log("%s: %s unavailable on this build (its engine functions were not found)", kFeature, kNames[k]);
        available += have && !g_vanillaOwns[k];
    }
    if (!available) {
        Log("%s: no event available", kFeature);
        return false;
    }
    if (!Engine::HookEngineEvents(&Sink)) {
        Log("%s: hooks could not be queued", kFeature);
        return false;
    }
    OnGameEntering(&OnEntering);
    OnGameEntered(&OnEntered);

    register_native_trigger({"constructed_building",
                             "In on_construction_finished: true if the finished building is of this type "
                             "(common/buildings name). False elsewhere.",
                             Form::Value, {{"type", ParamType::Token, true}}, ScopeMask::Any, &EvalBuilding, &TooltipBuilding},
                            kFeature);
    register_native_trigger({"researched_technology",
                             "In on_technology_researched: true if the researched technology is this one. False elsewhere.",
                             Form::Value, {{"technology", ParamType::Token, true}}, ScopeMask::Any, &EvalTechnology,
                             &TooltipTechnology},
                            kFeature);
    register_native_numeric_trigger({"constructed_province",
                                     "In on_construction_finished: the province of a finished province building (0 for "
                                     "state buildings and elsewhere). Also a game variable.",
                                     NumericKind::Count, {}, ScopeMask::Country | ScopeMask::State, "value",
                                     &ValueConstructedProvince, nullptr},
                                    kFeature);
    register_native_numeric_trigger({"battle_province",
                                     "In on_battle_started / on_battle_won / on_battle_lost: the battle's province (0 "
                                     "elsewhere). Also a game variable.",
                                     NumericKind::Count, {}, ScopeMask::Country | ScopeMask::State, "value",
                                     &ValueBattleProvince, nullptr},
                                    kFeature);
    register_native_trigger({"taken_decision",
                             "In on_decision_taken: true if the decision taken is this one. False elsewhere.", Form::Value,
                             {{"decision", ParamType::Key, true}}, ScopeMask::Any, &EvalDecision, &TooltipDecision},
                            kFeature);
    register_native_numeric_trigger({"captured_province",
                                     "In on_province_captured: the province (0 elsewhere). Also a game variable.",
                                     NumericKind::Count, {}, ScopeMask::Country | ScopeMask::State, "value",
                                     &ValueCapturedProvince, nullptr},
                                    kFeature);
    for (int k = 0; k < kKinds; ++k)
        if (!g_vanillaOwns[k] && Engine::HaveEngineEvent((Kind)k))
            document_on_action(kNames[k], kFeature, "Fired by the loader (NOTES.md Part Q, MODDING.md \"Engine on_actions\").");
    return true;
}
