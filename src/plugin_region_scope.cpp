// region_scope (engine tier): strategic region scopes for script (NOTES.md F5).
//
//   strategic_region = { region = 42 <triggers> }            trigger: all of them true in region 42
//   strategic_region = { region = 42 <effects> }             effect: run them there
//   any_strategic_region = { count = 2 <triggers> }          as any_state
//   every_strategic_region = { limit = { ... } <effects> }   as every_state
//   random_strategic_region = { limit = { ... } <effects> }  as random_state, without prioritize
//   strategic_region_id = 42   is_sea_region = yes   region_has_state = 16   (region scope)
//
// Without `region` the block uses the strategic region scope, else the state scope's region (the
// region of the state's first province). The iterators go through the regions in id order.
// Inside, the region-aware data triggers (air_superiority, divisions_count, ...) and
// set_strategic_region_name need no `region`.
#include <atomic>
#include <string>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "native_script.h"

namespace {

const char* const kFeature = "region_scope";
std::atomic<bool> g_ok{true};

std::string RegionText(int id) { return Engine::RegionName(id) + " (" + std::to_string(id) + ")"; }

// --- strategic_region: param 0 region ----------------------------------------------------------
bool RegionTarget(const Args& a, const Scope& s, ScopeTarget* out) {
    if (!g_ok.load(std::memory_order_relaxed)) return false;
    int id = a.has(0) ? a.i(0) : s.strategicRegionId();
    if (id <= 0 && s.stateId() > 0) id = Engine::RegionOfState(s.stateId());
    if (id <= 0 || !Engine::RegionById(id)) return false;
    out->type = ScopeMask::StrategicRegion;
    out->id = id;
    return true;
}

// --- Identity triggers (strategic region scope only) -------------------------------------------
bool RegionIdEval(const Args& a, const Scope& s) { return s.strategicRegionId() == a.i(0); }
void RegionIdTooltip(const Args& a, const Scope& s, bool negated, TooltipText& out) {
    out.add(negated ? "Strategic region is not " : "Strategic region is ").add(RegionText(a.i(0)).c_str());
    if (s.strategicRegionId() > 0 && s.strategicRegionId() != a.i(0))
        out.add(" (it is ").add(RegionText(s.strategicRegionId()).c_str()).add(")");
}

bool IsSeaEval(const Args& a, const Scope& s) {
    bool sea = false;
    return g_ok.load(std::memory_order_relaxed) && Engine::RegionIsSea(s.strategicRegionId(), &sea) && sea == a.b(0);
}
void IsSeaTooltip(const Args& a, const Scope&, bool negated, TooltipText& out) {
    bool sea = a.b(0) != negated;
    out.add(sea ? "Is a sea region" : "Is a land region");
}

bool HasStateEval(const Args& a, const Scope& s) {
    bool has = false;
    return g_ok.load(std::memory_order_relaxed) && Engine::RegionHasState(s.strategicRegionId(), a.i(0), &has) && has;
}
void HasStateTooltip(const Args& a, const Scope&, bool negated, TooltipText& out) {
    out.add(negated ? "Does not include state " : "Includes state ").add(a.i(0));
}

// --- Self-check on every game entry (main thread) ----------------------------------------------
bool SafeCheck(int* regions, int* sea, int* stateRegion, bool* stateBack) {
    __try {
        int n = Engine::RegionCount();
        for (int id = 1; id < n; ++id) {
            bool s = false;
            if (!Engine::RegionById(id)) continue;
            if (Engine::RegionTemplateId(Engine::RegionById(id)) != id || !Engine::RegionIsSea(id, &s)) return false;
            ++*regions;
            *sea += s;
        }
        // State 1 exists in every game; its region must contain it.
        *stateRegion = Engine::RegionOfState(1);
        return *stateRegion > 0 && Engine::RegionHasState(*stateRegion, 1, stateBack);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void OnEntered(bool) {
    int regions = 0, sea = 0, stateRegion = 0;
    bool back = false;
    if (!SafeCheck(&regions, &sea, &stateRegion, &back) || regions == 0 || sea == 0 || sea == regions || !back) {
        g_ok.store(false);
        Log("region_scope: self-check failed (%d regions, %d sea, state 1 -> region %d, back %d)", regions, sea, stateRegion,
            back);
        FeatureDeactivate(kFeature, "self-check failed: region data unreadable");
        return;
    }
    g_ok.store(true);
    Log("region_scope: self-check passed (%d strategic regions, %d sea; state 1 lies in %s)", regions, sea,
        RegionText(stateRegion).c_str());
}

}  // namespace

bool RegionScopeInit() {
    if (!Engine::HaveScopeBlocks() || !Engine::HaveRegionData()) {
        Log("region_scope: required engine functions missing (scope blocks %d, region data %d)", Engine::HaveScopeBlocks(),
            Engine::HaveRegionData());
        return false;
    }
    static const char* const kBlockHelp =
        "Runs its other keys in a strategic region scope: the region `region`, else the strategic region "
        "scope, else the state scope's region (the region of the state's first province). Inside, the "
        "region data triggers and set_strategic_region_name need no `region`. Only commands that work in "
        "any scope or in the strategic region scope are valid inside.";
    static const std::string kTriggerHelp =
        std::string(kBlockHelp) + " As a trigger: true if all triggers inside are true there; false if there is no region "
                                  "(so TRUE under NOT).";
    static const std::string kEffectHelp = std::string(kBlockHelp) + " As an effect: runs the effects there; nothing if there is no region.";
    const std::vector<ParamSpec> blockParams = {{"region", ParamType::Int, false}};
    register_native_scope_block({"strategic_region", kTriggerHelp.c_str(), CommandKind::Trigger, blockParams, ScopeMask::Any,
                                 ScopeMask::StrategicRegion, &RegionTarget},
                                kFeature);
    register_native_scope_block({"strategic_region", kEffectHelp.c_str(), CommandKind::Effect, blockParams, ScopeMask::Any,
                                 ScopeMask::StrategicRegion, &RegionTarget},
                                kFeature);
    register_native_iterator(
        {"any_strategic_region",
         "True if at least `count` (default 1) strategic regions meet all triggers inside, checked in region "
         "id order (as any_state). Tooltip header: localisation key HOI4_LOADER_ANY_STRATEGIC_REGION_STARTS if a mod "
         "defines it, else \"Any strategic region\"; `tooltip = key` replaces it.",
         IteratorKind::Any, ScopeMask::StrategicRegion, "HOI4_LOADER_ANY_STRATEGIC_REGION_STARTS", "Any strategic region"},
        kFeature);
    register_native_iterator(
        {"every_strategic_region",
         "Runs the effects in every strategic region that meets `limit`, in region id order (or in "
         "`random_select_amount` random ones), as every_state; `display_individual_scopes = yes` lists them "
         "one by one in the tooltip.",
         IteratorKind::Every, ScopeMask::StrategicRegion, nullptr, nullptr},
        kFeature);
    register_native_iterator(
        {"random_strategic_region",
         "Runs the effects in one random strategic region that meets `limit` (multiplayer-safe, the game's "
         "synced random numbers), as random_state. `prioritize` (state ids) is not supported: a parse error.",
         IteratorKind::Random, ScopeMask::StrategicRegion, nullptr, nullptr},
        kFeature);
    register_native_trigger({"strategic_region_id", "True if the strategic region scope is the region with this id.",
                             Form::Value, {{"id", ParamType::Int, true}}, ScopeMask::StrategicRegion, &RegionIdEval,
                             &RegionIdTooltip},
                            kFeature);
    register_native_trigger({"is_sea_region",
                             "True if the strategic region scope is a sea region (it has sea provinces, as the game "
                             "itself tests it); `is_sea_region = no` for land regions.",
                             Form::Value, {{"sea", ParamType::Bool, true}}, ScopeMask::StrategicRegion, &IsSeaEval,
                             &IsSeaTooltip},
                            kFeature);
    register_native_trigger({"region_has_state",
                             "True if the state with this id has a province in the strategic region scope (a state "
                             "can span two regions).",
                             Form::Value, {{"state", ParamType::Int, true}}, ScopeMask::StrategicRegion, &HasStateEval,
                             &HasStateTooltip},
                            kFeature);
    OnGameEntered(&OnEntered);
    return true;
}
