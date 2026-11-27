# Changelog

Release versions name the DLL package. The script version is the number script reads as
`global.hoi4_loader_version` (MODDING.md "Versions and compatibility"). Development went in parts
A–F; NOTES.md has the findings and test results behind each.

## Unreleased

- **New feature `ship_domains`** (engine tier, needs `lifecycle`; guard
  `hoi4_loader_feature_ship_domains`; MODDING.md "Ship domains", NOTES.md U14). Navies that never mix:
  a mod names domains (ship archetypes, the continents of their bases); built ships (production,
  delivered equipment, `create_ship`) go only to bases, task forces and fleets of their domain (else
  into the stockpile), and moving ships or task forces, a fleet's home base and a naval line's
  deployment target are refused when they would mix two domains (also the fleet window's
  reorganisation); a naval line's "Deploy target" list shows only its domain's fleets and bases. 4 new signatures and 2 code
  shapes, checked on 1.20.1, 1.20, 1.19.5 and 1.19.4.2; tested in game on 1.20.1 (the planets test's
  spaceships).
- **Game 1.20.1** (`v1.20.1.0.0fd1`, Steam update of 2026-11-26) is listed in `loader_config.ini`.
  A data-only hotfix: the code section is identical to 1.20, so every signature and anchor
  (185/185) and every code shape (97/97) matches at the same address as on 1.20 (checked offline on
  a copy). The vanilla files the loader patches are unchanged. No code changes. Tested in game on
  1.20.1 (2026-11-26).
- **Railway UI opt-in** (`railway_queue`; NOTES.md H8). The loader now leaves the railway UI
  vanilla until a mod sets the global flag `hoi4_loader_railway_dismantling`: without it there is
  no Alt+Shift dismantling on the railway map, no green marker on railway build entries, no badge
  splitting by level, no partial-line badges and no railway hover outline. Script commands and
  dismantle entries a mod queues work as before. **Players:** update HoI4Mod Loader: Railway
  Dismantle (it sets the flag); with an older copy of it, Alt+Shift dismantling is off. The test
  mod sets the flag at `on_startup` and has decisions to clear and set it.
- **New feature `camera_range`** (engine tier, needs `lifecycle`; guard
  `hoi4_loader_feature_camera_range`; MODDING.md "Camera range"). A mod's raised
  `NFrontend.CAMERA_MAX_HEIGHT` works: the map camera's far plane (fixed 4000) follows it. 2 new
  signatures, checked on 1.20, 1.19.5 and 1.19.4.2; tested in game on 1.20 with 6000.
- **New feature `large_map`** (engine tier; guard `hoi4_loader_feature_large_map`; NOTES.md Part T,
  MODDING.md "Large maps"). Maps larger than vanilla's: the engine's fixed 50 MB map scratch buffer,
  its 16 MB texture file limit and its 256 terrain blocks are sized to the map the game will load
  (read from its provinces bitmap before the game starts). Vanilla-sized maps are left alone. 4 new
  signatures, checked on 1.20, 1.19.5 and 1.19.4.2; tested in game on 1.20 with a 5632 x 4352 map.
- **`tools/make_planets_map.py`**: builds a test mod with several copies of the vanilla map stacked
  north to south, lake gaps between them, one new country owning each copy, and the globe's planets
  for them (NOTES.md Part T). Reads the owner's own game install; the generated files are game data
  and stay out of the repository.
- **`globe`: the space map** (NOTES.md Part U, MODDING.md "Space"). `space = { x y width height }` and
  per planet `orbit = { x y radius }`: zoomed out past `space_height` over a planet's globe, the camera
  goes over the planet's orbit in the space map (a flat map of space zones, the game's own naval map,
  fleets and missions), the planets drawn as their globes in their holes (centred on the map's level,
  covering whatever the hole holds); zooming in over a disc goes back to that planet, looking where it
  last looked. Over space the game draws only the planets' terrain for the discs (2 new signatures on
  the terrain's block and patch choices, checked on 1.20.1, 1.20, 1.19.5 and 1.19.4.2), so zooming in
  over space keeps its frame rate. `tools/make_planets_map.py --space-height` builds a space band with space zones,
  hidden spaceport docks per planet, a spaceport building, spaceship hulls and units, a
  spaceships production tab, the spaceships' ship domain and test decisions that build a spaceship
  and a destroyer. Fixed: the map's DDS textures (colour map, water) put the space band's rows at
  the south, so Earth A's were the texture's north edge repeated (dark stripes along the meridians).
- **`globe`: several planets and the overview** (NOTES.md Part T, MODDING.md "Planets"). `planet = {
  name x y width height north south polar_cap_* overview = { x y size } image }` blocks (up to 16) make
  rectangles of the map separate worlds: the flat camera stays on its planet, the globe is that
  planet's, and from `overview_height` all planets are shown at their places with their names curved
  across them in the game's map font (or a mod's image); zooming in over one goes to it as it was
  shown, and the camera moves freely once the other planets are out of sight.
  Without planet blocks the whole map is one planet, as before.
- **New feature `globe`** (engine tier, needs `lifecycle`; guard `hoi4_loader_feature_globe`; NOTES.md
  Part S, MODDING.md "Globe view"). The map as a planet: zoomed out far enough that the flat view
  would reach past the map's north or south edge (or above `globe_height`), the flat map folds into a
  sphere in front of the mod's background image; the point the camera looks at stays on top, so
  panning turns the planet (all the way round east-west, to the poles north-south); zooming in gives
  the flat map back. Turned on by a mod's `common/hoi4_loader_globe/*.txt` (`default`, `flat_height`,
  `globe_height`, `transition`, `north`, `south`, `background`, `polar_cap_north`, `polar_cap_south`, `polar_cap`, `polar_cap_color`, `polar_sea_color`);
  without one the map shaders are left alone. Clicks in space find nothing; names do not show through
  the planet; the sea keeps the flat map's colour; the map can end short of the poles, with polar seas and ice beyond it; 2D map
  icons hide while it is a planet; dragging and edge panning move the camera as on the flat map. `set_globe_view =
  yes/no` (country scope, saved as global flags `hoi4_globe_<TAG>_<0|1>`) and `is_globe_view`. Every
  3D map shader's vertex positions are bent (the loader patches the shader files as read, also mods'
  copies); the values reach them through the loader's own constant buffer on the game's DX11 device.
  While bent, the game's culling is opened, the camera's north/south clamp skipped and its screen
  rays aimed at the flat map point under the cursor, so hover, clicks and zoom to the cursor work on
  the planet. Display only. DX11 only. 11 new signatures and 1 code shape (smoke `2/196`), checked on
  1.20, 1.19.5 and 1.19.4.2; tested in game on 1.20 (2026-11-24). The test mod starts every game as a
  globe and has decisions to switch.
- **File patches can be shared:** several features may patch the same file (`register_file_patch`);
  each runs on the previous one's result (the globe and attention areas both patch `border.shader`).

## 0.7.1-beta (2026-11-23), script version 3

- **Game 1.20** (`v1.20.0.0.8873`, Steam update of 2026-11-19) is listed in `loader_config.ini`.
  Unlike 1.19.5 this is a real code update (183 of the 185 signatures sit at new addresses), but
  every signature and anchor (185/185) and every code shape (97/97) still matches exactly once
  (checked offline on a copy); no code changes were needed. The vanilla files the loader patches
  (`standardfuncsgfx.fxh`, `border.shader`, the three designer views, `countrystateview.gui`) are
  unchanged. Tested in game on 1.20 (2026-11-21): every feature active, every game-start
  self-check passed, the engine on_actions fired over ten game months. Same code as 0.7.0-beta.

## 0.7.0-beta (2026-11-18), script version 3

- **New feature `equipment_designers`** (engine tier; guard `hoi4_loader_feature_equipment_designers`;
  no script commands; NOTES.md Part N, MODDING.md "Equipment designers"). Modular equipment the game
  gives no designer (e.g. `type = infantry`) can open the tank designer window, listed per archetype
  in `common/hoi4_loader_equipment_designers/*.txt` with its own title, without the tank role
  dropdown and with its own slot positions. The archetype keeps its real type, so the production
  tab (infantry/artillery), the experience (army) and the save path stay the game's own. The tank
  window gets up to 20 normal and 20 `fixed_` slot windows (added to `tank_designer_view.gui` at
  load); other designs keep vanilla's look. Optional per designer: `stats` (the rows of the three
  stat panels, by `add_stats` names), `xp = army | navy | air` (the pool a design is checked against
  and paid from, shown and named in the cost texts; a game rule, so every multiplayer player needs
  the loader) and `hide` (elements of the tank window). Needs No Step Back and the archetype's
  `interface/equipmentdesigner` layout, as vanilla does. Land window only for now; the air and naval
  windows are planned. Hooks the "can open designer" check, the window's open, refresh, stats and
  buttons update, and, only when `xp` is used, the experience pool, the two deduction tests of the
  variant commands, the cost tooltip and icon; 13 new signatures and 2 anchors (smoke `2/144`).
  Checked on 1.19.4.2 and 1.19.5; tested in game on 1.19.5 (2026-11-14). The test mod has a small
  arms designer (decision "Unlock the test small arms").
- **`equipment_designers`: the plane and ship windows** (NOTES.md N14/N15). `window = air` opens the
  plane window, `window = naval` the ship window, for archetypes whose type the game sends there but
  never makes designable (e.g. `air_transport` and the missile types; `convoy`, `support_ship`,
  `floating_harbor`), with the window's DLC (By Blood Alone, Man the Guns). Title, slots (the plane
  and ship GUI files get up to 20 + 20 slot windows too), `stats`, `xp` and `hide` work in every
  window; `hide` also finds container windows (e.g. the plane window's role line `plane_role`).
  Stats now go through the game's Fill function for all three windows and the comparison panels (the
  tank-only stats hook is gone). Vanilla-designable archetypes can be listed too, to change their
  window. Backgrounds and module pictures are vanilla (the layout file); documented, and shown in the
  test mod. 2 new signatures and 1 anchor (smoke `2/147`). Checked on 1.19.4.2 and 1.19.5; the plane
  window tested in game on 1.19.5 (2026-11-15); the ship window offline only (no Man the Guns here).
  The test mod has a test drone (`type = air_transport`) and pictures for the small arms.
- **`equipment_designers`: custom roles** (NOTES.md N16/N17). `roles = { <equipment> = { name = <loc>
  stats = { ... } } ... }` lists the tank window's role dropdown for a designer: the archetype and its
  `duplicate_archetypes` copies (each with its own types), with the mod's names and optional
  per-role stats, instead of the game's seven fixed tank roles and names. E.g. one artillery
  designer with howitzer, anti-tank and anti-air roles. Which role a design may take, the save and
  the layouts per role stay vanilla. Hooks the dropdown's entry and label functions; 1 new signature
  (smoke `2/148`). Checked on 1.19.4.2 and 1.19.5; tested in game on 1.19.5 (2026-11-15). The test
  mod has a field gun with three roles and battalions for them.
- **New feature `production_tabs`** (engine tier; guard `hoi4_loader_feature_production_tabs`; no
  script commands; NOTES.md Part O, MODDING.md "Production tabs"). Which archetypes the production
  screen's "add production line" tabs list: `add` and `remove` lists per vanilla tab (`infantry`,
  `armor`, `air`, `naval`), and new tabs (up to 16) with the mod's own button and glow icon in
  `countryproductionlineview.gui`, defined in `common/hoi4_loader_production_tabs/*.txt`. Move, copy
  or hide equipment is the mod's choice; the same tab in several mods merges. `add_types` /
  `remove_types` select by equipment type (`type = { ... }` names, through the game's own name
  lookup), the more specific archetype entries win. `visible = <scripted trigger>` (any tab, vanilla
  ones too) shows the button only while the trigger is true for the player's country (evaluated in
  the UI, outside the synced RNG); `enabled = <scripted trigger>` keeps the button but greys it out
  while the trigger is false. The game's own checks
  (research, `can_be_produced`, outdated) and the add-line command stay; UI only. Hooks the view's
  setup (binds the new buttons), the buttons' state and the two add-line lists; 7 new signatures
  (smoke `2/155`). Checked on 1.19.4.2 and 1.19.5; tested in game on 1.19.5 (2026-11-15). The test
  mod moves the towed guns into a "Towed guns" tab and copies the test small arms into the armor tab.
- **New feature `province_terrain`** (engine tier; guard `hoi4_loader_feature_province_terrain`;
  NOTES.md Part P, MODDING.md "Province terrain"). `set_province_terrain = { province = N terrain =
  marsh }` changes a province's terrain category at runtime; in a state scope (or `state = N`) with
  `if_terrain = plains` (repeatable) every matching province of the state. `reset_province_terrain`
  goes back to `map/definition.csv`'s. Triggers `province_has_terrain` and `state_has_terrain`.
  Land categories only (vanilla's and mods'). Movement, combat (running battles switch at once),
  attrition, `has_terrain` and the province tooltip follow; supply is notified; the map's look
  stays. Stored as global flags `hoi4_ptr_<province>_<category>`, applied when a save is loaded;
  without the loader saves stay vanilla. Changes the simulation: in multiplayer every player needs
  the loader. 5 new signatures (smoke `2/160`). Checked on 1.19.4.2 and 1.19.5; tested in game on
  1.19.5 (2026-11-15). The test mod changes Brandenburg, a single province and Ethiopia.
- **New feature `engine_on_actions`** (engine tier; guard `hoi4_loader_feature_engine_on_actions`;
  NOTES.md Part Q, MODDING.md "Engine on_actions"). On_actions vanilla never fires:
  `on_construction_finished` (ROOT country, FROM state), `on_technology_researched`,
  `on_battle_started`, `on_battle_won` / `on_battle_lost` (land battles; FROM = the other side),
  `on_division_trained`, `on_division_destroyed` (in battle). They behave like vanilla's (events,
  random events, effect; at the moment, or at the end of the update pass they happened in; only
  while a game runs). Data commands `constructed_building`, `researched_technology`,
  `constructed_province`, `battle_province`. A name a later game version fires itself is left to
  the game. 12 new signatures (smoke `2/172`). Checked on 1.19.4.2 and 1.19.5; tested in game on
  1.19.5 (2026-11-16), `on_division_destroyed` with an encircled division.
  Second round: `on_naval_battle_won` / `on_naval_battle_lost` (FROM = the other side; admirals'
  or fleets' countries), `on_ship_sunk` (FROM = the country that sank it), `on_decision_taken`
  (data trigger `taken_decision`), `on_province_captured` (ROOT new controller, FROM old one; data
  value `captured_province`), `on_focus_completed` (data trigger `completed_focus`). 8 more
  signatures (smoke `2/180`). Checked on 1.19.4.2 and 1.19.5; tested in game on 1.19.5
  (2026-11-17): decisions (player and AI), Italy vs Ethiopia captures, 11 France-Italy naval
  battles and 80 ships sunk, focuses from `complete_national_focus` and the AI's daily progress.
- **New feature `map_adjacency`** (engine tier; guard `hoi4_loader_feature_map_adjacency`; NOTES.md
  Part R, MODDING.md "Map connections"). `set_province_adjacency = { province = A neighbour = B
  passable = no }` closes the border between two neighbouring provinces (both directions) as
  `map/adjacencies.csv`'s impassable does, `passable = yes` opens it (also a csv impassable one);
  `reset_province_adjacency` goes back to the map's; trigger `province_adjacency_passable`.
  `set_state_impassable = { state = N impassable = yes }` (or in a state scope) closes or opens a
  whole state as `history/states`' impassable; `reset_state_impassable`; vanilla's `impassable`
  trigger follows. The game reads both live: move orders across a closed border are refused ("no
  available land route"), troops may not enter a closed state (those inside may leave, as in
  vanilla), supply follows. The map's red impassable lines are redrawn at once (closed borders,
  states made impassable; opened ones lose them), and a state's resistance is checked again through
  vanilla's `should_initiate_resistance` after each change (an occupied state that opens gets
  resistance, one that closes loses it). Stored as global flags `hoi4_adj_<low>_<high>_<0|1>` and
  `hoi4_imp_<state>_<0|1>`, applied when a save is loaded; without the loader saves stay vanilla.
  Changes the simulation: in multiplayer every player needs the loader. 5 new signatures (smoke
  `2/185`). Checked on 1.19.4.2 and 1.19.5; tested in game on 1.19.5 (2026-11-17): Berlin's borders
  closed, Brandenburg and North Africa switched, the Alps border opened, save and load, supply (by
  the owner), the red lines appearing and going away live and after loading.
- **Native script API:** a Token parameter may be repeated (`repeat = true`, as Int and Block);
  `Args::at` gives the tokens in script order.
- **Shared code:** `Engine::ProvinceById`, `MapProvinceById`, `StateObjectOwner`,
  `StateObjectProvinces`, `ProvinceCombatObjects` and `SupplyProvinceChanged` (the supply system's
  "province changed" notification railway changes use) for any feature.
- **Docs:** MODDING.md's feature table listed `equipment_designers` twice (the tank-only row is gone).
- **Shared code:** `Engine::BindButtonClick` (a click handler on a GUI button through the engine's
  `CButtonWrapper`, from province_building_removal) for any feature.
- **Shared code:** the Paradox script reader (`src/script_tree.cpp`) and the mod folder scan
  (`ModList::ScanFolder`) moved out of air_scenarios, which now uses them; `UserFolder()` and
  `GameFolder()` in `loader.h`.
- **Railway badges after a merge** (`railway_queue`; NOTES.md H6). When a removed railway takes a
  junction with it, the two segments next to it merge into one badge. An entry queued on one of
  them used to vanish from the badge, and a click queued a second, overlapping entry. Now the entry
  keeps working on its own stretch, the badge shows its target, a click changes it, and
  Alt+Shift+click deepens a dismantle entry on part of the segment. Badges are also split where
  the railway level changes, as vanilla would show them: a merged segment used to show only its
  lowest link's level, so a finished upgrade looked lost. The engine's segment builder gets a copy
  of the supply array with those provinces marked as nodes; supply itself is unchanged. Hooks
  `FindRailwayLine` (only the badge click calls it) and the segment builder; three new signatures,
  and `kSigRailLineList` now starts at `FindRailwayLine`. Signatures checked on 1.19.4.2 and 1.19.5;
  tested in game on 1.19.5 (2026-11-14).
- **Railway badges while dismantling** (`railway_queue`; NOTES.md H7). A segment with a dismantle
  entry now looks like one being upgraded: the target level in red with the under-construction
  mark, and a "Currently dismantling ... to level N" tooltip line with its own click hints
  (localisation keys `HOI4_LOADER_RAILWAY_DISMANTLING` / `_DISMANTLE_HINTS`, else built-in English).
  A click or right click there dismantles one level less, Shift+click stops it. **Fix:** vanilla's
  click or right click on such a segment used to turn the dismantle entry into a rebuild of the
  links already taken down. A railway being removed entirely (dismantled to 0) keeps its badge,
  showing its current level in red, until it is gone; vanilla made no badge for it. Hooks the badge
  tooltip and right click; four new signatures. Tested in game on 1.19.5 (2026-11-14).
- **Fix:** an Alt+Shift+click on a badge right after its dismantle entry was cancelled (Shift+click,
  before the next in-game day) deepened the cancelled entry, so the click did nothing. Cancelled
  entries are now skipped, and the click queues a new one.
- **New feature `attention_area` (experimental)** (engine tier; guard
  `hoi4_loader_feature_attention_area`; script version unchanged). NOTES.md Part M. Mark any set of
  provinces on the map, every look a shader. The script syntax, the shader interface and the
  shaders' look may still change; only parts are tested in game (below).
  - `add_attention_area = { id line fill color red green blue alpha area = { ... } }`: `line` is the
    shader of the area's outline (the game's thin province group outline, drawn by `border.shader`),
    `fill` the shader of its provinces (through the map's stripe texture, drawn by
    `secondary_color_mask`); both show in every map mode. Built-ins: lines `solid`, `pulse`,
    `dashed`; fills `stripes`, `solid`, `band`, `glow`. Each `area` block selects provinces by
    `province`, `state` (or `state = this`), `strategic_region` ids, then `owner`, `controller`,
    `coastal`, and `random_provinces = n`.
  - Mods add or replace shaders as HLSL files in `gfx/FX/hoi4_loader_patterns/{line,fill}/`; the
    loader finds them in the enabled mods (`dlc_load.json`) and stitches them, with a dispatcher, into
    `border.shader` and `standardfuncsgfx.fxh` as the game reads them, so no game shader file is
    replaced. The shader id rides in the outline colour's alpha (lines) and the stripe texture's alpha
    byte (fills); 32 extra colour slots are appended to the renderer's outline colour table (no
    vanilla colour is changed).
  - **Only for mods that use it:** the two shader files are patched only when an enabled mod ships
    shaders or the marker `gfx/FX/hoi4_loader_patterns/patch_map_shaders.txt`; every other player keeps the game's shaders and gets no
    recompile. **Fail-safe:** every mod shader is compile-checked first (the system's DirectX
    compiler, against the documented interface); one that fails is logged and left out (its areas
    fall back to `solid` / `stripes`), so a broken mod shader can't break the map. While the fill
    shader is patched, a game stripe alpha in the fill ids' range is moved out of it (logged).
  - `shader_variables = { name = value ... }`: a shader declares up to 4 named values
    (`// hoi4_loader_variables: name min max default, ...`), script sets them per area. The colour's
    alpha reaches line and fill shaders.
  - `remove_attention_area = <id>|all`, `focus_attention_area = { id zoom }` (centre and zoom the
    player's camera), trigger `has_attention_area = <id>` (safe from any thread).
  - Display only, not saved: mods add areas again after loading. A hot-joining player has none, so
    `has_attention_area` must not drive game logic in multiplayer (MODDING.md).
  - Hooks the stripe texture's paint and the outline colour table's build (DEVELOPING.md "Global
    hooks"). New test `shader_patterns_test` (also splices shaders into the game's cached shader
    sources for fxc). 129 signatures and anchors in all, checked on 1.19.4.2 and 1.19.5.
  - Tested in game on 1.19.5 along the way: outlines, stripes, mod shaders (matrix rain), a line
    shader. Not yet tested in game: `shader_variables`, alpha in shaders, the latest fill edge
    fixes. Test mod: example decisions and shaders (matrix rain, inferno, fire line, DNA helix). An
    earlier gradient-border `band` style (eleven signatures, three hooks) is gone: a band is now a
    fill shader (NOTES.md M12).
- **Railway queue hover mark** (needs `railway_queue` and `attention_area`; experimental with it): hovering a railway
  entry in the construction queue outlines its provinces with the game's thin province outline in white
  (a colour slot of the loader's own; no vanilla colour changes); a right-click on the entry
  shows the whole line. UI only. NOTES.md M7.
- **Internal:** the launcher's mod list and descriptor reader (`src/mod_list.cpp`) is shared by
  air_scenarios and attention_area's shaders. `HOI4_LOADER_USER_DIR` is honoured only by the smoke
  test's stand-in executables.
- **Native script API:** parameters of type `Number` (decimals) and `Block` (a nested
  `key = { ... }` read by the game's own clause reader), `ParamSpec::repeat` (a key written several
  times keeps every value), and up to 16 parameters per command (was 8).

## 0.6.0-beta (2026-11-13), script version 3

- **New feature `air_scenarios`** (engine tier; guard `hoi4_loader_feature_air_scenarios`; no
  script commands; script version unchanged). NOTES.md Part L.
  - New air scenarios (the 3D air animations on the map) from mods: a vanilla-style mesh and
    entity, plus an entry in `common/hoi4_loader_air_scenarios/*.txt` with `entity`, `slot` (the
    game's scenario: mission and plane count), optional `attacker` / `defender` blocks
    (`equipment` types or archetypes, `country` tags) and `weight`.
  - A matching entry plays instead of the game's scenario; one matching plane per side is enough.
    Files come from the game folder and the launcher's enabled mods in load order.
  - The loader builds the templates with the game's own loader and swaps one in when a scenario
    starts. Graphics only, so multiplayer is unaffected. Two signatures (111 in all) and seven
    code shapes, checked on 1.19.4.2 and 1.19.5.
  - Test mod: four entries built from vanilla's meshes (a CAS orbit, two dogfights, an
    interception).
- **Game 1.19.5** (`v1.19.5.0.7c6c`, Steam update of 2026-11-12) is supported: its hash is in
  `loader_config.ini`. Every signature, anchor and code shape of this release matches (checked
  offline on a copy, as on 1.19.4.2); no code changes were needed.
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
