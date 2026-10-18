// army_condition (engine tier): average strength / organisation of divisions.
//
//   avg_division_strength < 0.7                       country scope: the whole army
//   avg_division_organisation = { state = 15 country = GER value < 0.5 }   divisions in a state
//
// Country scope (or `country` without a state): every division of the country. State scope (or
// `state`): the divisions in that state, owned by `country` if given, else all. The average of
// the divisions' ratios as the division UI shows them (current / max, 0..1). No divisions: the
// value does not exist and the trigger is false. Cost grows with the number of divisions
// (NOTES.md E2 measurements).
#include <atomic>
#include <stdint.h>
#include <string>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "native_script.h"

namespace {

const char* const kFeature = "army_condition";
std::atomic<bool> g_ok{true};

struct Sum {
    bool organisation;
    int filterTag;  // 0 = any owner
    int64_t total = 0;
    int64_t n = 0;
};

void AddDivision(void* unit, Sum* s) {
    int64_t strength = 0, org = 0;
    if (!Engine::DivisionRatios(unit, &strength, &org)) return;
    s->total += s->organisation ? org : strength;
    ++s->n;
}
void AddCountryDivision(void* unit, void* ctx) { AddDivision(unit, (Sum*)ctx); }
void AddAreaDivision(void* unit, int owner, void* ctx) {
    auto* s = (Sum*)ctx;
    if (!s->filterTag || Engine::IsSameCountry(owner, s->filterTag)) AddDivision(unit, s);
}

// Parameters: 0 state, 1 country.
bool Average(bool organisation, const Args& a, const Scope& sc, int64_t* out) {
    if (!g_ok.load(std::memory_order_relaxed)) return false;
    Sum s{organisation, 0};
    int state = a.has(0) ? a.i(0) : (a.has(1) ? 0 : sc.stateId());
    bool ok;
    if (state > 0) {
        s.filterTag = a.has(1) ? a.i(1) : 0;
        ok = Engine::ForEachAreaDivision(Engine::AreaKind::State, state, &AddAreaDivision, &s);
    } else {
        int tag = a.has(1) ? a.i(1) : sc.countryTag();
        ok = tag > 0 && Engine::ForEachDivision(tag, &AddCountryDivision, &s);
    }
    if (!ok || s.n == 0) return false;
    *out = s.total / s.n;
    return true;
}
bool StrengthValue(const Args& a, const Scope& s, int64_t* out) { return Average(false, a, s, out); }
bool OrgValue(const Args& a, const Scope& s, int64_t* out) { return Average(true, a, s, out); }

void Subject(const char* what, const Args& a, const Scope& sc, TooltipText& out) {
    out.add("Average ").add(what).add(" of ");
    int state = a.has(0) ? a.i(0) : (a.has(1) ? 0 : sc.stateId());
    if (state > 0) {
        out.add("divisions in state ").add(state);
        if (a.has(1)) out.add(" owned by ").add(Engine::CountryTagName(a.i(1)).c_str());
    } else {
        out.add(Engine::CountryTagName(a.has(1) ? a.i(1) : sc.countryTag()).c_str()).add("'s divisions");
    }
}
void StrengthSubject(const Args& a, const Scope& s, TooltipText& out) { Subject("strength", a, s, out); }
void OrgSubject(const Args& a, const Scope& s, TooltipText& out) { Subject("organisation", a, s, out); }

// Self-check (main thread, every game entry): the player's divisions must all be CArmy objects
// with ratios in 0..1.
bool SafeCheck(int tag, int64_t* divisions, int64_t* avgStrength) {
    __try {
        Sum s{false, 0};
        if (!Engine::ForEachDivision(tag, &AddCountryDivision, &s)) return false;
        *divisions = s.n;
        *avgStrength = s.n ? s.total / s.n : 0;
        return *avgStrength >= 0 && *avgStrength <= 100000;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void OnEntered(bool) {
    int tag = Engine::PlayerCountryTag();
    if (tag <= 0) return;
    int64_t n = 0, avg = 0;
    if (!SafeCheck(tag, &n, &avg)) {
        g_ok.store(false);
        FeatureDeactivate(kFeature, "self-check failed: division data unreadable or out of range");
        return;
    }
    g_ok.store(true);
    Log("army_condition: self-check passed (%s: %lld divisions, average strength %s)", Engine::CountryTagName(tag).c_str(),
        (long long)n, NativeFormatNumber(NumericKind::Ratio, avg).c_str());
}

}  // namespace

bool ArmyConditionInit() {
    if (!Engine::HaveDivisionCondition() || !Engine::HaveNumeric()) {
        Log("army_condition: required engine functions missing (data %d, numeric triggers %d)", Engine::HaveDivisionCondition(),
            Engine::HaveNumeric());
        return false;
    }
    const std::vector<ParamSpec> params = {{"state", ParamType::Int, false}, {"country", ParamType::Country, false}};
    const char* kWhere =
        " In a country scope (or with only `country`): all of the country's divisions. In a state scope (or with "
        "`state`): the divisions in that state, owned by `country` if given, else all. No divisions: false. "
        "Loops over every division it counts: avoid it in often-evaluated AI blocks (see MODDING.md, \"Data triggers\", Cost).";
    static const std::string kStrengthHelp =
        std::string("Average strength of divisions, 0 to 1: the fighting strength the division UI shows (manpower ratio times equipment ratio).") + kWhere;
    static const std::string kOrgHelp =
        std::string("Average organisation of divisions, 0 to 1 (current / max organisation).") + kWhere;
    register_native_numeric_trigger({"avg_division_strength", kStrengthHelp.c_str(), NumericKind::Ratio, params,
                                     ScopeMask::Country | ScopeMask::State, "value", &StrengthValue, &StrengthSubject},
                                    kFeature);
    register_native_numeric_trigger({"avg_division_organisation", kOrgHelp.c_str(), NumericKind::Ratio, params,
                                     ScopeMask::Country | ScopeMask::State, "value", &OrgValue, &OrgSubject},
                                    kFeature);
    OnGameEntered(&OnEntered);
    return true;
}
