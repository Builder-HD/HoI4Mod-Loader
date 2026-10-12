// Shared declarations for hoi4_loader. See README.md / NOTES.md for the big picture.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <string>

// Exposed to script as global variable hoi4_loader_version.
// 1: presence detection. 2: per-feature flags, file/engine tiers.
#define LOADER_VERSION 2

// ---------------------------------------------------------------------------
// main.cpp
// ---------------------------------------------------------------------------
void Log(const char* fmt, ...);

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
// Produces the replacement content. Called exactly once, inside VfsStart().
using ContentGenerator = std::string (*)();

// Serve generated content instead of any existing file whose path ends with `pathSuffix`
// (e.g. "common/scripted_triggers/foo.txt"; either slash, case-insensitive, matched on a path
// component boundary). Only read-only OPEN_EXISTING opens of files that really exist are
// redirected. Must be called before VfsStart().
bool register_virtual_file(const wchar_t* pathSuffix, ContentGenerator generator);

// Generates every registered file once, writes them to %TEMP%\hoi4_loader and hooks
// CreateFileW/A. Requires MH_Initialize() to have succeeded.
bool VfsStart();

// ---------------------------------------------------------------------------
// sigscan.cpp
// ---------------------------------------------------------------------------
// Finds `pattern` ("48 8B ?? ..." hex bytes, ?? = wildcard) in hoi4.exe's .text section.
// Returns the address only if there is exactly one match; logs and returns nullptr otherwise.
uint8_t* SigFindUnique(const char* name, const char* pattern);
// Follows a rel32 call/jmp: `insn` points at the E8/E9 opcode.
uint8_t* SigFollowRel32(uint8_t* insn);
// "hoi4.exe+0x1234" style description of an address, for logs.
std::string SigDescribe(const void* p);

// ---------------------------------------------------------------------------
// features.cpp
// ---------------------------------------------------------------------------
// File tier: only uses the virtual file API; runs on any hoi4.exe.
// Engine tier: hooks or patches code inside hoi4.exe; runs only on a supported hash.
enum class Tier { File, Engine };

// Decides which features run (engine tier only if `engineAllowed`), initialises them and
// registers the core detection files. Must run before VfsStart().
void FeaturesInit(bool engineAllowed);
// Comma-separated feature names in the given tier, for logs.
std::string FeatureNames(Tier tier);

// Plugin entry points (one file each). Return false to stay inactive; must leave the game
// unmodified in that case.
bool EngineProbeInit();
