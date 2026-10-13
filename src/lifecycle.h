// Engine lifecycle callbacks for engine-tier features. Register callbacks from a feature's init
// (DllMain); they run on the game's thread. See lifecycle.cpp.
#pragma once

// Queues the two lifecycle hooks. Requires Engine::Resolve() and MH_Initialize().
bool LifecycleInstall();

// Once, right after the lexer is synchronized: dynamic tokens may be created, scripts using
// them are not parsed yet. Register native effects here.
void OnLexerReady(void (*fn)());
// After every new game start and every save load. The game state is fully set up.
void OnGameEntered(void (*fn)(bool loadedFromSave));
// Same moment, but after every OnGameEntered callback (on_game_loaded must see refreshed state).
void OnGameEnteredLate(void (*fn)(bool loadedFromSave));

bool LexerReady();

// Inside a game-entered callback: may this machine write game state on its own right now?
// Only in single player and not after a hot join (NOTES.md Part D). Otherwise every machine of
// a multiplayer session would have to do the same thing at the same tick, which a local
// callback cannot guarantee. On false, *reason says why (for the log).
bool SessionAllowsLocalWrites(const char** reason);
