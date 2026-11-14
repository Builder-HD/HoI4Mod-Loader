// air_scenarios (engine tier): new air scenarios from mods (NOTES.md Part L).
//
//   # in a mod: common/hoi4_loader_air_scenarios/*.txt
//   gunship_orbit = {
//       entity = gunship_orbit_entity          # an entity of the mod (gfx/entities/*.asset)
//       slot = bomber_1_CAS                    # the game's scenario it plays in (mission + plane count)
//       attacker = { equipment = { ... } country = { ... } }   # optional
//       defender = { equipment = { ... } country = { ... } }   # optional, "_vs_" slots only
//       weight = 1                             # optional
//   }
//
// The files are read from the game folder and the launcher's enabled mods (dlc_load.json), in load
// order; a file of the same name in a later mod replaces an earlier one, and so does an entry of
// the same name. When the game builds its scenario templates, the loader builds one per entry with
// the game's own loader. When a scenario starts, the entries for its slot whose conditions hold
// for the planes are the candidates; one is picked by weight and plays instead of the game's.
// Graphics only: different machines may pick differently without any effect on the game.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "air_scenarios.h"
#include "engine.h"
#include "loader.h"
#include "mod_list.h"

namespace {

const char* const kFeature = "air_scenarios";
const char kFolder[] = "common/hoi4_loader_air_scenarios";

struct Built {
    Air::Definition def;
    void* tmpl = nullptr;
    std::atomic<bool> played{false};
};
std::vector<Built*> g_bySlot[64];  // read-only after the build
bool g_built = false;
std::atomic<uint32_t> g_rng{0};

std::vector<Air::Definition> LoadDefinitions() {
    ModList::FolderScan scan = ModList::ScanFolder(GameFolder(), UserFolder(), kFolder);
    if (!scan.modListRead)
        Log("air_scenarios: the launcher's mod list (dlc_load.json) is unreadable; only the game folder is searched");
    for (const std::string& note : scan.notes) Log("air_scenarios: %s", note.c_str());
    std::vector<Air::Definition> all;
    for (const ModList::FolderFile& f : scan.files) {  // file name order, as the game reads a folder
        std::string text;
        if (!ModList::ReadWholeFile(f.path, &text)) {
            Log("air_scenarios: %s unreadable", f.shown.c_str());
            continue;
        }
        std::vector<Air::Definition> defs;
        std::vector<Air::Problem> problems;
        Air::ParseFile(text, f.shown, &defs, &problems);
        for (const Air::Problem& p : problems)
            Log("air_scenarios: %s line %d: %s", p.file.c_str(), p.line, p.message.c_str());
        Air::Merge(&all, defs);
    }
    Log("air_scenarios: %zu definition(s) from %zu file(s) (%zu enabled mod(s), %d with an air scenario folder)", all.size(),
        scan.files.size(), scan.enabledMods, scan.modsWithFolder);
    return all;
}

int SlotOf(const std::string& name) {
    for (int i = 0; i < Engine::AirSlotCount(); ++i)
        if (Air::EqualsNoCase(name, Engine::AirSlotName(i))) return i;
    return -1;
}

std::string SideText(const Air::Side& s) {
    if (!s.present) return "any";
    std::string t;
    for (const std::string& e : s.equipment) t += (t.empty() ? "" : " ") + e;
    if (!s.countries.empty()) {
        t += t.empty() ? "countries" : ", countries";
        for (const std::string& c : s.countries) t += " " + c;
    }
    return t.empty() ? "any" : t;
}

// Main thread (the manager's constructor), once per session: read and build every definition.
void OnBuilt() {
    if (g_built) {
        Log("air_scenarios: the game rebuilt its scenario templates; the loader's are kept");
        return;
    }
    g_built = true;
    g_rng.store((uint32_t)GetTickCount64() ^ (GetCurrentProcessId() << 16) | 1);
    int ok = 0;
    for (Air::Definition& d : LoadDefinitions()) {
        std::string where = d.file + " line " + std::to_string(d.line);
        auto skip = [&](const std::string& why) { Log("air_scenarios: %s (%s) skipped: %s", d.name.c_str(), where.c_str(), why.c_str()); };
        int slot = SlotOf(d.slot);
        void* vanilla = slot >= 0 ? Engine::AirVanillaTemplate(slot) : nullptr;
        if (slot < 0) {
            skip("unknown slot '" + d.slot + "' (one of the game's scenarios, e.g. bomber_1_CAS; see MODDING.md)");
            continue;
        }
        if (!vanilla) {
            skip("the game has no template for " + d.slot);
            continue;
        }
        if (d.defender.present && Engine::AirTemplateNodes(vanilla, true) == 0) {
            skip(d.slot + " has no defending side; leave out 'defender'");
            continue;
        }
        if (!Engine::AirEntityExists(d.entity.c_str())) {
            skip("no entity named " + d.entity + " (gfx/entities/*.asset)");
            continue;
        }
        void* tmpl = Engine::AirBuildTemplate(slot, d.entity);
        if (!tmpl || !Engine::AirTemplateEntity(tmpl)) {
            skip("the game could not load " + d.entity + " as a scenario");
            continue;
        }
        if (Engine::AirTemplateNodes(tmpl, false) != Engine::AirTemplateNodes(vanilla, false) ||
            Engine::AirTemplateNodes(tmpl, true) != Engine::AirTemplateNodes(vanilla, true)) {
            skip("its scenario has other attachment nodes than " + d.slot);
            continue;
        }
        Built* b = new Built;  // kept for the session (the template too)
        b->def = d;
        b->tmpl = tmpl;
        g_bySlot[slot].push_back(b);
        ++ok;
        Log("air_scenarios: %s: %s plays in %s (attacker: %s; defender: %s; weight %d) [%s]", d.name.c_str(), d.entity.c_str(),
            Engine::AirSlotName(slot), SideText(d.attacker).c_str(), SideText(d.defender).c_str(), d.weight, where.c_str());
    }
    Log("air_scenarios: %d new air scenario(s) ready; a missing attachment node shows in the game's error.log "
        "(\"failed to find attachment node\")",
        ok);
}

uint32_t NextRandom() {
    uint32_t x = g_rng.load(std::memory_order_relaxed), y;
    do {
        y = x;
        y ^= y << 13;
        y ^= y >> 17;
        y ^= y << 5;
    } while (!g_rng.compare_exchange_weak(x, y, std::memory_order_relaxed));
    return y;
}

void ReadSide(const void* list, std::vector<Air::Plane>* out) {
    int n = Engine::AirListCount(list);
    for (int i = 0; i < n && i < 64; ++i) {
        Air::Plane p;
        int tag = 0;
        if (!Engine::AirReadDesign(Engine::AirListDesign(list, i), &p.equipment, &p.archetype, &tag)) continue;
        if (tag > 0) p.country = Engine::CountryTagName(tag);
        out->push_back(std::move(p));
    }
}

// "small_plane_airframe_2 (small_plane_airframe, GER), ..." for the no-match log line.
std::string PlanesText(const std::vector<Air::Plane>& planes) {
    if (planes.empty()) return "none";
    std::vector<std::string> seen;
    for (const Air::Plane& p : planes) {
        std::string t = p.equipment + " (" + (p.archetype.empty() ? "?" : p.archetype) + ", " + (p.country.empty() ? "?" : p.country) + ")";
        if (std::find(seen.begin(), seen.end(), t) == seen.end()) seen.push_back(t);
    }
    std::string out;
    for (const std::string& s : seen) out += (out.empty() ? "" : ", ") + s;
    return out;
}

// Once per slot: a scenario started where the slot has entries but none matched, with what the
// planes were, so a modder sees why their entry didn't play.
std::atomic<bool> g_noMatchLogged[64];

// When a scenario starts: a candidate's template, or null for the game's own.
void* OnPick(int slot, const void* attackers, const void* defenders) {
    const std::vector<Built*>& entries = g_bySlot[slot & 63];
    if (entries.empty()) return nullptr;
    std::vector<Air::Plane> a, d;
    ReadSide(attackers, &a);
    ReadSide(defenders, &d);
    std::vector<Built*> candidates;
    std::vector<int> weights;
    for (Built* b : entries)
        if (Air::Matches(b->def, a, d)) {
            candidates.push_back(b);
            weights.push_back(b->def.weight);
        }
    int i = Air::PickByWeight(weights, NextRandom());
    if (i < 0) {
        if (!g_noMatchLogged[slot & 63].exchange(true))
            Log("air_scenarios: %s started without a matching entry (attackers: %s; defenders: %s); the game's own scenario "
                "plays (logged once per slot)",
                Engine::AirSlotName(slot), PlanesText(a).c_str(), PlanesText(d).c_str());
        return nullptr;
    }
    Built* b = candidates[(size_t)i];
    if (!b->played.exchange(true))
        Log("air_scenarios: %s plays for the first time (%s, %zu candidate(s))", b->def.name.c_str(), Engine::AirSlotName(slot),
            candidates.size());
    return b->tmpl;
}

}  // namespace

bool AirScenariosInit() {
    if (!Engine::HaveAirScenarios()) {
        Log("air_scenarios: required engine functions missing");
        return false;
    }
    return Engine::HookAirScenarios(&OnBuilt, &OnPick);
}
