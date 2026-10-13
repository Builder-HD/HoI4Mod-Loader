# hoi4_loader

A `version.dll` proxy that adds loader features to Hearts of Iron IV (Windows x64) at runtime.
Nothing on disk in the game is modified.

Game script can detect it:

- scripted trigger `hoi4_loader_present` evaluates to `yes`
- scripted effect `hoi4_loader_init` sets global flag `hoi4_loader_present`, global variable
  `hoi4_loader_version` (currently **2**), and one flag `hoi4_loader_plugin_<name>` per active
  feature. It clears the flag of every inactive feature.

Mods ship stub versions of the trigger and effect (see the test mod). The loader serves its own
content in their place, so mods work, and stay error-free, with or without the loader.

See [NOTES.md](NOTES.md) for the reverse-engineering findings and what may break on updates.

## Feature tiers

| Tier | What it may do | Runs on |
|---|---|---|
| **file** | Serve virtual files only (`register_virtual_file`) | any hoi4.exe |
| **engine** | Hook or patch code inside hoi4.exe (signature-located) | only hoi4.exe builds whose SHA-256 is in `loader_config.ini` |

Current features:

| Feature | Tier | Script flag | What it does |
|---|---|---|---|
| core detection | file | `hoi4_loader_present` | the trigger/effect above |
| `engine_probe` | engine | `hoi4_loader_plugin_engine_probe` | diagnostic only: resolves two PhysFS signatures and cross-checks them; patches nothing |
| `lifecycle` | engine | `hoi4_loader_plugin_lifecycle` | "lexer ready" and "game entered" (new game *and* loaded save) callbacks for other features; refreshes the plugin flags right after a save is loaded |
| `region_names` | engine (needs `lifecycle`) | `hoi4_loader_plugin_region_names` | runtime strategic region names, see below |

On an unknown build the file tier stays on, the engine tier is off, and `loader.log` lists the
disabled features and prints the exact line to add to `loader_config.ini`. A feature whose init
fails (for example a signature not found) is left inactive and its flag stays cleared. Any
feature can also be switched off by hand in `loader_config.ini`:

```
[features]
region_names = 0
```

### Checking for features in script

```
# Is the loader there at all? (Only true when the loader served the trigger this session.)
hoi4_loader_present = yes

# Is a given feature active? (Requires hoi4_loader_init to have run, e.g. from on_startup.)
has_global_flag = hoi4_loader_plugin_engine_probe
```

When to call `hoi4_loader_init` (the test mod's `common/on_actions` file shows both):
- from `on_startup`, inside a country scope such as `every_country = { limit = { is_ai = no } ... }`,
  because `on_startup` effects have no scope. This covers a **new game**.
- from `on_weekly` (country scope, `limit = { is_ai = no }`). `on_startup` does **not** fire
  when a save is loaded, so flags in a loaded save stay as they were saved. The weekly call
  corrects them within 7 in-game days.

Have your stub `hoi4_loader_init` clear the flags too, so a save loaded without the loader loses
them at the next weekly refresh. `hoi4_loader_present = yes` is always current. With the
`lifecycle` feature the loader refreshes the plugin flags itself right after a save is loaded; on
builds without it, a `hoi4_loader_plugin_*` flag can lag up to a week after loading a save.

## Strategic region names (`region_names`)

Rename a strategic region (id from `map/strategicregions/*.txt`) to a localisation key, and back:

```
set_strategic_region_name = { region = 42 name = MY_LOC_KEY }
reset_strategic_region_name = { region = 42 }
```

These are native effects, usable in any effect block (decisions, events, focuses, on_actions,
`meta_effect`). They exist only while `region_names` is active. Without the loader, or on an
unsupported build, the game logs `Unknown effect` for every use when parsing.

**Storage: a plain global flag per region**, `hoi4_srn_<region id>_<localisation key>`. The
native effects only manage these flags: set clears the region's other `hoi4_srn` flags, then
sets its own; reset clears them. Consequences:
- **Persistence:** names survive save and load, because flags are saved.
- **No loader or unsupported build:** the flags are inert and every region shows its original
  name. The engine's own name field is never written, so saves stay vanilla.
- **Loader back:** loading the same save on a supported build with the loader shows the names
  again.

**Mods that must also run without the loader** can use the flags directly, with no native
effect and therefore no parse errors:

```
clr_global_flag = hoi4_srn_42_MY_OLD_KEY   # clear the previous name yourself
set_global_flag = hoi4_srn_42_MY_LOC_KEY   # rename
```

If several `hoi4_srn` flags exist for one region, the most recently set one wins.

**Rules:**
- The key may contain `A-Z a-z 0-9 _ . -` and be up to 100 characters long, and it must exist
  in the localisation.
- Invalid region ids, malformed or missing keys are ignored with a line in `loader.log`; the
  region keeps its name.
- The effects add no tooltip line; use `custom_effect_tooltip` if a decision needs one.
  Avoid `{` in localisation text: HOI4 treats it as markup.

**Where the name shows:** everything that asks the engine for the region's name. That includes
map-mode and region tooltips, the region panel, air and naval mission screens, task forces,
naval invasion and blockade texts, radar, theaters, trigger tooltips and map labels.

**Multiplayer:** names are cosmetic; the flags are ordinary game state set by script on every
client. All players need the loader anyway, because it changes the files the game reads, which
affects the checksum.

## Layout

```
CMakeLists.txt                DLL + smoke-test executables (MinHook fetched at configure time)
loader_config.ini             supported hoi4.exe SHA-256 hashes (gates the engine tier)
src/main.cpp                  DllMain, version.dll forwarding, logging, startup sequence
src/hash.cpp                  SHA-256 of hoi4.exe, hash cache, supported-hash lookup
src/vfs.cpp                   virtual file API + CreateFileW/A hooks
src/sigscan.cpp               signature scanner over hoi4.exe .text
src/signatures.h              all engine-tier byte signatures
src/features.cpp              feature registry, tier gating, core detection files
src/plugin_engine_probe.cpp   engine_probe feature
src/plugin_region_names.cpp   region_names feature (flags, GetNameKey hook, native effects)
src/engine.cpp, engine.h      engine access: all hoi4.exe functions/data the features use
src/lifecycle.cpp, .h         lexer-ready / game-entered callbacks (lifecycle feature)
src/proxy.asm, version.def    version.dll export stubs
src/sha256.h                  self-contained SHA-256
tools/check_signatures.py     check src/signatures.h against hoi4.exe copies
tests/                        out-of-game smoke test (stand-in hoi4.exe)
mod/                          companion test mod
```

## Build

Requirements: Visual Studio 2022 with **Desktop development with C++** (MSVC, MASM, CMake), plus
internet access on first configure (MinHook v1.3.3 is fetched from GitHub).

From a "x64 Native Tools Command Prompt for VS 2022":

```
cd C:\Users\palpa\source\repos\hoi4_loader
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build\Release\version.dll` and `build\Release\loader_config.ini`. Add
`-DHOI4_LOADER_BUILD_TESTS=OFF` to skip the smoke-test executables.

Or in Visual Studio: **File > Open > Folder...**, pick this folder, choose `x64-Release`, then
**Build > Build All**.

## Install

1. Quit HOI4. Copy `version.dll` and `loader_config.ini` into
   `C:\Program Files (x86)\Steam\steamapps\common\Hearts of Iron IV\`
   (or `cmake --install build --config Release --prefix "C:\Program Files (x86)\Steam\steamapps\common\Hearts of Iron IV"`).
2. Copy the test mod into `<Documents>\Paradox Interactive\Hearts of Iron IV\mod\`
   (here `C:\Users\palpa\OneDrive\Dokumente\...`):
   - `mod\hoi4_loader_test.mod` -> `...\mod\hoi4_loader_test.mod` (the launcher may rewrite its `path`)
   - `mod\hoi4_loader_test\`    -> `...\mod\hoi4_loader_test\`
3. Enable **HOI4 Loader Test** in the launcher playset.

## Configuration

`loader_config.ini` (next to `version.dll`):

```
[supported_hashes]
1.19.3_vanilla = 7dc947be34970da1e1c787bcddbe7f62b610ff258aebf8f06aa8b348f3a031d5
1.19.3_checksum_patched = 2d939e60d31aa15aeb70a9f6d4abd88b568b2f657990f0eeb9020e04f55221f8
```

The label is free text and is printed in `loader.log` when it matches. Before adding a new build,
run `python tools/check_signatures.py <copy of hoi4.exe>`: every signature must report exactly one
match.

### Hash cache

Hashing hoi4.exe (55 MB) takes about 140 ms. The result is cached in
`%LOCALAPPDATA%\hoi4_loader\hash_cache.ini`, keyed on the exe path, size and last-write time, so
an unchanged exe is not rehashed. `loader.log` says whether the cache was used, or why not.
Delete the file to force a rehash. The cache is a speed-up, not a security boundary.

## Logs and files

- `<Documents>\Paradox Interactive\Hearts of Iron IV\loader.log`: rewritten on every launch. If
  another hoi4.exe holds it open, the loader writes `loader.<pid>.log` instead.
- `%TEMP%\hoi4_loader\`: generated virtual files (safe to delete).
- `%LOCALAPPDATA%\hoi4_loader\hash_cache.ini`: hash cache (safe to delete).

## Smoke test (no game needed)

```
cmake --build build --config Release
sh tests/run_smoke.sh build        # from Git Bash
```

It runs two stand-in `hoi4.exe` builds through eight scenarios: engine tier on/off, unknown hash,
missing ini, missing signatures, and cache hit/miss/mtime/size/corrupt. It checks the served
content and the log lines. It overwrites `loader.log` unless the game is running.

## Uninstall

Delete `version.dll` and `loader_config.ini` from the game folder. Optionally delete
`%TEMP%\hoi4_loader\` and `%LOCALAPPDATA%\hoi4_loader\`.
