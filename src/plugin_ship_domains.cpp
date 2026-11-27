// ship_domains (engine tier): navies that never mix. A mod names domains of ships (e.g. ships in
// space beside the navy at sea), each with its ship archetypes and the continents its naval bases are
// on; every other ship and base is domain 0. The loader keeps them apart (NOTES.md U14):
// - a built ship (production line, delivered equipment, create_ship) is delivered to a naval base,
//   task force or fleet of its domain: the line's target if it fits, else the best base of the domain
//   the country may use (the game's own rule: the highest level), else into the stockpile;
// - moving ships into a task force, task forces into a fleet, a fleet's home base and a naval line's
//   deployment target are refused when they would put two domains together (the command can't be
//   executed, as for any command the game refuses).
//
//   # in a mod: common/hoi4_loader_ship_domains/*.txt
//   space = {
//       archetypes = { space_hull_light space_hull_cruiser space_hull_heavy }   # ship archetypes
//       continents = { space }                         # continents (map/continent.txt) of its bases
//   }
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <utility>
#include <vector>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "mod_list.h"
#include "script_tree.h"

namespace {

const char* const kFeature = "ship_domains";
const char kFolder[] = "common/hoi4_loader_ship_domains";
const int kMaxDomains = 15;

struct Entry {
    std::string name, file;
    int line = 0;
};
struct Domain {
    std::string name;
    std::vector<Entry> archetypes, continents;
};
std::vector<Domain> g_domains;  // domain i + 1

// Built before each game entry, read-only while the game runs (production may update on several
// threads): archetype -> domain, sorted by address; per province id its domain.
std::vector<std::pair<const void*, int>> g_archetypes;
std::vector<unsigned char> g_provinces;
std::vector<std::vector<int>> g_domainProvinces;  // per domain (index domain - 1), ascending ids
std::vector<std::pair<void*, int>> g_continents;  // continent entry -> domain
std::atomic<bool> g_ready{false};
std::atomic<int> g_logged{0};

int ArchetypeDomain(const void* archetype) {
    if (!g_ready.load(std::memory_order_acquire)) return 0;
    size_t lo = 0, hi = g_archetypes.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_archetypes[mid].first < archetype) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_archetypes.size() && g_archetypes[lo].first == archetype ? g_archetypes[lo].second : 0;
}

int ProvinceDomain(int id) {
    if (!g_ready.load(std::memory_order_acquire)) return 0;
    return id > 0 && (size_t)id < g_provinces.size() ? g_provinces[(size_t)id] : 0;
}

int DomainProvinces(int domain, const int** ids) {
    if (!g_ready.load(std::memory_order_acquire) || domain < 1 || (size_t)domain > g_domainProvinces.size()) return 0;
    const std::vector<int>& v = g_domainProvinces[(size_t)domain - 1];
    *ids = v.data();
    return (int)v.size();
}

const char* DomainName(int d) { return d > 0 && (size_t)d <= g_domains.size() ? g_domains[(size_t)d - 1].name.c_str() : "none"; }

void Delivered(int tag, const void* design, int count, int domain, int from, int to) {
    if (g_logged.fetch_add(1) >= 40) return;
    std::string name = Engine::CountryTagName(tag);
    (void)design;
    if (to > 0)
        Log("%s: %s: %d %s ship(s) delivered at province %d (a base of their domain) instead of %d", kFeature, name.c_str(), count,
            DomainName(domain), to, from);
    else
        Log("%s: %s: %d %s ship(s) into the stockpile: no naval base of their domain the country may use", kFeature, name.c_str(),
            count, DomainName(domain));
}

void Refused(int command) {
    static const char* const kNames[] = {"moving ships into a task force", "detaching ships into a task force",
                                         "putting task forces into a fleet", "a fleet's home base", "a naval line's deployment target",
                                         "reorganizing a fleet's ships", "a new fleet from task forces and ships",
                                         "merging task forces", "a new fleet in a theater"};
    // The game asks again while its UI shows the choice: one line per command and second.
    static int lastCommand = -1;
    static ULONGLONG lastTick = 0;
    ULONGLONG now = GetTickCount64();
    if (command == lastCommand && now - lastTick < 1000) return;
    lastCommand = command, lastTick = now;
    if (g_logged.fetch_add(1) >= 40) return;
    Log("%s: %s refused: it would put two ship domains together", kFeature, command >= 0 && command < 9 ? kNames[command] : "a command");
}

// --- The definitions ----------------------------------------------------------------------------

void ParseFile(const std::string& text, const std::string& shown) {
    std::vector<ScriptTree::Node> nodes;
    std::string error;
    int errorLine = 0;
    if (!ScriptTree::Parse(text, &nodes, &error, &errorLine)) {
        Log("%s: %s line %d: %s; file skipped", kFeature, shown.c_str(), errorLine, error.c_str());
        return;
    }
    for (const ScriptTree::Node& n : nodes) {
        if (!n.block || n.key.empty()) {
            Log("%s: %s line %d: expected a domain block (name = { ... })", kFeature, shown.c_str(), n.line);
            continue;
        }
        Domain* d = nullptr;
        for (Domain& x : g_domains)
            if (ScriptTree::EqualsNoCase(x.name, n.key)) d = &x;
        if (!d) {
            if ((int)g_domains.size() >= kMaxDomains) {
                Log("%s: %s line %d: more than %d domains; %s skipped", kFeature, shown.c_str(), n.line, kMaxDomains, n.key.c_str());
                continue;
            }
            g_domains.push_back({n.key, {}, {}});
            d = &g_domains.back();
        }
        for (const ScriptTree::Node& c : n.children) {
            std::vector<Entry>* list = ScriptTree::EqualsNoCase(c.key, "archetypes")   ? &d->archetypes
                                       : ScriptTree::EqualsNoCase(c.key, "continents") ? &d->continents
                                                                                        : nullptr;
            std::vector<std::string> names;
            std::string why;
            if (!list) {
                Log("%s: %s line %d: unknown key '%s' in %s", kFeature, shown.c_str(), c.line, c.key.c_str(), d->name.c_str());
                continue;
            }
            if (!ScriptTree::ListOf(c, &names, &why)) {
                Log("%s: %s line %d: %s", kFeature, shown.c_str(), c.line, why.c_str());
                continue;
            }
            for (const std::string& name : names) list->push_back({name, shown, c.line});
        }
    }
}

void LoadDefinitions() {
    ModList::FolderScan scan = ModList::ScanFolder(GameFolder(), UserFolder(), kFolder);
    if (!scan.modListRead) Log("%s: the launcher's mod list (dlc_load.json) is unreadable; only the game folder is searched", kFeature);
    for (const std::string& note : scan.notes) Log("%s: %s", kFeature, note.c_str());
    for (const ModList::FolderFile& f : scan.files) {
        std::string text;
        if (!ModList::ReadWholeFile(f.path, &text)) {
            Log("%s: %s unreadable", kFeature, f.shown.c_str());
            continue;
        }
        ParseFile(text, f.shown);
    }
}

// Before each game entry (the equipment database and the map are loaded): the archetypes and
// continents. Until the provinces are read after the entry every answer is "no domain", so nothing
// is moved (the game places what its setup creates).
void ResolveBeforeEntry() {
    g_ready.store(false, std::memory_order_release);
    g_logged = 0;
    g_archetypes.clear();
    g_continents.clear();
    std::vector<std::pair<void*, int>>& continents = g_continents;
    for (size_t i = 0; i < g_domains.size(); ++i) {
        const Domain& d = g_domains[i];
        int domain = (int)i + 1, archetypes = 0, found = 0;
        for (const Entry& e : d.archetypes) {
            const void* type = Engine::EquipmentByToken(Engine::Intern(e.name.c_str()));
            if (!type) {
                Log("%s: %s (%s line %d): archetype %s not found; skipped", kFeature, d.name.c_str(), e.file.c_str(), e.line, e.name.c_str());
                continue;
            }
            const void* arch = *(const void* const*)((const uint8_t*)type + Engine::TypeArchetypeOffset());
            if (arch && arch != type) {
                Log("%s: %s (%s line %d): %s is an equipment type; its archetype is used", kFeature, d.name.c_str(), e.file.c_str(),
                    e.line, e.name.c_str());
                type = arch;
            }
            bool taken = false;
            for (const auto& a : g_archetypes) taken = taken || a.first == type;
            if (taken) {
                Log("%s: %s (%s line %d): %s is in another domain already; skipped", kFeature, d.name.c_str(), e.file.c_str(), e.line,
                    e.name.c_str());
                continue;
            }
            g_archetypes.push_back({type, domain});
            ++archetypes;
        }
        for (const Entry& e : d.continents) {
            void* c = Engine::ContinentByToken(Engine::Intern(e.name.c_str()));
            if (!c) {
                Log("%s: %s (%s line %d): continent %s not found (map/continent.txt); skipped", kFeature, d.name.c_str(), e.file.c_str(),
                    e.line, e.name.c_str());
                continue;
            }
            continents.push_back({c, domain});
            ++found;
        }
        Log("%s: %s: %d archetype(s), %d continent(s)", kFeature, d.name.c_str(), archetypes, found);
    }
    std::sort(g_archetypes.begin(), g_archetypes.end());
}

// After the entry (the game's states exist): each province's domain by its state's continent.
void ResolveAfterEntry(bool) {
    int count = Engine::ProvinceCount(), states = Engine::StateCount();
    g_provinces.assign(count > 0 ? (size_t)count : 0, 0);
    g_domainProvinces.assign(g_domains.size(), {});
    int bases = 0, readable = 0;
    for (int id = 1; id < states; ++id) {
        void* c = Engine::StateContinent(id);
        if (!c) continue;
        ++readable;
        int domain = 0;
        for (const auto& x : g_continents)
            if (x.first == c) domain = x.second;
        if (!domain) continue;
        void* const* provinces = nullptr;
        int n = 0;
        if (!Engine::StateObjectProvinces(Engine::StateById(id), &provinces, &n)) continue;
        for (int k = 0; k < n; ++k) {
            int pid = provinces[k] ? Engine::ProvinceIdOf(provinces[k]) : 0;
            if (pid > 0 && pid < count) g_provinces[(size_t)pid] = (unsigned char)domain, ++bases;
        }
    }
    for (int pid = 1; pid < count; ++pid)
        if (g_provinces[(size_t)pid]) g_domainProvinces[(size_t)g_provinces[(size_t)pid] - 1].push_back(pid);
    Log("%s: %d province(s) in states on the domains' continents (%d of %d state(s) with a continent)", kFeature, bases, readable,
        states > 0 ? states - 1 : 0);
    g_ready.store(true, std::memory_order_release);
}

}  // namespace

bool ShipDomainsInit() {
    LoadDefinitions();
    if (g_domains.empty()) {
        Log("%s: no domain definitions; ships and naval bases are left alone", kFeature);
        return true;
    }
    if (!Engine::HaveShipDomains()) {
        Log("%s: required engine functions or fields missing", kFeature);
        return false;
    }
    if (!Engine::HookShipDomains({&ArchetypeDomain, &ProvinceDomain, &Delivered, &Refused, &DomainProvinces})) {
        Log("%s: hooks could not be queued", kFeature);
        return false;
    }
    OnGameEntering(&ResolveBeforeEntry);
    OnGameEntered(&ResolveAfterEntry);
    Log("%s: %zu domain(s) from %s", kFeature, g_domains.size(), kFolder);
    return true;
}
