// air_scenarios without engine access (see air_scenarios.h, NOTES.md Part L).
#include "air_scenarios.h"

#include "script_tree.h"

#include <cctype>
#include <cstdlib>

namespace Air {

namespace {

using ScriptTree::ListOf;
using ScriptTree::Node;

bool ReadSide(const Node& n, Side* side, std::string* why) {
    if (!n.block) {
        *why = "'" + n.key + "' takes a block: " + n.key + " = { equipment = { ... } country = { ... } }";
        return false;
    }
    side->present = true;
    for (const Node& c : n.children) {
        if (EqualsNoCase(c.key, "equipment")) {
            if (!ListOf(c, &side->equipment, why)) return false;
        } else if (EqualsNoCase(c.key, "country")) {
            if (!ListOf(c, &side->countries, why)) return false;
        } else {
            *why = "unknown key '" + (c.key.empty() ? c.value : c.key) + "' in '" + n.key + "' (allowed: equipment, country)";
            return false;
        }
    }
    return true;
}

bool EndsWithNoCase(const std::string& s, const char* suffix) {
    size_t n = std::char_traits<char>::length(suffix);
    return s.size() > n && EqualsNoCase(s.substr(s.size() - n), suffix);
}

}  // namespace

bool EqualsNoCase(const std::string& a, const std::string& b) { return ScriptTree::EqualsNoCase(a, b); }

void ParseFile(const std::string& text, const std::string& fileName, std::vector<Definition>* out,
               std::vector<Problem>* problems) {
    std::vector<Node> top;
    std::string error;
    int errorLine = 0;
    bool syntaxOk = ScriptTree::Parse(text, &top, &error, &errorLine);
    // Entries complete before a syntax error still count; the error is reported.
    for (const Node& e : top) {
        auto problem = [&](int line, const std::string& message) { problems->push_back({fileName, line, message}); };
        if (e.key.empty() || !e.block) {
            problem(e.line, "expected an entry: <name> = { entity = ... slot = ... }");
            continue;
        }
        Definition d;
        d.name = e.key;
        d.file = fileName;
        d.line = e.line;
        std::string why;
        bool ok = true;
        for (const Node& c : e.children) {
            if (EqualsNoCase(c.key, "entity") && !c.block) {
                d.entity = c.value;
            } else if (EqualsNoCase(c.key, "slot") && !c.block) {
                d.slot = c.value;
            } else if (EqualsNoCase(c.key, "weight") && !c.block) {
                char* end = nullptr;
                long w = std::strtol(c.value.c_str(), &end, 10);
                if (c.value.empty() || *end || w < 1 || w > 1000000) {
                    why = "weight must be a whole number from 1 to 1000000";
                    ok = false;
                } else {
                    d.weight = (int)w;
                }
            } else if (EqualsNoCase(c.key, "attacker")) {
                ok = ReadSide(c, &d.attacker, &why);
            } else if (EqualsNoCase(c.key, "defender")) {
                ok = ReadSide(c, &d.defender, &why);
            } else {
                why = "unknown key '" + (c.key.empty() ? c.value : c.key) + "' (allowed: entity, slot, attacker, defender, weight)";
                ok = false;
            }
            if (!ok) {
                problem(c.line, d.name + ": " + why);
                break;
            }
        }
        if (!ok) continue;
        if (d.entity.empty() || d.slot.empty()) {
            problem(e.line, d.name + ": needs both 'entity' and 'slot'");
            continue;
        }
        if (!EndsWithNoCase(d.entity, "_entity")) {
            problem(e.line, d.name + ": the entity's name must end with _entity (as the game's scenario entities do): " + d.entity);
            continue;
        }
        out->push_back(std::move(d));
    }
    if (!syntaxOk) problems->push_back({fileName, errorLine, error + "; the rest of the file is ignored"});
}

void Merge(std::vector<Definition>* all, const std::vector<Definition>& later) {
    for (const Definition& d : later) {
        bool replaced = false;
        for (Definition& old : *all)
            if (EqualsNoCase(old.name, d.name)) {
                old = d;
                replaced = true;
                break;
            }
        if (!replaced) all->push_back(d);
    }
}

bool SideMatches(const Side& side, const std::vector<Plane>& planes) {
    if (!side.present || (side.equipment.empty() && side.countries.empty())) return true;
    for (const Plane& p : planes) {
        bool equipment = side.equipment.empty();
        for (const std::string& e : side.equipment)
            if (EqualsNoCase(e, p.equipment) || (!p.archetype.empty() && EqualsNoCase(e, p.archetype))) equipment = true;
        bool country = side.countries.empty();
        for (const std::string& c : side.countries)
            if (!p.country.empty() && EqualsNoCase(c, p.country)) country = true;
        if (equipment && country) return true;
    }
    return false;
}

bool Matches(const Definition& d, const std::vector<Plane>& attackers, const std::vector<Plane>& defenders) {
    return SideMatches(d.attacker, attackers) && SideMatches(d.defender, defenders);
}

int PickByWeight(const std::vector<int>& weights, uint32_t r) {
    uint64_t total = 0;
    for (int w : weights) total += (uint64_t)(w > 0 ? w : 0);
    if (total == 0) return -1;
    uint64_t x = r % total;
    for (size_t i = 0; i < weights.size(); ++i) {
        uint64_t w = (uint64_t)(weights[i] > 0 ? weights[i] : 0);
        if (x < w) return (int)i;
        x -= w;
    }
    return (int)weights.size() - 1;
}

}  // namespace Air
