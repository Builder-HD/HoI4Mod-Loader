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

- **Correction (in-game test 2026-10-13):** callers display its result **as text**. Returning
  a localisation key showed the raw key in the UI. The template's name string at `+0x20` is
  therefore most likely already the localised name; the read function fills both `+0x40` (key)
  and `+0x20`. The loader now logs the engine's own result for region 42 to confirm. It is
  looked up each time (the region panel update `FUN_1417369d0` copies it into the widget
  text); there are no cached copies in game state.
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

  **Result (B3, in game):** `[A-Za-z0-9_.-]` is accepted. `-` works in the effect argument and
  in `has_global_flag`.
- **Length.** Vanilla keys are up to 111 characters (99th percentile 54); vanilla flag names
  reach 68. No engine limit was found: flags are interned tokens backed by strings, and binary
  saves store strings with a 16-bit length. **Result (B3):** the loader caps keys at 100
  characters (flags at 112). A 112-character flag is verified through script parsing and
  `set`/`has_global_flag`; save/load with it is still open (see B3 results). The cap stays.
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
  *(Superseded in B3: located; see below.)*
- **Invalid region id:** ignored with a log line.

## B3: implementation (option 2 approved: flag core plus native syntax)

Approved 2026-10-12. Design as recommended in B2: the flags are the only storage, and the
native effects only manage them.

### Additional engine findings used by B3

- **Region by id:** `gamestate->regions[id]`, array at `gamestate+0x2e0`, count `+0x2ec`,
  valid `1 <= id < count` (`FUN_1413a7770`, used by naval triggers: "Region ID had an unexpected
  value"). The offsets are read from the signature at runtime.
- **Tokens:**
  - `FUN_1424bb460(int* out, {const char*, size_t}*)` interns a string, creating dynamic tokens.
    It asserts "lexer is synchronized initialized".
  - `FUN_1424bc260(int token)` returns the token's `const std::string*` from the lexer table
    (locked).
  - The lexer becomes ready at the end of `FUN_140176f30`, which writes the ready byte; it is
    called only indirectly. `FUN_1424cc9e0`/`FUN_1424ca600` are just int-to-string, not
    token lookups.
- **Flags:**
  - `FUN_140cbfea0(flags, token, value, days)` sets; `FUN_140cbfe60(flags, token)` clears (both
    called from the effects' Execute).
  - A flag entry (`CScriptFlag`, 0x30 bytes) holds the token at `+8`, the set date at `+0x18`
    (corrected in the D3 follow-up; `+0x20` is the date's secondary vtable, which the save
    reader calls for token `0x284a`), the value at `+0x28` and the days at `+0x2a`.
- **Localisation:** `FUN_142245e60(std::string* out, const char* key, 0, 0)` is `Localize()`
  (`localize.cpp`). `FUN_14011e010` is only a std::string copy; widgets localise keys themselves.
- **Effect registry:** each registration stub is
  `entry = new CEffectEntry<T>{vtable, help}; Register(_EffectEntries, token, entry)`. The loader
  finds the `set_province_name` stub through its help string and reads, from fixed offsets
  checked by opcode, the registry global, the entry vtable, the token and the `Register` function.

### How it is wired

| Piece | Mechanism |
|---|---|
| `engine.cpp` | Resolves the 14 signatures, the anchor and RTTI vtables (`.?AVCEffect@@`, `.?AVCSetGlobalFlagEffect@@`, `.?AVCClearGlobalFlagEffect@@`, `.?AVCRenameProvinceEffect@@`). Groups fail independently. |
| `lifecycle` feature | Hooks the lexer sync (registers native effects and tokens right after it, before scripts using them are parsed) and "game entered" (callbacks get `loadedFromSave`). After a loaded save it sets `hoi4_loader_present` and sets or clears every `hoi4_loader_plugin_*` flag immediately. |
| Display | `CStrategicRegion::GetNameKey` is hooked. If the region (id = `template+0xa0`) has an entry in a lock-free table of atomic pointers, that entry is returned; otherwise the original runs. Entries hold the key's **localised text** (`Localize()` once per key, cached), because callers display the result as is. Strings in the table are never freed (readers on other threads). A language change at runtime keeps the old text until the next rename. |
| Self-check | On every game entered: `RegionTemplateId(regions[i]) == i` for all regions, else the feature deactivates itself and clears its flag. |
| Flags to names | Rescan on game entered, after every `set_global_flag` / `clr_global_flag` Execute (hooked via RTTI vtable slot 13) and after the native effects. The latest set date wins, ties go to the larger key; the key must exist in localisation. Changes set the map manager's names-dirty flag (`+0x81 = 0x101`), as `set_state_name` does. |
| Native effects | Tokens `set_strategic_region_name`/`reset_strategic_region_name` are registered with entries whose factory calls `set_province_name`'s factory and then swaps in a cloned vtable (slot `[-1]` keeps the RTTI locator). Slot 4 maps `region` to `id`; slot 7 returns an empty tooltip; slot 13 runs set or reset under SEH. Before cloning, slots 4/7/13 are checked to be real overrides of `CEffect`. |
| Kill switch | `[features] region_names = 0` (or `lifecycle = 0`) in `loader_config.ini`. |

### In-game test results (2026-10-13, vanilla 1.19.3, loaded save)

Verified:
- **Native effects:** registered in time. The lexer-sync callback ran at +2.5 s, before any
  script, and `error.log` had no "Unknown effect". Rename A/B and reset work.
- **Flag protocol:** decision C works.
- **Task forces** show the new name.
- **Persistence:** name B survived save, game restart and load. The game-entered (loaded save)
  rescan restored it, and the plugin flags were refreshed immediately.
- **Missing key:** detected (`Localize()` returned the key or nothing); the region keeps its
  name and `loader.log` has an error line.
- **Invalid region 9999:** refused with a log line, no crash.
- **Self-check:** passed with 304 regions.

Found and fixed: the UI displayed the raw key (see B1 #2 correction). The table now holds
localised text.

Re-test after the fix (2026-10-13, new game and loaded save):
- **Real names** ("Loader Test Sea A") everywhere checked, with no raw key.
- **Engine behaviour confirmed:** `engine name of region 42: "Bay of Biscay"`, so `GetNameKey`
  returns display text.
- **Map labels** in air and naval map modes update after a rename. The names-dirty flag is
  enough; no label rebuild call is needed.
- **Air wings and task forces** show the new name.
- **Effect tooltip:** the native effect adds no visible line. The odd tooltip text that showed
  up was the test decision's own description, which contained `{` (HOI4 localisation treats `{`
  as markup and swallowed `{ r`). The descriptions were reworded.

Dash and long-key test (2026-10-13 19:47 session, new game; checked from `loader.log`,
`logs/error.log` and the save folder):
- **`-` in keys works:** `HOI4_LOADER_TEST-REGION-NAME-DASH` renamed region 42 to "Loader Test
  Sea Dash-Key".
- **The 100-character key (112-character flag) works.** Long names are cut off in labels and
  tooltips exactly like vanilla's long sea zone names.
- **Script parsing is verified for both:** the decisions file
  (`set_strategic_region_name = { ... name = <key> }`) and the scripted localisation
  (`has_global_flag = hoi4_srn_42_<key>`) were parsed at startup with no `error.log` entries.
- **Save/load is NOT verified for these two flags.** That session never loaded a save, and no
  save made after the renames exists. Save/load is verified for the shorter keys (A, B).

Still open:
1. **Save/load with the dash and 112-character flags:** take one of the two decisions, save,
   restart, load, and expect `region 42 -> <key>` after `game entered (loaded save)`.
2. **Unchecked UI places:** the region panel/tooltip and naval mission screens were not
   explicitly reported. They use the same `GetNameKey` path as the places that work.

Not covered: `modify_global_flag`, flags with `days =` expiring, and console flag commands do not
trigger a rescan. The next game-entered or flag effect picks the change up.

---

# Part C: native effect and trigger API

## C1 findings: triggers (vanilla 1.19.3 copy; structure verified on patched 1.19.3 and 1.19.2)

Addresses are 1.19.3 for orientation. All anchors and RTTI classes below exist in all three
builds with identical vtable slot counts.

### Registration

- **Registry.** Triggers have their own registry, a `std::map<int token, CTriggerEntryBase*>` at
  `DAT_143330050` (created lazily by `FUN_14054cea0`), next to the effects' `DAT_143330000`. The
  documentation generator (`FUN_140299ad0`) dumps both.
- **Stub.** There are 595 trigger registration stubs, all the same shape as the effect stubs
  except for a 0x18-byte entry:
  `entry = new CTriggerEntry<T>{ vtable, help, byte flag }; FUN_140550970(registry, token, entry)`.
  The byte at `+0x10` is 1 in 584 stubs and 0 in 11 (e.g. a conscription-ratio trigger); its
  meaning is unknown and new entries use 1.
- **Anchor.** `has_global_flag`'s stub (token 0x2bf1) references its unique help text
  ("has global flag been set.Check flag val date set and days since set.\nExample: ..."), so it
  can serve as the trigger-registry anchor exactly like `set_province_name` does for effects.
- **Entry vtables.** `CTriggerEntry<T>` has slot 0 = destructor and slot 1 = factory, like
  `CEffectEntry<T>`.
- **Map layout.** Both registries are MSVC `std::map`. A node is
  `{left, parent, right, color, isnil @+0x19, key int @+0x20, value @+0x28}`, read in
  `FUN_1405415a0`. A lookup can therefore be done read-only by walking the tree, which is how a
  "name already exists" check can work.
- **Register function.** It overwrites an existing key; only debug builds assert. So refusing
  duplicates is the loader's job.

### `CTrigger` interface (23 slots; vtable `.?AVCTrigger@@`)

| Slot | Role (evidence) |
|---|---|
| 0 | destructor |
| 3 | checked evaluate: scope check, then slot 22 (`FUN_14054d0b0` prints ": Invalid Scope, supported: ... provided: ...") |
| 4 | read single value (`trigger = value`). The base handles scope targets first (`THIS` 0x29c3, `ROOT` 0x2c94, `PREV` 0x2c95, `FROM` 0x298f, `event_target:`...) and records them as flags in `+0x24`. |
| 5 | read clause. Base `FUN_14054fd00`: block loop, calls slot 6 per key and slot 7 to validate ("Non-assign trigger is not enclosed..."). Single-value triggers override it to call slot 4 ("Not a valid value"). |
| 6 | read one block item `(this, parser, keyToken, ctx)`; the base rejects unknown keys |
| 14 | **supported scopes mask** (pure virtual), e.g. 0 = any (`always`), 2 = State, 4 = Country |
| 15 | scopes that a scope-target value may point at (pure virtual), e.g. `0x5fc` |
| 18 | dependency hook per scope type (base no-op) |
| 21 | **tooltip** `std::string* (this, std::string* out, ctx, bool negated)` (pure virtual). E.g. `always`: picks `TRIGGER_ALWAYS_TRUE/FALSE` by `negated`, localises, formats with the scope (`FUN_1410e48a0(out, ctx, text)`), appends `\n`. |
| 22 | **evaluate** `bool (this, CEventScope*)` (pure virtual) |
| 23 | numeric triggers only: `GetValue(scope)`, compared in slot 22 |

The effect side, for comparison: slot 3 reads the clause (the base loops over a block and calls
slot 4 per key; single-value effects call slot 6), slot 7 is the tooltip, slot 13 executes and
**slot 19 is the supported-scopes mask** (e.g. `set_state_name` returns 2).

### Templates to clone

- **Single value:** `CAlwaysTrigger` (`always = yes`). The object is 0x60 bytes and its only own
  field is a bool at `+0x58`. Slots: 4 reads a bool via `FUN_1424c5640(value, bool*)`, 22 returns
  it, 21 is the tooltip. Integers are read the same way with `FUN_1424c5220(value, int*)`
  (`num_of_factories`).
- **Block with named parameters:** any trigger can be given the base slot 5 (block loop) and a
  custom slot 6. The parser reads typed values through `FUN_1424c08d0(parser, int*)` (int),
  `FUN_1424c0ab0(parser, std::string*, 0)` (string) and `FUN_1424c2060(parser)` (unexpected key);
  `has_global_flag` (0xb0-byte object) is the vanilla example of a block trigger. Effects:
  `CRenameProvinceEffect` (0x80 bytes, block, any scope) as already used in B3.
- **Spare room in the object:** each template's private area starts at `+0x58`. A generic
  command keeps a pointer to its own parsed-argument record there; every slot that touches that
  area is replaced.

### Comparison operators

- **Value triggers with comparison** (`num_of_factories > 50`) share one base:
  - slot 5 reads the operator token from the parser (`parser+0x78`: 0x1d3 = `>`, 0x1d4 = `<`; any
    other token logs "Not a valid compare token in trigger" and falls back to equality), then
    calls slot 4;
  - slot 4 stores an int (`+0x60`) or a variable/script value (`+0x68`, flag `+0x70`);
  - slot 22 compares `GetValue(scope)` (slot 23) with it using the operator at `+0x58`.

  **A clone of `CNumOfFactoriesTrigger` with slots 14, 21 and 23 replaced gets `>`, `<`, `=` and
  variable arguments for free.** Effort: small.
- **Operators inside blocks** (`has_global_flag = { value < 5 }`): the block item handler would
  read `parser+0x78` per key and store the operator per parameter. Feasible, but a separate step.

### Scopes

`CEventScope` (persistent; written by `FUN_14053bda0`) holds one current scope plus links. The
scope type is a bitmask computed from which field is set (`FUN_140537d90`); names come from the
table at `0x1427d4500`:

| Mask | Scope | Field |
|---|---|---|
| 0x2 | State | `+0xa8` int state id |
| 0x4 | Country | `+0x08` int tag index |
| 0x8 | Character | `+0x50` 8-byte id |
| 0x10 | Combatant | `+0x60` pointer (not saved) |
| 0x20 | Ace | `+0x68` id |
| 0x40 | StrategicRegion | `+0x48` `CStrategicRegion*` (confirmed by `add_region_efficiency`) |
| 0x80 | Operation | `+0x58` id |
| 0x100 | Unit | `+0x70` id |
| 0x200 | Military Industrial Organisation | `+0x78` id |
| 0x400 | Purchase Contract | `+0x80` id |
| 0x800 | Raid Instance | `+0x88` id |
| 0x1000 | Special Project | `+0x90` id |
| 0x2000 | Faction | `+0x98` id |

`ROOT`, `FROM` and `PREV` are `CEventScope*` at `+0x18`, `+0x20` and `+0x28` (pointing to the
scope itself when unset). Effects (slot 13) and triggers (slot 22) receive the `CEventScope*`
directly. The state, country and region fields are plain reads; the 8-byte ids need lookups.

### Tooltips

- **Triggers:** slot 21's text appears in requirement lists (decisions, focuses) and gets the
  ✓/✗ marking from the engine. The `negated` argument tells the trigger it sits under `NOT`.
- **Building text:** `Localize(key)` gives an engine-owned `std::string`. Arbitrary text
  can be built in engine memory with the engine's `std::string::assign(ptr, len)`
  (`FUN_14011d970`, used by `Localize` itself), so the engine can free it later. The B3 effects
  avoid this by returning an empty string; triggers need real text.
- **Effects:** slot 7, same signature without `negated`.

### Why triggers are riskier than effects

1. **Volume.** Triggers run for every AI country, every visible decision or focus tooltip and
   every frame a tooltip is open. A slow `evaluate` costs game speed for everyone.
2. **Concurrency.** AI evaluation runs on worker threads; the engine's
   `_ThreadForbidCount`/gamestate asserts exist because of it. `evaluate` must be thread-safe
   and lock-free, and must not allocate or log on the normal path.
3. **Determinism.** AI choices follow trigger results, so a trigger that reads anything but game
   state desyncs multiplayer.
4. **Without the loader the semantics invert.** An unknown *effect* is dropped, so nothing
   happens, which is safe. An unknown *trigger* is also dropped, so the condition disappears and
   the surrounding `visible`/`available`/`limit` becomes **true**. Mods must guard native triggers
   with the feature flag in the same block (e.g.
   `has_global_flag = hoi4_loader_plugin_region_names`).
5. **Tooltips allocate.** That is acceptable in UI code, but the tooltip callback must still be
   bounded and must never touch game-state locks.

## C2: API design (approved 2026-10-13)

Decisions:
- design approved as below;
- no comparison operators in v1 (numeric `name > 5` triggers are the next step);
- the `id` alias is dropped (only `region`);
- a generic tooltip line for triggers without a tooltip callback;
- **addition: per-feature guard triggers**, see C3.

### Shape

```cpp
// src/native_script.h (internal API for plugins)
enum class ParamType { Int, Bool, Key };   // Key = identifier-like string (loc keys, names)
enum class Form { Value, Block };          // name = <value>   |   name = { key = value ... }
struct ParamSpec { const char* name; ParamType type; bool required; };

struct Args {                               // typed, in schema order; filled at parse time
    bool has(int i) const; int i(int i) const; bool b(int i) const; const char* key(int i) const;
};
struct Scope {                              // read-only view of the engine's CEventScope
    uint32_t types() const;                 // ScopeMask bits that are set
    int stateId() const; int countryTag() const; int strategicRegionId() const;
    Scope root() const; Scope from() const; Scope prev() const;   // invalid view if unset
};
struct TooltipText { void add(const char* utf8); };   // builds the engine-owned tooltip string

struct NativeEffect {
    const char* name; const char* help; Form form; std::vector<ParamSpec> params;
    uint32_t scopes;                        // ScopeMask, 0 = any
    void (*execute)(const Args&, const Scope&);
    void (*tooltip)(const Args&, const Scope&, TooltipText&);           // optional
};
struct NativeTrigger {
    const char* name; const char* help; Form form; std::vector<ParamSpec> params;
    uint32_t scopes;
    bool (*evaluate)(const Args&, const Scope&);                     // hot path
    void (*tooltip)(const Args&, const Scope&, bool negated, TooltipText&);  // optional
};
bool register_native_effect(const NativeEffect&);    // call from the feature's init
bool register_native_trigger(const NativeTrigger&);
```

### Behaviour

- **When.** Plugins call `register_*` from their feature init (in `DllMain`). Calls are validated
  immediately (schema: name and parameter names `[a-z0-9_]+`, no duplicate parameter, `Value`
  form has exactly one parameter, callbacks present) and queued. The engine registration
  happens in the `lifecycle` lexer-ready callback. Any call after that window is refused with a
  log line. Commands therefore exist only while their feature is active.
- **No clobbering.** At registration the name's token is looked up read-only in the engine
  registry (tree walk) and in the loader's own list. If either has it, that command is refused
  and logged. The plugin's other commands still register, and the feature is told so it can log
  or deactivate.
- **Engine binding** (one place, `native_script_engine.cpp`), per command:
  - Clone the template vtable, keeping the RTTI locator at `[-1]`:
    `CRenameProvinceEffect` for effects, `CAlwaysTrigger` for triggers.
  - Replace these slots:
    - **effects:** block 3 (base loop) / 4 (item), value 3 / 6, 7 (tooltip), 13 (execute),
      19 (scopes);
    - **triggers:** block 5 (base loop) / 6 (item), value 5 / 4, 14 (scopes), 15 (`0`),
      21 (tooltip), 22 (evaluate).
  - The factory calls the template's factory, then sets our vtable and an argument-record
    pointer at `+0x58`.
  - Entries are `{vtable, help}` for effects and `{vtable, help, 1}` for triggers.
- **Parsing.**
  - The item handler maps key token to parameter and reads it with the engine's readers (int,
    bool, string), so type errors are reported by the engine with file and line.
  - Unknown keys go to the base handler (engine error).
  - A missing required parameter is logged once per command and marks the record invalid.
  - Records are allocated at parse time and never freed: they are bounded by script size, and
    the engine may copy or keep objects longer than expected.
- **Callbacks.**
  - Every callback runs under SEH; an exception logs once per command (atomic flag).
  - Effects do nothing on an invalid record or an exception. Triggers return `false`, and the
    engine applies `NOT` itself. So a failing trigger inside `NOT = { ... }` counts as true;
    the README will say so.
  - `evaluate` gets lock-free, allocation-free access to `Args` and `Scope` (plain reads).
- **Tooltips.**
  - `TooltipText` assembles UTF-8 text and hands it to the engine through `std::string::assign`
    (engine allocator); a trailing `\n` is added as vanilla does.
  - Triggers without a tooltip get a generic line (`<name>: <param>=<value> ...`).
  - Effects without a tooltip show nothing, as now.
- **Generated docs.** After registration, `loader_script_docs.md` next to `loader.log` lists, per
  command: kind, feature, status (registered or refused with reason), scopes (names from the
  engine table), form, parameters (name, type, required) and help.
- **Scope safety.**
  - `Scope` only reads the three plain fields (state, country, region id via the template) and
    the `ROOT`/`FROM`/`PREV` links, each guarded by the type mask.
  - The other scope types (ids that need engine lookups) are exposed only as "type present" in
    v1.

### Deliberate v1 limits

- **No comparison operators.** Proposed next step: a `NumericTrigger` kind on the
  `CNumOfFactoriesTrigger` template (`name > 5`, `< 5`, `= 5`, variables) with
  `int (*value)(const Args&, const Scope&)`.
- **Scope targets as values are not offered** (`name = ROOT` style).
- **The `id` alias goes away.** `set_strategic_region_name` currently also accepts `id = 42` (a
  side effect of the `set_province_name` clone). After the move to the API only `region` is
  accepted.

### Testability (C5)

The registration logic splits into an engine-free core and the engine binding:
- **Engine-free core:** schema validation, the registration window state machine, conflict
  checks against an abstract registry, and the documentation writer.
- **Engine binding:** vtable cloning and the engine reader calls.

The smoke tests link the core directly with a fake registry and check:
- valid and invalid schemas;
- duplicate and vanilla names refused;
- calls before and after the window refused;
- the exact docs file content.

The engine binding is covered by the in-game test plan.

## C3-C5: implementation

### Files

| File | Content |
|---|---|
| `src/native_script.h` | Public plugin API (`ParamSpec`, `Args`, `Scope`, `TooltipText`, `NativeEffect`, `NativeTrigger`, `register_native_*`, `NativeFeatureComplete`) and the internal core/binding contract (`Command`, `ParsedCommand`, `NativeBackend`). |
| `src/native_script.cpp` | Engine-free core: schema checks, window, loader-side duplicates, finalize (engine-name conflict, token interning, bind), typed `NativeSetArg`, `NativeValidate`, SEH-wrapped `NativeExecute` / `NativeEvaluate` / `NativeTooltip`, docs writer. |
| `src/native_script_engine.cpp` | Binding: vtable clones, generic slots, entries and factories, `Scope` accessors, `NativeScriptRegisterWithEngine()` (lexer-ready). |
| `src/engine.cpp` | Trigger registry from the `has_global_flag` anchor; `MapHasKey` (read-only `std::map` walk) for both registries; `ParserReadString`, `ParserUnexpected`, `ConstructEngineString`; RTTI vtables of the templates. |
| `src/vfs.cpp` | Lazy virtual files (generated on the first served read, under an SRW lock). |
| `src/features.cpp` | Guard triggers per feature; native registration queued at lexer-ready after the features' own callbacks. |

### Binding details

| | Effect (template `CRenameProvinceEffect`, 0x80) | Trigger (template `CAlwaysTrigger`, 0x60) |
|---|---|---|
| Clause reader | block: `CEffect` slot 3; value: `CSetStateNameEffect` slot 3 | block: `CTrigger` slot 5; value: `CAlwaysTrigger` slot 5 |
| Per-key / value | slot 4 / slot 6 | slot 6 / slot 4 |
| Validate | slot 5 | slot 7 |
| Tooltip | slot 7 (empty without callback) | slot 21 (+`\n`; generic line without callback) |
| Run | slot 13 execute | slot 22 evaluate |
| Scopes | slot 19 | slot 14 (slot 15 = 0) |
| Entry | `{vtable, help, cmd, objVt}` | `{vtable, help, 1, cmd, objVt}` |

- **Records:** objects come from the template's factory; the binding swaps in its vtable and
  stores a `ParsedCommand*` at `+0x58`, the template's private area, used only by replaced slots.
- **Values are read as text:** `ParserReadString` for block items, the parser value struct
  (`+8` text, `+0x14` length) for single values. The core converts the text: the engine's int
  readers have 11 and 4 identical copies, so they can't be signatures, while the text reader is
  unique.
- **Unknown block keys:** they go to `ParserUnexpected` (the engine's "Unexpected token" with
  file/line, which skips the value). They are **not** passed to the base per-key slot: `CEffect`
  is itself an effect container, and its slot 4 would parse the key as a nested effect.
- **Leaks, deliberate and bounded by script size:** parse-time allocations (records and engine
  strings over 15 characters from `ParserReadString`) are never freed.
- **Engine-owned strings:** tooltip strings are built with the engine's own string
  construction, so the engine frees them with its allocator.

### Guard triggers (approved addition)

- **What is served:** for every feature, `common/scripted_triggers/hoi4_loader_feature_<name>.txt`
  is a lazy virtual file:
  - `always = yes` iff the feature is active **and** `NativeFeatureComplete` (every native
    command of the feature registered; true for features without any);
  - otherwise `always = no`, with the reason as a comment, also logged as
    `Guard trigger hoi4_loader_feature_<name> = ...`.
- **Ordering:** in-game logs so far show the lexer-ready callback at about +2.5 s and the first
  script file read at about +6 s. A guard file read before registration would get `no` (fail
  closed) and log "read BEFORE native registration ran".
- **Runtime deactivation:** a feature deactivated later in the session (e.g. a `region_names`
  self-check failure) keeps its served guard value for that session. Its native triggers then
  evaluate false anyway (the name table is disabled).

### Behaviour changes in region_names

- **Native commands are declarations now:** `set_` and `reset_strategic_region_name` are schemas
  through the API. Behaviour is unchanged except that only `region` is accepted (`id` is gone).
- **New trigger** `strategic_region_has_custom_name = { region = N }`: a lock-free check of the
  display table (false while the self-check hasn't passed). The tooltip names the region as it
  currently shows.

### Out-of-game tests

`tests/native_script_test.cpp`: 64 checks on the core with a fake backend, all passing. The
stand-in exe checks the guard files: `engine_probe` = yes or no by tier, `region_names` = no.

### To verify in game

1. **Registration:** `loader.log` shows the trigger registry, the three region commands
   registered, and the docs file written.
2. **Regression:** the region effects behave as before.
3. **Requirement tooltips:** the trigger's tooltip in decision requirements (`available`, plus
   `NOT`) looks right.
4. **Parse errors:** unknown or invalid parameters produce engine errors with file and line,
   not crashes.
5. **No loader:** the guard keeps guarded decisions hidden immediately after loading a save
   made with the loader, while the "UNGUARDED" decision shows up.
6. **Docs file:** `loader_script_docs.md` content.

### In-game results (2026-10-13, vanilla 1.19.3)

- **Registration:** at lexer-ready (+2.6 s after load) `loader.log` showed 3 of 3 commands
  registered and the docs file written. The first guard-file read came at +6.1 s and was served
  `yes`, so the ordering holds with about 3.5 s of margin.
- **Trigger and tooltips:**
  - `strategic_region_has_custom_name` flips correctly with rename and reset, in `visible` and
    in `available`;
  - the negated requirement line, read while renamed, was "Strategic region Loader Test Sea A
    (42) does NOT have a custom name";
  - the effects work as before after the migration;
  - after a save load, `region 42 -> HOI4_LOADER_TEST_REGION_NAME_A` came back through the
    rescan.
- **Without the loader** (`version.dll` removed, save made with the loader loaded):
  - the region shows "Bay of Biscay";
  - the guarded decision is hidden immediately;
  - the UNGUARDED decision (plugin flag as guard) is visible, which confirms the stale-flag
    hazard;
  - the status event showed trigger no, guard no, plugin flag stale "active";
  - `error.log` has only the expected `Unknown trigger-type` / `Unknown effect-type` lines
    (`trigger.cpp:568/700`, `effect.cpp:358/445`).
  - The `remotefile.cpp:238 Failed allocate data buffer` lines appear without the loader too, so
    they are unrelated to it.
- **Test-mod fix from this run:** the flag-protocol decision used the saved
  `hoi4_loader_present` flag, which was stale without the loader. It now uses the
  `hoi4_loader_present = yes` trigger. The status decision is always visible.
- **Follow-up run (2026-10-13), all passed:**
  - `error.log` with the loader has no test-mod errors;
  - `loader_script_docs.md` lists all three commands as registered;
  - the dash-key and long-key names survive save and load.
  The dash/long save/load item from Part B step 0 is closed by this run.

# Part D: on_game_loaded

## D1: findings (1.19.3 in Ghidra; addresses are from that build and given for orientation only)

### How on_actions are defined: a dynamic, name-keyed database, not a hardcoded list

- **The database:** `COnActionDataBase` (RTTI name, a `TGameItemDatabase`; 0x150 bytes,
  created by `FUN_140a75a40`, the global instance is `DAT_14332efa0`).
  - **Lookup table** at `+0x28`: 0x28-byte entries `{std::string name, int hash}`, count at
    `+0x34`. The hash is a case-insensitive FNV-1a (`FUN_140bc96d0`).
  - **Object table** at `+0x40`: `COnActionList*`, count at `+0x4c`. `[0]` is a
    `TNullObject<COnActionList>`.
  - **Lookup by name:** `FUN_140a792b0(db, const std::string&)` → `FUN_140a75940` compares the
    hash, then `_stricmp`. An unknown name returns the null object.
- **Loading:** `FUN_140a79930` lists every file under `common/on_actions` (vanilla and mods)
  and calls `FUN_140a760d0(db, parser)` for each. Inside `on_actions = { ... }` every key is
  looked up by name. If it is not found, a **new `COnActionList` (0xd8 bytes) is created for
  any key** and appended to both tables. Only the prefixes `on_daily_` / `on_weekly_` /
  `on_monthly_` get extra handling (tag-specific lists, with "%s is not a valid tag of
  on_daily").
  - There is **no whitelist and no error for an unknown key** (`FUN_140625b10`, called per key,
    is only a debug filename-portability check).
- **Merging:** the block is parsed into the existing list when the name exists, otherwise into
  the new one (`list->vtable[3](list, parser)`). So every file and mod that writes
  `on_startup = { ... }` adds to the same list. A new key such as `on_game_loaded` merges
  exactly the same way: the first file creates the list, later files add to it.
- **Consequence:** `on_game_loaded = { effect = { ... } }` in a mod is already parsed into a
  real on_action today, **with or without the loader**. Vanilla simply never fires it. The
  loader only has to fire it; nothing needs registering.

### How the engine fires an on_action (from on_startup)

In the game-entered function `FUN_140dd9b20` (`ingameidler.cpp`), only when
`!IsLoadedFromSave`:

```
seed  = SyncedRandom("ingameidler.cpp", 0x1546)          // FUN_142233fa0 (random.cpp, the OOS-checked game RNG)
CEventScope scope(seed & 0x7fffffff)                     // FUN_140534fe0 (0xb0 bytes), dtor FUN_140535820
list  = OnActionDb_Find(db, std::string("on_startup"))   // FUN_140a792b0
OnAction_Fire(db, list, &scope, 0)                       // FUN_140a79ba0
```

- **`OnAction_Fire`**: does nothing if the game state is being deleted (`gamestate+0xa3a`).
  Otherwise it fires:
  - the list's `events` (`+0x60`, count `+0x6c`), with a scope check per event ("On action event
    X has the wrong scope");
  - one of its `random_events` (`FUN_140a79410`);
  - its `effect` block (`list+0x78`, vtable slot 12, called with the scope).
- **The "game has started" check** (`gamestate+0xa39`) is a debug-build assertion only.
- **Scope:**
  - `on_startup` gets a scope with **no ROOT/FROM** (only the random seed), so country events
    and country effects fail there.
  - Earlier in the same function, for new games the engine itself builds a **country scope for
    the local player** (`FUN_14053a610(scope, &gamestate+0x520 (player tag; +0x524 when 0), 1)`).
    That is exactly what `on_game_loaded` needs.

### Load paths and flags

- **`CCurrentGameState::Load`** (`FUN_1401e2ac0`, logs `CCurrentGameState::Load_START/END`) is
  the only code that sets `gamestate+0xa34` ("loaded from save") = 1. The new-game init
  (`FUN_1401e0610`) and the constructor clear it.
- **Callers of Load:**
  - the file loader dispatch `FUN_140da04f0` (two variants: `FUN_140d9f9c0`, `FUN_140d9ff80`);
  - the stream loader `FUN_141999f00`, which keeps the local player's country and is used by the
    lobby's "File transfer in gamesetup done." path and by `HotJoinGame` (`ingamempcommands.cpp`).
- **Main menu load, Continue, loading from the in-game menu and ironman** all go through the
  file loader, then the in-game idler entry `FUN_140ddc350` → `FUN_140dd9b20`. That is where the
  lifecycle hook already sees "game entered (loaded save)".
  - **Ironman** is `gamestate+0xc0 & 1` and makes no difference for firing.
  - Loading again from the in-game menu is expected to re-enter the idler; the D3 test plan
    checks it.
- **The engine's own session classification** (`playsession_start` telemetry, `FUN_14021ba20`):
  - new/load = `IsLoadedFromSave`;
  - `game_type` = multiplayer if the session's server object (`session+0x58`) is a
    `CNetworkServer` or `CProxyServer`, ironman if `gamestate+0xc0 & 1`, else normal;
  - `hot_join` = `session+0x73e`.
- **Server classes (RTTI):**
  - `CDummyServer`: local, single player;
  - `CNetworkServer`: multiplayer host;
  - `CProxyServer`: multiplayer client;
  - `CDedicatedServer`.
  - The session (`CSession`, `session.cpp`) comes from the idler's vtable slot 17 (`+0x88`) or
    from `(*DAT_14332f698)->vtable[17]`.

### Multiplayer

- **Hot join:** `CSession::bIsHotjoin` (`+0x73e`) is set when the joining client enters
  `HOTJOIN_ASKING_FOR_JOIN` (`FUN_142251120`; also `FUN_142252520(session, bool)`).
  - The joiner loads the host's **current** game state through the stream loader, then enters
    the game. So the game-entered function runs **on the joiner only**, with "loaded from save"
    set.
  - Host and other clients keep running; they don't re-enter.
  - Anything the loader does to game state at that point happens on one machine: an **OOS
    desync**.
- **Session-wide load:** the host loads the save from file; clients receive it by the lobby
  file transfer and load it through `Load` as well. All machines then enter the game before
  the first tick. In principle a local trigger fires on every machine at the same point
  (`on_startup` relies on the same pattern for new games). But:
  - **Not verified statically:** whether a multiplayer **new** game also transfers the state
    to clients (then clients would see "loaded" while the host sees "new"), and which machines
    re-enter after an OOS **resync**.
  - The scope seed uses the synced RNG, so any machine that fires when the others don't also
    shifts the RNG stream.
- **Engine command path:**
  - Lockstep execution is done with `CCommand` subclasses (745 RTTI names), e.g.
    `CSelectEventOptionCommand`, `CExecuteButtonCommand`, and `CPostHotJoinCommand` /
    `CReadyAfterHotJoinCommand` for hot join.
  - There is **no generic "run effect / fire on_action" command**. Adding one means a new
    command class registered in the engine's command factory with matching IDs on all machines.
- **Existing loader code with the same hazard:** `lifecycle`'s "refresh plugin flags after
  load" also writes game state in the game-entered callback. In a hot join it runs on the joiner
  only. It writes the same values the host already has, because all players must run the same
  loader build and features: the served files are part of the checksum. So in practice it
  changes nothing, but it should get the same hot-join guard.

### Without the loader

- **No error expected:** the key is accepted (see above), so `error.log` should say nothing
  about `on_game_loaded`; the list just never fires. To be confirmed in game (D3 test plan).

## D2: options (approved 2026-10-13: Option 1, see "D2 decisions")

### Option 1 (recommended): real on_action, fired by the loader (single player in v1)

- **What the loader does:** after the game-entered callbacks (flag refresh, region rescan),
  when the save was loaded, the loader looks up `on_game_loaded` in the engine's on_action
  database and fires it exactly as the engine fires `on_startup`: synced-RNG seed, engine
  fire function. No registration and no parser hook, because the engine already builds the list
  from every mod's files.
- **Scope:** ROOT = the local player's country (the engine's own player-scope construction),
  so `country_event`, `log`, `set_country_flag` etc. work directly. In observer mode (no
  player country) there is no ROOT, like `on_startup`.
- **When it fires:** only when **all** of these hold, otherwise it logs why and skips:
  - "loaded from save";
  - the server object is a `CDummyServer` (single player);
  - `bIsHotjoin` is 0.
  The test is the engine's own multiplayer/hot-join classification, read through RTTI and
  instruction-derived offsets.
- **Multiplayer: v1 never fires,** with the log line "on_game_loaded not fired: multiplayer
  (host|client, hotjoin=N)". That's safe in every MP case, including hot join and resync. The
  log line also gives the data for a v2.
- **Without the loader:** silent, expected no `error.log` line. The on_action exists but never
  fires.
- **Game update:** about 8 signatures and offsets:
  - DB global, find, fire;
  - scope ctor/dtor and set-country;
  - synced RNG;
  - session getter, server offset, hot-join offset.
  These are ordinary engine functions with distinctive bodies and string anchors
  (`onaction.cpp`, `ingameidler.cpp`). Any miss: the feature stays off, logged, and the guard
  says `no`.
- **Guard:** `hoi4_loader_feature_game_loaded`. Mods normally don't need it (an unfired
  on_action is harmless); it's useful for UI ("migrations supported"). In MP it still says
  `yes`, because it is served before the session type is known; README documents that the
  on_action never fires in MP in v1.
- **Docs:** `loader_script_docs.md` gets an "On actions" section.

### Option 2: scripted effects `hoi4_loader_on_game_loaded_*`, run by name

The loader enumerates the scripted-effect database, sorts the names and runs each with the
player scope.
- **Pro:** independent of the on_action database.
- **Con:** needs scripted-effect enumeration and execution (more reverse engineering than
  Option 1). It's not the requested syntax, and mods must invent unique names.
- **Same MP problem and same SP-only gate.** Without the loader it is silent too.
- **Verdict:** strictly worse than Option 1 now that on_action keys are known to be dynamic.
  Keep it only as a fallback if a future build hardcodes on_actions.

### Option 3 (later, multiplayer): lockstep via a network command

Only the host decides, sends a custom `CCommand`, and every machine runs it in the same tick.
A hot joiner receives the post-command state.
- **Pro:** the only design that is MP-safe by construction.
- **Con:** a custom command class plus registration in the engine's command factory (IDs,
  serialization, replay on hot join). That's the largest and riskiest piece of reverse
  engineering so far. **Not proposed for v1.**

### Option 1b (possible v2, needs an MP test first)

Also fire in multiplayer when the whole session loads together: every machine "loaded",
`bIsHotjoin` = 0, before the first tick. It needs evidence that host and clients agree in
every case: lobby load, new game, resync. The v1 log line in MP (the optional MP step of the D3
plan) gathers it.

### Also proposed

Apply the same hot-join guard to `lifecycle`'s flag refresh: skip it on a hot-join client.

## D2 decisions (2026-10-13)

1. **Option 1, single player only in v1:** approved.
2. **Scope, permanent and forward-compatible with a multiplayer version:**
   - once per human-controlled country, in alphabetical tag order, ROOT = that country;
   - in single player that is just the player's country;
   - with no human-controlled country (observer), once without scope, like `on_startup`.
3. **Hot-join guard on the plugin-flag refresh:** same rule as `on_game_loaded`.
   - **The region rescan stays unguarded,** because it only reads game state. It reads the
     `hoi4_srn` flags and writes the loader's own display table (`g_names`), which the
     `GetNameKey` hook serves. Nothing it writes is game state, saved, or compared between
     machines (the OOS checksum covers game state), so running it on one machine cannot
     desync.
4. **Guard semantics:** documenting is enough. `hoi4_loader_feature_game_loaded` means
   "supported", not "fired". README recommends idempotent migrations run from `on_game_loaded`
   AND a fallback (`on_weekly`), and warns against using the guard to skip the fallback.

## D3: implementation

### Signatures (all unique on vanilla 1.19.3, patched 1.19.3 and 1.19.2, identical extracted values)

| Signature | Where | Yields |
|---|---|---|
| `kSigPlayerScope` | game-entered, new-game player scope | `CEventScope` ctor, set-country, dtor; player tag `+0x520` (imm and disp must agree) |
| `kSigOnStartupSeed` | game-entered, before `on_startup` | `SyncedRandom(file, line)`; ctor cross-checked against `kSigPlayerScope` |
| `kSigOnStartupFire` | game-entered, `on_startup` fire | `COnActionDataBase*` global, `Find`, `Fire`; literal checked to be `"on_startup"` |
| `kSigSessionHotjoin` | `playsession_start` telemetry | idler session getter (vtable `+0x88`), `bIsHotjoin` `+0x73e` |
| `kSigSessionServer` | same | session getter (cross-checked), server `+0x58` |

**Server classes:** the vtables of `CDummyServer`, `CNetworkServer`, `CProxyServer` and
`CDedicatedServer` are found by RTTI name. A server whose vtable matches none of them is
logged with its RTTI name if readable, and counts as "not single player".

### Code

- **`Engine::QuerySession(idler)`:** session → server vtable and `bIsHotjoin`, read under SEH.
- **`Engine::OnActionDefined(name)`:** compares `Find(name)` with `Find("hoi4_loader_nx")`
  (the null object). So there's no hardcoded "is null" offset.
- **`Engine::FireOnAction(name, tag)`** does what the engine does for `on_startup`:
  - seed from the synced RNG (file string "hoi4_loader on_game_loaded");
  - `CEventScope(seed & 0x7fffffff)` in a 0x400-byte buffer (engine size 0xb0);
  - `SetCountry(scope, &tag, true)` if `tag > 0`;
  - `Fire(db, list, scope, false)`, then the destructor;
  - all under SEH; a fault deactivates the feature.
- **lifecycle:**
  - queries the session on every game entry and logs it, e.g. "session: CDummyServer, single
    player";
  - adds `OnGameEnteredLate` (runs after all `OnGameEntered` callbacks: region rescan, flag
    refresh);
  - adds `SessionAllowsLocalWrites`: true only inside a game-entered callback, with a known
    session, single player, no hot join. An unknown session counts as multiplayer (fail
    closed).
- **`plugin_game_loaded.cpp`:** on a loaded save, if `SessionAllowsLocalWrites` and the
  on_action is defined, it fires once with ROOT = the player's country, or once without scope
  when observing. Every skip is logged with its reason.
  - **v1 human countries = the local player:** single player has at most one human country.
    A multiplayer version would collect every human country and sort them by tag at that
    point.
- **Docs:** `document_on_action` adds an "On actions" section to `loader_script_docs.md`.

### Tests

- `native_script_test`: on_actions docs section, 66 checks.
- Smoke runs: `game_loaded` "DISABLED, requires lifecycle" on the stand-in exe; guard file
  served `no`.

### Still to verify in game

- **Fires once** per load path: main menu, in-game load, Continue, ironman.
- **Order:** after the flag refresh.
- **Scope:** ROOT is the player's country.
- **Session:** the class logged in single player is `CDummyServer`.
- **No error** for the on_action without the loader.
- **The weekly fallback** migrates alone (`[features] game_loaded = 0`).
- **Optional multiplayer run:** the logged session classes on host and client, and that
  nothing fires.

### In-game results (2026-10-13, vanilla 1.19.3, single player)

- **Steps 1-6 pass:**
  - on a loaded save it fires right after loading ("fired 1"), and a second load shows 2;
  - it never fires on a new game (`on_game_loaded: new game, not fired`);
  - ironman behaves the same.
- **Session and order:** the session logs as `CDummyServer, single player`. The `loader.log`
  order is right: region rescan, plugin flag refresh, then `on_game_loaded fired`.
- **Step 7 (migration through `on_game_loaded`):** works as expected.
- **Still open:**
  - **step 8:** the fallback on its own, with the kill switch;
  - **step 9:** without the loader (no `error.log` line expected for `on_game_loaded`);
  - **step 10:** multiplayer, including hot join: logged session classes, nothing fired, no
    OOS.

### Follow-up fixes after the D3 test

- **region_names change detection:** it used to rescan (and log) on every `set_global_flag` /
  `clr_global_flag` effect, five times per week from `hoi4_loader_init` alone.
  - It now hooks the engine's flag set/clear functions (`kSigFlagSet` / `kSigFlagClear`,
    already resolved) instead of the two effect `Execute` slots, so the resolved token is
    available.
  - It rescans only for the global flag container and a token whose engine string starts with
    `hoi4_srn_`: a prefix `strncmp`, no allocation.
  - Rescans are logged only when they change something.
  - Once per game entered it logs "since the last game entry: N global flag change(s) ignored
    (not hoi4_srn_), M rescan(s) without change".
  - The native effects edit flags under a thread-local "own edit" marker and rescan once
    afterwards.
  - Coverage: everything that reaches the two engine functions. That's the same effects as
    before plus anything else that calls them.
- **on_game_loaded log line:** it names the tag (`ROOT = FRA`), via `kSigCountryTagName`, the
  engine's tag → string function used by the telemetry `nation` field. It matches once on
  vanilla 1.19.3, patched 1.19.3 and 1.19.2.

### Bug found in the D3 log: the flag's set date was read from the wrong offset

- **Where the date really is:** `CScriptFlag` = `{ vtable, token @+8, CGameDate @+0x10 (date int
  @+0x18; secondary vtable @+0x20, which the save reader calls for token 0x284a), short value
  @+0x28, short days @+0x2a }`.
  - Get-or-create (`FUN_140cbf6d0`) writes the current date to `+0x18` on every set.
  - `CGameDate`'s secondary `Read` (this = `+0x20`) stores at `this-8` = `+0x18`.
- **The bug:** the loader read `+0x20`, the low half of a vtable pointer, identical for every
  flag. So "most recently set flag wins" was really "alphabetically larger key wins". It's
  fixed (`kFlagEntryDate = 0x18`). It only matters when a region has several `hoi4_srn` flags.

### Open: a region name disappears about a week after loading

- **What the log shows:** in the D3 log, region 42 showed name A after two loads of the same
  save (22:18:59, 22:20:22). Both times it went back to its original name at the first weekly
  flag burst (22:19:59, 22:20:59), with no reset and no `clr_global_flag` of the flag.
- **The only engine removal path found besides clear effects: `flag_manager.daily`**
  (`FUN_140cbfa90`). Every day it decrements `days` for each flag with `days > 0` and removes
  the flag when the count reaches 0.
  - The loader always sets `days = 0`.
  - The save writer writes `days` only when > 0; the save reader reads it into `+0x2a`.
- **Unconfirmed explanation:** a flag loaded from the save ends up with a small positive
  `days` (e.g. load copies a flag object whose `days` was not initialised).
- **Diagnostics added:** on every game entry `loader.log` lists each `hoi4_srn` flag with its
  value, `days` (marked EXPIRES if > 0) and set date.
- **Consequence of the new change detection:** an engine-side removal is no longer noticed at
  the next unrelated flag change, only at the next `hoi4_srn` change or game entry. The display
  keeps the name until then. The re-check below decides the real fix.
- **Second observation (2026-10-14, re-check run accidentally still on the old DLL, build
  `a0d36d00…`):**
  - **When it happened:** after loading the name-A save (19:28:20), the name survived the first
    two weekly flag bursts (19:28:58, 19:29:03). It reverted at the third (19:29:51), again with
    no reset and no `clr_global_flag`.
  - **What that suggests:** the flag vanishes a varying number of days after a load (within a
    week in the D3 log, two to three weeks here). That fits an uninitialised `days` better than
    a fixed timer. Still unconfirmed: the flag dump needs the new DLL.
  - **Same run, date-offset bug:** "Rename to C via flags" clears only the A and B flags, so the
    long-key flag stayed. The old DLL then logged "region 42 has 2 hoi4_srn flags; using
    '…VERY_LONG…'": the alphabetically larger key, not the newer C. That matches the
    `kFlagEntryDate` bug fixed in `17c75ef`. With the fix, C must win.

### Re-check with `17c75ef` (2026-10-14, DLL `a0ac28dd…`)

- **Steps 1-4 pass:**
  - **Startup:** `country_tag_name` resolves; the hooks are `flag_set` / `flag_clear`.
  - **No noise:** no rescan lines during play. Entry summaries "15 ignored, 1 rescan without
    change" and "29 ignored, 3 without change" (the entry rescan plus the two `hoi4_srn` clears
    of the C decision).
  - **Renames:** A, B, reset, dash, long and C each log one change. C now wins over the
    remaining long-key flag ("has 2 hoi4_srn flags; using '…_C'"), so the date fix is confirmed
    in game.
  - **Tag:** `on_game_loaded fired: ROOT = FRA`.
- **Step 5, first half:** the name-A save loads with `hoi4_srn_42_…_A: value 1, days 0, set date
  60759371`.
  - `days 0` means the flag has no expiry countdown, so the expiry hypothesis is ruled out for
    this save.
  - Still open: whether the flag survives four weeks of play (status event, then save and
    reload).
- **Step 5, second half: passes.**
  - **What was tested:** from the loaded name-A save, the game ran 3 March to 10 April (5+
    weekly flag bursts, 39 non-`hoi4_srn` flag changes ignored). The map kept "Loader Test Sea A"
    throughout. The game was then saved and reloaded.
  - **Result:** the reload lists the same flag unchanged (`value 1, days 0, set date 60759371`).
    The flag was never removed.
- **Conclusion on the disappearing name:** it does not reproduce with `17c75ef`, so it is
  closed. The flag itself survived; what reverted was the old build's display.
  - **Where it came from:** the old build re-read the global flags after every
    `set_global_flag` / `clr_global_flag` effect `Execute`. Twice it found no `hoi4_srn` flag
    there and reset the name, although the flag existed: the same set date survives in the
    save.
  - **Exact cause:** not identified. One unverified explanation is that those `Execute` calls
    ran while another thread was changing the flag container. The irregular 19:29:51 burst (5
    sets spread over 120 ms) would fit.
  - **Residual risk in the new build:** it still rescans inside the flag set/clear hook, but
    only for `hoi4_srn_` flags. A wrong read could only follow a mod's own region-name change.
    The next change or game entry corrects it.
  - **Superseded:** the rescan is now deferred to the main thread (see "Threading model"
    below), so no hook reads the container any more.

---

# Threading model (the loader's platform rules)

This section holds for every hook into game state, not only `region_names`.

**Method:** Ghidra on the 1.19.3 copy. Evidence came from:
- profiler zone names (`country.daily_parallel`, `flag_manager.daily`, ...);
- RTTI and the template names of the TBB `start_for<...>` vtables;
- the engine's own debug assertions (`ThreadIsMainThread()`, `_ThreadForbidCount`);
- cross-references to the flag functions.

Addresses are 1.19.3. The new signature is checked on all three builds.

## Threads

- **Main thread:**
  - `main` (`FUN_140126e50`) registers it as "Main Thread": `FUN_1424d3fb0` sets a per-thread
    flag, and `ThreadIsMainThread()` (`FUN_1424d3f90`) reads it.
  - It then runs `CApplication::Run` (`FUN_14222e7e0`): `while (!quit) { ...;
    DoFrame(app, true); }`.
- **TBB worker threads:** `tbb.dll`'s task scheduler, used through `PdxParallelFor` /
  `PdxParallelForContainer` (`tbb::parallel_for` with an auto partitioner). The calling thread
  waits for each loop to finish.
- **Others, seen in strings but not traced:** the audio update thread (`AudioUpdateThread`), the
  XInput haptic thread, and threaded loading (`threadedload`) while the databases load.

## One frame: `CApplication::DoFrame(app, bool topLevel)` (`FUN_14222eeb0`)

In order:
1. The main-thread assertion.
2. The synced RNG is switched to "forbidden" for the frame.
3. Events are pumped.
4. The current idler's `Update` runs (`app+0x38`, vtable `+0x20`).
5. If an idler switch is pending, it happens (this is where "game entered", `FUN_140dd9b20`,
   runs).
6. Frame pacing.

**In game**, `CInGameIdler` runs `CSession::Update` (`FUN_142252d00`, `session.cpp`). It executes
the queued commands (vtable `+0x50`, with `session+0x73c` set while one executes): player input,
network commands, and `CHourlyTickCommand::Execute` (`FUN_140f06bf0`, RTTI). That calls
`CGameState::Update` (`FUN_1401dd370`):

| Part | Function |
|---|---|
| hourly | `FUN_1401df400` |
| daily, `gamestate.daily` | `FUN_1401d4810` |
| weekly, `gamestate.weekly` | `FUN_1401f2430` |
| monthly | `FUN_1401e34f0` |
| yearly | `gamestate.yearly` |

**Nested frames:**
- During long ticks the tick functions call `DoFrame(app, false)` themselves: 60+ call sites,
  for example every 5 countries in the daily and weekly country loops and between phases. This
  is gated on `gamestate+0x4c0` and a global switch, and keeps the UI responsive.
- So UI code, and any loader hook into UI functions, can run **on the main thread in the middle
  of a tick**, between two countries. These calls never overlap a parallel section, because
  they are made from the serial code.
- Only `CApplication::Run` calls `DoFrame` with `true`. This is verified on 1.19.3 and 1.19.2
  (`B2 01 48 8B CB E8 <DoFrame>`).

## What runs where

| Work | Thread | Evidence |
|---|---|---|
| `on_weekly`: per country, `CCountry::WeeklyUpdate` `FUN_140718ca0` fires the list cached at `db+0x98` | main, serial loop over countries | `FUN_1401f2430` |
| Country daily on_actions (e.g. `on_border_war_lost`), country flag expiry: `country.daily` `FUN_1406e76a0` | main, serial loop | `FUN_1401d4810`, after `country.daily_parallel` |
| Global flag expiry: `flag_manager.daily` on `gamestate+0x258` | main, serial | `FUN_1401d4810` |
| `on_startup`, `on_game_loaded` (loader) | main, in "game entered" | Part D |
| Commands: decisions, player actions, MP | main, `CSession::Update` | |
| Script effects and triggers called from all of the above | main | |
| `state.daily_parallel` (`CStateDailyUpdateThreaded`), `daily.strategic_region_update`, `country.daily_parallel`, a per-country member update, `CUpdateDiplomacyFirstPass`/`SecondPass`, `country.post_daily_parallel` (`CCountryPostDailyUpdateThreaded`), `hourly_parallel`, weekly career profiles | TBB workers; main waits | `start_for<...>` vtables in `FUN_1401d4810`, `FUN_1401f2430`, `FUN_1401d85a0` |

**Who writes flags.** Callers of `FlagSet` / `FlagClear`:
- the `Execute` slot (13) of the 15 `Set*/Modify*/Clear*FlagEffect` classes: global, country,
  state, character, industrial organisation, project;
- three console commands.

The loader on the save is `CFlagManager::Read`, which creates entries directly. Nothing else
calls them.

## Guards the engine has (and does not have)

- **No lock on flags.** `CFlagManager` holds a `CPdxArray` of 0x30-byte `CScriptFlag` entries:
  pointer `+8`, capacity `+0x10`, count `+0x14`, allocator `+0x18`.
  - Set is a linear search, then an append.
  - Growth allocates a new buffer and moves the entries while **count is temporarily 0**, then
    frees the old buffer.
  - Clear erases in place.
  - So a reader on another thread can see count 0, shifted entries or freed memory.
  - The same pattern (`CPdxArray`, no lock) holds for game state generally.
- **The checks are debug-only.**
  - Gamestate accessors assert `_ThreadForbidCount == 0 && "Current thread is forbidden to
    access gamestate"` (TLS `+0x10`).
  - Many functions assert `ThreadIsMainThread()`: "Should not be called in a thread", "You are
    not allowed to do this here", "Refreshing abilities from non-serialcontext, OOS may occur!".
  - All of these are gated on the assertion switch `DAT_1435e1b52`. In this release build
    nothing writes the forbid counter: the only TLS `+0x10` writer found is profiler code. **The
    engine enforces nothing at runtime; the convention is the protection.**
- **Synced RNG:** it is switched to forbidden around every parallel section and in each frame
  (`FUN_1422345b0`, `CRandom::IsForbidden()`), and `"ChangeGameStateSeed == false ||
  ThreadIsMainThread()"`.
- **Some subsystems have their own `SRWLock`** (e.g. `DAT_14332f6a0+0xa18`, taken at the end of
  the daily and weekly update). Flags have none.

## Rules for the loader

1. **Game state is written only on the main thread, in serial code.** Flags in particular are
   written only from effects, the console and save loading.
2. **A hook into an engine function may run on any thread:**
   - a TBB worker, for anything reachable from the parallel sections (state, country and
     diplomacy updates, AI helpers, map and UI helpers);
   - a loading thread;
   - the main thread in the middle of a tick (nested frames).

   **Don't read game state in a hook.** Record what you saw with an atomic and defer the work.
3. **Safe point: the start of a top-level frame.** That is the outermost `DoFrame` on the main
   thread, before the frame's commands and tick. No tick, no parallel section and no nested
   frame is running there.
   - `lifecycle` hooks it (`kSigAppDoFrame`) and offers `OnMainThreadFrame` (every frame) and
     `RunOnMainThread` (one-shot, from any thread).
   - It runs in the main menu too.
4. **"Game entered" runs on the main thread, outside the tick** (an idler switch inside a
   frame). Its callbacks may read and write game state. `loader.log` now warns if one ever runs
   on another thread.
5. **Native effects run where effects run (main thread). Native triggers may be evaluated on any
   thread**, as vanilla triggers are: atomics only, no locks, no allocation.
6. **Lexer-ready:** no game exists yet. `loader.log` now records its thread and whether it is the
   main thread.

## Not proven statically (runtime checks added)

- **That no TBB task body executes script effects.** The calls are virtual, so the call graph is
  incomplete. Every on_action, event and command entry point found is serial.
  - **Runtime check:** the flag hooks count every flag set/clear (any container) made off the main
    thread. Each game entry logs it as "N flag change(s) off the main thread". 0 is expected.
- **Which threads call `GetNameKey`** (map labels may be built on workers): the `region_names`
  detour reads only atomics, so it is safe either way.
- **The old disappearing name:** no explanation has been confirmed. If the counter ever shows
  off-main flag writes, the cause is found. Either way, the deferred design no longer reads the
  container from a hook.

## Implementation: deferred rescan and main-thread queue (2026-10-14)

- **`kSigAppDoFrame`:** `DoFrame`'s prologue. Unique on vanilla 1.19.3 (`0x14222eeb0`), patched
  1.19.3 and 1.19.2 (`0x142219f70`).
- **`lifecycle`:**
  - **Frame detour:** at nesting depth 0 it records the main thread on the first frame, then runs
    `RunMainThreadWork`: the frame callbacks, then the queued tasks, which are swapped out under a
    mutex and run outside it, each under SEH.
  - **API:** `OnMainThreadFrame`, `RunOnMainThread`, `IsMainThread`, `MainThreadQueueAvailable`.
  - **The frame hook is optional:** without it, lifecycle callbacks still work and features
    needing the queue stay off.
  - **New log lines:** "Lifecycle: main thread N (first frame); lexer callbacks ran on ...",
    and the lexer thread id.
- **`region_names`:**
  - **Hooks:** `flag_set` / `flag_clear` only compare the container pointer (cached on game
    entry), compare the token prefix, and OR a reason bit into `g_dirty`. They also count calls
    made off the main thread.
  - **`OnFrame`:** exchanges `g_dirty`; if any bit is set it rescans and logs the combined reason
    (e.g. "hoi4_srn flag set, set_strategic_region_name").
  - **Native effects:** they still edit flags inline (main thread), then only mark dirty.
  - **Game entry still rescans synchronously,** which keeps the order rescan → plugin flag
    refresh → `on_game_loaded`.
  - **It now requires the main-thread queue.**
  - **`strategic_region_has_custom_name`** reflects the display, so right after a rename in the
    same effect block it answers for the old name. Its help text and the README say so.
- **README:** a new section, "Threads and hooks (for plugin authors)".

## In-game results (2026-10-14, vanilla 1.19.3, DLL `de1d0118…`)

- **Startup:** `app_do_frame` resolves to `+0x222eeb0` and its hook is queued.
- **Threads:** in three runs, "Lifecycle: main thread N (first frame); lexer callbacks ran on
  the main thread". So lexer-ready runs on the main thread too.
- **Renames:** A, B, reset and C each give one rescan line, written ~16 ms (one frame) after the
  effect's own line. The C decision gives a single `rescan (hoi4_srn flag set, hoi4_srn flag
  cleared): 1 change(s)`.
- **Entry summaries so far:** "0 flag change(s) off the main thread". These are only the first
  entries of fresh sessions, so they cover no play time yet. A count over weeks of play is
  still open.
- **No "game entered off the main thread" warning.**
- **The D3 step 8 and 9 attempts did not test what they should have:**
  - **Step 8:** `loader_config.ini` was unchanged (last modified 2026-10-13), so `game_loaded`
    stayed active. The log has `on_game_loaded fired: ROOT = FRA`, and the "immediate"
    migration was `on_game_loaded`'s.
  - **Step 9:** the loader was still loaded (`loader.log` 22:36, pid 46308; `version.dll` in the
    game folder untouched since 21:17). `game.log`: "save migrated to data version 2 by
    on_game_loaded".
  - **Steps 8 and 9 stay open.** One positive result: a version-1 save loaded with the loader was
    migrated by `on_game_loaded` right on load.

### D3 step 8 passes (2026-10-15, kill switch `game_loaded = 0`)

- **Startup:** `loader.log` has "Feature game_loaded (engine tier): DISABLED in
  loader_config.ini [features]", and the guard is served as "no".
- **Load:** `old_save` loads with no `on_game_loaded` and no event.
- **Status event:** data version 1, migration pending, guard no.
- **Fallback:** within the first week, "Save Migrated … by the on_weekly fallback". `game.log`
  has `save migrated to data version 2 by the on_weekly fallback` (loaded twice, both times the
  same).
- **Bonus threading data:** the second load in the same session summarised 17 flag changes made
  during play, with 0 off the main thread.

### D3 step 9 passes (2026-10-15, without the loader: `version.dll` renamed)

- **The loader did not run:** `loader.log` is unchanged since the 19:17 run.
- **`old_save` loads normally.** `game.log` has `save migrated to data version 2 by the
  on_weekly fallback` within the first week.
- **`error.log` has no line about `on_game_loaded`.** An `on_actions` key nobody fires is
  silently accepted, as D1 predicted.
- **Expected errors:** `Unknown trigger-type` / `Unknown effect-type` for the three native region
  commands (the test mod's decisions and scripted localisation) are the documented cost of
  native commands without the loader.
  - The user's verdict: acceptable. A mod that uses native commands requires the loader.
  - Mods that must run without it use the flag protocol (README).
- **Remaining `error.log` lines:** they are unrelated (mod descriptors, vanilla graphics, the
  lobby `remotefile.cpp` exception) and appear with the loader too.
- **Afterwards:** `version.dll` was restored, and the kill switch stays commented out.
- **D3 status:** steps 1-9 pass. Only step 10 (multiplayer and hot join) is open; it needs a
  second player.

---

# Part E: numeric data triggers

## E1: findings (vanilla 1.19.3 copy in Ghidra; addresses for orientation only)

Sources:
- `documentation/triggers_documentation.md` and `dynamic_variables_documentation.md`;
- the vanilla trigger classes found by RTTI (slot 22 = evaluate, slot 23 = value);
- the UI code that shows the same numbers.

Nothing below has been checked on 1.19.2 or the patched build yet; E2 does that per signature.

### Engine number format (all values)

- **Fixed point:** game values, script variables and value triggers use `CFixedPoint`, an
  **`int64` scaled by 100000** (1.0 = 100000).
  - Evidence: variables are converted with `/ 100000` (`FUN_1413a1cd0`); strength and
    organisation ratios are computed as `cur * 100000 / max` and clamped to `[0, 100000]`.
  - Integer arithmetic only, so it's **deterministic on every machine**. The loader computes in
    `int64` the same way and never uses floats.
- **Two vanilla numeric trigger bases:**
  - **`CIntTrigger`** (e.g. `CNumOfFactoriesTrigger`): slot 22 `FUN_1413a1cd0` compares
    `int GetValue(scope)` (slot 23) with an int or a variable (`/100000`). Operators: `>`
    (token 0x1d3), `<` (0x1d4), `=` (equality).
  - **`CValueTrigger`** (e.g. `CHasUnitStrengthTrigger`, `has_stability`): slot 22 `FUN_1413a2180`
    compares a `CFixedPoint` (slot 23 writes it). The right side is a script value or variable,
    evaluated with `FUN_140544c90`. Operators: `<` means less; **`>` and `=` both mean
    "greater than"**, which is vanilla's quirk. An optional 0..1 range check is slot 25.
- **Block triggers with operators exist in vanilla.** Examples: `divisions_in_state = { state = 5
  size > 3 }`, `army_manpower_in_state = { amount < var }`.
  - The block-item reader (slot 6, e.g. `FUN_140439320`) takes the operator from `parser+0x78`
    when it reads the compare key and stores it next to the value: `size` → op `+0x13c`, value
    `+0x138` via `FUN_1424c08d0` (int).
  - Variable-capable parameters are read into a script-value holder (its vtable `+0x18` reads)
    and evaluated with `FUN_140544c20` (int) or `FUN_140544c90` (fixed).

### Data locations found

| Data | Where | Cost per evaluation |
|---|---|---|
| Strategic region by id | `gamestate+0x2e0[id]`, count `+0x2ec` (already used by `region_names`) | O(1) |
| State by id | `gamestate+0x2c8[id]`, count `+0x2d4` | O(1) |
| Provinces of a state / strategic region | `obj+0x18` array, count `obj+0x24` (both, from `divisions_in_state` and `ships_in_area`) | O(provinces) |
| Land units (divisions) in a province | `province+0x110`, count `+0x11c`; owner tag at `unit+0x1d8` | O(units there) |
| Naval units (fleets/task forces) in a province | `province+0xf8`, count `+0x104`; ships of a unit via `FUN_140d6eeb0(unit)` → array, count `+0xc` | O(units × ships) |
| Combats in a province | `province+0x170`, count `+0x17c` (added by `FUN_140e797f0`) | O(combats there) |
| All combats | `CCombatManager` embedded at `gamestate+0x260`: array `+0x268`, count `+0x274`. `CLandCombat` (0xd8) and `CNavalCombat` (0x128) | O(all combats) |
| Country's divisions | `country+0x290`, count `+0x29c` (the `army.daily` loop) | O(divisions) |
| Division strength / organisation ratio | `CArmy` vtable slot 34 / 39 (`FUN_140c87920` / `FUN_140c82b60`): `cur*100000/max` via virtual getters `+0xf8/+0x108`, `+0x120/+0x130` | 4 virtual calls |
| At war (A, B) | `country(A)+0xf88` → `+8[tagIndex(B)]` → `+0x2e8` war, and `war+0x49 == 0` (from `has_war_with`, `FUN_1403d9c40`) | O(1) |
| Air power per country per region | `gamestate+0x690` air system → `+0x30[tagIndex]` country air → `+0xe0[region+0x60]`, entries of 0xa0 bytes; `[0]` = our power, `[1]` = enemy power (`FUN_140c54e30`) | O(1) |
| Naval dominance per region | `region+0xe8` → array `+0x40` of 0x50-byte entries (country tag at `+0`, dominance `int64` at `+0x20`), count `+0x4c`; friendly vs enemy summed in `FUN_141003f50` (`has_naval_control`) | O(countries present) |
| Military production | `country+0xf68` production object: `+0x2b8` military factories (fixed), `+0x2d8`/`+0x2f0` subtracted for "available" (`num_of_available_military_factories`) | O(1) |
| Production lines | `CMilitaryProductionLine` (RTTI): country `+0x20`, equipment `+0x88` (→ `+0x3f0` type, archetype `+0x4d8`). **Line list location and the factories field are not pinned down yet** (save tokens of the line writer `0x35c9`, `0x2e42`, ... to match in E2) | O(lines) |

**Air superiority as shown in the region panel** (`FUN_141880500`, `AIR_SUPERIORITY_BAR_TOOLTIP`):
- **Formula:** `pct = our * 100000 / (our + enemy)`, clamped to `0..100000`, and 0 if both are 0.
  The country is the player (`gamestate+0x520`).
- **It is simulation state:** the same array is read by land combat (`FUN_1412af7c0`), unit
  speed (`FUN_140c7fdb0`), raids and AI, so it must match on every machine. It's recomputed by
  the air manager (`airmanager.daily` and hourly).
- **Open question:** whether it's valid right after loading a save, before the first update.
  An E2 test point.

**Naval dominance** (1.13+ "Naval Dominance" with a "Control ratio" in the UI):
- **Formula:** `has_naval_control` sums friendly (allied) and enemy (at war) dominance in the
  region and compares the ratio with a define.
- **Proposed value:** `friendly / (friendly + enemy)` as 0..1, the "control ratio". E2 must check
  it against the UI.
- **Synced:** dominance moves day by day toward a target, so it's simulation state.

### Per wanted trigger

1. **Air superiority (region, country).**
   - Vanilla has nothing (no strategic-region triggers apart from `hidden_trigger` /
     `meta_trigger`).
   - Feasible: O(1) read; synced (used by combat).
2. **Naval dominance (sea region, country).**
   - Vanilla only has the yes/no `has_naval_control` / `has_enemy_naval_control`.
   - Feasible: a small loop; synced.
3. **Active battles (state / strategic region / involving country).**
   - Vanilla has nothing at country or region level (only combatant-scope triggers inside a
     battle).
   - State and region: loop over provinces, read the combat count. Cheap.
   - Country: loop over all combats and check both sides. That's O(combats), and the side
     layout is still to be found in E2.
   - Synced: combats are game state.
4. **Units present.** Vanilla partly covers this, own units only:
   - `divisions_in_state` (state; owner = scope country or the same tag group via
     `FUN_140bb52f0`);
   - `ships_in_area` (sea region, own);
   - `num_planes_stationed_in_regions` (regions, own planes by base);
   - country variables `num_armies_in_state@id`, `num_ships`, `num_deployed_planes`.

   What's missing: strategic-region divisions, and the `any` / `enemy` owner filters.
   - Divisions and ships: loop over the region's or state's provinces → units → owner check
     (plus the at-war check for `enemy`).
   - Planes: `own` loops over the country's air wings, as vanilla does. `enemy` / `any` would
     either read counts from the 0xa0-byte air entry (if it has them, which E2 checks; the UI
     shows "active fighters/bombers/CAS" per side) or fall back to looping over every
     country's wings, which is too expensive.
5. **Average strength / organisation.**
   - Vanilla has unit-scope `unit_strength` / `unit_organization` (0..1) only.
   - Country: loop over all divisions with 4 virtual calls each, 100-1000+ divisions. **The
     most expensive item**; it must not sit in AI-evaluated blocks.
   - State: divisions in the state's provinces. Cheap.
6. **Factories assigned to an equipment type or archetype.**
   - Vanilla has only totals (`num_of_military_factories`, `num_of_available_military_factories`).
   - Loop over the country's production lines (tens). Cheap once the list is found (E2).
   - Synced: production is game state.
7. **Resource deficit: drop it, vanilla has it.**
   - Variable `resource@steel` = "surplus resources in country".
   - The trigger `has_resources_in_country = { resource = steel amount < 0 }` (the balance).
   - The components are there too: `resource_produced@`, `resource_imported@`,
     `resource_exported@`, `resource_consumed@`.
   - So deficit = `-resource@X`: needed = consumed + exported, available = produced + imported.

### Variables (`set_variable = { x = ... }`)

- **How vanilla auto-generates them:** game variables are per-scope maps of
  `std::function<CFixedPoint(scope, int target, CScopedVariable*)>`
  (`NCountryGameVariableInitialization`, map at `DAT_143330ce0`). The "auto generated using the
  trigger with same name" ones are added by
  `CCountry/CState/CUnitLeader/CGameState::AddTriggerDynamicVariable(name, CValueTrigger* or
  CIntTrigger*, desc)` (`FUN_1406d2a20` etc.).
- **The dispatcher:** a value trigger's **vtable slot 18** (`FUN_1413aafb0` for `CValueTrigger`,
  `FUN_1413aae70` for `CIntTrigger`) calls the right `AddTriggerDynamicVariable` for each scope
  bit of its slot 14 mask.
- **Consequence:** a native numeric trigger cloned from these bases can register itself as a
  variable by calling its own slot 18 once, on a prototype object the loader keeps forever. Who
  calls slot 18 in vanilla, and when, is still open; E2 makes sure the loader's call comes after
  the engine's map exists.
- **Limits:**
  - **Scopes:** variables exist for country, state, unit leader and global scope only, not
    strategic regions. So `air_superiority` can't be a region-scope variable.
  - **Parameters:** a trigger-backed variable has no arguments, so it works only when every
    parameter defaults from scope, e.g. `avg_division_strength` in country scope.
- **Variables with a target** (`x = air_superiority@42`) would need the loader's own
  `std::function` in the engine's map. That relies on MSVC STL binary compatibility and the
  engine's map insert, so it's riskier. Proposed for later, not v1.

### API proposal

1. **Numeric value triggers** (`register_native_numeric_trigger`):
   - **Value:** `int64 value(const Args&, const Scope&)` in fixed point. The kind is `Int` (a
     count) or `Ratio` (0..1).
   - **Templates:** the int kind clones a `CIntTrigger` (`CNumOfFactoriesTrigger`); the ratio
     kind a `CValueTrigger`, with the 0..1 range check (slot 25) kept.
   - **Value form:** `air_superiority > 0.6` when the scope supplies every parameter. Operators,
     variables on the right (`> var`) and **variable registration (slot 18) come from the
     engine**.
   - **Ratios are 0..1 like vanilla** (`has_stability < 0.6`). The tooltip shows percent.
2. **Block form with a comparison:**
   - Example: `air_superiority = { region = 42 value > 0.6 }`.
   - **Reading:** the loader's block-item reader stores `parser+0x78` (the operator) when it
     reads the designated compare key (`value`, or `count` for counts). The right side goes
     into an engine script-value holder (variable-capable, like vanilla's `state =` holder);
     E2 finds its constructor and vtable in a vanilla block trigger.
   - **Evaluation:** compute the value, resolve the right side with `FUN_140544c20/90`, compare.
     The compare semantics are copied from the base (int: `=` `<` `>`; ratio: `<` and `>`,
     with `=` meaning `>` as in vanilla, documented).
3. **Parameter defaults from scope** (general; also for existing commands):
   - `ParamSpec` gains `from_scope`: `StrategicRegion`, `State`, `Country`, or `RootCountry`.
   - **Parse time:** a parameter that is missing but has a scope default isn't an error.
   - **Evaluate time:** a missing value comes from the current scope (or ROOT for
     `RootCountry`). If the scope doesn't provide it, the trigger is false and one line is
     logged (no allocation on the hot path: a per-command flag).
   - **Existing command:** `set_strategic_region_name = { name = KEY }` inside a strategic
     region scope.
   - **Which country:** in a strategic-region scope, data triggers use **ROOT** if it is a
     country, else the scope's country if any.
4. **Tooltips:**
   - Ratio, e.g. "Air superiority over Bay of Biscay (42) above 60% (currently 64%)".
   - Count, e.g. "Battles in Normandy (state 15): more than 2 (currently 3)".
   - Built in the tooltip callback (allocation is fine there). The current value comes from
     the same evaluator.

### Thread rule (refining README "Threads and hooks")

The current rule is "native triggers may only read atomics". Data triggers must read game state.

**Proposed rule:**
- `evaluate` may **read** game state. It's safe because all game-state writes happen serially on
  the main thread (NOTES "Threading model"), and the engine's own triggers read the same data
  from AI worker threads.
- **No** writes, no caching across calls, no allocation, no logging (except one flag-guarded
  line), and no locks.
- **Never call an engine function that:**
  - asserts or needs the main thread (synced RNG `FUN_142233fa0`, anything in the tick,
    `DoFrame`);
  - allocates;
  - logs on its normal path. Several vanilla getters log on bad input, e.g. "Region ID had an
    unexpected value". Validate ids before calling them.
- **Loops must be bounded** by the data, with no recursion over scopes.

**Getters found safe so far** (pure reads, no asserts in release, no allocation):
- `FUN_140bb48f0` (tag → country), `FUN_140bb5490` (tag → index), `FUN_140bb52f0` (tag
  group compare);
- `FUN_1406f9600` (country air), `FUN_140c54e30` (air region entry);
- `FUN_141003f50` (naval control: reads, plus a define via `FUN_14143c720`, to be checked);
- `FUN_140d6eeb0` (ships of a naval unit);
- `CArmy` slots 34/39 and the getters they call (to be checked in E2 for allocation);
- `FUN_140c7e7c0` / `FUN_140b9f5c0` (unit → template → type);
- direct reads of the arrays in the table above.

The release asserts are compiled out, so "asserts" here means behaviour, not the check.

### Feature split (one guard per group; a broken signature disables only its group)

| Feature | Triggers | Depends on |
|---|---|---|
| `region_control` | `air_superiority`, `naval_dominance` | air system, air entry, region naval object, at-war / ally checks |
| `military_counts` | `battles_count`, `divisions_count`, `ships_count`, `planes_count` | state/region province lists, province unit/combat arrays, combat manager, at-war check |
| `army_condition` | `avg_division_strength`, `avg_division_organisation` | country division list, `CArmy` slots 34/39 (RTTI) |
| `production_data` | `factories_assigned` | production lines (to be found) |

Each feature requires `lifecycle` (native registration) and the new numeric API. Before it
reports "supported", each one also runs a self-check: array sizes against the region and state
counts, values in range.
