// air_scenarios (NOTES.md Part L): the parts without engine access, so they can be unit tested
// (tests/air_scenarios_test.cpp): the definition files' parser, the launcher's mod list, and
// which definition plays for a scenario.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Air {

// One side of a definition: an attacking or defending plane must have one of `equipment` (type or
// archetype names) and one of `countries` (tags of the equipment's creator). Empty lists match
// anything. `present` is false when the block is left out (the side matches anything).
struct Side {
    bool present = false;
    std::vector<std::string> equipment;
    std::vector<std::string> countries;
};

struct Definition {
    std::string name;    // the entry's key
    std::string entity;  // the scenario's entity (must end with "_entity")
    std::string slot;    // the game's scenario it plays in, e.g. bomber_1_CAS
    Side attacker;
    Side defender;
    int weight = 1;
    std::string file;    // for messages
    int line = 0;
};

struct Problem {
    std::string file;
    int line = 0;
    std::string message;
};

// Parses one definition file (Paradox script). Valid entries go to *out; an entry with a problem
// is left out and the problem added to *problems (file and line).
void ParseFile(const std::string& text, const std::string& fileName, std::vector<Definition>* out,
               std::vector<Problem>* problems);

// Merges entries in load order: an entry of the same name (case-insensitive) replaces the
// earlier one. Order of first appearance is kept.
void Merge(std::vector<Definition>* all, const std::vector<Definition>& later);

// The launcher's dlc_load.json: the enabled mods' descriptor paths ("mod/<name>.mod"), in order.
bool ParseDlcLoad(const std::string& json, std::vector<std::string>* modFiles);

// A mod descriptor (.mod): its folder (`path`, or the legacy `archive` for zipped mods),
// its name and its replace_path entries.
struct ModDescriptor {
    std::string name;
    std::string path;
    std::string archive;
    std::vector<std::string> replacePaths;
};
bool ParseDescriptor(const std::string& text, ModDescriptor* out);

// One plane of a scenario side, as the loader read it from the game.
struct Plane {
    std::string equipment;  // its equipment type, e.g. small_plane_cas_airframe_1
    std::string archetype;  // the type's archetype, e.g. small_plane_airframe ("" if unknown)
    std::string country;    // the equipment creator's tag, e.g. GER ("" if unknown)
};

// One matching plane is enough: a plane matches if its equipment (type or archetype) is listed
// (or the list is empty) and its country is listed (or the list is empty).
bool SideMatches(const Side& side, const std::vector<Plane>& planes);
bool Matches(const Definition& d, const std::vector<Plane>& attackers, const std::vector<Plane>& defenders);

// Index of the candidate to play, by weight, for a random number r (any value); -1 if empty.
int PickByWeight(const std::vector<int>& weights, uint32_t r);

bool EqualsNoCase(const std::string& a, const std::string& b);

}  // namespace Air
