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
| `game_loaded` | engine (needs `lifecycle`) | `hoi4_loader_plugin_game_loaded` | the `on_game_loaded` on_action, see below |

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

# Is a feature usable right now? (Never stale; the mod must ship the stub file, see below.)
hoi4_loader_feature_region_names = yes

# Was a feature active when hoi4_loader_init last ran? (Saved with the game; can be stale.)
has_global_flag = hoi4_loader_plugin_engine_probe
```

**Feature guard triggers** `hoi4_loader_feature_<feature>` exist for every feature. A mod that
uses one ships a stub, one file per feature:

```
# common/scripted_triggers/hoi4_loader_feature_region_names.txt
hoi4_loader_feature_region_names = { always = no }
```

The loader serves `always = yes` instead, but only if that feature is active **and** every native
command it declares was registered this session. Otherwise it serves `always = no`, with the
reason as a comment.
- **Generated late:** the content is generated on the first read of the file. Native commands
  are registered when the lexer becomes ready, before any script file is read (about 2.5 s into
  startup, against about 6 s for the first script file in the tests). If the file were ever read
  before registration, it gets `no` (fail closed) and `loader.log` says
  `read BEFORE native registration ran`.
- **Never stale:** guard triggers are read from script each session and never saved, so they
  can't go stale.
- **Unknown features:** a mod that ships a stub for a feature the installed loader doesn't know
  keeps its stub (`no`). Either way there's no undefined-trigger error.

When to call `hoi4_loader_init` (the test mod's `common/on_actions` file shows both):
- from `on_startup`, inside a country scope such as `every_country = { limit = { is_ai = no } ... }`,
  because `on_startup` effects have no scope. This covers a **new game**.
- from `on_weekly` (country scope, `limit = { is_ai = no }`). `on_startup` does **not** fire
  when a save is loaded, so flags in a loaded save stay as they were saved. The weekly call
  corrects them within 7 in-game days.

Have your stub `hoi4_loader_init` clear the flags too, so a save loaded without the loader loses
them at the next weekly refresh. `hoi4_loader_present = yes` is always current. With the
`lifecycle` feature the loader refreshes the plugin flags itself right after a save is loaded, in
single player. In multiplayer (and after a hot join) it doesn't, because a change made on one
machine only would desync the game; there the weekly refresh does it. On builds without
`lifecycle`, a `hoi4_loader_plugin_*` flag can lag up to a week after loading a save.

## Strategic region names (`region_names`)

Rename a strategic region (id from `map/strategicregions/*.txt`) to a localisation key, and back:

```
set_strategic_region_name = { region = 42 name = MY_LOC_KEY }
reset_strategic_region_name = { region = 42 }
```

These are native effects, usable in any effect block (decisions, events, focuses, on_actions,
`meta_effect`). They exist only while `region_names` is active. Without the loader, or on an
unsupported build, the game logs `Unknown effect` for every use when parsing. Only `region` is
accepted; the `id` alias of earlier versions is gone.

There is also a native trigger, true while the region shows a loader-provided name:

```
hoi4_loader_feature_region_names = yes             # guard: required, see "Native script commands"
strategic_region_has_custom_name = { region = 42 }
```

Its requirement line reads e.g. "Strategic region Loader Test Sea A (42) has a custom name"
(negated: "does NOT have a custom name").

**Timing:** a rename shows from the next frame. The loader applies flag changes on the main
thread at the start of a frame, not inside the effect (see "Threads and hooks"). So right after
`set_strategic_region_name`, in the same effect block, the trigger still answers for the old
name.

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
  - Both `-` and the full length (a 112-character `hoi4_srn` flag) are tested in game,
    including script parsing of the flag in `has_global_flag`.
  - Long display names are cut off in labels and tooltips the same way vanilla's long sea zone
    names are.
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

## `on_game_loaded` (`game_loaded`)

Vanilla `on_startup` fires for new games only. `on_game_loaded` is its counterpart for loaded
saves. Use it like any on_action; any number of files and mods can add to it:

```
on_actions = {
    on_game_loaded = {
        effect = { ... }
    }
}
```

**Semantics (permanent):**
- **When:** it fires each time a saved game is loaded: from the main menu, Continue, from the
  in-game menu, and ironman. It never fires for a new game; use `on_startup` for that.
- **Order:** it runs after the loader's own refresh (plugin flags, region names), so the
  effect sees current state.
- **Scope:** once per **human-controlled country**, in alphabetical tag order, with ROOT = that
  country. `country_event`, country flags and so on work directly, with no `every_country`
  workaround. If no country is human-controlled (observer), it fires once without scope, like
  `on_startup`.

**This version fires in single player only.** In multiplayer (host, client or hot join) it
doesn't fire, and `loader.log` says why. Firing from the loader happens on one machine. In
multiplayer every machine would have to do it at the same moment, and a hot-joining player
loads alone, so the game would desync. See NOTES.md Part D.

**Without the loader** the game accepts the `on_game_loaded` block silently (on_action names
are free-form) and simply never fires it. There is no error.

**Guard:** `hoi4_loader_feature_game_loaded = yes` (ship the stub, see "Checking for features
in script") means **supported, not fired**. It is `yes` in multiplayer too, where the on_action
never fires. Use it for UI ("this loader can run migrations on load"), never to decide whether
something already happened.

**Pattern for save migrations:** store a data version in the save, and make the migration
idempotent: the first run migrates, every later run does nothing. Run it from
`on_game_loaded` **and** from a fallback such as `on_weekly`:

```
# common/scripted_effects/my_mod_migrate.txt
my_mod_migrate = {
    if = {
        limit = { NOT = { check_variable = { global.my_mod_data_version > 1 } } }
        # ... convert version 1 data to version 2 ...
        set_variable = { global.my_mod_data_version = 2 }
    }
}

# common/on_actions/my_mod_on_actions.txt
on_actions = {
    on_startup = { effect = { set_variable = { global.my_mod_data_version = 2 } } }  # new games are current
    on_game_loaded = { effect = { my_mod_migrate = yes } }             # right after loading, with the loader
    on_weekly = { effect = { if = { limit = { is_ai = no } my_mod_migrate = yes } } }  # fallback
}
```

**Do not** skip the fallback when `hoi4_loader_feature_game_loaded = yes`. Without the loader,
in multiplayer, or with the feature switched off, the fallback is the only thing that runs.
The test mod's "Save migration" decisions are built exactly this way.

**Switch off:** `[features] game_loaded = 0` in `loader_config.ini`. Every
`on_game_loaded` block then stays silent and the fallbacks take over.

## Native script commands (for mod authors)

Features can add real script effects and triggers. Every registered command, with its
parameters, scopes and help, is listed in
`<Documents>\Paradox Interactive\Hearts of Iron IV\loader_script_docs.md`, written at each start.

**Without the loader** (or on an unsupported build, or with the feature off) the game doesn't
know these commands and logs `Unknown effect` / `Unknown trigger` while parsing. What that
means differs:
- **An unknown effect** is dropped: nothing happens. That's safe.
- **An unknown trigger** is dropped too. Its condition disappears, so the surrounding `visible`,
  `available`, `limit` or `trigger` block can become **true**.

So every native trigger needs the feature guard **in the same AND block**:

```
visible = {
    hoi4_loader_feature_region_names = yes
    strategic_region_has_custom_name = { region = 42 }
}
```

The plugin flag `has_global_flag = hoi4_loader_plugin_region_names` is **not** a safe guard:
- Global flags are saved with the game. A save made with the loader keeps the flag when it is
  loaded without the loader, or on a build where the engine tier is off (no lifecycle refresh),
  until the weekly refresh runs.
- In that window the native trigger has been dropped and the flag is still set, so the condition
  is true.
- `hoi4_loader_feature_region_names` is read from script each session, so it is `no` from the
  first frame. The test mod's "UNGUARDED" decision demonstrates the difference.

**NOT and OR:** keep the guard next to the native trigger, never let the native trigger stand
alone in a block:

```
# "Region 42 has its original name", false without the loader (fail closed):
hoi4_loader_feature_region_names = yes
NOT = { strategic_region_has_custom_name = { region = 42 } }

# Same, but true without the loader (no loader = no custom names):
OR = {
    NOT = { hoi4_loader_feature_region_names = yes }
    NOT = { strategic_region_has_custom_name = { region = 42 } }
}

# OR with other conditions: wrap the native trigger with its guard.
OR = {
    has_war = yes
    AND = {
        hoi4_loader_feature_region_names = yes
        strategic_region_has_custom_name = { region = 42 }
    }
}
```

If a native trigger fails at runtime (a plugin bug), it evaluates to false and `loader.log`
gets one line. Inside `NOT` that counts as true.

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
  - Types: `Int`, `Bool` (`yes`/`no`) and `Key` (one word, up to 255 characters).
  - Unknown keys, type errors and missing required parameters are reported by the engine (with
    file and line) and logged once per command; that use then does nothing.
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
  - tooltip text is plain UTF-8 (colour codes allowed, no `{`).
- **Trigger performance:** `evaluate` runs for AI and UI, often, possibly on several threads at
  once. **No locks, no allocation, no logging** in it; read lock-free data (atomics, immutable
  tables).
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
  so they may read and write game state. So do native effects. Native triggers can be evaluated
  on any thread, so they follow the rules above.
- **Example:** `region_names` does exactly this. Its flag hooks only set dirty bits; the rescan
  runs from `OnMainThreadFrame`.

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
src/plugin_region_names.cpp   region_names feature (flags, GetNameKey hook, native commands)
src/plugin_game_loaded.cpp    game_loaded feature (fires on_game_loaded after a save is loaded)
src/native_script.h, .cpp     native effect/trigger API: schemas, registration, parsing, docs
src/native_script_engine.cpp  native API engine binding (vtable clones, registries, Scope)
src/engine.cpp, engine.h      engine access: all hoi4.exe functions/data the features use
src/lifecycle.cpp, .h         lexer-ready / game-entered callbacks, main-thread queue (lifecycle feature)
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

It first runs `native_script_test` (64 checks on the native script API core with a fake engine).
That covers:
- schema validation;
- duplicate, vanilla-name and late registrations;
- feature completeness;
- int, bool and key parsing and their errors;
- missing parameters;
- crash containment;
- generic tooltips;
- the exact docs file content.

Then it runs two stand-in `hoi4.exe` builds through eight scenarios: engine tier on/off, unknown
hash, missing ini, missing signatures, and cache hit/miss/mtime/size/corrupt. Each one checks
the served content, including the lazily generated guard triggers, and the log lines. It
overwrites `loader.log` unless the game is running.

## Uninstall

Delete `version.dll` and `loader_config.ini` from the game folder. Optionally delete
`%TEMP%\hoi4_loader\` and `%LOCALAPPDATA%\hoi4_loader\`.
