// military_counts (engine tier): numeric count triggers for battles and forces in an area.
//
//   battles_count = { state = 15 count > 2 }        battles located in a state / strategic region
//   battles_count = { involving = ROOT count > 0 }  battles the country's divisions fight in
//   divisions_count = { region = 42 owner = enemy count > 5 }
//   ships_count = { region = 42 owner = own count < 3 }
//   planes_count = { region = 42 owner = own count > 100 }    (stationed there; own only for now)
//
// Without state / region (and involving) the scope decides: a state scope, a strategic region
// scope (or for battles_count a country scope = involving). `owner` is any (default), own (the
// country) or enemy (at war with it); the country is `country`, else ROOT, else the country scope.
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

// The country an owner filter is relative to: `country`, else ROOT, else the scope's country.
int FilterCountry(const Args& a, size_t countryParam, const Scope& s) {
    if (a.has(countryParam)) return a.i(countryParam);
    Scope r = s.root();
    int tag = r.valid() ? r.countryTag() : 0;
    return tag > 0 ? tag : s.countryTag();
}

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

const char* OwnerText(int owner) { return owner == 1 ? "own " : owner == 2 ? "enemy " : ""; }

// --- battles_count: 0 state, 1 region, 2 involving ---------------------------------------------
bool BattlesValue(const Args& a, const Scope& s, int64_t* out) {
    if (!g_ok.load(std::memory_order_relaxed)) return false;
    Engine::AreaKind kind;
    int id;
    if (a.has(0) || a.has(1)) return Area(a, s, &kind, &id) && Engine::CountBattles(kind, id, out);
    if (a.has(2)) return Engine::CountBattlesInvolving(a.i(2), out);
    if (Area(a, s, &kind, &id)) return Engine::CountBattles(kind, id, out);
    return s.countryTag() > 0 && Engine::CountBattlesInvolving(s.countryTag(), out);
}
void BattlesSubject(const Args& a, const Scope& s, TooltipText& out) {
    Engine::AreaKind kind;
    int id;
    if (!a.has(0) && !a.has(1) && (a.has(2) || (s.stateId() <= 0 && s.strategicRegionId() <= 0))) {
        int tag = a.has(2) ? a.i(2) : s.countryTag();
        out.add("Battles involving ").add(Engine::CountryTagName(tag).c_str());
    } else if (Area(a, s, &kind, &id)) {
        out.add("Battles in ").add(AreaText(kind, id).c_str());
    } else {
        out.add("Battles");
    }
}

// --- divisions_count / ships_count: 0 state, 1 region, 2 owner, 3 country -----------------------
bool UnitsValue(bool ships, const Args& a, const Scope& s, int64_t* out) {
    if (!g_ok.load(std::memory_order_relaxed)) return false;
    Engine::AreaKind kind;
    int id;
    if (!Area(a, s, &kind, &id)) return false;
    auto who = (Engine::UnitOwner)(a.has(2) ? a.i(2) : 0);
    int tag = who == Engine::UnitOwner::Any ? 0 : FilterCountry(a, 3, s);
    if (who != Engine::UnitOwner::Any && tag <= 0) return false;
    return ships ? Engine::CountShips(kind, id, tag, who, out) : Engine::CountDivisions(kind, id, tag, who, out);
}
bool DivisionsValue(const Args& a, const Scope& s, int64_t* out) { return UnitsValue(false, a, s, out); }
bool ShipsValue(const Args& a, const Scope& s, int64_t* out) { return UnitsValue(true, a, s, out); }
void UnitsSubject(const char* what, const Args& a, const Scope& s, TooltipText& out) {
    Engine::AreaKind kind;
    int id;
    int owner = a.has(2) ? a.i(2) : 0;
    out.add(OwnerText(owner)).add(what);
    if (owner) out.add(" (").add(Engine::CountryTagName(FilterCountry(a, 3, s)).c_str()).add(")");
    if (Area(a, s, &kind, &id)) out.add(" in ").add(AreaText(kind, id).c_str());
}
void DivisionsSubject(const Args& a, const Scope& s, TooltipText& out) { UnitsSubject("Divisions", a, s, out); }
void ShipsSubject(const Args& a, const Scope& s, TooltipText& out) { UnitsSubject("Ships", a, s, out); }

// --- planes_count: 0 region, 1 owner (own only), 2 country -------------------------------------
bool PlanesValue(const Args& a, const Scope& s, int64_t* out) {
    if (!g_ok.load(std::memory_order_relaxed)) return false;
    int region = a.has(0) ? a.i(0) : s.strategicRegionId();
    int tag = FilterCountry(a, 2, s);
    return region > 0 && tag > 0 && Engine::CountPlanesStationed(region, tag, out);
}
void PlanesSubject(const Args& a, const Scope& s, TooltipText& out) {
    int region = a.has(0) ? a.i(0) : s.strategicRegionId();
    out.add("Own planes (").add(Engine::CountryTagName(FilterCountry(a, 2, s)).c_str()).add(") stationed in ");
    out.add(Engine::RegionName(region).c_str()).add(" (").add(region).add(")");
}

// --- Self-check on every game entry (main thread) ----------------------------------------------
bool SafeCheck(int tag, int* states, int64_t* divisions, int64_t* battles) {
    __try {
        for (int id = 1; id < 100000; ++id) {
            if (!Engine::StateById(id)) {
                if (id > 1) break;
                return false;
            }
            int64_t n = 0;
            if (!Engine::CountDivisions(Engine::AreaKind::State, id, 0, Engine::UnitOwner::Any, &n) || n < 0 || n > 100000)
                return false;
            *divisions += n;
            if (!Engine::CountBattles(Engine::AreaKind::State, id, &n) || n < 0) return false;
            *battles += n;
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
    Log("military_counts: self-check passed (%d states, %lld divisions on the map, %lld battles)", states,
        (long long)divisions, (long long)battles);
}

}  // namespace

bool MilitaryCountsInit() {
    if (!Engine::HaveMilitaryCounts() || !Engine::HaveNumeric()) {
        Log("military_counts: required engine functions missing (data %d, numeric triggers %d)", Engine::HaveMilitaryCounts(),
            Engine::HaveNumeric());
        return false;
    }
    const char* kOwners = "any|own|enemy";
    register_native_numeric_trigger(
        {"battles_count",
         "Number of battles (land and naval) located in a state or strategic region, or the number of battles "
         "a country's divisions fight in (`involving`). Give one of `state`, `region`, `involving`; without "
         "them the scope decides (state, strategic region, or country = involving).",
         NumericKind::Count,
         {{"state", ParamType::Int, false}, {"region", ParamType::Int, false}, {"involving", ParamType::Country, false}},
         ScopeMask::Any, "count", &BattlesValue, &BattlesSubject},
        kFeature);
    register_native_numeric_trigger(
        {"divisions_count",
         "Number of divisions in a state or strategic region (`state` / `region`, else the scope's). `owner`: "
         "any (default), own (belonging to `country`) or enemy (at war with `country`); `country` defaults to "
         "ROOT, else the country scope.",
         NumericKind::Count,
         {{"state", ParamType::Int, false},
          {"region", ParamType::Int, false},
          {"owner", ParamType::Choice, false, ScopeDefault::None, kOwners},
          {"country", ParamType::Country, false}},
         ScopeMask::Any, "count", &DivisionsValue, &DivisionsSubject},
        kFeature);
    register_native_numeric_trigger(
        {"ships_count",
         "Number of ships in a strategic (sea) region or state (`region` / `state`, else the scope's). `owner` and "
         "`country` as for divisions_count.",
         NumericKind::Count,
         {{"state", ParamType::Int, false},
          {"region", ParamType::Int, false},
          {"owner", ParamType::Choice, false, ScopeDefault::None, kOwners},
          {"country", ParamType::Country, false}},
         ScopeMask::Any, "count", &ShipsValue, &ShipsSubject},
        kFeature);
    if (Engine::HavePlanes())
        register_native_numeric_trigger(
            {"planes_count",
             "Number of a country's planes stationed (by air base) in a strategic region (`region`, else the "
             "scope's). `owner` accepts only own for now (any/enemy are reserved); `country` defaults to ROOT, "
             "else the country scope.",
             NumericKind::Count,
             {{"region", ParamType::Int, false},
              {"owner", ParamType::Choice, false, ScopeDefault::None, "own"},
              {"country", ParamType::Country, false}},
             ScopeMask::Any, "count", &PlanesValue, &PlanesSubject},
            kFeature);
    else
        Log("military_counts: plane data not found; planes_count not registered");
    OnGameEntered(&OnEntered);
    return true;
}
