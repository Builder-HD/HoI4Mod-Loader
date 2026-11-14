// equipment_designers (engine tier): designers for equipment the game gives none (NOTES.md Part N).
//
//   # in a mod: common/hoi4_loader_equipment_designers/*.txt
//   small_arms_designer = {
//       archetypes = { infantry_equipment }   # archetypes with module_slots; their type must be one
//                                              # the tank designer window serves (e.g. infantry)
//       window = land                          # optional; only land for now
//       title = SMALL_ARMS_DESIGNER_TITLE      # optional loc key instead of "Tank Designer"
//       hide_roles = yes                       # optional (default): hide the tank role dropdown
//       slots = { small_arms_slot = { x = 1 y = 1 } }   # optional slot window positions
//   }
//
// The game opens the old upgrade window for modular equipment without a designer. For a listed
// archetype the loader lets the game open its tank designer window instead (the archetype still
// needs its interface/equipmentdesigner layout, and No Step Back). The archetype keeps its real
// type, which decides the production tab, the XP the design costs and how it is saved, all as the
// game does it. UI only: designs are saved by the game's own command, so multiplayer and saves are
// unaffected.
#include <windows.h>

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine.h"
#include "equipment_designers.h"
#include "loader.h"
#include "mod_list.h"

namespace {

const char* const kFeature = "equipment_designers";
const char kFolder[] = "common/hoi4_loader_equipment_designers";

std::vector<Eqd::Designer> g_defs;
std::map<std::string, size_t> g_byName;  // archetype name (lower case) -> g_defs index

// Resolved on the main thread at first use (the equipment database is loaded by then).
bool g_resolved = false;
std::unordered_map<const void*, size_t> g_byType;  // archetype -> g_defs index
std::vector<const EngineString*> g_titles;          // per definition, localised once (null: none)
std::vector<bool> g_titleTried;

// Slot windows the GUI file had before the loader's patch: windows past these are the loader's.
volatile LONG g_customBefore = -1, g_fixedBefore = -1;

// Views whose roles or slot positions the loader changed (at most the three designer windows),
// with the listed archetype shown last.
struct Touched {
    const void* view = nullptr;
    const void* archetype = nullptr;
};
Touched g_touched[8];

Touched* TouchedOf(const void* view, bool create) {
    for (Touched& t : g_touched)
        if (t.view == view) return &t;
    if (create)
        for (Touched& t : g_touched)
            if (!t.view) {
                t.view = view;
                return &t;
            }
    return nullptr;
}

bool StartsWithFixed(const char* s) { return strncmp(s, "fixed_", 6) == 0; }

void ResolveTypes() {
    g_resolved = true;
    std::vector<bool> served(g_defs.size(), false);
    for (const auto& e : g_byName) {
        const Eqd::Designer& d = g_defs[e.second];
        const void* type = Engine::EquipmentByToken(Engine::Intern(e.first.c_str()));
        if (!type) {
            Log("%s: %s (%s line %d): archetype %s not found; skipped", kFeature, d.name.c_str(), d.file.c_str(), d.line,
                e.first.c_str());
            continue;
        }
        if (Engine::TypeArchetypeOf(type) != type) {
            Log("%s: %s: %s is an equipment type, not an archetype; list its archetype", kFeature, d.name.c_str(), e.first.c_str());
            continue;
        }
        int slots = Engine::TypeSlotCount(type);
        if (slots == 0) {
            Log("%s: %s: archetype %s has no module_slots; skipped", kFeature, d.name.c_str(), e.first.c_str());
            continue;
        }
        if (!Engine::DesignerLandType(type)) {
            Log("%s: %s: archetype %s has no type the land designer serves (add e.g. infantry to its type, as towed "
                "artillery has); skipped",
                kFeature, d.name.c_str(), e.first.c_str());
            continue;
        }
        g_byType[type] = e.second;
        served[e.second] = true;
        int custom = 0, fixed = 0, placed = 0;
        LONG before[2] = {g_customBefore, g_fixedBefore};
        for (int i = 0; i < slots; ++i) {
            const char* name = Engine::TypeSlotName(type, i);
            bool isFixed = StartsWithFixed(name);
            int w = (isFixed ? fixed : custom)++;
            if (Eqd::FindSlot(d, name))
                ++placed;
            else if (before[isFixed ? 1 : 0] >= 0 && w >= before[isFixed ? 1 : 0])
                Log("%s: %s: slot %s of %s has no position and the GUI file has no window for it; it shows at the first "
                    "slot's place (give it one in slots)",
                    kFeature, d.name.c_str(), name, e.first.c_str());
        }
        for (const Eqd::SlotPos& p : d.slots) {
            bool found = false;
            for (int i = 0; i < slots && !found; ++i) found = Eqd::Lower(Engine::TypeSlotName(type, i)) == Eqd::Lower(p.slot);
            if (!found) Log("%s: %s: archetype %s has no slot %s (its position is unused)", kFeature, d.name.c_str(), e.first.c_str(), p.slot.c_str());
        }
        if (custom > Eqd::kMaxSlotWindows || fixed > Eqd::kMaxSlotWindows)
            Log("%s: %s: archetype %s has more slots than the designer has windows (%d plus %d fixed_); the rest don't show",
                kFeature, d.name.c_str(), e.first.c_str(), Eqd::kMaxSlotWindows, Eqd::kMaxSlotWindows);
        Log("%s: %s serves %s (%d slot(s), %d fixed_, %d placed by the definition)", kFeature, d.name.c_str(), e.first.c_str(),
            custom, fixed, placed);
    }
    if (!Engine::TankDesignerUnlocked()) Log("%s: the tank designer is not unlocked (No Step Back); designs open the old window", kFeature);
}

const Eqd::Designer* DesignerOf(const void* type) {
    if (!type) return nullptr;
    if (!g_resolved) ResolveTypes();
    auto it = g_byType.find(Engine::TypeArchetypeOf(type));
    return it == g_byType.end() ? nullptr : &g_defs[it->second];
}

const void* Find(const void* type) { return DesignerOf(type); }

const EngineString* TitleOf(const Eqd::Designer& d) {
    size_t i = (size_t)(&d - &g_defs[0]);
    if (!g_titleTried[i]) {
        g_titleTried[i] = true;
        g_titles[i] = Engine::LocalizeKey(d.title.c_str());
        if (!g_titles[i]) Log("%s: %s: title key %s has no localisation; the window keeps its own title", kFeature, d.name.c_str(), d.title.c_str());
    }
    return g_titles[i];
}

void Opened(void* view, const void* archetype) {
    if (!Engine::IsTankDesigner(view)) return;
    const Eqd::Designer* d = DesignerOf(archetype);
    if (d && !d->title.empty())
        if (const EngineString* text = TitleOf(*d)) Engine::DesignerSetTitle(view, text);
}

// Window i of a group: back where the GUI file puts it (only needed after the loader moved it).
void ResetGroup(void* view, bool fixed) {
    int n = Engine::DesignerSlotWindows(view, fixed);
    for (int i = 0; i < n; ++i) Engine::DesignerResetSlot(view, fixed, i);
}

void Refreshed(void* view, const void* archetype) {
    if (!Engine::IsTankDesigner(view) || !archetype) return;
    const Eqd::Designer* d = DesignerOf(archetype);
    if (d) {
        int slots = Engine::TypeSlotCount(archetype);
        Touched* t = TouchedOf(view, true);
        Engine::DesignerShowRoles(view, !d->hideRoles);
        if (t && t->archetype && t->archetype != archetype) {  // another listed design was shown: start from the GUI's positions
            ResetGroup(view, false);
            ResetGroup(view, true);
        }
        if (t) t->archetype = archetype;
        int next[2] = {0, 0};
        for (int i = 0; i < slots; ++i) {
            const char* name = Engine::TypeSlotName(archetype, i);
            bool fixed = StartsWithFixed(name);
            int w = next[fixed ? 1 : 0]++;
            if (const Eqd::SlotPos* p = Eqd::FindSlot(*d, name)) Engine::DesignerPlaceSlot(view, fixed, w, p->x, p->y);
        }
        // Only the windows that hold a slot: the game draws the others locked where they are.
        for (int g = 0; g < 2; ++g) {
            int n = Engine::DesignerSlotWindows(view, g == 1);
            for (int i = 0; i < n; ++i) Engine::DesignerShowSlot(view, g == 1, i, Engine::DesignerSlotAssigned(view, g == 1, i));
        }
        return;
    }
    if (Touched* t = TouchedOf(view, false)) {  // back to the game's look for every other design
        Engine::DesignerShowRoles(view, true);
        ResetGroup(view, false);
        ResetGroup(view, true);
        *t = Touched();
    }
    // Every other design sees the GUI file's own windows (locked where unused, as vanilla draws
    // them); the windows the loader added stay hidden (vanilla dropped slots beyond its windows).
    LONG before[2] = {g_customBefore, g_fixedBefore};
    for (int g = 0; g < 2; ++g) {
        int n = Engine::DesignerSlotWindows(view, g == 1);
        int own = before[g] >= 0 ? (int)before[g] : n;
        for (int i = 0; i < n; ++i) Engine::DesignerShowSlot(view, g == 1, i, i < own);
    }
}

void OnFault(const char* where) { FeatureDeactivate(kFeature, (std::string("fault in the ") + where).c_str()); }

std::string PatchGui(const std::string& original) {
    int custom = 0, fixed = 0;
    std::string out = Engine::PatchTankDesignerViewGui(original, Eqd::kMaxSlotWindows, &custom, &fixed);
    InterlockedExchange(&g_customBefore, custom);
    InterlockedExchange(&g_fixedBefore, fixed);
    if (!out.empty())
        Log("%s: tank_designer_view.gui: %d custom and %d fixed slot window(s) before, %d each now", kFeature, custom, fixed,
            Eqd::kMaxSlotWindows);
    return out;
}

void LoadDefinitions() {
    ModList::FolderScan scan = ModList::ScanFolder(GameFolder(), UserFolder(), kFolder);
    if (!scan.modListRead) Log("%s: the launcher's mod list (dlc_load.json) is unreadable; only the game folder is searched", kFeature);
    for (const std::string& note : scan.notes) Log("%s: %s", kFeature, note.c_str());
    std::vector<Eqd::Problem> problems;
    for (const ModList::FolderFile& f : scan.files) {
        std::string text;
        if (!ModList::ReadWholeFile(f.path, &text)) {
            Log("%s: %s unreadable", kFeature, f.shown.c_str());
            continue;
        }
        std::vector<Eqd::Designer> defs;
        Eqd::ParseFile(text, f.shown, &defs, &problems);
        Eqd::Merge(&g_defs, defs);
    }
    std::vector<Eqd::Designer> land;
    for (Eqd::Designer& d : g_defs) {
        if (d.window != "land") {
            problems.push_back({d.file, d.line, d.name + ": window = " + d.window + " is not supported yet (only land); skipped"});
            continue;
        }
        land.push_back(std::move(d));
    }
    g_defs.swap(land);
    g_byName = Eqd::ByArchetype(g_defs, &problems);
    for (const Eqd::Problem& p : problems) Log("%s: %s line %d: %s", kFeature, p.file.c_str(), p.line, p.message.c_str());
    g_titles.assign(g_defs.size(), nullptr);
    g_titleTried.assign(g_defs.size(), false);
    Log("%s: %zu designer(s) for %zu archetype(s) from %zu file(s) (%zu enabled mod(s), %d with a designer folder)", kFeature,
        g_defs.size(), g_byName.size(), scan.files.size(), scan.enabledMods, scan.modsWithFolder);
}

}  // namespace

bool EquipmentDesignersInit() {
    if (!Engine::HaveDesigners()) {
        Log("%s: required engine functions or fields missing", kFeature);
        return false;
    }
    LoadDefinitions();
    if (g_defs.empty()) {
        Log("%s: no designer definitions; the designer windows are left alone", kFeature);
        return true;
    }
    if (!Engine::HookDesigners(&Find, &Opened, &Refreshed, &OnFault)) {
        Log("%s: hooks could not be queued", kFeature);
        return false;
    }
    if (!register_file_patch(L"interface/tank_designer_view.gui", &PatchGui))
        Log("%s: tank_designer_view.gui patch could not be registered; only the GUI's own slot windows exist", kFeature);
    return true;
}
