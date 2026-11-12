// Engine access for time_control (NOTES.md Part K): the game speed, the pause and the top bar's
// date text. Resolved from src/signatures.h and RTTI, each piece found where the game uses it in
// two places (the console `gamespeed` command, the speed commands, the top bar's pause button).
//
// Hooks (all four are global; DEVELOPING.md "Global hooks"):
//   CGameState::SetGameSpeed   the only writer of the speed: while a lock is set, any value
//                              becomes the locked speed (buttons, keys, console, commands);
//   CInGameIdler::TogglePause  while pausing is blocked, the player's pause (mode 1) is refused
//                              when the game runs; unpausing and automatic pauses pass;
//   CTopBar::UpdateDate        after the game set the date text: hide or show the text widget;
//   CTopBar destructor         forgets the top bar it saw.
// Each detour calls the original unchanged unless a policy is set. A fault inside our part is
// contained (SEH), logged once, and from then on every hook does exactly what the game does.
#include <string.h>

#include <atomic>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "signatures.h"

namespace Engine {

namespace {

template <typename T>
T Rd(const void* base, size_t offset) {
    T v;
    memcpy(&v, (const uint8_t*)base + offset, sizeof v);
    return v;
}

using SetSpeed_t = void(__fastcall*)(void* gameState, int speed);
using Request_t = void(__fastcall*)(void* idler, int speed, bool fromPlayer);
using Toggle_t = void(__fastcall*)(void* idler, int mode);
using IsPaused_t = bool(__fastcall*)(void* idler);
using UpdateDate_t = void(__fastcall*)(void* topBar);
using Dtor_t = void*(__fastcall*)(void* self, unsigned flags);
using WidgetFn_t = void(__fastcall*)(void* widget);

// Game speed.
void** g_ppIdler = nullptr;    // the in-game idler global the console uses
void** g_idlerVt = nullptr;    // CInGameIdler vtable (the idler is used only if it has it)
SetSpeed_t g_setSpeed = nullptr, g_setSpeedOrig = nullptr;
Request_t g_request = nullptr;
uint32_t g_speedField = 0;     // CGameState -> speed, 0-based
int g_speeds = 0;              // number of speeds (the console's bound; SetGameSpeed clamps to n-1)
// Pause.
Toggle_t g_toggle = nullptr, g_toggleOrig = nullptr;
IsPaused_t g_isPaused = nullptr;
uint32_t g_pausedField = 0;    // for the log only
uint32_t g_toggleDisp = 0;     // TogglePause's vtable byte offset
// Date display.
UpdateDate_t g_updateDate = nullptr, g_updateDateOrig = nullptr;
Dtor_t g_topBarDtor = nullptr, g_topBarDtorOrig = nullptr;
uint32_t g_dateText = 0;       // CTopBar -> DateText widget
uint32_t g_hideSlot = 0, g_showSlot = 0;  // widget vtable slots
uint8_t g_widgetFlags = 0, g_hiddenBit = 0;
// The whole time panel (time_control_window): root->FindWindow(&name, true), widget inside it.
struct NameView {
    const char* p;
    size_t n;
};
using FindWindow_t = uint8_t*(__fastcall*)(void* root, const NameView* name, bool recursive);
uint32_t g_rootMember = 0;     // CTopBar -> root window
uint32_t g_findWindowSlot = 0; // root window vtable slot
uint32_t g_windowWidget = 0;   // window -> its widget part
NameView g_panelName = {nullptr, 0};  // "time_control_window", the game's own literal
// Calendar.
int g_dateBase = 0;            // hour count of year 0
bool g_calendar = false;

// Policies and state.
std::atomic<int> g_lock{0};
std::atomic<bool> g_blockPause{false};
std::atomic<bool> g_dateHidden{false};
std::atomic<bool> g_panelHidden{false};
std::atomic<bool> g_faulted{false};
std::atomic<void*> g_topBar{nullptr};  // the live top bar (seen by its date update)
void* g_appliedBar = nullptr;          // main thread: the top bar the applied states are about
bool g_appliedHidden = false, g_appliedPanel = false;
volatile LONG g_refusedSpeedLogged = 0, g_refusedPauseLogged = 0;
volatile LONG g_threadLogged[3] = {};

const int kDaysBefore[13] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};
const int kHoursPerYear = 365 * 24;

void Fault(const char* where) {
    if (!g_faulted.exchange(true)) Log("ERROR: time_control: fault in the %s hook; the game's own behaviour from now on", where);
}

// Thread evidence for each hook, once (NOTES.md K2).
void LogThreadOnce(int i, const char* what) {
    if (InterlockedExchange(&g_threadLogged[i], 1)) return;
    Log("time_control: %s runs on thread %lu (%s)", what, GetCurrentThreadId(),
        !MainThreadQueueAvailable() ? "main thread unknown: no frame hook" : IsMainThread() ? "the main thread" : "NOT the main thread");
}

// --- Detours (no C++ objects in the SEH functions) ----------------------------------------------
void __fastcall SetSpeedDetour(void* gameState, int speed) {
    LogThreadOnce(0, "SetGameSpeed");
    int lock = g_faulted.load() ? 0 : g_lock.load();
    if (lock > 0 && speed != lock - 1) {
        if (!InterlockedExchange(&g_refusedSpeedLogged, 1))
            Log("time_control: speed %d refused, the speed is locked at %d (logged once per lock)", speed + 1, lock);
        speed = lock - 1;
    }
    g_setSpeedOrig(gameState, speed);
}

bool ReadPausedRaw(void* idler, bool* paused) {
    __try {
        *paused = g_isPaused(idler);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall ToggleDetour(void* idler, int mode) {
    LogThreadOnce(1, "TogglePause");
    if (mode == 1 && g_blockPause.load() && g_lock.load() > 0 && !g_faulted.load()) {
        bool paused = true;
        if (!ReadPausedRaw(idler, &paused)) {
            Fault("pause");
        } else if (!paused) {
            if (!InterlockedExchange(&g_refusedPauseLogged, 1))
                Log("time_control: the player's pause refused while the speed is locked (logged once per lock)");
            return;
        }
    }
    g_toggleOrig(idler, mode);
}

// The engine's idiom (kCodeWidgetHide / Show).
void SetWidgetHidden(uint8_t* w, bool hidden) {
    void** vt = *(void***)w;
    if (hidden) {
        ((WidgetFn_t)vt[g_hideSlot])(w);
        w[g_widgetFlags] |= g_hiddenBit;
    } else {
        w[g_widgetFlags] &= (uint8_t)~g_hiddenBit;
        ((WidgetFn_t)vt[g_showSlot])(w);
    }
}

void ApplyDate(void* topBar) {
    g_topBar.store(topBar);
    if (topBar != g_appliedBar) {  // a new top bar shows everything
        g_appliedBar = topBar;
        g_appliedHidden = g_appliedPanel = false;
    }
    bool want = g_dateHidden.load();
    if (want != g_appliedHidden) {
        uint8_t* w = Rd<uint8_t*>(topBar, g_dateText);
        if (w) {
            SetWidgetHidden(w, want);
            g_appliedHidden = want;
        }
    }
    want = g_panelHidden.load();
    if (want != g_appliedPanel && g_findWindowSlot) {
        uint8_t* root = Rd<uint8_t*>(topBar, g_rootMember);
        uint8_t* panel = root ? ((FindWindow_t)(*(void***)root)[g_findWindowSlot])(root, &g_panelName, true) : nullptr;
        if (panel) {
            SetWidgetHidden(panel + g_windowWidget, want);
            g_appliedPanel = want;
        }
    }
}

bool ApplyDateRaw(void* topBar) {
    __try {
        ApplyDate(topBar);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall UpdateDateDetour(void* topBar) {
    g_updateDateOrig(topBar);
    LogThreadOnce(2, "CTopBar::UpdateDate");
    if (!g_faulted.load() && !ApplyDateRaw(topBar)) Fault("date display");
}

void* __fastcall TopBarDtorDetour(void* self, unsigned flags) {
    void* expected = self;
    g_topBar.compare_exchange_strong(expected, nullptr);
    if (g_appliedBar == self) g_appliedBar = nullptr;
    return g_topBarDtorOrig(self, flags);
}

// --- Engine calls from the plugin (main thread) --------------------------------------------------
bool IdlerRaw(void** out) {
    __try {
        void* p = g_ppIdler ? *g_ppIdler : nullptr;
        *out = p && *(void***)p == g_idlerVt ? p : nullptr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* Idler() {
    void* p = nullptr;
    return g_idlerVt && IdlerRaw(&p) ? p : nullptr;
}

bool RequestRaw(void* idler, int speed0) {
    __try {
        g_request(idler, speed0, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ToggleRaw(void* idler) {
    __try {
        g_toggle(idler, 1);  // through the hook: an unpause always passes
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool UpdateDateRaw(void* topBar) {
    __try {
        g_updateDate(topBar);  // through the hook, which applies the policy
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- Resolution ------------------------------------------------------------------------------------
void ResolveSpeed(uint8_t* cp, uint8_t* cs, uint8_t* ss, uint8_t* inc) {
    void** idler = (void**)SigRipTarget(cp + kSigConsoleIdlerLoad, 3, 7);
    bool sameIdler = idler == (void**)SigRipTarget(cp + kSigConsoleIdlerLoad2, 3, 7) &&
                     idler == (void**)SigRipTarget(cs + kSigConsoleSetIdlerLoad, 3, 7) &&
                     idler == (void**)SigRipTarget(cs + kSigConsoleSetIdlerLoad2, 3, 7);
    uint32_t toggleDisp = Rd<uint32_t>(cp, kSigConsoleToggleDisp), pausedDisp = Rd<uint32_t>(cp, kSigConsoleIsPausedDisp);
    int speeds = Rd<uint8_t>(cp, kSigConsoleSpeedCountImm);
    uint32_t field = Rd<uint32_t>(ss, kSigSetGameSpeedFieldDisp);
    int maxIndex = (int)Rd<uint32_t>(ss, kSigSetGameSpeedMaxImm);
    uint8_t* request = SigFollowRel32(cs + kSigConsoleSetRequestCall);
    bool agree = sameIdler && toggleDisp == Rd<uint32_t>(cs, kSigConsoleSetToggleDisp) && toggleDisp % 8 == 0 &&
                 pausedDisp % 8 == 0 && speeds >= 2 && speeds <= 10 && maxIndex == speeds - 1 &&
                 field == Rd<uint32_t>(inc, kSigIncreaseSpeedDisp) && field < 0x2000 &&
                 (void**)SigRipTarget(inc, kSigIncreaseGameStateRel, kSigIncreaseGameStateRel + 4) == GameStateSlot() &&
                 SigFollowRel32(inc + kSigIncreaseSetSpeedJmp) == ss && SigMatchAt(request, kCodeGameSpeedRequest);
    size_t n = 0;
    void** vt = FindVtableByRtti(".?AVCInGameIdler@@", &n);
    size_t toggleSlot = toggleDisp / 8, pausedSlot = pausedDisp / 8;
    bool vtOk = vt && n > toggleSlot && n > pausedSlot && SigMatchAt((uint8_t*)vt[toggleSlot], kCodeIdlerTogglePause) &&
                SigMatchAt((uint8_t*)vt[pausedSlot], kCodeIdlerIsPaused) &&
                Rd<uint32_t>(request, kCodeGameSpeedRequestCheckDisp) ==
                    Rd<uint32_t>((uint8_t*)vt[toggleSlot], kCodeIdlerToggleCheckDisp);
    if (!agree || !vtOk) {
        Log("ERROR: time_control: the game speed call sites disagree (fields %d, idler vtable %d); speed and pause off", agree,
            vtOk);
        return;
    }
    g_ppIdler = idler;
    g_idlerVt = vt;
    g_setSpeed = (SetSpeed_t)ss;
    g_request = (Request_t)request;
    g_speedField = field;
    g_speeds = speeds;
    g_toggle = (Toggle_t)vt[toggleSlot];
    g_toggleDisp = toggleDisp;
    g_isPaused = (IsPaused_t)vt[pausedSlot];
    g_pausedField = Rd<uint32_t>((uint8_t*)vt[pausedSlot], kCodeIdlerPausedDisp);
    Log("Time control: %d speeds, speed +0x%x, idler %s, TogglePause slot %zu, IsPaused slot %zu (paused +0x%x)", g_speeds,
        g_speedField, SigDescribe(g_ppIdler).c_str(), toggleSlot, pausedSlot, g_pausedField);
}

// The time panel lookup (pause button refresh start) and the window hide in the top bar's setup.
// Needs the widget idiom (ResolveDate) first.
void ResolvePanel(uint8_t* refresh, uint8_t* hw) {
    if (!SigMatchAt(refresh, kCodeTopBarPauseRefresh)) {
        Log("ERROR: time_control: the pause button refresh has another shape; hiding the time panel off");
        return;
    }
    const char* name = (const char*)SigRipTarget(refresh + kCodeRefreshNameLea, 3, 7);
    size_t len = Rd<uint8_t>(refresh, kCodeRefreshNameLen);
    uint8_t widget = Rd<uint8_t>(hw, kSigHideWindowWidgetDisp);
    uint32_t find = Rd<uint32_t>(refresh, kCodeRefreshFindDisp);
    bool agree = len == 19 && memcmp(name, "time_control_window", 20) == 0 &&
                 widget == Rd<uint8_t>(hw, kSigHideWindowWidgetDisp2) &&
                 Rd<uint32_t>(hw, kSigHideWindowHideDisp) == g_hideSlot * 8 &&
                 Rd<uint32_t>(hw, kSigHideWindowFlagsDisp) == (uint32_t)widget + g_widgetFlags &&
                 Rd<uint8_t>(hw, kSigHideWindowBit) == g_hiddenBit &&
                 Rd<uint8_t>(hw, kSigHideWindowRootDisp) == Rd<uint8_t>(refresh, kCodeRefreshRootDisp) && find % 8 == 0 &&
                 find < 0x1000;
    if (!agree) {
        Log("ERROR: time_control: the window hide and the time panel lookup disagree; hiding the time panel off");
        return;
    }
    g_panelName = {name, len};
    g_rootMember = Rd<uint8_t>(refresh, kCodeRefreshRootDisp);
    g_findWindowSlot = find / 8;
    g_windowWidget = widget;
    Log("Time control: time panel \"%s\" = top bar root (+0x%x)->FindWindow (slot %u), widget +0x%x", name, g_rootMember,
        g_findWindowSlot, g_windowWidget);
}

void ResolveDate(uint8_t* ud, uint8_t* pc, uint8_t* hw) {
    uint8_t* tail = SigFindIn(ud, 0x180, kCodeTopBarDateTail);
    uint8_t* fmt = tail ? SigFollowRel32(tail + kCodeTopBarFormatCall) : nullptr;
    if (!fmt || !SigMatchAt(fmt, kCodeGameDateFormat)) {
        Log("ERROR: time_control: the top bar's date update has another shape; date display off");
        return;
    }
    uint32_t dateObj = Rd<uint32_t>(tail, kCodeTopBarDateObjDisp);
    uint32_t hours = dateObj + Rd<uint8_t>(fmt, kCodeGameDateHoursDisp);
    // The same hour count the loader reads elsewhere (kSigGameDate). Required: a timed lock ends by
    // that date, so without it a lock would never end (GameDateHours() would stay 0).
    if (GameDateOffset() != 0 && GameDateOffset() == hours) {
        g_dateBase = (int)Rd<uint32_t>(fmt, kCodeGameDateBaseImm);
        g_calendar = g_dateBase > 0 && g_dateBase % kHoursPerYear == 0;
    }
    if (!g_calendar)
        Log("ERROR: time_control: %s (+0x%x / +0x%x); time_control off", GameDateOffset() ? "the date format disagrees with the game date"
                                                                       : "the game date (kSigGameDate) was not found",
            hours, GameDateOffset());

    uint8_t* refresh = SigFollowRel32(pc + kSigTopBarPauseRefreshJmp);
    uint8_t* hide = SigFindIn(refresh, 0x200, kCodeWidgetHide);
    uint8_t* show = SigFindIn(refresh, 0x200, kCodeWidgetShow);
    if (!hide || !show) {
        Log("ERROR: time_control: the widget hide/show code was not found; date display off");
        return;
    }
    uint8_t flags = Rd<uint8_t>(hide, kCodeWidgetFlagsDisp), bit = Rd<uint8_t>(hide, kCodeWidgetHiddenBit);
    uint32_t hideDisp = Rd<uint32_t>(hide, kCodeWidgetHideDisp);
    uint8_t showDisp = Rd<uint8_t>(show, kCodeWidgetShowDisp);
    // The pause button toggles through the idler's TogglePause (when the speed part resolved).
    uint32_t clickToggle = Rd<uint32_t>(pc, kSigTopBarPauseToggleDisp);
    bool agree = Rd<uint8_t>(show, kCodeWidgetShowFlagsDisp) == flags &&
                 Rd<uint8_t>(show, kCodeWidgetShowMask) == (uint8_t)~bit && bit != 0 && (bit & (bit - 1)) == 0 &&
                 hideDisp % 8 == 0 && showDisp % 8 == 0 && hideDisp != showDisp && hideDisp < 0x800 &&
                 (!g_toggle || clickToggle == g_toggleDisp);
    size_t n = 0;
    void** vt = FindVtableByRtti(".?AVCTopBar@@", &n);
    bool dtorOk = vt && n > 0 && SigMatchAt((uint8_t*)vt[0], kCodeScalarDeletingDtor);
    uint32_t text = Rd<uint32_t>(ud, kSigTopBarDateTextDisp);
    if (!agree || !dtorOk || text == 0 || text > 0x10000) {
        Log("ERROR: time_control: top bar call sites disagree (widget %d, destructor %d); date display off", agree, dtorOk);
        return;
    }
    g_updateDate = (UpdateDate_t)ud;
    g_topBarDtor = (Dtor_t)vt[0];
    g_dateText = text;
    g_hideSlot = hideDisp / 8;
    g_showSlot = showDisp / 8;
    g_widgetFlags = flags;
    g_hiddenBit = bit;
    Log("Time control: top bar DateText +0x%x, widget hide slot %u / show slot %u, flags +0x%x bit 0x%x; date +0x%x, base %d",
        g_dateText, g_hideSlot, g_showSlot, g_widgetFlags, g_hiddenBit, hours, g_dateBase);
    if (hw) ResolvePanel(refresh, hw);
}

}  // namespace

void ResolveTimeControl() {
    uint8_t* cp = SigFindUnique("console_speed_pause", kSigConsoleSpeedPause);
    uint8_t* cs = SigFindUnique("console_speed_set", kSigConsoleSpeedSet);
    uint8_t* ss = SigFindUnique("set_game_speed", kSigSetGameSpeed);
    uint8_t* inc = SigFindUnique("increase_speed_tail", kSigIncreaseSpeedTail);
    uint8_t* ud = SigFindUnique("topbar_update_date", kSigTopBarUpdateDate);
    uint8_t* pc = SigFindUnique("topbar_pause_click", kSigTopBarPauseClick);
    uint8_t* hw = SigFindUnique("topbar_hide_window", kSigTopBarHideWindow);
    if (cp && cs && ss && inc && GameStateSlot()) ResolveSpeed(cp, cs, ss, inc);
    if (ud && pc) ResolveDate(ud, pc, hw);
}

bool HaveGameSpeed() { return g_ppIdler && g_idlerVt && g_setSpeed && g_request && g_speedField && g_speeds > 0; }
bool HavePause() { return HaveGameSpeed() && g_toggle && g_isPaused; }
bool HaveDateDisplay() { return g_updateDate && g_topBarDtor && g_dateText && g_hideSlot && g_showSlot && g_hiddenBit; }
bool HaveGameCalendar() { return g_calendar; }
bool HaveTimePanel() { return HaveDateDisplay() && g_rootMember && g_findWindowSlot && g_windowWidget && g_panelName.p; }

bool HookTimeControl() {
    if (!HaveGameSpeed() || !HavePause() || !HaveDateDisplay()) return false;
    // The destructor first: a top bar is never used without seeing its end. A hook that fails to
    // queue leaves its policy without effect; the caller then stays inactive (the queued ones only
    // pass through while no policy is set).
    return HookQueue((void*)g_topBarDtor, (void*)&TopBarDtorDetour, (void**)&g_topBarDtorOrig, "topbar_dtor") &&
           HookQueue((void*)g_updateDate, (void*)&UpdateDateDetour, (void**)&g_updateDateOrig, "topbar_update_date") &&
           HookQueue((void*)g_setSpeed, (void*)&SetSpeedDetour, (void**)&g_setSpeedOrig, "set_game_speed") &&
           HookQueue((void*)g_toggle, (void*)&ToggleDetour, (void**)&g_toggleOrig, "toggle_pause");
}

int GameSpeedCount() { return g_speeds; }

int GameSpeed() {
    void* gs = GameState();
    return gs && HaveGameSpeed() ? Rd<int>(gs, g_speedField) + 1 : 0;
}

bool IsPaused() {
    void* idler = HavePause() && GameState() ? Idler() : nullptr;
    bool paused = false;
    return idler && ReadPausedRaw(idler, &paused) && paused;
}

bool RequestGameSpeed(int speed) {
    void* idler = HaveGameSpeed() && GameState() ? Idler() : nullptr;
    if (!idler || speed < 1 || speed > g_speeds) return false;
    if (RequestRaw(idler, speed - 1)) return true;
    Fault("speed request");
    return false;
}

bool TogglePause() {
    void* idler = HavePause() && GameState() ? Idler() : nullptr;
    if (!idler) return false;
    if (ToggleRaw(idler)) return true;
    Fault("pause toggle");
    return false;
}

void SetSpeedLock(int speed, bool blockPause) {
    if (speed < 0 || speed > g_speeds) speed = 0;
    g_blockPause.store(speed > 0 && blockPause);
    g_lock.store(speed);
    InterlockedExchange(&g_refusedSpeedLogged, 0);
    InterlockedExchange(&g_refusedPauseLogged, 0);
}

void SetDateHidden(bool hidden) { g_dateHidden.store(hidden); }
void SetTimePanelHidden(bool hidden) { g_panelHidden.store(hidden && HaveTimePanel()); }

bool RefreshDateDisplay() {
    void* bar = g_topBar.load();
    if (!bar || !HaveDateDisplay() || g_faulted.load() || !GameState()) return false;
    if (UpdateDateRaw(bar)) return true;
    Fault("date refresh");
    return false;
}

bool TimeHooksFaulted() { return g_faulted.load(); }

bool DateToHours(int year, int month, int day, int hour, int* out) {
    if (!g_calendar || year < 0 || year > 9999 || month < 1 || month > 12 || hour < 1 || hour > 24 || day < 1 ||
        day > kDaysBefore[month] - kDaysBefore[month - 1])
        return false;
    *out = g_dateBase + year * kHoursPerYear + (kDaysBefore[month - 1] + day - 1) * 24 + (hour - 1);
    return true;
}

bool HoursToDate(int hours, int* year, int* month, int* day, int* hour) {
    if (!g_calendar || hours < g_dateBase) return false;
    int h = hours - g_dateBase;
    *year = h / kHoursPerYear;
    h %= kHoursPerYear;
    *hour = h % 24 + 1;
    int doy = h / 24, m = 1;
    while (m < 12 && doy >= kDaysBefore[m]) ++m;
    *month = m;
    *day = doy - kDaysBefore[m - 1] + 1;
    return true;
}

}  // namespace Engine
