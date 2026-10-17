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
| `region_control` | engine (needs `lifecycle`) | `hoi4_loader_plugin_region_control` | `air_superiority`, `naval_dominance` (data triggers, see below) |
| `military_counts` | engine (needs `lifecycle`) | `hoi4_loader_plugin_military_counts` | `battles_count`, `divisions_count`, `ships_count`, `planes_count` |
| `army_condition` | engine (needs `lifecycle`) | `hoi4_loader_plugin_army_condition` | `avg_division_strength`, `avg_division_organisation` |
| `production_data` | engine (needs `lifecycle`) | `hoi4_loader_plugin_production_data` | `factories_assigned` |

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

The loader serves `hidden_trigger = { always = yes }` instead, but only if that feature is active
**and** every native command it declares was registered this session. Otherwise it serves
`hidden_trigger = { always = no }`, with the reason as a comment. `hoi4_loader_present` is served
the same way.
- **No tooltip line:** the served guards are hidden, so they add no "Always true" line to
  tooltips. A failed guard is hidden too, which leaves the player without a visible reason; to
  show one, wrap the guard in `custom_trigger_tooltip`:
  ```
  available = {
      custom_trigger_tooltip = {
          tooltip = my_mod_requires_hoi4_loader_tt
          hoi4_loader_feature_region_scope = yes
      }
      strategic_region = { region = 42 naval_dominance > 0.5 }
  }
  ```
  ```yaml
   my_mod_requires_hoi4_loader_tt:0 "Requires hoi4_loader (strategic region scopes)"
  ```
  The line shows ticked when the feature is there and crossed when it isn't, including when the
  loader is missing. Your stub's `always = no` stays inside the `custom_trigger_tooltip`, so
  its "Always false" line doesn't show either.
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
- **Tooltips:** worded like vanilla's province renames, e.g. "**Bay of Biscay** is renamed to
  **Loader Test Sea A**." / "**Bay of Biscay** gets its original name back." The loader shows
  English unless a loaded mod defines the keys, so no loc file is needed. To translate them, add
  to your mod's localisation for each language:
  ```yaml
   HOI4_LOADER_SET_STRATEGIC_REGION_NAME:0 "$OLDNAME|Y$ wird zu $NAME|Y$ umbenannt."
   HOI4_LOADER_RESET_STRATEGIC_REGION_NAME:0 "$NAME|Y$ erhält seinen ursprünglichen Namen zurück."
  ```
  `$OLDNAME$` is the name shown now, `$NAME$` the new one; the game formats them (`|Y`). The keys
  are also listed in `loader_script_docs.md`.
- Avoid `{` in localisation text: HOI4 treats it as markup.

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

**Parameters from scope.** Some parameters may be left out; the docs mark them "optional: from
<scope>" (e.g. `region` inside a strategic region scope, the country from ROOT).
- If the scope doesn't provide the value, an effect does nothing and a trigger is **false**.
  Under `NOT` it is therefore **true**, the same as a dropped trigger.
- `loader.log` notes it once per command ("... not given and not available from the scope").
- So write the parameter explicitly wherever the scope isn't certain, or wrap the trigger together
  with a condition that guarantees the scope.

## Data triggers (numeric)

Four features add numeric triggers. They read values the game already computes and compare them
like vanilla numeric triggers. Each feature has its own guard trigger, so one broken signature
after a game update only switches off its own group.

| Feature | Trigger | Value |
|---|---|---|
| `region_control` | `air_superiority` | ratio: the region panel's air superiority bar, own side / (own + enemy) air power; 0 if neither side flies there |
| `region_control` | `naval_dominance` | ratio: naval dominance of the country and its friends / (friends + countries at war with it), the sums `has_naval_control` uses; 0 in land regions |
| `military_counts` | `battles_count` | battles in a state / strategic region: located there or fought by divisions standing there, each once (`location = only`: located there only); or the battles a country's divisions fight in (`involving`) |
| `military_counts` | `divisions_count`, `ships_count` | divisions / ships in a state or strategic region; filters `owner = <country>/any`, `enemy_of = <country>`, `allied_with = <country>` |
| `military_counts` | `planes_count` | a country's planes based (by air base) in a strategic region; `owner = <country>` only (`enemy_of`, `allied_with` reserved) |
| `army_condition` | `avg_division_strength`, `avg_division_organisation` | ratio: average over the country's whole army (country scope) or the divisions in a state |
| `production_data` | `factories_assigned` | factories on the country's production lines (dockyards on naval lines), optionally for one equipment type or archetype |

**Syntax:**

```
air_superiority > 0.6                                        # inside a strategic region scope (country = ROOT)
air_superiority = { region = 19 country = GER value > 0.6 }  # block form; `value` holds the comparison
naval_dominance = { region = 42 value < global.my_threshold } # variables work on the right side
battles_count = { involving = ROOT count > 0 }
divisions_count = { state = 16 enemy_of = ROOT count > 5 }
16 = { divisions_count = { owner = ROOT count > 0 } }        # state from the scope
battles_count = { region = 7 location = only count > 0 }    # only battles located there
avg_division_strength < 0.7                                  # country scope: the whole army
factories_assigned = { equipment = infantry_equipment count > 10 }
```

**Comparisons, exactly as vanilla:**
- **Ratios** (`air_superiority`, `naval_dominance`, the averages): values 0..1, shown in
  percent in tooltips. Use `<` (below) or `>` (above). **`=` also means above**, which is
  vanilla's behaviour for fractional triggers such as `has_stability = 0.5`. The right side must
  be within 0..1.
- **Counts** (`*_count`, `factories_assigned`): `>`, `<`, and `=` meaning **equal**.
- **The right side** may be a number or a variable (`global.x`, a country variable, ...).

**Defaults:**
- Without `region` / `state` the triggers use the current strategic region or state scope.
- Without `country` they use ROOT, else the country scope (the averages and `factories_assigned`
  use the country scope). `planes_count` takes its country from `owner` the same way.

**Unit filters** (`divisions_count`, `ships_count`): all optional, combined with AND; a country is
a tag, THIS, ROOT, FROM or PREV.
- `owner = <country>`: units of that country; `owner = any` (or no `owner`): every unit.
- `enemy_of = <country>`: units of countries at war with it.
- `allied_with = <country>`: units of its allies, as naval control counts allies (faction
  members, subjects and overlords); the country itself is not included.

**Battles in an area** count each battle once, if it is located in the area's provinces or a
division standing in the area fights in it (an attack from Western Germany into Northern France
counts for both regions). `location = only` counts only the battles located there.
- If a value cannot be determined (unknown region, missing scope, an unknown equipment name, no
  divisions to average), the trigger is **false**, and true under `NOT` (see above).

**Tooltips:** e.g. "Air superiority of FRA over Northern France (19) above 60% (currently
64%)".

**Game variables:** triggers whose parameters can all come from a country or state scope are also
game variables of the same name: `avg_division_strength`, `avg_division_organisation`,
`factories_assigned`. Example: `set_variable = { my_strength = avg_division_strength }`.
Strategic region values can't be variables, because HOI4 has no region-scope variables.

**Resource deficits are not here: vanilla already has them.**
- `has_resources_in_country = { resource = steel amount < 0 }` compares the balance.
- The variable `resource@steel` is the surplus (negative = deficit).
- `resource_produced@`, `resource_imported@`, `resource_exported@` and `resource_consumed@` give
  the parts: needed = consumed + exported, available = produced + imported.

**Cost:**
- Counts and ratios over an area read the area's provinces and units.
- `battles_count = { involving = ... }` and the country-wide averages loop over every division of
  the country.
- Measured (vanilla 1.19.3, 1936, speed 5). The test mod's AI probe made every AI country
  evaluate six of the triggers in `ai_will_do`, and that cost **about 1.8 ms per in-game day in
  total**. Speed 5 runs about 620 ms per day, so that is roughly 0.3%. Time per evaluation:

  | Trigger | per evaluation |
  |---|---|
  | `naval_dominance`, `air_superiority` | 0.2-0.3 µs |
  | `battles_count` | 0.4 µs |
  | `factories_assigned` | 0.7 µs |
  | `divisions_count` (strategic region) | 0.8 µs |
  | `avg_division_organisation` (country) | 0.8 µs |
  | `avg_division_strength` (country) | 1.5 µs |
  | `planes_count` | 1.6 µs |

  The country-wide averages and `involving` grow with the number of divisions (France: 74). Even a
  few hundred divisions stay in the µs range, so no caching is needed. Hovering a tooltip
  re-evaluates it every frame; that costs nothing noticeable either.
- With `[diagnostics] trigger_timing = 1` in `loader_config.ini`, `loader.log` reports
  evaluations, time per evaluation and time per in-game day every 10 days, so you can measure your
  own mod. Details are in NOTES.md (Part E, in-game results).

**Multiplayer:** all values are game state that every machine computes the same way (integer
fixed point, no floats), so the triggers are as deterministic as vanilla's.

## Strategic region scopes (`region_scope`)

Vanilla script has no way to enter a strategic region scope; only the game's C++ code creates
one (NOTES.md Part F). The `region_scope` feature adds it, plus three triggers about the region
in scope:

| Command | Kind | What it does |
|---|---|---|
| `strategic_region = { region = 42 ... }` | trigger | true if all triggers inside are true in region 42 |
| `strategic_region = { region = 42 ... }` | effect | runs the effects inside in region 42 |
| `any_strategic_region = { ... }` | trigger | as `any_state`: true if a region (or `count` regions) meets all triggers inside |
| `every_strategic_region = { limit = { ... } ... }` | effect | as `every_state`: runs the effects in every region that meets `limit` |
| `random_strategic_region = { limit = { ... } ... }` | effect | as `random_state`: runs the effects in one random region that meets `limit` |
| `strategic_region_id = 42` | trigger, region scope | the region in scope is region 42 |
| `is_sea_region = yes` | trigger, region scope | the region has sea provinces (`no`: a land region) |
| `region_has_state = 16` | trigger, region scope | state 16 has a province in the region |

```
# Inside, the data triggers and set_strategic_region_name take the region from the scope:
strategic_region = { region = 19 air_superiority > 0.5 divisions_count = { enemy_of = ROOT count < 10 } }
16 = { strategic_region = { naval_dominance > 0.5 } }       # no region: the state's region
any_strategic_region = { count = 3 is_sea_region = yes naval_dominance > 0.6 }
every_strategic_region = {
    limit = { is_sea_region = yes air_superiority > 0.5 }
    display_individual_scopes = yes
    add_to_variable = { global.my_air_regions = 1 }
}
random_strategic_region = {
    limit = { OR = { strategic_region_id = 16 strategic_region_id = 18 } }
    set_strategic_region_name = { name = MY_SEA_NAME }
}
```

- **Which region:** `region = <id>` (a number). Without it: the strategic region scope, else the
  state scope's region (the region of the state's first province). With no region the trigger is
  false (true under `NOT`) and the effect does nothing.
- **Inside the blocks:** only commands that work in any scope or in the strategic region scope.
  That includes `global.` variables, flags, `if`, `NOT`, `OR`, the loader's data triggers and
  `set_strategic_region_name`. The engine reports others with file and line, as it does for a
  state trigger inside `GER = { }`. HOI4 has no region-scope variables, so use `global.` ones.
- **Iterators:** the game's own `any_state` / `every_state` / `random_state` code, with regions
  instead of states.
  - `count`, `tooltip`, `limit`, `random_select_amount` and `display_individual_scopes` behave
    as there.
  - Regions are checked in id order (1, 2, 3, ...).
  - `random_strategic_region` picks with the game's synced random numbers, so it is
    multiplayer-safe.
  - `prioritize` (a list of state ids) is not supported: it is a parse error, and the rest of
    the block still works.
- **Tooltips:**
  - `strategic_region` shows "Bay of Biscay:" with the lines inside indented below it, each
    marked in that region, like `GER = { }`.
  - `any_strategic_region` shows "Any strategic region" ("At least 3 of ..." with `count`) and
    the lines inside, as `any_state` does. A mod can translate the header with the localisation
    key `HOI4_LOADER_ANY_STRATEGIC_REGION_STARTS`. Without that key the loader shows the English
    text, so no loc file is needed. `tooltip = key` replaces the header as in vanilla.
  - `every_` / `random_strategic_region` show the region names (for random: the one that will be
    picked) and the effects, as vanilla does for states.
- **Without the loader** the blocks are unknown commands, which the game logs and drops. A
  dropped trigger block counts as **true**, so guard it with
  `hoi4_loader_feature_region_scope = yes` in the same AND block (see "Checking for features in
  script").
- **Cost:** a block costs what its contents cost, plus building one scope. An iterator checks its
  contents in each of the 304 regions, so `any_strategic_region` with a data trigger inside costs
  up to 304 evaluations of that trigger. `trigger_timing` reports both.
  - **Measured** (1939, France, vanilla 1.19.3):
    - `strategic_region` with a few triggers inside: about 1.7 µs per evaluation.
    - `any_strategic_region = { is_sea_region = yes air_superiority > 0.5 }`: 27 µs per
      evaluation in peacetime and 49 µs at war (about 90–160 ns per region). With every country
      evaluating it as an AI weight, that added 4.5–11 ms per in-game day.
    - Use it in AI weights and frequently checked triggers only when needed; prefer
      `strategic_region = { region = <id> ... }` when the region is known.

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
    `"any|own|enemy"`; `Args::i` is the index) and `Token` (a key interned at parse time;
    `Args::i` is the engine token, for lookups without allocation).
  - `ParamSpec::fromScope` (`StrategicRegion`, `State`, `Country`, `RootCountry`): the parameter
    may be left out and then comes from the scope. Such a parameter is not `required`. Missing
    in the scope: effects do nothing, triggers are false (see "Parameters from scope").
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
  - **`subject`:** optional, the start of the tooltip line.
- **Scope blocks and iterators:** `register_native_scope_block` (a block whose other keys run in
  the scope a `target` callback picks, e.g. `strategic_region`) and `register_native_iterator`
  (any / every / random over every object of a scope type, cloned from the game's state
  iterators). Only strategic region scopes are supported; see `src/plugin_region_scope.cpp`.
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
match. Also run `python tools/check_code_shapes.py <copy of hoi4.exe>`: every code shape the
numeric triggers rely on must report ok.

Optional diagnostics:

```
[diagnostics]
trigger_timing = 1   ; time numeric triggers, scope blocks and iterators; loader.log summary every 10 in-game days
```

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
