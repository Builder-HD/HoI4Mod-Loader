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
| `add_to_army` | engine (needs `create_army`) | `hoi4_loader_plugin_add_to_army` | `add_to_army`: put a division into a general's army, see below |
| `railways` | engine (needs `lifecycle`) | `hoi4_loader_plugin_railways` | `downgrade_railway`: lower or remove a railway; `has_railway_between` trigger, see below |
| `railway_queue` | engine (needs `railways`) | `hoi4_loader_plugin_railway_queue` | `queue_railway_dismantle`: dismantling as a construction queue entry, see below |
| `time_control` | engine (needs `lifecycle`) | `hoi4_loader_plugin_time_control` | `force_game_speed`, `release_game_speed`, `hide_date_display`, `hide_time_controls`, `has_forced_game_speed`: lock the game speed (and the pause) for a while, hide the top bar's date; single player, see below |
| `province_building_removal` | engine (needs `lifecycle`) | `hoi4_loader_plugin_province_building_removal` | a removal cross on the state view's province buildings (supply hub, naval base, bunkers); no script commands, see below |
| `equipment_conversion` | engine (needs `lifecycle`) | `hoi4_loader_plugin_equipment_conversion` | tools a mod switches on with global flags: "Convert from Stockpile" from every earlier generation, a cost factor, "same role only"; off until a mod sets them, see below |
| `air_scenarios` | engine | `hoi4_loader_plugin_air_scenarios` | new air scenarios (the 3D air animations on the map) for chosen missions, planes and countries, defined in `common/hoi4_loader_air_scenarios/`; no script commands, see below |

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
| `add_to_army` | `scripted_triggers/hoi4_loader_feature_add_to_army.txt` |
| `downgrade_railway`, `has_railway_between` | `scripted_triggers/hoi4_loader_feature_railways.txt` |
| `queue_railway_dismantle` | `scripted_triggers/hoi4_loader_feature_railway_queue.txt` |
| `force_game_speed`, `release_game_speed`, `hide_date_display`, `hide_time_controls`, `has_forced_game_speed` | `scripted_triggers/hoi4_loader_feature_time_control.txt` |
| a check that `province_building_removal` is on (it has no script commands) | `scripted_triggers/hoi4_loader_feature_province_building_removal.txt` |
| `equipment_conversion` (no commands; for guards only) | `scripted_triggers/hoi4_loader_feature_equipment_conversion.txt` |
| `air_scenarios` (no commands; for guards only) | `scripted_triggers/hoi4_loader_feature_air_scenarios.txt` |

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
- Refused either way in the cases the game's own "assign general" button refuses: the loader runs
  the game's own check on the army the general leads now. The red line names the case, in the
  game's words:
  - "their army <name> is currently being deployed";
  - "their army <name> is currently being withdrawn";
  - "their army <name> is in a border war" (e.g. after vanilla `start_border_war` picked this
    general).
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
| General not available (any reason above) | nothing spawns; red tooltip line, without the `create_unit` lines |
| `create_unit` creates nothing (unknown template, no valid province) | no army; the general is unchanged; `create_unit`'s own error in `error.log` |
| A key that is not allowed inside | parse error at load; this `create_army` does nothing |
| The game refuses to create the army | the divisions stay without an army |

**Tooltip.**
- "Creates a new army led by **Erwin Rommel**." With `name`: "Creates the army
  **1. Panzerarmee** led by **Erwin Rommel**." Below it each `create_unit` adds its own vanilla
  line, e.g. "Create 10 Italian unit in Lazio". When the general is not available, only the red
  line shows: nothing will be created, so the `create_unit` lines are left out.
- **The preview shows the general as things are when the player hovers.** If the same effect
  block makes the general available first, the line is red although taking it works. For
  example, `recruit_character` or freeing the general earlier in the block. Recruit the general
  in an earlier decision, event or focus for an accurate preview.
- All texts can be translated with the keys listed in `loader_script_docs.md`:
  - `HOI4_LOADER_CREATE_ARMY`, `_NAMED`, `_LEAVES` and `_UNAVAILABLE`;
  - one key per reason, e.g. `HOI4_LOADER_CREATE_ARMY_LEADS_ARMY`.

**AI countries.**
- The game's AI rearranges its own armies by front. It moves divisions between them and assigns
  generals.
- **For an AI country the army is only a starting point.** In the test (Italy, 1936), the AI:
  - moved the general to another front at once (the Ethiopian front);
  - kept the 10 divisions together for a few days and added others to them;
  - had split them over its own armies after about a month, regrouping again whenever it changed
    its theatres.

  So the general does not stay with the divisions. Use `create_army` for player countries, or
  for an AI country when only the initial setup matters.
- Players' armies are not touched.

**Multiplayer:** built so that every machine runs the same effect and gets the same army: the
army is created by the game's own functions, called the way vanilla's `start_border_war` calls
them, and `create_unit` places divisions with the game's synced random numbers. **Not tested in
multiplayer yet.**

**Saves:** the army, its divisions, its name and its general are saved by the game itself. A save
loaded without the loader keeps them (they are ordinary armies).

**Without the loader:** `create_army` is an unknown effect. The game logs it once per use when
parsing, and the whole block, `create_unit`s included, does nothing. Guard decisions with
`hoi4_loader_feature_create_army = yes` (stub: `stubs/common/scripted_triggers/hoi4_loader_feature_create_army.txt`).

### Adding divisions to a general's army (`add_to_army`)

`create_army` makes an army from new divisions. `add_to_army` puts **existing** divisions into the
army a general leads, the way the player assigns divisions to an army in the game. If the general
leads no army yet, the first division forms one under them.

```
every_country_division = {               # vanilla division scope
    limit = { division_has_majority_template = cavalry }
    add_to_army = {
        leader = GER_erwin_rommel        # character id (required)
        name = "1. Panzerarmee"          # optional: only used when a new army is formed
    }
}
```

- **Scope:** a division, e.g. inside `every_country_division`, `random_country_division`,
  `every_state_division` or `random_state_division`. Anywhere else the effect does nothing.
- **What happens:**
  - The division joins the army the general leads, through the game's own assignment. It leaves
    its old army; an old army left without divisions is disbanded, as when the player reassigns
    divisions in the game.
  - **The general leads no army:** a new army is formed from this division, led by the general
    and named `name` if given. The next divisions of the same `every_country_division` then join it.
  - **The division is already in that army:** nothing happens.
- **Orders:** the division joins without the army's front or other orders. Give the army its
  orders in game.
- **Command power:** none is charged (the game's own assignment costs none either).

**The general (`leader`)** must be, as for `create_army`:
- an army leader (general or field marshal);
- recruited by the **division's** country;
- not captured, and not leading an army group.

**When nothing happens.** That division is left as it is; `loader.log` says why (a reason that
repeats for every division of an `every_country_division` is logged once and then counted), and
the tooltip shows a red line such as "Cannot join the army of Erwin Rommel: not recruited."

| Case | Reason |
|---|---|
| The general is unknown, not recruited, serves another country, is not an army leader, is captured, or leads an army group | as for `create_army` |
| The division's army is in a border war | "its army <name> is in a border war" (the game's own assignment refuses this too) |
| The general's army is in a border war | "their army <name> is in a border war" (stricter than the game, so a script can't change the strength of a running border war) |
| The scope is not a land division | "this is not a land division" |

**Tooltip.**
- "Joins the army of **Erwin Rommel**."
- "Forms a new army under **Erwin Rommel**." (with `name`: "Forms the army **1. Panzerarmee**
  under **Erwin Rommel**.") when the general leads no army at the moment of hovering.
- Inside `every_country_division` the game shows the line once, for the iterator as a whole; the
  general is then checked against the scope's country.
- The texts can be translated: `HOI4_LOADER_ADD_TO_ARMY`, `_NEW`, `_NEW_NAMED`, `_UNAVAILABLE`,
  `HOI4_LOADER_ADD_TO_ARMY_NOT_DIVISION`, `HOI4_LOADER_ADD_TO_ARMY_DIVISION_BORDER_WAR`, and the
  `create_army` reason keys (`loader_script_docs.md` lists them).

**AI countries:** as for `create_army`, the AI rearranges its own armies, so for an AI country
the result is only a starting point. Players' armies are not touched.

**Multiplayer:** an ordinary effect that runs the game's own assignment on every machine in the
same order. **Not tested in multiplayer yet.**

**Saves:** the armies are saved by the game itself; a save loaded without the loader keeps them.

**Without the loader:** `add_to_army` is an unknown effect. The game logs it once when parsing and
skips it; the other effects in the same block still run. Guard with
`hoi4_loader_feature_add_to_army = yes` (stub:
`stubs/common/scripted_triggers/hoi4_loader_feature_add_to_army.txt`).

## Lowering railways (`railways`)

Vanilla script can build railways (`build_railway`) but never lower them:
`remove_building = { type = rail_way }` only changes a hidden building that mirrors the level,
not the railway itself (NOTES.md Part H). The `railways` feature adds a native effect and a
trigger:

```
downgrade_railway = { start_province = 6521 target_province = 6282 }                   # one level
downgrade_railway = { start_province = 6521 target_province = 6282 levels = 2 }        # two levels
downgrade_railway = { start_province = 6521 target_province = 6282 levels = all }      # remove it
downgrade_railway = { start_province = 6521 target_province = 6282 all_routes = yes }  # disconnect
has_railway_between = { start_province = 6521 target_province = 6282 }                 # trigger
has_railway_between = { start_province = 6521 target_province = 6282 min_level = 3 }
```

- **Which links:** the existing railway with the fewest links from `start_province` to
  `target_province` (province ids from `map/definition.csv`). Every link on it loses `levels`
  levels (default 1, at least 1). A link that reaches 0 is removed; `levels = all` removes the
  whole route.
- **Every route:** `all_routes = yes` removes the shortest route completely, then the next one,
  until no railway connects the two provinces. That includes detours, which may run far away or
  through other countries, so use it where the network is known. `levels` is ignored with it.
- **What is updated:** both ends of each link, each province's `rail_way` building (which
  `has_railway_level` reads) and the supply system, which recalculates supply and redraws the
  railway as it does after `build_railway`.
- **Nothing to lower:** if no railway connects the two provinces (or they are the same), nothing
  changes and `loader.log` says so.
- **Construction:** the effect is immediate. A railway construction line on the same links keeps
  building upwards from the new level.
- **Tooltip:** "Railway from province 6521 to province 6282 (3 links) loses 1 level(s)." A mod
  can translate it with the keys `HOI4_LOADER_DOWNGRADE_RAILWAY` (placeholders `$FROM$`, `$TO$`,
  `$LINKS$`, `$LEVELS$`), `HOI4_LOADER_REMOVE_RAILWAY` (`levels = all`; `$FROM$`, `$TO$`,
  `$LINKS$`), `HOI4_LOADER_REMOVE_ALL_RAILWAYS` (`all_routes`; the same, `$LINKS$` of the
  shortest route) and `HOI4_LOADER_DOWNGRADE_RAILWAY_NONE` (`$FROM$`, `$TO$`).
- **The trigger:** `has_railway_between` is true if any railway route connects the two provinces;
  with `min_level`, only links of at least that level count ("is there a level 3 line all the
  way?"). Vanilla's `has_railway_connection` answers the plain question too (and also takes states
  and `build_only_on_allied`), but has no level. Tooltip keys: `HOI4_LOADER_HAS_RAILWAY_BETWEEN`,
  `HOI4_LOADER_HAS_RAILWAY_BETWEEN_LEVEL`, `HOI4_LOADER_HAS_NO_RAILWAY_BETWEEN` and
  `HOI4_LOADER_HAS_NO_RAILWAY_BETWEEN_LEVEL` (`$FROM$`, `$TO$`, `$LEVEL$`). Put the guard in the
  same AND block: without the loader it is an unknown trigger.
- **Scope:** any. The provinces are always written explicitly.
- **Guard:** `hoi4_loader_feature_railways = yes` (ship the stub). Without the loader the effect is
  an unknown effect and does nothing.
- **Self-check:** on every game entry the loader compares each stored railway link with its
  other end. On a mismatch (e.g. a game patch moved the data) the feature switches itself off,
  its guard and flag read as missing, and `loader.log` names the first bad link.
- **Multiplayer:** the effect only reads and writes game state and picks the path
  deterministically, so it runs the same on every machine; all players need the loader.
- **Game version:** the railway signatures were found on 1.19.4.2. They are not yet checked on the
  listed 1.19.3 builds; there the feature may report missing signatures and stay off.

### Dismantling through the construction queue (`railway_queue`)

```
queue_railway_dismantle = { start_province = 6521 target_province = 6282 }             # one level
queue_railway_dismantle = { start_province = 6521 target_province = 6282 levels = all }  # all of it
```

- **What it does:** adds an entry to the **scope country's** construction queue (railway tab, at
  the end of the queue) for the same path `downgrade_railway` would take. The entry lowers the
  links one level at a time, in path order, down to the path's highest level minus `levels`
  (default 1); links already at or below that stay as they are.
- **Cost and time:** each level costs 30% of what building that level costs (the game's own
  railway cost, with the country's modifiers), so with the same factories it takes 30% of the
  time. Factories are assigned to it like to any construction. A mod can change the share with a
  game rule, see "Dismantling cost" below.
- **In the queue:** its progress bar is filled from the dismantling cost (how the entry's level
  text looks is not checked in game yet). Cancelling it works as for any line and keeps what was
  dismantled so far.
- **Don't use the queue's level buttons on it:** the engine raises the target back to at least the
  starting level, which turns the entry into a normal railway line that rebuilds lowered links up
  to that level.
- **Stops by itself:** like a railway being built, if the path is no longer usable (lost control
  of a province on it), the entry is cancelled.
- **Saves:** the entry is saved with the game. Without the loader the game sees a railway line
  whose target is below the railway, and finishes it the next day without changing anything
  (expected from the engine's code; the in-game check is still open, NOTES.md H3).
- **If `railways` switches itself off** (its self-check on game entry fails), `railway_queue` goes
  off with it for the rest of the session. Entries already in the queue **pause** (no progress,
  not finished) and stay in the queue and in the save, so they carry on in a later session where
  the feature works. Nothing else changes railways meanwhile, and the map badges are vanilla.
- **Where the country may not dismantle:** `queue_railway_dismantle` still adds the entry, but its
  first daily step cancels it (see "Where the player may dismantle" below). The effect's tooltip
  then adds a red line (`HOI4_LOADER_QUEUE_RAILWAY_DISMANTLE_NOT_ALLOWED`) and `loader.log` says so
  when it runs.
- **Scope:** country. **Guard:** `hoi4_loader_feature_railway_queue = yes` (ship the stub). The
  feature needs `railways` and has its own guard, so a game update that breaks only the queue
  signatures leaves `downgrade_railway` working.
- **Tooltip** key: `HOI4_LOADER_QUEUE_RAILWAY_DISMANTLE` (`$FROM$`, `$TO$`, `$LINKS$`, `$LEVEL$`).
- **The entry's own tooltip** in the construction queue is vanilla's "Upgrading ... railways ... to
  level ...". If the loaded localisation defines `HOI4_LOADER_RAILWAY_DISMANTLE_TOOLTIP`, dismantle
  entries use it instead (other railway entries keep the vanilla text). Same placeholders as
  vanilla's `RAILWAY_UPGRADE_TOOLTIP`: `$START_LEVEL$`, `$RAILWAY_NAME$`, `$TARGET_LEVEL$`.
  ```yaml
   HOI4_LOADER_RAILWAY_DISMANTLE_TOOLTIP:0 "§RDismantling§! level $START_LEVEL|H$ railways in $RAILWAY_NAME|H$ to level $TARGET_LEVEL|H$"
  ```
  Without the key the vanilla upgrade text shows. `loader.log` says which on first use.
- **On the map (single player):** in the railway construction view, hold **Alt+Shift**: the
  level circle on every railway segment you may dismantle shows vanilla's removal cross (the one
  on the state view's buildings; a red **X** if the text icon cannot be used). **Alt+Shift+click**
  it to add one dismantle level for that segment to the construction queue; click again for
  another level (the same entry goes one level deeper, down to 0). Vanilla reads three keys on
  those circles: Shift (upgrade to the maximum level), Ctrl (first in the queue) and Alt (last in
  the queue). Only Alt+Shift+click (vanilla: maximum level, last in the queue) on a circle you may
  dismantle is taken over; every other combination, and Ctrl+Alt+Shift, stays vanilla. In
  multiplayer the click is ignored (a click happens on one machine only; it would desync), and
  `loader.log` says so once.
- **Where the player may dismantle** (the X, Alt+Shift+click, and every day for a queued entry): every
  province of the segment must be controlled by the player's country, or by a country these
  global flags allow:

  | Global flag | Also allowed |
  |---|---|
  | `hoi4_loader_railway_dismantle_allies` | members of the country's faction |
  | `hoi4_loader_railway_dismantle_subjects` | the country's subjects |
  | `hoi4_loader_railway_dismantle_overlord` | the country's overlord |

  A queued entry is cancelled (what was dismantled stays dismantled) once that no longer holds,
  e.g. a province changes hands or an alliance ends. The usual way to set the flags is a game
  rule per flag, read at `on_startup` (`has_game_rule`) to set the flag. Note that 1.19.4.2
  accepts `desc` only inside a rule's `option` / `default` blocks, not on the rule itself
  ("Unexpected token: desc" in `error.log`). Script may also set or clear them at any time. The effects (`downgrade_railway`, `queue_railway_dismantle`) are
  not limited by them: script decides where it uses those.
- **Dismantling cost:** 30% of the building cost per level by default. A global flag
  `hoi4_loader_railway_dismantle_cost_<percent>` (0 to 1000, e.g. `..._cost_50` for half) changes
  it for every queued dismantle, Alt+Shift+click and `queue_railway_dismantle` alike; with several set,
  the most recently set one counts. It is read every day, so an entry already in the queue
  follows a change (its progress so far is kept, capped at the new total). The rules-only mod has
  a game rule "Cost" (10, 20, 30, 50, 75 or 100%) that sets the flag at game start; it is last in the
  rules' own "Dismantle Railways" group (`group = "RULE_GROUP_HOI4_LOADER_RAILWAY_DISMANTLE"`, a
  new group needs only that string and a localisation key for its header).
  `downgrade_railway` is immediate and has no cost.
- **Railway entries' titles** ("Berlin - Stettin") start with a marker: vanilla's red down triangle
  text icon (`£decrease_texticon£`) for dismantling, its green up triangle (`£increase_texticon£`)
  for building and upgrading. A mod can replace them by defining
  `HOI4_LOADER_RAILWAY_DISMANTLE_PREFIX` / `HOI4_LOADER_RAILWAY_BUILD_PREFIX` (the text goes in
  front of the title; include the trailing space). Without them the loader's built-in markers
  show; no mod needs to define anything for that.

## Game speed and the date display (`time_control`)

Lock the game speed from script for a while, optionally without pause, and hide the date in the
top bar. **Single player only.**

```
force_game_speed = {
    speed = 2                 # 1-5, as the speed buttons count (required)
    days = 2                  # optional: for this long, days and/or hours ...
    hours = 12
    until = "1940.5.10.12"    # ... or until this date: year.month.day or year.month.day.hour
    lock_pause = no           # optional; default yes: the player cannot pause meanwhile
}
release_game_speed = yes      # ends the lock now
hide_date_display = yes       # hides the date and time in the top bar; `= no` shows it again
hide_time_controls = yes      # hides the whole time panel; `= no` shows it again
has_forced_game_speed = yes   # trigger: a lock is running (`= no`: none)
```

- **The lock:** while it runs, every speed change gives the locked speed. That covers the speed
  buttons, the +/- keys and the console's `gamespeed`. The buttons still click; the speed stays.
- **How long:** `until` a date, or `days` and/or `hours` from now, or without them until
  `release_game_speed = yes`. `until` must be in the future. The hour is 1-24 as the top bar
  shows it ("12:00"); without it the lock ends at the start of that day. When the time is up the
  lock ends by itself.
- **When it ends** (time up or released), the speed the player had before the lock comes back. A
  new `force_game_speed` while one runs replaces it and keeps that original speed.
- **Pause:** with `lock_pause = yes` (the default) the player's pause is refused: the pause button,
  Space and the console. Still allowed:
  - unpausing, always;
  - the game's own automatic pauses (event popups with "pause on event" in the settings);
  - the Esc menu, which pauses as usual.
- **If the game is paused** when `force_game_speed` runs (for example a decision taken while
  paused), it is unpaused.
- **Date display:** `hide_date_display = yes` hides the date and time text in the top bar (the
  pause button and the speed steps stay). It changes at once, also while paused.
- **Time panel:** `hide_time_controls = yes` hides the whole panel: the date, the pause button,
  the −/+ buttons and the five speed steps. The top bar's empty frame stays. It is independent of
  `hide_date_display`, and a running `force_game_speed` holds either way. Whether the panel's
  keys (Space, numpad +/−) still act while it is hidden is not tested yet; the lock refuses them
  anyway.
- **Tooltips:** "The game speed is locked at **2** for **2** days. Meanwhile the game cannot be
  paused.", "The date is hidden." The texts can be translated with the keys in
  `loader_script_docs.md` (`HOI4_LOADER_FORCE_GAME_SPEED`, `_UNTIL`, `_DAYS`, `_HOURS`,
  `_NO_PAUSE`, `HOI4_LOADER_RELEASE_GAME_SPEED`, `HOI4_LOADER_HIDE_DATE_DISPLAY`,
  `HOI4_LOADER_SHOW_DATE_DISPLAY`, `HOI4_LOADER_HIDE_TIME_CONTROLS`, `HOI4_LOADER_SHOW_TIME_CONTROLS`).

```
my_mod_operation_weserubung = {
    visible = { hoi4_loader_feature_time_control = yes }
    available = { has_forced_game_speed = no }
    complete_effect = {
        force_game_speed = { speed = 1 days = 7 }   # a week at the slowest speed, no pause
    }
}
```

**Saves:** saved with the game as global flags:
- `hoi4_loader_time_lock`: the lock;
- `hoi4_loader_date_hidden`: the hidden date;
- `hoi4_loader_time_controls_hidden`: the hidden time panel.

A mod may test the last two with `has_global_flag`. After loading, all three come back; a loaded game stays paused until the
player unpauses. A timed lock's end is rounded to whole days after loading (it may end up to a
day later than it would have). A save loaded without the loader runs at normal speeds with the
date shown; the flags do nothing there.

**Not from `on_startup`:** it runs before the loader has checked that the game is single player,
so the effects do nothing there (logged). Fire an event from `on_startup` with `hours = 1` (or use
a decision or focus) and run them in that event.

**Multiplayer:** not supported. The effects do nothing there (logged once), and a save's lock or
hidden date is ignored when it is loaded in multiplayer.

**Without the loader:** the effects and the trigger are unknown to the game (logged once per use
when parsing) and do nothing. Guard them with `hoi4_loader_feature_time_control = yes` (stub:
`stubs/common/scripted_triggers/hoi4_loader_feature_time_control.txt`).

## Removing province buildings (`province_building_removal`)

No script commands: this feature changes the UI.

- **What the player sees:** in the state view's province row (supply hub, naval base, bunker,
  coastal bunker and the other province buildings), each building gets the same red cross that
  vanilla shows on dockyards and factories in the shared slots. Clicking it asks for confirmation
  (below); OK removes one level of that building in that province, as vanilla's removal does for
  state buildings.
- **When the cross shows:** the province has a level of the building, the local player's country
  owns the state (as vanilla requires for the shared slot cross) and controls the province. So
  there is no cross in an occupied enemy province, nor in an owned province the enemy occupies.
  During a war, vanilla's removal cooldown applies: the cross is
  greyed out and its tooltip says until when. A removal starts the cooldown, as for state
  buildings. The tooltip is vanilla's `BUILDING_SLOT_REMOVE_BUILDING_LEVEL` ("Remove the
  building.").
- **Confirmation:** a click opens vanilla's "Remove Building" dialog ("Are you sure you want to
  remove this Building?"), as on dockyards, and OK removes the level. If the building is gone, the
  player loses the state or the province, or the cooldown starts while the dialog is open, it
  closes without removing anything. (If the dialog's engine functions are not found on a build,
  the cross removes without asking; `loader.log` says which.)
- **Supply hubs:** the supply system follows the removal (the same notification
  `remove_building` sends): the hub stops supplying at the next supply update.
- **Single player only:** a click happens on one machine, so in multiplayer the cross stays hidden.
- **Fail-safe:** if anything in the loader's part faults, the feature logs it and switches itself
  off for the session. The crosses hide (also in state views opened afterwards), clicks do
  nothing, and `hoi4_loader_feature_province_building_removal` turns false.
- **Saves:** nothing of the loader's own is saved. A removal changes only what vanilla's removal
  changes (the building level, and the state's last removal date for the cooldown), so saves load
  the same without the loader.
- **How the cross gets there:** the loader patches `interface/countrystateview.gui` as the game
  reads it, adding a `remove` button (`GFX_cancel_conversion_button`) at the end of
  `province_building_entry`. It patches whichever copy the game loads, vanilla's or a mod's, so a
  UI mod that changes that file keeps its version. If the entry already has a `remove` button, or
  there is no `province_building_entry`, the file is left as it is. Without the loader, or with
  the feature off from the start, the file is not patched and there is no button at all. If the
  feature switches itself off during a game, the patched file stays loaded and the loader keeps
  the button hidden.
- **Script:** use vanilla `remove_building = { type = supply_node level = 1 province = N }`; it
  updates supply too. `hoi4_loader_feature_province_building_removal = yes` (ship the stub) tells
  whether the cross is available, for example for a hint in a decision's description.
## Converting earlier generations (`equipment_conversion`)

Tools for a mod about equipment conversion. **The loader changes nothing on its own:** until a mod
sets the flags below, conversion is exactly the base game's, AI included. The separate mod
**HoI4Mod Loader: Equipment Conversion** (its own repository) sets them from game rules.

Vanilla's **Convert from Stockpile** on a production line only takes the equipment that the
target's `can_convert_from` list names, or other variants of the same equipment. So Infantry
Equipment 0 never becomes Infantry Equipment II, and a mod that wants it writes a
`can_convert_from` pair for every equipment it has, and cannot cover other mods' equipment.

**The flags** (global flags; set them at `on_startup`, from game rules or however the mod likes):

| Flag | What it switches on |
|---|---|
| `hoi4_loader_conversion_generations` | every equipment type also converts from all of its earlier generations: the types along its `parent =` chain within the same archetype (`infantry_equipment_0` and `_1` into `_2`, `light_tank_chassis_1` designs into `light_tank_chassis_2` designs). For every archetype and every mod that defines `parent`, with nothing else to write |
| `hoi4_loader_conversion_factor_<percent>` | with generations on: the cost factor per generation beyond the first (0-100; the most recently set counts; none: 75) |
| `hoi4_loader_conversion_existing_loader_cost` | with generations on: the factor also applies to pairs the game or a mod already allows (default: their game cost) |
| `hoi4_loader_conversion_naval` | with generations on: earlier ship hulls too (refits); without it ships refit only as the game allows |
| `hoi4_loader_conversion_role_mode` | Shift+click on a line's convert button toggles "same role only", see below |

- **When a flag change counts:** always at game entry (a new game after `on_startup`, a loaded
  save, a hot join), so a flag set at `on_startup` counts at once. In **single player** also
  mid-game: the loader reads the flags every 30 frames (a decision or the console counts within a
  second). In **multiplayer only at game entry:** a flag a mod sets or clears mid-game counts from
  the next load, so every machine changes at the same point. Clearing
  `hoi4_loader_conversion_generations` removes the loader's pairs again.
- Without `hoi4_loader_conversion_generations` the conversion lists stay the game's; with it the
  loader adds the pairs at a safe point (no tick running) and checks them.

**Which stock the game takes** (unchanged by the loader): a country's own designs convert only
once they are **decommissioned** (the production view's "Decommission"); foreign designs
(captured, from a country that is not friendly) always.

**Cost:** the game's own conversion cost (the `NProduction` conversion defines, modules,
upgrades). For a source one generation back that is all. Each further generation back multiplies
it by the factor: with 75%, two back costs 75% and three back 56%. Strategic resources stay as the
game computes them. A pair the game or a mod already allows (a `can_convert_from` entry between
generations, such as older light tanks into light tank artillery) keeps the game's cost unless
`hoi4_loader_conversion_existing_loader_cost` is set; it is never refused.

**Who:** every country, AI included: the extra pairs are in the game's own conversion data, which
the AI's production planning reads too.

**Same role only** (`hoi4_loader_conversion_role_mode`; Shift+click on a line's convert button,
single player): vanilla converts every stockpiled design of the same airframe, chassis or hull, so
a fighter line also uses up CAS and naval bomber designs. Shift+click switches that equipment type
between "every role" (the default) and "same role only": a source design converts only if it has
the same role as the line's design (the unit types its modules give it, `add_equipment_type`:
fighters from fighters, CAS from CAS). The convert button's tooltip shows the mode, and in "same
role only" mode lists the designs it leaves out, greyed (HoI4 text has no strikethrough).
- It counts for every line of that equipment type of your country, from the line's next rebuild
  (at once in testing).
- It is saved as the global flag `hoi4_loader_conversion_role_<TAG>_<equipment type>`; a mod can
  set it too. Without the loader the flag does nothing.
- Without the opt-in flag, Shift+click is the game's normal click and the tooltip is the game's.
- In multiplayer, Shift+click is the game's normal click (logged once).
- The tooltip text: `HOI4_LOADER_CONVERSION_MODE_ALL` / `HOI4_LOADER_CONVERSION_MODE_ROLE`, and
  `HOI4_LOADER_CONVERSION_LEFT_OUT` / `HOI4_LOADER_CONVERSION_LEFT_OUT_ENTRY` (`$VARIANT$`) for the
  left-out list, from the mod's localisation; without them the loader's English text.

**The mod HoI4Mod Loader: Equipment Conversion** (its own repository, not part of the loader; group
"Equipment Conversion" in the game rules screen):

| Rule | Options (default first) | Sets |
|---|---|---|
| Convert Earlier Generations | allowed, blocked | `hoi4_loader_conversion_generations` when allowed |
| Cost per Extra Generation | 75%, 100%, 90%, 50%, 25% | `hoi4_loader_conversion_factor_<percent>` (none for 75) |
| Conversions the Game Already Allows | game cost, factor applies | `hoi4_loader_conversion_existing_loader_cost` |
| Same Role Only (Shift+Click) | allowed, blocked | `hoi4_loader_conversion_role_mode` when allowed |
| Earlier Ship Hulls | blocked, allowed | `hoi4_loader_conversion_naval` |

- New games: `on_startup` sets the flags from the rules (scripted effect
  `hoi4_loader_conversion_apply_rules`).
- **Existing saves, or switching during a game, from the console** (hidden events; nothing
  shows, the loader picks the flags up within a second):
  - `event hoi4_loader_equipment_conversion.1`: opt in, the flags as the game rules say;
  - `event hoi4_loader_equipment_conversion.2`: opt out, every flag cleared (the loader removes
    its pairs; the per-country "same role only" choices are kept).

**Multiplayer:** the extra pairs come from the equipment files and the flags are saved game state,
so every machine with the loader computes the same. **Not tested in multiplayer yet;** every player
needs the loader.

**Saves:** the loader saves nothing; the flags are the mod's. A line's sources are rebuilt from the
stockpile, so a save loaded without the loader converts only what the game allows.

**Without the loader:** the flags do nothing; the mod's rules show and do nothing.

## Air scenarios (`air_scenarios`)

The 3D air animations on the map (patrols, dogfights, interceptions, CAS and bombing runs) are
"air scenarios". The game has a fixed list of 21. A mod can replace one, by defining an entity
of the same name, but can't add any. With the loader, a mod can add **new air scenarios** and say
**which planes** they play for: a mission (the slot), the attacking or defending planes'
equipment, and their country.

A new scenario has three parts. The first two are written exactly as for vanilla's scenarios
(`gfx/entities/planes.gfx` and `units_planes.asset`):

1. **The mesh** (`gfx/entities/*.gfx`): an animated skeleton whose joints the planes are attached
   to, with its animations by id.
   ```
   objectTypes = {
       pdxmesh = {
           name = "my_gunship_orbit_mesh"
           file = "gfx/models/units/planes/my_gunship_orbit.mesh"
           animation = { id = "default" type = "my_gunship_orbit_animation" }
       }
   }
   ```
2. **The entity** (`gfx/entities/*.asset`): uses the mesh, with one state per result. Its events
   make the planes fire, bomb or crash: `a_...` is the attacking side, `d_...` the defending side.
   The name must end with `_entity`, as all of the game's scenario entities do.
   ```
   entity = {
       name = "my_gunship_orbit_entity"
       pdxmesh = "my_gunship_orbit_mesh"
       state = { name = "default" animation = "default"
           event = { time = 0.5 id = a_fire }
           event = { time = 1.5 id = a_idle }
       }
   }
   ```
3. **The entry** (`common/hoi4_loader_air_scenarios/*.txt`, the only new part): where it plays.
   ```
   my_gunship_orbit = {
       entity = my_gunship_orbit_entity
       slot = bomber_1_CAS                   # the game's scenario: mission + number of planes
       attacker = {                          # optional
           equipment = { CAS_gunship_unique_1 }   # equipment types or archetypes
           country = { GER }                       # country tags
       }
       defender = { ... }                    # optional, same keys; only for the "_vs_" slots
       weight = 1                            # optional: chance against other matching entries
   }
   ```

**Which entry plays:**
- When the game starts a scenario, the candidates are the entries for its slot whose conditions
  all hold. Within a side, **one matching plane is enough**: the plane's equipment type or its
  archetype is listed, and its country is listed. A missing list, key or block matches anything.
- With candidates, one is picked by `weight` and plays **instead of** the game's scenario. With
  none, the game's own scenario plays.
- **The country** is the plane equipment's creator, as the game uses it for the plane model
  (`<tag>_<equipment>_entity`). For planes a country built itself, that is its own tag.
  Licensed or captured planes show their creator's tag.
- Types and archetypes as in `common/units/equipment/`: e.g. `fighter_equipment_1` or
  `small_plane_cas_airframe_1` (types), `small_plane_airframe` (an archetype, all its types).
  Names are compared without case.

**Slots.** The number in a slot's name is **how many planes fly in the scene**, not a plane
generation. A new scenario needs the slot's **joints** (the mesh's attachment points) and
**states**; the game decides per slot how many planes there are and which side they are on.

| Slot | Mission | Attacker joints (`a_...`) | Defender joints (`d_...`) | States |
|---|---|---|---|---|
| `fighter_1_patrol`, `fighter_3_patrol` | patrol (air superiority) | `fighter` / `fighter_1..3` | - | `default` |
| `scout_1_patrol`, `scout_3_patrol` | scouting | `fighter` / `fighter_1..3` | - | `default` |
| `bomber_1_CAS`, `bomber_3_CAS` | close air support | `fighter` / `fighter_1..3` | - | `default` |
| `bomber_1_bombing`, `bomber_3_bombing` | strategic bombing | `bomber` / `bomber_1..3` | - | `default` |
| `bomber_1_carpetbombing`, `bomber_3_carpetbombing` | heavier strategic bombers | `bomber` / `bomber_1..3` | - | `default` |
| `bomber_1_firebombing`, `bomber_3_firebombing` | the heaviest strategic bombers | `bomber` / `bomber_1..3` | - | `default` |
| `fighter_1_vs_bomber_1` | interception: the fighter attacks the bomber | `fighter` | `bomber` | `pass`, `success` |
| `fighter_1_vs_transport_1` | interception of a transport plane | `fighter` | `bomber` (the transport) | `pass`, `success` |
| `fighter_1_vs_fighter_1` | dogfight, 1 vs 1 | `fighter1` | `fighter2` | `pass`, `success`, `fail` |
| `fighter_3_vs_fighter_3` | dogfight, 3 vs 3 | `fighter_1..3` | `defender_1..3` | `pass`, `success`, `fail` |
| `fighter_1_kamikaze`, `fighter_3_kamikaze` | kamikaze | `fighter_1` / `fighter_1..3` | - | `default`, `success` |
| `missile_1_bombing` | V2 rocket | `rocket` | - | `idle` |
| `transport_1_supplying`, `transport_3_supplying` | air supply | `bomber` / `bomber_1..3` | - | `default` |

- **Attacker and defender** are the two sides of the animation, not of the war. In an
  interception, the fighter is the attacker and the bomber the defender, whichever country
  started the raid.
- **The result** is seen from the attacker: `pass` (nobody falls; in an interception the bomber
  gets through), `success` (the attacker wins), `fail` (the defender wins).
- In the 1 vs 1 and 3 vs 3 dogfights both sides are fighters; which of them the game puts on the
  attacking joints is not known yet.

**Files and mods:**
- The loader reads `common/hoi4_loader_air_scenarios/*.txt` from the game folder and from every
  mod enabled in the launcher (`dlc_load.json` in the game's documents folder), in that order.
  - A file of the same name in a later mod replaces the earlier one, as the game does.
  - An entry of the same name replaces an earlier entry.
  - A mod with `replace_path = "common/hoi4_loader_air_scenarios"` drops the earlier mods' files.
  - Zipped (`archive=`) mods can't be read.
- The files use the game's script syntax: `#` comments, quoted or bare words.

**Checks and messages** (`loader.log`, when the game builds its scenarios at game start):
- Each entry that is ready: `air_scenarios: my_gunship_orbit: my_gunship_orbit_entity plays in
  bomber_1_CAS (...)`.
- **Skipped**, with file, line and reason:
  - a syntax error or an unknown key;
  - a missing `entity` or `slot`;
  - an unknown slot or entity;
  - a `defender` block on a slot without defenders.
- **A missing joint** is reported by the game itself in `error.log` ("Airplane animation analyze -
  failed to find attachment node: fighter1"), as when a vanilla scenario is replaced with a wrong
  mesh. The plane then has no flight path.
- **The first time an entry plays:** `air_scenarios: my_gunship_orbit plays for the first time
  (bomber_1_CAS, 2 candidate(s))`.

**Testing:** the game's console command `airc` plays a slot on demand, with chosen equipment and
countries: `airc <slot> <result> <province> <state> <state> <attacker equipment> <defender
equipment> <attacker tag> <defender tag>`, e.g.
`airc fighter_1_vs_fighter_1 pass 11471 126 29 fighter_equipment_1 fighter_equipment_1 GER FRA`.
The test mod has four entries (`common/hoi4_loader_air_scenarios/hoi4_loader_test_air.txt`) built
from vanilla's meshes and animations.

**Multiplayer:** graphics only. Each machine picks for itself, so they may show different
scenarios; nothing in the game state changes.

**Saves:** nothing is saved.

**Without the loader:** the entries are ignored (a folder the game doesn't read); the game's own
scenarios play. The mod's extra entities are unused. Guard: `hoi4_loader_feature_air_scenarios`.
