// Feature registry and tier gating, plus the core detection files every mod relies on.
#include "loader.h"

#include <stdio.h>
#include <string.h>

#include "engine.h"
#include "lifecycle.h"
#include "native_script.h"

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
    {"game_loaded", Tier::Engine, &GameLoadedInit, "lifecycle", false, 0},
    {"region_control", Tier::Engine, &RegionControlInit, "lifecycle", false, 0},
    {"military_counts", Tier::Engine, &MilitaryCountsInit, "lifecycle", false, 0},
    {"army_condition", Tier::Engine, &ArmyConditionInit, "lifecycle", false, 0},
    {"production_data", Tier::Engine, &ProductionDataInit, "lifecycle", false, 0},
    {"region_scope", Tier::Engine, &RegionScopeInit, "lifecycle", false, 0},
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

// hoi4_loader_feature_<name>: a scripted trigger that is never stale (read from script each
// session, not saved). Served only if the mod ships the stub file; generated on its first read,
// i.e. after native registration (lexer ready comes before any script is parsed).
static std::string GuardTrigger(const Feature& f) {
    bool complete = NativeFeatureComplete(f.name);
    bool yes = f.active && complete;
    const char* why = !f.active ? "feature inactive"
                      : complete ? "feature active, all native commands registered"
                      : NativeScriptFinalized() ? "a native command of this feature was refused"
                                                : "read BEFORE native registration ran";
    Log("Guard trigger hoi4_loader_feature_%s = %s (%s)", f.name, yes ? "yes" : "no", why);
    char buf[256];
    sprintf_s(buf,
              "# Served by hoi4_loader (version.dll): %s\r\n"
              "hoi4_loader_feature_%s = { always = %s }\r\n",
              why, f.name, yes ? "yes" : "no");
    return buf;
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
    // Writing flags here happens on this machine only: never in multiplayer or after a hot join
    // (NOTES.md Part D). There the weekly hoi4_loader_init, which runs on every machine in the
    // same tick, refreshes them.
    const char* why = nullptr;
    if (!SessionAllowsLocalWrites(&why)) {
        Log("Plugin flags NOT refreshed after loading: %s (the weekly hoi4_loader_init refreshes them)", why);
        return;
    }
    Engine::SetGlobalFlag(g_presentToken);
    for (const Feature& f : g_features)
        f.active ? Engine::SetGlobalFlag(f.flagToken) : Engine::ClearGlobalFlag(f.flagToken);
    Log("Plugin flags refreshed after loading a save");
}

// --- Trigger timing diagnostics ([diagnostics] trigger_timing = 1) -----------------------------
// Every evaluation of a numeric trigger, a scope block trigger or an any_ iterator is timed
// (QueryPerformanceCounter, two calls per evaluation). On the main thread, every 10 in-game days,
// loader.log gets per trigger: evaluations, average time per evaluation, and time per in-game day;
// then the counters restart. Scope blocks and iterators include the triggers inside them, so the
// total adds up the numeric triggers only.
static int g_timingDay = -1, g_timingDays = 0;
static LARGE_INTEGER g_qpcFreq;

static void TimingFrame() {
    int hours = Engine::GameDateHours();
    if (hours <= 0) {
        g_timingDay = -1;
        return;
    }
    int day = hours / 24;
    if (day == g_timingDay) return;
    if (g_timingDay >= 0 && day > g_timingDay && day - g_timingDay <= 7) {
        g_timingDays += day - g_timingDay;
    } else {  // first frame, new game or loaded save: restart the window
        for (Command* c : NativeCommands()) {
            InterlockedExchange64(&c->evalCount, 0);
            InterlockedExchange64(&c->evalTicks, 0);
        }
        g_timingDays = 0;
    }
    g_timingDay = day;
    if (g_timingDays < 10) return;
    double totalMs = 0;
    for (Command* c : NativeCommands()) {
        bool timed = c->numeric || (c->kind == CommandKind::Trigger && (c->scopeBlock || c->iterator));
        if (!timed || c->status != CommandStatus::Registered) continue;
        long long n = InterlockedExchange64(&c->evalCount, 0);
        long long ticks = InterlockedExchange64(&c->evalTicks, 0);
        if (n == 0) continue;
        double ms = ticks * 1000.0 / (double)g_qpcFreq.QuadPart;
        if (c->numeric) totalMs += ms;
        Log("Trigger timing (%d days): %s: %lld evaluations, %.0f ns each, %.3f ms per day", g_timingDays, c->name.c_str(), n,
            ms * 1e6 / (double)n, ms / g_timingDays);
    }
    Log("Trigger timing (%d days): all numeric triggers %.3f ms per in-game day", g_timingDays, totalMs / g_timingDays);
    g_timingDays = 0;
}

void FeaturesInit(bool engineAllowed, const wchar_t* iniPath) {
    for (Feature& f : g_features) {
        if (f.tier == Tier::Engine && !engineAllowed) {
            Log("Feature %s (%s tier): DISABLED, hoi4.exe hash not supported", f.name, TierName(f.tier));
            continue;
        }
        wchar_t key[64];
        swprintf_s(key, L"%hs", f.name);
        if (iniPath && GetPrivateProfileIntW(L"features", key, 1, iniPath) == 0) {
            Log("Feature %s (%s tier): DISABLED in loader_config.ini [features]", f.name, TierName(f.tier));
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
        // Native effects/triggers queued by the features above go into the engine right after
        // the lexer is ready, before any script is parsed.
        if (Engine::HaveTokens()) OnLexerReady(&NativeScriptRegisterWithEngine);
    } else if (!NativeCommands().empty()) {
        Log("Native script: %zu command(s) not registered, lifecycle feature inactive", NativeCommands().size());
    }
    if (iniPath && GetPrivateProfileIntW(L"diagnostics", L"trigger_timing", 0, iniPath) == 1) {
        if (MainThreadQueueAvailable()) {
            QueryPerformanceFrequency(&g_qpcFreq);
            NativeSetTiming(true);
            OnMainThreadFrame(&TimingFrame);
            Log("Diagnostics: trigger timing ON for numeric triggers, scope blocks and iterators (report every 10 in-game days)");
        } else {
            Log("Diagnostics: trigger timing needs the main-thread queue; off");
        }
    }
    if (iniPath && GetPrivateProfileIntW(L"diagnostics", L"production_lines", 0, iniPath) == 1 && Engine::HaveProduction()) {
        OnGameEntered([](bool) { Engine::LogProductionLines(Engine::PlayerCountryTag()); });
        Log("Diagnostics: production line dump ON (on game entry, player country)");
    }
    register_virtual_file(L"common/scripted_triggers/hoi4_loader_detect.txt", &DetectTrigger);
    register_virtual_file(L"common/scripted_effects/hoi4_loader_detect.txt", &DetectEffect);
    for (const Feature& f : g_features) {
        wchar_t path[128];
        swprintf_s(path, L"common/scripted_triggers/hoi4_loader_feature_%hs.txt", f.name);
        const Feature* fp = &f;
        register_virtual_file(path, [fp] { return GuardTrigger(*fp); }, /*lazy=*/true);
    }
}

bool LifecycleFeatureInit() { return LifecycleInstall(); }
