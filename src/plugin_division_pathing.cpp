// division_pathing (engine tier): division-scope triggers on the game's own route search
// (NOTES.md Part W).
//
//   has_path_home = no                       # no land route home: stranded (needs a ship, or none)
//   has_land_path = 6521                     # a land route to province 6521 exists
//   needs_naval_transport = home             # home only by sea (or `= 6521`: to that province)
//   travel_time < 10                         # days home, as the move order would go (land, else sea)
//   travel_time = { province = 6521 naval = no days < 10 }   # days to 6521 by land only
//
// "Home" is the owner's capital province, or the first province of its capital state it controls
// (Design A: allies don't count). A division standing in territory its country controls is home
// already (a land route, 0 days). The route and time are the game's own: what the unit tooltip's
// "arrives in N days" computes, with the same search the move order runs. Every trigger is false
// when it can't tell (no division, at sea, no capital), so true under NOT. Read only, no cache:
// deterministic on every machine.
#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "native_script.h"

namespace {

const char* const kFeature = "division_pathing";
std::atomic<bool> g_ok{true};

// The division's route to `toId` (0 = home). *atHome: it stands in territory its country controls.
Engine::Route RouteOf(void* division, int toId, int64_t* time, bool* atHome = nullptr) {
    *time = 0;
    if (atHome) *atHome = false;
    if (!g_ok.load(std::memory_order_relaxed) || !division) return Engine::Route::Unknown;
    int owner = Engine::UnitOwnerTag(division);
    void* here = Engine::DivisionProvince(division);
    if (owner <= 0 || !here) return Engine::Route::Unknown;
    void* to = nullptr;
    if (toId > 0) {
        to = Engine::ProvinceById(toId);
    } else {
        bool sea = false;
        if (Engine::ProvinceIsSea(here, &sea) && !sea &&
            Engine::IsSameCountry(Engine::ProvinceController(Engine::ProvinceIdOf(here)), owner)) {
            if (atHome) *atHome = true;
            return Engine::Route::Land;
        }
        to = Engine::HomeProvince(owner);
    }
    if (!to) return Engine::Route::Unknown;
    return Engine::DivisionRoute(division, to, time);
}

void* Division(const Scope& s) { return Engine::ScopeDivision(s.raw()); }

// Fixed point days from the route time, through whole hours rounded up as the tooltip shows them.
int64_t Days(int64_t time) {
    int64_t hours = time / 100000 + (time % 100000 > 0 ? 1 : 0);
    return hours * 100000 / 24;
}

std::string ProvinceText(int id) {
    std::string key = "VICTORY_POINTS_" + std::to_string(id);
    std::string name = native_localize_key(key.c_str());
    if (!name.empty() && name != key) return name + " (" + std::to_string(id) + ")";
    return "province " + std::to_string(id);
}

std::string HomeText(void* division) {
    int owner = division ? Engine::UnitOwnerTag(division) : 0;
    void* home = owner > 0 ? Engine::HomeProvince(owner) : nullptr;
    return home ? "home (" + ProvinceText(Engine::ProvinceIdOf(home)) + ")" : std::string("home");
}

const char* RouteText(Engine::Route r, bool atHome) {
    if (atHome) return "it stands at home";
    switch (r) {
    case Engine::Route::Land: return "by land";
    case Engine::Route::Naval: return "only by sea";
    case Engine::Route::None: return "no route";
    default: return "unknown: at sea or no capital";
    }
}

}  // namespace

// send_home's `travel_time = yes`: whole days home, rounded up, as travel_time computes them; -1
// when it can't tell or there is no route (or the feature is off), 0 when the division is home.
int DivisionTravelDaysHome(void* division) {
    if (!FeatureActive(kFeature)) return -1;
    int64_t t = 0;
    Engine::Route r = RouteOf(division, 0, &t);
    if (r != Engine::Route::Land && r != Engine::Route::Naval) return -1;
    int64_t d = Days(t);
    return (int)(d / 100000 + (d % 100000 > 0 ? 1 : 0));
}

namespace {

std::string DaysText(int64_t time) { return NativeFormatNumber(NumericKind::Count, Days(time)) + " days"; }

// --- has_path_home = yes/no ----------------------------------------------------------------------
bool PathHomeEval(const Args& a, const Scope& s) {
    int64_t t = 0;
    Engine::Route r = RouteOf(Division(s), 0, &t);
    if (r == Engine::Route::Unknown) return false;
    return (r == Engine::Route::Land) == a.b(0);
}
void PathHomeTooltip(const Args& a, const Scope& s, bool negated, TooltipText& out) {
    bool want = a.b(0) != negated;
    out.add(want ? "Has a land route " : "Has no land route ");
    void* division = Division(s);
    out.add(HomeText(division).c_str());
    if (!division) return;
    int64_t t = 0;
    bool atHome = false;
    Engine::Route r = RouteOf(division, 0, &t, &atHome);
    out.add(" (currently ").add(RouteText(r, atHome)).add(")");
}

// --- has_land_path = <province> ------------------------------------------------------------------
bool LandPathEval(const Args& a, const Scope& s) {
    int64_t t = 0;
    return a.i(0) > 0 && RouteOf(Division(s), a.i(0), &t) == Engine::Route::Land;
}
void LandPathTooltip(const Args& a, const Scope& s, bool negated, TooltipText& out) {
    out.add(negated ? "Has no land route to " : "Has a land route to ").add(ProvinceText(a.i(0)).c_str());
    void* division = Division(s);
    if (!division || a.i(0) <= 0) return;
    int64_t t = 0;
    Engine::Route r = RouteOf(division, a.i(0), &t);
    out.add(" (currently ").add(RouteText(r, false)).add(")");
}

// --- needs_naval_transport = <province> | home -----------------------------------------------------
// "home" -> 0, a province id -> itself, anything else -> -1 (false).
int TargetArg(const Args& a, size_t i) { return IntWordIndex(a.i(i)) == 0 ? 0 : a.i(i) > 0 ? a.i(i) : -1; }
bool NavalEval(const Args& a, const Scope& s) {
    int to = TargetArg(a, 0);
    if (to < 0) return false;
    int64_t t = 0;
    return RouteOf(Division(s), to, &t) == Engine::Route::Naval;
}
void NavalTooltip(const Args& a, const Scope& s, bool negated, TooltipText& out) {
    int to = TargetArg(a, 0);
    void* division = Division(s);
    out.add(negated ? "Does not need naval transport to reach " : "Needs naval transport to reach ")
        .add((to > 0 ? ProvinceText(to) : HomeText(division)).c_str());
    if (!division || to < 0) return;
    int64_t t = 0;
    bool atHome = false;
    Engine::Route r = RouteOf(division, to, &t, &atHome);
    out.add(" (currently ").add(RouteText(r, atHome)).add(")");
}

// --- travel_time (days) -------------------------------------------------------------------------
// Parameters: 0 province, 1 naval.
bool TravelValue(const Args& a, const Scope& s, int64_t* out) {
    int64_t t = 0;
    Engine::Route r = RouteOf(Division(s), a.has(0) ? a.i(0) : 0, &t);
    bool naval = !a.has(1) || a.b(1);
    if (r != Engine::Route::Land && !(naval && r == Engine::Route::Naval)) return false;
    *out = Days(t);
    return true;
}
void TravelSubject(const Args& a, const Scope& s, TooltipText& out) {
    void* division = Division(s);
    out.add("Travel time ");
    if (a.has(1) && !a.b(1)) out.add("by land ");
    out.add("to ").add((a.has(0) ? ProvinceText(a.i(0)) : HomeText(division)).c_str()).add(" in days");
    if (!division) {
        out.markScopeMissing();
        return;
    }
    int64_t t = 0;
    bool atHome = false;
    Engine::Route r = RouteOf(division, a.has(0) ? a.i(0) : 0, &t, &atHome);
    if (r == Engine::Route::Naval) out.add(" (by sea)");
}

// --- Self-check on every game entry (main thread) ----------------------------------------------
struct CheckResult {
    int sea = 0, provinces = 0, routed = 0, land = 0, naval = 0, none = 0;
    int home = 0;
    const char* why = "";
};

// No C++ objects here (SEH): the division list comes from the caller.
bool SafeCheck(int tag, void* const* units, size_t count, CheckResult* c) {
    __try {
        int n = Engine::ProvinceCount();
        for (int id = 1; id < n; ++id) {
            void* p = Engine::ProvinceById(id);
            bool sea = false;
            if (!p) continue;
            if (!Engine::ProvinceIsSea(p, &sea)) {
                c->why = "a province's sea test is unreadable";
                return false;
            }
            ++c->provinces;
            c->sea += sea;
        }
        if (c->provinces == 0 || c->sea == 0 || c->sea == c->provinces) {
            c->why = "the sea test does not split the map into land and sea";
            return false;
        }
        void* home = Engine::HomeProvince(tag);
        if (!home) return true;  // no capital the player controls: nothing to route to
        bool sea = true;
        c->home = Engine::ProvinceIdOf(home);
        if (!Engine::ProvinceIsSea(home, &sea) || sea) {
            c->why = "the capital is a sea province";
            return false;
        }
        for (size_t i = 0; i < count && c->routed < 8; ++i) {
            void* unit = units[i];
            void* here = Engine::DivisionProvince(unit);
            if (!here || here == home || !Engine::ProvinceIsSea(here, &sea) || sea) continue;
            int64_t t = 0;
            int steps = 0, last = 0;
            Engine::Route r = Engine::DivisionRoute(unit, home, &t, &steps, &last);
            ++c->routed;
            if (r == Engine::Route::Unknown) {
                c->why = "a route has a province id that does not exist";
                return false;
            }
            if (r == Engine::Route::None) {
                ++c->none;
                continue;
            }
            if (last != c->home || t <= 0 || t > 100000LL * 24 * 3650) {
                c->why = "a route does not end at its target or its time is out of range";
                return false;
            }
            r == Engine::Route::Land ? ++c->land : ++c->naval;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        c->why = "crashed";
        return false;
    }
}

void OnEntered(bool) {
    int tag = Engine::PlayerCountryTag();
    if (tag <= 0) return;  // observer: nothing to check against
    CheckResult c;
    std::vector<void*> units;
    Engine::CountryDivisionList(tag, &units);  // none readable: only the sea test is checked
    if (!SafeCheck(tag, units.data(), units.size(), &c)) {
        g_ok.store(false);
        Log("division_pathing: self-check failed for %s (%s)", Engine::CountryTagName(tag).c_str(), c.why);
        FeatureDeactivate(kFeature, "self-check failed: routes or the sea test unreadable");
        return;
    }
    if (!g_ok.load()) return;  // switched off earlier this session: stays off
    Log("division_pathing: self-check passed (%d provinces, %d sea; %s home province %d: %d division(s) routed, %d by land, "
        "%d by sea, %d without a route)",
        c.provinces, c.sea, Engine::CountryTagName(tag).c_str(), c.home, c.routed, c.land, c.naval, c.none);
}

}  // namespace

bool DivisionPathingInit() {
    if (!Engine::HaveDivisionPathing() || !Engine::HaveNumeric()) {
        Log("division_pathing: required engine functions missing (pathing %d, numeric triggers %d)", Engine::HaveDivisionPathing(),
            Engine::HaveNumeric());
        return false;
    }
    static const char* const kHome =
        " Home is the division's country's capital province, or the first province of its capital state the country "
        "controls; allies' territory does not count. A division standing in territory its country controls is home. The "
        "route is the game's own search, as a move order goes (land first, else with naval transport). False if it can't "
        "tell (not a land division, at sea, no capital), so true under NOT.";
    static const std::string kPathHomeHelp =
        std::string("Division scope. `has_path_home = yes`: the division can walk home by land; `= no`: it can't (it needs a "
                    "ship, or there is no route at all: stranded).") + kHome;
    static const std::string kNavalHelp =
        std::string("Division scope. `needs_naval_transport = home` or `= <province id>`: there is no land route there, but "
                    "one with naval transport.") + kHome;
    static const std::string kTravelHelp =
        std::string("Division scope. Days the division would travel, as the unit tooltip's arrival time (whole hours, "
                    "rounded up): `travel_time < 10` home, or `travel_time = { province = <id> days < 10 }`. `naval = no`: "
                    "land routes only. No route: false. One route search per evaluation: filter with cheap triggers first "
                    "and don't evaluate it for every division every day.") + kHome;
    bool ok = register_native_trigger({"has_path_home", kPathHomeHelp.c_str(), Form::Value, {{"path", ParamType::Bool, true}},
                                       ScopeMask::Unit, &PathHomeEval, &PathHomeTooltip},
                                      kFeature);
    ok = register_native_trigger({"has_land_path",
                                  "Division scope. `has_land_path = <province id>`: the division can walk there by land (the "
                                  "game's own search, as a move order). False if not, or if it can't tell (at sea, no such "
                                  "province).",
                                  Form::Value, {{"province", ParamType::Int, true}}, ScopeMask::Unit, &LandPathEval,
                                  &LandPathTooltip},
                                 kFeature) &&
         ok;
    ok = register_native_trigger({"needs_naval_transport", kNavalHelp.c_str(), Form::Value,
                                  {{"target", ParamType::Int, true, ScopeDefault::None, "home"}}, ScopeMask::Unit, &NavalEval,
                                  &NavalTooltip},
                                 kFeature) &&
         ok;
    ok = register_native_numeric_trigger({"travel_time", kTravelHelp.c_str(), NumericKind::Count,
                                          {{"province", ParamType::Int, false}, {"naval", ParamType::Bool, false}},
                                          ScopeMask::Unit, "days", &TravelValue, &TravelSubject},
                                         kFeature) &&
         ok;
    OnGameEntered(&OnEntered);
    return ok;
}
