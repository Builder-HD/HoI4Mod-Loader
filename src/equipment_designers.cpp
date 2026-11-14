// equipment_designers without engine access (see equipment_designers.h, NOTES.md Part N).
#include "equipment_designers.h"

#include <cctype>

#include "script_tree.h"

namespace Eqd {

using ScriptTree::EqualsNoCase;
using ScriptTree::Label;
using ScriptTree::Node;

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
            } else {
                why = "unknown key '" + Label(c) + "' (allowed: archetypes, window, title, hide_roles, slots)";
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
