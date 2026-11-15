// Unit test for src/equipment_designers.cpp and the tank_designer_view.gui patch (equipment_designers,
// NOTES.md Part N): the definition parser, merging, which designer serves an archetype, and the
// added slot window positions.
//   equipment_designers_test                 hand-written snippets
//   equipment_designers_test <file.gui> ...  also patches real tank_designer_view.gui copies
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/engine.h"
#include "../src/equipment_designers.h"

static int g_checks = 0, g_failed = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("  FAIL %s\n", what);
    }
}

using namespace Eqd;

static std::vector<Designer> Parse(const std::string& text, std::vector<Problem>* problems) {
    std::vector<Designer> out;
    ParseFile(text, "test.txt", &out, problems);
    return out;
}

static bool HasProblem(const std::vector<Problem>& problems, const char* part) {
    for (const Problem& p : problems)
        if (p.message.find(part) != std::string::npos) return true;
    return false;
}

static size_t Count(const std::string& s, const std::string& needle) {
    size_t n = 0;
    for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1)) ++n;
    return n;
}

static void CheckPatched(const std::string& original, const char* label) {
    int custom = -1, fixed = -1;
    std::string out = Engine::PatchDesignerViewGui(original, kMaxSlotWindows, &custom, &fixed);
    std::string what = std::string(label) + ": patched";
    Check(!out.empty(), what.c_str());
    for (int k = 0; k < 2; ++k)
        for (int n = 0; n < kMaxSlotWindows; ++n) {
            std::string name = std::string(k ? "\"pos_fixed_module_slot_window_" : "\"pos_custom_module_slot_window_") +
                               std::to_string(n) + "\"";
            if (Count(out, name) != 1) {
                what = std::string(label) + ": exactly one " + name;
                Check(false, what.c_str());
            }
        }
    int braces = 0;
    for (char c : out) braces += c == '{' ? 1 : c == '}' ? -1 : 0;
    what = std::string(label) + ": braces balanced";
    Check(braces == 0, what.c_str());
    int c2 = -1, f2 = -1;
    what = std::string(label) + ": patching twice changes nothing";
    Check(Engine::PatchDesignerViewGui(out, kMaxSlotWindows, &c2, &f2).empty() && c2 == kMaxSlotWindows &&
              f2 == kMaxSlotWindows,
          what.c_str());
    std::printf("  %s: %d custom and %d fixed slot window(s) before\n", label, custom, fixed);
}

int main(int argc, char** argv) {
    // --- A full file as in the docs ------------------------------------------------------------
    const std::string full =
        "\xEF\xBB\xBF# comment\n"
        "small_arms_designer = {\n"
        "\tarchetypes = { infantry_equipment support_equipment }\n"
        "\twindow = land\n"
        "\ttitle = SMALL_ARMS_DESIGNER_TITLE\n"
        "\thide_roles = no\n"
        "\tslots = {\n"
        "\t\tsmall_arms_slot = { x = 1 y = 1 }\n"
        "\t\tfixed_sight_slot = { x = -20 y = 300 }\n"
        "\t}\n"
        "}\n"
        "artillery_designer = { archetypes = carriage_equipment }\n";
    std::vector<Problem> problems;
    std::vector<Designer> defs = Parse(full, &problems);
    Check(problems.empty(), "full: no problems");
    Check(defs.size() == 2, "full: two designers");
    if (defs.size() == 2) {
        const Designer& d = defs[0];
        Check(d.name == "small_arms_designer" && d.archetypes.size() == 2 && d.archetypes[1] == "support_equipment",
              "full: name and archetypes");
        Check(d.window == "land" && d.title == "SMALL_ARMS_DESIGNER_TITLE" && !d.hideRoles, "full: window, title, hide_roles");
        Check(d.slots.size() == 2 && d.slots[1].slot == "fixed_sight_slot" && d.slots[1].x == -20 && d.slots[1].y == 300,
              "full: slot positions");
        Check(FindSlot(d, "SMALL_ARMS_SLOT") && !FindSlot(d, "other_slot"), "full: FindSlot ignores case");
        Check(d.line == 2 && d.file == "test.txt", "full: file and line");
        const Designer& a = defs[1];
        Check(a.archetypes.size() == 1 && a.hideRoles && a.window == "land" && a.title.empty() && a.slots.empty(),
              "full: defaults (single archetype, roles hidden, land)");
    }

    // --- Problems: each entry is checked on its own --------------------------------------------
    problems.clear();
    defs = Parse(
        "no_archetypes = { title = X }\n"
        "bad_window = { archetypes = { a } window = space }\n"
        "bad_bool = { archetypes = { a } hide_roles = maybe }\n"
        "bad_key = { archetypes = { a } colour = red }\n"
        "bad_slot = { archetypes = { a } slots = { s = { x = 1 } } }\n"
        "far_slot = { archetypes = { a } slots = { s = { x = 5000 y = 1 } } }\n"
        "twice_slot = { archetypes = { a } slots = { s = { x = 1 y = 1 } S = { x = 2 y = 2 } } }\n"
        "bad_list = { archetypes = { a = b } }\n"
        "air = { archetypes = { plane } window = air }\n"
        "fine = { archetypes = { a } }\n"
        "loose_value\n"
        "broken = { archetypes = { a }\n",
        &problems);
    Check(defs.size() == 2 && defs[0].name == "air" && defs[1].name == "fine", "problems: only the valid entries remain");
    Check(HasProblem(problems, "needs 'archetypes'"), "problems: archetypes required");
    Check(HasProblem(problems, "window must be land, air or naval"), "problems: window value");
    Check(HasProblem(problems, "hide_roles must be yes or no"), "problems: hide_roles value");
    Check(HasProblem(problems, "unknown key 'colour'"), "problems: unknown key");
    Check(HasProblem(problems, "needs both x and y"), "problems: slot needs x and y");
    Check(HasProblem(problems, "whole number from -4000 to 4000"), "problems: coordinate range");
    Check(HasProblem(problems, "listed twice"), "problems: a slot listed twice");
    Check(HasProblem(problems, "takes a list of names"), "problems: archetypes must be a list");
    Check(HasProblem(problems, "expected an entry"), "problems: a loose value");
    Check(HasProblem(problems, "the rest of the file is ignored"), "problems: syntax error reported");

    // --- stats, xp, hide ------------------------------------------------------------------------
    problems.clear();
    defs = Parse(
        "full = {\n"
        "\tarchetypes = { a }\n"
        "\txp = NAVY\n"
        "\tstats = { base = { reliability maximum_speed } combat = { Soft_Attack ap_attack armor_value } misc = { } }\n"
        "\thide = { select3d_button historical_design_button }\n"
        "}\n"
        "bad_stat = { archetypes = { a } stats = { combat = { soft_attack laser_attack } } }\n"
        "bad_panel = { archetypes = { a } stats = { extra = { soft_attack } } }\n"
        "twice_panel = { archetypes = { a } stats = { base = { defense } base = { hardness } } }\n"
        "bad_xp = { archetypes = { a } xp = research }\n",
        &problems);
    Check(defs.size() == 1 && defs[0].name == "full", "options: only the valid entry remains");
    if (defs.size() == 1) {
        const Designer& d = defs[0];
        Check(d.xp == Xp::Navy, "options: xp = navy (any case)");
        Check(d.hasStats[kStatsBase] && d.hasStats[kStatsCombat] && d.hasStats[kStatsMisc], "options: all three panels given");
        Check(d.stats[kStatsBase].size() == 2 && d.stats[kStatsCombat].size() == 3 && d.stats[kStatsMisc].empty(),
              "options: stat lists kept in order, misc empty");
        Check(d.hide.size() == 2 && d.hide[1] == "historical_design_button", "options: hide list");
    }
    Check(HasProblem(problems, "unknown stat 'laser_attack'"), "options: unknown stat");
    Check(HasProblem(problems, "must be base, combat or misc"), "options: unknown panel");
    Check(HasProblem(problems, "base is listed twice"), "options: a panel twice");
    Check(HasProblem(problems, "xp must be army, navy or air"), "options: xp value");
    problems.clear();
    defs = Parse("plain = { archetypes = { a } }\n", &problems);
    Check(defs.size() == 1 && defs[0].xp == Xp::Game && !defs[0].hasStats[kStatsBase] && !defs[0].hasStats[kStatsCombat] &&
              !defs[0].hasStats[kStatsMisc] && defs[0].hide.empty(),
          "options: all optional (the game's experience, the window's stats, nothing hidden)");

    // --- roles ------------------------------------------------------------------------------------
    problems.clear();
    defs = Parse(
        "guns = {\n"
        "\tarchetypes = { field_gun }\n"
        "\troles = {\n"
        "\t\tfield_gun = { name = ROLE_HOWITZER }\n"
        "\t\tfield_gun_at = { name = ROLE_AT stats = { combat = { hard_attack ap_attack } } }\n"
        "\t\tfield_gun_aa = { }\n"
        "\t}\n"
        "}\n"
        "twice_role = { archetypes = { a } roles = { x = { } X = { } } }\n"
        "bad_role_key = { archetypes = { a } roles = { x = { icon = 3 } } }\n"
        "loose_role = { archetypes = { a } roles = { x } }\n"
        "empty_roles = { archetypes = { a } roles = { } }\n"
        "air_roles = { archetypes = { p } window = air roles = { p = { } } }\n",
        &problems);
    Check(defs.size() == 2 && defs[0].name == "guns" && defs[1].name == "air_roles", "roles: the valid entries remain");
    if (defs.size() == 2) {
        const std::vector<Role>& r = defs[0].roles;
        Check(r.size() == 3 && r[0].equipment == "field_gun" && r[0].name == "ROLE_HOWITZER" && r[2].name.empty(),
              "roles: kept in order, name optional");
        Check(r.size() == 3 && r[1].hasStats[kStatsCombat] && r[1].stats[kStatsCombat].size() == 2 && !r[1].hasStats[kStatsBase],
              "roles: per-role stats");
        Check(defs[1].roles.empty(), "roles: dropped outside the land window");
    }
    Check(HasProblem(problems, "roles: X is listed twice"), "roles: a role twice (any case)");
    Check(HasProblem(problems, "unknown key 'icon'"), "roles: unknown role key");
    Check(HasProblem(problems, "must be <equipment> = { ... }"), "roles: a loose value");
    Check(HasProblem(problems, "list at least one role"), "roles: an empty list");
    Check(HasProblem(problems, "roles need the land window"), "roles: other windows reported");

    // --- The stat names ---------------------------------------------------------------------------
    Check(StatNameCount() == 78, "stats: 78 names (the game's table has ids 0..77)");
    Check(std::string(StatLocKey("ap_attack")) == "STAT_COMMON_PIERCING" && std::string(StatLocKey("ARMOR_VALUE")) == "STAT_COMMON_ARMOR" &&
              std::string(StatLocKey("suppression")) == "STAT_ARMY_SUPRESSION" && !StatLocKey("laser_attack"),
          "stats: script names map to the game's loc keys");
    bool unique = true;
    for (int i = 0; i < StatNameCount(); ++i)
        for (int j = i + 1; j < StatNameCount(); ++j)
            unique = unique && std::string(StatName(i)) != StatName(j) &&
                     std::string(StatLocKey(StatName(i))) != StatLocKey(StatName(j));
    Check(unique, "stats: names and keys unique");

    // --- Merge and archetype owners -------------------------------------------------------------
    std::vector<Designer> all;
    problems.clear();
    Merge(&all, Parse("one = { archetypes = { a b } }\ntwo = { archetypes = { c } }\n", &problems));
    Merge(&all, Parse("ONE = { archetypes = { a } title = T }\nthree = { archetypes = { C } }\n", &problems));
    Check(all.size() == 3 && all[0].title == "T" && all[0].archetypes.size() == 1, "merge: a later entry of the same name replaces it");
    std::map<std::string, size_t> by = ByArchetype(all, &problems);
    Check(by.size() == 2 && by["a"] == 0 && by["c"] == 2, "by archetype: later designer wins, names lower case");
    Check(HasProblem(problems, "archetype C was also listed by two"), "by archetype: the clash is reported");

    // --- The GUI patch on a snippet -------------------------------------------------------------
    const std::string gui =
        "guiTypes = {\r\n"
        "\tcontainerWindowType = {\r\n"
        "\t\tname = \"tank_designer_view\"\r\n"
        "\t\tcontainerWindowType = {\r\n"
        "\t\t\tname = \"module_area\"\r\n"
        "\t\t\t@col_0 = 1\r\n"
        "\t\t\tpositionType = {\r\n"
        "\t\t\t\tname = \"ypos_module_row_0\"\r\n"
        "\t\t\t\tposition = { x=0 y=1 }\r\n"
        "\t\t\t}\r\n"
        "\t\t\tpositionType = {\r\n"
        "\t\t\t\tname = \"pos_custom_module_slot_window_0\"\r\n"
        "\t\t\t\tposition = { x=@col_0 y=1 }\r\n"
        "\t\t\t}\r\n"
        "\t\t\tpositionType = { name = \"pos_custom_module_slot_window_1\" position = { x = 80 y = 1 } }\r\n"
        "\t\t\tpositionType = { name = \"pos_custom_module_slot_window_3\" position = { x = 80 y = 300 } }\r\n"
        "\t\t\ticonType = { name = \"after\" }\r\n"
        "\t\t}\r\n"
        "\t}\r\n"
        "}\r\n";
    int custom = -1, fixed = -1;
    std::string out = Engine::PatchDesignerViewGui(gui, kMaxSlotWindows, &custom, &fixed);
    Check(custom == 2 && fixed == 0, "gui: counts the windows the engine makes (stops at the gap)");
    Check(Count(out, "\"pos_custom_module_slot_window_3\"") == 1 && Count(out, "\"pos_custom_module_slot_window_2\"") == 1 &&
              Count(out, "\"pos_custom_module_slot_window_19\"") == 1 && Count(out, "\"pos_fixed_module_slot_window_19\"") == 1 &&
              Count(out, "\"pos_custom_module_slot_window_20\"") == 0,
          "gui: fills the gap and adds up to 19, no duplicates");
    Check(Count(out, "position = { x=@col_0 y=1 }") == 1 + 2 * kMaxSlotWindows - 3, "gui: added ones copy window 0's position");
    size_t added = out.find("hoi4_loader: equipment_designers");
    Check(added != std::string::npos && added > out.find("slot_window_3") && added < out.find("\"after\""),
          "gui: inserted after the last slot position, inside its container");
    Check(out.find("\r\n\t\t\tpositionType = { name = \"pos_fixed_module_slot_window_0\"") != std::string::npos,
          "gui: CRLF and the container's indentation kept");
    Check(Engine::PatchDesignerViewGui("guiTypes = { }", kMaxSlotWindows, &custom, &fixed).empty(),
          "gui: a file without slot positions is left alone");
    CheckPatched(gui, "snippet");

    for (int i = 1; i < argc; ++i) {
        std::ifstream f(argv[i], std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        if (ss.str().empty()) {
            Check(false, (std::string("unreadable: ") + argv[i]).c_str());
            continue;
        }
        CheckPatched(ss.str(), argv[i]);
    }

    std::printf("equipment_designers_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
