// Engine lifecycle callbacks (engine tier, feature "lifecycle"):
//   OnLexerReady:  once, right after the lexer accepts dynamic tokens and before any script that
//                  could use them is parsed (hook on the lexer synchronisation function).
//   OnGameEntered: after a new game starts AND after a save is loaded (hook on the in-game idler
//                  "game entered" function, which is also where on_startup fires for new games).
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
std::atomic<bool> g_lexerReady{false};

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
    Log("Lifecycle: game entered (%s); running %zu callback(s)", loaded ? "loaded save" : "new game",
        g_onGameEntered.size());
    for (auto& fn : g_onGameEntered)
        if (fn && !CallSafe(fn, loaded)) fn = nullptr;
}

}  // namespace

bool LifecycleInstall() {
    if (!Engine::HaveLifecycle()) {
        Log("Lifecycle: required engine functions not found");
        return false;
    }
    return HookQueue(Engine::pLexerSync, (void*)&LexerSyncDetour, (void**)&g_origLexerSync, "lexer_sync") &&
           HookQueue(Engine::pGameEntered, (void*)&GameEnteredDetour, (void**)&g_origGameEntered, "game_entered");
}

void OnLexerReady(void (*fn)()) { g_onLexerReady.push_back(fn); }
void OnGameEntered(void (*fn)(bool loadedFromSave)) { g_onGameEntered.push_back(fn); }
bool LexerReady() { return g_lexerReady.load(); }
