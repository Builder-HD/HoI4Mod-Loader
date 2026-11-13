// air_scenarios without engine access (see air_scenarios.h, NOTES.md Part L).
#include "air_scenarios.h"

#include <cctype>
#include <cstdlib>

namespace Air {

namespace {

// --- A small reader for Paradox script: key = value, key = { ... }, bare lists, # comments ---
struct Node {
    std::string key;     // empty for a bare list item
    std::string value;   // for key = value and bare items
    bool block = false;  // key = { ... }
    std::vector<Node> children;
    int line = 0;
};

struct Reader {
    const std::string& s;
    size_t i = 0;
    int line = 1;
    std::string error;
    int errorLine = 0;

    explicit Reader(const std::string& text) : s(text) {
        if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) i = 3;
    }
    void SkipSpace() {
        while (i < s.size()) {
            char c = s[i];
            if (c == '\n') {
                ++line;
                ++i;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                ++i;
            } else if (c == '#') {
                while (i < s.size() && s[i] != '\n') ++i;
            } else {
                break;
            }
        }
    }
    bool Fail(const std::string& message) {
        if (error.empty()) {
            error = message;
            errorLine = line;
        }
        return false;
    }
    // A word or a quoted string.
    bool Word(std::string* out) {
        out->clear();
        if (i >= s.size()) return Fail("unexpected end of file");
        if (s[i] == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\n') return Fail("quoted text runs past the end of the line");
                *out += s[i++];
            }
            if (i >= s.size()) return Fail("quoted text is not closed");
            ++i;
            return true;
        }
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '=' || c == '{' || c == '}' || c == '#' || c == '"') break;
            *out += c;
            ++i;
        }
        if (out->empty()) return Fail(std::string("unexpected '") + s[i] + "'");
        return true;
    }
    // Items until '}' (inside a block) or the end of the file (top level).
    bool Items(std::vector<Node>* out, bool inBlock) {
        for (;;) {
            SkipSpace();
            if (i >= s.size()) return inBlock ? Fail("a '{' is not closed") : true;
            if (s[i] == '}') {
                if (!inBlock) return Fail("a '}' without its '{'");
                ++i;
                return true;
            }
            Node n;
            n.line = line;
            std::string word;
            if (!Word(&word)) return false;
            SkipSpace();
            if (i < s.size() && s[i] == '=') {
                ++i;
                SkipSpace();
                n.key = word;
                if (i < s.size() && s[i] == '{') {
                    ++i;
                    n.block = true;
                    if (!Items(&n.children, true)) return false;
                } else if (!Word(&n.value)) {
                    return false;
                }
            } else {
                n.value = word;  // a bare list item
            }
            out->push_back(std::move(n));
        }
    }
};

bool ParseTree(const std::string& text, std::vector<Node>* out, std::string* error, int* errorLine) {
    Reader r(text);
    if (r.Items(out, false)) return true;
    *error = r.error;
    *errorLine = r.errorLine;
    return false;
}

// key = { a b c } or key = a: the words.
bool ListOf(const Node& n, std::vector<std::string>* out, std::string* why) {
    if (!n.block) {
        out->push_back(n.value);
        return true;
    }
    for (const Node& c : n.children) {
        if (!c.key.empty() || c.block) {
            *why = "'" + n.key + "' takes a list of names, e.g. " + n.key + " = { a b }";
            return false;
        }
        out->push_back(c.value);
    }
    return true;
}

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

bool EqualsNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

void ParseFile(const std::string& text, const std::string& fileName, std::vector<Definition>* out,
               std::vector<Problem>* problems) {
    std::vector<Node> top;
    std::string error;
    int errorLine = 0;
    bool syntaxOk = ParseTree(text, &top, &error, &errorLine);
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

bool ParseDlcLoad(const std::string& json, std::vector<std::string>* modFiles) {
    modFiles->clear();
    size_t k = json.find("\"enabled_mods\"");
    if (k == std::string::npos) return false;
    size_t open = json.find('[', k), close = open == std::string::npos ? open : json.find(']', open);
    if (open == std::string::npos || close == std::string::npos) return false;
    for (size_t i = open + 1; i < close; ++i) {
        if (json[i] != '"') continue;
        std::string v;
        for (++i; i < close && json[i] != '"'; ++i) {
            if (json[i] == '\\' && i + 1 < close) ++i;  // \" \\ \/
            v += json[i];
        }
        if (!v.empty()) modFiles->push_back(v);
    }
    return true;
}

bool ParseDescriptor(const std::string& text, ModDescriptor* out) {
    *out = ModDescriptor{};
    std::vector<Node> top;
    std::string error;
    int line = 0;
    ParseTree(text, &top, &error, &line);  // what was read before an error still counts
    for (const Node& n : top) {
        if (n.block) continue;
        if (EqualsNoCase(n.key, "name")) out->name = n.value;
        else if (EqualsNoCase(n.key, "path")) out->path = n.value;
        else if (EqualsNoCase(n.key, "archive")) out->archive = n.value;
        else if (EqualsNoCase(n.key, "replace_path")) out->replacePaths.push_back(n.value);
    }
    return !out->path.empty() || !out->archive.empty();
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
