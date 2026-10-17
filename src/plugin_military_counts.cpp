// military_counts (engine tier): numeric count triggers for battles and forces in an area.
//
//   battles_count = { state = 15 count > 2 }          battles there (located there, or fought by
//                                                      divisions standing there)
//   battles_count = { region = 7 location = only count > 0 }   only battles located there
//   battles_count = { involving = ROOT count > 0 }    battles the country's divisions fight in
//   divisions_count = { region = 42 enemy_of = ROOT count > 5 }
//   ships_count = { region = 18 owner = ENG count < 3 }
//   16 = { divisions_count = { owner = ROOT count > 0 } }     the state from the scope
//   planes_count = { region = 42 owner = ROOT count > 100 }   planes based there
//
// Without state / region (and involving) the scope decides: a state scope, a strategic region
// scope (or for battles_count a country scope = involving). Unit filters, all optional and
// combined: owner = <country>/any, enemy_of = <country> (at war with it), allied_with = <country>
// (its allies, itself excluded). planes_count needs a country (owner, else ROOT, else the scope's
// country); its enemy_of / allied_with are reserved until the data supports them.
#include <atomic>
#include <stdint.h>
#include <string>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "native_script.h"

namespace {

const char* const kFeature = "military_counts";
std::atomic<bool> g_ok{true};

// The area: explicit state (param 0) or region (param 1), else the scope's state or region.
bool Area(const Args& a, const Scope& s, Engine::AreaKind* kind, int* id) {
    if (a.has(0)) *kind = Engine::AreaKind::State, *id = a.i(0);
    else if (a.has(1)) *kind = Engine::AreaKind::StrategicRegion, *id = a.i(1);
    else if (s.stateId() > 0) *kind = Engine::AreaKind::State, *id = s.stateId();
    else if (s.strategicRegionId() > 0) *kind = Engine::AreaKind::StrategicRegion, *id = s.strategicRegionId();
    else return false;
    return true;
}

std::string AreaText(Engine::AreaKind kind, int id) {
    if (kind == Engine::AreaKind::State) return "state " + std::to_string(id);
    return Engine::RegionName(id) + " (" + std::to_string(id) + ")";
}

// Country arguments arrive resolved (ResolveArgs); `any` stays kCountryAny and means no filter.
int CountryArg(const Args& a, size_t i) {
    if (!a.has(i)) return 0;
    int v = a.i(i);
    return v == kCountryAny ? 0 : v;
}

// --- battles_count: 0 state, 1 region, 2 involving, 3 location (any / only) ----------------------
bool LocationOnly(const Args& a) { return a.has(3) && a.i(3) == 1; }

bool BattlesValue(const Args& a, const Scope& s, int64_t* out) {
    if (!g_ok.load(std::memory_order_relaxed)) return false;
    Engine::AreaKind kind;
    int id;
    if (a.has(0) || a.has(1)) return Area(a, s, &kind, &id) && Engine::CountBattles(kind, id, LocationOnly(a), out);
    if (a.has(2)) return Engine::CountBattlesInvolving(a.i(2), out);
    if (Area(a, s, &kind, &id)) return Engine::CountBattles(kind, id, LocationOnly(a), out);
    return s.countryTag() > 0 && Engine::CountBattlesInvolving(s.countryTag(), out);
}
void BattlesSubject(const Args& a, const Scope& s, TooltipText& out) {
    Engine::AreaKind kind;
    int id;
    if (!a.has(0) && !a.has(1) && (a.has(2) || (s.stateId() <= 0 && s.strategicRegionId() <= 0))) {
        int tag = a.has(2) ? a.i(2) : s.countryTag();
        out.add("Battles involving ").add(Engine::CountryTagName(tag).c_str());
    } else if (Area(a, s, &kind, &id)) {
        out.add(LocationOnly(a) ? "Battles located in " : "Battles in or fought from ").add(AreaText(kind, id).c_str());
    } else {
        out.add("Battles");
        out.markScopeMissing();
    }
}

// --- divisions_count / ships_count: 0 state, 1 region, 2 owner, 3 enemy_of, 4 allied_with --------
Engine::UnitFilter Filter(const Args& a) {
    Engine::UnitFilter f;
    f.owner = CountryArg(a, 2);
    f.enemyOf = CountryArg(a, 3);
    f.alliedWith = CountryArg(a, 4);
    return f;
}

bool UnitsValue(bool ships, const Args& a, const Scope& s, int64_t* out) {
    if (!g_ok.load(std::memory_order_relaxed)) return false;
    Engine::AreaKind kind;
    int id;
    if (!Area(a, s, &kind, &id)) return false;
    Engine::UnitFilter f = Filter(a);
    return ships ? Engine::CountShips(kind, id, f, out) : Engine::CountDivisions(kind, id, f, out);
}
bool DivisionsValue(const Args& a, const Scope& s, int64_t* out) { return UnitsValue(false, a, s, out); }
bool ShipsValue(const Args& a, const Scope& s, int64_t* out) { return UnitsValue(true, a, s, out); }

void UnitsSubject(const char* what, const Args& a, const Scope& s, TooltipText& out) {
    Engine::AreaKind kind;
    int id;
    Engine::UnitFilter f = Filter(a);
    out.add(what);
    if (f.owner) out.add(" of ").add(Engine::CountryTagName(f.owner).c_str());
    if (f.enemyOf) out.add(" at war with ").add(Engine::CountryTagName(f.enemyOf).c_str());
    if (f.alliedWith) out.add(" allied with ").add(Engine::CountryTagName(f.alliedWith).c_str());
    if (Area(a, s, &kind, &id)) out.add(" in ").add(AreaText(kind, id).c_str());
    else out.markScopeMissing();  // no area here (e.g. under an iterator's header)
}
void DivisionsSubject(const Args& a, const Scope& s, TooltipText& out) { UnitsSubject("Divisions", a, s, out); }
void ShipsSubject(const Args& a, const Scope& s, TooltipText& out) { UnitsSubject("Ships", a, s, out); }

// --- planes_count: 0 region, 1 owner (default ROOT / scope), 2 enemy_of, 3 allied_with (reserved)
bool PlanesValue(const Args& a, const Scope& s, int64_t* out) {
    if (!g_ok.load(std::memory_order_relaxed)) return false;
    int region = a.has(0) ? a.i(0) : s.strategicRegionId();
    int tag = CountryArg(a, 1);
    return region > 0 && tag > 0 && Engine::CountPlanesStationed(region, tag, out);
}
void PlanesSubject(const Args& a, const Scope& s, TooltipText& out) {
    int region = a.has(0) ? a.i(0) : s.strategicRegionId();
    out.add("Planes of ").add(Engine::CountryTagName(CountryArg(a, 1)).c_str());
    if (region > 0) out.add(" based in ").add(Engine::RegionName(region).c_str()).add(" (").add(region).add(")");
    else out.markScopeMissing();
}

// --- Self-check on every game entry (main thread) ----------------------------------------------
bool SafeCheck(int tag, int* states, int64_t* divisions, int64_t* battles) {
    __try {
        Engine::UnitFilter all;
        for (int id = 1; id < 100000; ++id) {
            if (!Engine::StateById(id)) {
                if (id > 1) break;
                return false;
            }
            int64_t n = 0;
            if (!Engine::CountDivisions(Engine::AreaKind::State, id, all, &n) || n < 0 || n > 100000) return false;
            *divisions += n;
            if (!Engine::CountBattles(Engine::AreaKind::State, id, true, &n) || n < 0) return false;
            *battles += n;
            if (!Engine::CountBattles(Engine::AreaKind::State, id, false, &n) || n < 0) return false;
            ++*states;
        }
        int64_t own = 0;
        return Engine::CountBattlesInvolving(tag, &own) && own >= 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void OnEntered(bool) {
    int tag = Engine::PlayerCountryTag();
    if (tag <= 0) return;
    int states = 0;
    int64_t divisions = 0, battles = 0;
    if (!SafeCheck(tag, &states, &divisions, &battles)) {
        g_ok.store(false);
        FeatureDeactivate(kFeature, "self-check failed: state/province data unreadable");
        return;
    }
    g_ok.store(true);
    // Literal tags in script (owner = GER) are parsed with the engine's tag database: the player's
    // tag must come back as the same country.
    std::string name = Engine::CountryTagName(tag);
    int parsed = Engine::TagFromText(name.c_str());
    Log("military_counts: self-check passed (%d states, %lld divisions on the map, %lld battles; tag text %s -> %s)",
        states, (long long)divisions, (long long)battles, name.c_str(),
        parsed == tag ? "same country" : parsed > 0 ? "ANOTHER tag (literal tags unreliable)" : "not found");
}

}  // namespace

bool MilitaryCountsInit() {
    if (!Engine::HaveMilitaryCounts() || !Engine::HaveNumeric()) {
        Log("military_counts: required engine functions missing (data %d, numeric triggers %d)", Engine::HaveMilitaryCounts(),
            Engine::HaveNumeric());
        return false;
    }
    // allied_with needs the engine's alliance test (resolved with region_control's signatures).
    bool alliance = Engine::HaveAlliance();
    if (!alliance) Log("military_counts: alliance test not found; allied_with is reserved (a parse error)");
    const std::vector<ParamSpec> unitParams = {
        {"state", ParamType::Int, false},
        {"region", ParamType::Int, false},
        {"owner", ParamType::Country, false, ScopeDefault::None, "any"},
        {"enemy_of", ParamType::Country, false},
        {"allied_with", ParamType::Country, false, ScopeDefault::None, nullptr, !alliance},
    };
    // Help texts must outlive registration (the command keeps the pointer).
    static const std::string kFilters =
        " Unit filters by owner, all optional and combined: `owner` = that country (any: no filter, the default), "
        "`enemy_of` = at war with that country, `allied_with` = allied with that country (allies as naval control "
        "counts them; the country itself is not included).";
    static const std::string kDivisionsHelp =
        "Number of divisions in a state or strategic region (`state` / `region`, else the scope's)." + kFilters;
    static const std::string kShipsHelp =
        "Number of ships in a strategic (sea) region or state (`region` / `state`, else the scope's)." + kFilters;
    register_native_numeric_trigger(
        {"battles_count",
         "Number of battles (land and naval) in a state or strategic region: located there, or fought by divisions "
         "standing there, each battle once; `location = only` counts only the battles located there. "
         "`involving`: the battles a country's divisions fight in. Give one of `state`, `region`, `involving`; "
         "without them the scope decides (state, strategic region, or country = involving).",
         NumericKind::Count,
         {{"state", ParamType::Int, false},
          {"region", ParamType::Int, false},
          {"involving", ParamType::Country, false},
          {"location", ParamType::Choice, false, ScopeDefault::None, "any|only"}},
         ScopeMask::Any, "count", &BattlesValue, &BattlesSubject},
        kFeature);
    register_native_numeric_trigger({"divisions_count", kDivisionsHelp.c_str(), NumericKind::Count, unitParams, ScopeMask::Any,
                                     "count", &DivisionsValue, &DivisionsSubject},
                                    kFeature);
    register_native_numeric_trigger({"ships_count", kShipsHelp.c_str(), NumericKind::Count, unitParams, ScopeMask::Any, "count",
                                     &ShipsValue, &ShipsSubject},
                                    kFeature);
    if (Engine::HavePlanes())
        register_native_numeric_trigger(
            {"planes_count",
             "Number of a country's planes based (by air base) in a strategic region (`region`, else the scope's). "
             "`owner`: the country, default ROOT, else the country scope; any is not supported. `enemy_of` and "
             "`allied_with` are reserved until the data supports them.",
             NumericKind::Count,
             {{"region", ParamType::Int, false},
              {"owner", ParamType::Country, false, ScopeDefault::RootCountry},
              {"enemy_of", ParamType::Country, false, ScopeDefault::None, nullptr, true},
              {"allied_with", ParamType::Country, false, ScopeDefault::None, nullptr, true}},
             ScopeMask::Any, "count", &PlanesValue, &PlanesSubject},
            kFeature);
    else
        Log("military_counts: plane data not found; planes_count not registered");
    OnGameEntered(&OnEntered);
    return true;
}
