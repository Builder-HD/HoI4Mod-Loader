# hoi4_loader roadmap

Triage of the suggestion list received on 2026-10-15. Status is one of **done**, **partly
done**, **superseded** or **open**. Effort and risk are rough: S = hours, M = a day or two,
L = several days, including in-game testing.

| # | Suggestion | Status | Effort / risk |
|---|---|---|---|
| 1 | MP: host-decides lockstep through an existing networked command | open | L / high |
| 2 | Incremental flag hook (update from the hooked token) | superseded | S / low, little gain |
| 3 | Static C++ runtime | done | — |
| 4 | Chain-load another `version.dll` | open | S-M / medium |
| 5 | Native parameters optional when the scope provides them | open, scheduled in E | S-M / low |
| 6 | Bump `hoi4_loader_version` or document the guard triggers | partly done | S / low |
| 7a | Release: opt-in engine tier on unlisted builds | open | M / high |
| 7b | Release: per-feature gating instead of per-build | partly done | M / medium |
| 7c | Release: public signature checker | partly done | S-M / low |
| 8 | Plugin ABI: versioned C ABI, plugin DLLs, trust model | open | L / high |
| 9a | Requirements: mods declare needed features; main-menu warning | open | M / low-medium |
| 9b | Requirements: in-game guard warning | partly done | S / low |
| 10a | Distribution: published hashes / code signing | open | S (hashes) / L (signing, cost) |
| 10b | Distribution: stub pack for mods | partly done | S / low |
| 10c | Distribution: Proton notes | open | S / low (untested) |
| 10d | Distribution: compatibility and deprecation policy | open | S / low |

## Notes per item

**1. MP lockstep through an existing command (open, L / high).** This is a better variant of
NOTES D2 "Option 3". Option 3 needed a custom `CCommand` class; this reuses a command the engine
already networks (decision activation, or a scripted GUI effect).
- **How:**
  1. The loader serves a hidden decision or scripted GUI as a virtual file (file tier).
  2. The host posts its activation command: `CDummyServer` or `CNetworkServer`, not a hot join,
     loaded from a save.
  3. The command carries a native effect, which runs on every machine in the same tick. That
     effect fires `on_game_loaded` and refreshes the plugin flags.
  4. It would replace the single-player path too.
- **Engine work:**
  - find the "post command" call (`CSession`) and the constructor or arguments of the
    decision-activation or scripted-GUI command;
  - the command must name a country the host controls.
- **Open questions (all need an MP test):**
  - Do commands execute while the game is paused right after loading? Players can take
    decisions while paused, which suggests yes.
  - Does the host re-enter "game entered" on an OOS resync? If so, a saved "fired this session"
    marker (e.g. a global flag with the session's load counter) is needed.
  - What happens to hot joiners arriving while the command is in flight? They should receive
    the post-command state, but the engine's replay must be checked.
- **Risks:** an OOS if any part of the effect depends on local state, and a command visible
  in the UI (decision history, tooltips) unless it is hidden well.
- Depends on step 10 (MP test) for the baseline log data.

**2. Incremental flag hook (superseded).** Since `0adfb50` the hooks do no reading at all and
only set a dirty bit. The full scan runs at most once per frame, and only after a `hoi4_srn_`
flag changed.
- Incremental updates would need a lock-free token queue from the hooks and still a full scan
  for "most recent flag wins".
- The scan is a single pass over the global flags (tens to a few thousand) at a rare moment.
  Not worth it unless a profiler says otherwise.

**3. Static C++ runtime (done).** `CMakeLists.txt` sets
`CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"`, so `version.dll` depends
on system DLLs only.

**4. Chain-load another `version.dll` (open, S-M / medium).** Today the exports are forwarded to
`System32\version.dll` (`LoadRealVersionDll`).
- **Change:** a `[chain] dll = version_other.dll` line in `loader_config.ini`. The loader loads
  that file (from the game folder) instead and forwards to it; the other proxy forwards on to
  System32 itself.
- **Risks:**
  - it runs inside `DllMain` under the loader lock, the same as today's `LoadLibraryW`, but now
    with foreign code whose `DllMain` may do anything;
  - two tools hooking the same functions (`CreateFileW`) conflict in ways MinHook can't detect.
- Log the chained module's path and version. Fail safe: if it fails to load, fall back to
  System32.

**5. Parameters from scope (open, part of E).** Scheduled with the data triggers: a general
"default from scope" for any parameter. It applies to existing commands too, e.g.
`set_strategic_region_name = { name = X }` inside a strategic region scope. See NOTES E1, "API
proposal".

**6. Version bump vs guard triggers (partly done).** README "Checking for features in script"
already tells mods to check `hoi4_loader_feature_<name>`, which never goes stale. The version is
still 2 although `game_loaded` was added since.
- **Proposal:** a policy line in the README. The version goes up when the script-visible
  contract of existing features changes (renamed or removed commands, changed semantics). New
  features don't bump it, because the guard triggers announce them.

**7a. Opt-in engine tier on unlisted builds (open, M / high).** "Every signature matches
exactly once" is necessary but not sufficient.
- Several features also read **offsets** out of the matched code (`flags +0x258`, regions
  `+0x2e0`, `gamestate+0x520`, ...). Those can change while the bytes around them still match,
  because they're wildcarded.
- Only `region_names` has a semantic self-check. Others would need one each.
- **If done:** opt-in per user (`[engine] allow_unlisted = 1`), a loud warning in `loader.log`
  and in game, and feature by feature only where a self-check exists.

**7b. Per-feature gating (partly done).** Each feature already stays off when its own
signatures fail and has its own guard trigger; that is the "one broken signature disables one
group" property. What's missing is gating by something finer than the exe hash. That only
matters together with 7a.

**7c. Public signature checker (partly done).** `tools/check_signatures.py` exists but needs
Python and a copy of the exe.
- **Small step:** on an unlisted build, let the loader itself scan all signatures read-only and
  log "N/M signatures match exactly once". It patches nothing. Users can then report new hashes
  with that log.

**8. Plugin ABI (open, L / high).** Today features are compiled into the DLL.
- **Needs:**
  - a versioned C ABI over the native script API, the VFS, lifecycle and `Engine` access;
  - a `plugins\` folder next to the exe;
  - forced command prefixes (e.g. `modname_`);
  - generated guard triggers per plugin.
- **Trust model:** DLLs installed by hand only, never loaded from mod folders or Workshop
  content, because a Workshop mod must not be able to run native code.
- **Risks:** ABI freeze; every engine change becomes a compatibility question; and support load
  for third-party crashes, which SEH containment only partly limits.

**9a. Declared requirements and a main-menu warning (open, M).**
- **Feasible with the file tier:** a mod ships a main-menu text or GUI element saying "requires
  hoi4_loader with features X, Y". The loader serves a virtual replacement: empty, or "loader
  active, features OK / missing: Z".
- The declaration could be a small file in the mod (`hoi4_loader_requires.txt`) that the loader
  reads through the VFS observer.
- Needs a GUI or localisation key the main menu shows reliably (to be found).

**9b. In-game guard warning (partly done).** Guard triggers exist; the test mod shows the
pattern (decisions and events gated on them). Missing: a documented snippet ("show an event at
game start if a required feature is off").

**10a. Published hashes / code signing.** Publishing SHA-256 hashes of releases is S.
Authenticode signing needs a certificate (cost, identity), so it's the user's decision.

**10b. Stub pack (partly done).** The test mod contains every stub. Missing: a standalone
folder (`stubs/`) with only the stub files, and a README table.

**10c. Proton notes (open, S, untested).** `WINEDLLOVERRIDES="version=n,b" %command%` in Steam
launch options should make Wine load the game-folder `version.dll`. Untested; it needs a Linux
tester. The SEH, MinHook and CreateFileW paths should work under Wine, but that's unverified.

**10d. Compatibility and deprecation policy (open, S).**
- Commands are never removed or changed within a major loader version; they're deprecated
  with a log line for one version first.
- Guard triggers stay available forever (`no` when gone).
- To be written down together with item 6.
