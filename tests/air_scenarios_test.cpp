// Unit test for src/air_scenarios.cpp (air_scenarios without engine access, NOTES.md Part L):
// the definition parser, the launcher's mod list, descriptors, matching and the weighted pick.
#include <cstdio>
#include <string>
#include <vector>

#include "../src/air_scenarios.h"
#include "../src/mod_list.h"

static int g_checks = 0, g_failed = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("  FAIL %s\n", what);
    }
}

using namespace Air;

static std::vector<Definition> Parse(const std::string& text, std::vector<Problem>* problems) {
    std::vector<Definition> out;
    ParseFile(text, "test.txt", &out, problems);
    return out;
}

int main() {
    // --- A full file as in the docs ------------------------------------------------------------
    const std::string full =
        "\xEF\xBB\xBF# comment\n"
        "gunship_orbit = {\n"
        "\tentity = gunship_orbit_entity   # trailing comment\n"
        "\tslot = bomber_1_CAS\n"
        "\tattacker = { equipment = { CAS_gunship_unique_1 small_plane_cas_airframe } country = { GER } }\n"
        "\tweight = 10\n"
        "}\n"
        "interception = {\n"
        "\tentity = \"my interception_entity\"\n"
        "\tslot = fighter_1_vs_bomber_1\n"
        "\tattacker = { equipment = fighter_equipment_1 }\n"
        "\tdefender = { equipment = { strat_bomber_equipment_1 } country = { ENG USA } }\n"
        "}\n";
    std::vector<Problem> problems;
    std::vector<Definition> defs = Parse(full, &problems);
    Check(problems.empty(), "full file: no problems");
    Check(defs.size() == 2, "full file: two entries");
    if (defs.size() == 2) {
        const Definition& a = defs[0];
        Check(a.name == "gunship_orbit" && a.entity == "gunship_orbit_entity" && a.slot == "bomber_1_CAS", "entry 1: name, entity, slot");
        Check(a.weight == 10 && a.line == 2, "entry 1: weight and line");
        Check(a.attacker.present && a.attacker.equipment.size() == 2 && a.attacker.countries.size() == 1, "entry 1: attacker lists");
        Check(!a.defender.present, "entry 1: no defender block");
        const Definition& b = defs[1];
        Check(b.entity == "my interception_entity", "entry 2: quoted entity");
        Check(b.attacker.equipment.size() == 1 && b.attacker.equipment[0] == "fighter_equipment_1", "entry 2: single word list");
        Check(b.defender.present && b.defender.countries.size() == 2 && b.weight == 1, "entry 2: defender, default weight");
    }

    // --- Problems: the entry is left out, the others stay ---------------------------------------
    problems.clear();
    defs = Parse("bad_key = { entity = a_entity slot = bomber_1_CAS colour = red }\n"
                 "ok = { entity = b_entity slot = bomber_1_CAS }\n"
                 "no_slot = { entity = c_entity }\n"
                 "bad_entity = { entity = c_mesh slot = bomber_1_CAS }\n"
                 "bad_weight = { entity = d_entity slot = bomber_1_CAS weight = 0 }\n"
                 "bad_side = { entity = e_entity slot = bomber_1_CAS attacker = { planes = { x } } }\n"
                 "loose_word\n",
                 &problems);
    Check(defs.size() == 1 && defs[0].name == "ok", "problems: only the valid entry is kept");
    Check(problems.size() == 6, "problems: one per bad entry");
    Check(problems.size() == 6 && problems[0].line == 1 && problems[0].message.find("colour") != std::string::npos,
          "problems: unknown key named with its line");
    Check(problems.size() == 6 && problems[5].line == 7, "problems: loose word at line 7");

    problems.clear();
    defs = Parse("first = { entity = a_entity slot = bomber_1_CAS }\nbroken = { entity = b_entity\n", &problems);
    Check(defs.size() == 1 && problems.size() == 1 && problems[0].message.find("not closed") != std::string::npos,
          "syntax error: earlier entries kept, error reported");

    // --- Merge: same name replaces (case-insensitive), order kept -------------------------------
    std::vector<Definition> all, later;
    Definition x;
    x.name = "a";
    x.entity = "one_entity";
    all.push_back(x);
    x.name = "b";
    all.push_back(x);
    x.name = "A";
    x.entity = "two_entity";
    later.push_back(x);
    x.name = "c";
    later.push_back(x);
    Merge(&all, later);
    Check(all.size() == 3 && all[0].entity == "two_entity" && all[2].name == "c", "merge: replace by name, append new");

    // --- dlc_load.json and descriptors (src/mod_list.cpp, shared with the shader collector) -----
    std::vector<std::string> mods;
    Check(ModList::ParseDlcLoad("{\n  \"enabled_mods\": [\n    \"mod/a.mod\",\n    \"mod/ugc_1.mod\"\n  ],\n  \"disabled_dlcs\": []\n}", &mods) &&
              mods.size() == 2 && mods[1] == "mod/ugc_1.mod",
          "dlc_load.json: two mods in order");
    Check(ModList::ParseDlcLoad("{ \"enabled_mods\": [] }", &mods) && mods.empty(), "dlc_load.json: none");
    Check(!ModList::ParseDlcLoad("{}", &mods), "dlc_load.json: no enabled_mods");
    ModList::ModDescriptor md;
    Check(ModList::ParseDescriptor("version=\"1.0\"\ntags={\n\t\"Graphics\"\n}\nname=\"Air Test\"\nreplace_path=\"common/x\"\n"
                                   "path=\"C:/test/air\"\n",
                                   &md) &&
              md.path == "C:/test/air" && md.name == "Air Test" && md.replacePaths.size() == 1,
          "descriptor: path, name, replace_path");
    Check(!ModList::ParseDescriptor("name=\"no path\"\n", &md), "descriptor: no path");
    Check(ModList::ParseDescriptor("# path=\"commented\"\nname = \"Zip\" archive = \"mod/zip.zip\"\n", &md) && md.path.empty() &&
              md.archive == "mod/zip.zip" && md.name == "Zip",
          "descriptor: archive, comment ignored");
    Check(ModList::ParseDescriptor("tags = { path = \"inner\" }\npath = mod/plain\n", &md) && md.path == "mod/plain",
          "descriptor: path in a block ignored, bare value read");

    // --- Matching -----------------------------------------------------------------------------
    Side side;
    std::vector<Plane> none, ger = {{"fighter_equipment_1", "small_plane_airframe", "GER"}},
                             mixed = {{"fighter_equipment_0", "small_plane_airframe", "ITA"},
                                      {"CAS_gunship_unique_1", "small_plane_cas_airframe", "GER"}};
    Check(SideMatches(side, none) && SideMatches(side, ger), "match: absent side matches anything");
    side.present = true;
    side.equipment = {"small_plane_airframe"};
    Check(SideMatches(side, ger), "match: by archetype");
    side.equipment = {"FIGHTER_EQUIPMENT_1"};
    Check(SideMatches(side, ger), "match: by type, any case");
    side.countries = {"ENG"};
    Check(!SideMatches(side, ger), "match: country must hold too");
    side.equipment = {"CAS_gunship_unique_1"};
    side.countries = {"GER"};
    Check(SideMatches(side, mixed), "match: one matching plane is enough");
    side.countries = {"ITA"};
    Check(!SideMatches(side, mixed), "match: equipment and country on the same plane");
    Check(!SideMatches(side, none), "match: a present side needs a plane");
    Definition d;
    d.attacker.present = true;
    d.attacker.equipment = {"fighter_equipment_1"};
    d.defender.present = true;
    d.defender.countries = {"ENG"};
    Check(!Matches(d, ger, ger), "match: defender condition fails");
    std::vector<Plane> eng = {{"strat_bomber_equipment_1", "large_plane_airframe", "ENG"}};
    Check(Matches(d, ger, eng), "match: both sides hold");

    // --- Weighted pick ------------------------------------------------------------------------
    std::vector<int> w = {1, 3};
    int counts[2] = {0, 0};
    for (uint32_t r = 0; r < 400; ++r) ++counts[PickByWeight(w, r)];
    Check(counts[0] == 100 && counts[1] == 300, "pick: weights 1:3");
    Check(PickByWeight({}, 5) == -1, "pick: no candidates");
    Check(PickByWeight({0, 2}, 7) == 1, "pick: zero weight never picked");

    std::printf("air_scenarios_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
