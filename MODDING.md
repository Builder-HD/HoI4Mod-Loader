# hoi4_loader for mod authors

Everything script-facing: how to detect the loader and its features, the stub pack, and every
command it adds. Players: [README.md](README.md). Building or extending the loader:
[DEVELOPING.md](DEVELOPING.md).

## Detection

Game script can detect it:

- scripted trigger `hoi4_loader_present` evaluates to `yes`
- scripted effect `hoi4_loader_init` sets global flag `hoi4_loader_present`, global variable
  `hoi4_loader_version` (currently **3**, see "Versions and compatibility"), and one flag
  `hoi4_loader_plugin_<name>` per active feature. It clears the flag of every inactive feature.

Mods ship stub versions of the trigger and effect (see the test mod). The loader serves its own
content in their place, so mods work, and stay error-free, with or without the loader.

## Features

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
| `region_scope` | engine (needs `lifecycle`) | `hoi4_loader_plugin_region_scope` | `strategic_region`, `any_`/`every_`/`random_strategic_region` and the region identity triggers, see below |
| `create_army` | engine (needs `lifecycle`) | `hoi4_loader_plugin_create_army` | `create_army`: spawn divisions as an army under a chosen general, see below |

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

Have your stub `hoi4_loader_init` clear the flags too, one `clr_global_flag` per feature in the
table above (the test mod's stub lists them all), so a save loaded without the loader loses them
at the next weekly refresh. `hoi4_loader_present = yes` is always current. With the
`lifecycle` feature the loader refreshes the plugin flags itself right after a save is loaded, in
single player. In multiplayer (and after a hot join) it doesn't, because a change made on one
machine only would desync the game; there the weekly refresh does it. On builds without
`lifecycle`, a `hoi4_loader_plugin_*` flag can lag up to a week after loading a save.

### Stub pack

`stubs/` (also in the release zip) holds every stub file a mod may need, in the folder layout
of a mod. Copy the ones you use into your mod, keeping the paths. Stubs are what the game reads
when the loader is missing or a feature is off. Without them, using a loader trigger is an
"unknown trigger" error.

| You use in script | Copy from `stubs/common/` |
|---|---|
| `hoi4_loader_present` | `scripted_triggers/hoi4_loader_detect.txt` |
| `hoi4_loader_init` (version variable, `hoi4_loader_plugin_*` flags) | `scripted_effects/hoi4_loader_detect.txt` |
| anything from `engine_probe` | `scripted_triggers/hoi4_loader_feature_engine_probe.txt` |
| anything from `lifecycle` | `scripted_triggers/hoi4_loader_feature_lifecycle.txt` |
| `set_`/`reset_strategic_region_name`, `strategic_region_has_custom_name` | `scripted_triggers/hoi4_loader_feature_region_names.txt` |
| the `on_game_loaded` on_action | `scripted_triggers/hoi4_loader_feature_game_loaded.txt` |
| `air_superiority`, `naval_dominance` | `scripted_triggers/hoi4_loader_feature_region_control.txt` |
| `battles_count`, `divisions_count`, `ships_count`, `planes_count` | `scripted_triggers/hoi4_loader_feature_military_counts.txt` |
| `avg_division_strength`, `avg_division_organisation` | `scripted_triggers/hoi4_loader_feature_army_condition.txt` |
| `factories_assigned` | `scripted_triggers/hoi4_loader_feature_production_data.txt` |
| `strategic_region`, `any_`/`every_`/`random_strategic_region`, `strategic_region_id`, `is_sea_region`, `region_has_state` | `scripted_triggers/hoi4_loader_feature_region_scope.txt` |
| `create_army` | `scripted_triggers/hoi4_loader_feature_create_army.txt` |

- **Combinations:** a block that combines features needs each feature's stub and guard, e.g.
  `air_superiority` inside `strategic_region` needs both `region_control` and `region_scope`.
- **The `hoi4_loader_init` stub** clears every `hoi4_loader_plugin_*` flag. Keep it complete
  when you update: a newer stub pack may clear more flags.
- **Updating:** `tools/make_stubs.py` writes the pack from the feature list. `run_smoke.sh`
  fails when `stubs/` or this table misses a feature.

### Warning players when a required feature is off

Guarded content simply disappears when a feature is missing, for example after a game patch,
when the engine tier switches itself off. To tell the player why, show an event once. Copy the
three pieces below and replace `my_mod`, the features and the texts. Ship the guard stubs of
those features (see "Stub pack").

```
# common/scripted_effects/my_mod_requirements.txt
my_mod_check_requirements = {
	if = {
		limit = {
			OR = {
				NOT = { hoi4_loader_feature_region_scope = yes }
				NOT = { hoi4_loader_feature_region_control = yes }
			}
			NOT = { has_country_flag = my_mod_requirements_warned }
		}
		set_country_flag = my_mod_requirements_warned
		country_event = { id = my_mod_requirements.1 hours = 1 }
	}
	else_if = {
		limit = {
			hoi4_loader_feature_region_scope = yes
			hoi4_loader_feature_region_control = yes
		}
		clr_country_flag = my_mod_requirements_warned  # re-arm: warn again if they go missing later
	}
}
```

```
# common/on_actions/my_mod_requirements_on_actions.txt
on_actions = {
	on_startup = {      # new game (no scope, so enter the human countries)
		effect = { every_country = { limit = { is_ai = no } my_mod_check_requirements = yes } }
	}
	on_game_loaded = {  # loaded save, loader with game_loaded present (single player); ROOT = player
		effect = { my_mod_check_requirements = yes }
	}
	on_weekly = {       # fallback: no loader, engine tier off after a patch, or multiplayer
		effect = { if = { limit = { is_ai = no } my_mod_check_requirements = yes } }
	}
}
```

```
# events/my_mod_requirements.txt
add_namespace = my_mod_requirements
country_event = {
	id = my_mod_requirements.1
	title = my_mod_requirements.1.t
	desc = my_mod_requirements.1.d
	picture = GFX_report_event_generic_read_write
	is_triggered_only = yes
	option = { name = my_mod_requirements.1.a }
}
```

```yaml
 my_mod_requirements.1.t:0 "hoi4_loader Features Missing"
 my_mod_requirements.1.d:0 "This mod needs hoi4_loader with its region_scope and region_control features. Without them, the parts that use them are switched off. If you just updated the game, hoi4_loader may not support the new version yet."
 my_mod_requirements.1.a:0 "Understood"
```

**When it shows:**

| Situation | When |
|---|---|
| New game | at the start (`on_startup`) |
| Loaded save, loader present but a feature off (e.g. `loader_config.ini`) | right after loading (`on_game_loaded`) |
| Loaded save without the loader, or after a game patch | within 7 in-game days (`on_weekly`); `on_game_loaded` does not fire then |
| Multiplayer | within 7 days (`on_weekly`) |

- **Once per country:** the flag is saved. A save made after the warning doesn't warn again until
  the features were back in between, and that clears the flag.
- **The test mod** uses the same pieces (`hoi4_loader_test_requirements*` files). Its event also
  lists each feature as available or missing, with scripted localisation.

## Native script commands

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
- **Tooltips** leave the missing part out and show no current value, e.g. "Air superiority of
  FRA above 50%" under `any_strategic_region`'s header, where the region is only known per
  region.
- So write the parameter explicitly wherever the scope isn't certain, or wrap the trigger together
  with a condition that guarantees the scope.

## Versions and compatibility

There are two version numbers:

| Number | Where | Changes when |
|---|---|---|
| **Release** (`0.5.0-beta`) | zip name, `CHANGELOG.md`, first line of `loader.log` | every published build |
| **Script version** (`global.hoi4_loader_version`, now **3**) | script, `loader.log` | something script-visible that already existed changes or goes away |

**What raises the script version:** removing or renaming a command, a parameter, a guard
trigger, a flag or a variable, or changing what an existing one means (its result, its scope,
when it fires).
- History:
  - **1:** presence detection.
  - **2:** per-feature flags and the file/engine tiers.
  - **3:** `set_strategic_region_name` no longer accepts the `id` alias, only `region`. The alias
    was dropped in Part C without a bump. Version 3 records that.

**What does not raise it:**
- **New features, commands or optional parameters.** Script finds them through their guard
  trigger (`hoi4_loader_feature_<name> = yes`), which is the recommended check anyway.
- **Tooltip and log wording.** Serving the guards as `hidden_trigger` (so they print no "Always
  true" line) is an example: their results are unchanged.
- **Bug fixes** that restore the documented behaviour.

So check features with guards, and check the version only to detect a change listed above,
e.g. `check_variable = { global.hoi4_loader_version > 2 }`.

**Compatibility rules:**
- **Guards are forever.** `hoi4_loader_feature_<name>` keeps working for every feature ever
  released. If a feature is removed, the loader stops serving its file, so your stub (`always =
  no`) answers: the feature reads as missing, never as an unknown trigger. Stubs never need to
  be deleted.
- **Deprecation before removal.** A command or parameter that is to go away first keeps working
  for at least one more release.
  - While it is deprecated, `loader.log` says so once per session and names the replacement,
    and MODDING.md and `CHANGELOG.md` mark it. Nothing is deprecated in 0.5.
  - It is removed in a later release, and that release raises the script version.
  - During 0.x the window is one release; from 1.0 on it is one major version.
- **Saves.** The loader stores nothing outside vanilla flags and variables (e.g. `hoi4_srn_*`),
  so saves load with and without it, and with any later loader release.
- **Game versions.**
  - The engine tier runs only on hoi4.exe builds listed in `loader_config.ini`.
  - After a game patch it switches itself off until a release (or you) lists the new hash. A
    list entry is a SHA-256 of hoi4.exe, and only builds that were checked are listed.
  - The file tier (detection, guards, virtual files) keeps working on any build, so mods see
    the features as missing rather than broken.
  - `loader.log` then reports which signatures still match (see [DEVELOPING.md](DEVELOPING.md), "Unknown game builds").

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
thread at the start of a frame, not inside the effect (see [DEVELOPING.md](DEVELOPING.md), "Threads and hooks"). So right after
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
- **Tooltips:**
  - With `region` written, they are worded like vanilla's province renames, e.g. "**Bay of
    Biscay** is renamed to **Loader Test Sea A**." / "**Bay of Biscay** gets its original name
    back."
  - Inside a strategic region scope (`region` left out), they are worded like vanilla's state
    renames: "Changes name to **Loader Test Sea A**." / "Gets its original name back." The
    scope's header already names the region. An `every_strategic_region` block also builds its
    lines only once, for its first region, so a named line would mention only that one.
  - The loader shows English unless a loaded mod defines the keys, so no loc file is needed. To
    translate them, add to your mod's localisation for each language:
    ```yaml
     HOI4_LOADER_SET_STRATEGIC_REGION_NAME:0 "$OLDNAME|Y$ wird zu $NAME|Y$ umbenannt."
     HOI4_LOADER_RESET_STRATEGIC_REGION_NAME:0 "$NAME|Y$ erhält seinen ursprünglichen Namen zurück."
     HOI4_LOADER_SET_STRATEGIC_REGION_NAME_IN_SCOPE:0 "Ändert den Namen zu $NAME|Y$."
     HOI4_LOADER_RESET_STRATEGIC_REGION_NAME_IN_SCOPE:0 "Erhält seinen ursprünglichen Namen zurück."
    ```
  - `$OLDNAME$` is the name shown now, `$NAME$` the new one; the game formats them (`|Y`). The
    keys are also listed in `loader_script_docs.md`.
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
  - **Under `NOT`** the header keeps its wording and the lines inside are negated, exactly as
    the game does for `any_state`; its localisation has no "none of" text. So
    `NOT = { any_strategic_region = { count = 99 is_sea_region = yes } }` (fewer than 99 sea
    regions) reads "At least 99 of Any strategic region: Is a land region", which says
    something else. Give such a block its own header:
    ```
    NOT = { any_strategic_region = { tooltip = my_fewer_than_99_sea_regions_tt count = 99 is_sea_region = yes } }
    ```
    ```yaml
     my_fewer_than_99_sea_regions_tt:0 "Fewer than 99 sea regions"
    ```
  - `every_` / `random_strategic_region` show the region names and the effects, as vanilla does
    for states. For random, that is the region that will be picked: the tooltip and the
    execution make the same draw from the same scope seed.
    - The pick changes only when the candidate list changes, or when the decision's own record
      changes after it was taken. It does not change with the date.
    - It can differ from the tooltip in two cases. The first is when `limit` gives a different
      result by the time the decision is taken.
    - The second is when an effect earlier in the same block draws a random number only when it
      runs and not for its tooltip. Put `random_strategic_region` first if that matters.
    - Details in NOTES.md F9.
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

## Armies under a chosen general (`create_army`)

Vanilla `create_unit` spawns divisions without an army, and no vanilla effect puts given divisions
under a given general. `create_army` does both: it runs vanilla `create_unit` blocks and makes the
divisions they create a new army led by the general you name.

```
55 = {                                   # a state, as for vanilla create_unit (55: Nassau)
    create_army = {
        leader = GER_erwin_rommel        # character id (required)
        name = "1. Panzerarmee"          # army name (optional; else the game's default "Nth Army")
        reassign = yes                   # optional, see "General already leads an army"
        country = GER                    # optional: the army's country (default ROOT)
        create_unit = {
            division = "division_template = \"Infanterie-Division\" start_experience_factor = 0.5 start_equipment_factor = 1.0"
            owner = ROOT
            prioritize_location = 6488   # Frankfurt
            count = 8
        }
        create_unit = {
            division = "division_template = \"Panzer-Division\" start_experience_factor = 0.5"
            owner = ROOT
            prioritize_location = 6488
            count = 2
        }
    }
}
```

- **Scope:** a state, exactly where vanilla `create_unit` must be used (`55 = { ... }`,
  `capital_scope = { ... }`, `random_owned_state = { ... }`). The divisions spawn in that state.
- **The army's country:** `country` (a tag, ROOT, THIS, FROM or PREV), by default ROOT. In a
  decision, event or focus that is the country taking it.
- **Inside:** only `leader`, `name`, `reassign`, `country` and any number of `create_unit` blocks.
  - The `create_unit` blocks are vanilla, with all their keys (`division`, `owner`,
    `prioritize_location`, `allow_spawning_on_enemy_provs`, `country_score`, `count`, `id`,
    `divisional_commander_xp`).
  - Any other key is a parse error: `error.log` names the file and line ("Unexpected token"),
    and that `create_army` does nothing. That keeps it exact which divisions join the army: the
    ones its `create_unit` blocks created.
- **Which divisions join:** every division the blocks created for the army's country, so give
  `create_unit` the same `owner` (usually `owner = ROOT`). Divisions created for another owner
  stay outside the army.
- **Where they spawn:** in the state in scope, wherever `create_unit` puts them by its own rules:
  `prioritize_location` if that province of the state works, otherwise the state's owned
  provinces first, then friendly ones, enemy ones only with `allow_spawning_on_enemy_provs =
  yes`. Divisions of one army may stand in different provinces, as in any army. An army spread
  over several states needs one `create_army` per state; they make separate armies.
- **Size:** no limit. More divisions than the general's cap (vanilla
  `NMilitary.CORPS_COMMANDER_DIVISIONS_CAP` and `FIELD_MARSHAL_DIVISIONS_CAP`, both 24) is
  allowed, as in the game, and reduces the general's bonuses ("Commanding too many divisions").
- **Command power:** none is charged (vanilla's `start_border_war` creates its armies the same
  way). If your mod wants a cost, add vanilla `add_command_power = -20` in the same effect block.

**The general (`leader`).** The character must be:
- an army leader: a general (corps commander) or a field marshal (a field marshal may lead a
  single army, as in the game);
- recruited by the army's country;
- not captured, and not leading an army group.

If any of these fails, **nothing spawns**. `loader.log` says why, and the tooltip shows a red line
such as "Loader Test Admiral cannot lead a new army: not a general or field marshal. Nothing is
created."

**General already leads an army.**
- **Default:** refused. Nothing spawns, logged, red tooltip line "... already leads an army".
- **`reassign = yes`:** the general leaves the army they lead now and takes the new one. The old
  army keeps its divisions without a general. The tooltip adds "... leaves <old army>, which keeps
  its divisions without a general."
- Refused either way while the old army's headquarters is deploying or withdrawing, as in the
  game's own "assign general" button.
- **AI countries' generals usually lead armies already:** use `reassign = yes`, or pick a
  general who is free.

To show the condition in the decision itself, check it with vanilla triggers:

```
my_mod_raise_panzer_army = {
    visible = { hoi4_loader_feature_create_army = yes }
    available = {
        hoi4_loader_feature_create_army = yes
        has_character = GER_erwin_rommel
        GER_erwin_rommel = {
            is_army_leader = yes
            is_leading_army = no            # leave out with reassign = yes
            is_leading_army_group = no
            is_general_captured = no
        }
    }
    complete_effect = {
        capital_scope = {
            create_army = { leader = GER_erwin_rommel name = "1. Panzerarmee" create_unit = { ... } }
        }
    }
}
```

**When nothing is created.** Logged in `loader.log`; no crash, nothing half done:

| Case | Result |
|---|---|
| General not available (any reason above) | nothing spawns; red tooltip line |
| `create_unit` creates nothing (unknown template, no valid province) | no army; the general is unchanged; `create_unit`'s own error in `error.log` |
| A key that is not allowed inside | parse error at load; this `create_army` does nothing |
| The game refuses to create the army | the divisions stay without an army |

**Tooltip.**
- "Creates a new army led by **Erwin Rommel**." With `name`: "Creates the army
  **1. Panzerarmee** led by **Erwin Rommel**." `create_unit` itself shows no line (as in
  vanilla, mods usually add a `custom_effect_tooltip` for the divisions).
- All texts can be translated with the keys listed in `loader_script_docs.md`:
  - `HOI4_LOADER_CREATE_ARMY`, `_NAMED`, `_LEAVES` and `_UNAVAILABLE`;
  - one key per reason, e.g. `HOI4_LOADER_CREATE_ARMY_LEADS_ARMY`.

**AI countries.**
- The game's AI rearranges its own armies by front. It moves divisions between them and assigns
  generals.
- A `create_army` for an AI country gives it that army as a starting point. The AI may later move
  divisions out or give the army another general.
- Players' armies are not touched.

**Multiplayer:** the army is created by the game's own functions, called the way vanilla's
`start_border_war` calls them. `create_unit` places divisions with the game's synced random
numbers. Every machine runs the same effect and gets the same army.

**Saves:** the army, its divisions, its name and its general are saved by the game itself. A save
loaded without the loader keeps them (they are ordinary armies).

**Without the loader:** `create_army` is an unknown effect. The game logs it once per use when
parsing, and the whole block, `create_unit`s included, does nothing. Guard decisions with
`hoi4_loader_feature_create_army = yes` (stub: `stubs/common/scripted_triggers/hoi4_loader_feature_create_army.txt`).
