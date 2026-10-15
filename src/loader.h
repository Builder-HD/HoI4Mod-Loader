// Shared declarations for hoi4_loader. See README.md / NOTES.md for the big picture.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <functional>
#include <string>

// Exposed to script as global variable hoi4_loader_version.
// 1: presence detection. 2: per-feature flags, file/engine tiers.
#define LOADER_VERSION 2

// ---------------------------------------------------------------------------
// main.cpp
// ---------------------------------------------------------------------------
void Log(const char* fmt, ...);
// <Documents>\Paradox Interactive\Hearts of Iron IV\<fileName> (where loader.log lives).
std::wstring LoaderOutputPath(const wchar_t* fileName);

// ---------------------------------------------------------------------------
// hash.cpp
// ---------------------------------------------------------------------------
// SHA-256 of `exePath` as lowercase hex, using %LOCALAPPDATA%\hoi4_loader\hash_cache.ini
// (keyed on path, size and last-write time) to skip rehashing an unchanged file.
bool GetExeHash(const wchar_t* exePath, char hexOut[65]);
// True if `hash` is one of the values under [supported_hashes] in the ini. `label` receives
// the matching key (e.g. "1.19.3_checksum_patched"), `entries` the number of hashes checked.
bool IsHashSupported(const wchar_t* iniPath, const char* hash, wchar_t* label, size_t labelCch, int* entries);

// ---------------------------------------------------------------------------
// vfs.cpp: virtual file API (file tier)
// ---------------------------------------------------------------------------
// Produces the replacement content. Called exactly once: in VfsStart(), or on the first served
// read for lazy files.
using ContentGenerator = std::function<std::string()>;

// Serve generated content instead of any existing file whose path ends with `pathSuffix`
// (e.g. "common/scripted_triggers/foo.txt"; either slash, case-insensitive, matched on a path
// component boundary). Only read-only OPEN_EXISTING opens of files that really exist are
// redirected. Must be called before VfsStart(). `lazy`: generate the content on the first read
// that is served (so it can reflect state that only exists later, e.g. native registration)
// instead of at startup.
bool register_virtual_file(const wchar_t* pathSuffix, ContentGenerator generator, bool lazy = false);

// Generates every registered file once, writes them to %TEMP%\hoi4_loader and queues the
// CreateFileW/A hooks (applied by main.cpp). Requires MH_Initialize() to have succeeded.
bool VfsStart();

// ---------------------------------------------------------------------------
// main.cpp: hooks
// ---------------------------------------------------------------------------
// Creates a MinHook hook and queues it for enabling; main.cpp applies all queued hooks at once
// after the features and the VFS are set up. Logs failures.
bool HookQueue(void* target, void* detour, void** original, const char* name);

// ---------------------------------------------------------------------------
// sigscan.cpp
// ---------------------------------------------------------------------------
// Finds `pattern` ("48 8B ?? ..." hex bytes, ?? = wildcard) in hoi4.exe's .text section.
// Returns the address only if there is exactly one match; logs and returns nullptr otherwise.
uint8_t* SigFindUnique(const char* name, const char* pattern);
// True if `pattern` matches at `p` (inside .text). For code reached through RTTI vtables: a
// known address whose bytes must still look as analysed.
bool SigMatchAt(const uint8_t* p, const char* pattern);
// Follows a rel32 call/jmp: `insn` points at the E8/E9 opcode.
uint8_t* SigFollowRel32(uint8_t* insn);
// Target of a RIP-relative operand: rel32 at insn+relOffset, instruction length insnLen.
uint8_t* SigRipTarget(uint8_t* insn, size_t relOffset, size_t insnLen);
// The single `lea r64,[rip+x]` in .text that references the unique NUL-terminated string
// `text` in .rdata. Logs and returns nullptr unless both are unique.
uint8_t* AnchorFindLea(const char* name, const char* text);
// A section of hoi4.exe's in-memory image (".text", ".rdata", ".data").
bool ImageSection(const char* name, uint8_t** begin, size_t* size);
// "hoi4.exe+0x1234" style description of an address, for logs.
std::string SigDescribe(const void* p);
// "<module>+0x1234" for any address (crash reports).
std::string DescribeAddress(const void* p);

// ---------------------------------------------------------------------------
// features.cpp
// ---------------------------------------------------------------------------
// File tier: only uses the virtual file API; runs on any hoi4.exe.
// Engine tier: hooks or patches code inside hoi4.exe; runs only on a supported hash.
enum class Tier { File, Engine };

// Decides which features run (engine tier only if `engineAllowed`; any feature can be turned
// off with `<name> = 0` under [features] in the ini), initialises them and registers the core
// detection files. Must run before VfsStart().
void FeaturesInit(bool engineAllowed, const wchar_t* iniPath);
// Comma-separated feature names in the given tier, for logs.
std::string FeatureNames(Tier tier);

bool FeatureActive(const char* name);
// Runtime failure after init (e.g. a self-check once the game exists): marks the feature
// inactive and clears its script flag.
void FeatureDeactivate(const char* name, const char* reason);

// Plugin entry points (one file each). Return false to stay inactive; must leave the game
// unmodified in that case.
bool EngineProbeInit();
bool LifecycleFeatureInit();  // features.cpp -> lifecycle.cpp
bool RegionNamesInit();       // plugin_region_names.cpp
bool GameLoadedInit();        // plugin_game_loaded.cpp
bool RegionControlInit();     // plugin_region_control.cpp
bool MilitaryCountsInit();    // plugin_military_counts.cpp
bool ArmyConditionInit();     // plugin_army_condition.cpp
bool ProductionDataInit();    // plugin_production_data.cpp
