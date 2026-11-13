# Changelog

Release versions name the DLL package. The script version is the number script reads as
`global.hoi4_loader_version` (MODDING.md "Versions and compatibility"). Development went in parts
A–F; NOTES.md has the findings and test results behind each.

## Unreleased

- **Game 1.19.5** (`v1.19.5.0.7c6c`, Steam update of 2026-11-12) is supported: its hash is in
  `loader_config.ini`. All 109 signatures and anchors and all 59 code shapes match (checked
  offline on a copy); no code changes were needed.
- **Game 1.19.2** (vanilla and checksum-patched) is now listed in `loader_config.ini`, from the
  hashes recorded in October. Features added since were not re-checked there; any whose
  signatures don't match stay off.
- **Railway rules mod removed from the repo:** `mod/hoi4_loader_railway_rules` is no longer part
  of the loader or its release zip; the global flags it set stay documented in MODDING.md.
- **New feature `time_control`** (engine tier, single player; guard
  `hoi4_loader_feature_time_control`; script version unchanged). NOTES.md Part K.
  - `force_game_speed = { speed = 1-5 days = <n> hours = <n> until = "y.m.d.h" lock_pause = yes }`
    locks the game speed: every speed change (buttons, keys, console) gives that speed, for a
    time, until a date, or until `release_game_speed = yes`. With `lock_pause` (default yes) the
    player cannot pause meanwhile; automatic pauses and the Esc menu still pause. When the lock
    ends, the speed from before comes back. Trigger `has_forced_game_speed`.
  - `hide_date_display = yes / no` hides or shows the date and time in the top bar.
  - `hide_time_controls = yes / no` hides or shows the whole time panel (date, pause button,
    speed buttons and steps).
  - Saved as global flags `hoi4_loader_time_lock`, `hoi4_loader_date_hidden` and
    `hoi4_loader_time_controls_hidden`; a save loads without the loader at normal speeds with
    everything shown. Effects do nothing in multiplayer.
  - Hooks four engine functions (DEVELOPING.md "Global hooks"); seven new signatures (53 in all).
    Checked on 1.19.4.2 only; 1.19.3 is unchecked.
  - **Fixed after the #5 review:** without the game date (`kSigGameDate`), time_control now stays
    off (a timed lock would never have ended). The single-player decision is dropped before each
    game is entered, so `on_startup` of the next game (e.g. a multiplayer game after a single
    player one) can't use the previous game's and write flags on one machine only.
- **Equipment conversion tools** (`equipment_conversion`, engine tier). **Off until a mod sets
  their global flags:** without them conversion is the base game's, AI included. NOTES.md Part J.
  - `hoi4_loader_conversion_generations`: a production line's "Convert from Stockpile" also takes
    every earlier generation of the equipment (its `parent` chain), for every archetype and mod,
    without `can_convert_from` entries. The pairs are reserved in the game's lists at load and
    shown only while the flag is set. Flag changes count at game entry, and in single player also
    mid-game; in multiplayer at game entry only.
  - Cost: two or more generations back, the game's conversion cost times a factor per generation
    (`hoi4_loader_conversion_factor_<percent>`, default 75); `..._existing_loader_cost` applies
    it to pairs the game already allows; `..._naval` allows earlier ship hulls.
  - `hoi4_loader_conversion_role_mode`: Shift+click on a line's convert button (single player)
    limits that equipment type's lines to sources of the same role (no CAS used up by a fighter
    line); the tooltip shows the mode and, greyed, the designs it leaves out.
  - The separate mod [HoI4Mod Loader: Equipment Conversion](https://github.com/ni1saux/HoI4Mod-Loader-Equipment-Conversion)
    sets the flags from game rules at `on_startup`; existing saves opt in or out from the console with
    `event hoi4_loader_equipment_conversion.1` / `.2`. Test decisions in "HOI4 Loader: Equipment
    Conversion" (including opt in / opt out).
  - Eight signatures and three anchors (109 in all). Checked on 1.19.4.2 only; 1.19.3 is unchecked.
- **Game 1.19.4.2** ("Operation Postern", Steam, as distributed) is supported: its hash is in
  `loader_config.ini`. Every signature matches; three create_army code shapes changed and were
  adapted (NOTES.md "1.19.4.2 added"). 1.19.3 stays supported.
- **`province_building_removal` feature (engine tier, single player):** the state view's province
  buildings (supply hub, naval base, bunkers, ...) get vanilla's red removal cross, as dockyards and
  factories have in the shared slots. A click asks with vanilla's "Remove Building" dialog, and OK
  removes one level as vanilla's removal does: supply follows for hubs, the wartime removal cooldown
  applies and is started, and the tooltip is vanilla's. Shown only where the player owns the state
  (as vanilla requires) and controls the province. Hidden in multiplayer.
  - The cross is a `remove` button the loader adds to `province_building_entry` by patching
    `interface/countrystateview.gui` as the game reads it, whichever copy (vanilla's or a mod's).
  - New virtual file API call `register_file_patch(suffix, patcher)`: patches each existing copy
    of a file separately instead of replacing them all with one generated file.
  - 14 new signatures and anchors (98 in total) and 7 code shapes, found on 1.19.4.2 like
    the railway ones (NOTES.md Part I). New out-of-game test `gui_patch_test` (run by
    `run_smoke.sh`).
  - Test mod: "HOI4 Loader: Province buildings", vanilla-only decisions that remove, damage and
    rebuild the Berlin and West Prussia hubs.
  - Built to the PR #2 review's rules: the hooks run under SEH and a fault switches the feature
    off for the session; the item and window vtables are checked before use; the hooks only act
    on the main thread (the first call logs its thread); nothing of the loader's own is saved.
    After a fault, crosses stay hidden in state views opened later too. If init fails after some
    hooks were queued, they are taken out again (new `HookUnqueue`).
  - The file patch reads `CreateFileA` paths in the ANSI code page.

- **Disclaimer** at the top of the player guide: unofficial, not affiliated with or endorsed by
  Paradox Interactive, runtime only, use at your own risk. The 0.5.0-beta zip's PLAYERS.md does
  not have it yet.
- **Issue forms** on GitHub: a bug report asking for the loader release, game version and build,
  logs, mod list and steps, plus a form for questions and requests.
- **Docs for three audiences.**
  - README.md is the player guide: the disclaimer, a link to the latest release, install,
    uninstall, game patches, reporting problems (GitHub issues or Builder_HD on Discord), and
    the supported game version. It absorbs PLAYERS.md, which now only points there.
  - MODDING.md has everything script-facing: guards, the stub pack, the warning snippet, the
    version policy and every command.
  - DEVELOPING.md covers the build, the plugin API, threads and hooks, the layout, configuration
    and diagnostics, the signature report, the smoke test and the release process.
  - The release zip ships README.md, MODDING.md and DEVELOPING.md instead of PLAYERS.md, and
    `tools/make_release.py` also writes the GitHub release notes.
  - Help texts the DLL writes into `loader_script_docs.md` point to MODDING.md instead of the
    README.
- **New feature `add_to_army`** (engine tier, needs `create_army`; guard
  `hoi4_loader_feature_add_to_army`; script version unchanged). NOTES.md G4.
  - `add_to_army = { leader = <character> name = "..." }` in a division scope (e.g. inside
    `every_country_division`): the division joins the army that general leads, through the
    game's own assignment (as the player's "assign to army"); an old army left without divisions
    is disbanded. If the general leads no army, a new one is formed from the division, named
    `name` if given, and the next divisions join it.
  - The general must be available as for `create_army`. Also refused when the division's army
    (the game's own rule) or the general's army (stricter than the game) is in a border war.
    The tooltip shows the reason in red; `loader.log` logs a repeated line once, then counts it.
  - No command power, no front orders. New code shapes (no new signatures); checked on 1.19.4.2
    only, 1.19.3 and 1.19.2 are unchecked (no copies).
- **New feature `create_army`** (engine tier; guard `hoi4_loader_feature_create_army`; script version
  unchanged, as for every new feature).
  - `create_army = { leader = <character> name = "..." reassign = yes country = <tag> create_unit = { ... } ... }`,
    in a state scope like vanilla `create_unit`, spawns divisions there with `create_unit` and
    makes them a new army of `country` (default ROOT) under that general.
  - If the general is not available, nothing spawns: the tooltip shows only a red line (no
    `create_unit` lines) and `loader.log` says why. A general who already leads an army is
    refused unless `reassign = yes`.
  - With `reassign = yes`, a general whose army can't change its general now is refused as the
    game's own button refuses. The reason has its own key per case:
    `HOI4_LOADER_CREATE_ARMY_DEPLOYING`, `_WITHDRAWING` and `_BORDER_WAR`.
    `HOI4_LOADER_CREATE_ARMY_CANNOT_LEAVE` is now only the fallback ("their army ... cannot change
    its general now"); it used to cover deploying and withdrawing together.
  - No command power is charged. It works on 1.19.4.2 and on 1.19.3 vanilla and checksum-patched,
    and its signatures also match 1.19.2.
- **Native API:** effect blocks (`register_native_effect_block`), whose children run inside the
  plugin's execute (their tooltip callback can hide the children's lines), and a `Text`
  parameter type for free text such as army names.
- **`railways` feature (engine tier)** with the native effect `downgrade_railway = {
  start_province = A target_province = B levels = N }`: lowers every link of the existing railway
  with the fewest links between A and B, on both ends, with the rail_way buildings and the supply
  system updated as the engine does when it builds one. Vanilla script can only raise railways.
  - A self-check on game entry compares every stored link with its other end; on any mismatch
    the feature switches itself off.
  - 7 new signatures (51 in total). They were found on 1.19.4.2, which is not a listed build;
    on 1.19.3 they are unchecked (NOTES.md Part H).
  - Test mod: a "HOI4 Loader: Railways" category with downgrade, remove, no-path and rebuild
    decisions on the Brandenburg to West Prussia railway.
  - `levels = all` removes the whole route; `all_routes = yes` removes route after route until no
    railway connects the two provinces. `queue_railway_dismantle` takes `levels = all` too.
  - Native trigger `has_railway_between = { start_province target_province min_level }`: any
    railway route between two provinces, optionally only over links of at least `min_level`.
    Test mod: "remove every route" and "level 3 or 4 connection?" decisions.
  - Dismantling cost is now 30% of building by default (was a third) and follows a global flag
    `hoi4_loader_railway_dismantle_cost_<percent>`.
  - **Fixed:** without a mod defining the title marker keys, railway entries showed
    "£decrease_texticon£" as text instead of the triangle: the engine converts the "£" icon
    delimiter when it loads localisation, so the built-in markers now take it from vanilla's
    `TRIGGER_FULLFILLED_PREFIX` ("£trigger_yes£"); without it titles stay unmarked.
  - Native script API: an `Int` parameter can accept words next to numbers (`ParamSpec::choices`,
    read with `IntWordIndex`).
- **`railway_queue` feature (engine tier, needs `railways`)** with the native effect
  `queue_railway_dismantle`: the same lowering as an entry in the scope country's construction
  queue, one level at a time, each level at a third of the cost of building it.
  - The entry is a railway construction line whose target is below its starting level; hooks on
    the line's daily step, progress and finished check run it. Normal railway lines are unchanged.
  - 10 more signatures (61 in total) and 3 code shapes (20); `tools/check_code_shapes.py` checks
    the new shapes.
  - Test mod: "queue dismantling one level" and "queue removing it" decisions.
  - The queue entry's tooltip says "Dismantling" when a mod defines
    `HOI4_LOADER_RAILWAY_DISMANTLE_TOOLTIP` (hooks on the entry's tooltip builder and on
    Localize; one more signature and one anchor), and railway entries' titles start with a red down triangle (dismantling) or a green up triangle (building, upgrading) (hooks on the entry update and SetText; two more signatures).
  - **Alt+Shift on the railway map:** in the railway construction view, holding Alt+Shift shows a
    red X in each railway segment's level circle; Alt+Shift+click queues one dismantle level for
    that segment, a further click deepens it (single player only). Four more signatures. (First
    Shift alone, which took over vanilla's Shift+click upgrade to the maximum level; Alt+Shift
    only takes over vanilla's "maximum level, last in the queue".)
  - **Fixed a crash** after a railway was removed (seen after loading a save): the railway graphics
    (pieces, trains, bridges) are now cleared and rebuilt after every removal, as the game does after
    loading. One more signature.
  - A queued or deepened dismantle shows in the construction queue at once (the construction view
    gets the build command's refresh event). One more signature.
  - **Where the player may dismantle:** every province of the segment must be controlled by the
    player's country, or (per global flag) by a faction member, a subject or the overlord. The test
    mod sets the flags from three game rules. A queued entry is cancelled once that no longer holds.
    Three more signatures, 74 in total.
  - **After review (PR #2):**
    - When `railways` switches itself off (failed self-check), `railway_queue` goes off with it.
      Queued dismantle entries pause (no progress, not finished, kept in the queue and the save),
      nothing changes railways any more, and the badges are vanilla.
    - The railway graphics rebuild runs only on the main thread (deferred otherwise). The first
      daily step logs its thread as evidence.
    - The output getter's slot and the badge's hidden flag are read from the code (one more
      signature; the badge signature now wildcards those bytes).
    - The badge shows vanilla's removal cross (`GFX_cancel_conversion_button`) instead of a red X
      (the X stays as the fallback).
    - `queue_railway_dismantle` on a path the country may not dismantle logs it, and its tooltip
      adds a red line (`HOI4_LOADER_QUEUE_RAILWAY_DISMANTLE_NOT_ALLOWED`).
    - NOTES: the railway findings are now Part H (Part G is `create_army`).

## 0.5.0-beta (2026-10-18), script version 3

First packaged release. It supports Hearts of Iron IV 1.19.3 (Steam), both as distributed and
after the universal checksum patcher.

**Script-visible changes:**
- **Script version 3.** `set_strategic_region_name` accepts only `region`. Its `id` alias was
  removed in Part C without a version bump; 3 records that.
- **Guards hidden.** The guard triggers `hoi4_loader_feature_<name>` and `hoi4_loader_present`
  are served as `hidden_trigger`. Their results are unchanged, and tooltips no longer show an
  "Always true" line for them.
- **Rename tooltip lines.**
  - Wording follows vanilla's province renames ("Bay of Biscay is renamed to …"), or its state
    renames ("Changes name to …") inside a strategic region scope.
  - They are translatable through `HOI4_LOADER_SET_/RESET_STRATEGIC_REGION_NAME[_IN_SCOPE]`.
- **Data trigger lines without a region** read normally (e.g. "Air superiority of FRA above
  50%" under an iterator's header) instead of showing the raw trigger name.

**For mod authors:**
- **Stub pack:** `stubs/` holds every stub file. MODDING.md "Stub pack" lists which one each command
  needs.
- **Warning snippet:** MODDING.md "Warning players when a required feature is off" shows an event
  at game start or on load when a needed feature is missing. The test mod uses it.
- **Policy:** MODDING.md "Versions and compatibility" covers the version policy, guards that work
  forever, and deprecation before removal.
- **Documented:** `random_strategic_region`'s tooltip names the region that is then picked
  (NOTES F9).

**Loader:**
- **Signature report.** On a hoi4.exe build that `loader_config.ini` does not list, the
  `loader.log` report says how many signatures, anchors and code shapes still match, and lists
  what to send when reporting. It only reads code and never hooks or patches.
  `[diagnostics] signature_scan = 1` writes it on listed builds too.
- **Version in the log:** the first `loader.log` line names the release.
- **Test mod fix:** its `hoi4_loader_init` stub clears every plugin flag (it missed five).

**Packaging:** the release zip, `PLAYERS.md`, `SHA256SUMS.txt` and this changelog. Published
under the MIT license.

**Known limits:**
- `on_game_loaded` and the loader's own flag refresh after loading are single player only. In
  multiplayer, script must use `on_weekly`.
- Multiplayer is untested (NOTES D3 step 10).

## Part F: strategic region scopes (2026-10-17/18), feature `region_scope`

- **Scope blocks:** `strategic_region = { region = 42 … }` as a trigger and as an effect. The
  contents run in that region.
- **Iterators:** `any_strategic_region`, `every_strategic_region` and
  `random_strategic_region`. They are the game's own `any_state`/`every_state`/`random_state`
  code with regions instead of states:
  - `count`, `tooltip`, `limit`, `random_select_amount` and `display_individual_scopes` work as
    there;
  - `prioritize` is a parse error.
- **Region triggers:** `strategic_region_id`, `is_sea_region`, `region_has_state`.
- **Parameters from scope:** the data triggers and the rename effects take their region from
  the scope.
- **Fix:** the trigger registry's map is read at its real offset. Reading it at the wrong one
  crashed the lexer callback on startup.

## Part E: numeric data triggers (2026-10-15/16)

- **Features and triggers:**
  - `region_control`: `air_superiority`, `naval_dominance` (ratios as in the region panel and
    `has_naval_control`);
  - `military_counts`: `battles_count`, `divisions_count`, `ships_count`, `planes_count`, with
    `owner`, `enemy_of`, `allied_with` and literal tags;
  - `army_condition`: `avg_division_strength`, `avg_division_organisation`;
  - `production_data`: `factories_assigned` (factories on production lines, by equipment type
    or archetype).
- **How they work:** they use the engine's own numeric trigger classes, so `>`, `<`, `=` and
  variables work. They can also be read as game variables.
- **API:** scope defaults for parameters, and country, choice and token parameter types.
- **Diagnostics:** `trigger_timing` (cost per in-game day) and `production_lines`.
- **Fixes:** data features start correctly at startup, commands bind under SEH one by one, and
  crash addresses are logged.

## Part D: `on_game_loaded` (2026-10-13/15), feature `game_loaded`

- **The on_action:** a real `on_game_loaded` on_action, fired after a save is loaded (not for a
  new game), once per human country. It is single player only. Without the loader it never
  fires and logs no error.
- **Fallback:** with the `on_weekly` fallback, the test mod demonstrates a save migration.
- **Threading:** a main-thread queue runs at the start of each frame. Region-name rescans are
  deferred to it.
- **Fixes:** the flag set date is read from the right offset, and rescans run only on
  `hoi4_srn_` flag changes.

## Part C: native script API (2026-10-13)

- **Native commands:** plugins register their own effects and triggers with the game's script
  parser and registry. The engine reports parse errors with file and line, and a docs file is
  generated (`loader_script_docs.md`).
- **Guard triggers:** `hoi4_loader_feature_<name>` per feature. They are never stale, and a mod
  ships an `always = no` stub for each.
- **Lazy virtual files:** generated on their first read.
- **region_names:** `set_`/`reset_strategic_region_name` became native commands, and the new
  trigger `strategic_region_has_custom_name` was added. The `id` alias was dropped (only
  `region`).

## Part B: strategic region names (2026-10-12/13), features `lifecycle`, `region_names`

- **Runtime names:** strategic regions can be renamed from global flags,
  `hoi4_srn_<region>_<loc key>`, which survive save and load. Without the loader every region
  shows its original name.
- **Display:** names show as localised text, and keys with `-` and up to 100 characters are
  tested.
- **Lifecycle hooks:** "lexer ready" and "game entered" (a new game and a loaded save).
- **Kill switch:** `[features]` in `loader_config.ini` turns off single features.

## Part A: the loader (2026-10-12)

- **Proxy:** `version.dll` forwards every export to the system DLL and adds features at runtime.
  Nothing on disk is modified.
- **Tiers:** the file tier (virtual files, script detection) runs on any build. The engine tier
  (code hooks) runs only on hoi4.exe builds whose SHA-256 is listed in `loader_config.ini`. The
  hash is cached.
- **Detection:** `hoi4_loader_present`, `hoi4_loader_init`, `global.hoi4_loader_version` and one
  `hoi4_loader_plugin_<name>` flag per active feature.
- **Supported builds:** vanilla 1.19.3 (Steam-verified). Signatures are also checked on the
  checksum-patched 1.19.3 and on 1.19.2.
