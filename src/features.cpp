// Feature registry and tier gating, plus the core detection files every mod relies on.
#include "loader.h"

#include <stdio.h>
#include <string.h>

#include "engine.h"
#include "lifecycle.h"

struct Feature {
    const char* name;      // script flag: hoi4_loader_plugin_<name>
    Tier tier;
    bool (*init)();
    const char* requires;  // feature that must be active first, or nullptr
    bool active;
    int flagToken;         // interned once the lexer is ready (lifecycle only)
};

// Add new plugins here, after anything they require.
static Feature g_features[] = {
    {"engine_probe", Tier::Engine, &EngineProbeInit, nullptr, false, 0},
    {"lifecycle", Tier::Engine, &LifecycleFeatureInit, nullptr, false, 0},
    {"region_names", Tier::Engine, &RegionNamesInit, "lifecycle", false, 0},
};
static int g_presentToken = 0;

bool FeatureActive(const char* name) {
    for (const Feature& f : g_features)
        if (strcmp(f.name, name) == 0) return f.active;
    return false;
}

void FeatureDeactivate(const char* name, const char* reason) {
    for (Feature& f : g_features) {
        if (strcmp(f.name, name) != 0 || !f.active) continue;
        f.active = false;
        Log("Feature %s: DEACTIVATED at runtime (%s); its flag is cleared", name, reason);
        if (f.flagToken) Engine::ClearGlobalFlag(f.flagToken);
    }
}

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

// The served hoi4_loader_init refreshes flags only when script runs it (on_startup: new games;
// on_weekly). With the lifecycle feature the loader also refreshes them itself right after a
// save is loaded, so a save from a session with other features never shows stale flags.
static void InternFlagTokens() {
    g_presentToken = Engine::Intern("hoi4_loader_present");
    char name[96];
    for (Feature& f : g_features) {
        sprintf_s(name, "hoi4_loader_plugin_%s", f.name);
        f.flagToken = Engine::Intern(name);
    }
}

static void RefreshFlagsOnGameEntered(bool loadedFromSave) {
    if (!loadedFromSave) return;  // new games: on_startup runs hoi4_loader_init
    Engine::SetGlobalFlag(g_presentToken);
    for (const Feature& f : g_features)
        f.active ? Engine::SetGlobalFlag(f.flagToken) : Engine::ClearGlobalFlag(f.flagToken);
    Log("Plugin flags refreshed after loading a save");
}

void FeaturesInit(bool engineAllowed) {
    for (Feature& f : g_features) {
        if (f.tier == Tier::Engine && !engineAllowed) {
            Log("Feature %s (%s tier): DISABLED, hoi4.exe hash not supported", f.name, TierName(f.tier));
            continue;
        }
        if (f.requires && !FeatureActive(f.requires)) {
            Log("Feature %s (%s tier): DISABLED, requires %s", f.name, TierName(f.tier), f.requires);
            continue;
        }
        f.active = CallInitSafe(f.init, f.name);
        Log("Feature %s (%s tier): %s", f.name, TierName(f.tier), f.active ? "active" : "FAILED, left inactive");
    }
    if (FeatureActive("lifecycle")) {
        if (Engine::HaveTokens() && Engine::HaveFlags()) {
            OnLexerReady(&InternFlagTokens);
            OnGameEntered(&RefreshFlagsOnGameEntered);  // after every feature's own callbacks
        } else {
            Log("Lifecycle: token/flag functions missing; plugin flags refresh weekly only");
        }
    }
    register_virtual_file(L"common/scripted_triggers/hoi4_loader_detect.txt", &DetectTrigger);
    register_virtual_file(L"common/scripted_effects/hoi4_loader_detect.txt", &DetectEffect);
}

bool LifecycleFeatureInit() { return LifecycleInstall(); }
