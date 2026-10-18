# Changelog

Release versions name the DLL package. The script version is the number script reads as
`global.hoi4_loader_version` (README "Versions and compatibility"). Development went in parts
A–F; NOTES.md has the findings and test results behind each.

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
- **Stub pack:** `stubs/` holds every stub file. README "Stub pack" lists which one each command
  needs.
- **Warning snippet:** README "Warning players when a required feature is off" shows an event
  at game start or on load when a needed feature is missing. The test mod uses it.
- **Policy:** README "Versions and compatibility" covers the version policy, guards that work
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

**Packaging:** the release zip, `PLAYERS.md`, `SHA256SUMS.txt` and this changelog.

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
