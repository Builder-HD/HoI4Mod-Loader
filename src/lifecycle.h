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

bool LexerReady();
