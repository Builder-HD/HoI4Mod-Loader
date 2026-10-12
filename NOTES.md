# NOTES: findings for hoi4_loader

Addresses below are for orientation in the named build only. Code uses the signatures in
`src/signatures.h`, never addresses.

## Executables analysed (copies only)

| File in game folder | Build string | SHA-256 | Size | PE link time (UTC) | File date |
|---|---|---|---|---|---|
| `hoi4.exe` | `v1.19.3.0.c01a` | `2d939e60d31aa15aeb70a9f6d4abd88b568b2f657990f0eeb9020e04f55221f8` | 55,778,424 | 2026-09-09 09:15 | 2026-10-10 |
| `hoi4.exe.backup` | `v1.19.2.0.a729` | `870ebf1f615262facca48a3b947775c41f0a5338f3068c40958209f5c41c65ad` | 55,680,632 | 2026-06-29 14:22 | 2026-07-10 |

**`hoi4.exe.backup` is NOT vanilla 1.19.3.** Evidence:
- Its embedded build string is `v1.19.2.0.a729`. The current exe's `v1.19.3.0.c01a` matches
  `hoi4_rev.txt` (`c01a3d507f10…`).
- It is a different compile: different PE timestamp, entry point, image size, PDB GUID
  (`3bdd6708…` vs `fc828413…`), and every section differs.
- The version resource is useless for this: it says `1.0` in both files.
- Most likely `universal-checksum-patcher.exe` saved this backup when it patched 1.19.2 in July.
  Steam's 1.19.3 update then replaced `hoi4.exe` and left the stale backup, and the patcher was
  re-run on 2026-10-10.
- It is probably an unpatched 1.19.2, but that can't be confirmed locally, and 1.19.2 isn't the
  build in use. So it was not added to `loader_config.ini`.

There is therefore **no vanilla 1.19.3 hoi4.exe on this machine**, and the "patched vs vanilla"
signature comparison could only be done against 1.19.2 (below). Ways to get a vanilla 1.19.3
without touching the game folder are listed under "Obtaining a vanilla 1.19.3 exe".

Tools: Ghidra 12.1.4 headless (full auto-analysis of the 1.19.3 copy, then read-only xref and
decompile scripts), a Python PE parser, and `tools/check_signatures.py`.

## How the game reads script files (1.19.3)

1. **Imports.** `KERNEL32`: `CreateFileW`, `CreateFileA`, `ReadFile`, `GetFileSize(Ex)`,
   `SetFilePointer(Ex)`, `GetFileAttributesExW`, `FindFirstFileExW`. There are no
   `CreateFileMapping`/`MapViewOfFile` imports and no CRT DLL imports (the CRT is statically
   linked). `VERSION.dll` is imported statically (`GetFileVersionInfoA`,
   `GetFileVersionInfoSizeA`, `VerQueryValueA`) and is not a KnownDLL, so a `version.dll` in the
   game folder is loaded instead of the System32 copy.
2. **PhysFS is statically linked** (strings `PHYSFS_init fail: `, `physfs\physfs.cpp`; symbol
   `PHYSFS_utf8ToUtf16` recovered by Ghidra). Its Windows backend is the only engine code that
   opens script files:

   | PhysFS function | Address (1.19.3) | What it does |
   |---|---|---|
   | `doOpen` | `0x142513a20` | UTF-8 path → `PHYSFS_utf8ToUtf16` → `CreateFileW(path, mode, FILE_SHARE_READ\|FILE_SHARE_WRITE, NULL, creation, FILE_ATTRIBUTE_NORMAL, NULL)` |
   | `__PHYSFS_platformOpenRead` | `0x142514720` | `doOpen(path, GENERIC_READ, OPEN_EXISTING)` |
   | `__PHYSFS_platformOpenWrite` | `0x142514750` | `doOpen(path, GENERIC_WRITE, CREATE_ALWAYS)` |
   | `__PHYSFS_platformOpenAppend` | `0x142514690` | `doOpen(path, GENERIC_WRITE, OPEN_ALWAYS)` + `SetFilePointer(END)` |
   | `__PHYSFS_platformFileLength` | `0x1425144b0` | `GetFileSize(handle)`, i.e. size comes from the **open handle** |
   | `__PHYSFS_platformRead` | `0x142514780` | `ReadFile` loop, stops on a short read |
   | `__PHYSFS_platformStat` | `0x1425148b0` | `GetFileAttributesExW` (+ `FindFirstFileW` for reparse points) |

3. **Script loaders.** The strings `common/scripted_triggers` and `common/scripted_effects` are
   referenced from two functions (`0x14018bb20` and `0x140147ee0` in 1.19.3), which hand the
   relative folder to the PhysFS virtual filesystem. Mod folders, DLC folders, the game folder
   and `tweakergui_assets` are all PhysFS search roots.
4. **Runtime confirmation** (2026-10-12): vanilla files such as
   `common\scripted_triggers\00_scripted_triggers.txt` and the mod stubs are all opened through
   `KernelBase!CreateFileW` with full native backslash paths.

**Conclusion:** script files go through a hookable OS API, so the file tier needs no engine
internals.

**Verified in game (v1, 2026-10-12 21:06):** the test event showed "Loader PRESENT, version 1";
`logs/error.log` had no `hoi4_loader` entries; replacements were served only for the mod's copies;
loader startup cost about 180 ms.

## Loader architecture (v2)

Startup, all inside `DllMain` of `version.dll` and before the game opens any script:

1. Forward the 17 version.dll exports to System32. Outside `hoi4.exe`, stop here.
2. Hash `hoi4.exe` (with the cache, see below) and look the hash up in `loader_config.ini`.
   A match allows the **engine tier**.
3. `MH_Initialize`, then `FeaturesInit(engineAllowed)`:
   - each feature in `g_features` (`src/features.cpp`) is either disabled (engine tier on an
     unknown build) or initialised. Init runs under SEH, so a crash leaves just that feature off.
   - Core detection registers its two virtual files.
4. `VfsStart()` generates every virtual file **once**, writes it to `%TEMP%\hoi4_loader\NN_<path>`,
   then enables the CreateFileW/A hooks in one `MH_ApplyQueued` call. Applying them one at a
   time cost about 75 ms extra, because each apply suspends all threads.

### Virtual file API (file tier)

`register_virtual_file(path_suffix, content_generator)` in `src/loader.h`:
- `path_suffix` is relative (`common/scripted_effects/foo.txt`). Either slash and any case is
  accepted. Empty components, `..`, `:` and duplicate registrations are rejected.
- The generator is a `std::string (*)()` called exactly once in `VfsStart()`. If it throws or
  its file can't be written, only that virtual file is dropped.
- An open is redirected only if **all** of these hold:
  - the path ends with the suffix on a path-component boundary;
  - it is `OPEN_EXISTING` without write access;
  - **the original file exists**.
- The existence check is required: PhysFS's `openRead` probes every search root with
  `CreateFileW`. Without the check, a phantom copy appeared in `tweakergui_assets` and the game
  loaded the effect twice (seen in the first v1 test).
- Content must be valid on every read: the game opens each stub 2 to 4 times per startup, some
  of them for the tweakergui database.
- Stub vs generated size doesn't matter: PhysFS takes the length from the open handle and
  tolerates short reads.
- Hooks: `KernelBase!CreateFileW` and `KernelBase!CreateFileA` (kernel32 as fallback).
  `CreateFileA` does **not** route through `CreateFileW` on current Windows (verified by test).

### Feature flags

The served `hoi4_loader_init`:
- sets `hoi4_loader_present` and `global.hoi4_loader_version`;
- adds `set_global_flag = hoi4_loader_plugin_<name>` for each **active** feature;
- adds `clr_global_flag = hoi4_loader_plugin_<name>` for every other known feature.

Clearing matters because global flags are saved with the game. Without it, a save made on a
supported build and loaded on an unsupported one (engine tier off) would still carry the flag.
For the same reason the test mod's stub effect clears `hoi4_loader_present`, the version
variable and the plugin flags, for when the save is loaded with no loader at all. The trigger
`hoi4_loader_present` is never saved, so it is always current.

### Hash cache

`%LOCALAPPDATA%\hoi4_loader\hash_cache.ini` (UTF-16, so any path works) has one section per
exe path, with `size`, `mtime` (FILETIME as an integer) and `sha256`. The hash is reused only if
path, size and last-write time all match and the stored value is 64 hex digits. `loader.log`
states "from hash cache" or "computed ... cache not used: <reason>". Rehash cost: about 140 ms.
Caveat: a tool that rewrites hoi4.exe but preserves both size and timestamp would fool the cache.
Delete the file to force a rehash.

## Byte signatures

`??` = wildcard (rel32 call targets, RIP-relative displacements). Signatures used by the loader
live in `src/signatures.h`. Run `python tools/check_signatures.py <copies...>` to verify them; it
applies the loader's rule (exactly one match in `.text`).

**Used by the loader (engine tier, `engine_probe`):**

| Signature | 1.19.3 checksum-patched | 1.19.2 (`hoi4.exe.backup`) |
|---|---|---|
| `kSigPhysfsDoOpen` | 1 match, `0x142513a20` | 1 match, `0x1424fea50` |
| `kSigPhysfsOpenRead` (its `E8` at +15 calls doOpen) | 1 match, `0x142514720` | 1 match, `0x1424ff750` |

Both match uniquely in both builds, even though the code moved by about 0x15000 between them.
That is good evidence the patterns survive a patch release. They have **not** been checked
against a vanilla 1.19.3, which isn't available locally. The checksum patcher most likely
changes only checksum-validation code, not PhysFS, but this is unverified.

**Reference only (not used by the loader), checked on 1.19.3:**

| Target | Signature | Matches |
|---|---|---|
| `CreateFileW` call inside `doOpen` | `48 C7 44 24 30 00 00 00 00 45 33 C9 C7 44 24 28 80 00 00 00 41 B8 03 00 00 00 41 8B D7 44 89 74 24 20 48 8B CE FF 15 ?? ?? ?? ??` | 1 (also 1 in 1.19.2) |
| `__PHYSFS_platformFileLength` | `40 53 48 83 EC 20 48 8D 54 24 30 C7 44 24 30 00 00 00 00 FF 15 ?? ?? ?? ?? 8B D8 83 F8 FF 75 ?? FF 15` | 1 (also 1 in 1.19.2) |
| `__PHYSFS_platformRead` | `48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 30 45 33 FF 49 8B D8 48 8B EA 4C 8B F1 41 8B F7 4D 85 C0 74 ?? 41 BC FF FF FF FF` | 2 (read, plus the same-layout `0x142514ba0`, probably `platformWrite`) |

To find the script loaders, search `.rdata` for `common/scripted_triggers\0` /
`common/scripted_effects\0` and follow the `LEA` xrefs.

The runtime scanner (`src/sigscan.cpp`) anchors on a fixed byte inside the pattern that is
uncommon in x64 code, rather than the first byte (usually `0x48`/`0x40`), so `memchr` stops
less often. The scan time is logged per signature.

### Obtaining a vanilla 1.19.3 exe (needed to finish the dual-build check)

Either way, keep a *copy* outside the game folder and run `tools/check_signatures.py` on both:
- **SteamCMD** (no change to the game folder): `download_depot 394360 <depot id> <manifest id>`
  into a separate directory (394360 is HOI4's app id). Needs a Steam login, plus the Windows
  depot id and 1.19.3 manifest id, e.g. from SteamDB (not verified here). Ask before downloading.
- **Steam "Verify integrity of game files"**: restores vanilla `hoi4.exe` in place. Copy it out
  before re-running the checksum patcher. This changes the game folder, so it's your call (steps
  in the chat summary).

Then add it to `loader_config.ini` as `1.19.3_vanilla = <sha256>`.

## Script findings (in-game tests)

- `set_global_variable` does **not** exist in HOI4 ("Unknown effect-type"). Use
  `set_variable = { global.x = 1 }` (the vanilla pattern).
- `on_startup` effects run with **no scope**, and scripted effects need one ("Invalid Scope ...
  provided: None"). Call `hoi4_loader_init` inside e.g. `every_country = { limit = { is_ai = no } ... }`.
- **`on_startup` does NOT run when a save is loaded**, only on a new game, matching the HOI4
  wiki. Test on 2026-10-12: the save `loader_test_A` was loaded straight from the main menu; the
  test mod's `on_startup` `log` lines never appeared in `logs/game.log`, and no event fired.
  (An earlier test seemed to show otherwise, but that session had started a new game before
  loading, so it was unreliable.)
- Consequence: global flags and variables in a save keep the values from the session that saved
  them. A save made with the engine tier active still carries `hoi4_loader_plugin_*` after it is
  loaded on a build where that feature is disabled. Mitigations:
  - **File tier (now):** also call `hoi4_loader_init` from `on_weekly` (country scope, limited to
    human players). Flags are then correct within at most 7 in-game days of loading a save.
  - **Engine tier (Part B):** an after-load hook that refreshes them immediately on supported
    builds.
  - The trigger `hoi4_loader_present` is never stale: it is read from script, not saved.
- **Verified in game (2026-10-12, 22:09 session, engine tier disabled):**
  - Loaded `loader_test_A` (saved with the engine tier active): the console event right away
    showed "Engine tier: active" (stale).
  - After 7 in-game days, the weekly refresh made it show "disabled".
  - A new game afterwards: "disabled", and `game.log` contains `hoi4_loader_test: on_startup
    fired` only for the new game, not for the load.

  (The `Something went wrong with the Conflict Risk stuff` line in `game.log` comes from vanilla
  `NORDIC_scripted_effects.txt`, not the loader.)

## What will likely break on game updates

| Change | Effect | Fix |
|---|---|---|
| Any new hoi4.exe (patch, hotfix, checksum patcher, Steam verify) | Hash unknown: file tier keeps working, engine tier off, new hash in the log | Run `tools/check_signatures.py` on a copy, test, add the hash |
| A signature stops matching exactly once | That feature logs "matched N time(s)" and stays inactive; its flag is cleared | Re-derive the pattern (Ghidra), update `src/signatures.h` |
| Engine stops using PhysFS / switches to memory-mapped or async (`ReadFileEx`, IOCP, `NtCreateFile` directly) file I/O | Stubs no longer replaced, event shows "NOT present" | Re-check imports; hook the new API (or `NtCreateFile`) |
| Engine adds a binary script cache or reads mods from an archive (`.zip`) | Stub content never opened from disk | Would need a hook inside PhysFS (signatures above) |
| Script effect/trigger syntax changes | `error.log` entries for `hoi4_loader_detect.txt` | Update the generators in `src/features.cpp` |
| `tweakergui_assets` or another root gets a real copy of a virtual file | That copy is replaced too | Harmless (identical content) |
| A Windows update changes how CreateFileA/W chain internally | Covered: both entry points are hooked separately | n/a |

Not affected by game updates: the version.dll export list (17 exports, forwarded with
signature-agnostic `jmp` stubs), and the Documents path (read from the registry, so OneDrive
redirection works).

## Testing done outside the game

`tests/run_smoke.sh` runs two stand-in `hoi4.exe` builds that import version.dll, so the proxy
loads exactly as in the game. `smoke/sig` embeds the two PhysFS signature byte sequences in its
own `.text`; `smoke/nosig` does not.

Every scenario checks:
- the proxy's load location and version.dll forwarding;
- that the trigger is served via `CreateFileW`, `CreateFileA` (forward slashes) and CRT
  `_wfopen` (mixed case);
- the effect's flag lines;
- that unrelated files, write-opens and nonexistent probe paths are untouched;
- the expected `loader.log` lines.

| # | Scenario | Expected |
|---|---|---|
| 1 | listed hash + signatures | engine_probe active, flag **set**; cache miss "no entry" |
| 2 | same again | "from hash cache" |
| 3 | exe touched | rehash, "last-write time changed" |
| 4 | corrupt cache entry | rehash, "entry is corrupt" |
| 5 | unlisted hash | file tier only, flag **cleared**, disabled-features summary, `my_build = <hash>` |
| 6 | no ini | same as 5, plus "not found" warning |
| 7 | listed hash, no signatures | "matched 0 time(s)", feature "FAILED, left inactive", flag cleared |
| 8 | exe size changed | rehash, "size changed", engine tier off |

All pass as of 2026-10-12. SHA-256 was cross-checked against `sha256sum` (58 MB in 157 ms).
