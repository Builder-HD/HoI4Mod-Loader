// Feature registry and tier gating, plus the core detection files every mod relies on.
#include "loader.h"

#include <stdio.h>

struct Feature {
    const char* name;  // script flag: hoi4_loader_plugin_<name>
    Tier tier;
    bool (*init)();
    bool active;
};

// Add new plugins here.
static Feature g_features[] = {
    {"engine_probe", Tier::Engine, &EngineProbeInit, false},
};

static const char* TierName(Tier t) { return t == Tier::Engine ? "engine" : "file"; }

// No C++ objects here, so SEH catches a plugin crashing during init.
static bool CallInitSafe(bool (*init)(), const char* name) {
    __try {
        return init();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("ERROR: feature %s crashed during init (exception 0x%08lX)", name, GetExceptionCode());
        return false;
    }
}

std::string FeatureNames(Tier tier) {
    // Core detection (hoi4_loader_present / hoi4_loader_init) is file tier but not a plugin.
    std::string s = tier == Tier::File ? "core detection" : "";
    for (const Feature& f : g_features) {
        if (f.tier != tier) continue;
        if (!s.empty()) s += ", ";
        s += f.name;
    }
    return s.empty() ? "(none)" : s;
}

// ---------------------------------------------------------------------------
// Core detection files (file tier, always served)
// ---------------------------------------------------------------------------
static std::string DetectTrigger() {
    return "# Served by hoi4_loader (version.dll)\r\n"
           "hoi4_loader_present = { always = yes }\r\n";
}

// Sets one flag per active feature and clears the flag of every other known feature, so a
// flag saved by an earlier session with a different hoi4.exe doesn't linger after loading.
static std::string DetectEffect() {
    char line[160];
    std::string s = "# Served by hoi4_loader (version.dll)\r\n"
                    "hoi4_loader_init = {\r\n"
                    "\tset_global_flag = hoi4_loader_present\r\n";
    sprintf_s(line, "\tset_variable = { global.hoi4_loader_version = %d }\r\n", LOADER_VERSION);
    s += line;
    for (const Feature& f : g_features) {
        sprintf_s(line, "\t%s = hoi4_loader_plugin_%s # %s tier, %s\r\n", f.active ? "set_global_flag" : "clr_global_flag",
                  f.name, TierName(f.tier), f.active ? "active" : "inactive");
        s += line;
    }
    s += "}\r\n";
    return s;
}

void FeaturesInit(bool engineAllowed) {
    for (Feature& f : g_features) {
        if (f.tier == Tier::Engine && !engineAllowed) {
            Log("Feature %s (%s tier): DISABLED, hoi4.exe hash not supported", f.name, TierName(f.tier));
            continue;
        }
        f.active = CallInitSafe(f.init, f.name);
        Log("Feature %s (%s tier): %s", f.name, TierName(f.tier), f.active ? "active" : "FAILED, left inactive");
    }
    register_virtual_file(L"common/scripted_triggers/hoi4_loader_detect.txt", &DetectTrigger);
    register_virtual_file(L"common/scripted_effects/hoi4_loader_detect.txt", &DetectEffect);
}
