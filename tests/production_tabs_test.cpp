// Unit test for src/production_tabs.cpp (production_tabs, NOTES.md Part O): the definition parser
// and how definitions from several files and mods merge.
#include <cstdio>
#include <string>
#include <vector>

#include "../src/production_tabs.h"

static int g_checks = 0, g_failed = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("  FAIL %s\n", what);
    }
}

using namespace Ptab;

static std::vector<Tab> Parse(const std::string& text, std::vector<Problem>* problems, const char* file = "test.txt") {
    std::vector<Tab> out;
    ParseFile(text, file, &out, problems);
    return out;
}

static bool HasProblem(const std::vector<Problem>& problems, const char* part) {
    for (const Problem& p : problems)
        if (p.message.find(part) != std::string::npos) return true;
    return false;
}

static bool Has(const std::vector<Archetype>& list, const char* name) {
    for (const Archetype& a : list)
        if (a.name == name) return true;
    return false;
}

int main() {
    std::vector<Problem> problems;

    // A new tab and a vanilla tab, as in MODDING.md.
    auto tabs = Parse(
        "artillery_tab = {\n"
        "    button = artillery_tab_button\n"
        "    glow = artillery_tab_button_glow\n"
        "    add = { artillery_equipment anti_tank_equipment anti_air_equipment }\n"
        "}\n"
        "infantry = { remove = { artillery_equipment anti_tank_equipment anti_air_equipment } }\n",
        &problems);
    Check(problems.empty(), "example: no problems");
    Check(tabs.size() == 2, "example: two entries");
    if (tabs.size() == 2) {
        Check(tabs[0].vanilla == -1 && tabs[0].button == "artillery_tab_button" && tabs[0].glow == "artillery_tab_button_glow",
              "example: new tab with button and glow");
        Check(tabs[0].add.size() == 3 && Has(tabs[0].add, "anti_air_equipment") && tabs[0].remove.empty(), "example: new tab's add");
        Check(tabs[1].vanilla == kInfantry && tabs[1].remove.size() == 3 && tabs[1].add.empty(), "example: infantry's remove");
        Check(tabs[0].add[0].line == 4 && tabs[0].add[0].file == "test.txt", "example: list entries keep file and line");
    }

    // Vanilla names, case-insensitive; other names are new tabs.
    Check(VanillaIndex("infantry") == kInfantry && VanillaIndex("ARMOR") == kArmor && VanillaIndex("air") == kAir &&
              VanillaIndex("naval") == kNaval && VanillaIndex("tank") == -1,
          "vanilla names");
    Check(std::string(VanillaName(kNaval)) == "naval" && std::string(VanillaName(7)).empty(), "vanilla name by index");

    // Type lists next to the archetype lists; merged like them.
    problems.clear();
    tabs = Parse("guns = { button = g add_types = { artillery anti_tank } remove_types = { infantry } }\n"
                 "infantry = { remove_types = { artillery } add_types = { artillery } }\n",
                 &problems);
    Check(tabs.size() == 2, "types: two entries");
    if (tabs.size() == 2) {
        Check(tabs[0].addTypes.size() == 2 && Has(tabs[0].addTypes, "anti_tank") && tabs[0].removeTypes.size() == 1,
              "types: add_types and remove_types read");
        Check(tabs[0].add.empty() && tabs[0].remove.empty(), "types: archetype lists untouched");
    }
    Check(HasProblem(problems, "type artillery is in both add_types and remove_types"), "types: both lists reported");
    std::vector<Tab> merged;
    Merge(&merged, tabs, &problems);
    Merge(&merged, Parse("guns = { add_types = { anti_air artillery } }", &problems), &problems);
    Check(!merged.empty() && merged[0].addTypes.size() == 3, "types: merged without duplicates");

    // visible: a scripted trigger name, on new and vanilla tabs; first definition wins when merging.
    problems.clear();
    tabs = Parse("guns = { button = g visible = guns_visible }\nnaval = { visible = has_navy_tabs }\nbad = { button = b visible = { tag = GER } }\n",
                 &problems);
    Check(tabs.size() == 2 && tabs[0].visible == "guns_visible" && tabs[1].visible == "has_navy_tabs", "visible read on new and vanilla tabs");
    Check(HasProblem(problems, "visible takes the name of a scripted trigger"), "visible with a block reported");
    std::vector<Tab> vis;
    Merge(&vis, tabs, &problems);
    Merge(&vis, Parse("guns = { visible = other_trigger }", &problems), &problems);
    Check(!vis.empty() && vis[0].visible == "guns_visible" && HasProblem(problems, "visible = other_trigger ignored"),
          "visible: first definition kept, a different later one reported");

    // enabled: as visible, both on one tab.
    problems.clear();
    tabs = Parse("guns = { button = g visible = guns_visible enabled = guns_enabled }\nair = { enabled = has_air_tabs }\n"
                 "bad = { button = b enabled = { tag = GER } }\n",
                 &problems);
    Check(tabs.size() == 2 && tabs[0].visible == "guns_visible" && tabs[0].enabled == "guns_enabled" && tabs[1].enabled == "has_air_tabs" &&
              tabs[1].visible.empty(),
          "enabled read next to visible, on new and vanilla tabs");
    Check(HasProblem(problems, "enabled takes the name of a scripted trigger"), "enabled with a block reported");
    std::vector<Tab> en;
    Merge(&en, tabs, &problems);
    Merge(&en, Parse("guns = { enabled = other_trigger }\nair = { enabled = has_air_tabs }", &problems), &problems);
    Check(!en.empty() && en[0].enabled == "guns_enabled" && HasProblem(problems, "enabled = other_trigger ignored") &&
              !HasProblem(problems, "enabled = has_air_tabs ignored"),
          "enabled: first definition kept, a different later one reported, the same one quiet");

    // Several add blocks in one entry append.
    problems.clear();
    tabs = Parse("armor = { add = { a } add = { b } remove = { c } }", &problems);
    Check(problems.empty() && tabs.size() == 1 && tabs[0].add.size() == 2 && tabs[0].remove.size() == 1, "two add blocks append");

    // Problems: an entry with a problem is left out, the others stay.
    problems.clear();
    tabs = Parse("infantry = { button = x add = { a } }\n"
                 "t1 = { button = { x } }\n"
                 "t2 = { colour = red }\n"
                 "t3 = { add = a_single_word_is_not_a_block }\n"
                 "plain_value = 5\n"
                 "ok_tab = { button = ok_button }\n",
                 &problems);
    Check(HasProblem(problems, "vanilla tabs keep their own buttons"), "button on a vanilla tab reported");
    Check(HasProblem(problems, "button takes the name of an element"), "button block reported");
    Check(HasProblem(problems, "unknown key 'colour'"), "unknown key reported");
    Check(HasProblem(problems, "'add' takes a block"), "add without a block reported");
    Check(HasProblem(problems, "expected an entry"), "a plain value at the top reported");
    Check(tabs.size() == 1, "problem entries left out");
    Check(!tabs.empty() && tabs.back().name == "ok_tab", "the good entry kept");

    // A block inside a list: a syntax error or a list problem, either way not listed.
    problems.clear();
    tabs = Parse("t4 = { button = b4 add = { a { b } } }", &problems);
    Check(!problems.empty() && tabs.empty(), "a block inside add rejected");

    // add and remove of the same archetype in one tab: reported, both kept (remove wins at runtime).
    problems.clear();
    tabs = Parse("air = { add = { a } remove = { a } }", &problems);
    Check(HasProblem(problems, "in both add and remove"), "add and remove of the same archetype reported");
    Check(tabs.size() == 1 && tabs[0].add.size() == 1 && tabs[0].remove.size() == 1, "both lists kept");

    // A syntax error keeps the entries before it.
    problems.clear();
    tabs = Parse("armor = { add = { a } }\nbroken = { add = { b }\n", &problems);
    Check(tabs.size() == 1 && HasProblem(problems, "the rest of the file is ignored"), "syntax error keeps earlier entries");

    // Merging in load order: lists are joined without duplicates; the first button wins.
    problems.clear();
    std::vector<Tab> all;
    Merge(&all, Parse("guns = { button = guns_button add = { a b } }\ninfantry = { remove = { a } }", &problems, "mod1.txt"), &problems);
    Merge(&all, Parse("GUNS = { button = other_button glow = guns_glow add = { b c } remove = { d } }\ninfantry = { remove = { b } }",
                      &problems, "mod2.txt"),
          &problems);
    Check(all.size() == 2, "merge: same names joined (case-insensitive)");
    if (all.size() == 2) {
        Check(all[0].name == "guns" && all[0].file == "mod1.txt", "merge: the first definition's name and file");
        Check(all[0].add.size() == 3 && Has(all[0].add, "c") && all[0].remove.size() == 1, "merge: add and remove joined");
        Check(all[0].button == "guns_button" && all[0].glow == "guns_glow", "merge: first button kept, a later glow fills in");
        Check(all[1].remove.size() == 2, "merge: vanilla tab lists joined");
    }
    Check(HasProblem(problems, "button = other_button ignored"), "merge: a different later button reported");

    // Finish: a new tab needs a button; at most kMaxNewTabs.
    problems.clear();
    all.clear();
    Merge(&all, Parse("no_button = { add = { a } }\nnaval = { add = { s } }", &problems), &problems);
    std::string many;
    for (int i = 0; i < kMaxNewTabs + 2; ++i) many += "t" + std::to_string(i) + " = { button = b" + std::to_string(i) + " }\n";
    Merge(&all, Parse(many, &problems), &problems);
    Finish(&all, &problems);
    Check(HasProblem(problems, "needs 'button'"), "finish: a new tab without a button reported");
    Check(HasProblem(problems, "more than 16 new tabs"), "finish: too many new tabs reported");
    int fresh = 0;
    bool navalKept = false;
    for (const Tab& t : all) {
        fresh += t.vanilla < 0;
        navalKept = navalKept || t.vanilla == kNaval;
    }
    Check(fresh == kMaxNewTabs && navalKept, "finish: 16 new tabs and the vanilla entry kept");

    std::printf("production_tabs_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
