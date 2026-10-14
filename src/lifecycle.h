// Engine lifecycle callbacks for engine-tier features. Register callbacks from a feature's init
// (DllMain); they run on the game's main thread. See lifecycle.cpp and the threading model in
// NOTES.md.
#pragma once

// Queues the lifecycle hooks. Requires Engine::Resolve() and MH_Initialize().
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

// --- Main thread ------------------------------------------------------------------------------
// The engine changes game state on its main thread only, but hooks into engine functions may be
// called on other threads (TBB workers, loading, audio). A hook should not read game state:
// it records what it saw (an atomic flag) and leaves the work to the main thread.
//
// Safe point: the start of each top-level frame, on the main thread, before the frame's events,
// commands and game tick. No tick and no parallel section is running there; the long tick's own
// nested frames are excluded. Runs in the main menu too.

// False if the frame hook is unavailable (signature missing). Then nothing below ever runs.
bool MainThreadQueueAvailable();
// Every frame at the safe point. Register from a feature's init. Keep it cheap: check an atomic
// flag and return. A callback that crashes is disabled.
void OnMainThreadFrame(void (*fn)());
// Runs fn(ctx) once at the next safe point, in posting order. Callable from any thread. Returns
// false (and does not queue) if the queue is unavailable.
bool RunOnMainThread(void (*fn)(void*), void* ctx);
// True on the game's main thread. False until the first frame has run.
bool IsMainThread();
