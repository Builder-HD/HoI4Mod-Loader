// equipment_designers without engine access (see equipment_designers.h, NOTES.md Part N).
#include "equipment_designers.h"

#include <cctype>

#include "script_tree.h"

namespace Eqd {

using ScriptTree::EqualsNoCase;
using ScriptTree::Label;
using ScriptTree::Node;

namespace {

struct StatEntry {
    const char* name;
    const char* key;
};
// Every stat the game's table has (1.19.5: ids 0..77), by the names script uses for it.
const StatEntry kStats[] = {
    {"default_morale", "STAT_ARMY_DEFAULT_MORALE"},
    {"defense", "STAT_ARMY_DEFENSE"},
    {"breakthrough", "STAT_ARMY_BREAKTHROUGH"},
    {"hardness", "STAT_ARMY_HARDNESS"},
    {"soft_attack", "STAT_ARMY_SOFT_ATTACK"},
    {"hard_attack", "STAT_ARMY_HARD_ATTACK"},
    {"recon", "STAT_ARMY_RECON"},
    {"entrenchment", "STAT_ARMY_ENTRENCHMENT"},
    {"initiative", "STAT_ARMY_INITIATIVE"},
    {"casualty_trickleback", "STAT_ARMY_CASUALTY_TRICKLEBACK"},
    {"supply_consumption_factor", "STAT_ARMY_SUPPLY_CONSUMPTION_FACTOR"},
    {"supply_consumption", "STAT_ARMY_SUPPLY_CONSUMPTION"},
    {"suppression", "STAT_ARMY_SUPRESSION"},
    {"suppression_factor", "STAT_ARMY_SUPRESSION_FACTOR"},
    {"experience_loss_factor", "STAT_ARMY_EXPERIENCE_LOSS_FACTOR"},
    {"equipment_capture_factor", "STAT_ARMY_EQUIPMENT_CAPTURE_FACTOR"},
    {"fuel_capacity", "STAT_ARMY_FUEL_CAPACITY"},
    {"recovery_rate", "STAT_ARMY_RECOVERY"},
    {"additional_collateral_damage", "STAT_ARMY_ADDITIONAL_COLLATERAL_DAMAGE"},
    {"surface_detection", "STAT_NAVY_SURFACE_DETECTION"},
    {"sub_detection", "STAT_NAVY_SUB_DETECTION"},
    {"surface_visibility", "STAT_NAVY_SURFACE_VISIBILITY"},
    {"sub_visibility", "STAT_NAVY_SUB_VISIBILITY"},
    {"carrier_sub_detection", "STAT_NAVY_CARRIER_SUB_DETECTION"},
    {"carrier_surface_detection", "STAT_NAVY_CARRIER_SURFACE_DETECTION"},
    {"lg_attack", "STAT_NAVY_LG_ATTACK"},
    {"lg_armor_piercing", "STAT_NAVY_LG_ARMOR_PIERCING"},
    {"hg_attack", "STAT_NAVY_HG_ATTACK"},
    {"hg_armor_piercing", "STAT_NAVY_HG_ARMOR_PIERCING"},
    {"torpedo_attack", "STAT_NAVY_TORPEDO_ATTACK"},
    {"sub_attack", "STAT_NAVY_SUB_ATTACK"},
    {"anti_air_attack", "STAT_NAVY_ANTI_AIR_ATTACK"},
    {"amphibious_defense", "STAT_NAVY_AMPHIBIOUS_DEFENSE"},
    {"naval_speed", "STAT_NAVY_MAXIMUM_SPEED"},
    {"naval_range", "STAT_NAVY_RANGE"},
    {"mines_planting", "STAT_NAVY_MINES_PLANTING"},
    {"mines_sweeping", "STAT_NAVY_MINES_SWEEPING"},
    {"naval_light_gun_hit_chance_factor", "STAT_NAVY_LIGHT_GUN_HIT_CHANCE"},
    {"naval_heavy_gun_hit_chance_factor", "STAT_NAVY_HEAVY_GUN_HIT_CHANCE"},
    {"naval_torpedo_hit_chance_factor", "STAT_NAVY_TORPEDO_HIT_CHANCE"},
    {"naval_torpedo_damage_reduction_factor", "STAT_NAVY_TORPEDO_DAMAGE_REDUCTION"},
    {"naval_torpedo_enemy_critical_chance_factor", "STAT_NAVY_TORPEDO_ENEMY_CRIT_CHANCE"},
    {"naval_weather_penalty_factor", "STAT_NAVY_WEATHER_PENALTY"},
    {"convoy_raiding_coordination", "STAT_NAVY_RAIDING_COORDINATION"},
    {"patrol_coordination", "STAT_NAVY_PATROL_COORDINATION"},
    {"search_and_destroy_coordination", "STAT_NAVY_SEARCH_AND_DESTROY_COORDINATION"},
    {"air_range", "STAT_AIR_RANGE"},
    {"air_defence", "STAT_AIR_DEFENCE"},
    {"air_attack", "STAT_AIR_ATTACK"},
    {"air_agility", "STAT_AIR_AGILITY"},
    {"air_bombing", "STAT_AIR_BOMBING"},
    {"air_superiority", "STAT_AIR_SUPERIORITY"},
    {"naval_strike_attack", "STAT_AIR_NAVAL_STRIKE_ATTACK"},
    {"naval_strike_targetting", "STAT_AIR_NAVAL_STRIKE_TARGETTING"},
    {"air_ground_attack", "STAT_AIR_GROUND_ATTACK"},
    {"air_visibility_factor", "STAT_AIR_VISIBILITY_FACTOR"},
    {"railway_gun_attack", "STAT_RAILWAY_GUN_ATTACK"},
    {"railway_gun_attack_range", "STAT_RAILWAY_GUN_ATTACK_RANGE"},
    {"railway_gun_annex_ratio", "STAT_RAILWAY_GUN_ANNEX_RATIO"},
    {"railway_gun_hours_to_redistribute", "STAT_RAILWAY_GUN_HOURS_TO_REDISTRIBUTE"},
    {"max_organisation", "STAT_COMMON_MAX_ORG"},
    {"max_strength", "STAT_COMMON_MAX_STRENGTH"},
    {"maximum_speed", "STAT_COMMON_MAXIMUM_SPEED"},
    {"armor_value", "STAT_COMMON_ARMOR"},
    {"ap_attack", "STAT_COMMON_PIERCING"},
    {"reliability", "STAT_COMMON_RELIABILITY"},
    {"reliability_factor", "STAT_COMMON_RELIABILITY_FACTOR"},
    {"weight", "STAT_COMMON_WEIGHT"},
    {"thrust", "STAT_COMMON_THRUST"},
    {"fuel_consumption", "STAT_COMMON_FUEL_CONSUMPTION"},
    {"fuel_consumption_factor", "STAT_COMMON_FUEL_CONSUMPTION_FACTOR"},
    {"strategic_attack", "STAT_STRATEGIC_ATTACK"},
    {"carrier_size", "STAT_CARRIER_SIZE"},
    {"submarine_carrier_size", "STAT_SUB_CARRIER_SIZE"},
    {"acclimatization_hot_gain", "STAT_COMMON_ACCLIMATIZATION_HOT_GAIN"},
    {"acclimatization_cold_gain", "STAT_COMMON_ACCLIMATIZATION_COLD_GAIN"},
    {"night_penalty", "STAT_COMMON_NIGHT_PENALTY"},
    {"build_cost_ic", "STAT_COMMON_BUILD_COST_IC"},
};

}  // namespace

const char* StatLocKey(const std::string& name) {
    for (const StatEntry& e : kStats)
        if (EqualsNoCase(name, e.name)) return e.key;
    return nullptr;
}
int StatNameCount() { return (int)(sizeof kStats / sizeof kStats[0]); }
const char* StatName(int i) { return i >= 0 && i < StatNameCount() ? kStats[i].name : ""; }

namespace {

bool ReadSlots(const Node& n, std::vector<SlotPos>* out, std::string* why) {
    if (!n.block) {
        *why = "'slots' takes a block: slots = { <slot name> = { x = 0 y = 0 } }";
        return false;
    }
    for (const Node& s : n.children) {
        if (s.key.empty() || !s.block) {
            *why = "slots: '" + Label(s) + "' must be <slot name> = { x = 0 y = 0 }";
            return false;
        }
        SlotPos p;
        p.slot = s.key;
        bool haveX = false, haveY = false;
        for (const Node& c : s.children) {
            long v = 0;
            bool isX = EqualsNoCase(c.key, "x"), isY = EqualsNoCase(c.key, "y");
            if (!isX && !isY) {
                *why = "slots: " + s.key + ": unknown key '" + Label(c) + "' (allowed: x, y)";
                return false;
            }
            if (!ScriptTree::IntOf(c, -kMaxCoordinate, kMaxCoordinate, &v)) {
                *why = "slots: " + s.key + ": " + c.key + " must be a whole number from -" + std::to_string(kMaxCoordinate) +
                       " to " + std::to_string(kMaxCoordinate);
                return false;
            }
            (isX ? p.x : p.y) = (int)v;
            (isX ? haveX : haveY) = true;
        }
        if (!haveX || !haveY) {
            *why = "slots: " + s.key + " needs both x and y";
            return false;
        }
        for (const SlotPos& old : *out)
            if (EqualsNoCase(old.slot, p.slot)) {
                *why = "slots: " + p.slot + " is listed twice";
                return false;
            }
        out->push_back(p);
    }
    return true;
}

bool ReadStats(const Node& n, bool* hasStats, std::vector<std::string>* stats, std::string* why) {
    static const char* const kPanels[kStatPanels] = {"base", "combat", "misc"};
    if (!n.block) {
        *why = "'stats' takes a block: stats = { base = { ... } combat = { ... } misc = { ... } }";
        return false;
    }
    for (const Node& g : n.children) {
        int panel = -1;
        for (int i = 0; i < kStatPanels; ++i)
            if (EqualsNoCase(g.key, kPanels[i])) panel = i;
        if (panel < 0 || !g.block) {
            *why = "stats: '" + Label(g) + "' must be base, combat or misc = { <stat> ... }";
            return false;
        }
        if (hasStats[panel]) {
            *why = std::string("stats: ") + kPanels[panel] + " is listed twice";
            return false;
        }
        std::vector<std::string> names;
        if (!ScriptTree::ListOf(g, &names, why)) return false;
        for (const std::string& s : names)
            if (!StatLocKey(s)) {
                *why = "stats: unknown stat '" + s + "' (MODDING.md lists the names)";
                return false;
            }
        hasStats[panel] = true;
        stats[panel] = names;
    }
    return true;
}

bool ReadRoles(const Node& n, std::vector<Role>* out, std::string* why) {
    if (!n.block) {
        *why = "'roles' takes a block: roles = { <equipment> = { name = <loc key> } ... }";
        return false;
    }
    for (const Node& r : n.children) {
        if (r.key.empty() || !r.block) {
            *why = "roles: '" + Label(r) + "' must be <equipment> = { ... } (the archetype or a twin of it)";
            return false;
        }
        Role role;
        role.equipment = r.key;
        role.line = r.line;
        for (const Node& c : r.children) {
            if (EqualsNoCase(c.key, "name") && !c.block) {
                role.name = c.value;
            } else if (EqualsNoCase(c.key, "stats")) {
                if (!ReadStats(c, role.hasStats, role.stats, why)) {
                    *why = "roles: " + r.key + ": " + *why;
                    return false;
                }
            } else {
                *why = "roles: " + r.key + ": unknown key '" + Label(c) + "' (allowed: name, stats)";
                return false;
            }
        }
        for (const Role& o : *out)
            if (EqualsNoCase(o.equipment, role.equipment)) {
                *why = "roles: " + r.key + " is listed twice";
                return false;
            }
        out->push_back(role);
    }
    if (out->empty()) {
        *why = "roles: list at least one role (or leave 'roles' out)";
        return false;
    }
    return true;
}

}  // namespace

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void ParseFile(const std::string& text, const std::string& fileName, std::vector<Designer>* out, std::vector<Problem>* problems) {
    std::vector<Node> top;
    std::string error;
    int errorLine = 0;
    bool syntaxOk = ScriptTree::Parse(text, &top, &error, &errorLine);
    // Entries complete before a syntax error still count; the error is reported.
    for (const Node& e : top) {
        auto problem = [&](int line, const std::string& message) { problems->push_back({fileName, line, message}); };
        if (e.key.empty() || !e.block) {
            problem(e.line, "expected an entry: <name> = { archetypes = { ... } }");
            continue;
        }
        Designer d;
        d.name = e.key;
        d.file = fileName;
        d.line = e.line;
        std::string why;
        bool ok = true;
        for (const Node& c : e.children) {
            if (EqualsNoCase(c.key, "archetypes")) {
                ok = ScriptTree::ListOf(c, &d.archetypes, &why);
            } else if (EqualsNoCase(c.key, "window") && !c.block) {
                d.window = Lower(c.value);
                if (d.window != "land" && d.window != "air" && d.window != "naval") {
                    why = "window must be land, air or naval";
                    ok = false;
                }
            } else if (EqualsNoCase(c.key, "title") && !c.block) {
                d.title = c.value;
            } else if (EqualsNoCase(c.key, "hide_roles")) {
                if (!ScriptTree::BoolOf(c, &d.hideRoles)) {
                    why = "hide_roles must be yes or no";
                    ok = false;
                }
            } else if (EqualsNoCase(c.key, "slots")) {
                ok = ReadSlots(c, &d.slots, &why);
            } else if (EqualsNoCase(c.key, "stats")) {
                ok = ReadStats(c, d.hasStats, d.stats, &why);
            } else if (EqualsNoCase(c.key, "roles")) {
                ok = ReadRoles(c, &d.roles, &why);
            } else if (EqualsNoCase(c.key, "xp") && !c.block) {
                std::string v = Lower(c.value);
                d.xp = v == "army" ? Xp::Army : v == "navy" ? Xp::Navy : v == "air" ? Xp::Air : Xp::Game;
                if (d.xp == Xp::Game) {
                    why = "xp must be army, navy or air";
                    ok = false;
                }
            } else if (EqualsNoCase(c.key, "hide")) {
                ok = ScriptTree::ListOf(c, &d.hide, &why);
            } else {
                why = "unknown key '" + Label(c) +
                      "' (allowed: archetypes, window, title, hide_roles, slots, stats, xp, hide, roles)";
                ok = false;
            }
            if (!ok) {
                problem(c.line, d.name + ": " + why);
                break;
            }
        }
        if (!ok) continue;
        if (d.archetypes.empty()) {
            problem(e.line, d.name + ": needs 'archetypes' (the equipment archetypes this designer serves)");
            continue;
        }
        if (!d.roles.empty() && d.window != "land") {
            problem(e.line, d.name + ": roles need the land window (the tank window's role dropdown); 'roles' is ignored");
            d.roles.clear();
        }
        if (d.slots.size() > (size_t)(2 * kMaxSlotWindows)) {
            problem(e.line, d.name + ": more slot positions than the designer has slot windows (" +
                                std::to_string(kMaxSlotWindows) + " plus " + std::to_string(kMaxSlotWindows) + " fixed_)");
            continue;
        }
        out->push_back(std::move(d));
    }
    if (!syntaxOk) problems->push_back({fileName, errorLine, error + "; the rest of the file is ignored"});
}

void Merge(std::vector<Designer>* all, const std::vector<Designer>& later) {
    for (const Designer& d : later) {
        bool replaced = false;
        for (Designer& old : *all)
            if (EqualsNoCase(old.name, d.name)) {
                old = d;
                replaced = true;
                break;
            }
        if (!replaced) all->push_back(d);
    }
}

std::map<std::string, size_t> ByArchetype(const std::vector<Designer>& all, std::vector<Problem>* problems) {
    std::map<std::string, size_t> by;
    for (size_t i = 0; i < all.size(); ++i)
        for (const std::string& a : all[i].archetypes) {
            std::string key = Lower(a);
            auto it = by.find(key);
            if (it != by.end() && it->second != i)
                problems->push_back({all[i].file, all[i].line,
                                     all[i].name + ": archetype " + a + " was also listed by " + all[it->second].name +
                                         " (" + all[it->second].file + "); " + all[i].name + " serves it"});
            by[key] = i;
        }
    return by;
}

const SlotPos* FindSlot(const Designer& d, const std::string& slot) {
    for (const SlotPos& p : d.slots)
        if (EqualsNoCase(p.slot, slot)) return &p;
    return nullptr;
}

}  // namespace Eqd
