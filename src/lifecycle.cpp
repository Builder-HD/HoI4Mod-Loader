// Engine lifecycle callbacks (engine tier, feature "lifecycle"):
//   OnLexerReady:  once, right after the lexer accepts dynamic tokens and before any script that
//                  could use them is parsed (hook on the lexer synchronisation function).
//   OnGameEntered: after a new game starts AND after a save is loaded (hook on the in-game idler
//                  "game entered" function, which is also where on_startup fires for new games).
//                  OnGameEnteredLate callbacks run after all OnGameEntered ones.
#include "lifecycle.h"

#include <atomic>
#include <vector>

#include "engine.h"
#include "loader.h"

namespace {

using LexerSync_t = void(__fastcall*)();
using GameEntered_t = void(__fastcall*)(void* idler);

LexerSync_t g_origLexerSync = nullptr;
GameEntered_t g_origGameEntered = nullptr;
std::vector<void (*)()> g_onLexerReady;
std::vector<void (*)(bool)> g_onGameEntered;
std::vector<void (*)(bool)> g_onGameEnteredLate;
std::atomic<bool> g_lexerReady{false};
Engine::SessionInfo g_session;  // of the game being entered; valid during the callbacks
bool g_inGameEntered = false;

// No C++ objects in these two, so SEH can contain a crashing callback.
bool CallSafe(void (*fn)()) {
    __try {
        fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("ERROR: lexer-ready callback crashed (exception 0x%08lX); it is disabled", GetExceptionCode());
        return false;
    }
}

bool CallSafe(void (*fn)(bool), bool arg) {
    __try {
        fn(arg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("ERROR: game-entered callback crashed (exception 0x%08lX); it is disabled", GetExceptionCode());
        return false;
    }
}

void __fastcall LexerSyncDetour() {
    g_origLexerSync();
    if (g_lexerReady.exchange(true)) return;
    Log("Lifecycle: lexer synchronized; running %zu callback(s)", g_onLexerReady.size());
    for (auto& fn : g_onLexerReady)
        if (fn && !CallSafe(fn)) fn = nullptr;
}

void __fastcall GameEnteredDetour(void* idler) {
    g_origGameEntered(idler);
    bool loaded = Engine::IsLoadedFromSave(Engine::GameState());
    g_session = Engine::QuerySession(idler);
    size_t n = g_onGameEntered.size() + g_onGameEnteredLate.size();
    if (g_session.known)
        Log("Lifecycle: game entered (%s; session: %s, %s%s); running %zu callback(s)", loaded ? "loaded save" : "new game",
            g_session.serverClass, g_session.singlePlayer ? "single player" : "multiplayer",
            g_session.hotJoin ? ", HOT JOIN" : "", n);
    else
        Log("Lifecycle: game entered (%s; session type unknown); running %zu callback(s)", loaded ? "loaded save" : "new game",
            n);
    g_inGameEntered = true;
    for (auto& fn : g_onGameEntered)
        if (fn && !CallSafe(fn, loaded)) fn = nullptr;
    for (auto& fn : g_onGameEnteredLate)
        if (fn && !CallSafe(fn, loaded)) fn = nullptr;
    g_inGameEntered = false;
}

}  // namespace

bool LifecycleInstall() {
    if (!Engine::HaveLifecycle()) {
        Log("Lifecycle: required engine functions not found");
        return false;
    }
    if (!Engine::HaveSession())
        Log("Lifecycle: session functions not found; game state is never written on game entry");
    return HookQueue(Engine::pLexerSync, (void*)&LexerSyncDetour, (void**)&g_origLexerSync, "lexer_sync") &&
           HookQueue(Engine::pGameEntered, (void*)&GameEnteredDetour, (void**)&g_origGameEntered, "game_entered");
}

void OnLexerReady(void (*fn)()) { g_onLexerReady.push_back(fn); }
void OnGameEntered(void (*fn)(bool loadedFromSave)) { g_onGameEntered.push_back(fn); }
void OnGameEnteredLate(void (*fn)(bool loadedFromSave)) { g_onGameEnteredLate.push_back(fn); }
bool LexerReady() { return g_lexerReady.load(); }

bool SessionAllowsLocalWrites(const char** reason) {
    const char* why = !g_inGameEntered          ? "not inside a game-entered callback"
                      : !g_session.known        ? "session type unknown (signatures missing or unreadable)"
                      : g_session.hotJoin       ? "multiplayer hot join"
                      : !g_session.singlePlayer ? "multiplayer"
                                                : nullptr;
    if (reason) *reason = why;
    return why == nullptr;
}
