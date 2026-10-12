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

**What universal-checksum-patcher changes (verified 2026-10-12):**
- On 2026-10-12 22:54 the game folder's `hoi4.exe` was replaced. The file is
  `v1.19.2.0.a729`, sha256 `0de7bdfa278347209fe67c134b9b321f2beab38dd107bc5397f1ab1f6e8d0998`,
  the same size as `hoi4.exe.backup`. Only `hoi4.exe` changed; `hoi4_rev.txt` still says 1.19.3.
- It differs from `hoi4.exe.backup` by **exactly one byte**, at file offset `0x16e0fa`
  (VA `0x14016ecfa`). The backup has `85 C0 0F 94 C3` (`test eax,eax; sete bl`, after a call
  comparing a computed checksum string with a stored one). The new file has `31 C0 0F 94 C3`
  (`xor eax,eax; sete bl`): the comparison always reports "equal".
- So that file is a **checksum-patched 1.19.2**, and `hoi4.exe.backup` is the unpatched 1.19.2.
- The 1.19.3 exe carries the same patch: pattern `E8 ?? ?? ?? ?? 31 C0 0F 94 C3` matches once,
  at `0x14016f205` (call) / `0x14016f20a` (patched byte). Unpatched builds have no match for
  that pattern; the `85 C0` form appears about 30 times as ordinary code.
- **Consequence:** a patched exe differs from its vanilla build only in that byte, so every
  loader signature behaves identically on both. The vanilla 1.19.3 hash would be the 1.19.3
  copy with that byte reverted, but that is unverified until a Steam-original 1.19.3 file is
  available.

| Build | State | SHA-256 |
|---|---|---|
| 1.19.2 | unpatched (`hoi4.exe.backup`) | `870ebf1f615262facca48a3b947775c41f0a5338f3068c40958209f5c41c65ad` |
| 1.19.2 | checksum-patched (game folder 22:54 to 23:01) | `0de7bdfa278347209fe67c134b9b321f2beab38dd107bc5397f1ab1f6e8d0998` |
| 1.19.3 | checksum-patched (game folder until 22:54) | `2d939e60d31aa15aeb70a9f6d4abd88b568b2f657990f0eeb9020e04f55221f8` |
| **1.19.3** | **vanilla, from Steam "Verify integrity" at 23:01** | `7dc947be34970da1e1c787bcddbe7f62b610ff258aebf8f06aa8b348f3a031d5` |

**Vanilla 1.19.3 verified (2026-10-12 23:01):**
- After Steam's "Verify integrity", `hoi4.exe` is `v1.19.3.0.c01a`, the same size as the
  patched 1.19.3.
- It differs from the patched 1.19.3 in **exactly one byte**: file offset `0x16e60a`
  (VA `0x14016f20a`), `0x85` vanilla vs `0x31` patched. That is the patch site identified above.
- The patch pattern `E8 ?? ?? ?? ?? 31 C0 0F 94 C3` has 0 matches in it.
- Added to `loader_config.ini` as `1.19.3_vanilla`; the patched hash stays as
  `1.19.3_checksum_patched`.

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

| Signature | 1.19.3 vanilla | 1.19.3 checksum-patched | 1.19.2 (`hoi4.exe.backup`) |
|---|---|---|---|
| `kSigPhysfsDoOpen` | 1 match, `0x142513a20` | 1 match, `0x142513a20` | 1 match, `0x1424fea50` |
| `kSigPhysfsOpenRead` (its `E8` at +15 calls doOpen) | 1 match, `0x142514720` | 1 match, `0x142514720` | 1 match, `0x1424ff750` |

Both match uniquely in all three builds, even though the code moved by about 0x15000 between
1.19.2 and 1.19.3. That is good evidence the patterns survive a patch release. Vanilla and
patched builds of one version differ in a single checksum-compare byte, so they always give
identical results.

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

### Adding a new build

1. Get the exe: Steam "Verify integrity of game files" restores the vanilla one. Do not run the
   checksum patcher before copying it.
2. Copy it outside the game folder.
3. Run `python tools/check_signatures.py <copy>`. Every signature must match exactly once.
4. Add `<version>_vanilla = <sha256>` to `loader_config.ini`. If you also use the checksum
   patcher, add its output as `<version>_checksum_patched`: it differs from vanilla in only the
   one patched byte.

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

---

# Part B: strategic region names

## B1 findings (1.19.3 checksum-patched copy; addresses are for orientation only)

Method: Ghidra headless on the 1.19.3 copy (string xrefs, RTTI vtables, decompilation),
plus byte-level checks on the 1.19.2 copy. RTTI class names (`.?AV...@@`) survive in the
binary, which made the classes easy to identify.

### 1. Where strategic regions live and where the name is

- **Loading.** `FUN_140a65010` loads `map/` + `strategicregions` (plus `map/colors.txt` and
  `map/weatherpositions.txt`) into the strategic-region database (singleton getter
  `FUN_140163c30`, instance pointer `DAT_14332f080`).
- **Two objects per region:**
  - `CStrategicRegionTemplate` (vtable `0x1429dbe50`): static data from the file. Its read
    function (vtable slot 4) maps save token `0xb` (`id`) to `+0xa0` and `0x1b` (`name`) to the
    strings at `+0x40` and `+0x20`. So `template+0x20` holds `"STRATEGICREGION_<id>"`, the
    localisation key. Province lists etc. follow.
  - `CStrategicRegion` (vtable `0x14296d558`, second interface at `+8`): live game state.
    `+0x30` points to its template, `+0x60` is the id. **`+0x38` is a string (0x20-byte MSVC
    `std::string`, length at `+0x48`) holding an optional name override.**
- **The override is built in and saved natively.** `CStrategicRegion` write (slot 2) emits
  token `0x1b` (`name`) with `+0x38` **only when it is non-empty**; read (slot 4) restores it.
  Vanilla script never writes it (no effect references it), so vanilla saves never contain it.
  It is the region equivalent of the state override that `set_state_name` writes at
  `CState+0x38`.

### 2. How the UI gets the name: one getter, looked up every time

`FUN_140e23410` is effectively `CStrategicRegion::GetNameKey()`:

```c
const std::string* GetNameKey(CStrategicRegion* r) {
    if (r->customName.size /* +0x48 */ == 0)
        return &r->template /* +0x30 */ ->nameKey /* +0x20 */;
    return &r->customName /* +0x38 */;
}
```

- It returns a **localisation key**; callers localise it each time (e.g. the region panel
  update `FUN_1417369d0` does `SetText(Localize(*GetNameKey(region)))`). No cached copies in
  game state.
- **99 call sites in 60+ functions**, including the air mission tooltip
  (`COMMAND_POWER_TOOLTIP_AIR_MISSION`, `LOCATION`), army air mission panel, air wing states,
  naval invasion, blockade, port strike and naval battle titles, task force activity, mines
  effects, radar tooltip, faction theater map mode, the region info panel, map-mode tooltips,
  trigger tooltips (`TRIGGER_STRAT_REGION_ID`), and the localisation object system (`EV_*`
  table, i.e. scripted loc `GetName`).
- An instruction scan for inlined copies (`cmp [r+0x48],0` with `lea [r+0x38]` and
  `mov [r+0x30]` nearby) found only one other region copy, in a debug message
  ("Province ... added twice to strategic area"). So **hooking or relying on this one function
  covers every UI location**.
- **Map labels are cached**: `FUN_140b59200(mapNames, labelType)` builds the map text labels
  (it calls `GetNameKey`) for label types 0/2/3/6 at interface creation (`FUN_140b554b0`,
  called from the in-game interface setup that loads `interface/main.gui`). Single-type
  rebuild entry points exist (`FUN_140b58430`, `FUN_140b58440`). `set_state_name`'s execute
  also marks map text dirty: `*(u16*)(DAT_14333cfb8 + 0x81) = 0x101` via `FUN_140e139c0`, and
  pushes a UI refresh command (0xd8-byte record) on the in-game interface handler
  (`DAT_14332f698`, vtable +0xb8). Refreshing region labels after a rename is therefore feasible.

### 3. How script effects are registered and executed

- **Names to type ids.** Every effect and trigger name is a compile-time token. For example,
  `FUN_1424c4b70(descriptor, 0x37a4, "set_state_name")` sits in the giant static initialiser
  `FUN_1400831c0`. Known ids: `set_state_name` 0x37a4, `set_province_name` 0x37a3,
  `set_global_flag` 0x2739.
- **Registry.** `_EffectEntries` is a `std::map<int token, CEffectEntryBase*>` at
  `DAT_143330000`. Each effect has a small static registration function, e.g. `FUN_140017260`:
  `entry = new CEffectEntry<CSetStateNameEffect>{ vtable, "set_state_name = <string> - ..." };
  FUN_1405415a0(registry, 0x37a4, entry)`. It asserts the token is not registered yet.
  `entry->vtable[1]` is the factory (`new` effect object).
- **`CEffect` interface** (25 virtual slots, vtable `0x1427d4990`). Slot 3 is the generic
  reader: it iterates a `{ ... }` block and calls slot 4 per key. Slot 4 handles one block
  item, slot 6 a single value (`effect = value`), slot 7 builds the tooltip, slot 13 executes.
- **`CSetStateNameEffect`** (vtable `0x1427501d8`, object 0x78 bytes):
  - slot 6 stores the value at `+0x58` (std::string);
  - slot 7 builds tooltip `EFFECT_SET_STATE_NAME` / `EFFECT_RESET_STATE_NAME`;
  - slot 13 gets the scope state from the global gamestate (`DAT_14332f260`; states at `+0x2c8`,
    count `+0x2d4`), copies the string into `state+0x38`, and triggers the map/UI refresh above.
- **`CRenameProvinceEffect`** (`set_province_name = { id = <province> name = <string> }`, any
  scope; vtable `0x142773728`; object 0x80 bytes; registered as token 0x37a3 by
  `FUN_14001dec0`). It overrides only 4 slots:
  - 0: destructor;
  - 4: block item; `id` (token 0xb) goes to an int at `+0x58`, `name` (token 0x1b) to a
    std::string at `+0x60`, anything else is a parse error;
  - 7: tooltip;
  - 13: execute; `province = gamestate->provinces.Get(id)`, then `province+0x40 = name`.

  **This is an almost exact template for `set_strategic_region_name = { region = .. name = .. }`.**
- Script scopes: `STRATEGIC_REGION` is a real scope (documented triggers/effects), but there is
  no region name effect or trigger in vanilla.
- **No scripted-effect parameters in HOI4.** Vanilla never uses `$PARAM$` in scripted
  effects, and neither the shipped docs nor the exe show argument support (the `$X$` syntax
  exists only in localisation). Scripted effects are invoked as `name = yes`.
  The closest tool is `meta_effect = { text = { ... [X] ... } X = "..." }`, which builds and
  runs script text at runtime. So **the requested syntax `set_strategic_region_name = {
  region = 42 name = KEY }` is only possible as a native (engine-registered) effect.**

### 4. Global flags, and the after-load / after-new-game hook point

- **Storage.** `CSetGlobalFlagEffect::Execute` (vtable slot 13, `FUN_140362930`) resolves the
  flag name to a token (`FUN_141393330`; dynamic names via `FUN_1424bb460`, string to token)
  and calls `FUN_140cbfea0(*(gamestate+0x258), token, value, days)`. The global flag container
  is `*(gamestate+0x258)`: a flat array (`data +8`, count `+0x14`) of 0x30-byte entries with
  `+8` token, `+0x18` set date (copied from `gamestate+0x468`), `+0x28` value, `+0x2a` days.
  `FUN_140cbf6d0` is get-or-create (linear search by token). Enumeration is a simple loop.
  Token to string: `FUN_1424ca600(out, token)`.
- **Hook point for load and new game.** `FUN_140dd9b20` (in-game idler, `ingameidler.cpp`) runs
  when the game is entered, for **both** a new game and a loaded save. It fires `on_startup`
  only `if (!FUN_1401e2930(gamestate))`, where `FUN_1401e2930` returns the byte
  `gamestate+0xa34`, i.e. **"loaded from save"**. This explains the in-game observation that
  `on_startup` does not run on load. Hooking `FUN_140dd9b20` and running code after the
  original returns gives one callback for both cases, with the new/loaded distinction.

### Signature status (checked on 1.19.3 patched and 1.19.2)

| Function | Pattern | 1.19.3 (vanilla and patched) | 1.19.2 |
|---|---|---|---|
| `CStrategicRegion::GetNameKey` | `48 83 79 48 00 75 09 48 8B 41 30 48 83 C0 20 C3 48 8D 41 38 C3` (whole function) | 1 match, `0x140e23410` | 1 match, `0x140e0eb10` |
| `CGameState::IsLoadedFromSave` | `0F B6 81 34 0A 00 00 C3` | 1 match, `0x1401e2930` | 1 match, `0x1401e24b0` |

The `CState` equivalent of GetNameKey has a different body (`... 74 05 48 8D 41 38 C3 ...`),
so the region pattern stays unique. The remaining signatures (game-entered function, gamestate
pointer, flag container functions, effect registry) will be derived and checked on both builds
in B3, on vanilla 1.19.3, patched 1.19.3 and 1.19.2.

Vtables can also be found at runtime from RTTI names (`.?AVCRenameProvinceEffect@@` → type
descriptor → complete object locator → vtable). That is more robust than byte patterns for the
effect classes; `rtti.py` in the analysis scratchpad implements it offline.

## B2: options for `set_strategic_region_name`

Requirements: rename and reset at runtime; show everywhere; survive save/load; cosmetic only;
with no loader or on an unsupported build the original name shows. Every option below uses
the engine's own `GetNameKey` path, so every UI location from B1 picks the name up.

### Option 1: native effects

Register `set_strategic_region_name` and `reset_strategic_region_name` in `_EffectEntries`.
The effect class would be a clone of `CRenameProvinceEffect`'s vtable with three slots
replaced: block item (accept `region` and `name`), tooltip, and execute.

- **Pro:** the exact requested syntax; runs wherever effects run (decisions, events, focuses,
  `meta_effect`); validation at execution time; can show a proper tooltip.
- **Pro:** much less reverse engineering than feared, thanks to the `CRenameProvinceEffect`
  template.
- **Con:** new engine surface: create a token for the new names, insert into the registry after
  hoi4.exe's static initialisers ran but before scripts are parsed, clone vtables, implement a
  tooltip. The most fragile option on updates (vtable layout, registry type, token API).
- **Con, important:** without the loader, or on an unsupported build (engine tier off), every
  use is an **"Unknown effect" parse error**. The line is dropped and the rest of the block
  still runs, but `error.log` gets spammed and mods cannot ship this safely to non-loader
  users. A mitigation would need a new VFS feature (mod-supplied "overlay" files served only
  when the engine feature is active).
- **Con, multiplayer:** game state written by the effect would differ between clients with
  and without it. The loader also changes the files the game reads, so a mixed lobby should
  already fail the checksum, but this is a reason to keep the effect's state change identical
  everywhere.

### Option 2: flag protocol + engine watcher (no new effects)

Names are expressed as plain global flags, set by normal script:

```
set_global_flag = hoi4_srn_42_MY_LOC_KEY   # rename region 42
set_global_flag = hoi4_srn_42_reset        # back to the original name
```

The loader enumerates `hoi4_srn_<id>_<key>` flags. For each region, the most recently set one
wins (entry set date, and live ordering from a set/clear hook). It answers `GetNameKey` from
that table. Updates come from:
- the game-entered hook, on load and new game;
- a hook on the global flag set/clear path, for immediate updates (this also covers flags set
  by `meta_effect`, which runs the same effect classes).

After an update it refreshes the map labels and open panels. The loader can serve readable
wrappers through the VFS, e.g. per-region `hoi4_srn_reset_42 = yes`.

- **Pro:** script stays vanilla. No parse errors without the loader, and a save with these
  flags loads anywhere; without the loader or on an unsupported build the flags are inert, so
  the original names show.
- **Pro:** persistence comes free: flags are saved with the game. Load the save with the loader
  again and the names come back.
- **Pro:** no effect registry, vtable cloning, parser or tooltip work; smallest engine surface.
- **Pro:** deterministic. Flags are set by identical script on every client, and the name
  lookup is local and cosmetic, so it cannot desync.
- **Con:** not the requested syntax. A dynamic key needs `meta_effect`. The decision tooltip
  shows "Set global flag hoi4_srn_42_..." unless the modder hides it with
  `hidden_effect` + `custom_effect_tooltip`.
- **Con:** no script-time validation; invalid ids or keys are reported in `loader.log`.
- **Con:** flags accumulate (one per key ever used per region; bounded by the mod's key set).
  Timed flags (`days =`) are not supported.

### Option 3: native effect that writes the engine's own override (`region+0x38`)

The smallest possible implementation: the engine saves and loads the field itself.

- **Con, disqualifying:** the vanilla engine reads `name` back from the save. A save loaded
  without the loader, or on an unsupported build, **keeps showing the custom name**, or the raw
  key if the mod's localisation is missing. That violates the requirement, and the save cannot
  be cleaned without the loader. Rejected; listed for completeness.

### Recommendation: Option 2 as the core, with Option 1's syntax added on top

1. **First, Option 2 (core):**
   - the flag protocol;
   - the `GetNameKey` hook, which never writes `region+0x38`, so saves stay vanilla;
   - the game-entered lifecycle callback (also used to refresh `hoi4_loader_plugin_*` flags
     immediately after loading);
   - the flag set/clear hook;
   - map label refresh.

   It meets every requirement, including the no-loader and unsupported-build cases. It is
   multiplayer-safe and has the smallest, most update-resistant engine footprint.
2. **Then, optionally, Option 1 as thin syntax sugar:** native
   `set_strategic_region_name = { region = 42 name = KEY }` and
   `reset_strategic_region_name = { region = 42 }` whose execute **only sets the corresponding
   `hoi4_srn_*` flag** (via the engine's flag container), so storage and behaviour stay
   identical to the core. Mods that must also run without the loader keep using the flag
   protocol; mods that require the loader get the nice syntax. The test mod would exercise
   both.

The core is the part that has to be right; the native effect is a front-end that can be added,
or dropped after an update, without touching persistence.

### Flag names built from localisation keys (if the flag approach is used)

- **Characters.** Across all 148,611 vanilla localisation keys the character set is
  `[A-Za-z0-9_]` plus `.` (24,203 keys, e.g. `political.14.a.tt`) and `-` (67 keys, e.g.
  `ETH_italo-ethiopian_empire_decisions`).
  - `.` already appears in vanilla flag names (`news.157_fired`, `SOV_soviet.48_trotsky`), so it
    is safe.
  - `-` never appears in a vanilla flag name. Its handling by the script lexer is unverified
    (risk: parsed as a separator or sign).
  - Spaces, quotes, `:`, `@`, `[`, `]`, `$`, `=` and `#` do not occur in keys, and some of them
    are special in script.

  Plan: accept `[A-Za-z0-9_.]` as-is; test `-` in B3 and, if it fails, reject such keys with
  a log line.
- **Length.** Vanilla keys are up to 111 characters (99th percentile 54); vanilla flag names
  reach 68. `hoi4_srn_<id>_` adds up to 13, giving 124 at most. No engine limit was found:
  flags are interned tokens backed by strings, and binary saves store strings with a 16-bit
  length. B3 will test a 124-character flag through set, save, load and `has_global_flag`;
  until verified, the loader logs and ignores names over a conservative cap.
- **Parsing.** The flag carries `<id>` then `_` then `<key>`. The id is all digits, so the first
  `_` after the prefix is unambiguous even when the key contains `_`. The reserved key `reset`
  means "original name". A key equal to `reset` would be unusable, which is acceptable.

### Behaviour without the loader or on an unsupported build

- **Options 1 and 2 (anything that never writes `region+0x38`):** the engine sees no
  override, `GetNameKey` returns `STRATEGICREGION_<id>`, and the original name shows
  everywhere. The `hoi4_srn_*` flags stay in the save, unused. Loading the same save later
  with the loader on a supported build brings the names back. With Option 1 alone, script
  using the effect also logs "Unknown effect" parse errors in that setup.
- **Option 3:** the custom name persists in the save and keeps showing. This is why it is
  rejected.
- **Missing localisation key (planned for B3):** the loader will check the key against the
  game's localisation before using it. If it is missing, the region keeps its original name
  and `loader.log` says why, so the UI never shows a raw key. The localisation lookup function
  still has to be located; the panel code shows the call path (`FUN_14011e010`) to start from.
- **Invalid region id:** ignored with a log line.
