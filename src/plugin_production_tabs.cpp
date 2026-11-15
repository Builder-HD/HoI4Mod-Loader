// production_tabs (engine tier): which designs the production view's "add production line" tabs
// list, and new tabs with the mod's own buttons (NOTES.md Part O).
//
//   # in a mod: common/hoi4_loader_production_tabs/*.txt
//   # The vanilla tabs are infantry | armor | air | naval; any other name is a new tab.
//   artillery_tab = {
//       button = artillery_tab_button      # a buttonType the mod adds to countryproductionlineview.gui
//       glow = artillery_tab_button_glow   # optional iconType, shown while a design of the tab is new
//       add = { artillery_equipment anti_tank_equipment anti_air_equipment }
//   }
//   infantry = { remove = { artillery_equipment anti_tank_equipment anti_air_equipment } }
//
// A design is in a tab when its archetype is in the tab's `add`, or (vanilla tabs) its type matches
// the tab as in the game; and its archetype is not in the tab's `remove`. Nothing else changes:
// research, can_be_produced and the rest of the game's checks stay; the add-line command doesn't
// look at the tab, so this is UI only (multiplayer and saves are unaffected).
#include <windows.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "mod_list.h"
#include "production_tabs.h"

namespace {

const char* const kFeature = "production_tabs";
const char kFolder[] = "common/hoi4_loader_production_tabs";

std::vector<Ptab::Tab> g_defs;   // after Finish: the vanilla tabs with lists and the new tabs
int g_vanillaDef[4] = {-1, -1, -1, -1};  // g_defs index per vanilla tab
std::vector<size_t> g_newDefs;           // g_defs index per new tab (engine tab 4 + i)

// Resolved once on the main thread, before each game entry or at an earlier first use there (the
// equipment database is loaded by then).
struct Sets {
    std::unordered_set<const void*> add, remove;
    uint64_t addTypes = 0, removeTypes = 0;  // type mask bits
};
std::vector<Sets> g_sets;  // per engine tab: 0..3 vanilla, then the new tabs
bool g_resolved = false;

const Ptab::Tab* DefOf(int tab) {
    if (tab < 4) return g_vanillaDef[tab] >= 0 ? &g_defs[(size_t)g_vanillaDef[tab]] : nullptr;
    size_t i = (size_t)(tab - 4);
    return i < g_newDefs.size() ? &g_defs[g_newDefs[i]] : nullptr;
}

void ResolveList(const Ptab::Tab& t, const std::vector<Ptab::Archetype>& list, const char* key,
                 std::unordered_set<const void*>* out) {
    for (const Ptab::Archetype& a : list) {
        const void* type = Engine::EquipmentByToken(Engine::Intern(a.name.c_str()));
        if (!type) {
            Log("%s: %s (%s line %d): %s: archetype %s not found; skipped", kFeature, t.name.c_str(), a.file.c_str(), a.line, key,
                a.name.c_str());
            continue;
        }
        if (Engine::PtabArchetypeOf(type) != type) {
            Log("%s: %s (%s line %d): %s: %s is an equipment type, not an archetype; list its archetype", kFeature, t.name.c_str(),
                a.file.c_str(), a.line, key, a.name.c_str());
            continue;
        }
        out->insert(type);
    }
}

uint64_t ResolveTypes(const Ptab::Tab& t, const std::vector<Ptab::Archetype>& list, const char* key) {
    uint64_t mask = 0;
    for (const Ptab::Archetype& a : list) {
        if (!Engine::HavePtabTypes()) {
            Log("%s: %s (%s line %d): %s is unavailable on this build; ignored", kFeature, t.name.c_str(), a.file.c_str(), a.line, key);
            return 0;
        }
        uint64_t m = Engine::PtabTypeMask(a.name.c_str());
        if (!m)
            Log("%s: %s (%s line %d): %s: %s is not an equipment type (the names of `type = { ... }`); skipped", kFeature,
                t.name.c_str(), a.file.c_str(), a.line, key, a.name.c_str());
        mask |= m;
    }
    return mask;
}

void Resolve() {
    g_resolved = true;
    g_sets.assign(4 + g_newDefs.size(), Sets());
    for (int tab = 0; tab < (int)g_sets.size(); ++tab) {
        const Ptab::Tab* t = DefOf(tab);
        if (!t) continue;
        Sets& s = g_sets[(size_t)tab];
        ResolveList(*t, t->add, "add", &s.add);
        ResolveList(*t, t->remove, "remove", &s.remove);
        s.addTypes = ResolveTypes(*t, t->addTypes, "add_types");
        s.removeTypes = ResolveTypes(*t, t->removeTypes, "remove_types");
        Log("%s: %s%s: %zu archetype(s) added, %zu removed; types added 0x%llx, removed 0x%llx", kFeature, t->name.c_str(),
            tab < 4 ? " (vanilla tab)" : "", s.add.size(), s.remove.size(), (unsigned long long)s.addTypes,
            (unsigned long long)s.removeTypes);
    }
}

void ResolveBeforeEntry() {
    if (!g_resolved) Resolve();
}

// UI thread (the production view). The most specific rule decides: an archetype's remove, its add,
// then the types' remove, their add, then the game's own answer.
bool Member(int tab, const void* archetype, uint64_t types, bool vanilla) {
    if (!g_resolved) {
        if (!IsMainThread()) return vanilla;
        Resolve();
    }
    if (tab < 0 || (size_t)tab >= g_sets.size()) return vanilla;
    const Sets& s = g_sets[(size_t)tab];
    if (s.remove.count(archetype)) return false;
    if (s.add.count(archetype)) return true;
    if (types & s.removeTypes) return false;
    if (types & s.addTypes) return true;
    return vanilla;
}

void OnFault(const char* where) { FeatureDeactivate(kFeature, (std::string("fault in the ") + where).c_str()); }

void LoadDefinitions() {
    ModList::FolderScan scan = ModList::ScanFolder(GameFolder(), UserFolder(), kFolder);
    if (!scan.modListRead) Log("%s: the launcher's mod list (dlc_load.json) is unreadable; only the game folder is searched", kFeature);
    for (const std::string& note : scan.notes) Log("%s: %s", kFeature, note.c_str());
    std::vector<Ptab::Problem> problems;
    for (const ModList::FolderFile& f : scan.files) {
        std::string text;
        if (!ModList::ReadWholeFile(f.path, &text)) {
            Log("%s: %s unreadable", kFeature, f.shown.c_str());
            continue;
        }
        std::vector<Ptab::Tab> defs;
        Ptab::ParseFile(text, f.shown, &defs, &problems);
        Ptab::Merge(&g_defs, defs, &problems);
    }
    Ptab::Finish(&g_defs, &problems);
    for (const Ptab::Problem& p : problems) Log("%s: %s line %d: %s", kFeature, p.file.c_str(), p.line, p.message.c_str());
    for (size_t i = 0; i < g_defs.size(); ++i) {
        if (g_defs[i].vanilla >= 0)
            g_vanillaDef[g_defs[i].vanilla] = (int)i;
        else
            g_newDefs.push_back(i);
    }
    Log("%s: %zu new tab(s), %d vanilla tab(s) changed, from %zu file(s) (%zu enabled mod(s), %d with a tabs folder)", kFeature,
        g_newDefs.size(), (g_vanillaDef[0] >= 0) + (g_vanillaDef[1] >= 0) + (g_vanillaDef[2] >= 0) + (g_vanillaDef[3] >= 0),
        scan.files.size(), scan.enabledMods, scan.modsWithFolder);
}

}  // namespace

bool ProductionTabsInit() {
    if (!Engine::HaveProductionTabs()) {
        Log("%s: required engine functions or fields missing", kFeature);
        return false;
    }
    LoadDefinitions();
    if (g_defs.empty()) {
        Log("%s: no tab definitions; the production tabs are left alone", kFeature);
        return true;
    }
    bool changed[4] = {false, false, false, false};
    for (int i = 0; i < 4; ++i) {
        const Ptab::Tab* t = DefOf(i);
        changed[i] = t && (!t->add.empty() || !t->remove.empty() || !t->addTypes.empty() || !t->removeTypes.empty());
    }
    std::vector<Engine::PtabNewTab> tabs;
    for (size_t i : g_newDefs) tabs.push_back({g_defs[i].name.c_str(), g_defs[i].button.c_str(), g_defs[i].glow.c_str()});
    if (!Engine::HookProductionTabs(changed, tabs.data(), (int)tabs.size(), &Member, &OnFault)) {
        Log("%s: hooks could not be queued", kFeature);
        return false;
    }
    OnGameEntering(&ResolveBeforeEntry);
    return true;
}
