// region_control (engine tier): numeric data triggers for control of a strategic region.
//
//   air_superiority > 0.6                                  (in a strategic region scope; ROOT's side)
//   air_superiority = { region = 42 country = GER value > 0.6 }
//   naval_dominance = { region = 42 value < 0.3 }
//
// air_superiority: the region panel's bar, our / (our + enemy) air power (0 when neither side has
// any). naval_dominance: friendly (own + allied) / (friendly + enemy) naval dominance, the sums
// vanilla's has_naval_control compares. Both read values the simulation keeps (NOTES.md E1).
#include <atomic>
#include <stdint.h>
#include <string>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "native_script.h"

namespace {

const char* const kFeature = "region_control";
std::atomic<bool> g_ok{true};  // false after a failed self-check: triggers are false

// a / (a + b) in fixed point, clamped to 0..1; 0 when both are 0.
int64_t Share(int64_t a, int64_t b) {
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    while (a > INT64_MAX / 200000 || b > INT64_MAX / 200000) {  // keep a * 100000 and a + b in range
        a /= 2;
        b /= 2;
    }
    if (a + b <= 0) return 0;
    int64_t r = a * 100000 / (a + b);
    return r < 0 ? 0 : r > 100000 ? 100000 : r;
}

// Parameters: 0 region, 1 country.
bool AirValue(const Args& a, const Scope&, int64_t* out) {
    int64_t our = 0, enemy = 0;
    if (!g_ok.load(std::memory_order_relaxed) || !Engine::AirPower(a.i(1), a.i(0), &our, &enemy)) return false;
    *out = Share(our, enemy);
    return true;
}

bool NavalValue(const Args& a, const Scope&, int64_t* out) {
    int64_t friendly = 0, enemy = 0;
    if (!g_ok.load(std::memory_order_relaxed) || !Engine::NavalDominance(a.i(1), a.i(0), &friendly, &enemy)) return false;
    *out = Share(friendly, enemy);
    return true;
}

void Subject(const char* what, const Args& a, TooltipText& out) {
    out.add(what).add(" of ").add(Engine::CountryTagName(a.i(1)).c_str()).add(" over ");
    out.add(Engine::RegionName(a.i(0)).c_str()).add(" (").add(a.i(0)).add(")");
}
void AirSubject(const Args& a, const Scope&, TooltipText& out) { Subject("Air superiority", a, out); }
void NavalSubject(const Args& a, const Scope&, TooltipText& out) { Subject("Naval dominance", a, out); }

// Main thread, every game entry: the data must be readable for every region without faults.
bool SafeCheck(int tag, int regions, int* airRegions, int* navalRegions) {
    __try {
        for (int id = 1; id < regions; ++id) {
            int64_t a = 0, b = 0;
            if (Engine::AirPower(tag, id, &a, &b) && (a < 0 || b < 0)) return false;
            if (a + b > 0) ++*airRegions;
            if (!Engine::NavalDominance(tag, id, &a, &b) || a < 0 || b < 0) return false;
            if (a + b > 0) ++*navalRegions;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void OnEntered(bool) {
    int tag = Engine::PlayerCountryTag();
    if (tag <= 0) return;  // observer: nothing to check against
    int air = 0, naval = 0, regions = Engine::RegionCount();
    if (!SafeCheck(tag, regions, &air, &naval)) {
        g_ok.store(false);
        FeatureDeactivate(kFeature, "self-check failed: region data unreadable or negative");
        return;
    }
    g_ok.store(true);
    Log("region_control: self-check passed (%d regions; %s: air data in %d, naval data in %d)", regions - 1,
        Engine::CountryTagName(tag).c_str(), air, naval);
}

}  // namespace

bool RegionControlInit() {
    if (!Engine::HaveRegionControl() || !Engine::HaveNumeric()) {
        Log("region_control: required engine functions missing (data %d, numeric triggers %d)", Engine::HaveRegionControl(),
            Engine::HaveNumeric());
        return false;
    }
    const std::vector<ParamSpec> params = {{"region", ParamType::Int, false, ScopeDefault::StrategicRegion},
                                           {"country", ParamType::Country, false, ScopeDefault::RootCountry}};
    register_native_numeric_trigger(
        {"air_superiority",
         "Air superiority of a country over a strategic region, 0 to 1, as the region panel's bar: its side's "
         "air power / (its side + enemies). 0 when neither side has planes there. `region` defaults to the "
         "strategic region scope, `country` to ROOT (or the country scope).",
         NumericKind::Ratio, params, ScopeMask::Any, "value", &AirValue, &AirSubject},
        kFeature);
    register_native_numeric_trigger(
        {"naval_dominance",
         "Naval dominance of a country in a strategic region, 0 to 1: dominance of the country and its friends / "
         "(friends + countries at war with it), the sums vanilla's has_naval_control uses. 0 when nobody has "
         "dominance there (and in land regions). `region` defaults to the strategic region scope, `country` to "
         "ROOT (or the country scope).",
         NumericKind::Ratio, params, ScopeMask::Any, "value", &NavalValue, &NavalSubject},
        kFeature);
    OnGameEntered(&OnEntered);
    return true;
}
