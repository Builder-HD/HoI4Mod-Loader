// production_data (engine tier): factories assigned to production.
//
//   factories_assigned > 20                                   country scope: all production lines
//   factories_assigned = { equipment = infantry_equipment count > 10 }      an archetype
//   factories_assigned = { equipment = small_plane_airframe_1 country = GER count < 5 }   a type
//
// Sums the factories of the country's military production lines (and the dockyards of its naval
// lines) that produce the equipment: an equipment type, or any type of an archetype. Without
// `equipment`: all lines. Resource deficits are not here: vanilla has them (README).
#include <atomic>
#include <stdint.h>
#include <string>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "native_script.h"

namespace {

const char* const kFeature = "production_data";
std::atomic<bool> g_ok{true};

// Parameters: 0 equipment (token), 1 country.
bool Value(const Args& a, const Scope& s, int64_t* out) {
    if (!g_ok.load(std::memory_order_relaxed)) return false;
    int tag = a.has(1) ? a.i(1) : s.countryTag();
    const void* equipment = nullptr;
    if (a.has(0) && !(equipment = Engine::EquipmentByToken(a.i(0)))) return false;  // unknown equipment
    return tag > 0 && Engine::FactoriesAssigned(tag, equipment, out);
}

void Subject(const Args& a, const Scope& s, TooltipText& out) {
    out.add("Factories of ").add(Engine::CountryTagName(a.has(1) ? a.i(1) : s.countryTag()).c_str());
    if (a.has(0)) out.add(" producing ").add(a.key(0));
    else out.add(" assigned to production");
}

bool SafeCheck(int tag, int64_t* total) {
    __try {
        return Engine::FactoriesAssigned(tag, nullptr, total) && *total >= 0 && *total < 100000;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void OnEntered(bool) {
    int tag = Engine::PlayerCountryTag();
    if (tag <= 0) return;
    int64_t total = 0;
    if (!SafeCheck(tag, &total)) {
        g_ok.store(false);
        FeatureDeactivate(kFeature, "self-check failed: production data unreadable or out of range");
        return;
    }
    g_ok.store(true);
    Log("production_data: self-check passed (%s: %lld factories/dockyards on production lines)",
        Engine::CountryTagName(tag).c_str(), (long long)total);
}

}  // namespace

bool ProductionDataInit() {
    if (!Engine::HaveProduction() || !Engine::HaveNumeric()) {
        Log("production_data: required engine functions missing (data %d, numeric triggers %d)", Engine::HaveProduction(),
            Engine::HaveNumeric());
        return false;
    }
    register_native_numeric_trigger(
        {"factories_assigned",
         "Factories a country has assigned to production lines: military factories on equipment lines plus "
         "dockyards on naval lines. `equipment`: an equipment type (infantry_equipment_1) or an archetype "
         "(infantry_equipment, every type of it); unknown names make it false. Without `equipment`: all lines. "
         "`country` defaults to the country scope.",
         NumericKind::Count, {{"equipment", ParamType::Token, false}, {"country", ParamType::Country, false}},
         ScopeMask::Country, "count", &Value, &Subject},
        kFeature);
    OnGameEntered(&OnEntered);
    return true;
}
