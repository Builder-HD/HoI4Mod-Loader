# Developing hoi4_loader

Building, extending and releasing the loader. Players: [README.md](README.md). Mod authors:
[MODDING.md](MODDING.md). Reverse-engineering findings and what may break on game updates:
[NOTES.md](NOTES.md). Release history: [CHANGELOG.md](CHANGELOG.md).

## Build

Requirements: Visual Studio 2022 with **Desktop development with C++** (MSVC, MASM, CMake), plus
internet access on first configure (MinHook v1.3.3 is fetched from GitHub).

From a "x64 Native Tools Command Prompt for VS 2022":

```
cd <this repository>
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build\Release\version.dll` and `build\Release\loader_config.ini`. Add
`-DHOI4_LOADER_BUILD_TESTS=OFF` to skip the smoke-test executables.

Or in Visual Studio: **File > Open > Folder...**, pick this folder, choose `x64-Release`, then
**Build > Build All**.

## Install a development build

1. Quit HOI4. Copy `version.dll` and `loader_config.ini` into
   `C:\Program Files (x86)\Steam\steamapps\common\Hearts of Iron IV\`
   (or `cmake --install build --config Release --prefix "C:\Program Files (x86)\Steam\steamapps\common\Hearts of Iron IV"`).
2. Copy the test mod into `<Documents>\Paradox Interactive\Hearts of Iron IV\mod\` (with
   OneDrive often `C:\Users\<you>\OneDrive\Documents\...`):
   - `mod\hoi4_loader_test.mod` -> `...\mod\hoi4_loader_test.mod` (the launcher may rewrite its `path`)
   - `mod\hoi4_loader_test\`    -> `...\mod\hoi4_loader_test\`
3. Enable **HOI4 Loader Test** in the launcher playset.

## Layout

```
CMakeLists.txt                DLL + smoke-test executables (MinHook fetched at configure time)
loader_config.ini             supported hoi4.exe SHA-256 hashes (gates the engine tier)
src/main.cpp                  DllMain, version.dll forwarding, logging, startup sequence
src/hash.cpp                  SHA-256 of hoi4.exe, hash cache, supported-hash lookup
src/vfs.cpp                   virtual file API + CreateFileW/A hooks
src/sigscan.cpp               signature scanner over hoi4.exe .text
src/sigreport.cpp             read-only signature report (unlisted builds, signature_scan)
src/signatures.h              all engine-tier byte signatures
src/features.cpp              feature registry, tier gating, core detection files
src/plugin_engine_probe.cpp   engine_probe feature
src/plugin_region_names.cpp   region_names feature (flags, GetNameKey hook, native commands)
src/plugin_game_loaded.cpp    game_loaded feature (fires on_game_loaded after a save is loaded)
src/plugin_region_control.cpp, plugin_military_counts.cpp, plugin_army_condition.cpp,
src/plugin_production_data.cpp   data trigger features (numeric triggers)
src/plugin_region_scope.cpp   region_scope feature (strategic region scope blocks, iterators)
src/engine_data.cpp           engine access for the data triggers (read-only game state)
src/engine_scope.cpp          engine access for scope blocks (vanilla iterator templates, child scopes)
src/native_script.h, .cpp     native effect/trigger API: schemas, registration, parsing, docs
src/native_script_engine.cpp  native API engine binding (vtable clones, registries, Scope)
src/native_scope_engine.cpp   native API engine binding of scope blocks and iterators
src/native_binding.h          binding internals shared by the two binding files
src/engine.cpp, engine.h      engine access: all hoi4.exe functions/data the features use
src/lifecycle.cpp, .h         lexer-ready / game-entered callbacks, main-thread queue (lifecycle feature)
src/proxy.asm, version.def    version.dll export stubs
src/sha256.h                  self-contained SHA-256
tools/check_signatures.py     check src/signatures.h against hoi4.exe copies
tools/check_code_shapes.py    check the CODE shapes at their RTTI-found addresses on hoi4.exe copies
tools/make_stubs.py           write (or --check) stubs/ from the feature list
tools/make_release.py         build release/hoi4_loader-v<version>.zip and its SHA256SUMS
tests/                        out-of-game smoke test (stand-in hoi4.exe)
mod/                          companion test mod
stubs/                        stub pack for mod authors (MODDING.md "Stub pack")
README.md                     players: install, uninstall, game patches, reporting problems
MODDING.md                    mod authors: guards, stubs, every script command
DEVELOPING.md                 this file
CHANGELOG.md                  release history
NOTES.md                      reverse-engineering findings and test results
PLAYERS.md                    pointer to README.md (0.5.0-beta shipped it as the player guide)
.github/ISSUE_TEMPLATE/       issue forms: bug report, question or request
```

## Configuration

`loader_config.ini` (next to `version.dll`):

```
[supported_hashes]
1.19.3_vanilla = 7dc947be34970da1e1c787bcddbe7f62b610ff258aebf8f06aa8b348f3a031d5
1.19.3_checksum_patched = 2d939e60d31aa15aeb70a9f6d4abd88b568b2f657990f0eeb9020e04f55221f8
```

The label is free text and is printed in `loader.log` when it matches. Before adding a new build,
run `python tools/check_signatures.py <copy of hoi4.exe>`: every signature must report exactly one
match. Also run `python tools/check_code_shapes.py <copy of hoi4.exe>`: every code shape the
numeric triggers rely on must report ok.

`[features]` switches single features off, e.g. `region_names = 0` (MODDING.md "Features").

Optional diagnostics:

```
[diagnostics]
trigger_timing = 1   ; time numeric triggers, scope blocks and iterators; loader.log summary every 10 in-game days
production_lines = 1 ; log the player's production lines on game entry
signature_scan = 1   ; write the signature report (below) on a listed build too
```

### Unknown game builds

On a hoi4.exe whose hash is not listed, the engine tier stays off and the loader writes a
**signature report** to `loader.log`. It only reads hoi4.exe's code; nothing is hooked or
patched.
- **Signatures and anchors:** `Signature report: N/44 signatures and anchors match exactly
  once`, then one `FAILS` line per signature or anchor that does not, with its match count.
- **Code shapes:** the resolver runs with faults contained (an exception is logged, never
  raised), and `Signature report: N/17 code shapes match where the resolver checks them`
  follows.
  - `FAILS` means the code was found but looks different.
  - `NOT CHECKED` means the code it belongs to was not found, usually because a signature or
    RTTI class before it failed.
- **What to send:** the log ends with the list: `loader.log`, `logs\error.log` and the game
  version from the main menu. The `my_build = <sha256>` line above it identifies the exe.
- **All matching** (44/44 and 17/17) is necessary for listing a build, not sufficient. Offsets
  read out of the code can still differ, so a build is listed only after the in-game checks.
- `[diagnostics] signature_scan = 1` writes the same report on a listed build, for comparison.

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

## Writing a native effect or trigger (for plugin authors)

Declare commands in your feature's init (see `src/plugin_region_names.cpp`); the API in
`src/native_script.h` handles parsing, registration, errors and docs:

```cpp
static void ExecSet(const Args& a, const Scope& s) { DoSomething(a.i(0), a.key(1)); }
static bool EvalHas(const Args& a, const Scope& s) { return g_table[a.i(0)].load() != nullptr; }

bool MyFeatureInit() {
    register_native_effect({"set_my_thing", "Help text shown in loader_script_docs.md.", Form::Block,
                            {{"region", ParamType::Int, true}, {"name", ParamType::Key, true}},
                            ScopeMask::Any, &ExecSet, /*tooltip*/ nullptr},
                           "my_feature");
    register_native_trigger({"has_my_thing", "Help text.", Form::Block, {{"region", ParamType::Int, true}},
                             ScopeMask::Country, &EvalHas, /*tooltip*/ nullptr},
                            "my_feature");
    return true;
}
```

- **Forms:**
  - `Form::Value` (`name = <value>`) takes exactly one required parameter.
  - `Form::Block` (`name = { key = value ... }`) takes named parameters.
  - Types: `Int`, `Bool` (`yes`/`no`), `Key` (one word, up to 255 characters), `Country` (a tag
    or THIS/ROOT/FROM/PREV, resolved per use), `Choice` (one of `ParamSpec::choices`, e.g.
    `"any|own|enemy"`; `Args::i` is the index), `Token` (a key interned at parse time;
    `Args::i` is the engine token, for lookups without allocation) and `Text` (free text, quoted
    in script if it has spaces, up to 255 bytes, no braces or control characters; e.g. army
    names).
  - `ParamSpec::fromScope` (`StrategicRegion`, `State`, `Country`, `RootCountry`): the parameter
    may be left out and then comes from the scope. Such a parameter is not `required`. Missing
    in the scope: effects do nothing, triggers are false (see MODDING.md, "Parameters from scope").
    `Args::fromScope(i)` tells whether the value came from the scope, e.g. for a tooltip that
    leaves out what the scope's header already names.
  - Unknown keys, type errors and missing required parameters are reported by the engine (with
    file and line) and logged once per command; that use then does nothing.
- **Numeric triggers:** `register_native_numeric_trigger` with
  `bool value(const Args&, const Scope&, int64_t* out)`.
  - **Kind:** `Count` (a whole number) or `Ratio` (fixed point, 100000 = 1.0).
  - **Comparisons:** the engine's own numeric trigger classes do the operators and variables.
    `name > x` works when every parameter can come from scope; otherwise
    `name = { ... <compareKey> > x }`.
  - **Game variables:** with every parameter from scope and country or state among the scopes,
    the trigger is also a game variable.
  - **Missing value:** return false when the value does not exist; the trigger is then false.
  - **`subject`:** optional, the start of the tooltip line. A parameter whose scope default the
    scope lacks arrives absent (`Args::has`): leave it out. If the subject reads the scope itself
    and finds nothing, call `out.markScopeMissing()`. Either way the line shows no current value.
- **Scope blocks and iterators:** `register_native_scope_block` (a block whose other keys run in
  the scope a `target` callback picks, e.g. `strategic_region`) and `register_native_iterator`
  (any / every / random over every object of a scope type, cloned from the game's state
  iterators). Only strategic region scopes are supported; see `src/plugin_region_scope.cpp`.
- **Effect blocks:** `register_native_effect_block` declares an effect whose children run in the
  current scope at the moment its execute callback calls `children.run()`, so a feature can act
  before and after them (`create_army` compares the country's divisions around the
  `create_unit` children; see `src/plugin_create_army.cpp`).
  - Only the effects listed in `children` may appear inside. Each must exist in the game, or the
    block is refused at engine registration. Any other key is the engine's "Unexpected token" and
    makes that use do nothing.
  - `run()` returns false if the children crashed (contained, logged once).
  - Tooltip: the `tooltip` callback's line, then the children's lines one level deeper.
- **When:** register from the feature's init only. Engine registration happens at lexer-ready,
  before any script is parsed; later calls are refused. The feature needs the engine tier and
  the `lifecycle` feature.
- **Name conflicts:** a name the game already has, or one the loader already registered, is
  refused, so vanilla commands are never replaced. One refused command makes
  `hoi4_loader_feature_<feature>` say `no`.
- **Callbacks:**
  - every callback runs under SEH; a crash is logged once, after which effects do nothing and
    triggers return false;
  - `Scope` offers read-only `stateId()`, `countryTag()`, `strategicRegionId()`, `types()` and
    `root()`/`from()`/`prev()`;
  - scopes are checked by the engine against the declared `ScopeMask`.
- **Tooltips:**
  - effects without a tooltip callback show no line;
  - triggers without one show `name: param=value ...`;
  - tooltip text is plain UTF-8 (colour codes allowed, no `{`);
  - **translatable text:** declare `LocText{key, english}` (placeholders as in the game:
    `$NAME$`, `$NAME|Y$`) and build the line with `native_localize(text, {{"NAME", value}})`.
    It uses the mod's localisation of `key` when one is loaded (the game fills the placeholders
    and formats them), else the English text. List the texts in `NativeEffect::texts` so
    `loader_script_docs.md` names the keys. `native_localize_key(key)` localises a plain key
    (for example a name key written in script).
- **Trigger performance:** `evaluate` (and a numeric `value`) runs for AI and UI, often,
  possibly on several threads at once.
  - It **may read game state**: all game-state writes are serial on the main thread, and
    vanilla's own triggers read the same data from AI worker threads (NOTES.md, "Threading
    model").
  - **No writes, no caching across calls, no locks, no allocation, no logging.** One
    flag-guarded line is fine.
  - Call only engine getters that just read: no function that needs the main thread (synced
    RNG, tick code), allocates or logs on its normal path. NOTES.md Part E lists the safe ones.
  - Validate ids before calling a getter, because several vanilla getters log on bad input.
  - Keep loops bounded by the data.
- **Determinism (multiplayer):** `execute` and `evaluate` may only read and write game state: no
  randomness, no wall-clock time, no file I/O, nothing thread-dependent. Every client runs the
  same script and must get the same result, or multiplayer desyncs. Display-only state, such as
  `region_names`' name table, must be derived from game state (its flags).

## Threads and hooks (for plugin authors)

The details are in NOTES.md, "Threading model".

**How the game uses threads:**
- **Main thread:** HOI4 changes game state only here, in serial code. That covers the tick
  (hourly, daily, weekly, monthly), on_actions, events, decisions and every flag change.
- **TBB worker threads:** the tick runs some sections in parallel on them, for example states,
  regions, diplomacy and parts of the country update. The main thread waits until they finish.
- **Other threads:** loading and audio.
- **No locks:** game state has none. Flags, for example, are a plain array that is reallocated
  when it grows.

**Rules:**
- **Assume a hook can run on any thread.** A hook into an engine function may run on a worker
  thread, a loading thread, or on the main thread in the middle of a tick: during long ticks the
  game draws frames between countries.
- **Don't read game state in a hook:** no flags, no countries, no lists you would iterate.
  Record what you saw (an atomic flag or counter) and return.
- **Do the work on the main thread.** Defer it with `lifecycle.h`:
  - `OnMainThreadFrame(fn)`: register at init. It runs at the start of every frame, at a safe
    point where no tick, parallel section or nested frame is running (main menu included). Keep
    it cheap: check your flag and return.
  - `RunOnMainThread(fn, ctx)`: runs `fn(ctx)` once at the next safe point. Callable from any
    thread.
  - `IsMainThread()`: true only on the main thread, and only once the first frame has run.
  - If `MainThreadQueueAvailable()` is false (the frame hook's signature was not found), none of
    this runs. A feature that depends on it must then stay off.
- **Callbacks that run on the main thread:** game-entered callbacks run there, outside the tick,
  so they may read and write game state. So do native effects.
- **Native triggers can be evaluated on any thread,** but unlike hooks they may *read* game state:
  they run where vanilla triggers run, never while game state is being written. They must not
  write, cache or allocate ("Trigger performance" above).
- **Example:** `region_names` does exactly this. Its flag hooks only set dirty bits; the rescan
  runs from `OnMainThreadFrame`.

## Smoke test (no game needed)

```
cmake --build build --config Release
sh tests/run_smoke.sh build        # from Git Bash
```

It first runs `native_script_test`: the native script API core with a fake engine. Its last line
gives the number of checks. It covers:
- schema validation;
- duplicate, vanilla-name and late registrations;
- feature completeness;
- int, bool and key parsing and their errors;
- missing parameters;
- crash containment;
- generic tooltips;
- the exact docs file content;
- scope defaults, country, choice and token parameters, and numeric triggers;
- scope blocks and iterators;
- effect blocks (allowed children, before/after execution, crashes);
- free text parameters;
- translatable tooltip texts.

Next come repository checks:
- the test mod's stub clears every feature flag;
- MODDING.md's feature and stub tables list every feature, and the stub pack matches
  `src/features.cpp`;
- README.md has the disclaimer, and PLAYERS.md only points to README.md;
- no executables, debug files or analysis projects are tracked in git.

Then it runs two stand-in `hoi4.exe` builds through nine scenarios:
- engine tier on/off;
- the signature report (unknown hash, and `signature_scan = 1`);
- missing ini and missing signatures;
- cache hit/miss/mtime/size/corrupt.

Each one checks the served content, including the lazily generated guard triggers, and the log
lines. It overwrites `loader.log` unless the game is running.

## Release process

1. **Version and changelog.** Move the "Unreleased" entries in `CHANGELOG.md` under a new
   `## <version> (YYYY-MM-DD), script version N` heading, and set `LOADER_RELEASE` in
   `src/loader.h` to the same version. Raise `LOADER_VERSION` only for the changes MODDING.md
   "Versions and compatibility" lists.
2. **Build and test.** Build Release and run `sh tests/run_smoke.sh build`.
3. **Package.** Run `python -I tools/make_release.py`. It refuses a DLL that is older than `src/`
   or lacks the release string, a stale stub pack, and a release without a CHANGELOG entry. It
   writes to `release/`:
   - `hoi4_loader-v<version>.zip`, with one top folder of the same name: `game/` (`version.dll`,
     `loader_config.ini`), `README.md`, `MODDING.md`, `DEVELOPING.md`, `CHANGELOG.md`,
     `LICENSE.txt`, `mod/`, `stubs/` and `SHA256SUMS.txt`;
   - `SHA256SUMS.txt` (the zip's contents) and `<zip>.sha256` (the zip itself);
   - `RELEASE_NOTES.md`: the text for the GitHub release, made from the CHANGELOG entry, the
     builds listed in `loader_config.ini` and the zip's hash.

   Text files are stored with CRLF line ends and the date of the CHANGELOG entry, so the same
   tree gives the same zip.
4. **Install test.** Install the zip as README.md describes and run the in-game checks.
5. **Publish.** Tag the commit (`v0.5-beta` for 0.5.0-beta), push the branch and the tag, then
   create a GitHub release from the tag:
   - title `hoi4_loader <version>`, body `release/RELEASE_NOTES.md`;
   - attach the zip and `release/SHA256SUMS.txt`;
   - mark betas as pre-release.
