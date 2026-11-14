// equipment_designers (NOTES.md Part N): the parts without engine access, so they can be unit
// tested (tests/equipment_designers_test.cpp): the definition files' parser, how definitions
// merge, and which designer serves an archetype.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace Eqd {

// Where one slot window goes in the designer, in the coordinates of the window's own
// pos_custom_module_slot_window_N positions.
struct SlotPos {
    std::string slot;  // the archetype's module slot name (as in module_slots)
    int x = 0;
    int y = 0;
};

struct Designer {
    std::string name;                     // the entry's key
    std::vector<std::string> archetypes;  // archetype names (required)
    std::string window = "land";          // land | air | naval; only land is served for now
    std::string title;                    // loc key; empty = the window's own title
    bool hideRoles = true;                // hide the tank role dropdown
    std::vector<SlotPos> slots;
    std::string file;  // for messages
    int line = 0;
};

struct Problem {
    std::string file;
    int line = 0;
    std::string message;
};

// The positions the GUI patch gives the slot windows (pos_custom/fixed_module_slot_window_N):
// the engine creates at most this many of each.
const int kMaxSlotWindows = 20;
const int kMaxCoordinate = 4000;  // |x|, |y|

// Parses one definition file. Valid entries go to *out; an entry with a problem is left out and
// the problem added (file and line).
void ParseFile(const std::string& text, const std::string& fileName, std::vector<Designer>* out, std::vector<Problem>* problems);

// Merges entries in load order: an entry of the same name (case-insensitive) replaces the earlier
// one. Order of first appearance is kept.
void Merge(std::vector<Designer>* all, const std::vector<Designer>& later);

// Archetype name (lower case) -> index into `all`. An archetype claimed by two designers goes to
// the later one; the clash is reported.
std::map<std::string, size_t> ByArchetype(const std::vector<Designer>& all, std::vector<Problem>* problems);

// The position for a slot, or null.
const SlotPos* FindSlot(const Designer& d, const std::string& slot);

std::string Lower(std::string s);

}  // namespace Eqd
