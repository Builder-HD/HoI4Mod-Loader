// Engine lifecycle callbacks (engine tier, feature "lifecycle"):
//   OnLexerReady:  once, right after the lexer accepts dynamic tokens and before any script that
//                  could use them is parsed (hook on the lexer synchronisation function).
//   OnGameEntered: after a new game starts AND after a save is loaded (hook on the in-game idler
//                  "game entered" function, which is also where on_startup fires for new games).
//                  OnGameEnteredLate callbacks run after all OnGameEntered ones.
//   Main thread:   OnMainThreadFrame / RunOnMainThread run at the start of each top-level frame
//                  (hook on CApplication::DoFrame). NOTES.md, "Threading model".
#include "lifecycle.h"

#include <atomic>
#include <mutex>
#include <vector>

#include "engine.h"
#include "loader.h"

namespace {

using LexerSync_t = void(__fastcall*)();
using GameEntered_t = void(__fastcall*)(void* idler);
using DoFrame_t = void(__fastcall*)(void* app, bool topLevel);

LexerSync_t g_origLexerSync = nullptr;
GameEntered_t g_origGameEntered = nullptr;
DoFrame_t g_origDoFrame = nullptr;
std::vector<void (*)()> g_onLexerReady;
std::vector<void (*)(bool)> g_onGameEntered;
std::vector<void (*)(bool)> g_onGameEnteredLate;
std::atomic<bool> g_lexerReady{false};
Engine::SessionInfo g_session;  // of the game being entered; valid during the callbacks
bool g_inGameEntered = false;

// Main thread: identified by the first top-level frame.
bool g_frameHooked = false;
std::atomic<DWORD> g_mainThreadId{0};
DWORD g_lexerThreadId = 0;
thread_local int g_frameDepth = 0;  // > 0 inside DoFrame; long ticks nest frames
std::vector<void (*)()> g_onFrame;  // registered from DllMain, then main thread only

struct Task {
    void (*fn)(void*);
    void* ctx;
};
std::mutex g_queueLock;
std::vector<Task> g_queue;
std::atomic<bool> g_queued{false};  // g_queue may be non-empty

// No C++ objects in these, so SEH can contain a crashing callback.
int CrashFilter(EXCEPTION_POINTERS* ep, DWORD* code, void** where) {
    *code = ep->ExceptionRecord->ExceptionCode;
    *where = ep->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

bool CallSafeInner(void (*fn)(), DWORD* code, void** where) {
    __try {
        fn();
        return true;
    } __except (CrashFilter(GetExceptionInformation(), code, where)) {
        return false;
    }
}

bool CallSafe(void (*fn)()) {
    DWORD code = 0;
    void* where = nullptr;
    if (CallSafeInner(fn, &code, &where)) return true;
    Log("ERROR: lexer-ready callback crashed (exception 0x%08lX at %s); it is disabled", code, DescribeAddress(where).c_str());
    return false;
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

bool CallSafeFrame(void (*fn)()) {
    __try {
        fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("ERROR: main-thread frame callback crashed (exception 0x%08lX); it is disabled", GetExceptionCode());
        return false;
    }
}

void CallSafeTask(void (*fn)(void*), void* ctx) {
    __try {
        fn(ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("ERROR: main-thread task crashed (exception 0x%08lX)", GetExceptionCode());
    }
}

void RunMainThreadWork() {
    for (auto& fn : g_onFrame)
        if (fn && !CallSafeFrame(fn)) fn = nullptr;
    if (!g_queued.load(std::memory_order_acquire)) return;
    std::vector<Task> tasks;
    {
        std::lock_guard<std::mutex> guard(g_queueLock);
        tasks.swap(g_queue);
        g_queued.store(false, std::memory_order_release);
    }
    for (const Task& t : tasks) CallSafeTask(t.fn, t.ctx);  // tasks posted meanwhile run next frame
}

void __fastcall DoFrameDetour(void* app, bool topLevel) {
    if (g_frameDepth == 0) {
        if (g_mainThreadId.load(std::memory_order_relaxed) == 0) {
            DWORD id = GetCurrentThreadId();
            g_mainThreadId.store(id, std::memory_order_release);
            Log("Lifecycle: main thread %lu (first frame); lexer callbacks ran on %s", id,
                g_lexerThreadId == 0 ? "no thread yet" : g_lexerThreadId == id ? "the main thread" : "ANOTHER thread");
        }
        RunMainThreadWork();
    }
    ++g_frameDepth;
    g_origDoFrame(app, topLevel);
    --g_frameDepth;
}

void __fastcall LexerSyncDetour() {
    g_origLexerSync();
    if (g_lexerReady.exchange(true)) return;
    g_lexerThreadId = GetCurrentThreadId();
    Log("Lifecycle: lexer synchronized (thread %lu); running %zu callback(s)", g_lexerThreadId, g_onLexerReady.size());
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
    if (g_frameHooked && !IsMainThread())
        Log("WARNING: game entered on thread %lu, not the main thread %lu", GetCurrentThreadId(),
            g_mainThreadId.load());
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
    if (!HookQueue(Engine::pLexerSync, (void*)&LexerSyncDetour, (void**)&g_origLexerSync, "lexer_sync") ||
        !HookQueue(Engine::pGameEntered, (void*)&GameEnteredDetour, (void**)&g_origGameEntered, "game_entered"))
        return false;
    // Optional: without it the lifecycle callbacks work, but nothing can be deferred to the main
    // thread; features that need that stay off.
    g_frameHooked = Engine::pAppDoFrame &&
                    HookQueue(Engine::pAppDoFrame, (void*)&DoFrameDetour, (void**)&g_origDoFrame, "app_do_frame");
    if (!g_frameHooked) Log("Lifecycle: frame hook unavailable; main-thread queue is off");
    return true;
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

bool MainThreadQueueAvailable() { return g_frameHooked; }

void OnMainThreadFrame(void (*fn)()) { g_onFrame.push_back(fn); }

bool RunOnMainThread(void (*fn)(void*), void* ctx) {
    if (!g_frameHooked || !fn) return false;
    std::lock_guard<std::mutex> guard(g_queueLock);
    g_queue.push_back({fn, ctx});
    g_queued.store(true, std::memory_order_release);
    return true;
}

bool IsMainThread() {
    DWORD id = g_mainThreadId.load(std::memory_order_acquire);
    return id != 0 && id == GetCurrentThreadId();
}
