// Byte signatures for engine-tier features. ?? = wildcard (rel32 targets, RIP displacements).
//
// Every SIG(...) line is also read by tools/check_signatures.py, which checks that each
// pattern matches exactly once in the .text of the given hoi4.exe copies. Keep one SIG per
// line, with the pattern as a single string literal.
#pragma once

#define SIG(name, pattern) static const char name[] = pattern;

// PhysFS Windows backend, doOpen(): UTF-8 path -> CreateFileW. Function start.
SIG(kSigPhysfsDoOpen, "40 55 41 56 41 57 48 83 EC 40 48 8D 6C 24 40 48 89 5D 20 45 8B F0 48 89 75 28 44 8B FA 48 89 7D 30 48 8B D9 48 85 C9 0F 84")

// PhysFS __PHYSFS_platformOpenRead(): doOpen(path, GENERIC_READ, OPEN_EXISTING). The call to
// doOpen is the E8 at offset 15.
SIG(kSigPhysfsOpenRead, "48 83 EC 28 BA 00 00 00 80 41 B8 03 00 00 00 E8 ?? ?? ?? ?? 33 C9 48 83 F8 FF 48 0F 44 C1 48 83 C4 28 C3")
static const size_t kSigPhysfsOpenReadCallOffset = 15;

#undef SIG
