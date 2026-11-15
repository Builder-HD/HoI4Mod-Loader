// production_tabs without engine access (see production_tabs.h, NOTES.md Part O).
#include "production_tabs.h"

#include "script_tree.h"

namespace Ptab {

using ScriptTree::EqualsNoCase;
using ScriptTree::Label;
using ScriptTree::Node;

namespace {

const char* const kVanillaNames[kVanillaTabs] = {"infantry", "armor", "air", "naval"};

bool ReadList(const Node& n, const std::string& fileName, std::vector<Archetype>* out, std::string* why) {
    std::vector<std::string> names;
    if (!n.block) {
        *why = "'" + n.key + "' takes a block of names: " + n.key + " = { ... }";
        return false;
    }
    if (!ScriptTree::ListOf(n, &names, why)) return false;
    for (const std::string& s : names) out->push_back({s, fileName, n.line});
    return true;
}

bool Listed(const std::vector<Archetype>& list, const std::string& name) {
    for (const Archetype& a : list)
        if (EqualsNoCase(a.name, name)) return true;
    return false;
}

// Appends the names not listed yet.
void AddNew(std::vector<Archetype>* to, const std::vector<Archetype>& from) {
    for (const Archetype& a : from)
        if (!Listed(*to, a.name)) to->push_back(a);
}

}  // namespace

const char* VanillaName(int tab) { return tab >= 0 && tab < kVanillaTabs ? kVanillaNames[tab] : ""; }

int VanillaIndex(const std::string& name) {
    for (int i = 0; i < kVanillaTabs; ++i)
        if (EqualsNoCase(name, kVanillaNames[i])) return i;
    return -1;
}

void ParseFile(const std::string& text, const std::string& fileName, std::vector<Tab>* out, std::vector<Problem>* problems) {
    std::vector<Node> top;
    std::string error;
    int errorLine = 0;
    bool syntaxOk = ScriptTree::Parse(text, &top, &error, &errorLine);
    // Entries complete before a syntax error still count; the error is reported.
    for (const Node& e : top) {
        auto problem = [&](int line, const std::string& message) { problems->push_back({fileName, line, message}); };
        if (e.key.empty() || !e.block) {
            problem(e.line, "expected an entry: <tab> = { add = { ... } remove = { ... } }");
            continue;
        }
        Tab t;
        t.name = e.key;
        t.vanilla = VanillaIndex(e.key);
        t.file = fileName;
        t.line = e.line;
        std::string why;
        bool ok = true;
        for (const Node& c : e.children) {
            bool isButton = EqualsNoCase(c.key, "button"), isGlow = EqualsNoCase(c.key, "glow");
            if (EqualsNoCase(c.key, "add")) {
                ok = ReadList(c, fileName, &t.add, &why);
            } else if (EqualsNoCase(c.key, "remove")) {
                ok = ReadList(c, fileName, &t.remove, &why);
            } else if (EqualsNoCase(c.key, "visible") || EqualsNoCase(c.key, "enabled")) {
                std::string key = EqualsNoCase(c.key, "visible") ? "visible" : "enabled";
                if (c.block || c.value.empty()) {
                    why = key + " takes the name of a scripted trigger: " + key + " = my_tab_" + key +
                          " (write the trigger in common/scripted_triggers)";
                    ok = false;
                } else {
                    (key == "visible" ? t.visible : t.enabled) = c.value;
                }
            } else if (EqualsNoCase(c.key, "add_types")) {
                ok = ReadList(c, fileName, &t.addTypes, &why);
            } else if (EqualsNoCase(c.key, "remove_types")) {
                ok = ReadList(c, fileName, &t.removeTypes, &why);
            } else if ((isButton || isGlow) && t.vanilla >= 0) {
                why = c.key + ": the vanilla tabs keep their own buttons (change them in countryproductionlineview.gui)";
                ok = false;
            } else if ((isButton || isGlow) && (c.block || c.value.empty())) {
                why = c.key + " takes the name of an element: " + c.key + " = my_tab_button";
                ok = false;
            } else if (isButton) {
                t.button = c.value;
            } else if (isGlow) {
                t.glow = c.value;
            } else {
                why = "unknown key '" + Label(c) + "' (allowed: add, remove, add_types, remove_types, visible, enabled" +
                      (t.vanilla >= 0 ? "" : ", button, glow") + ")";
                ok = false;
            }
            if (!ok) {
                problem(c.line, t.name + ": " + why);
                break;
            }
        }
        if (!ok) continue;
        for (const Archetype& a : t.add)
            if (Listed(t.remove, a.name)) problem(a.line, t.name + ": " + a.name + " is in both add and remove; remove wins");
        for (const Archetype& a : t.addTypes)
            if (Listed(t.removeTypes, a.name))
                problem(a.line, t.name + ": type " + a.name + " is in both add_types and remove_types; remove_types wins");
        out->push_back(std::move(t));
    }
    if (!syntaxOk) problems->push_back({fileName, errorLine, error + "; the rest of the file is ignored"});
}

void Merge(std::vector<Tab>* all, const std::vector<Tab>& later, std::vector<Problem>* problems) {
    for (const Tab& t : later) {
        Tab* old = nullptr;
        for (Tab& o : *all)
            if (EqualsNoCase(o.name, t.name)) old = &o;
        if (!old) {
            Tab copy;
            copy.name = t.name;
            copy.vanilla = t.vanilla;
            copy.button = t.button;
            copy.glow = t.glow;
            copy.visible = t.visible;
            copy.enabled = t.enabled;
            copy.file = t.file;
            copy.line = t.line;
            AddNew(&copy.add, t.add);
            AddNew(&copy.remove, t.remove);
            AddNew(&copy.addTypes, t.addTypes);
            AddNew(&copy.removeTypes, t.removeTypes);
            all->push_back(std::move(copy));
            continue;
        }
        auto keep = [&](std::string* mine, const std::string& theirs, const char* key) {
            if (theirs.empty() || EqualsNoCase(*mine, theirs)) return;
            if (mine->empty()) {
                *mine = theirs;
                return;
            }
            problems->push_back({t.file, t.line, t.name + ": " + key + " = " + theirs + " ignored; " + old->file + " already set " +
                                                     key + " = " + *mine});
        };
        keep(&old->button, t.button, "button");
        keep(&old->glow, t.glow, "glow");
        keep(&old->visible, t.visible, "visible");
        keep(&old->enabled, t.enabled, "enabled");
        AddNew(&old->add, t.add);
        AddNew(&old->remove, t.remove);
        AddNew(&old->addTypes, t.addTypes);
        AddNew(&old->removeTypes, t.removeTypes);
    }
}

void Finish(std::vector<Tab>* all, std::vector<Problem>* problems) {
    std::vector<Tab> kept;
    int fresh = 0;
    for (Tab& t : *all) {
        if (t.vanilla < 0 && t.button.empty()) {
            problems->push_back({t.file, t.line, t.name + ": a new tab needs 'button' (a buttonType in the production view); skipped"});
            continue;
        }
        if (t.vanilla < 0 && ++fresh > kMaxNewTabs) {
            problems->push_back({t.file, t.line, t.name + ": more than " + std::to_string(kMaxNewTabs) + " new tabs; skipped"});
            continue;
        }
        kept.push_back(std::move(t));
    }
    all->swap(kept);
}

}  // namespace Ptab
