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
  build in use. So it was not added to `loader_config.ini` at first.
- **2026-11-12:** both 1.19.2 hashes (the table below) are now listed in `loader_config.ini`, at the
  owner's request ("nothing major changed"). The signatures and code shapes added since October
  (create_army's 1.19.4.2 shapes, add_to_army, railways, time_control, province buildings) were
  never checked on 1.19.2 or 1.19.3; on those builds each feature whose patterns do not match
  stays off (fail safe).

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
| **1.19.4.2** | **vanilla (`v1.19.4.2.2da5`), Steam update of 2026-11-05** | `bccc62909b6a1ec2c1d5a46ef18608e4d0c7cdc89ad341b7402a23c40f7a8037` |

**Vanilla 1.19.3 verified (2026-10-12 23:01):**
- After Steam's "Verify integrity", `hoi4.exe` is `v1.19.3.0.c01a`, the same size as the
  patched 1.19.3.
- It differs from the patched 1.19.3 in **exactly one byte**: file offset `0x16e60a`
  (VA `0x14016f20a`), `0x85` vanilla vs `0x31` patched. That is the patch site identified above.
- The patch pattern `E8 ?? ?? ?? ?? 31 C0 0F 94 C3` has 0 matches in it.
- Added to `loader_config.ini` as `1.19.3_vanilla`; the patched hash stays as
  `1.19.3_checksum_patched`.

**1.19.4.2 added (2026-11-10):**
- Steam updated the game to `v1.19.4.2.2da5` ("Operation Postern") on 2026-11-05. The copy has
  no checksum patch (`E8 ?? ?? ?? ?? 31 C0 0F 94 C3`: 0 matches), so it is vanilla.
- `tools/check_signatures.py`: all 46 signatures and anchors match exactly once.
- `tools/check_code_shapes.py`: everything from before create_army passed unchanged. Three
  create_army shapes did not, and were adapted (NOTES Part G1):
  - `kCodeBorderWarSetLeader`: same instructions, other registers (army in rbx, vtable in rax);
    the register bytes are now wildcards.
  - `kCodeCharacterOwnerCheck`: `mov rdi,rax` instead of `mov rsi,rax`; wildcard.
  - `CSetArmyLeaderCommand` slot 9 (can execute) was restructured: new checks first, then the
    same two `can_change_leader` calls (the target army, then the general's current army via
    `+0x1048`). New `kCodeSetLeaderCheck1194`, searched inside the function when the old shape
    does not match at its start; its led-group field must equal is_leading_army's.
  - `set_leader` (`0x140c4b460`) and `can_change_leader` (`0x140c41310`) themselves are
    unchanged, and so are all fields read: army leader +0x88, border war +0x1b0, deploying
    +0x199/+0x19a, withdrawing +0x19b, characters +0x6a8, owner +0x68.
- After that: 31 of 31 shapes ok. The wildcarded shapes still accept the old bytes and the old
  check shape is tried first, so 1.19.3 and 1.19.2 resolve as before; they could not be
  re-run there (the copies are gone).
- Added to `loader_config.ini` as `1.19.4.2_vanilla`. A checksum-patched 1.19.4.2 is not listed
  (no copy); the loader prints its line in `loader.log` if one is used.

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
    the docs (now MODDING.md) say so.
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
  `yes`, because it is served before the session type is known; MODDING.md documents that the
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
   "supported", not "fired". MODDING.md recommends idempotent migrations run from `on_game_loaded`
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
    same effect block it answers for the old name. Its help text and MODDING.md say so.
- **Docs:** a new section, "Threads and hooks (for plugin authors)", now in DEVELOPING.md.

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
  - Mods that must run without it use the flag protocol (MODDING.md).
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

### Thread rule (refining DEVELOPING.md "Threads and hooks")

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

## E1 decisions (2026-10-15)

1. **Ratios are 0..1** like vanilla; tooltips show percent.
2. **Operators copy vanilla:** for ratios `=` behaves like `>`; for counts `=` is equality. This
   is documented in MODDING.md and the generated docs.
3. **The country-wide averages ship anyway.** Their cost is measured (time per evaluation and per
   in-game day at speed 5, with the test mod using them in `ai_will_do`), then documented from
   the numbers. No caching across calls.
4. **Planes take `owner = own` only** if that is all the region data supports. The `owner`
   parameter exists now and other values are refused with a parse error, so they can be added
   later without a syntax change.
5. **API, thread rule and feature split:** approved.
6. **Two documentation additions:** "missing scope value is false, true under NOT", both next to
   the guard rules and in each docs entry. Resource deficits stay with vanilla (MODDING.md).

## E2: implementation (2026-10-15)

### API (`native_script.*`, commit `2e845bf`)

- **`register_native_numeric_trigger`:**
  - **Kinds:** `Count` (int) and `Ratio` (fixed 1e5).
  - **Value form:** `name > x`, allowed when every parameter can come from scope.
  - **Block form:** `name = { ... <compareKey> > x }`.
  - **Right side:** a number or a variable.
  - **Tooltip:** "<subject> above 60% (currently 64%)".
- **Engine binding:**
  - **Clones:**
    - `Count` clones `CNumOfFactoriesTrigger` (`CIntTrigger`; 0x138 bytes).
    - `Ratio` clones `CHasStabilityTrigger` (`CValueTrigger`; 0x130 bytes, range-checked 0..1).
  - **Objects:** they are built by the template's own operator new and constructor (decoded from
    `CTriggerEntry<T>::Create`), in a block 0x148 bytes long. The record pointer sits at `+0x138`.
    The engine allocator is plain `HeapAlloc`/`HeapFree` and ignores the size on free.
  - **Our slots:**
    - 5: read clause. A block value (type 3 at `parser+0xc0`) goes to the base block loop;
      otherwise the operator comes from `parser+0x78` and the value is read by the template's
      slot 4.
    - 6: block item. The compare key stores the operator at `+0x58` and reads the value into the
      template's `CScopedVariable` (`+0x60` ratio, `+0x68` count with flag `+0x70`) through its
      vtable slot 3.
    - 7 validate, 14 scopes, 15 targets, 21 tooltip.
    - 22 evaluate: our value, the right side through `ScriptValueEval`, our compare with the
      engine's semantics.
    - 23 GetValue.
  - **Game variable:** the template's slot 18 is called once on a prototype when every
    parameter comes from scope and the scopes include country or state.
- **Parameters:**
  - `ParamSpec::fromScope` (strategic region, state, country, ROOT country);
  - `Country` (tag or THIS/ROOT/FROM/PREV);
  - `Choice`;
  - `Token` (interned at parse time).
- **Arguments are resolved per use** into a stack array (8 parameters at most), so there is no
  allocation on the hot path.
- **Timing:** optional QueryPerformanceCounter counters per numeric trigger
  (`[diagnostics] trigger_timing = 1`, summary every 10 in-game days, date from `kSigGameDate`).
- **Code shapes:** `CODE(...)` in `signatures.h` are patterns checked with `SigMatchAt` at
  RTTI-found addresses. `tools/check_code_shapes.py` checks them offline.
- **`region_names`:** `region` may be left out inside a strategic region scope.

### Features

| Feature | Commit | Engine data (all unique / ok on vanilla 1.19.3, patched 1.19.3, 1.19.2) |
|---|---|---|
| `region_control` | `d08675f` | air bar call site (`TagToCountry`, `CountryAir`, `AirRegionEntry`), `has_naval_control` (region `+0xe8`), naval sums loop (entries, friendly check, diplomacy `+0xf88`, war `+0x2e8`) |
| `military_counts` | `4fb296f` | state array (`+0x2c8`/`+0x2d4`), `divisions_in_state` unit loop (provinces `+0x18`/`+0x24`, land units `+0x110`/`+0x11c`, owner `+0x1d8`), `ships_in_area` (naval units `+0xf8`/`+0x104`, ships getter), province combats (`+0x170`/`+0x17c`), unit combats (`+0x1a8`/`+0x1b4`), planes stationed (air wings `+0x58`/`+0x64`, wing region, plane count), "army.daily" divisions (`+0x290`/`+0x29c`) |
| `army_condition` | `7315370` | `CArmy` vtable (RTTI) slots 34/39 = strength/organisation ratio (code shape); only objects with that vtable count |
| `production_data` | `ef8e129` | equipment lookup by token (`has_equipment`'s resolver), "production.daily_serial" lines (`+0x58`/`+0x64`), factories `+0x48` (the "non productive line" check), country production `+0xf68` (`num_of_available_military_factories`), line equipment `+0x88` → type `+0x3f0` → archetype `+0x4d8` |

**Semantics chosen:**
- **`battles_count`:** combats listed in the provinces of the state or region (land and naval),
  or the distinct combats of a country's divisions (`involving`; land battles of its divisions).
- **`divisions_count` / `ships_count` owner `own`:** same country by the engine's tag-index test
  (as vanilla `divisions_in_state`).
- **Owner `enemy`:** at war (a war not ended).
- **`planes_count`:** a country's planes **stationed** (air base) in the region, as vanilla
  `num_planes_stationed_in_regions` counts them. `owner = own` only: the per-region air entry
  holds power, not plane counts per country (not proven), and looping over every country's
  wings is too expensive.
- **The averages:** the mean of each division's ratio.
- **`factories_assigned`:** military lines' factories plus naval lines' dockyards (naval lines
  only if their slot 11 reads the same equipment field). An equipment type or archetype matches
  the line's type or its archetype.
- **Self-checks** on every game entry (main thread, SEH) read the player's data. A fault
  deactivates the feature, and its triggers then return false.

### Tests

- **Core (`native_script_test`, 113 checks):**
  - choice and country parsing;
  - scope defaults (region from scope, ROOT country, FROM, fallback to the country scope);
  - the missing default (false, logged once);
  - compare semantics (ratio `=` means `>`, count `=` equality, strict `<`/`>`);
  - formatting, tooltips (negated, unknown right side, unavailable value);
  - value-form refusal with required parameters, a missing compare key, timing counters;
  - docs text.
- **Not testable out of game:** operator parsing itself (engine parser). It's covered by the
  in-game plan.

### In-game test plan (E2)

**Setup:**
- `version.dll` and `loader_config.ini` with `[diagnostics] trigger_timing = 1`; the test mod
  enabled.
- Places: state 16 = Ile de France, regions 19 Northern France, 18 Channel, 42 Bay of Biscay.

1. **Startup log:** signatures `air_superiority_ui`, `naval_control_eval`, `naval_sums`,
   `state_array`, `state_land_units`, `ships_in_area`, `province_add_combat`, `unit_add_combat`,
   `planes_stationed`, `equipment_lookup`, `production_line_factories`, `game_date`; anchors
   `army.daily`, `production.daily_serial`.
   - "Numeric triggers: count template 312 bytes, ratio template 304 bytes ...".
   - Three "Data triggers: ..." layout lines.
   - The four features active; the numeric triggers "registered", three of them "also a game
     variable"; "Diagnostics: numeric trigger timing ON".
2. **`loader_script_docs.md`** lists the 9 new triggers with usage, the value semantics and the
   NOT caveat.
3. **New game as France (1936).** On game entry, four self-check lines pass: region_control,
   military_counts, army_condition, production_data.
4. **Values against the game UI** (hover the status decisions' requirements):
   - **air_superiority:** put fighters on an air superiority mission over Northern France. The
     region panel's air superiority bar and `air_superiority = { region = 19 ... }` must agree
     (at peace: 100% with own planes active, 0% without).
   - **naval_dominance:** a fleet patrolling the Bay of Biscay; compare with the region's naval
     dominance "Control ratio".
   - **divisions_count:** the division counters in Ile de France (state 16) and Northern France
     (19) on the map.
   - **ships_count:** ships of the fleets in the Bay of Biscay / Channel.
   - **planes_count:** the planes based in Northern France (air base tooltips).
   - **avg_division_strength / avg_division_organisation:** the army overview / division bars
     (whole army, and Ile de France).
   - **factories_assigned:** the production screen (all lines; infantry equipment lines; light
     hull lines in dockyards). `no_such_equipment` always shows "currently unavailable".
5. **At war** (1939 bookmark as France after the declaration on Germany, or any war):
   - owner = enemy counts on the front;
   - battles_count (state, region, involving ROOT) while attacking;
   - air superiority and naval dominance below 100% where the enemy is active.
6. **Comparisons:**
   - "> 50%" and "< 50%" flip as air superiority changes;
   - "= 50%" is available exactly when "> 50%" is;
   - "Divisions = 0" means equal;
   - the threshold decisions (LOW/HIGH) flip the variable-based decisions.
7. **Caveats:**
   - "NOT air superiority without a region" is available, and `loader.log` notes the missing
     region once;
   - "Planes with a reserved owner": `loader.log` says `'owner' expects one of own, got 'enemy'`
     once, and the decision is never available.
8. **Variables:** "Data triggers as variables" opens event 5 with the same numbers as the
   status tooltips and the UI.
9. **Cost:** run 60+ in-game days at speed 5 (the AI probe decision makes every AI country
   evaluate the triggers).
   - Send the "Trigger timing" lines (per trigger: evaluations, ns each, ms per day; the total).
   - Note whether speed 5 feels slower than with `[features]` switching the four features off.
10. **Without the loader:** the data decisions are hidden; `error.log` has "Unknown
    trigger" for the new triggers (the documented cost).

### In-game results (2026-10-15, vanilla 1.19.3)

- **Step 1, first run (DLL `74b750e7`):**
  - **Three features failed to start:** military_counts, army_condition and production_data
    ("data 0"). `HaveCountries()` required a running game.
  - **The lexer-ready callback crashed** (access violation) after the two region effects. The
    docs file was not rewritten and the guards said "read BEFORE native registration ran".
  - **Fix (`b5c34c2`):** HaveCountries no longer needs a game; every command now binds under its
    own SEH guard and a crash logs module+offset.
  - **Root cause of the crash:** not identified. It did not recur in the next two starts.
- **Step 1, second run (DLL `f8f05a99`):**
  - **Passes:**
    - every signature and anchor resolves;
    - the four data features are active and all 12 commands register;
    - avg_division_strength, avg_division_organisation and factories_assigned are also game
      variables;
    - all six guards are yes;
    - the docs list the 9 data triggers (both forms, value semantics, the NOT caveat), and the
      region commands show `region` as "optional: from the strategic region scope".
  - **Found:** the `=` operator token came from `Intern("=")`, which created a new dynamic token
    (`0x4e60`) the parser never uses, so `= x` comparisons would have been refused.
- **Fix:** `=` is looked up by name among the static tokens, and `>` / `<` (from the template
  code) are checked by name too. **Third run: `'>' 0x1d3, '<' 0x1d4, '=' 0x1`, 12 of 12
  registered.** The game was launched directly by Claude for steps 1-2 (main menu only).
- **Steps 3-4, first pass (DLL `5fa51532`, new FRA 1936 game, user):**
  - **Correct:** region control (0% in peacetime is vanilla; > 50% unlocks the decision once
    fighters win the region), divisions (4 in state 16; `= 0` and `< 3` false), ships, planes
    (348 in Northern France), organisation 100%, the `owner = enemy` parse error.
  - **Strength was wrong:** 100% where the UI shows 50%. CArmy slot 34 is the manpower ratio
    only; the UI ("Strength", CURRENT_STRENGTH / UNIT_AVERAGE_STR) calls slot 35 = slot 34 x
    equipment ratio (+0x468). Now reads slot 35 (kCodeArmyFightingStrength). **Second pass: 50%
    in state 16, 59.9% army-wide.**
  - **Production was wrong:** 21 against the UI's 6 military + 11 dockyards in use. The
    production line dump showed equipment matching is fine (variant -> type -> archetype resolve
    to the right names), but line+0x48 is the amount still to build (-1 = unlimited, so military
    lines were skipped; the submarine lines' 3/2/2 orders made up the extra). The assigned
    factories are line+0x18 + line+0x1c, written by Production::SetLineFactories
    (0x140e611b0, kSigLineAssignFactories); the rest of the requested count (+0xe8) is queued.
    Sums per kind: production+0x2d8 (military) and +0x338 (dockyards).
  - **Test mod:** the HOI4 loc parser drops "{" plus the next two characters; descriptions now use
    parentheses. fighter_equipment no longer exists in 1.19 (it correctly showed "unavailable");
    the test now uses small_plane_airframe.
- **Steps 3-9, second pass (DLL `67f8e6b8`, new FRA 1936 game, user):**
  - **Production:** 17 in total (6 military + 11 dockyards, as the Factories panel shows),
    infantry_equipment / infantry_equipment_1 2 each, small_plane_airframe 0, ship_hull_light 4,
    no_such_equipment unavailable. The line dump matches the history file (the submarine lines
    beyond the 11 dockyards get 0).
  - **Comparisons and variables:** LOW and HIGH flip decisions 12 and 17 as predicted. Decisions 7
    and 15 stay as predicted, and the tooltips show the variable's current value. Decision 18's
    event shows 59.9% / 100% / 17.
  - **Caveats:** the "NOT without a region" decision is available and logged once. The
    `owner = enemy` parse error is logged once at load and the decision is never available.
  - **Cost (step 9), 60 in-game days at speed 5, AI probe on:** steady windows of 10 days take
    6.2-7.1 s of real time. All numeric triggers together cost 1.73-1.78 ms per in-game day, about
    0.3% of the day.
    - Per evaluation: air_superiority 310 ns, naval_dominance 200 ns, battles_count 430 ns,
      divisions_count 770-830 ns, avg_division_strength 1.48-1.54 µs (FRA, whole army),
      avg_division_organisation 770-860 ns, factories_assigned 715-745 ns.
    - About 580 evaluations per day of each probe trigger.
    - The first window (9.9 ms/day, 56k air_superiority evaluations) was the paused testing phase:
      hovered tooltips re-evaluate every frame. That is not a gameplay cost.
    - **Decision: no caching.** MODDING.md "Cost" now gives these numbers.
  - **Open:**
    - step 5 (at war: enemy counts, battles, air/naval below 100%);
    - step 10 (without the loader);
    - D3 step 10 (multiplayer).

### E3. Battles in an area, unit filters, literal tags (2026-10-16)

**User report (step 5, at war):** battles in France counted. A battle located in Northern France
whose attackers stand in Western Germany did not count for Western Germany.

- **battles_count in an area:**
  - Each battle counts once if it is located in the area's provinces (province +0x170) or a
    division standing in the area fights in it (unit combat list +0x1a8).
  - Deduplication uses a fixed 1024-entry stack buffer; there is no allocation.
  - `location = only` gives the old behaviour.
  - `involving` keeps counting per division.
- **Unit filters** (divisions_count, ships_count): `owner = <country>/any`, `enemy_of = <country>`
  and `allied_with = <country>`, combined with AND.
  - `allied_with` uses the IsFriendly relation test (FUN_140d25410, from kSigNavalSums), the one
    naval control uses. The country itself is excluded.
  - Without IsFriendly the parameter is reserved.
- **planes_count:** `owner = <country>` (default ROOT, else the country scope). `any`, `enemy_of`
  and `allied_with` give clear parse errors. The new `reserved` ParamSpec flag gives "'enemy_of' is
  not supported by planes_count yet".
- **Literal tags never worked.** EngineBackend did not implement CountryTag, so `GER` was refused
  at parse time. Earlier tests used only ROOT. Now kSigCountryTagFromText
  (CCountryTag-from-text, FUN_140bb4640; unique on all 3 builds) gives:
  - the static tag database global;
  - its lookup FUN_14071bc80(db, const std::string*), a hash-map read that returns -1 if unknown
    and works while scripts are parsed.

  The military_counts self-check round-trips the player's tag (name -> tag -> same country).
- **Docs and params:** a Country param with choices "any" accepts `any` (kCountryAny stays
  unresolved). Usage lines skip reserved params.

**In-game test plan (DLL `42a6c3f6`; the test mod is synced):**
1. **Startup:**
   - `country_tag_from_text` resolves and 12 of 12 commands register.
   - `loader.log` has exactly two parse errors from the test mod: `'enemy_of' is not supported
     by planes_count yet` and `'owner' does not accept any in planes_count; …`.
   - No error for GER / ENG.
2. **Any new game:** the military_counts self-check line ends with "tag text FRA -> same
   country".
3. **1939 bookmark as France, after 3 September**, with an attack running from the Saar border
   into Germany (or a German attack into France). "Battles: located vs. fought from":
   - Western Germany (7) default >= Western Germany located only.
   - A battle located in Northern France whose German attackers stand in Western Germany counts
     in both region 7 (default) and region 19, but not in region 7 with `location = only`.
   - "Battles involving GER" > 0.
4. **"Unit filters":**
   - divisions of GER in region 7 > 0 (tooltip "Divisions of GER in Western Germany (7)");
   - at war with FRA: the same number, as long as only Germany has troops there;
   - GER and at war with FRA: the same;
   - divisions allied with FRA in Northern France: the British BEF once it lands (0 before);
   - all divisions in Northern France: own + allied + enemy;
   - British ships in the North Sea (16): > 0;
   - allied ships in the Channel (18): British ships there;
   - German planes based in Western Germany: compare with the air wings in the region view.
5. **"State scope form":** with France's divisions in Ile de France, all three lines are met
   without a `state` parameter. The tooltip shows the state block.
6. **Old decisions with the new syntax:** "Military counts: status" (divisions of FRA / at war
   with FRA in Northern France) and "French divisions in Ile de France > count variable" behave
   as before.
7. **Parse-error decisions:** "Planes with a reserved parameter" and "Planes with owner = any" are
   never available.

**In-game results (2026-10-17, vanilla 1.19.3, DLL `42a6c3f6`, 1939 France): all steps pass.**
- **Startup:**
  - 12 of 12 commands registered.
  - `loader.log` has exactly the two planes_count parse errors.
  - `error.log` has only the two matching InitPostRead lines (test data decisions, lines 141 and
    201).
  - No error for GER / ENG.
- **Game entry:** every self-check passed: "tag text FRA -> same country"; FRA 115 divisions at
  63.1% average strength; 16 factories/dockyards on lines.
- **Peacetime values:**
  - air/naval 0% (no missions);
  - 4 divisions in Ile de France, 76 French divisions in Northern France;
  - 537 French planes in Northern France, 868 German planes in Western Germany;
  - 16 lines, 2 on infantry_equipment (archetype = type 1);
  - 6 dockyards on ship_hull_light, 0 small airframes;
  - no_such_equipment unavailable.
- **At war: France attacks from Northern France into Western Germany (the mirror of step 3).**
  - **Battles:**
    - Western Germany 1 (default) / 1 (located);
    - Northern France 1 (default, fought from) / 0 (located);
    - FRA involved 1, GER involved 20 (Poland).
  - **Unit filters:**
    - GER = at war with FRA = both = 18 in Western Germany;
    - allied with FRA in Northern France 0 (no BEF yet);
    - all divisions in Northern France 90 = French 90;
    - ENG ships in the North Sea 17;
    - allied ships in the Channel 153, FRA's own 0, so the country itself is excluded as
      documented.
  - **State scope form:** greyed once the 4 divisions left Ile de France, as predicted.
- **Timing (one 10-day window, includes hovering, so not a gameplay figure):**

  | Trigger | ns per evaluation |
  |---|---|
  | air_superiority | 600 |
  | naval_dominance | 370 |
  | battles_count | 1402 |
  | divisions_count | 1000 |
  | ships_count | 390 |
  | planes_count | 2184 |
  | avg_division_strength | 5206 |
  | avg_division_organisation | 1489 |
  | factories_assigned | 1344 |

  - All numeric triggers: 14.4 ms per in-game day.
- **Not covered by this report:**
  - the threshold switch (LOW / HIGH) flipping the variable decisions;
  - the "Data triggers as variables" event values.

  Both are unchanged code paths from E2.

## Part F: strategic region scope (investigation)

Investigation only, nothing implemented. Addresses are from vanilla 1.19.3 (`hoi4_copy.exe`). The
scope-mask scan and the setter signature were also checked on patched 1.19.3 and on 1.19.2.

### F1. Does vanilla script ever enter a strategic region scope?

**No.** STRATEGIC_REGION is a real scope type (mask `0x40`; the mask→name builder
`FUN_141480d30` prints "STRATEGIC_REGION" for bit 0x40). However, no script command creates
one.

- **Who declares 0x40.** A scan of the scope-mask slot of every registered command (439
  `CEffectEntry<>` and 519 `CTriggerEntry<>`; effect slot 19, trigger slot 14; all constant
  `mov eax,imm; ret`) finds only:
  - `add_region_efficiency` (`CAddRegionEfficiency`, mask exactly 0x40);
  - `hidden_trigger`, `meta_trigger` (0x3ffe = every scope);
  - the unregistered generic classes `CContextTrigger` and `CScriptedEffect` (0x3ffe).

  The result is the same on 1.19.3 vanilla, patched 1.19.3 and 1.19.2. The game's own
  `documentation/*_documentation.md` agrees: the "STRATEGIC_REGION" sections list only these
  commands.
- **Any-scope commands.** 74 triggers and 79 effects have mask 0 ("any"): variables, global flags,
  event targets, `if`/`else`, the any_/all_ country and state iterators, and so on. Those work
  inside a region scope too. So do our own region-aware data triggers, which already default
  `region` from the scope (`Scope::strategicRegionId`, CEventScope `+0x48`).
- **No vanilla script uses it.** `add_region_efficiency` appears in no file under `common/`,
  `events/` or `history/`, and nothing in the scripts enters a region scope.
- **Closest vanilla tool.** `any_state_in = { strategic_region = 42 ... }` iterates the *states*
  of a region in state scope.
- **Engine side.** `CEventScope::SetStrategicRegion(scope, CStrategicRegion*, bool clear)` =
  `FUN_14053b670`. It calls the clear routine `FUN_14053bd00` when `clear` is set, then writes
  `+0x48`. Its 34 call sites are all C++:
  - the event-target resolver `FUN_14147de40`, but only when it copies an existing scope
    (THIS/ROOT/FROM/PREV or a saved event target, `FUN_1401dbc30`);
  - saved-scope loading (`FUN_140539530`, save key token 0x2eee: region id → gamestate
    `+0x2e0[id]`) and saving (`FUN_14053a750`: region → saved `+0x20`);
  - unit leader abilities (`CTriggerAbilityCommand` slots 9 and 10);
  - command power UI (`FUN_1406ee8f0`) and one large UI routine (`FUN_141d1c2a0`);
  - **12 script collection template instantiations** (strings "Collection operator %s doesn't
    support … strategic_region"). Collections support region elements internally, but no
    collection input or operator produces them. The documented inputs are `game:all_countries`,
    `all_possible_countries`, `all_states`, `scope`, `constant` and named collections.

  So a region scope, once created, survives `PREV`/`ROOT`/`FROM`, saved event targets and
  savegames. Script just has no way to create one.

### F2. Prefix scopes (`mio:`, `sp:`): can we add `region:42`?

The prefixes are **hard-coded if-chains, not a table.** The relevant code:

| Where | What |
|---|---|
| `FUN_140546530` (scopedvariable.cpp, 9 KB) | `CScopedVariable` parse (value positions): `strncmp`/`StartsWith` chain `var:`, `temp_var:`, `token:`, `mtth:`, `mio:`, `sp:`, `constant:`, `event_target`, `global_event_target` |
| `FUN_14147db70` → `FUN_14147fe10` (eventtarget.cpp, 2.5 KB) | scope-key parse (`GER = {`, `16 = {`, `var:x = {`, `event_target:x`, dotted chains `a.b`) into a 0x148-byte target link (`+0x0` key token, `+0x48` prefix token, `+0x4c` value token, `+0x50` next link) |
| `FUN_1414807f0` | prefix validation: `FUN_1424be210` splits `prefix:value`, then token 0x4d37 (`mio`) / 0x4d38 (`sp`) are looked up in their databases ("does not match any MIO in database") |
| `FUN_14147fb80` | target → resulting scope mask (hard-coded token cases: 0x4d37→0x200, 0x4d38→0x1000, 0x2996/0x2a7d→0x2000, `capital_scope` 0x29fe→2, …) |
| `FUN_14147de40` (7.5 KB) | target → CEventScope at runtime (same hard-coded cases; then `SetMio`/`SetSpecialProject`/`SetFaction`/… setters) |
| `FUN_1413a5ad0` (7 KB) | the tooltip header for `X = { }` (TRIGGER_OWNER, "State ", TRIGGER_CAPITAL, …); **has no region case** |

`region:42` would need **4–5 detours** on large engine functions (parse validation, mask, resolve,
tooltip header, and the CScopedVariable parse if it should also work in value positions). Each
detour would need a byte signature and would handle our prefix token before falling through to
the original.

- **Feasibility:** feasible.
- **Update risk:** these functions grow every time Paradox adds a scope type (sp: and factions
  were recent additions). They run on worker threads during AI evaluation.
- **Data structure:** there is no registry to insert into.

### F3. Scope changers to clone

All generic `X = { … }` scope changes are **`CContextTrigger` / `CContextEffect`**. The trigger
factory `FUN_140550030` (and the effect factory `FUN_140540ee0`) builds one when a block key is
not a registered command.

| | `CContextTrigger` (0x60, vt 0x1429b84c0) | `CContextEffect` (0x68, vt 0x1429b6608) |
|---|---|---|
| base | `CAndTrigger` (children `CPdxArray<CTrigger*>` at +0x8, count +0x14) | `CEffect` list (same layout) |
| target | `+0x58` → 0x148-byte target link | `+0x58`, flag `+0x60` |
| scope switch | slot 3 `FUN_1413a41c0`: `FUN_14147de40(target, &stackScope, cur)`; if valid (`+0xac`) → `FUN_14054d0b0(this, &stackScope)` (base `CTrigger::Evaluate`: scope check "Invalid Scope, supported:", then slot 22 = `CAndTrigger::Evaluate` `FUN_14054ab30`); dtor `FUN_140535820` | slot 12 `FUN_141393830`: same, then base `FUN_14053d9e0` (→ slot 13 = children loop `FUN_14053e130`) |
| tooltip | slot 11 `FUN_1413a2730`: resolve scope, header `FUN_1413a5ad0`, children `FUN_14054c580(this,out,newScope,0,indent,depth+1,…)` evaluated **in the new scope** (✓/✗ per child), joined with ":" | slots 7–10 |
| scope mask | slot 14 → 0x3ffe | — |

The iterator templates are generic and **driven by vtable slots**:

- **`any_state` = `CAnyStateTrigger`**
  - `CTargetTrigger<CAnyStateTrigger, CState, 2, 0>`; 0x148 bytes, vt 0x1427b02c8; factory
    `CTriggerEntry<>` slot 1 `FUN_140467ea0`.
  - Slot 22 `FUN_140463a90` evaluates:
    1. stack CEventScope `FUN_140534f00`, with ROOT/FROM copied and PREV = parent;
    2. `count` = CScopedVariable `+0x78` via `FUN_140544c90`;
    3. loop over the gamestate states `+0x2c8/+0x2d4`;
    4. per state: slot 26 filter, slot 25 `SetScope` (`FUN_140473e10` → `SetState`), then
       `CAndTrigger::Evaluate`;
    5. true once `count` states match.
  - Slot 23 = header loc key ("TRIGGER_ANY_STATE_STARTS").
  - Slot 6 item handler `FUN_140473280`: `tooltip` (0x92) → `+0x58`, `count` (0x29ea) → `+0x78`,
    anything else → the trigger factory.
  - Slot 11 tooltip `FUN_1403e0480`: `TARGET_TRIGGER_COUNT_PREFIX` when count > 1, the header
    from slot 23 (or the custom `tooltip` key), then the children at depth+1 (indented).
- **`random_state` = `CRandomStateEffect`** (0x108 bytes, vt 0x14276d618)
  - Slot 13 `FUN_1402f4520`: clear the candidate array (`+0xd0`, count `+0xdc`) → slot 25
    collect → slot 27 pick → slot 26 `SetScope` → children (`FUN_14053e130`).
  - Slot 25 `FUN_1403383f0`: loop over all states (optionally the `prioritize` list
    `+0xf0/+0xfc` first); evaluate the embedded `limit` trigger at `+0x78` per state.
  - Slot 27 `FUN_1402f7510`: **synced random from the scope's own RNG**: seed `+0x10`, counter
    `+0xc` incremented, hash, `% n`. Deterministic, so multiplayer-safe.
  - Slot 28: the state name for `display_individual_scopes`.
- **`every_state` = `CEveryStateEffect`** (0xf0 bytes): same slot layout (25 collect, 26
  `SetScope`, 27, 28).
- **Children's parse-time scope:** `CAndTrigger` ReadClause (slot 5 `FUN_14054fd00`) passes the
  scope mask it receives to every child item. A clone's slot 5 can pass 0x40, so children are
  validated against region scope ("Invalid scope type for trigger" on the factory's slot 16
  check).

**Engine pieces a native implementation needs:**

| Piece | Where it comes from | New signature? |
|---|---|---|
| `SetStrategicRegion` `FUN_14053b670` | `48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 45 84 C0 74 14 E8 ?? ?? ?? ?? 48 89 7B 48 …`, unique on all 3 builds (or the CODE call in `CTriggerAbilityCommand` slot 9/10) | 1 |
| CEventScope(parent) ctor `FUN_140534f00` | the first call in `CAnyStateTrigger` slot 22, via RTTI + CODE | RTTI |
| `CTrigger::Evaluate` base `FUN_14054d0b0` | `CAndTrigger` slot 3 | RTTI |
| `CAndTrigger` ReadClause | slot 5 | RTTI |
| `CAndTrigger::Evaluate` | slot 22 | RTTI |
| `CEffect` base execute | slot 12 | RTTI |
| children loop | slot 13 | RTTI |
| CEventScope dtor `FUN_140535820` | already resolved by the loader | — |
| region array | gamestate `+0x2e0/+0x2ec`, already resolved by the loader | — |

### F4. Options

| Option | Script syntax | Effort | New engine hooks / sigs | Crash risk | MP desync risk | Update fragility | Nested-block tooltip |
|---|---|---|---|---|---|---|---|
| **A. Prefix scope** `region:42 = { }` | vanilla-style, chainable, works in triggers and effects | 4–6 days | 4–5 **detours** on big parse/resolve/tooltip functions + byte sigs | medium (detours in hot paths, worker threads) | low (deterministic) | **high**: each new scope type changes those functions | Header needs a detour of `FUN_1413a5ad0` (no region case); without it the header is empty. Children are shown evaluated (✓/✗) like `GER = { }`. |
| **B. Native scope changers** (recommended) | `strategic_region = { region = 42 … }` (trigger and effect; `region` optional from scope); `any_strategic_region = { count = 2 tooltip = key … }`; `every_strategic_region` / `random_strategic_region = { limit = { … } random_select_amount / display_individual_scopes … }` | 3–4 days | **no detours**; 1 byte sig (SetStrategicRegion) + RTTI/CODE for the rest; our own objects only (vtable clones like the existing native commands) | low (the engine only calls our slots; SEH around ours) | low: random picks use the scope's synced RNG; iteration in region id order | low–medium: template slot numbers (22–28) are CODE-checked on each build | `strategic_region`: our own slot 11 prints "<region name>:" and then the children evaluated in the region scope (✓/✗), like `GER = { }`. `any_strategic_region`: **vanilla tooltip code** (count prefix, custom `tooltip`, indented children) with our header key `HOI4_LOADER_ANY_STRATEGIC_REGION_STARTS` (needs a loc entry; virtual yml or the mod). every/random: vanilla effect tooltip with our header key; `display_individual_scopes` uses our slot 28 (region name). |
| C. Global event targets per region (created by the loader at game start) | `event_target:hoi4_region_42 = { }` | 2 days | writes into the gamestate's event-target map | medium | medium (must be created identically on every machine; saved into savegames) | medium | vanilla, but the header shows the event-target name |
| D. Collection input `game:all_strategic_regions` | `every_collection_element = { collection = game:all_strategic_regions … }` | 4+ days (collections internals not yet mapped) | detour of the hard-coded input parser | medium | low | high | vanilla collection tooltips (name from the collection) |

**Without the loader** (all options): `strategic_region = {…}` or `region:42 = {…}` is an unknown
trigger or effect, or an invalid event target. error.log logs it and the block is dropped. A
dropped trigger block means the surrounding condition becomes **true**, so the existing guard
rule (`hoi4_loader_feature_<x> = yes` in the same AND block) applies unchanged.

**Recommendation: B.** It fits the loader's existing native-command model: guard trigger,
generated docs, the self-check and fail-safe. It needs no detours, reuses the vanilla iterator
skeletons (count/tooltip/limit/random/display_individual_scopes) by replacing only the
iterate/set-scope/name slots, and is multiplayer-safe by construction. Our region-aware triggers
(`air_superiority`, `naval_dominance`, `divisions_count`, `battles_count`, `planes_count`) and
`set_strategic_region_name` then work without a `region` parameter inside these blocks.

**Limitations:**
- `prioritize` (a list of state ids in `random_state`) has no region meaning and should be
  rejected with a parse error.
- Only any-scope vanilla commands (and ours) are valid inside the block.

Option A can follow later if the nicer syntax becomes important.

### F5. Implementation: option B (2026-10-17)

Approved as option B; option A stays a possible later upgrade. Feature `region_scope`
(`src/plugin_region_scope.cpp`), guard `hoi4_loader_feature_region_scope`:

- `strategic_region = { region = 42 ... }`, as a trigger and as an effect.
- `any_strategic_region`, `every_strategic_region`, `random_strategic_region`.
- `strategic_region_id = <int>`, `is_sea_region = yes/no`, `region_has_state = <state id>`, all
  with scope mask 0x40 only.

No vanilla trigger reads the region in scope: the game's documentation lists only
`hidden_trigger` and `meta_trigger` for STRATEGIC_REGION. Vanilla's `region` trigger is a
**state** trigger ("check state's strategic area id").

**API** (`native_script.h`):
- `register_native_scope_block`: the block's own parameters, plus a `target` callback that picks
  the scope (`ScopeTarget { type, id }`, only StrategicRegion supported).
- `register_native_iterator`: Any / Every / Random over every object of one scope type.
- Both go through the same registry as the other native commands: name conflicts, docs
  (`- Children run in:`, `- Keys (as the game's state iterator):`), guard trigger and status.

**Binding** (`src/native_scope_engine.cpp`; engine pieces in `src/engine_scope.cpp`):

| Command | Object | Replaced slots |
|---|---|---|
| `strategic_region` trigger | `CAlwaysTrigger` clone, `ParsedCommand*` at +0x58 (as the generic triggers) | 5 = `CTrigger` block loop; 6 = parameter, else `CTrigger` slot 6 (child factory) with mask 0x40; 11 / 12 = tooltips; 21 = `CAndTrigger`'s empty line; 22 = evaluate (child scope, `CAndTrigger::Evaluate` on its own children) |
| `strategic_region` effect | `CRenameProvinceEffect` clone, `ParsedCommand*` at +0x58 | 3 = `CEffect` block loop; 4 = parameter, else `CEffect` slot 4 with mask 0x40; 7 = `CEffect`'s empty line; 8 / 10 = tooltips; 13 = children loop (`CEffect` slot 13) in the child scope |
| `any_strategic_region` | built by `CTriggerEntry<CAnyStateTrigger>`, vtable swapped | 6 = vanilla keys to the vanilla handler, others to the child factory with mask 0x40; 22 = loop over regions in id order (`count` from +0x78, as vanilla); 23 = header text |
| `every_strategic_region` | built by `CEffectEntry<CEveryStateEffect>` | 4 = item (`limit` read with mask 0x40); 10 = per-region tooltip; 25 = collect into the candidate list; 26 = `SetStrategicRegion`; 27 = highlighted region name |
| `random_strategic_region` | built by `CEffectEntry<CRandomStateEffect>` | 4 = item (`prioritize` rejected); 25; 26; 28 = region name. Slot 27 (the synced-RNG pick) stays vanilla. |

- **Iterator data:** each iterator clone keeps an `IteratorInfo*` one slot past the template's
  last slot, so the shared slot functions find their command without a lookup table.
- **Kept vanilla:** execute, tooltips, `random_select_amount` trimming and the pick all stay
  vanilla. They reach the elements only through slots 25–28, which were checked in the
  decompilation.
- **every_state slot 10 (`FUN_14036c840`):** the one vanilla slot that reads an element as a
  state directly (`SetState(elem+0x58)`), so it is replaced.
- **Child scopes, built as the vanilla iterators build them:**
  - CEventScope copy constructor `FUN_140534f00(child, parent)`. It copies all fields, including
    the scope's synced RNG seed and counter (+0xc).
  - Then ROOT/FROM = the parent's, and PREV = the parent.
  - Then `SetStrategicRegion(child, region, clear = true)`, which clears the copied
    state/country/etc.
  - Destroyed with the CEventScope destructor.
  - One child scope per evaluation; the region is re-set per element, as `any_state` re-sets the
    state.
- **Item handlers.** The vanilla handler's compare chain is decoded at startup:
  - The pattern is `cmp r8d,<key>` / `je|jne` ... `mov r9d,2` / `jmp <child factory>`.
  - The decoder yields the keys the vanilla handler reads itself; the `limit` key's handler is
    recognised by its code (`kCodeTargetEffectLimit`).
  - Our handler sends those keys to the vanilla handler, reads `limit` into the embedded trigger
    at +0x78 with mask 0x40 (a second `limit` goes to vanilla for its "Multiple limits" error),
    and sends every other key to the child factory with mask 0x40.
  - The same keys on all three builds:

    | Template | Keys |
    |---|---|
    | any_state | 0x92 (`tooltip`), 0x29ea (`count`) |
    | every_state | 0x92, 0x2a0a (`limit`), 0x3b8b, 0x3fff, 0x4bed |
    | random_state | 0x3640 (`prioritize`), 0x92, 0x2a0a, 0x3b8b |

  - `loader_script_docs.md` names them via the token table.
- **Rejecting `prioritize`.** The key named `prioritize` among random_state's keys is rejected
  with the engine's "unexpected token" (file and line, value skipped) and one loader.log line.
  The rest of the block works.
- **Tooltip temporaries.** Strings the engine returns are freed with the engine's
  `operator delete`, taken from `CEveryStateEffect`'s scalar deleting destructor, using MSVC's
  `_Tidy_deallocate` rule. The engine's own string destructor exists twice (identical copies,
  0x14011ebd0 and 0x140a0cf30), so it cannot be a unique signature.
- **Tooltip formats:**
  - Trigger slot 11, the formatter form: "<region>:" as one requirement line, marked by the
    whole block (the base slot 3 in the outer scope), then the children through `CTrigger`
    slot 11 at indent + 1 in the region scope. This is the shape of `CContextTrigger`'s slot 11.
  - Trigger slot 12, the text form: 3 spaces per indent level, the `TRIGGER_(UN)FULLFILLED_PREFIX`
    rule of `FUN_1403e0940`, "<region>:\n", then the children through `CTrigger` slot 12.
  - Effect slots 8 / 10: "<indent><region>:" then the children through `CEffect` slot 8 / 10 at
    indent + 1.
- **any_ header text (request item 2).**
  - **Options weighed:**
    - a stub loc file in the stub pack, served through the VFS: needs a yml per language and
      depends on the load order;
    - text the loader provides itself.
  - **Chosen: the loader provides the text.** Slot 23 returns the localisation of
    `HOI4_LOADER_ANY_STRATEGIC_REGION_STARTS` if a mod defines it, else "Any strategic region".
    It is looked up once per session, at the first tooltip. No file, works in every language, and
    a mod can still translate it.
  - every_ and random_ need no key: vanilla builds their headers from the element names
    (slots 27 / 28).
- **`is_sea_region`.**
  - **Source:** the region's sea-province share (fixed point at `region+0xa0`, written with
    +0x98 and +0xa8 in `FUN_140e23bb0`). The game's own region console command tests it for
    "Selected region has no sea province" (`kSigRegionSeaShare`).
  - **Checked offline:** the map files give 98 of 304 regions with sea provinces, and the
    self-check logs the engine's count.
  - **Not usable:** the region's naval object (+0xe8) is created for every region in the
    `CStrategicRegion` constructor, so it says nothing about sea.
- **`region_has_state`:** a pointer compare between the state's and the region's province
  lists. `RegionOfState` (used without `region` in a state scope) is the region holding the
  state's first province. State 16 lies wholly in region 19.
- **Fail safe:**
  - `HaveScopeBlocks()` needs every piece; otherwise the feature does not start.
  - Engine calls that run children (evaluate, execute, collect) run under SEH. A fault logs once
    and turns all scope blocks and iterators off (false / nothing) for the session.
  - The self-check on game entry checks every region's id and sea flag and state 1's region; a
    failure deactivates the feature.

**New signatures and shapes:**

| Name | What | Checked |
|---|---|---|
| `kSigScopeSetRegion` (SIG) | `SetStrategicRegion` | unique on vanilla 1.19.3, patched 1.19.3, 1.19.2 |
| `kSigRegionSeaShare` (SIG) | the sea share offset | unique on all three |
| `kCodeAnyStateEvaluate` (CODE) | any_state slot 22: copy constructor call; it must also call the known scope destructor and `scriptValueEval` | all three |
| `kCodeTargetEffectExecute` (CODE) | every/random slot 13: candidate count +0xdc, collect = slot 25 | all three |
| `kCodeTargetEffectLimit` (CODE) | the `limit` handler: limit trigger at +0x78 | all three |
| `kCodeScalarDeletingDtor` (CODE) | every_state slot 0: `operator delete` | all three |

`tools/check_code_shapes.py` decodes the key chains the same way. `tools/check_signatures.py`
also passes on all three builds.

**Tests:** `native_script_test` covers:
- registration rules: duplicates, scope type, missing target or header;
- target resolution: explicit, from the scope, none, a crash, the wrong type;
- the docs lines.

All pass. The engine side is not testable offline beyond the shape checks.

### F6. In-game test plan (part 3)

Install the DLL and sync the test mod, start a 1939 game as France, and keep
`trigger_timing = 1` on.

1. **Startup (`loader.log`):**
   - `region_scope: scope copy ..., any_state keys 2, every_state keys 5 (limit 0x2a0a), random_state keys 4 (limit 0x2a0a)`.
   - `registered` for `strategic_region` (trigger and effect), the three iterators and the three
     identity triggers.
   - `loader_script_docs.md` lists the iterators' keys by name.
   - `error.log`: one unexpected-token error for `prioritize` in
     `hoi4_loader_test_region_scope_decisions.txt`, and no other error from that file.
2. **Game entry:** `region_scope: self-check passed (304 strategic regions, 98 sea; state 1 lies in ... (169))`.
3. **Hover "Region scope: strategic_region and identity triggers":**
   - "Northern France:" with three ticked lines;
   - "Bay of Biscay:" with two;
   - the state form;
   - the NOT line.
4. **Hover "Region scope: data triggers inside":** the data trigger lines name Northern France
   (19) and show current values.
5. **Hover "Region scope: any_strategic_region":**
   - the header is "Any strategic region";
   - the count lines read "At least 98 of" / "99";
   - the custom tooltip line;
   - all ticked.
6. **"Region scope: rename the Bay of Biscay to A":** the tooltip shows "Bay of Biscay:" with the
   rename line. Take it: the map shows "Loader Test Sea A" and the description shows "Ran: 1".
7. **every_strategic_region decisions:**
   - "count the sea regions with our air superiority above 50 percent" lists the regions (none in
     peacetime).
   - "3 random sea regions" shows "Last count: 3" after taking it.
   - "rename ... to B" renames the Bay of Biscay.
8. **random_strategic_region:**
   - The tooltip names one of the North Sea, Channel or Bay of Biscay, and taking it renames that
     one to C.
   - The prioritize decision shows "Ran: 1".
   - Then "reset all renamed regions".
9. **Cost:** after 10 in-game days, the timing block lists `any_strategic_region` and
   `air_superiority` (AI probe plus hovering), with evaluations and ns each.

**In-game results (2026-10-17, vanilla 1.19.3, 1939 France)**

- **First run (DLL `f8745ca8`): the lexer-ready callback crashed** in the trigger name-conflict
  check, so every native command was off.
  - **Cause:** the check walked the trigger registry object as a std::map. The map is at +0x18
    (an array and allocator come first), so the walk had always read unrelated heap memory. It
    returned "not present" by luck until the heap layout changed.
  - **Fix `e38b986`:**
    - the offset is now read from the trigger Register (`kCodeTriggerRegisterMap`, +0x18 on
      all three builds);
    - the lookup checks the map's shape, runs under SEH, and refuses the name on failure.
- **Second run (DLL `ec5d1a53`): startup, self-check and cost pass.**
  - **Startup:**
    - `map +0x18`;
    - 20 of 20 commands registered;
    - the iterator keys as expected (2 / 5 / 4, limit 0x2a0a);
    - every guard is yes.
  - **Errors:** `error.log` has only the `prioritize` unexpected-token error from the region
    scope decisions (plus the two known planes errors). `loader.log` has the prioritize line
    once.
  - **Self-check:** "304 strategic regions, 98 sea; state 1 lies in Tyrrhenian Sea (169)".
    Region 169 is a mixed region: Corsica's land provinces plus sea.
  - **Timing:**

    | Window | `strategic_region` | `any_strategic_region` | air_superiority | Numeric triggers only |
    |---|---|---|---|---|
    | 1, peacetime with hovering | 4319 evaluations, 1721 ns | 4059 evaluations, 26882 ns (10.9 ms/day) | 149412 evaluations, 130 ns | 5.2 ms/day |
    | 2, war from 3 September | 45 evaluations | 918 evaluations, 49369 ns (4.5 ms/day) | | 2.0 ms/day |

    - That is about 90–160 ns per region visited.
    - The "all numeric triggers" total excludes scope blocks and iterators, as designed.
  - **Not related to the loader:** the AI's "invalid command: unlock_trait_command" spam (851
    lines in the run before part 3 too) and the AI's order_* commands at war.
  - **Steps 3–8 pass (user report):**
    - **Identity:** "Northern France:" (id 19, land, holds state 16); "Bay of Biscay:" (sea,
      not state 16); "Ile de France: Northern France:" (the game puts a single child on its
      parent's line); NOT Western Germany is sea, shown as "Is a land region".
    - **Data inside:** "Northern France (19)" with current values, no region parameter.
    - **any_strategic_region:** the header "Any strategic region", "At least 98 of", and the
      custom tooltip. All results are right.
    - **Effects:**
      - "Bay of Biscay:" and the random pick's "(North Sea):" headers;
      - random_select_amount 3 lists 3 sea regions, Last count 3;
      - prioritize Ran: 1;
      - every_ with the air limit: Last count 0 in peacetime.
      - Renames A, B and C show on the map, and the reset restores every name.
  - **Tooltip findings:**
    - **NOT around `any_strategic_region = { count = 99 ... }`** reads "At least 99 of Any
      strategic region: Is a land region". That is vanilla's any_state rendering (slots 11/12
      kept): the localisation has no "none of" text.
    - **Guards:** each guard shows an "Always true" line (the generated `always = yes`).
    - **Empty effect headers:** set/reset_strategic_region_name have no tooltip line (Part B),
      and global variable effects show nothing, so a block holding only those prints an empty
      header.
    - **Data trigger under an iterator's header:** it has no region, so it shows its raw name
      and "(currently unavailable)", e.g. "air_superiority above 50% (currently
      unavailable)".

### F7. Tooltip fixes (2026-10-17, approved: all four)

1. **Rename effect lines.**
   - **Text:** set/reset_strategic_region_name now show "$OLDNAME|Y$ is renamed to $NAME|Y$." /
     "$NAME|Y$ gets its original name back." (vanilla's province wording).
   - **Keys:** `HOI4_LOADER_SET_STRATEGIC_REGION_NAME` and
     `HOI4_LOADER_RESET_STRATEGIC_REGION_NAME`, with English used when no mod defines them.
   - **Placeholders through the engine.** A text written as `$X$` cannot be filled in after
     `Localize()`, because the localiser resolves `$X$` itself and localises an unknown name as
     a key. So the loader passes real arguments:
     - `Localize(out, key, CPdxLocalizeKeyValuePair*, n)`;
     - the pair stride is 0x68: type @0 (1 = std::string value), `const char*` name @8,
       std::string name @0x10, std::string value @0x30;
     - the matching is in FUN_142248c20 and the formatting (`|Y`) in FUN_1422479f0.
   - **Self-test on first use:** `EFFECT_RESET_PROVINCE_NAME` with NAME = a probe string must
     contain the probe. Otherwise the loader logs once and uses English only.
   - **English fallback:** the loader fills the placeholders itself and wraps `|Y`/`|H` values
     in the game's colour codes (0x11).
   - **API:** `LocText`, `native_localize`, `native_localize_key`, and `NativeEffect::texts`
     (listed in the docs; the any_strategic_region header key is listed too).
   - **Layout:** the effect's own line (slot 7) needs no indent or newline. CEffect slot 8
     (FUN_14053f090) prefixes 3 spaces per level and puts a newline between lines.
2. **Hidden guards.**
   - The served `hoi4_loader_feature_<name>` and `hoi4_loader_present` are now
     `hidden_trigger = { always = yes|no }`. `hidden_trigger` is valid in every scope, the
     results are unchanged, and there is no "Always true" line any more.
   - MODDING.md shows `custom_trigger_tooltip` around the guard for mods that want a visible
     "Requires hoi4_loader" line.
   - Stubs keep `always = no`. Inside the `custom_trigger_tooltip` the stub's line is not shown
     either.
3. **Data trigger lines without their scope object.**
   - **How tooltips resolve now:** they resolve parameters "partially". A scope default the scope
     lacks stays absent, with no log line (the evaluation still logs it once).
   - **Numeric triggers:** the subject leaves the part out, and the line drops "(currently ...)".
     Example: "Air superiority of FRA above 50%" under any_strategic_region's header, instead of
     "air_superiority above 50% (currently unavailable)".
   - **Subjects that read the scope themselves** (divisions/ships/battles/planes areas) call
     `TooltipText::markScopeMissing()`.
   - **Other trigger tooltips** also run with absent parameters:
     `strategic_region_has_custom_name` shows "Has a custom name".
   - **Side effect:** test decision 8 (NOT air_superiority outside a region) now reads
     "NOT: Air superiority of FRA above 0%".
   - **Unchanged:** "(currently unavailable)" remains for values that do not exist (unknown
     region id etc.).
4. **Docs note on NOT around a counted any_.**
   - MODDING.md explains vanilla's wording and recommends `tooltip = <key>`.
   - The test decision "any_strategic_region" adds that form and vanilla's
     `NOT = { any_state = { count = 2000 is_coastal = yes } }` for comparison.

**In-game test plan (F7):**
1. **Startup:**
   - 20 of 20 registered;
   - `loader_script_docs.md` lists "Tooltip texts" for set/reset_strategic_region_name and
     any_strategic_region;
   - the guard files under `%TEMP%\hoi4_loader` read `hidden_trigger = { always = yes }`.
2. **Decisions hover:**
   - **No "Always true" lines** anywhere.
   - **"strategic_region and identity triggers"** starts with "Requires hoi4_loader (strategic
     region scopes)", ticked.
   - **"a sea region with our air superiority above 50 percent":** the line under the header
     reads "Air superiority of FRA above 50%" (no "currently").
   - **"any_strategic_region":**
     - "Fewer than 99 sea regions", ticked;
     - vanilla's "At least 2000 of Any state:" with "Is not coastal" (the same wording as ours).
   - **Data test "NOT air superiority without a region":** "NOT: Air superiority of FRA above
     0%".
3. **Effects:**
   - "rename the Bay of Biscay to A": "Bay of Biscay is renamed to Loader Test Sea A." with
     both names highlighted;
   - the random C rename names the picked region the same way;
   - "reset all renamed regions": "... gets its original name back." per renamed region;
   - `loader.log` has no "localisation with arguments failed its self-test" line.

**In-game results (F7, 2026-10-18; decisions 23, 27, 31, 33 reported):**
- **loader.log:** 20 of 20 registered; no self-test failure line.
- **23 (identity):** "Requires hoi4_loader (strategic region scopes)" ticked first; no "Always
  true" line.
- **27 (rename to A):** "Bay of Biscay: Bay of Biscay is renamed to Loader Test Sea A."
- **31 (random C):** "(North Sea): North Sea is renamed to Loader Test Sea C (flag protocol)."
- **33 (reset all):** "(Loader Test Sea C (flag protocol), Loader Test Sea A): Loader Test Sea C
  (flag protocol) gets its original name back."
  - Only one line, naming only the first region. An every_ block builds its child lines once,
    for its first matching scope (vanilla every_state does the same).
  - My test plan expected one line per region. That was wrong.
- **8 (NOT air superiority without a region):** "NOT: Air superiority of FRA above 0%", ticked.
- **25 (any_strategic_region):** every line ticked.
  - Under NOT the game negates the children, so "At least 99 of Any strategic region:" and the
    custom header "Fewer than 99 sea regions:" both list "Is a land region".
  - Vanilla shows "At least 2000 of Any state:" with "Is not coastal." the same way, which
    confirms the MODDING.md note: the wording comes from the engine, and `tooltip = <key>` is the
    fix for the header.
- **26 (air superiority above 50%, peace, not available):** "Any strategic region:", "Is a sea
  region", "Air superiority of FRA above 50%". No "(currently ...)".
- **"Always true":** none in any decision.

### F8. Rename lines inside a region scope (2026-10-18)

- **Problem:**
  - Inside a region scope the header names the region already, so 27 names it twice.
  - Under every_strategic_region, 33 names only the first region.
- **Vanilla's model:**
  - States: `EFFECT_SET_STATE_NAME` "Changes name to $NAME|Y$." and `EFFECT_RESET_STATE_NAME`
    "Gets its original name back." These do not name the state (the scope header does).
  - Provinces (given by id): `EFFECT_SET_PROVINCE_NAME` / `EFFECT_RESET_PROVINCE_NAME`, which
    name it.
- **Fix:**
  - `ArgValue::fromScope` / `Args::fromScope(i)` are set when ResolveArgs fills a parameter from
    the scope.
  - With `region` from the scope, set/reset use the state wording, under the new keys
    `HOI4_LOADER_SET_STRATEGIC_REGION_NAME_IN_SCOPE` and
    `HOI4_LOADER_RESET_STRATEGIC_REGION_NAME_IN_SCOPE`.
  - With `region` written, they keep the province wording.
- **Expected:**
  - 27: "Bay of Biscay: Changes name to Loader Test Sea A.";
  - 31: "(North Sea): Changes name to Loader Test Sea C (flag protocol).";
  - 33: "(Loader Test Sea C (flag protocol), Loader Test Sea A): Gets its original name back.";
  - the every_ rename (B): the header, then "Changes name to Loader Test Sea B.".

**In-game results (F8, 2026-10-18): all as expected.**
- **27:** "Bay of Biscay: Changes name to Loader Test Sea A."
- **30 (every_ rename to B):** "Loader Test Sea A: Changes name to Loader Test Sea B." With a
  single match the header shows the name without parentheses.
- **31:** "(English Channel): Changes name to Loader Test Sea C (flag protocol)."
- **33:** "(Loader Test Sea C (flag protocol), Loader Test Sea A): Gets its original name back."

### F9. Does random_strategic_region's tooltip name the region that is picked? (2026-10-18)

**Answer:** yes, for a decision, unless the candidate list or the draws before it differ between
hover and execution. Both paths make the same draw from a scope seeded the same way. Read from
1.19.3 (the code is vanilla; the loader replaces only the candidate collection, slot 25).

**The draw: `CRandomStateEffect` slot 27 `FUN_1402f7510(effect, scope)`.**
- `h = hash(scope+0x10 (seed) + scope+0xc (counter) * -0x4ad685b3)`, then
  `scope+0xc += 1`, and the result is `candidates[(h & 0x7fffffff) % n]`.
- `candidates` is our slot 25's list, filled in region id order with the regions that meet
  `limit`.
- It reads no global RNG and no date. The result depends only on the scope's seed, its counter
  and the list.

**Both paths call it with the scope they were given:**
- Execute is slot 13, `FUN_1402f4520(effect, scope)`: collect, then `slot27(scope)`, then
  `SetScope` on a copy, then the children.
- Tooltip is slot 8, `FUN_1402f4ff0(effect, out, scope, depth)`: collect, then
  `slot27(scope)`. The header shows that pick, through slot 28, and the children's lines are
  built in a copy set to that pick.

**The decision builds both scopes the same way:**
- **Tooltip of `complete_effect`:** `FUN_14072aa30(decision, out, country)`.
  1. `CEventScope()` (`FUN_140535110`: seed 0x5ea6ba9e, counter 1).
  2. `SetCountry`.
  3. `FUN_140735d00(decision, scope, 0)`.
  4. `decision+0x408` slot 8 (the tooltip).
- **Execution (`CSelectDecisionCommand` → `FUN_1407357f0` → `FUN_140727ae0(decision, country)`):**
  1. `CEventScope()`.
  2. `SetCountry`.
  3. `FUN_140735d00(decision, scope, 0)`.
  4. `decision+0x408` slot 12 (execute, which reaches slot 13).
- **`FUN_140735d00(decision, scope, target)` sets the seed:**
  - `x = gamestate+0x720 + decision+0x140 + record+0x10 + target`;
  - `scope+0x10 = H1(x)`, `scope+0xc = H2(x)`, the same hashes as the seeded `CEventScope`
    constructor; FROM is seeded from it too.
  - `gamestate+0x720` is one synced random number drawn in `CInGameIdler::InitData`
    (`FUN_1401e1120`, `gamestate.cpp:0x980`). It is the campaign's seed, not the date.
  - `decision+0x140` is the decision's id.
  - `record` is the country's entry for this decision and target (`FUN_14072dda0`, list at
    country+0x148), or 0 if there is none. Taking the decision updates it only after the
    effects ran.
  - Targeted decisions pass their target (`FUN_140728720`, the same pattern).

**What follows:**
- Within one campaign, a decision shows and picks the same region as long as its candidate
  list is the same.
  - That holds across days and while the game runs: the command executes a tick later, but the
    seed has no date in it.
  - Different campaigns pick differently. F7 picked the North Sea and F8 the English Channel
    with the same three candidates.
- **The pick changes legitimately when:**
  - the `limit` result changes;
  - the decision's record changes after it was taken (a later hover may then show another
    region, and that is again the one taken).
- **The tooltip and execution can disagree when:**
  1. the candidate list changes between hover and execution, e.g. a `limit` with a data
     trigger whose value moves during that tick;
  2. an earlier effect in the same block draws from the scope's counter only when it executes,
     not when its tooltip is built. Each draw advances the counter, so the later pick shifts.
     The same holds for vanilla `random_state`.

**In-game check (F9):** "Release 0.5.0-beta: install test from the zip", session 1 step 4.

### Step 10 passes: without the loader (2026-10-18, current feature set)

- **Setup:**
  - Diagnostics are off in the installed `loader_config.ini`.
  - **Phase A, with the loader (DLL `2e0bb12b`):** new game, region 42 renamed to A, save, quit.
  - `version.dll` was then renamed to `version.dll.off`.
- **The loader did not run:** `loader.log` is unchanged since 11:17:40 (phase A).
- **The save loads.**
  - The Bay of Biscay shows its original name.
  - "Region Name Check":
    - stored flag name: Loader Test Sea A;
    - native trigger: no;
    - guard: no;
    - plugin flag: active (stale from the save).
  - After the first weekly refresh the plugin flag is cleared and the "unguarded" demo decision
    disappears, as designed.
- **Decision categories:** "data" and "AI cost" are hidden (`hoi4_loader_present` stub). The test
  category shows only the unguarded decisions.
- **`error.log`:** only the documented cost.
  - "Unknown trigger-type" / "Unknown effect-type" for every native command, in:
    - the three decision files;
    - the scripted localisation (`strategic_region_has_custom_name`).
  - The commands inside a dropped `strategic_region` block (`strategic_region_id`,
    `is_sea_region`, `region_has_state`, the data triggers) get no line of their own: the
    engine drops the whole block.
  - No line for `hoi4_loader_feature_*`, `hoi4_loader_present` or `hoi4_loader_init` (the stubs
    are served from disk).
  - Everything else is unrelated and appears with the loader too: mod descriptors, vanilla
    entities, the lobby `remotefile.cpp` exception.
- **Found while preparing (`ec2621e`):**
  - The test mod's stub `hoi4_loader_init` cleared only 4 of the 9 plugin flags. Now it clears
    all of them.
  - `run_smoke.sh` checks the stub and the feature table (now in MODDING.md) against `src/features.cpp`.
- **Afterwards:** `version.dll` was restored (`2e0bb12b`).
- **Open:** D3 step 10, multiplayer and hot join. It needs a second player.

## Release 0.5.0-beta: install test from the zip (2026-10-18)

The test uses only the zip's contents on a clean setup. Files are relative to the unpacked
`hoi4_loader-v0.5\`; "the ini" is `loader_config.ini` in the game folder.

**Setup:**
1. Quit the game.
2. Unpack `release\hoi4_loader-v0.5.zip`.
3. Copy `game\version.dll` and `game\loader_config.ini` into the game folder.
4. Delete `Documents\...\mod\hoi4_loader_test\` and `hoi4_loader_test.mod`, then copy the
   zip's `mod\` contents there.
5. Delete `%TEMP%\hoi4_loader\` and `%LOCALAPPDATA%\hoi4_loader\`.
6. Enable HOI4 Loader Test in the launcher.

**Session 1 (as shipped):**
1. **`loader.log`:**
   - the first line is `hoi4_loader 0.5.0-beta (script version 3)`;
   - `Supported hoi4.exe build: 1.19.3_...`;
   - `20 of 20 command(s) registered`;
   - `Loader ready ... hoi4_loader_version = 3, engine tier enabled`;
   - no `Signature report` line.
2. **New game, France 1939:** no "hoi4_loader Features Missing" event. The status event shows
   loader version 3.
3. **Decisions:** both categories are there, and hovering behaves as in F7/F8.
4. **F9, the random pick:**
   1. Hover "rename one of North Sea, Channel, Bay of Biscay to C" and note the region in the
      header.
   2. Take it while the game runs: that region is renamed.
   3. Take "reset all renamed regions".
   4. Hover C again and note the region; take it again. The renamed region is the one the
      tooltip named.
5. Save as `v05_ok` and quit.

**Session 2 (ini: add `[diagnostics]` `signature_scan = 1`):**
- `loader.log` says `Signature report ([diagnostics] signature_scan = 1)`, then `44/44
  signatures and anchors` and `17/17 code shapes`.
- It has no `FAILS` or `NOT CHECKED` line, and the engine tier is enabled.
- The main menu is enough; quit.

**Session 3 (ini: remove `signature_scan`, add `[features]` `region_scope = 0`):**
- Load `v05_ok`. Right after loading, the warning event shows: region scopes missing, region
  control data available.
- Don't save; quit.

**Session 4 (ini: remove `region_scope = 0`; put `;` before both `[supported_hashes]` lines):**
1. **`loader.log`:**
   - `Unknown hoi4.exe build`;
   - `44/44` and `17/17`;
   - the "To report this game version" list;
   - `engine tier disabled`.
2. **Load `v05_ok`:**
   - the data category is hidden;
   - within 7 in-game days the warning event shows both features missing;
   - the game runs normally.
3. Don't save; quit.

**Session 5 (copy `game\loader_config.ini` from the zip again):** the log is as in session 1.

**Results (2026-10-18, vanilla 1.19.3): all five sessions pass.** The installed files are the
zip's: `version.dll` sha256 `f5485f06…`, and the ini is identical to the release ini.
- **Session 1:**
  - **Startup:** `0.5.0-beta (script version 3)`, `1.19.3_vanilla`, 20 of 20 registered,
    engine tier enabled.
  - **New game:** no warning event.
  - **F9:** the random pick matched the tooltip each time, including the second pick after the
    reset.
- **Session 2 (`signature_scan = 1`):**
  - `44/44 signatures and anchors`, `17/17 code shapes`, no FAILS or NOT CHECKED lines;
  - report 0.47 s; engine tier enabled.
  - An inline comment after the value (`signature_scan = 1 ; …`) still reads as 1:
    `GetPrivateProfileInt` takes the leading number.
- **Session 3 (`region_scope = 0`):**
  - the log says `DISABLED in loader_config.ini [features]` and guard `= no`;
  - after loading `v05_ok`, `on_game_loaded fired: ROOT = FRA`;
  - the warning event showed region scopes missing and region control data available.
- **Session 4 (both hashes commented out):**
  - **Log:** `Unknown hoi4.exe build (checked 0 hash(es))`, `44/44`, `17/17`, no fault, the
    reporting list, engine tier disabled.
  - **In game:** with `v05_ok`, the data category is hidden and the weekly check shows the
    warning event. The game ran normally.
  - **`error.log`:** only the known lobby `remotefile.cpp` lines.
- **Session 5 (the release ini again):** the log is as in session 1.
- **Still open:** multiplayer (D3 step 10).

## Part G: create an army under a chosen general (investigation, G1)

Investigation only, nothing implemented. Branch `feature/create-army`. Addresses are from vanilla
1.19.3 (`hoi4_copy.exe`) and only for orientation; an implementation finds everything through
RTTI and code shapes and is checked on all three builds first.

### G1.1 What vanilla covers

`documentation/effects_documentation.md` and `triggers_documentation.md`, 1.19.3:

| Need | Vanilla | Notes |
|---|---|---|
| Spawn divisions | `create_unit = { division = "<OOB text>" owner = TAG prioritize_location = <prov> allow_spawning_on_enemy_provs = no country_score = { … } count = N id = N divisional_commander_xp = N }` | `division` holds `division_template`, `start_experience_factor`, `start_equipment_factor`, `name`; no army, no general |
| Spawn from an OOB file | `load_oob = "ENG_1936"` (country) | OOB files have no army or general keys: `officer = { … }` in a division is its divisional commander, `army = { … }` only appears inside `portraits` |
| Create or recruit generals | `create_corps_commander`, `create_field_marshal`, `recruit_character`, `promote_leader`, `add_corps_commander_role`, `add_field_marshal_role` | |
| Move armies | `teleport_armies` (state scope) | moves existing units, no regrouping |
| Iterate divisions / leaders | `every_country_division`, `random_country_division`, `every_state_division`, `every_army_leader`, … | division scope exists (scope bit 0x100, NOTES "Scopes") |
| Read leader state | `is_corps_commander`, `is_field_marshal`, `is_leading_army`, `is_leading_army_group`, `is_army_leader`, `is_general_captured` | |
| Army from divisions + general | **none for script.** `start_border_war` does it internally (G1.2) but only for the units already in the border state and a leader it picks by `leader_score` | |

So vanilla cannot put given divisions under a given general. That part is missing.

### G1.2 Engine structures

**Naming.** The UI's "army" is a `COrdersGroup` (source `ordersgroup.cpp`). An army group is the
same base class with the `IsArmyGroup` flag. A division is a `CArmy` (the `CUnit` handle
sub-object sits at `+0x10`). The general is a `CUnitLeader` role of a `CCharacter`.

**Reference handles.** Game objects are addressed by 8-byte handles `{type, index}`; the object's
own handle is at `obj+8`. `FUN_14221f310(&handle)` resolves one (per-type hash tables;
`DAT_14333d528` is the null handle). New armies take type `0x35` and the next index from the
global counter `DAT_143087260`.

**Per-country army manager (called X below):**
- From a country: `X = **(country+0x168)` (as `start_border_war` does). From a division:
  `X = unit+0x2a0` (`FUN_140bf9660`, as the UI does). From an army: `army+0x40`.
- `X+0x48` tag; `+0x68` divisions (count `+0x74`); `+0x80` armies; `+0x98` army groups;
  `+0xb0` all groups; `+0xc8` theatres (count `+0xd4`).

**Army (`COrdersGroup`, 0x230 bytes; army group 0x250):**

| Offset | Meaning |
|---|---|
| `+0x39` | is army group |
| `+0x3c` | country tag |
| `+0x40` | X |
| `+0x50` / `+0x5c` | member list (division member sub-objects) / count |
| `+0x68` | deployed army HQ (non-null while deployed) |
| `+0x88` | leader (`CUnitLeader*`) |
| `+0x160` | name string |
| `+0x180` | theatre (`FUN_140befc20`, asserts it is set) |
| `+0x199` / `+0x19a` / `+0x19b` | HQ deploying / deploying / withdrawing (leader change refused) |
| `+0x1b0` | border war object (non-null while the army is in one; G1.3) |
| `+0x1b8` | parent army group |
| `+0x230` / `+0x23c` | child armies (army groups only) |

**Division:** army at `unit+0xc0`, member sub-object at `unit+0xb8`, tag at `unit+0x1d8`,
handle at `unit+0x18`, X at `unit+0x2a0`. The country's division list is `country+0x290`
(Part E).

**Leader:** role type at `leader+0xe7c` (0 field marshal, 1 corps commander, ≥ 2 navy; "army
leader" is `< 2`); tag at `leader+0x120`; name at `leader+0x40`; handle of the army or army group
it leads at `leader+0x1048`; vtable `+0xe8` is "captured" (`is_general_captured`).
**Character:** `FUN_1406b4eb0(gamestate+0x6a8, token)` finds it by token (`recruit_character`,
"Unknown character %s"); owner tag at `character+0x68` (< 1 = not recruited); army leader role
`FUN_140f9f9c0(character)` (`+0xa8`, else handle `+0xc8`). A character scope holds the handle at
`scope+0x50`; `FUN_140535e00(scope)` gives its army leader role.

**UI commands (lockstep `CCommand`s).** Slot 9 = can execute, slot 10 = execute, slot 11 = type
id, slot 13 = clone. The game-state work in each execute:

| UI action | Command | Inner function (what the loader would call) |
|---|---|---|
| Create army from selected divisions | `COrderGroupCommand` | `FUN_140bebf30(&theatre, &X, &unitHandles, keepEmpty)` → new army; then `FUN_140beab30(parent, army, -1)` if a parent army group was given |
| Assign general | `CSetArmyLeaderCommand` (id 0x3051) | `FUN_140bf5a20(army, leader, 1)`; before it `FUN_140bebaa0(army, 0)` = can change leader |
| Create army group | `CArmyGroupCommand` | `FUN_140ef0fc0(X, armies, 0, 0)` |
| Add army to army group | `CAssignToArmyGroupCommand` | `FUN_140beab30(group, army, index)` (owner check first) |
| Delete army | `CDeleteOrderGroupCommand` | `FUN_140f009e0(X, army, 1)` |

- **`FUN_140bebf30` (create army)** does all the bookkeeping:
  - creates the army with `FUN_140ef1180(X, …)`, which puts it into X's lists and, with no
    theatre given, into X's last theatre (or a new one; `FUN_140eecf20`);
  - for each handle: skips units of another tag, asserts against railway guns, and moves the unit
    in (`FUN_140befcb0`: out of its old army, into the member list, `unit+0xc0` = army);
  - deletes the new army again if it stayed empty (unless `keepEmpty`);
  - deletes each old army the units left if that one is now empty (`FUN_140ef16f0`).
- **`FUN_140bf5a20` (set leader)** stores `army+0x88` and, with the last argument 1, calls
  `FUN_140c267a0(leader, army, 1)`. That detaches the leader from the army it led before (which
  keeps its divisions and has no general) and sets `leader+0x1048`.
- **`CSetArmyLeaderCommand` execute adds, around that call:**
  - the command power charge: `FUN_140c15140(leader)` computes the assignment cost (country
    modifier 0x249 for army leaders, "UNIT_LEADER_ASSIGNMENT_COST_HEADER") and `FUN_1406cfe60`
    applies it to `country+0x1f0`;
  - an AI bookkeeping reset (`FUN_141950d70` on `country+0xf88` → `+0x290` → `+0x9f8`);
  - HQ handling;
  - UI refreshes through `DAT_14332f698`, which an effect must not call.
- **Vanilla precedent: `start_border_war` (`FUN_140335500`) does the same from an effect:**
  1. `FUN_140bebf30(null theatre, &X, &units, 0)`;
  2. `FUN_140bf5a20(army, leader, 1)`;
  3. the leader's vtable slot 25.

  It does no command power charge and no UI call. The loader would follow exactly this sequence.

**`create_unit` (`CCreateUnitEffect`, execute = slot 13, `FUN_140352140`):**
- **Location:** chosen per division by `FUN_14036aa00`, honouring `prioritize_location`,
  `allow_spawning_on_enemy_provs` and `country_score`. The choice uses the scope's synced random
  numbers (the same hash as NOTES F9). On failure it logs "pBestLocation was not found
  [reason] %s".
- **Creation:** a new `CArmy` (0x688 bytes); the `division` string is parsed into it at execute
  time ("error reading army data"); then it is registered (`FUN_1406d2ca0`, `FUN_140bb9220`).
- **Return value:** none. The new units are not returned. A before/after comparison of the
  country's division list (or X's) identifies them; whether they are appended at the end is still
  to be confirmed.

### G1.3 Rules the game enforces

- **Division caps are soft:**
  - `NMilitary.CORPS_COMMANDER_DIVISIONS_CAP = 24`, `FIELD_MARSHAL_DIVISIONS_CAP = 24` and
    `FIELD_MARSHAL_ARMIES_CAP = 5` (0 = unlimited, < 0 = blocked);
  - going over is allowed. "Commanding too many divisions will reduce all positive Commander
    Trait and Skill modifiers" (`LEADING_TOO_MANY_DIVISIONS_DESC`; armies:
    `ARMY_AMOUNT_PENALTY`);
  - `GARRISON_ORDER_ARMY_CAP_FACTOR = 3.0` raises the cap for garrison orders.
- **Leader change refused** (`FUN_140bebaa0`) while the army's HQ is deploying
  (`CANNOT_CHANGE_LEADER_DEPLOYING`, `+0x199` or `+0x19a`) or withdrawing (`…_WITHDRAWING`,
  `+0x19b`), and with `CANNOT_CHANGE_LEADER_WAR` when `army+0x1b0` is set and the army has a
  leader. Order in the function: war, deploying, withdrawing; army groups always may.
- **`army+0x1b0` is the army's border war** (identified 2026-10-22, vanilla 1.19.3 copy; the
  function's layout is the same on 1.19.3 checksum-patched and 1.19.2):
  - **Every reader the game has gives it a border-war meaning.** Besides `FUN_140bebaa0`
    ("Army is in a border war"):
    - the unit move check `FUN_140bfa300` refuses with `UNIT_BORDER_WAR` ("$STATE$ is in a border
      war.") when the unit's army (`unit+0xc0`) has `+0x1b0` set;
    - `start_border_war` (`FUN_140335500`), choosing divisions for a new border war, skips units
      whose army has a `+0x1b0` object, unless that object's byte `+0x52` is set.
  - **What it points to:** an object with two sides (`+0x28`, `+0x30`). Each side holds an army
    handle at `+0x4e8` and a value at `+0x4f0`. `FUN_140bef440` (the army's commander skill)
    takes that value from the side matching the army instead of from its general;
    `FUN_140beb770` derives a ratio from it for the UI.
  - **Lifetime:** the army constructor (`FUN_140be9c10`) sets it to 0.
  - **Not found:** the store that sets it. No direct write exists in the army code or in
    `start_border_war`. It is likely a small generic setter: there are several `+0x1b0` /
    `+0x1b8` pair setters, such as `FUN_14126c7f0`. The in-game test 15 (G2) confirmed it
    end-to-end (2026-11-10, 1.19.4.2): an army in a vanilla border war is refused as "in a border
    war".
  - `create_army` reads these fields only to name the refusal (G2); the refusal itself is
    `FUN_140bebaa0`.
- **Leader eligibility:** the UI lists the country's army leaders. The engine checks for:
  - an army leader: `leader+0xe7c < 2`; a field marshal can lead a single army (the AI avoids
    it, `ARMY_LEADER_ASSIGN_FIELD_MARSHAL_TO_ARMY = -500`);
  - the same country;
  - not captured.

  A general who already leads an army is moved, and his old army keeps its divisions without a
  general.
- **Command power:** recruiting costs `NPolitics.ARMY_LEADER_COST = 5` × leaders of that type
  (max 80). Assigning through the UI costs the 0x249 modifier value; `start_border_war` charges
  nothing.
- **Units:** same tag as the army (asserted), no railway guns (asserted). `create_army` would add
  only divisions it spawned for the leader's country.
- **Location:** everything `create_unit` does: own provinces first, then friendly ones, enemy
  only with `allow_spawning_on_enemy_provs`.

### G1.4 Behaviour

- **AI countries reorganise armies.**
  - `ai_front.cpp` (`FUN_14106c700`, `FUN_14106cdf0`) reassigns divisions to armies per front
    ("Front %s: reassigned %i units to army %i") and creates and edits army groups.
  - AI leader assignment (`ARMY_LEADER_ASSIGN_*` defines) can swap generals.
  - No per-army lock flag was found. For an AI country a spawned army is a starting arrangement
    the AI may reshape.
- **Player armies** are not touched by that code. To check in game.
- **Multiplayer.**
  - Every call is game-state code that vanilla's own `start_border_war` effect already runs on
    all machines. Spawning uses the scope's synced random numbers. New army ids come from the
    shared counter, so they stay identical when every machine executes the same effects in the
    same order (lockstep).
  - The loader must skip the UI refreshes that `CSetArmyLeaderCommand` makes.
- **Saves:** armies, their members and their leader are the same objects the UI creates, so the
  game saves them. To check in game (save, reload, army and general still there).

### G1.5 Proposal (option A approved 2026-10-18)

**Approved:** option A, with the decisions G2 lists: no command power, refuse a busy general
unless `reassign = yes`, and only `leader` / `name` / `reassign` / `country` / `create_unit`
inside. C (`add_to_army`) follows as a separate step; B was dropped; D comes later. The
proposal as made:

See the report in the conversation of 2026-10-18: options A (block with vanilla `create_unit`
children), B (`division = { template count }` sugar), C (`add_to_army` in division scope),
D (army group / field marshal), feature and guard `create_army`.

### G2: implementation and in-game tests (2026-10-18/19)

Implemented as option A (`create_army` block with vanilla `create_unit` children; decisions:
no command power, refuse a busy general unless `reassign = yes`, only `leader` / `name` /
`reassign` / `country` / `create_unit` inside). Engine access: `src/engine_army.cpp`, resolved as
in G1.2; signatures and code shapes checked on 1.19.3 vanilla and checksum-patched and on 1.19.2.

**First in-game run: nothing spawned (fixed).**
- **Cause:** vanilla `create_unit` needs a **state** scope. Its execute reads the state at
  `scope+0xa8` and spawns in that state's provinces; without one it logs "invalid scope state"
  (`FUN_140352140`). Vanilla always uses it as `37 = { create_unit = { ... owner = ROOT } }` or
  in `capital_scope`.
- **The G1 proposal was wrong here:** it put `create_army` in country scope, so the children ran
  without a state, and the division list comparison correctly found nothing.
- **Its tooltip (slot 7, `FUN_14037dba0` -> `FUN_14036af20`) needs the state too.** It logged
  the same error every frame while hovered.
- **Fix:** `create_army` is used in a state scope like `create_unit`. Its children run there; the
  army's country is the new `country` parameter, ROOT by default.

**Tests 1-10 (Germany): all as expected.**
- Armies A (6, Frankfurt), B (30 over Brandenburg, soft cap shown), C (two `create_unit` blocks,
  field marshal) and the reassign case.
- Every failure case: busy, unknown, unrecruited, admiral, French general, army group, no
  divisions, key not allowed inside. Each showed a red preview line where expected, spawned
  nothing, wrote a loader.log line and did not crash.

**Preview:** the red line reflects the moment of hovering. A decision that recruits the general
earlier in the same effect block previews red but works (seen in the Italy test). Vanilla
`create_unit` adds its own line ("Create 10 Italian unit in Lazio").

**AI (test 11, Italy, 1936):**
- **Creation worked:** the army existed with 10 divisions under the test general.
- **The general was moved at once** to guard the Ethiopian front.
- **The divisions stayed together** for a while, and the AI added others to that army.
- **After about a month** everything was split into the AI's own armies: mostly 1-4 divisions,
  plus a few of 10-22.
- **Later regrouping** followed its theatre changes: 3 theatres by May, then Europe and Africa,
  then 6 after the Ethiopian war (October 1936).
- **Conclusion:** for AI countries the army is a starting arrangement only; no vanilla lock
  exists (G1.4). MODDING.md says so.

**Test 12, save and reload: character lookup failed (fixed).**
- **Symptom:** after loading a save, every `create_army` said "there is no such character", even
  for General A, who existed and led Army A.
- **Cause:** recruit_character's lookup (`FUN_1406b4eb0(gamestate+0x6a8, token)`) found nothing
  after a load. The armies themselves survived the save with their names, divisions and generals.
- **Fix:** the general is now looked up among the army country's own characters, as
  `has_character` does:
  - `CHasCharacterTrigger` slot 22 calls its target resolver (+0x38), which falls back to
    `FUN_1405434d0`;
  - that function calls `FUN_1406cf250(country)` (the country's characters), then
    `FUN_1410eb910(characters, token, true)`;
  - code shapes `kCodeHasCharacterEval` and `kCodeCountryCharacterLookup` match on all four exe
    copies.
- **The global table** is only asked when that lookup fails, to say why (not recruited / another
  country). After a load it may not know either, and the reason then reads "not a character of
  this country".

**Test 12 again, with the fix: passed.**
- After loading, the previews read correctly, and reassign worked twice: General A moved to a
  new army A2, then on to the next one.
- A field marshal could be taken from his army with `reassign = yes`.
- After a second load everything still worked.
- The two failure tests stay available, which is intended (the decisions repeat) and does
  nothing:
  - "create_unit creates nothing": a normal preview (the general is fine, and the template is
    only checked when `create_unit` runs). When taken, nothing spawns, loader.log says so and
    error.log has `create_unit`'s own reason.
  - "a key that is not allowed inside": the use is invalid, but its preview still showed the
    vanilla `create_unit` line. Now an invalid use shows no preview lines at all (checked in game:
    the decision has no effect tooltip).

**Test 13, without the loader (2026-10-22, `version.dll` renamed): passed.**
- The same save loads; the "HOI4 Loader: Armies" category is hidden (the detect stub clears the
  guard trigger), and the armies made by `create_army` stay as they were.
- error.log: `create_army` is an unknown effect once per use (13 uses, 13 errors), the 5
  `recruit_character` warnings, the unknown native commands of the other test decisions, and the
  known lobby `remotefile.cpp:238` lines. Nothing else new.

**Test 14, captured: passed.**
- No war is needed: vanilla `capture_by` (character scope, as the game's own debug decisions use
  it) and `release_from_captivity`. The test mod has "France captures General B", "fail, General B
  is captured" and "release General B".
- With General B captured, every `create_army` naming General B shows "Loader Test General B cannot
  lead a new army: captured. Nothing is created." in red (checked on decision B).
- **Found while testing:** a refused `create_army` still showed the vanilla `create_unit` lines
  under the red line ("Create 30 German unit in Brandenburg"), although nothing is created.
  Now the tooltip callback can call `TooltipText::hideChildren()`, and `create_army` does when the
  general is refused: only the red line shows. Checked in game: refused decisions (captured,
  already leads an army) show only the red line; reassign still shows its `create_unit` line;
  after the release, decision B shows its normal lines again.

**Still open:** multiplayer (G3). Test 15 passed (below).

**Refusal while the general's army can't change leader** (checked for the PR, 2026-10-22):
`ReadLeader` calls the engine's own check `FUN_140bebaa0(army, nullptr)` (found through
`kCodeSetLeaderCheck`) on the army the general leads now; it does not read `+0x199` / `+0x19b`
itself. The game's assign-general command (`CSetArmyLeaderCommand` can-execute,
`FUN_14184efb0`) runs that check on both armies:
- **The general's current army** (`leader+0x1048`): the same call as the loader.
- **The target army:** for a new army it always passes. The war case needs a leader at `+0x88`,
  `+0x199` / `+0x19a` / `+0x19b` are not set, and the HQ check after it (`FUN_140bf2020`) needs a
  leader and a deployed HQ.

So `create_army` refuses exactly where the button would: HQ deploying, HQ withdrawing, and the
army in a border war (`army+0x1b0`, G1.3).

**Reason text per case (fix before the merge, 2026-10-22).** The red line used to say "deploying
or withdrawing" for all three. Now, after `FUN_140bebaa0` refuses, `WhyNot` reads the same fields
in the function's order, only to choose the message:
- **Offsets:** taken from the function's own code (`kCodeCanChangeStart` / `Deploying` /
  `Withdrawing`). They are cross-checked against the army group flag and the leader field found
  elsewhere, and are the same on all three builds.
- **If they are not found,** the generic reason is used and `create_army` still works.

| Key | English | Vanilla text it follows |
|---|---|---|
| `HOI4_LOADER_CREATE_ARMY_DEPLOYING` | their army $ARMY$ is currently being deployed | `CANNOT_CHANGE_LEADER_DEPLOYING` "$NAME$ is currently being deployed" |
| `HOI4_LOADER_CREATE_ARMY_WITHDRAWING` | their army $ARMY$ is currently being withdrawn | `CANNOT_CHANGE_LEADER_WITHDRAWING` "$NAME$ is currently being withdrawn" |
| `HOI4_LOADER_CREATE_ARMY_BORDER_WAR` | their army $ARMY$ is in a border war | `CANNOT_CHANGE_LEADER_WAR` "Army is in a border war" |
| `HOI4_LOADER_CREATE_ARMY_CANNOT_LEAVE` | their army $ARMY$ cannot change its general now | (fallback: fields not found) |

**Test 15, border war (passed 2026-11-10 on 1.19.4.2).** The test mod's "border war from Saxony, led by General
B" runs vanilla `start_border_war` (65 into 69) with a `leader_score` that picks General B. Then:
- check that the border war army's general is General B;
- hover "fail, General B's army is in a border war": the red line should end "their army <name>
  is in a border war. Nothing is created.";
- take it: nothing spawns, and loader.log has the same reason;
- "end the border war" cancels it.

If another general is picked, the test is inconclusive; note who.

**Result:** the border war ran with General B; it actually stopped the units ("the border war
stops actually stuff"). loader.log: `cannot lead a new army: their army "Army 3" is in a border
war; nothing created`. `loader_script_docs.md` lists `_DEPLOYING`, `_WITHDRAWING` and
`_BORDER_WAR`.

**1.19.4.2 in game (2026-11-10, DLL `62adfc741d48`):** `Supported hoi4.exe build:
1.19.4.2_vanilla`; all 10 features active, no errors; every feature self-check passed; create_army
resolved the same fields as on 1.19.3 (`leader change refused on: border war +0x1b0, deploying
+0x199 / +0x19a, withdrawing +0x19b`). Checked by the tester: army creation (30 divisions under
General B), reassign, the refusals, the region rename.

### G3: multiplayer test plan (open)

**Blocks the next published release (zip), not the merge of `feature/create-army`.** Nothing in
the loader has been tested in multiplayer yet (README). One session also covers D3 step 10.

**Setup:**
- Two players on the same game build (the same `hoi4.exe` build line in both `loader.log`s) and
  the same `version.dll` (compare its SHA-256), both with `hoi4_loader_test` enabled.
- The host plays Germany, the client Italy. New game, 1936.
- After each step, compare on both machines:
  - the army's name, its division count, its general, and the province of its divisions;
  - the `create_army (...)` line in both `loader.log`s (every machine runs the effect, so the
    lines must be identical);
  - no out-of-sync warning in game.

**Steps:**
1. **Session classes (D3 step 10, part 1).** Host `loader.log`: "Lifecycle: game entered (...;
   session: CNetworkServer, multiplayer)". Client: the same with `CProxyServer`.
2. **The host takes a decision.** "Armies: prepare", then "Armies: A (6 divisions at Frankfurt)".
   Loader Test Army A has 6 divisions at Frankfurt under General A on both machines.
3. **The client takes a decision.** "Armies: prepare Italy (AI test)", then "Armies: give Italy
   an army (AI test)". Loader Test AI Army has 10 divisions in Lazio under General AI on both.
   Italy is human here, so the AI does not rearrange it.
4. **`reassign = yes`.** The host takes "Armies: reassign General A". Army A2 (2 divisions) is
   under General A, and Army A keeps 6 divisions without a general, on both machines.
5. **A refused case.** The host takes "Armies: fail, general already leads an army". Nothing
   spawns on either machine, and both `loader.log`s have the same refusal line.
6. **Hot join** (`DAT_143087260`, the army handle counter, G1.2):
   - the client leaves;
   - the host takes "Armies: C (two provinces, field marshal)" and keeps the game running;
   - the client hot-joins at once;
   - check: joiner's `loader.log` has "..., HOT JOIN" in the game-entered line, and Army C is
     the same on both;
   - then the host takes "Armies: B (30 divisions, spread)". B must be the same on both, with no
     OOS, for at least a week of game time;
   - give Army B and Army C a move order on the host and watch them move on the client. A
     counter that differs would give different army handles, and orders would go astray or
     cause an OOS.
7. **Save and reload in multiplayer:**
   - the host saves, and both quit to the menu;
   - the host loads that save in a multiplayer game, and the client joins;
   - every army from steps 2-6 is unchanged on both machines;
   - the host takes "Armies: reassign General A" once more; it must be the same on both, with no
     OOS.
8. **on_game_loaded (D3 step 10, part 2).** After step 7, both `loader.log`s have "on_game_loaded
   NOT fired: ... (this version fires in single player only)" and no "on_game_loaded fired". On
   the hot join (step 6) it must not fire either.

**Pass:** steps 1-8 as described, no OOS, no crash. Then MODDING.md's "Not tested in
multiplayer yet" for `create_army` and the README's multiplayer note can be updated.

## Part G4: `add_to_army` (option C, investigation)

Branch `feature/add-to-army` (from `main` after the merge of #1). Investigation only, nothing
implemented. Addresses are from vanilla **1.19.4.2** (`hoi4_11942_copy.exe`) and only for
orientation. The 1.19.3 and 1.19.2 copies are no longer on disk (Steam replaced the exe), so the
new code shapes can't be checked on them yet (G4.5).

Option C as proposed on 2026-10-18: `add_to_army = { leader = <character> }` in division scope
(e.g. inside `every_country_division`). The division joins the army that general leads; a new
army is formed if the general leads none.

### G4.1 How the game moves a division into an existing army

**The UI action** ("assign the selected divisions to this army"; texts `CONFIRMASSIGNUNITS`,
`CONFIRMASSIGNUNITS_DISBAND_ARMY_DESC` "After the unit assignment $TO_REMOVE$ will be
disbanded", `COMMAND_GROUP_CANT_REASSIGN`) is **`COrderAssignCommand`** (vtable `0x142a84ce0`).
- `CMergeArmiesCommand` is not it: in this engine a `CArmy` is a division, and that command
  merges divisions.
- `COrderGroupCommand` only ever creates a new army (create_army, `FUN_140c417a0`).

**`COrderAssignCommand` execute (slot 10, `FUN_1418d4220`):**
- target group at `cmd+0x40`; unit handles at `cmd+0x28` (count `+0x34`); flag `+0x4c` = also
  assign the units to the target's front orders;
- a target army group spreads the units over its armies; a target army takes them all;
- per unit: resolve it, check its tag (`unit+0x1d8`) against the group's (`+0x3c`, or
  `FUN_140c085f0`), remember its old army (`unit+0xc0`), then call
  **`FUN_140c45630(target, unit, 1, 1)`**; with the flag, also the front order assignment;
- at the end, **`FUN_140f70840(oldArmy)`** for every old army.

**Can-execute (slot 9, `FUN_1418de970`)** refuses only:
- "#~ invalid unit reference" (a handle that does not resolve);
- **"#~ order group is involved in a border war"**: the unit's current army has `+0x1b0` set
  (the same field as G1.3);
- "#~ invalid order group reference".

It does not check the target army for a border war, deploying or withdrawing.

**The move, `FUN_140c45630(army, unit, skipIfMember, flag)`** (`COrdersGroup` code):
- asserts `IsArmyGroup() == false` and the tag match ("Very important, please slap programmer
  if that happenes");
- with `skipIfMember`, a unit already in the member list (`army+0x50`, count `+0x5c`; entries
  are `unit+0xb8`) only refreshes the army;
- otherwise:
  - `FUN_140c4cc70(oldArmy, unit, flag)` ("COrdersGroup::UngroupMember") takes it out of its
    old army and refreshes that army;
  - appends `unit+0xb8` to the members, sets `unit+0xc0 = army`, and notifies the member
    sub-object (its vtable slot 0);
  - sets the army's dirty bytes `+0x19e` / `+0x19f`, recomputes the army (`FUN_140c411c0`,
    `FUN_140c40eb0`, `FUN_140c49660`) and refreshes its general if it has one
    (`FUN_140c77be0`).

**The cleanup, `FUN_140f70840(army)`:**
- does nothing unless the army has no members left (`+0x5c == 0`);
- then it removes the army from its manager (`army+0x40`; lists `+0x80` and `+0xb0`,
  `FUN_140f7f4d0`) and deletes it (vtable slot 0). This is the "will be disbanded" of the UI
  text.

**All callers of the move pass `(..., 1, 1)`** (bytes `41 B1 01 45 0F B6 C1` before each call):

| Caller | What |
|---|---|
| `FUN_140c417a0` | create_army (the loader's `CreateArmy`); cleanup `FUN_140f70840` per old army |
| `FUN_1418d4220` | `COrderAssignCommand` execute; the same cleanup |
| `FUN_14074b170` | OOB loading: empty army (`FUN_140f702d0`), move each unit in, then set_leader (`FUN_140c4b460`, which `create_army` uses) |
| `FUN_1411cb9e0`, `FUN_1415bdb80`, `FUN_14164d920` | not examined (AI or other game code) |

So "move into an existing army, then delete the old army if it is empty" is the sequence the
game itself uses from three places. Two of them are game-state code without any UI.

### G4.2 Division scope

- The scope's unit handle is at `CEventScope+0x70` (NOTES "Scopes", mask 0x100).
- Vanilla division-scope effects read it through a getter. `destroy_unit`
  (`CDestroyUnitEffect` slot 13) calls `FUN_14057cba0(scope)`: if `scope+0x70` is set,
  `resolve(scope+0x70) - 0x10`, else null. That is exactly the loader's resolver
  (`FUN_142284870`) and `kUnitRef` (0x10).
- A unit scope could in principle hold a non-land unit. The loader already resolves the `CArmy`
  vtable (`engine_data.cpp`, RTTI `.?AVCArmy@@`), so "is a land division" is an exact check.

### G4.3 How the loader would find them

- **Move and cleanup:** inside create_army (already resolved as `g_createArmy`):
  - the call after `41 B1 01 45 0F B6 C1 49 8B D4 4C 8B 74 24 ?? 49 8B CE E8` is the move;
  - the call in the "old armies" loop after it is the cleanup;
  - cross-check: `COrderAssignCommand` slot 10 calls the same two functions.
- **Division getter:** the first call in `CDestroyUnitEffect` slot 13. Its body
  `48 83 EC 28 48 83 C1 70 83 39 00 75 06 83 79 04 00 74 ?? E8 ...` gives `+0x70` and must call
  the resolver.
- **Border war field:** already resolved (`g_borderWar`, from can_change_leader). The
  can-execute above reads the same `+0x1b0` (cross-check).

### G4.4 Proposal

```
every_country_division = {
    limit = { ... }                      # vanilla division triggers
    add_to_army = {
        leader = GER_erwin_rommel        # character id (required)
        name = "1. Panzerarmee"          # optional: only used if a new army is formed
    }
}
```

**Behaviour.**
- **The division joins the army its general leads**, through the game's own move (G4.1). If its
  old army is left empty, that army is deleted, as the UI does ("will be disbanded"). Its
  general, if it had one, then leads nothing.
- **General leads no army:** a new army is formed from this division (create_army's
  `CreateArmy` plus set_leader, as now), named `name` if given. The next divisions of the same
  `every_country_division` then join it.
- **Already in that army:** nothing to do, no log line.
- **Front orders:** the division gets none of the army's front or other orders (the UI's `+0x4c`
  option is off). It joins the army without orders. To check in game.
- **Command power:** none (the UI charges none for this either).

**The general** must meet the same conditions as for `create_army`:
- an army leader (general or field marshal);
- recruited by the **division's owner**;
- not captured;
- not leading an army group.

**Refusals.** Nothing happens to that division; `loader.log` gets one line, and the tooltip shows
a red line.

| Case | Reason |
|---|---|
| The scope is not a land division | "not a division" |
| The general is unknown, not recruited, serves another country, is not an army leader, is captured, or leads an army group | as for `create_army` |
| The division's current army is in a border war | "its army $ARMY$ is in a border war" (the UI's own refusal) |
| **Proposed, stricter than the UI:** the general's army is in a border war | "their army $ARMY$ is in a border war" |

The last row is a choice. The UI lets divisions join an army that is in a border war. I'd refuse
it, so that a script can't change the strength of a running border war.

**Tooltip:**
- "Joins the army of **Erwin Rommel**.";
- "Forms a new army under **Erwin Rommel**." when the general leads none at hover time;
- otherwise the red line.

Inside `every_country_division`, vanilla shows the child lines once.

**Feature and guard:** a feature of its own, `add_to_army` (engine tier, needs `create_army`'s
engine part), with the guard `hoi4_loader_feature_add_to_army` and a stub. It is then off on its
own if its extra code shapes are not found, while `create_army` keeps working.

**Effort and risk:** about 2 days. The risk is low: it only calls functions the game already
calls from game-state code (create_army, OOB loading), and uses the UI's own refusal.

**AI:** as for `create_army` (G2, test 11). The AI regroups its own armies, so for AI countries
the result is only a starting arrangement. Player armies are not touched.

**Multiplayer:** it is an effect, run by every machine in the same order; the move and the
cleanup are deterministic game-state code; there is no UI call. The G3 session covers it with one
more step.

### G4.5 Decisions and implementation (2026-11-11)

**Approved** as proposed in G4.4:
- the stricter rule: a division may not join a general's army that is in a border war;
- a feature of its own, `add_to_army` (needs `create_army`), guard
  `hoi4_loader_feature_add_to_army`, with a stub;
- `name` is only used when a new army is formed; no front orders; an emptied old army is
  disbanded as the UI does.

**Implemented** (branch `feature/add-to-army`, rebased onto `main` after #2 and #5):
- **Engine** (`engine_army.cpp`, `ResolveJoin`, resolved after create_army's pieces):
  - the move and the cleanup from create_army (`kCodeCreateArmyMove`, `kCodeCreateArmyCleanup`),
    cross-checked: `COrderAssignCommand` slot 10 calls both;
  - inside the move: the unit's tag (`kCodeArmyMoveTag`, must equal the division owner offset
    from `kSigStateLandUnits`), the army's member list and count (`kCodeArmyMoveMembers`), the
    unit's army field read, compared and set at the same offset (`kCodeArmyMoveUnitArmy`); the
    count must equal the cleanup's (`kCodeArmyCleanup`) and list + 0xc;
  - the division getter from `CDestroyUnitEffect` slot 13 (`kCodeScopeDivision`): scope `+0x70`
    (the field `Scope::types` reads), the loader's resolver, `-0x10` (`kUnitRef`);
  - the CArmy vtable (RTTI), so only land divisions are accepted; and the border war field from
    can_change_leader (`g_borderWar`), now required.
  - On 1.19.4.2: move 0x140c45630, cleanup 0x140f70840, getter 0x14057cba0; division army
    `+0xc0`, member entry `+0xb8`, army tag `+0x3c`, members `+0x50` (count `+0x5c`).
- **`JoinArmy`** checks what the move only asserts (not an army group, same tag), skips a
  division already in the army, calls `move(army, division, 1, 1)`, checks the division's army
  field, then `cleanup(old)` (deletes the old army only if it has no members left).
- **Plugin** (`plugin_create_army.cpp`, shares create_army's general check): the general's check
  with "already leads an army" allowed; then the division's army in a border war ("its army ...")
  and the general's army in a border war ("their army ..."). A general without an army: create_army's
  `CreateArmy` with this one division, then the name and set_leader.
- **Self-check on game entry:** every division of the player's country that has an army is in that
  army's member list, and the army carries the country's tag. Otherwise `add_to_army` is switched
  off for the session (`create_army` stays).
- **Logging:** the same line for many divisions (an `every_country_division` with an unavailable
  general) is logged once, then "(the previous line N more time(s))".
- **Test mod:** vanilla has no division trigger for a template name (`has_template` is a country
  trigger; error.log: "has_template: Invalid Scope, supported: Country, provided: Unit"), so the
  Join decisions pick any of the country's divisions. Found in the first game test, where every
  `limit` was false and no Join decision did anything.
- `tools/check_code_shapes.py` (`check_add_to_army`): all ok on 1.19.4.2. Smoke test, unit
  tests, signature check and stub check pass. No new signatures (the count stays 84).

### G4.6 In-game test (passed 2026-11-11)

Germany 1936, test mod, single player. Take "Armies: prepare", then Army A, B and C, then
"Join: prepare" (recruits Loader Test General C). After each step, look at the army list (the
divisions and armies on the left) and `loader.log`.

1. **Load:** `loader.log` has the `add_to_army: move ..., cleanup ..., division getter ...` line
   and, on game entry, "add_to_army: self-check passed".
2. **New army:** "Join: one division forms Army J under General C". Tooltip "Forms the army Loader
   Test Army J under Loader Test General C." Then one division is in a new army "Loader Test Army
   J" led by General C (a random German division; if it was in an army, that army has one
   division fewer).
3. **Join:** "Join: three divisions join General C". Tooltip "Joins the army of Loader Test General
   C." Army J then has 4 divisions (fewer if a picked division was already in it: nothing happens
   for that one).
4. **Old army disbanded:** "Join: Nassau's divisions join General B". Every division in Nassau
   (Army A's, and Army A2's if "reassign General A" was taken) is in General
   B's army; Army A is gone from the list; `loader.log` says "...was disbanded" for its last
   division. Its general, if it had one, then leads no army.
5. **Failures** (the division stays; red tooltip line; one log line each): unknown character, an
   admiral, a French general; after "Armies: France captures General B", "Join: fail, General B
   is captured"; then "Armies: release General B".
6. **Border war:** "Armies: border war from Saxony, led by General B", then:
   - "Join: fail, General B's army is in a border war": the division joins General C, then the
     second line is refused ("their army ... is in a border war");
   - "Join: every division to General C during the border war": divisions of the border war
     army stay ("its army ... is in a border war", logged once and counted), the others join;
   - "Armies: end the border war".
7. **Orders:** a division that joined an army with front orders has none of them (expected; the
   UI's "also assign orders" option is not used).
8. **Save/load:** save after step 4, load: the armies are as before; the self-check passes.
9. **Without the loader** (`version.dll` renamed): the save loads with the same armies;
   `error.log` has "Unknown effect" for `add_to_army` when parsing the test mod; the Join decisions
   are hidden.

**Result (2026-11-11, 1.19.4.2, single player):** after the test mod fix in G4.5 (no
`has_template` in division scope), "everything works as expected" in the in-game test.

### G4.7 Open

- **Older builds:** the new code shapes must be checked on 1.19.3 (vanilla and checksum-patched)
  and 1.19.2. That needs copies of those exes (Steam's beta branch selector). Until then the
  feature stays off on those builds unless its shapes match (fail safe), like everything else.
- **Multiplayer:** G3 gets one more step: an `every_country_division` with `add_to_army` from a
  decision, then compare the armies on both machines (no OOS).

# Part H: railways (`downgrade_railway`)

## H1 findings (1.19.4.2 `v1.19.4.2.2da5` in Ghidra 12.2; addresses for orientation only)

Static analysis only; nothing here was observed at runtime yet.

### Storage

- `CRailwayManager` at gamestate+0x3e0 (constructed in the gamestate setup 0x140202a40, right after
  `CSupplySystem` at gamestate+0x3d8). Its `CProvinceRailwayInfo*` array (+0x08, size +0x14) is
  indexed by province id and sized to the gamestate's province count (+0x2bc).
- `CProvinceRailwayInfo` (0x70 bytes, created on demand by 0x140f05530):
  - +0x08 the map province (neighbours at +0x70, stride 0x30, id at +8, count +0x7c; the
    engine's `NeighborIndex(province, id)` 0x1414a6290 searches them);
  - +0x18 a `rail_way` building (created on first use) that mirrors the railway level;
  - +0x20 `_RailwayLevelsToNeighbors`, int per neighbour index;
  - +0x38 sorted `{ int neighbour, int level }` pairs, count +0x44, only levels > 0
    (the loader 0x140f07ed0 skips 0 when reading a save);
  - +0x50 `RailwaysInConstruction`, sorted 16-byte `{ neighbour, pad, int64 progress }`.
- Every railway is stored on both ends; every engine path updates both.

### Engine functions: all raise only

- `SetRailway(info, neighbour, level)` 0x140f08540: asserts `Level > 0`, stores
  max(current, level) clamped to `NSupply::MAX_RAILWAY_LEVEL`, updates the pair and sets the mirror
  building's level (`CBuilding::SetLevel` 0x141163f20) to the new level.
- `AddRailwayLevel(info, neighbour, delta, clearConstruction)` 0x140f055b0; the path variants
  0x140f05780 (`build_railway`, CBuildRailwayEffect slot 13 0x1403663e0) and 0x140f08840 (loading
  `map/railways.txt`) both end with `gamestate->supply->vtable[3](province)` per path province.
- `CSupplySystem` slot 3 (0x140f46c80): looks the province up and marks it and its railway
  neighbours dirty (0x140f41950 / 0x140f41ae0). The supply system keeps its own per-province
  `has_rail` / `province_dirty_railway` state (its OOS checksum 0x140f42ad0 lists them), so this
  call is required after any change.
- `remove_building` (CRemoveBuildingEffect slot 13 0x140327740) calls `SetLevel(level - 1)` on the
  mirror building only: the levels and pairs keep the railway, which is why it changes nothing.

### Construction (for a later queued dismantle)

- `CBuildRailwayCommand` execute (slot 10, 0x1411dc890): `from = min(level on the path)`; nothing
  if the target is not above it; else `CreateRailwayLine(country+0xf68, path, from, to, flag)`
  0x140ed1780 (allocates a 0xb0-byte `CRailwayProductionLine`, base level +0x78, target +0x7c,
  path +0x88 / count +0x94).
- Daily progress: `CRailwayProductionLine` slot 24 (0x141a944c0) works the first link below the
  target through `AddConstructionProgress` (0x140f059d0, cost `((level+1) * per_level_extra_cost +
  base_cost) * 100000`), carries overflow to the next link, completes the line when none is left.
- `CChangeRailwayConstructionLeveLCommand` (slot 10 0x1411df7d0 -> 0x141a93ce0) clamps the target
  to `[base, MAX]`; that is the UI's "already at current level".

## H2: `downgrade_railway`

- **Path:** breadth first over links with level > 0, neighbours in the pair order, so the path
  with the fewest links is the same on every machine. The search uses per-thread work arrays
  (grown once, reset by a generation stamp), so the trigger `has_railway_between` can run it from
  AI threads without allocating; `min_level` only skips links below that level.
- **All routes:** `all_routes = yes` repeats "find the shortest route, remove it completely" until
  none is left (each round removes at least one link), then refreshes supply and graphics once.
- **Lowering one end:** `levels[NeighborIndex]` = new level; the pair updated, or erased (memmove,
  count - 1) at 0; the mirror building set to the highest remaining level of that province (so
  `has_railway_level` follows). Construction progress (+0x50) is left alone.
- **Refresh:** `CSupplySystem` slot 3 once per affected province, as `build_railway` does.
- **Resolution** (7 signatures, engine_railway.cpp): offsets are read from the instructions and
  cross-checked between sites (gamestate global in two places, the supply offset from the setup
  and from the refresh, each SetRailway field read and written, the three SetRailway parts in one
  function, the neighbour lookup called there). The manager, supply system and info objects are
  checked against their RTTI vtables before every use.
- **Self-check** on game entry: every stored pair must match the level array and its other end
  (up to 2000 links); otherwise the feature deactivates.

### To verify in game

1. A 1936 save: "lower by one level" on Brandenburg (6521) to West Prussia (6282) shows the
   railway one level thinner in the logistics map mode; `loader.log` says 3 links lowered.
2. "Remove it": the railway disappears; supply in the area changes as it would without it.
3. Save and load: the change persists and the self-check passes.
4. `has_railway_connection` (the decisions' `available`) turns false after removal.
5. "Rebuild": vanilla `build_railway` restores it.
6. The signatures on 1.19.3 (`tools/check_signatures.py` on both listed builds).

### In-game results (2026-11-09, 1.19.4.2 as a local test entry in `loader_config.ini`)

- **Startup:** every railway field resolved to the H1 offsets; `downgrade_railway` registered;
  self-check passed (2001 links consistent on both ends).
- **Effect:** "lower by one" lowered the 3 links of 6521-6282 each use; "remove it" took the
  railway off the map (Berlin to Stettin); "no path" changed nothing and logged it; after a
  removal or rebuild the shortest remaining railway can have more links (4 once), as designed.
- **Save and load:** the removed railway stays removed after loading.
- **Not yet checked:** supply values before and after, `has_railway_connection` turning false,
  multiplayer, and 1.19.3 (item 6).

## H3: `queue_railway_dismantle` (construction queue)

### Findings (1.19.4.2)

- **Create:** `CreateRailwayLine(country->construction (+0xf68), CPdxArray<int>* path, from, to,
  insert)` 0x140ed1780 allocates the line, `Init` (0x141a960e0) copies the path to +0x88 (count
  +0x94) and stores base +0x78 and target +0x7c without any clamp, then appends it to the
  construction's line list (+0x70, count +0x7c). `insert` 0, 2 and 3 reorder the queue
  (0x140ee2b30); 1 leaves it at the end.
- **Save:** the line writes path, base (0x2c86), target (0x6b), current link +0xa0 (0x2f22),
  remaining cost +0xa8 (0x3b1e) and the cancelled flag +0x80 (0x28e5); loading reads them back
  unclamped (slot 4 0x141a96040).
- **Per day** (`production.daily_serial`, 0x140ed8390): for each construction line, slot 24 (daily
  step, 0x141a944c0), then slot 25 (finished, 0x141a958c0); a finished line is deleted.
  - Slot 24 resets +0xa0, checks the path (`PathUsable(tag*, first, last)` 0x14157bca0, else
    jmp slot 27 = cancel, which sets +0x80), then for each link: `ProvinceUsable(tag*, next)`
    0x14157bf10; the first link below the target gets the day's output (slot 14 reads +0x30)
    through `AddConstructionProgress`. It ends with slot 10 (0x141a96160): progress recompute
    (0x141a961b0), then the output bookkeeping.
  - Slot 25: cancelled, or every link at the target.
- **Progress** (0x141a961b0): remaining +0xa8 = cost still to build minus progress in the
  railway infos' +0x50 entries; done +0x38 = total +0x28 - remaining; then slot 17 (+0x88)
  (line, levels still to build) stores that count at +0x48 (the level command removes a line
  whose +0x48 < 1) and refreshes the UI.
- **Cost of a level:** `Cost(int64* out, level, country->tag holder (+0x2a8))` 0x140f05f90 =
  `(level * per_level_extra_cost + base_cost) * 100000` plus the holder's railway cost modifier.

### Design

- **Marker:** target < base. Vanilla never creates it (the build command needs target > the
  path's lowest level; `SetTarget` clamps to >= base) and it survives save/load as is.
- **Hooks** (MinHook): slot 24 (start found by signature, checked == vtable slot 24), the progress
  recompute (signature), slot 25 (vtable slot, code shape). Lines without the marker go to the
  originals.
- **Daily step:** the engine's own path and province checks (cancel if they fail); the first link
  above the target is "current"; `remaining` (+0xa8, saved) minus today's output; while the
  remaining work is no more than the levels after the next one cost, lower that link one level
  (`DowngradeRailway`). Then slot 10, as the engine does.
- **Cost:** a level costs `Cost(level) / 3`.
- **Progress:** remaining as above (reset to the cost still left when it is 0 or more than that:
  a new line, or levels changed by something else), total = max(total, remaining) (the total is
  not saved), done = total - remaining, slot 17 with the levels still to do, then the line's speed
  as the engine sets it: `LinkSpeed(line, &speed, province, next, 0)` (0x141a93d10) for the link
  being dismantled, stored by `SetSpeed` (0x140ff2830, asserts "vSpeed >= 0_fixed") at +0x30.
  - **Bug found in game (first build):** the hook replaced the whole recompute and never set the
    speed, so +0x30 stayed 0: factories were assigned (15/15) but the tooltip said "Never" and
    nothing progressed. +0x30 is the line's daily progress, not a plain output field.
- **Finished:** cancelled, or no link above the target.

### Queue wording

- The railway entry's tooltip builder `(item, element, out)` 0x141de2200 looks the line up with
  `LookupLine(item+8)` 0x142284870 and, for a line with base level > 0, localises
  `RAILWAY_UPGRADE_TOOLTIP` with RAILWAY_NAME, START_LEVEL (base) and TARGET_LEVEL (target) through
  `Localize` 0x1422aeab0 (base 0: `RAILWAY_CONSTUCTION_TOOLTIP`).
- The loader hooks the builder (thread-local "dismantle entry" flag while it runs) and `Localize`
  (with the flag set, `RAILWAY_UPGRADE_TOOLTIP` -> `HOI4_LOADER_RAILWAY_DISMANTLE_TOOLTIP` if the
  localisation has it). Everything else passes through.
- The entry's update (0x141ded950, `LookupLine(item+0xb50)`) sets the title with
  `SetText(item->+0x2178, std::string* temp copy)` 0x142347db0 (copies it to element+0x110), then the
  progress bar, the +/- buttons (vtable +0x298 enable / +0x2a0 disable) and `scorched_icon` (found
  by name through root vtable +0x88; +0x78 show, +0x80 hide with flag 0x10 at +0x75).
- Marker: while the update runs for a railway line, SetText on that title element gets the text
  with `£decrease_texticon£ ` (vanilla's 13x12 red down triangle; dismantle lines) or
  `£increase_texticon£ ` (green up triangle; the engine's lines) in front; a mod's
  `HOI4_LOADER_RAILWAY_DISMANTLE_PREFIX` / `_BUILD_PREFIX` replaces them.

### Map badges (`CRailwayMapIcon`, `railway_map_icon` in mapicons.gui)

- One per railway segment; fields: province objects +0x610 / +0x618, segment path +0x620
  (CPdxArray<int>), current level +0x63c, construction target +0x640, cooldown +0x644, elements
  text +0x658, bg +0x660, cooldown +0x668, under_construction +0x670 (found by name in 0x141993230).
- Left click 0x141992860: `Clickable(icon)` 0x141992220 (player may build here), modifier keys
  from the input object (gamestate UI +0x508, vtable +0x28/+0x30/+0x38; Shift = to the maximum,
  the others pick the queue position), then a synced command posted with 0x1422c4250: new
  `CBuildRailwayCommand` (0x1411cd870) or `CChangeRailwayConstructionLeveLCommand` (0x1411cdba0)
  for the existing line (`FindRailwayLine(path, tag)` 0x14157d360, path equal or reversed).
  Right click 0x141992b10 lowers a pending upgrade.
- Update 0x141992ce0: text = `Colorize(out, 'W' or 'O', level text)` 0x140245cf0 (writes the
  engine's colour code: byte 0x11 then the letter) -> `SetText` (takes the string and frees it).
- Loader: update hook replaces the text with a red X while Alt+Shift is held (and the player
  may build there and the segment has a railway); click hook with Alt+Shift held calls the plugin,
  which defers `QueueOrDeepenRailwayDismantle` to the next safe point (single player only). First
  Shift alone; changed after play testing, since vanilla's Shift+click (to the maximum) is still
  wanted in the same view. The click's modifiers (input vtable): +0x28 Shift (to the maximum),
  +0x38 Ctrl (queue position 0, first), +0x30 Alt (1, last), neither 2 (default); matched to keys
  by vanilla's `INSERT_CONSTRUCTION` text ("Ctrl+Click ... beginning", "Alt+Click ... end") and
  the check order. Alt+Shift with Ctrl up was picked as costing the least.
  - **Bug found in game (first build):** the X showed but clicks did nothing: the single-player
    test used `SessionAllowsLocalWrites`, which only answers inside a game-entered callback. Clicks
    now use `CurrentSessionSinglePlayer` (the session as classified when the game was entered).

### After the PR #2 review (2026-11-10)

- **When `railways` is switched off** (its self-check on game entry fails): the plugin calls
  `Engine::DisableRailways()` and deactivates `railway_queue` as well. From then on, for the rest
  of the session:
  - `downgrade_railway`, `queue_railway_dismantle` and Alt+Shift+click change nothing;
  - the badges are vanilla again (no cross, the click goes to vanilla);
  - a dismantle entry already in the queue **pauses**: its daily step and progress update do
    nothing and its finished check says no. So the engine neither runs it as an ordinary
    railway line nor removes it, and it stays in the queue and in saves for a later session
    where the feature works. `loader.log` says so once.
  - A later game entry in the same session does not switch it back on.
- **Thread of the daily step:** the first daily step of each session logs its thread and whether
  it is the main thread (`railway_queue: daily step on thread N (the main thread)`). Statically
  it runs from the production manager's serial daily update (`production.daily_serial`,
  0x140ed8390), which the main thread runs. The graphics rebuild that `DowngradeRailway` ends with
  now checks `IsMainThread()`: off the main thread it is deferred to the next safe point (logged
  once). Without the frame hook (no main-thread test) it runs where it is called, as before.
- **Fixed numbers now read from code:**
  - the output getter's vtable slot: the daily step calls it as `call [vtable+0x70]`
    (`kSigRailLineDailyOutput`, 0x141a94822; slot 14);
  - the badge text's flags byte and the mask that clears its hidden flag: the and-instruction in
    `kSigRailIconText` (`and byte [rcx+0x75],0xEF`), checked to clear exactly one bit.
- **Badge cross:** while Alt+Shift is held, the badge shows vanilla's removal cross
  (`GFX_cancel_conversion_button`, the sprite of the state view's cross) as a text icon. The red
  X stays as the fallback when the icon delimiter cannot be read.
- **`queue_railway_dismantle` where the country may not dismantle:** it still queues (the daily
  step cancels the entry, as before), but `loader.log` now says so when the effect runs. The
  effect's tooltip adds a red line (`HOI4_LOADER_QUEUE_RAILWAY_DISMANTLE_NOT_ALLOWED`). The test
  is the daily step's own: the game rules, or without the relation tests the build permission.
- **1.19.3:** no 1.19.3 copy is available here (only the current 1.19.4.2 `hoi4.exe`, and the
  1.19.4.2 copy analysed in Ghidra). So the railway signatures are unchecked on 1.19.3. A false
  match there would show as a failed cross-check or self-check, and the features would then stay
  off. That is not proven until `tools/check_signatures.py` and `tools/check_code_shapes.py` run
  on a 1.19.3 copy.

### To verify in game (review)

1. **A save with a dismantle entry, loaded without the loader** (`version.dll` renamed): what
   happens to the entry. Is it removed at once, and cleanly? Is there a notification? Is
   anything new in `logs\error.log`? Then once more with the loader and
   `railway_queue = 0` in `loader_config.ini` (the hooks are not installed, so the same as
   vanilla).
   *Tested 2026-11-10 without `version.dll`: the save loads fine and the dismantle entry is gone
   from the queue. Not yet checked: a notification, `error.log`, and the `railway_queue = 0`
   run.*
2. **`loader.log`:** the daily step's thread line says "the main thread".
3. **The badge:** the cross shows while Alt+Shift is held and fits the badge.
4. **Disallowed path:** `queue_railway_dismantle` on a path through another country's province
   gives the log line and the red tooltip line; the entry is cancelled on its first day.

## H4: crash after removing a railway (fixed)

- **Seen:** save, load, a dismantle step took link 9238-9294 to level 0, and one second later the
  game crashed: access violation reading 0 in the generic mesh draw (0x1422e7544, `mov rsi,
  [rsi+rcx*8]` with rsi = 0 and a non-zero count) under the map renderer (0x14130dd50, from the
  in-game render 0x140e4a140). Before the save, links taking provinces to no railway (9375, 526)
  had not crashed.
- **Railway graphics** (object at ui->vtable[0x80]()->+0x58->+0x5ce0): a hash table of pieces per
  province (+0x38, count +0x40); moving trains (+0xa8, count +0xb4; per frame in 0x14164a980,
  pathing in 0x141647440 "Found no valid TargetNodeIndex"); an entity table at +0x90.
  - The supply update hands changed provinces to 0x14164b150: a province with railways gets its
    piece rebuilt (0x141647150), one with none has it cleared (0x141cf7440: frees the piece's
    arrays only, nothing else).
  - Nothing removes the trains or entities that use a cleared piece: vanilla never removes a
    railway. A train on the removed railway is the likely culprit (trains run after loading,
    which fits the timing).
- **Fix:** after every `DowngradeRailway` that changed a link, the loader runs what the supply
  system's after-load refresh (0x140f46dc0) runs: `Clear(gfx)` 0x141647350 (pieces, trains via
  0x1416473d0, entities via 0x141647230) then `Rebuild(gfx)` 0x14164af50 (a piece for every province
  with a railway). Synchronous on the main thread, so no frame draws the stale state. Trains respawn
  by themselves.

### In-game results (2026-11-09, 1.19.4.2)

- First build: factories were assigned but nothing progressed (speed never set; see above).
- Fixed build: the entry progressed and lowered 6521-11444, 11444-3312, 3312-6282 in order, down
  to 0 with `levels = 5`. Logged costs per level 186.667 (level 3), 143.333 (2), 100 (1): a third
  of 560, 430 and 300. Progress about 126-142 per day.

### To verify in game

1. "Queue dismantling one level" adds an entry at the end of the construction queue; factories
   are assigned and the progress bar moves.
2. Each link loses a level when its share is paid; the entry disappears when all are done.
3. With equal factories it takes about a third of the time building that level takes.
4. Save and load mid-way: the entry, its progress and the railway are as before.
5. Cancel mid-way: the entry goes, the levels lowered so far stay lowered.
6. How the entry's text and level look in the queue.

## H5: who may dismantle where (game rules)

- Relations, as the vanilla triggers evaluate them:
  - `is_subject_of` (0x1403fa270): country = TagToCountry(tag) 0x140c07bf0, diplomacy country+0xf88,
    relation = diplomacy->+8[TagIndex(other) 0x140c08790], `IsSubject(relation)` 0x140d87160
    (relation+0x288 = the subject agreement, type 0x30d1, overlord tag at +0x10).
  - `is_in_faction_with` (0x1403f90f0): diplomacy+0x2d8 = faction, same faction = same pointer.
  - province controller: gamestate+8 is the province table (vtable[0](id)), controller tag at
    province+0x188 (CBuildRailwayCommand compares it with the command's country).
- The loader's check: every province of the path controlled by the country (the engine's "same
  country" test) or, per global flag, by a faction member / subject / overlord. It replaces the
  engine's build permission (`Clickable`, `PathUsable`/`ProvinceUsable`) for the X, the click and a
  dismantle line's daily step; without these signatures the build permission stays.
- Game rules can't be read from C++ here without more work; a mod sets the flags from
  `has_game_rule` at `on_startup` (rules are fixed per game, flags are saved).

# Part I: province building removal (supply hubs first)

The railway work (Part H) carried over to province buildings. It started with supply hubs and
widened to every building in the state view's province row: a removal cross as vanilla shows on
state buildings (I4), single player. I0 to I3 are the findings and decisions on the way there.

## I0: what vanilla already does (to test first, no loader code)

Unlike a railway, a hub is a plain province building (`map/supply_nodes.txt`, `supply_node` in
`map/buildings.txt`), and vanilla has `remove_building = { type = supply_node level = 1 province =
N }`. For `rail_way` that only lowered the mirror building (H1). Whether it is enough for a hub is
the first question, because it decides how much engine work is needed:

1. **`remove_building` on a hub:** does the hub leave the supply system (map icon gone, no supply
   flow from it, `num_of_supply_nodes` one lower, units around it lose supply), or does only the
   building level change?
2. **`damage_building` on a hub:** does a damaged hub stop supplying until repaired? If so, the
   engine already has a "hub off" state the loader could reuse.
3. **Save and load** after each: is the result the same after loading?
4. **Rebuild:** does `add_building_construction` with `instant_build` restore it cleanly?

The test mod's category "HOI4 Loader: Province buildings"
(`hoi4_loader_test_province_building_decisions.txt`) runs these on Berlin (6521, state 64) and West
Prussia (6282, state 63), both hubs in 1936, and logs the hub count to `logs/game.log`.

## I1 findings (1.19.4.2 `v1.19.4.2.2da5`, the H1 Ghidra project; addresses for orientation only)

Static analysis only. The analysed copy has the same SHA-256 as the game folder's hoi4.exe
(`bccc6290…`).

### A hub is the province building, and the supply system follows it

- **Storage:** no separate hub object. Loading `map/supply_nodes.txt` (0x140f386f0) looks up each
  province (`gamestate+8` vtable[1](id)), takes its building of the supply_node type
  (`FUN_140332260()+0x3a0`) from the province's building list (+400) and calls
  `CBuilding::SetLevel(building, level, 0)` (0x141163f20). A hub is that building's level.
- **Supply cache:** `CSupplySystem` (gamestate+0x3d8) keeps a 0x20-byte entry per province (+0x10,
  count +0x1c). The OOS checksum (0x140f42ad0) names its fields: +0x10 `controller_index`, +0x14
  `supply_node_level`, +0x15 `naval_base_level`, +0x18 bits `has_temp_node` / `has_rail` /
  `province_dirty` / `province_dirty_railway`. Per country (0x78-byte entries at +0x40) it keeps
  sorted province lists: +0x40 the country's hubs, +0x28 its naval bases.
- **Refill** (0x140f48e50, per province): walks the province's buildings; type token 0x4cbe
  (supply_node) gives `min(level +0x40, 1)` or the building's +0x42 if higher; 0x31f6 is the naval
  base. When the value changes, the province is added to or removed from the controller's hub
  list. The update (0x140f49f10) runs this only for provinces on the dirty lists (+0x58, +0x70);
  the after-load refresh (0x140f46dc0 -> 0x140f45d20) runs it for every province.
- **Building changes reach it:** `SetLevel` with its notify argument set (and gamestate+0xa39 set)
  calls the supply system's building-changed handler (0x140f46b50 -> 0x140f44ef0). For a province
  building of type 0x4cbe whose +0x42 differs from the cached `supply_node_level`, it marks the
  province and its railway neighbours dirty (0x140f41950), the same marking railways get from
  `CSupplySystem` slot 3.
- **`remove_building`** (CRemoveBuildingEffect slot 13, 0x140327740) calls
  `SetLevel(level - 1, notify = 1)` once per level. So, unlike `rail_way` (H1), vanilla
  `remove_building = { type = supply_node level = 1 province = N }` should take the hub out of the
  supply system at the next supply update. I0 still has to confirm it in game, including the
  map icon and a save/load.
- Building listeners: `SetLevel` also calls the per-type `CBuildingListener` list (0x1412055e0,
  vtable +0x18 (building, old level)) and the country/state hooks (0x14074c4c0, 0x140a261b0,
  0x140bbb6a0, 0x140ab7f40). Which of them update the hub's map icon is not checked.

### Vanilla building removal in the state view (for the GUI)

- The state view's building slot (`countrystateview.gui`, ~line 580) has a `remove` button
  (`GFX_cancel_conversion_button`, the cross) that removes one level. The slot code (0x141e42720)
  handles state buildings (state+0x120) and province buildings (province+400) alike. Its tooltip is
  `BUILDING_SLOT_REMOVE_BUILDING_LEVEL` ("Remove the building."), or the construction / conversion
  variants.
- Permission (0x140a25c50): in wartime a removal cooldown applies
  (`BUILDING_SLOT_REMOVE_COOLDOWN`, the **state's** last removal date at state+0x870); otherwise
  allowed.
- The button is not chosen per building type. The slot (0x141e44b50 hover, 0x141e44c60 show,
  0x141e47400 update/hide) shows `remove` only while the slot is hovered (+0x40), enables it when
  the viewer owns the state and the cooldown check (0x140a25c50) passes, and hides it
  otherwise. The slots are filled by the state view update (0x1417e5000, `state_building_entries` /
  `state_shared_slot_building_entries`).
- The ownership test in the show (0x141e44d69): the player's tag from gamestate +0x520 (or +0x524),
  the state's owner tag at state+0xcc turned into the country (0x140c07bf0), then 0x140748490
  (country, player tag): the same tag, or the same tag group (0x140c08790), else 0x140748500.
  Without it the cross is not shown at all. The loader reads +0xcc from there
  (`kSigStateOwnerCheck`, checked: the view's state field is the tooltip's +0x608).
- **So:** a building that has a slot in the state view should already get the cross. If the hub
  has none, it is because the hub is not listed in the state view, not because the cross is
  denied. Which buildings are listed (0x1417e5000 onwards) is not checked yet.

### Why the hub has no cross (screenshots 2026-11-10)

- The state view shows the hub in the province row at the bottom (`province_building_entries`,
  entries `province_building_entry`), next to naval base and bunkers. The cross the player sees on
  dockyards is in the shared slot row (`state_shared_slot_building_entry`).
- `countrystateview.gui` defines `remove` only in `state_shared_slot_building_entry`. And only
  `CStateSharedSlotBuildingItem::Setup` (0x141e41360) binds it: lambda_1 the picture, lambda_2
  (`_Do_call` 0x141e47cb0) the cross. `CProvinceBuildingItem` (vtable 0x142aeec50, 19 slots) binds
  none, so a `remove` button added to the entry by a mod would do nothing.
- The cross's click opens `CConfirmRemoveBuildingLevel` (0x1190 bytes) with the state id, the
  building type (+0x2e0) and a mode (0 remove a level, 1 a construction level, 3 a conversion).
  Confirming posts `CRemoveBuildingLevelCommand` (vtable 0x142a10dc8; execute slot 10,
  0x1411e2bb0): state id +0x28, building type +0x2c, flags +0x30 / +0x31 (construction /
  conversion). Execute looks the building up in the **state's** list only (state+0x120,
  0x141202610), then `SetLevel(level - 1, notify = 1)` or a construction-queue change. It has no
  province id, so it cannot remove a province building.
- **So the loader is needed:** vanilla has no UI path that removes a province building.

### Still to find

Mirroring H1 and H3:

- **Storage:** where the supply system keeps its hubs (`CSupplySystem` at gamestate+0x3d8, H1)
  and how a hub relates to the `supply_node` building: an object per hub, a flag per province, or
  derived from the building level each recompute.
- **Removal path:** what vanilla does when a hub's province is lost or the building is destroyed,
  and whether `CSupplySystem` slot 3 (the per-province refresh railways already call) also
  rebuilds hubs, or a full recompute is needed.
- **Graphics:** the hub's map entity; whether removing it can leave a stale pointer, as the
  railway graphics did (H4).
- **Construction:** the province building production line class for `supply_node`
  (`CBuildingProductionLine`, vtable 0x1429f8268, 28 slots; not the railway line), its daily step, finished check, cost (`supply_node` `base_cost` in
  `common/buildings`) and save fields, to see whether the H3 marker idea (target below base)
  carries over.
- **Map UI:** the hub badge or construction click in the logistics view, for an Alt+Shift
  dismantle like railways.

## I2: proposed script side (for approval after I0/I1)

**Superseded** by the decisions below and I4: no new script commands (vanilla `remove_building`
already updates supply), no queue, and the scope widened to all province buildings. Kept for the
record:

- `remove_supply_hub = { province = 6521 }`: removes the hub at once, with the supply system and
  graphics refreshed. Any scope. If I0 shows `remove_building` already does everything, this
  becomes a thin wrapper or is dropped, and only the queue below needs the engine.
- `has_supply_hub = { province = 6521 }`: trigger, the same data the effect uses.
- `queue_supply_hub_dismantle = { province = 6521 }`: country scope, a construction queue entry
  that removes the hub when paid. Cost: a share of building one, default 30%, from a global flag
  `hoi4_loader_supply_hub_dismantle_cost_<percent>`, like railways.
- Where the player may dismantle: own provinces, plus faction members / subjects / overlord per
  global flags, sharing the railway game rules or with their own. One feature per tier, as with
  railways: `supply_hubs` (effect and trigger), `supply_hub_queue` (needs `supply_hubs`).

### Scope widened (2026-11-10): all province buildings

Instead of hubs only: a removal cross for **every province building** in the state view's province
row (supply hub, naval base, bunker, coastal bunker, and so on), since they share
`CProvinceBuildingItem`. Hubs stay the main case (the supply system follows, I1). The other types
are to be checked one by one for engine state outside the building, as rail_way had.

Proposed shape (for approval):
- **Mod side:** a `remove` button (`GFX_cancel_conversion_button`) added to
  `province_building_entry` in a companion mod's `countrystateview.gui`. Without the loader it
  stays hidden or inert.
- **Loader feature `province_building_removal`:** hook `CProvinceBuildingItem`'s setup (bind the
  button) and update (show it on hover when the viewer controls the province and the building has
  a level, using the vanilla permission and cooldown check 0x140a25c50 and the same tooltip key
  `BUILDING_SLOT_REMOVE_BUILDING_LEVEL`). A click removes one level instantly:
  `SetLevel(level - 1, notify = 1)` on the province's building, at the next main-thread safe
  point.
- **Multiplayer:** the click is UI on one machine. **Decided (2026-11-10):** single player only
  in v1, as with railway badge clicks. An MP version needs a synced command carrying the province id. Options to find: a
  vanilla command that can carry it, or a script effect run through one.
- **Script side** (optional, same feature): `remove_province_building = { province = N type =
  supply_node }` is just vanilla `remove_building`, so no new effect is needed unless I0 shows a
  gap.

## I3: the province row's item (`CProvinceBuildingItem`), for the hooks

- **Construction:** `CProvinceBuildingItem(gui, stateView)` 0x141e40dd0 (called from the state
  view's province row fill 0x1417e1f10). It loads `province_building_entry` (0x1423434a0),
  sets the vtables (0x142aeec50 / 0x142aeecf0), builds a `CLegacyButtonObserverGlue` (0x141e40290)
  with one handler, `FUN_141e45310` (the picture's left click: opens building), and registers it
  on the child `building_picture` (window vtable +0x68 finds a button by name; observer list at
  button+0x80). Destructor: vtable slot 0, 0x141e41a20. Tooltip: second vtable slot 0, 0x141e41e80.
- **Fields:** +0x10 the entry window, +0x20 the state view (+0x600 the selected province, +0x608 the
  state), +0x28 the building type (+0x2e0 type index, +0x2e4 picture frame, +700 > 0 for province
  buildings). The province's building of that type: `FUN_141202610(province+400, type)`.
- **Update:** 0x141e462a0 (from the province row update 0x1417e44b0). It sets the picture frame,
  enables the picture when the province may build it (0x141201580; enable vtable +0x298, disable
  +0x2a0, state +0x218), writes the level text (`CAPACITY`) and the damage bar.
- **How the shared slot binds its cross:** `FUN_142359ed0(CButtonWrapper* out (0x5d8 bytes),
  button, std::function<void()>* click, std::function<void()>* other)` builds a `CButtonWrapper`
  (its own `CButtonObserverGlue`) on the button. The functions are MSVC `std::function`s (impl
  pointer at qword 7, `_Copy` called through it), so the loader can pass its own.

Hook plan (feature `province_building_removal`, single player):
1. **Bind:** hook the constructor. After the original, if the entry window has a child `remove`
   (only when the companion mod's .gui adds it), build a `CButtonWrapper` for it in loader-owned
   memory with `FUN_142359ed0` and a click function that remembers the item. Free it in the
   destructor hook (0x141e41a20) before the original runs.
2. **Show:** hook the update. After the original, show `remove` when the province has a level of
   that building, the viewer controls the province, and the vanilla permission and cooldown check
   (0x140a25c50) passes; otherwise hide it. Vanilla shows its cross only on hover. v1 shows it
   always and adds hover once the hover handler is mapped (the shared slot's is 0x141e44b50).
3. **Tooltip:** hook the tooltip (0x141e41e80). For the `remove` element, return
   `BUILDING_SLOT_REMOVE_BUILDING_LEVEL`, or the cooldown text 0x140a25c50 writes.
4. **Click:** single player only (as `railway_queue`'s badge clicks), deferred with
   `RunOnMainThread`: look up the province's building again, then `SetLevel(level - 1, notify = 1)`
   (0x141163f20), exactly as `remove_building` does. Vanilla's dockyard cross asks for confirmation
   (`CConfirmRemoveBuildingLevel`). Its dialog only carries a state and a type, so v1 removes
   without asking. (Since built: the confirmation dialog, I4.)
- **Signatures to make:** the constructor, destructor, update, tooltip, `FUN_142359ed0`,
  `FUN_141202610`, `SetLevel` (railways may already resolve it), the permission check 0x140a25c50;
  RTTI check on the `CProvinceBuildingItem` vtable before each use.
- **Risks:** the `std::function` ABI shared with the game (MSVC STL; checked at bind time by
  reading back the impl pointer); the window's find-by-name logging when `remove` is missing (find
  how vtable +0x68 behaves on a missing name before calling it, or use the non-logging lookup);
  building types with engine state outside the building (check each type in game: supply hub,
  naval base, bunker, coastal bunker, others in `common/buildings`).

### GUI (requested 2026-11-10)

A cross next to the supply hub icon, as vanilla shows next to removable buildings such as fuel
depots in the state view's building slots: the same `remove` button
(`GFX_cancel_conversion_button`) and the same click behaviour. Preferred path, given I1: make the
engine's own slot logic offer `remove` for the hub instead of drawing a new element. The vanilla
removal (and its wartime cooldown) then applies, and `SetLevel` keeps supply in step. A hook is
only needed where vanilla hides the button for the hub (to find). If the dismantle should go
through the construction queue instead of being instant, the click is redirected to
`queue_supply_hub_dismantle`. Optionally the same cross on the logistics map's hub badge
(`CSupplyNodeMapIcon`, `supply_node_map_icon`), with Alt+Shift as for railways.

**Decided (2026-11-10):** the cross removes the hub instantly, as vanilla does for the other
buildings (vanilla removal, wartime cooldown included). No construction queue entry for the cross.

Open decisions: own game rules or the railway ones; whether the capital's hub may be dismantled;
whether a hub's connecting railway should be left alone (assumed: yes). With the scope widened
and vanilla's rules applied (I4), none of these needed an answer for v1: the cross follows
vanilla's removal (controller, cooldown) and leaves railways alone.

## I4: implementation (2026-11-10)

Built as planned in I3, with these changes found while building it:

- **No companion mod: a file patch.** The virtual file API got `register_file_patch(suffix,
  patcher)`. On the first read of each existing file with that suffix (vanilla's and every mod's
  copy separately), the patcher gets the bytes and returns the patched content; the copy is written
  to `%TEMP%\hoi4_loader\patchNN_MM_<path>` and served instead. `gui_patch.cpp` adds the `remove`
  button (x 42, y 34: the shared slot's offset from its picture) before the closing brace of
  `province_building_entry`. A file that already has `remove` there, or no such entry, is left as
  it is. The patch is registered only if the feature's hooks were queued.
- **Bind in the update, not the constructor.** The first update of an item looks the button up
  without logging: `Lookup(window+0x12a0, name)` (0x142331d90), which returns null for a missing
  name. The finder at window vtable +0x68 logs "Could not find" only with its flag set, and with
  the flag clear it returns a shared placeholder, not null. Items whose window has no `remove`
  (patch not applied) are remembered and left alone. So the constructor needs no hook.
- **Hooks:** update (`kSigPbUpdate`), tooltip (`kSigPbTooltip`), destructor (item vtable slot 0,
  shape `kCodePbDtor`; its second `lea` is the tooltip sub-object's vtable, whose slot 0 must be
  the tooltip signature's address).
- **Button wrapper:** `CButtonWrapper` constructor (`kSigButtonWrapperCtor`, checked against the
  RTTI vtable it stores) into loader-owned 16-aligned storage. The size (0x5d8) comes from the
  distance between the two wrapper members in `CStateSharedSlotBuildingItem`'s destructor
  (`kSigSharedSlotDtor`, which also yields the wrapper destructor 0x14235a680). The wrapper copies
  the passed `std::function` and clears the caller's (`_Delete_this`, then impl = null), so the
  loader's copy is not destroyed twice. `static_assert` on the 64-byte layout.
  - **ABI assumption:** the loader's `std::function<void()>` is handed to the game's code as is. That
    holds only while the loader and the game use the same MSVC STL layout: 64 bytes, the impl
    pointer in the last qword, and `_Copy` / `_Delete_this` as the impl's virtual functions. Only
    the size is checked (`static_assert`). Both are built with MSVC, but a change of the STL's
    `std::function` (a new ABI generation) would break this without a compile error.
- **Show:** the cross shows when the province has a level, the local player's country owns the
  state (owner tag state+0xcc, `kSigStateOwnerCheck`: vanilla's shared slot condition) and controls
  the province (the controller offset `kSigRailController` gives), and the session is single
  player. Before the PR #3 review only control was checked, so the cross showed in occupied enemy
  provinces. It is enabled when vanilla's permission check (`kSigRemovePermission`, 0x140a25c50)
  passes, greyed out during the wartime cooldown. Element calls: hide vtable +0x80 then hidden bit
  0x10 at +0x75, show clears it then +0x78 (`kSigGuiHideShow`, from the shared slot's cross);
  IsEnabled +0x218, Enable +0x298, Disable +0x2a0 (`kCodePbUpdateEnable`, searched inside the
  update: the custom icon item has the same code, so it is not unique as a signature).
  Always visible, not hover-only like vanilla's.
- **Tooltip:** `BUILDING_SLOT_REMOVE_BUILDING_LEVEL`, or the cooldown text the permission check
  writes.
- **Cooldown start (new finding):** vanilla's `CRemoveBuildingLevelCommand` stamps today
  (gamestate +0x468) into the state's last removal date (+0x870) before it removes the level
  (0x1411e2e77, `kSigRemoveCooldownStamp`). The loader's removal does the same, so the wartime
  cooldown applies to province buildings as to state buildings.
- **Click:** deferred to the next safe point (single player). It carries the province, the type
  and the state, not the item, and a generation number that changes on every game start or load.
  There the building is looked up again, and level, owner, controller and permission are checked
  again.
  Then come the stamp and `SetLevel(level - 1, notify = 1)` (`kSigRemoveBuildingLoop`, cross-checked
  with the railway code's SetLevel).
- **UI refresh (found in the first game test):** the level dropped at the click, but the state view
  showed it only once the game ticked. Vanilla's command ends by posting event 0 to the UI target
  `ui->vtable[0xb8]()` (`kSigRemoveUiRefresh`, 0x1411e2ead; the same UI global, slot and post
  function as the railway construction view refresh, which posts 0x11). The loader's removal now
  does the same.
- **Signatures:** 12 new and 1 anchor, 7 code shapes, all unique on 1.19.4.2 (then 90 in total;
  after the rebase onto `time_control` and the PR #3 owner check, 13 signatures and 1 anchor of
  98).
  A static dry run of every resolver cross-check on the analysed copy passes: item window +0x10,
  type +0x28, tooltip sub-object +0x18 (view +0x8), view province +0x600 / state +0x608, buildings
  +0x190, level +0x40, button map +0x12a0, wrapper 0x5d8 bytes.

### Confirmation dialog (requested after the first game test, 2026-11-10)

- **Vanilla's dialog:** `CConfirmRemoveBuildingLevel` (0x1190 bytes; vtables 0x142b15440 /
  0x142b154d8 / 0x142b15500).
  - The shared slot cross's click (0x141e47cb0) allocates it with the engine's allocator
    (0x1425a01dc). It constructs it with `(ui, state id (state+0x58), building type index
    (type+0x2e0), mode)` (0x141fe6290: fields +0x1180 / +0x1184 / +0x1188; mode 0 remove a level, 1
    a construction level, 2 / 3 conversions, each with its own title and text keys
    `CONFIRM_REMOVE_BUILDING_LEVEL_TITLE` / `_DESC` and so on).
  - It is handed to the dialog manager `view+0x5f0->+0x4f8` with `Open` (0x140b95180), and the
    dialog frees itself when closed (slot 0, delete with size 0x1190).
- **Its slots:**
  - Slot 1 (0x141fe6bf0) is the "still valid" check. It looks the building up in the **state's**
    list (state+0x120) and fails for a province building, so the dialog would close at once.
  - Slot 16 (0x141fe6380) is OK. It posts `CRemoveBuildingLevelCommand` for the state building
    (or the construction / conversion variants).
- **The loader:**
  - The province cross opens the same dialog in mode 0, from the same pieces
    (`kSigConfirmRemoveOpen`: allocator, size, constructor, manager offsets, ui global, field
    offsets; the view is the item's +0x20, the same object the shared slot item keeps at +0x38).
    It remembers the dialog with its province, building type and state.
  - Three hooks, all three or none: slot 1, slot 16 and slot 0 (the destructor, which forgets the
    dialog). For the loader's dialogs, slot 1 answers whether the removal could still happen: the
    province still has a level, the player controls it, and the cooldown allows it. Slot 16 hands
    the removal to the plugin (deferred, single player) and closes. Every other dialog goes
    straight to vanilla.
  - Checks: the constructor's shape and the vtable it stores against RTTI; the three slots' shapes;
    the destructor's vtable; slots 1 and 16 read the state id field the constructor writes
    (`kCodeConfirmCtor` / `Dtor` / `Valid` / `Accept` / `ReadState`). If anything is missing, the
    cross removes without asking, as before. Faults in the hooks fall under the fail-safe below; a
    fault in the check answers "not valid", which closes the dialog.

### The PR #2 review points, applied here (2026-11-10)

The railway review (H3, "After the PR #2 review") asked the same questions of this feature:

- **Fail-safe:** the loader's part of each hook (update, tooltip, click, removal) runs under SEH.
  A fault is logged once and switches the feature off for the session (the engine side's
  `g_enabled`, and the plugin clears the feature's flag). After that, every cross hides on its
  entry's next update (bound or not: see "After the PR #3 review"), nothing new is bound, clicks
  and removals do nothing, and the tooltip is vanilla's. The destructor hook keeps freeing the button bindings, so
  closing the state view stays safe. There is no game-entry self-check like railways' because the
  feature keeps no data of its own to check. Instead, before any read, the item's vtable is
  compared with `CProvinceBuildingItem`'s (RTTI), and the window's with `CContainerWindow`'s.
- **Threads:** the update, tooltip, destructor and click run in the UI on the main thread. The
  first call logs its thread (`province_building_removal: first entry update on thread N (the
  main thread)`). A call from any other thread leaves the entry as vanilla made it. The removal
  itself is deferred to the next main-thread safe point.
- **Fixed numbers:** every offset, slot and flag comes from the signatures and code shapes listed
  in I4. The only constants are the button's name (`remove`), its position in the patched GUI
  file, and a 0x40-byte margin on the button wrapper's storage beyond the size the shared slot
  item implies.
- **Saves:** the feature saves nothing of its own. A removal changes only what vanilla's removal
  changes: the building level and the state's last removal date. So a save made with it loads
  the same without the loader. The patched GUI file is not part of any save.
- **Global hooks:** the GUI patch runs inside the virtual file API's `CreateFileW`/`CreateFileA`
  hooks (DEVELOPING.md, "Threads and hooks"). The three item hooks are specific to
  `CProvinceBuildingItem` and touch nothing else.
- **1.19.3:** unchecked, as for railways (no copy here). A false match would fail one of the
  resolver's cross-checks, and the feature would then stay off.

### After the PR #3 review (2026-11-11)

- **Crosses after a fault:** the switched-off branch of the update hid only buttons bound before
  the fault. The patched file is still served, and a buttonType is visible by default, so a state
  view opened after the fault showed working-looking crosses that did nothing. Now, while off, the
  update looks the entry's `remove` button up (the same non-logging lookup, no binding) and hides
  it.
- **Ownership:** the cross, the dialog's validity check and the removal require that the player
  owns the state (vanilla's condition, `kSigStateOwnerCheck`, see "Vanilla building removal in the
  state view") as well as controls the province. Before, an occupied enemy province's naval base,
  bunkers and hub could be removed.
- **Partial init:** the three item hooks are queued all or none. If a later one fails, the ones
  already queued are taken out (`HookUnqueue`, MinHook `MH_RemoveHook` before the queue is
  applied). The same happens if the file patch cannot be registered. The dialog's three hooks are
  handled the same way, independently (without them the cross removes without asking).
  `DisableProvinceBuildingRemoval` (never called) became `UnhookProvinceBuildingRemoval`.
- **`CreateFileA` paths:** the file patch widened each byte of an ANSI path. It now converts with
  `MultiByteToWideChar(CP_ACP)`, as CreateFileA does, so a path with non-ASCII characters in a
  non-Western code page is read and patched (before, it failed safe: the original was served).
- **VFS caching:** each patched copy is made once per session (DEVELOPING.md says so).
- **Docs:** the "at once" in MODDING.md, the state's (not the country's) removal date at +0x870,
  the `std::function` ABI assumption (I4).
- **Code shape checker:** `tools/check_code_shapes.py` reads the province and dialog offsets from
  `signatures.h` instead of repeating them.

### To verify in game

1. `loader.log`: `Province buildings: ...` with the values above, the feature active, and
   `File patch 'interface\countrystateview.gui': ... -> ...` on the first state view.
2. A state with a supply hub: the province row's hub shows the cross (bottom right of the entry);
   its tooltip says "Remove the building."; provinces without the building show none.
3. Click: the hub level drops to 0 and the entry updates; the logistics map mode shows no hub
   there and supply around it changes as with the I0 probe; `loader.log` says one level removed.
4. Naval base (level 3 to 2 per click), bunker, coastal bunker: one level per click, no crash,
   save and load keep it.
5. At war: after one removal the cross is greyed out and its tooltip gives the cooldown date
   (vanilla's dockyard cross does the same in that state).
6. **Ownership:** an occupied enemy province (the player controls it, the enemy owns the state):
   no cross. A province of the player's own state that the enemy occupies: no cross. The player's
   own, controlled provinces: the cross as before.
   *Tested 2026-11-11 after the PR #3 review fixes: as described.*
7. Multiplayer: no cross.
8. A UI mod that replaces countrystateview.gui: its copy is patched (`loader.log` names its path).
   *Tested 2026-11-11 with KaiserreduX (Workshop 2076426030, a heavily changed 258 KB copy): the
   cross shows and removes as in vanilla. The patch also applies offline (`gui_patch_test <file>`)
   to The Road to 56, The Great War Redux and The Fire Rises.*
9. Destroying the state view (close, reopen, switch states quickly): no crash (the destructor
   hook frees the wrapper first).
10. **Confirmation:** the cross opens "Remove Building"; Cancel leaves the level; OK removes one
    level; with two crosses clicked in a row only the confirmed one goes; a building lost while
    the dialog is open closes it (or OK does nothing).
    *Tested 2026-11-10: the dialog opens from the province row, Cancel and OK behave as described.*
11. **After a fault** (not reproducible on purpose; check by reading `loader.log` if one ever
    happens): state views opened afterwards show no cross.
# Part J: equipment conversion (`equipment_conversion`)

Built on `feature/equipment-conversion` (PR #4, closed in review: a gameplay change belongs in a
mod), reworked as opt-in tools on `feature/equipment-conversion-tools`, rebased on `main` after
#2 and #5 (J6). J1-J5 describe the first version; J6 says what changed.

The goal: a production line's **Convert from Stockpile** takes equipment of any earlier
generation (`infantry_equipment_0` / `_1` into `_2`, older chassis, airframes and hulls), for
every archetype and mod, instead of only the pairs `can_convert_from` names.

## J1 findings (1.19.4.2 `v1.19.4.2.2da5`, the H1 Ghidra project; addresses for orientation only)

Static analysis only.

### The convert-from list (one per equipment type)

- `CEquipmentType` fields: +0x18 name; +0x538 archetype (an archetype points to itself); +0x540
  parent (`parent =`; a root points to itself); +0x424 generation depth (parent hops, computed in
  the per-type post-load 0x140cf2110); +0x5b6 "derived" byte (then +0x548 is the base whose key
  +0x478 is used).
- **+0x4f0 convert-from list:** `CEquipmentType const*` array, capacity +0x4f8, count +0x4fc,
  allocator +0x500 (vtable +8 alloc(size, align 8), +0x10 free). The type's destructor
  (0x140a3bfa0) frees it with that allocator.
- **Built once per type at database load** by `BuildConvertFromList(type, database)` 0x140cf2490,
  called at the end of the post-load:
  1. count = 0; seed with the equal range of database+0x128 (types sorted by base key, count
     +0x134; 0x140a42e50) for the type's base key, minus the type itself: its own variants and
     types sharing its base;
  2. append each `can_convert_from` token (+0x508, stride 0x20, count +0x514), resolved with
     0x140a434d0. Errors name " in can_convert_from list for type " (unique string).
- So vanilla has no conversion between generations unless `can_convert_from` names it
  (`infantry_equipment_2` has none; light tank artillery lists the light tanks).

### Who reads it

- `CanConvert(targetDesign, sourceDesign)` 0x140c2cb00: same design root (design +0x3f8 parent
  design, walked to the top), else the target type's list contains the source design's type
  (design +0x3f0). Callers: the line source rebuild 0x1419bd150, naval refits 0x140de0270 /
  0x140ed14a0 ("Trying to refit a ship to equipment type that it cannot convert to."), the
  production UI 0x141811740 and 0x141e97a30.
- Five design-flag helpers (0x140c34e60, 0x140c38190, 0x140c38280, 0x140c383a0, 0x140c38530;
  design +0x427 / +0x428) read the list directly. They mark designs that can be converted to or
  from something (designer and stockpile markers). They follow the list without a hook; with
  naval pairs refused (J2) they can still mark a hull as convertible. Cosmetic, to look at in game.

### The production line

- Line +0x88 target design, +0xec "Convert from Stockpile" (saved), +0x70 sources (count +0x7c,
  0x28-byte entries made by 0x141ac2810, each with a per-organisation cost cache).
- **Sources are rebuilt** from the country's stockpile (country +0xf68 production, +0x200) by
  0x1419c2c30 -> 0x1419bd150, one `CanConvert(line target, stock design)` per stockpile entry,
  then sorted. They are not saved.
- The line's unit cost while converting: 0x1419c2ea0, the first source's cached cost.
- Tooltip: 0x1419c2fd0 (`PRODUCTION_LINE_CONVERT_*`), one entry per source 0x141ac3390.

### The cost

- `ConversionCost(sourceDesign, out cost, targetDesign, organisation, out breakdown[8])`
  0x140c29eb0, the only reader of the `NProduction` `BASE_LAND/NAVAL_EQUIPMENT_CONVERSION_IC_COST_FACTOR`
  and `MIN_*_EQUIPMENT_CONVERSION_RESOURCE_COST_FACTOR` defines. Module differences; the source's
  chassis IC (type +0x3b0) times BASE_LAND (minus the organisation's bonus) or BASE_NAVAL (naval:
  the unit-type mask 0x80004003c1 of `IsNaval`); upgrade differences; modifiers; then resources.
  out+0 is the IC (fixed point); breakdown entries 0, 2, 4, 6 are IC parts the tooltip shows (each
  added with +=), 1, 3, 5, 7 modifier percentages.
- Callers: production lines (0x141ac29b0 via the source cache), the tooltip, the UI and the AI's
  production planning (0x141143610, 0x141144830, 0x1411485e0).

## J2 design (decided with the user, 2026-11-11)

- **Engine tier, no generated script.** One list hook covers every reader. A generated
  `can_convert_from` patch would have to rewrite every equipment file of every mod.
- **Pairs:** after `BuildConvertFromList`, every type's `parent` chain within its archetype is
  appended where the list lacks it. The index where the loader's entries start is recorded per
  type, so "the loader added this pair" is known later.
- **Cost:** the game's cost times factor^(g-1), g = generations back (`GenerationsBack`, along the
  parent chain). One generation back: the game's cost. Applied to the IC and the breakdown's IC
  parts; resources stay the game's.
- **Pairs the game already allows** (the user's addition): the game's cost by default; a rule
  makes the factor apply to them as well. They are never refused.
- **Who:** every country, AI included (the list is global per type).
- **Naval:** first decided "no naval"; then (the user, "immersion breaking") a rule, off by
  default. The rules are only known in a game, after the lists are built, so naval pairs are
  added like the others and `CanConvert` refuses them while the rule is off.
- **Rules** (first version; superseded by J6 and J7): game rules in a rules-only mod set global
  flags at `on_startup` (railways' way, H5), read at game entry and daily. Now the tools are
  opt-in flags a mod sets (J6), read at game entry, and in single player also every 30 frames;
  in multiplayer at game entry only (J7). The hooks read only atomics.

## J3 implementation

- **Hooks** (MinHook; DEVELOPING.md "Global hooks"):
  - `BuildConvertFromList`: after the original, append (the list's own allocator; grows exactly to
    the new count) and record.
  - `CanConvert`: after the original, if it said yes for a pair the loader added and the design
    roots differ (the engine's other reason), refuse while the feature is off, the "off" rule is
    set, or the target is naval and the naval rule is not.
  - `ConversionCost`: snapshot the breakdown, run the original, scale for g >= 2.
- **Resolution** (5 SIG + 1 ANCHOR, engine_conversion.cpp; `tools/check_code_shapes.py`
  `check_equipment_conversion`):
  - list, capacity, count and allocator from the builder's grow code (`kCodeConvListGrow`), each
    read twice there and the count again from the prologue; the alloc and free vtable slots from
    the same code; the anchor's lea must lie inside the builder; the type name from the lea right
    after it;
  - `CanConvert`'s list test (`kCodeCanConvertList`) must use the same list and count; its design ->
    type offset must equal production_data's (`EquipmentTypeOffset`) and the cost function's;
  - parent from the post-load, read twice; depth from its store (`kCodePostLoadDepth`); the
    post-load must call the builder;
  - `IsNaval` by its mask (four sibling tests share all code before it); its archetype offset read
    twice and checked against production_data's.
- **Fail-safe:** SEH around the loader's part of each hook. On a fault: logged once, the feature
  switched off (vanilla behaviour: the loader's pairs refused, costs unchanged), deactivated on
  the main thread. Self-check on game entry: every recorded list still holds the loader's entries,
  and each is an earlier generation of its type; otherwise off.
- **Threads:** no graphics. The first `CanConvert` with the loader's pairs and the first cost call
  log their thread; static analysis says both run on the main thread (production daily, UI) and on
  AI planning threads. They read only type data fixed after load, the loader's table (written only
  during the database load, under an SRW lock) and atomics.
- **Saves:** nothing is saved by the loader; sources are rebuilt from the stockpile. Expected: a
  save loaded without the loader converts only what the game allows (to check in game).
- **1.19.3:** unchecked (no 1.19.3 copy here); a false match would fail a cross-check and the
  feature would stay off.
- `tools/check_signatures.py` and `tools/check_code_shapes.py`: all ok on the 1.19.4.2 copy
  (SHA-256 `bccc6290…`, same as the game folder's). Smoke test: the signature total grew by 6 here (J5 and the rebases: J7).

### To verify in game (1.19.4.2, single player, Germany 1936, decisions "HOI4 Loader: Equipment Conversion")

1. `loader.log` at startup: "Equipment conversion: list +0x4f0 ...", `equipment_conversion`
   active; on game entry "N lists built, M types extended, K pairs added (L naval)" and
   "self-check passed".
2. "Conversion: prepare", then **decommission** Infantry Equipment 0 and I (production view, the
   variant's "Decommission": the stockpile walk uses the country's own designs only when they are
   decommissioned, J5). An Infantry Equipment II line with Convert from Stockpile: the tooltip lists
   Infantry Equipment I (the game's cost) and Infantry Equipment 0 (75% of the game's cost); the
   line consumes both and produces Infantry Equipment II.
3. Tanks: a Panzer II chassis design converts Panzer I designs.
4. The rules mod: factor 50% shows 50% for two generations back; "blocked" removes the extra
   sources; naval allowed lets an older hull's ships refit into a newer hull, blocked refuses it.
5. Save, load: the same. Load that save **without** the loader: no error, the extra sources are
   gone, the line goes on (converting what the game allows, or producing).
6. `loader.log` thread lines; no "fault" line.

## J4 investigation: choosing which sources a line converts (the user's request)

Vanilla converts **every** stockpiled design that `CanConvert` accepts. A small airframe line
(say a fighter design) therefore also eats CAS and naval bomber designs of the same airframe: they
are designs of the same type, so not even a generation question. The user asked for a button to
enable or disable conversion per kind of source.

Findings so far:
- The line entry (`production_military_line_entry`, countryproductionlineview.gui ~line 1720) has
  one `convert` button (`GFX_production_line_convert`), the line's +0xec toggle. The sources are
  shown only in its tooltip ("Order of conversion"); there is no list UI to put checkboxes in.
- The per-line rebuild 0x1419c2c30(line, tag) is the place to filter: it knows the line, and after
  the original the loader can drop sources (line +0x70 / +0x7c) it should not use. The cost then
  follows (the line uses its first source).
- Saved state: the loader may not add fields to a line. Choices are country flags (saved with the
  game, harmless without the loader) or per-session state.
- A button needs a .gui change: the virtual file patch `register_file_patch` from Part I4 (PR #3),
  not on `main` yet.
- Clicks are UI on one machine: single player first, as railway badges and the building cross.

Options to decide:
- **A, one button, a mode per line:** next to `convert`, cycling "all sources" / "same role only"
  (the source design's role equals the line's: fighter only from fighters) / maybe "same type
  only" (no generation conversion). Small UI; the role field of a design is still to be found.
- **B, a list per line:** a popup listing the stockpiled sources with a checkbox each. Full
  control; a new window and a dynamic list, much more UI work.

**Decided (2026-11-11): A.**

### J5: the role mode (built)

Findings:
- **Role:** a design's unit-type bits at design +0x408 (`FUN_140c29730` builds them from the type
  and its modules: module +0x78 `add_equipment_type` bits, +0x88 removed bits, the `type_override`
  slot when set). `ConversionCost` tests the same field against the naval mask; that instruction
  gives the offset (`kCodeConvCostChassis`).
- **The line UI item** `CProductionLineMilitaryItem` (constructor 0x141dd3be0): `convert` is
  item +0x3880, its observer glue item +0x27e0 calls 0x141de8e30, which posts the toggle command
  (0x1411d16c0) for the line at item +0x3878 (toggle byte line +0xec).
- **The convert tooltip** 0x1419c2fd0(line, ?, std::string* out) writes into `out` with the game's
  `std::string::append` (0x14012b330).
- **Rebuild:** 0x1419c2c30(line, const int* tag) is reached from the line's vtable (its update
  0x1419c4750 and another slot 0x1419c4f90); not checked how often it runs.

Design (A without a new GUI element, so no file patch is needed on `main`):
- **Shift+click on `convert`** (single player; GetAsyncKeyState at the click): toggles "same role
  only" for (player country, the line's equipment type) at the next safe point, instead of
  vanilla's toggle. Stored as the global flag `hoi4_loader_conversion_role_<TAG>_<type>`; the rule
  reading rebuilds the table from these flags. In multiplayer Shift+click is swallowed and logged.
- **Filter:** the rebuild hook sets a thread-local context (target design, its role) when the
  line's type is in the mode; `CanConvert` then refuses a source whose role differs. Nothing is
  freed or edited in the line: the engine just builds fewer sources.
- **Tooltip:** the mode line is appended (localisation keys `HOI4_LOADER_CONVERSION_MODE_ALL` /
  `_ROLE`, else English).
- **Resolution:** 3 SIG + 1 ANCHOR (signatures now 56). The rebuild's target, type, parent and
  count offsets must match the list and `CanConvert`; one of its calls must reach `CanConvert`;
  the tooltip reads the same target offset and contains the header anchor; the click reads the
  item's line twice. Without them the role mode stays off and the rest works.
- A dedicated button (option A as first described) can follow once the file patch from Part I4
  is on `main`.

**The stockpile walk's own rule** (0x1419bd150, after `CanConvert` says yes): a design whose byte
+0x424 is 0 is skipped if its owner (+0x1c) is the country, or a friendly country (0x140c085f0).
So **the country's own designs convert only when decommissioned** (the production view's
"Decommission", `PRODUCTION_VARIANT_OBSOLETE_NO`: "marking it as outdated equipment"), foreign
ones when captured from a non-friendly country. +0x424 is read as "decommissioned" from that, not
checked in game yet.

**Left-out list (2026-11-11, the user's request):** HoI4 text has no strikethrough, so in "same
role only" mode the convert tooltip gets a greyed section "Not converted (another role):" with
one line per design the mode left out.
- `CanConvert`, refusing a source for role during a rebuild, records it (at most 48, no
  duplicates) if the stockpile walk would have used it (its rule above, `kCodeStockpileSkip`:
  the byte, owner and friendly test read from that code). After the rebuild the list is stored
  per line with its target design.
- The tooltip names them with the game's `DesignName(design, std::string*)` 0x140c30510, the
  function the "Order of conversion" entry uses (`kCodeConvVariantName`, the same call before and
  after `kAnchorConvertOrderEntry`'s lea), freed with the engine's delete (needs the scope block
  pieces). Colours via the rules mod's `HOI4_LOADER_CONVERSION_LEFT_OUT` / `_ENTRY` ($VARIANT$,
  formatted by `Localize`); without them plain text.
- Signatures 57 (+1 ANCHOR).

To verify in game:
1. A fighter line on a small airframe, with **decommissioned** fighter and CAS designs of that
   airframe in the stockpile and Convert from Stockpile on: the tooltip lists both and ends with
   "Sources: designs of every role".
2. Shift+click convert: `loader.log` "... lines of GER: same role only"; the tooltip line
   changes; after the line's next update (or toggling convert off and on) only the fighters are
   listed, and the CAS designs show greyed under "Not converted (another role)". A CAS line of the
   same airframe lists only the CAS.
3. Save and load: the mode stays (the flag). Shift+click again: every role, no greyed section.

### In-game results (2026-11-11, 1.19.4.2, Germany 1936, single player)

- **Decommissioning is required, as J5 read it:** with Hs 123 and Ju 87 still in commission, a
  Ju 87 G 'Kanonenvogel' line said "no equipment in stock that we could use". The user had not
  decommissioned them; the CAS chain (Hs 123, two generations back) is still to be retried.
- **Bf 109 D line** (Basic Small Airframe, fighter), Convert from Stockpile, with decommissioned
  stock: "Order of conversion" listed 280 He 51, 72 Do 22 and 1 Ju 87, then "Sources: designs of
  every role".
- **Shift+click:** the tooltip at once showed "Sources: designs of the same role only", only 280
  He 51 in the order of conversion, and greyed under "Not converted (another role)": Do 22, Ju 87.
  So the line's sources were rebuilt right after the toggle (the rebuild is not only daily).
- Not yet checked: the factor on a source two or more generations back (Infantry Equipment 0, or
  Hs 123 into the Kanonenvogel, which also tells whether the duplicated CAS airframes chain to
  each other), the naval rule, save/load (mode flag) and loading without the loader.

## J6: opt-in tools (after the PR #4 review, 2026-11-11)

**The review (Builder-HD, PR #4 closed):** earlier-generation conversion and the cost factor change
the balance for every country, AI included, as soon as the loader is installed, even for players
who installed it for an unrelated mod. That is a gameplay feature and belongs in a mod; the loader
gives mods tools that script cannot provide and does nothing until a mod uses them. Also a bug:
`ResolveRoleMode` treated every `E8` byte in the rebuild's first 0x100 bytes as a call and read
0x100 bytes at the target without a range check, so on another build a wildcard byte could send it
outside the image and crash the game at startup.

**What script cannot do** (why the loader is still needed): generation pairs for every equipment
of every mod (vanilla: a `can_convert_from` entry per type, and none for other mods' equipment);
a cost per generation (the `NProduction` conversion defines are global); a line's sources and the
convert tooltip (no script access).

**Changes:**
- **Opt-in flags.** `hoi4_loader_conversion_generations` and `hoi4_loader_conversion_role_mode`
  switch the two tools on; the factor, `..._existing_loader_cost` and `..._naval` only count with
  generations on. The old `hoi4_loader_conversion_off` is gone (its meaning is now the default).
- **No list change without the opt-in.** At load `BuildConvertFromList`'s hook only plans (read
  only: the type's earlier generations its list lacks). `Sync` (game entry, and `OnMainThreadFrame`
  every 30 frames in a game, so an `on_startup` flag counts before the first day) appends the
  planned pairs when the flag is set and truncates each list back to where the loader's entries
  start when it is gone (J7 replaced appending at runtime with entries reserved at load and a
  count change). Safe points only: no tick, no conversion test running. After appending, the self-check runs;
  on a failure the pairs are removed again and the feature switches off.
- **Pass-through without the flags.** `CanConvert` refuses a loader pair while the flag is gone but
  the entries are not removed yet; the cost hook scales nothing; the rebuild hook marks nothing;
  Shift+click is the game's click; the tooltip is the game's.
- **All or none at init:** the hooks are queued from one list; on a failure the ones queued so far
  are removed before anything is applied (`HookUnqueue` since J7).
- **E8 fix:** `InText` checks every range against `.text` (`ImageSection`); `CallsTarget` returns
  false outside it, and the rebuild's E8 scan only follows targets inside it.
- **The mod:** the rules-only mod became the example opt-in mod (it sets the opt-in flags from
  game rules, plus a new rule "Same Role Only (Shift+Click)"); since moved to its own repository
  (below). The test mod got "Conversion: opt in (as a mod would)" / "opt out".
- **Console opt-in** (the user's question): the game has no console command that sets a flag or
  runs an effect (only the script effects `set_global_flag` / `clr_global_flag` exist in the exe),
  but its `event` command fires any event. The example mod's rule-to-flag logic moved into the
  scripted effect `hoi4_loader_conversion_apply_rules` (it clears first, then sets per the rules),
  used by `on_startup` and by the hidden event `hoi4_loader_equipment_conversion.1`; `.2` runs
  `hoi4_loader_conversion_clear_flags`. So a save started without the mod opts in with one console
  line, and stays opted in (the flags are saved).

**First in-game test (2026-11-11, 1.19.4.2):**
- Without opt-in: three new games, each "230 types have earlier generations to add (471 pairs, 91
  naval), used only while a mod sets hoi4_loader_conversion_generations"; no list changed.
- "Conversion: opt in": at once "flags: earlier generations ON ... role mode ON", "471 pairs added
  to 230 types' lists", "self-check passed"; the first `CanConvert` with the pairs on the main thread.
- **The example mod did not opt in at game start** (two bugs, from `error.log`, fixed):
  - `on_startup` effects have no scope, and a scripted effect needs one ("hoi4_loader_conversion_apply_rules:
    Invalid Scope ... provided: None"); the call now runs inside `every_country = { limit = { is_ai = no } }`,
    as the test mod does it.
  - A rule-level `desc` is not accepted ("Unexpected token: desc", although vanilla's format
    comment lists it); removed, the option descriptions stay. `main`'s railway rules mod has the
    same five errors.
- **Rebase on `main`** (#2 railways, #5 time control): union merges; signatures 84 + 11 = 95. The
  test mod's create_army category on `main` had lost its `visible` block and brace in the railway
  merge (the railway category ended up inside it); restored.
- **The mod moved out of the loader** (the user, 2026-11-11, as with the railway rules mod): the
  opt-in mod is now its own repository, "HoI4Mod Loader: Equipment Conversion"
  ([ni1saux/HoI4Mod-Loader-Equipment-Conversion](https://github.com/ni1saux/HoI4Mod-Loader-Equipment-Conversion);
  launcher file `hoi4_loader_equipment_conversion.mod`). The loader
  keeps only the tools and the test mod's opt-in decisions; the mod's tooltip localisation keys
  fall back to the loader's English text without it.

**To verify in game:**
1. Loader and test mod, no conversion mod, no opt-in: Infantry Equipment II with decommissioned
   0 and I lists only Infantry Equipment I (the base game); Shift+click on convert toggles convert
   as usual; no mode line. `loader.log`: "N types have earlier generations to add ..., used only
   while a mod sets hoi4_loader_conversion_generations", no "ON" line.
2. "Conversion: opt in": within a second `loader.log` says "earlier generations ON: ... pairs
   added" and "self-check passed"; Infantry Equipment 0 is listed (75%); Shift+click and the tooltip
   work as in J5.
3. "Conversion: opt out": "earlier generations OFF"; back to step 1.
4. A new game with the example mod and its default rules: the flags are set at `on_startup` and
   step 2 holds without the decision.
5. A save started without the mod, then loaded with it: nothing changes until the console's
   `event hoi4_loader_equipment_conversion.1`; then step 2 holds (`game.log`: "opted in from the
   game rules"). `event hoi4_loader_equipment_conversion.2`: step 3.

## J7: PR #7 review (2026-11-11)

**The review (Builder-HD):** opt-in answers #4; one blocker and smaller points.

- **Blocker, multiplayer:** `Frame()` read the flags every 30 frames and `Sync()` then changed the
  lists. Frame rates differ between machines, so a flag a mod sets or clears mid-game would change
  `CanConvert`, the cost and the lists at a different tick on each machine (out of sync). **Fixed:**
  in multiplayer the flags are read at game entry only (new game after `on_startup`, loaded save,
  hot join: the same point everywhere); a flag set mid-game counts from the next load (logged once,
  documented in MODDING). Single player keeps the 30-frame read, so a decision or the console still
  counts within a second. "Single player" is the session as classified at game entry
  (`SessionAllowsLocalWrites`).
- **Growing the list freed the old buffer** at runtime, safe only because of the safe-point rule.
  **Fixed:** at load, right after the game built a type's list (no conversion test runs yet), the
  loader appends that type's earlier generations and sets the count back at once
  (`ConvertListSetCount`): the entries sit in the buffer beyond the count, invisible to the game. At
  runtime only the count changes (`first + n` while opted in, `first` otherwise); nothing is
  allocated or freed after load. Before raising the count, `StillReserved` checks that the game's
  count is unchanged and the entries are still there (otherwise that list is skipped and logged).
- **Shift+click in multiplayer** swallowed the click. **Fixed:** it falls through to the game's
  convert toggle.
- **Hook removal:** `HookUnqueue` (from #3) instead of `MH_RemoveHook` in the plugin.
- **NOTES:** J2 and J3 corrected, J6's mod path and a local path removed.
- **Rebase on `main`** (#3, #6, #8, #9): the two branch commits squashed into one, then rebased;
  add-only conflicts resolved as unions. The test mod's categories fix is now `main`'s (#9).
  Signatures 98 + 11 = 109.

**In-game checks** (still to run, from the review):
1. Opting out removes the pairs ("earlier generations OFF"; the tooltip back to the base game).
2. The factor on a source two or more generations back (Infantry Equipment 0 into II at 75%).
3. The naval flag: without it no hull refits into a newer hull; check whether the designer or
   stockpile still marks a hull as convertible (the design-flag helpers read the list; cosmetic).
4. A new game with the separate mod: the flags at game entry, no `desc` or scope errors.
5. A save made while opted in: loaded with the loader (still opted in), and without it (base game,
   no errors).
6. Multiplayer: a flag set mid-game changes nothing until the next load; no out of sync.

**Results (2026-11-11, the user, 1.19.4.2, single player; the build installed before the J7
changes, i.e. runtime append rather than reserved entries):** 2 (factor two or more generations
back), 4 (new game with the separate mod) and 5 (save made while opted in, loaded with and without
the loader) passed. Multiplayer (6) is not tested for now. Still open: 1 (opt-out), 3 (naval), and a
run of the J7 build itself (the list is now reserved at load).

**Results on the J7 build (2026-11-11, the user, 1.19.4.2, single player):** opt in, opt out and opt
in again work (1; the reserved entries shown, hidden and shown again by the count alone); the naval
flag works (3; whether a hull is still marked as convertible in the designer or stockpile was not
reported). Only multiplayer (6) remains untested.


# Part K: game speed and date display (`time_control`)

Branch `feature/time-control` (from main). Static analysis of vanilla 1.19.4.2 (v1.19.4.2.2da5)
in Ghidra; addresses are for orientation only. **1.19.3 is unchecked.** The loader finds
everything through `src/signatures.h` (seven SIGs, eight CODE shapes) and RTTI, with the
cross-checks below; `tools/check_signatures.py` and `tools/check_code_shapes.py` pass on 1.19.4.2.

## K1 findings

### K1.1 Game speed

- **Storage:** `CGameState +0x4bc`, an int 0..4 (the UI's speeds 1..5).
- **Only writer:** `CGameState::SetGameSpeed(int)` (0x140203f10). It clamps to 0..4, stores the
  value, asserts `ThreadIsMainThread()`, and queues a UI notification. Its three callers are the
  `Execute` of the synced speed commands:
  - `CSetGameSpeedCommand` (0x140f85bc0, speed at `cmd+0x28`);
  - `CIncreaseGameSpeedCommand` (0x140f859a0, speed + 1);
  - `CDecreaseGameSpeedCommand` (0x140f85460, speed − 1).
- **Requests:** `RequestGameSpeed(idler, speed0, bool fromPlayer)` (0x140e4c490):
  1. refuses for observers / game over (`FUN_140bbb0f0(idler+0x6b8)`);
  2. refuses for multiplayer non-hosts;
  3. stores the player's preference (0x14021a230);
  4. posts a `CSetGameSpeedCommand`.

  Its callers:
  - the top bar's speed steps (0x14192c6f0);
  - the console `gamespeed <0-5>` (0x140274800; 0 = pause);
  - `pause_in_hours` (0x140286900);
  - game start (0x140e47560, the remembered speed).

  The +/- keys and buttons post the increase / decrease commands.
- **Multiplayer:** commands are synced, and `CanExecute` says "only the host may change the game
  speed". So a lock that script decides would hold on every machine. The feature is still single
  player only (pause is local, below).

### K1.2 Pause

- **Where it lives:** in the in-game idler, not in the game state: `CInGameIdler +0x6c1`
  (`IsPaused`, vtable slot 94, 0x140e46300).
- **`TogglePause(int mode)`** (slot 72, 0x140e4e0b0):
  - single player: flips the byte (and +0x6c2 = "paused by the player" when mode == 1);
  - multiplayer: posts `CPauseGame`, whose `Execute` calls slot 83 with the player's name.
- **Callers by mode** (virtual calls `[rax+0x240]` found by byte scan):
  - mode 1: the top bar's pause button and its Space shortcut (0x14192efc0), the console, the
    idler's own input handling (0x140e3cb90, 0x1402b7eb0), one UI (0x14156e9c0);
  - mode 0: `CAutomaticPause` (0x14199e050, the event popups' "pause on event") and 0x1401e5660.
- **The Esc menu** pauses through another slot (95, 0x140e4e440, called by 0x140bbc540 when the
  menu opens). The loader does not hook it.
- Other direct writers of +0x6c1: game start, slots 7, 79 and 107. Engine pauses, left alone.

### K1.3 Top bar date

- `interface/topbar.gui`: `time_control_window` holds `pause_button_date(_paused)`,
  `button_speeddown/up`, `button_speedstep1..5` and the `instantTextBoxType` **`DateText`**.
- **`CTopBar` setup** (0x14192c720) keeps the widgets:
  - `+0x80` `DateText`;
  - `+0x68` the idler;
  - `+0xb850..` the five speed steps;
  - listener members for the buttons.
- **`CTopBar::UpdateDate`** (0x14192f460) does
  `SetText(DateText, FormatDate(&gamestate->date))`. Callers: the top bar's game state listener
  (vtable 0x142a8ac90 slot 6) on a date change, and the setup.
- **`FormatDate`** (0x14057a230): `CGameDate { vtable, int hours @+8 }` at `CGameState +0x460`
  (so hours at +0x468, the loader's `kSigGameDate` field).
  - Format: `" %d:00, %d %s, %d"`, with hour = `(h − 43800000) % 24 + 1` and
    year = `(h − 43800000) / 8760`. 43800000 = 5000 × 8760, so the count is hours since year 0,
    365-day years.
  - The loader reads the base from the code.
- **Widget hide/show idiom** (the pause button refresh 0x1419301d0):
  - hide: `w->vtable[0x80/8]()`, then `w[+0x75] |= 0x10`;
  - show: `w[+0x75] &= ~0x10`, then `w->vtable[0x78/8]()`.
- **The whole time panel:** the pause button refresh starts with
  `root->FindWindow(&{"time_control_window", 19}, true)`: root window `CTopBar +0x78`, find slot
  55 (vtable +0x1b8). A window's widget part is at window +0x30. The setup hides the music
  player's window with the same idiom on that part (0x14192d487: `Hide` at +0x80,
  `or byte [win+0xa5],0x10`, i.e. +0x30 + 0x75). `hide_time_controls` hides `time_control_window`
  this way: date, pause buttons, −/+ and the speed steps; the top bar background keeps its empty
  frame.
- `CInGameClock` (0x141842bf0) is the optional real-world clock (`localtime`, "%R"), not the game
  date. Left alone.

## K2 design (agreed with the user, 2026-11-11)

**Script commands** (MODDING.md "Game speed and the date display"):

```
force_game_speed = { speed = 1-5 days = <n> hours = <n> until = "y.m.d[.h]" lock_pause = yes }
release_game_speed = yes
hide_date_display = yes/no
has_forced_game_speed = yes/no
```

**Engine hooks, no `.gui` patch.** A `.gui` change cannot be switched at runtime from script.

| Hook | What it does |
|---|---|
| `SetGameSpeed` | while locked, any value becomes the locked speed: one choke point for buttons, keys, console and commands |
| `TogglePause` | while locked with `lock_pause`, a mode-1 call that would pause returns; unpausing, mode 0 (automatic pauses) and the Esc menu pass |
| `UpdateDate` | after the original, hides or shows `DateText` with the idiom above |
| `CTopBar` destructor | (RTTI slot 0, `kCodeScalarDeletingDtor`) forgets the top bar |

The loader's own actions run in `OnMainThreadFrame`:
- the speed request (the console's `RequestGameSpeed`);
- the unpause (`TogglePause(1)`);
- the date refresh (re-run `UpdateDate` on the live top bar, so a change shows while paused);
- the expiry check against the game date.

**Pause and timing:**
- A lock started while paused unpauses (default 1). A save loaded with a lock stays paused.
- At the end the speed from before comes back (default 2).
- Automatic pauses pass (default 3).

**Saves** (default 4): global flags `hoi4_loader_time_lock` and `hoi4_loader_date_hidden`, which
do nothing without the loader.
- The lock flag's value = speed + 10 × previous speed + 100 × lock_pause.
- Its engine day countdown = ⌈remaining hours / 24⌉ + 1. After loading, the end is
  now + max(1, days − 1) days, so it is rounded to whole days.

**Single player only** (default 5): the effects check `SessionAllowsLocalWrites` taken at game
entry. A save's flags are ignored in multiplayer. `on_startup` fires inside the original
game-entered function, before that check, so the effects do nothing there (documented: use an
event an hour later).

**Cross-checks** (`engine_time.cpp`, the same in `check_code_shapes.py`):
- **The idler global:** the same in four loads (both console sites), and its object must have the
  `CInGameIdler` RTTI vtable at use.
- **TogglePause's slot:** the same in the console's two sites and the top bar's pause click.
  `TogglePause` and `RequestGameSpeed` read the same observer-check member (+0x6b8).
- **The speed field:** the same in `SetGameSpeed` and the increase command's tail. That tail
  reads the loader's gamestate global and jumps to `SetGameSpeed`.
- **The speed count:** the console's bound (5) = `SetGameSpeed`'s clamp + 1.
- **The date:** the date field + `FormatDate`'s hours member = `kSigGameDate`'s field (0x468).
  The base is a multiple of 8760.
- **The widget idiom:** hide and show agree on the flags byte and the bit (mask = ~bit).

**Thread evidence:** each hook logs once on which thread it runs
(`time_control: SetGameSpeed runs on thread ...`, also `TogglePause`, `CTopBar::UpdateDate`, and
the script effects). `SetGameSpeed` asserts the main thread itself. The widget calls happen only
inside `UpdateDate` (after the game's own `SetText`) or from the frame callback.

## K3 in-game test (to do, single player, 1.19.4.2)

Test mod decisions in the category "hoi4_loader test", guarded by
`hoi4_loader_feature_time_control`:

1. **Log.** `loader.log` has:
   - "Time control: 5 speeds, speed +0x4bc, ... TogglePause slot 72, IsPaused slot 94";
   - the top bar line (DateText +0x80, hide slot 16 / show slot 15, date +0x468, base 43800000);
   - "Feature time_control (engine tier): active".
2. **Lock for 2 days at speed 1.**
   - Speed 1 at once; the speed buttons and +/- don't change it ("speed N refused" logged once).
   - Space and the pause button don't pause ("the player's pause refused" logged once).
   - Unpausing after an event popup's automatic pause works.
   - After 2 in-game days the speed from before comes back ("ended (its time is up)").
3. **Lock until a date**, set a few days ahead with the decision's date. It ends at that date
   and hour; compare with the top bar.
4. **Lock with `lock_pause = no`.** Pausing works; the speed stays.
5. **Taken while paused.** The game unpauses and runs at the locked speed.
6. **Release.** Speed from before, pausing works.
7. **Hide the date.**
   - The date text disappears, at once also while paused.
   - It stays hidden as the hours pass.
   - "Show" brings it back.
8. **Save and load** with a lock and a hidden date.
   - Both come back; the game stays paused until unpaused.
   - "restored from the save" in the log.
9. **Save without the loader.** The same save, game started without `version.dll`: it loads, runs
   at normal speeds, the date shows.
10. **Thread evidence.** The four "runs on thread" lines all say "the main thread".

### K3 results (2026-11-11, first run, new 1936 game as single player)

- **Passed:**
  - **Step 1.** Both resolver lines are as expected (5 speeds, speed +0x4bc, slots 72 / 94, paused
    +0x6c1; DateText +0x80, hide 16 / show 15, flags +0x75 bit 0x10, date +0x468, base 43800000).
    The feature is active, all four commands registered, and the guard says yes.
  - **Step 4.** `speed = 3 hours = 36 lock_pause = no` (taken at 1936.1.2.4):
    - locked until 1936.1.3.16;
    - "speed 2 refused" when the player tried to slow down;
    - ended at that hour ("its time is up"), back to the previous speed 4.
  - **Step 7, partly.** The date text is hidden and shown again at once. The black slot, the
    pause / −/+ buttons and the speed steps stay (screenshot from the user).
  - **Step 10.** UpdateDate, TogglePause, SetGameSpeed and the script effects all run on the
    main thread (thread 20656, the frame hook's).
- **Not run yet:** steps 2, 3, 5, 6, 8 and 9 (pause blocking, until a date, force while paused,
  release, save/load, save without the loader).
- **User request after the test:** cover the time panel too. Added `hide_time_controls`
  (2026-11-11, K1.3 "The whole time panel"; one more SIG, one more CODE shape; global flag
  `hoi4_loader_time_controls_hidden`). To test:
  - "Time: hide the time panel": the whole panel goes, at once, also while paused, and stays
    hidden as the hours pass;
  - "show" brings it back;
  - save/load keeps it;
  - whether Space and numpad +/− still act while it is hidden.

  A cover graphic over the empty frame would need the `.gui` patch API of PR #3 (later).

### K3 results (2026-11-11, second run, with `hide_time_controls`)

- **`hide_time_controls`:** the whole panel hides and shows at once (user: OK).
- **Step 2, passed.** `speed = 5` until released, pausing blocked (taken at 1936.1.1.13, speed
  was 1):
  - the speed buttons and numpad +/− were refused ("speed 4 refused");
  - pausing was refused ("the player's pause refused").
- **Step 8, passed for the lock.**
  - The save loaded with "restored from the save: speed locked at 5".
  - After loading, the speed change and the pause were refused again.
  - The panel was shown when saved. Save/load with it hidden: user OK.
- **Thread evidence again:** everything on the main thread (thread 24192).
- **Still open:**
  - step 3 (until a date);
  - step 5 (force while paused);
  - step 6 (release; the trigger `has_forced_game_speed`);
  - step 9 (the save without the loader);
  - whether Space / numpad +/− act while the panel is hidden *without* a lock.

## K4 after the #5 review (2026-11-12, fixed on main)

- **Lock that never ends without the game date.** `ResolveDate` accepted the top bar's date format
  when `kSigGameDate` had not resolved (`GameDateOffset() == 0`). `GameDateHours()` then always
  returned 0, so a timed lock (`days`, `hours`, `until`) never ended and the player stayed at that
  speed with pause blocked, also after loading. Now the game date is required: without it the
  calendar is off and `TimeControlInit` fails (time_control off, logged).
- **Stale single-player decision.** `g_sessionOk` was set in `OnEntered` and never reset, but
  `on_startup` runs inside the game's own entry, before the callbacks. After a single-player game,
  `force_game_speed` / `hide_date_display` in the next game's `on_startup` (e.g. hosting a
  multiplayer game in the same session) passed with the old `true` and wrote global flags on one
  machine only. Now lifecycle has `OnGameEntering` (before the game's entry; it also forgets the
  previous session), and time_control sets `g_sessionOk = false` there. `on_startup` effects do
  nothing, as MODDING already said.
- Not changed (smaller points of the review, for later): the lock stays set in the main menu until
  the next entry; the fault path doesn't restore the previous speed; speed 10 can't be saved;
  two mode-1 pause callers unexamined; `ResolvePanel`'s memcmp without SEH.

# Part L: air scenarios (`air_scenarios`)

Branch `feature/air-scenario-variants` (from `main`). Investigation only, nothing implemented.
Revised on 2026-11-13 after GloomyJZ named what is needed (L2): new scenarios per mission and equipment.
Feature request from GloomyJZ (Discord, 2026-11-11): air scenarios, the 3D animations on the map
for air missions (patrols, dogfights, bombing runs, CAS), can't be extended: their list is in
the exe. Addresses are from vanilla **1.19.4.2** and only for orientation.

## L1 findings

### L1.1 What a scenario is (game files)

- **A scenario is an entity** named `<scenario>_entity` (`gfx/entities/units_planes.asset`),
  e.g. `fighter_1_vs_fighter_1_entity`. It uses a pdxmesh `<scenario>_mesh`
  (`gfx/entities/planes.gfx`): "empty animated joints that we attach above meshes". The
  attachment nodes are the joints, e.g. `fighter_1`, `fighter_2`, `bomber_1`, `defender_1`.
- **Its states are the outcome:**
  - `default` (patrol, bombing, supplying);
  - `pass` / `success` / `fail` (fights).

  Their events fire the planes' own states, e.g. `a_fire`, `d_crash`.
- **Mods can already replace** a vanilla scenario's entity and mesh (same name), but not add one:
  the game loads exactly the 21 names below.

### L1.2 The exe

- **Name table** (`.data`, writable, `0x14312ae30`): 21 `char*`, index 0-20:
  - `fighter_1_patrol`, `fighter_3_patrol`, `scout_1_patrol`, `scout_3_patrol`;
  - `bomber_1_CAS`, `bomber_3_CAS`, `bomber_1_bombing`, `bomber_3_bombing`;
  - `bomber_1/3_carpetbombing`, `bomber_1/3_firebombing`;
  - `fighter_1_vs_bomber_1`, `fighter_1_vs_fighter_1`, `fighter_3_vs_fighter_3`;
  - `missile_1_bombing`, `fighter_1/3_kamikaze`, `fighter_1_vs_transport_1`;
  - `transport_1/3_supplying`.

  After it, the result names (`0x14312aed8`): `default`, `pass`, `success`, `fail`. Besides the
  template loader, only the console command `airc` reads the tables: name to index
  (`FUN_1412db3c0`, `0x15` = not found) and the help text.
- **Templates at startup.** The manager's constructor (`FUN_1412d0e50`, one caller) loops
  `i = 0 .. 0x15`:
  - `template = new(0x40)`, then `FUN_1412cfcc0(template, i)`;
  - the result goes to `manager+0x48 + 8*i`.
- **The template loader** (`FUN_1412cfcc0(template, index)`):
  - `template+0 = index`; looks up the entity `table[index] + "_entity"` through the entity
    database (`FUN_1422ff090()` → `FUN_142301290(db, name)`, a plain hash lookup that logs
    nothing) into `template+8`;
  - null logs "Failed to read airplane scenario entity: <name>" to error.log;
  - then a **hardcoded `switch (index)`** lists the attachment node names per side and role:
    attackers at `template+0x10` (count `+0x1c`), defenders at `+0x28` (count `+0x34`);
  - the node analysis logs "Airplane animation analyze - failed to find attachment node".
- **Playing a scenario:**
  - `FUN_1412d1e40(manager, ..., index, ...)` allocates the instance (0x88 bytes):
    `instance+0 = manager`, `instance+8 = manager+0x48[index]` (the template);
  - then `FUN_1412db6c0(instance, ...)` instantiates the template's entity
    ("Failed to instantiate scenario entity" if not) and attaches the planes to the template's
    nodes.
  - Two callers (`FUN_1412dc9a0`, `FUN_1412dcc20`). Which index a mission uses, and how many
    planes per side (the `NAirGfx` defines `AIRPLANES_1_..._ANIM` / `AIRPLANES_3_...`), is decided
    before that.
- **Graphics only.** Scenarios read the game state but never write it. A different animation on
  each machine changes nothing in the game.

### L1.3 Which scenario a mission plays, and the planes' equipment

- **The slot comes from the call site.** The game starts a scenario through
  `FUN_1412dc9a0(manager, index, ...)` from 24 call sites in the mission handlers
  (`0x1412cf5e5` ... `0x1412d82eb`) and once through `FUN_1412dcc20` (the console command,
  `0x1402a2d99`). Each site passes its own index, mostly a constant:
  - CAS: `bomber_1_CAS` (4);
  - scouting: `scout_1_patrol`;
  - V2: `missile_1_bombing`;
  - kamikaze: 16 / 17;
  - interception: `fighter_1_vs_bomber_1`;
  - dogfights: 13 / 14;
  - air supply: 19 / 20;
  - transport interception: 18.

  The `_3_` slots follow from the plane count. So "a scenario for CAS missions" means "a
  scenario for the slot `bomber_1_CAS` (or `bomber_3_CAS`)".
- **The equipment is known when the scenario starts.** `FUN_1412db6c0(instance, ..., attackers,
  defenders, ...)` gets one list per side. For each attachment node it takes an element of its
  side's list and passes it to `FUN_1412d1820` → `FUN_1412d8640` ("Failed to load entity of
  internal airplane scene, no equipment type"). That function picks the plane model by the
  `<tag>_<equipment>_entity` naming rule. So the elements are the planes' equipment. Still to
  confirm in code: whether an element is the equipment type or the design, and how to read its
  name (the loader already reads type names for equipment_conversion).
- **The mission handlers pick the planes with their own random numbers** (`param_1[1]`,
  graphics state, not the game's synced random). Nothing here writes game state.
- **No free-form data in entities:** `game_data` in vanilla `.asset` files only holds known keys
  (`hitmiss_effect`). A definition therefore needs its own file.
- **The mod list:** the launcher writes `Documents\Paradox Interactive\Hearts of Iron IV\
  dlc_load.json` (`"enabled_mods": ["mod/<name>.mod", ...]`, in load order). Each `.mod` file
  names the mod's folder (`path=`). That is what the game itself loads, so the loader can read
  the mods' definition files with plain file reads, without new engine signatures.

## L2 What is possible (after GloomyJZ's requirements, 2026-11-13)

GloomyJZ (who animates planes for a submod) needs:
1. **new air scenarios** (their own entity, mesh and animation);
2. **assigned to a mission type:** e.g. only for CAS missions;
3. **limited to equipment:** e.g. only for `CAS_gunship_unique_1`, so a gunship's orbiting
   animation never plays for ordinary CAS planes, and ordinary CAS planes don't use it.

All three are possible:
- **Mission type = the game's slot.** A new scenario names the slot it may play in, e.g.
  `bomber_1_CAS`. It needs that slot's attachment nodes, because the game decides the plane
  count and sides per slot (the template's node layout is hardcoded per index, L1.2).
- **Equipment:** the start of a scenario sees the attacking planes' equipment (L1.3), so the
  loader can choose a scenario whose equipment list matches.
- **New scenario:** the loader builds a template for the new entity with the game's own
  template loader (the slot's index, with the entity's name in the name table for that one call)
  and puts it into the scenario instance instead of the vanilla template.

Still not possible: new slots, i.e. scenarios for missions the game has no scenario for
(e.g. naval strike, port strike), or other plane counts than the slot's.

## L3 Proposal (revised; replaces the first variants-only idea)

**Feature `air_scenarios`** (engine tier, needs `lifecycle`; no script commands). It does nothing
until a mod defines scenarios.

**Definition** (in a mod, one or more files):
```
# common/hoi4_loader_air_scenarios/my_gunship.txt
gunship_orbit = {
    entity = gunship_orbit_entity          # the scenario's entity (the mod's gfx/entities/*.asset)
    slot = bomber_1_CAS                    # where it may play: the game's scenario (mission + plane count)
    equipment = { CAS_gunship_unique_1 }   # optional: only for these equipment types (or archetypes)
    weight = 10                            # optional: chance against other matching scenarios (default 1)
}
```

The entity is written like vanilla's (e.g. `bomber_1_CAS_entity` in `units_planes.asset`): a
pdxmesh with the slot's attachment nodes and the slot's states (`default` for CAS, patrol and
bombing; `pass` / `success` / `fail` for fights).

**Behaviour:**
- **At startup:** the loader reads `dlc_load.json` and the enabled mods' `.mod` files, then every
  `common/hoi4_loader_air_scenarios/*.txt` of each mod in load order. A file of the same name in a
  later mod replaces an earlier one, as the game does.
- **When the game builds its templates:** the loader builds one per definition with the game's
  own loader. Kept only if its entity loads and its node counts equal the slot's; otherwise it
  is skipped with a `loader.log` line naming the definition and the reason.
- **When a scenario starts** (slot X, attacking equipment E):
  - candidates are the definitions for slot X whose `equipment` includes every attacking plane's
    type (or its archetype), or that have no `equipment`;
  - with candidates, one is picked by `weight`; without any, vanilla plays.
- **Picking uses the loader's own random numbers.** It's graphics only, so in multiplayer
  different machines may pick different scenarios with no effect on the game.
- **Fail-safe:**
  - signatures and code shapes, cross-checked (the table count 21 from the console helper's
    bound; the loader's `lea` to the same table; the instance's template field at `+8` from the
    instance constructor);
  - SEH around both hooks; any fault logs once and returns to vanilla;
  - templates are kept for the session and never freed;
  - bad definition files are logged (file and line) and skipped.
- **Saves:** nothing is saved.

**Hooks:**
- the manager constructor (after it: build the templates);
- the scenario start `FUN_1412db6c0` (before it: swap `instance+8`).

**Testing:** the console command `airc <slot> <result> <province> <state> <state> <equipment>
<equipment> <tags>` plays a slot with chosen equipment, so a definition can be checked on demand.
The test mod gets a CAS definition limited to one equipment type, using a copy of vanilla's CAS
mesh with a different event timing.

**Effort:** about 3 days. **Risk:** low (graphics only; the game's own template loader; one field swapped).

## L4 Open

- **Decided** (L5): format B, with attacker/defender blocks. Was: approval of L3:
  - the name `air_scenarios`;
  - the folder `common/hoi4_loader_air_scenarios/`;
  - the keys `entity`, `slot`, `equipment`, `weight`;
  - **matching definitions replace vanilla**; vanilla plays only if none match. Alternative: vanilla
    takes part with weight 1.
- **Equipment matching:** "every attacking plane's type is listed" (archetype names allowed),
  or "at least one".
- **To confirm while implementing:** the element type of the equipment lists (L1.3); whether
  the manager is built once per session or per game; the thread of the scenario start.
- **Later, if wanted:** a script effect `play_air_scenario` to play a scenario (vanilla or a
  mod's) at a province from events or decisions (the console command's path).
- **Older builds:** unchecked, as for the other 1.19.4.2 features.

## L5 Agreed design and implementation (2026-11-13)

**Agreed with the owner** (after comparing a naming-rule format "A" with a definition file "B"
in two mock mods in `C:\test`): format **B**.
- Entries in `common/hoi4_loader_air_scenarios/*.txt` with `entity`, `slot`, optional `attacker`
  and `defender` blocks (`equipment` types or archetypes, `country` tags) and `weight`.
- A matching entry replaces vanilla. One matching plane per side is enough; every condition
  written must hold.
- An entry or file of the same name in a later mod replaces the earlier one.
- "Attacker" and "defender" are the animation's sides (`a_` / `d_` events): in an interception
  the fighter is the attacker and the bomber the defender, whoever started the raid.
- The `1` / `3` in a slot's name is the plane count, not a generation.

**Found while implementing:**
- **The country per side is the equipment's creator.** The plane model function reads it with
  `FUN_140c323c0(design)` (`&design->creator`, `+0x20`), and the console command `airc` takes
  "2 country tags for equipment creator". MODDING says so.
- **Each side's list holds designs.** design → type at `+0x3f0` (the same field as
  production_data's), type name `std::string` at `+0x18` (the same as equipment_conversion's),
  archetype from production_data's `+0x538`.
- **A missing joint** is not detectable from the template: the game logs "Airplane animation
  analyze - failed to find attachment node" and keeps the node without a path. That message,
  in the game's own error.log, is the report, as for a wrongly replaced vanilla scenario.
- **The name table is image-relative** (`lea rax,[image base]; mov rdx,[rax+r14*8+disp32]`). The
  loader swaps the slot's entry for its own build call only (the table is in `.data`) and
  restores it in a `__finally`.
- **1.19.5** (`v1.19.5.0.7c6c`, Steam update of 2026-11-12) has every pattern at the same
  addresses as 1.19.4.2.

**Implemented** (`src/engine_air.cpp`, `src/air_scenarios.cpp`, `src/plugin_air_scenarios.cpp`):
- **Signatures:** `kSigAirTemplates` (the manager's template loop: the allocator, the loader, the
  slot count) and `kSigAirSpawn` (the common spawn path: the instance constructor and the
  scenario start).
- **Code shapes,** each cross-checked against the next:
  - the loader's fields, entity database and name table;
  - the entity lookup;
  - the instance's template field (the same as the start's);
  - the start;
  - the plane model function among the start's calls, the creator getter and the design's type.
- **Hooks:**
  - the template loader: after the game's last template, the plugin builds its own once per
    session (re-recording the game's templates if the manager is rebuilt);
  - the scenario start: before it, the instance's template may be swapped. Only the game's own
    template for that slot is replaced.
  - SEH around both; a fault switches to the game's scenarios.
- **Definitions** are read when the templates are built: the game folder, then each mod from
  `dlc_load.json` (descriptor `path`, absolute or relative to the documents folder;
  `replace_path` supported; zip `archive` mods logged as unreadable), in file name order. The
  parser, mod list, matching and weighted pick have no engine access and a unit test
  (`tests/air_scenarios_test.cpp`, 32 checks, smoke test step 0a2).
- **An entry is skipped** (logged with file and line) on a syntax error or unknown key, a missing
  entity or slot, an unknown slot or entity, a `defender` on a slot without defenders, or a
  template whose node counts differ from the slot's.
- **Picking** uses the loader's own random numbers (graphics only).
- **Checks:**
  - `check_signatures.py`: 111/111 on 1.19.4.2 and on 1.19.5;
  - `check_code_shapes.py`: 62/62 on both;
  - build clean; unit tests and smoke test pass.
- **Test mod:** four entries from `C:\test` format B (gunship orbit, long chase, head-on
  weight 3, interception with a defender block), using vanilla meshes and animations.

## L6 In-game test (passed 2026-11-13: real dogfight and CAS)

Single player, test mod enabled, 1.19.5. Start any game and use the console:
1. **Load:** `loader.log` has:
   - `air_scenarios: 21 slots ...`;
   - at game start, "4 definition(s) from 1 file(s)";
   - one line per entry ("hlt_air_gunship_orbit: hlt_air_gunship_orbit_entity plays in
     bomber_1_CAS ...");
   - "4 new air scenario(s) ready".
   `error.log` has no "failed to find attachment node" for them.
2. **CAS:** `airc bomber_1_CAS default 11471 126 29 small_plane_cas_airframe_1
   small_plane_cas_airframe_1 GER FRA`: the plane circles and fires in bursts (instead of
   vanilla's dive). With `small_plane_cas_airframe_2`: vanilla's dive.
3. **Dogfight weights:** `airc fighter_1_vs_fighter_1 pass 11471 126 29 fighter_equipment_1
   fighter_equipment_1 GER FRA`, several times: mostly head-on (both fire almost all the time),
   sometimes the long chase. With `ITA` as the first tag, only the long chase (head-on needs GER).
4. **Interception with a defender block:** `airc fighter_1_vs_bomber_1 success 6103 126 29
   fighter_equipment_0 strat_bomber_equipment_1 GER ENG`: the bomber fires back. With `GER GER`
   (the bomber's creator isn't ENG/USA): vanilla's interception.
5. **Normal play:** a few days of an air war with CAS or fighters; nothing breaks, and
   `loader.log` shows "plays for the first time" if a matching plane flies. With By Blood Alone
   designs, the test entries only match through archetypes (the long chase), because CAS designs
   are `small_plane_airframe` types.
6. **Without the loader** (`version.dll` renamed): vanilla scenarios, no new error.log lines from
   the test mod's air files beyond unused entities.

### L6 results (2026-11-13, 1.19.5, single player, Germany 1936 at war)

- **Step 1 passed:** 4 definitions read and built while the game loaded; no error.log lines.
- **`airc` doesn't work in the Steam build.** The console allows a command only if the command
  carries an "allowed in release" flag (`+0x88` bit 0) or a release-build check says no; `airc` has
  no flag, and the retail exe always reports a release build, `-debug` or not ("Command available
  only for developers."). Steps 2-4 were done in normal play instead.
- **Real dogfight:** "a scenario starts on thread 7032" (the main thread), then
  `hlt_air_long_chase plays for the first time (fighter_1_vs_fighter_1, 1 candidate(s))`: a mod
  scenario replaced vanilla's in a real air war. Head-on didn't match (not German
  `fighter_equipment_1`), as expected.
- **CAS, first try:** the owner saw a CAS loop, but `loader.log` showed no entry playing: the test
  entry matched only `small_plane_cas_airframe_1`. A second try with the archetype
  `small_plane_airframe` (a wrong guess) didn't match either. The new log line (once per slot, when
  a slot with entries starts a scenario no entry matches) named the planes:
  `small_plane_cas_airframe_0 (small_plane_cas_airframe, GER)`. Both CAS animations seen so far
  were vanilla's. The test entry now uses the archetype `small_plane_cas_airframe`.
- **CAS, third try: passed.** `hlt_air_gunship_orbit plays for the first time (bomber_1_CAS, 1
  candidate(s))`; the owner saw the CAS plane circle the battle and fire in bursts, as the entity
  says. With the long chase earlier, a mod scenario has now replaced vanilla's in two slots in
  real play.
- **Not seen in play:** head-on's weight (needs German `fighter_equipment_1` attackers; the owner's
  fighters are designer types) and the interception's defender block (needs British or American
  heavy bombers). Both rest on the matching and pick code the unit test covers.
