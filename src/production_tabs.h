// production_tabs (NOTES.md Part O): the parts without engine access, so they can be unit tested
// (tests/production_tabs_test.cpp): the definition files' parser and how definitions merge.
#pragma once

#include <string>
#include <vector>

namespace Ptab {

// The vanilla "add production line" tabs, as the definitions name them.
enum VanillaTab { kInfantry = 0, kArmor = 1, kAir = 2, kNaval = 3, kVanillaTabs = 4 };
const char* VanillaName(int tab);           // "infantry", "armor", "air", "naval"
int VanillaIndex(const std::string& name);  // -1 for any other name (a new tab)

// New tabs the loader can tell apart (each gets one mask bit of its own, see NOTES.md O5).
const int kMaxNewTabs = 16;

struct Archetype {
    std::string name;
    std::string file;  // where it was listed (for messages)
    int line = 0;
};

struct Tab {
    std::string name;  // the entry's key
    int vanilla = -1;  // a VanillaTab, or -1 for a new tab
    std::string button, glow;  // new tabs: element names in the production view (glow optional)
    std::string visible;       // optional: a scripted trigger; the button shows while it is true
    std::string enabled;       // optional: a scripted trigger; the button is disabled while it is false
    std::vector<Archetype> add, remove;            // archetypes
    std::vector<Archetype> addTypes, removeTypes;  // equipment types (`type = ...` names), same entries
    std::string file;  // where it was first defined
    int line = 0;
};

struct Problem {
    std::string file;
    int line = 0;
    std::string message;
};

// Parses one definition file. Valid entries go to *out; an entry with a problem is left out and
// the problem added (file and line).
void ParseFile(const std::string& text, const std::string& fileName, std::vector<Tab>* out, std::vector<Problem>* problems);

// Merges entries in load order: the same tab (case-insensitive) in a later file or mod adds its
// `add` and `remove` lists to the earlier ones; `button`, `glow`, `visible` and `enabled` keep the
// first definition's (a different later one is reported). Order of first appearance is kept.
void Merge(std::vector<Tab>* all, const std::vector<Tab>& later, std::vector<Problem>* problems);

// After every file: new tabs without a button are dropped, the ones past kMaxNewTabs too (reported).
void Finish(std::vector<Tab>* all, std::vector<Problem>* problems);

}  // namespace Ptab
