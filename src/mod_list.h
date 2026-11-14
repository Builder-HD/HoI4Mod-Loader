// The launcher's enabled mods, shared by every feature that reads files from mods (air_scenarios,
// attention_area's shaders): dlc_load.json in the game's user folder lists the enabled mods'
// descriptors ("mod/<name>.mod"), in load order; each descriptor names the mod's folder.
#pragma once
#include <string>
#include <vector>

namespace ModList {

// dlc_load.json: the enabled mods' descriptor paths, in order. False if there is no enabled_mods list.
bool ParseDlcLoad(const std::string& json, std::vector<std::string>* modFiles);

// A mod descriptor (.mod): its folder (`path`, or the legacy `archive` for zipped mods), its name and
// its replace_path entries. False if it names neither a path nor an archive.
struct ModDescriptor {
    std::string name;
    std::string path;
    std::string archive;
    std::vector<std::string> replacePaths;
};
bool ParseDescriptor(const std::string& text, ModDescriptor* out);

// One enabled mod as read from disk.
struct EnabledMod {
    enum class Status { Ok, Unreadable, Archive };
    std::string file;  // as dlc_load.json lists it
    Status status = Status::Unreadable;
    ModDescriptor descriptor;
    std::wstring root;  // its folder (absolute, backslashes, no trailing one); Ok only
    std::string label() const { return descriptor.name.empty() ? file : descriptor.name; }
};
// The enabled mods of the user folder (`...\Hearts of Iron IV`, no trailing backslash), in load
// order. False (empty list) if dlc_load.json is missing or has no enabled_mods list. Paths in the
// list and the descriptors may be relative to the user folder.
bool Enabled(const std::wstring& userDir, std::vector<EnabledMod>* out);

// Small file helpers the mod readers share.
bool ReadWholeFile(const std::wstring& path, std::string* out);  // a UTF-8 BOM is dropped
std::wstring Wide(const std::string& utf8);
std::string Narrow(const std::wstring& w);

}  // namespace ModList
