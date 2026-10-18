// game_loaded feature: fires the script on_action `on_game_loaded` when a save is loaded, after
// the loader's own refresh (plugin flags, region names). NOTES.md Part D, MODDING.md.
//
// Permanent semantics:
//   - only for a loaded save, never for a new game (on_startup covers that);
//   - once per human-controlled country, in alphabetical tag order, ROOT = that country;
//     with no human-controlled country (observer) once without scope, like on_startup.
// v1 fires in single player only (exactly one human country). In multiplayer, or after a hot
// join, it does not fire: a local callback cannot make every machine fire at the same tick.
#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "native_script.h"

namespace {

const char* const kOnAction = "on_game_loaded";
const char* const kHelp =
    "Fires each time a saved game is loaded (main menu, Continue, loading from the in-game menu; "
    "ironman included), after the loader refreshed its plugin flags and region names. Never on a "
    "new game: use on_startup. Once per human-controlled country in alphabetical tag order with "
    "ROOT = that country; with no human-controlled country (observer) once without scope.\n\n"
    "This version fires in single player only; in multiplayer it does not fire. Write migrations "
    "so they can run more than once, and run them from here AND from a fallback such as on_weekly "
    "(see MODDING.md).";

void FireGameLoaded(bool loadedFromSave) {
    if (!loadedFromSave) {
        Log("on_game_loaded: new game, not fired (on_startup covers new games)");
        return;
    }
    if (!FeatureActive("game_loaded")) return;
    const char* why = nullptr;
    if (!SessionAllowsLocalWrites(&why)) {
        Log("on_game_loaded NOT fired: %s (this version fires in single player only)", why);
        return;
    }
    if (!Engine::OnActionDefined(kOnAction)) {
        Log("on_game_loaded: no script defines it; nothing to fire");
        return;
    }
    // Human-controlled countries in tag order. Single player has at most one: the local player.
    // A multiplayer version would collect every human country and sort them by tag here.
    int tag = Engine::PlayerCountryTag();
    if (!Engine::FireOnAction(kOnAction, tag)) {
        FeatureDeactivate("game_loaded", "firing on_game_loaded failed");
        return;
    }
    if (tag > 0)
        Log("on_game_loaded fired: ROOT = %s (the player's country)", Engine::CountryTagName(tag).c_str());
    else
        Log("on_game_loaded fired once without scope (observer: no human-controlled country)");
}

}  // namespace

bool GameLoadedInit() {
    if (!Engine::HaveOnActions()) {
        Log("game_loaded: on_action engine functions not found");
        return false;
    }
    if (!Engine::HaveSession()) {
        Log("game_loaded: session functions not found (cannot tell single player from multiplayer)");
        return false;
    }
    OnGameEnteredLate(&FireGameLoaded);
    document_on_action(kOnAction, "game_loaded", kHelp);
    return true;
}
