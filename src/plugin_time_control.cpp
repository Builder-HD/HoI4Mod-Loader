// time_control (engine tier): lock the game speed and hide the top bar's date from script
// (NOTES.md Part K). Single player only.
//
//   force_game_speed = {
//       speed = 2                  # 1-5, as the speed buttons count
//       days = 2                   # optional: for this long (days and/or hours) ...
//       hours = 12
//       until = "1940.5.10.12"     # ... or until this date (year.month.day[.hour]); none: until released
//       lock_pause = no            # optional, default yes: the player cannot pause meanwhile
//   }
//   release_game_speed = yes       # the speed the player had before comes back
//   hide_date_display = yes        # `no` shows it again
//   hide_time_controls = yes       # the whole time panel (date, pause, speed buttons); `no` shows it
//   has_forced_game_speed = yes    # trigger
//
// While locked, every speed change becomes the locked speed (buttons, keys, console) and, with
// lock_pause, the player's pause is refused; the game's own automatic pauses (event popups) and
// the Esc menu still pause, and unpausing always works. A force while paused unpauses the game.
// The state is saved as global flags (hoi4_loader_time_lock, hoi4_loader_date_hidden,
// hoi4_loader_time_controls_hidden) that do nothing without the loader; after loading, a timed
// lock's end is rounded to whole days.
#include <windows.h>
#include <stdio.h>

#include <atomic>
#include <string>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "native_script.h"

namespace {

const char* const kFeature = "time_control";

const LocText kForceUntil{"HOI4_LOADER_FORCE_GAME_SPEED_UNTIL", "The game speed is locked at $SPEED|Y$ until $DATE|Y$."};
const LocText kForceDays{"HOI4_LOADER_FORCE_GAME_SPEED_DAYS", "The game speed is locked at $SPEED|Y$ for $DAYS|Y$ days."};
const LocText kForceHours{"HOI4_LOADER_FORCE_GAME_SPEED_HOURS", "The game speed is locked at $SPEED|Y$ for $HOURS|Y$ hours."};
const LocText kForceOpen{"HOI4_LOADER_FORCE_GAME_SPEED", "The game speed is locked at $SPEED|Y$ until it is released."};
const LocText kNoPause{"HOI4_LOADER_FORCE_GAME_SPEED_NO_PAUSE", "Meanwhile the game cannot be paused."};
const LocText kRelease{"HOI4_LOADER_RELEASE_GAME_SPEED", "The game speed is no longer locked."};
const LocText kHide{"HOI4_LOADER_HIDE_DATE_DISPLAY", "The date is hidden."};
const LocText kShow{"HOI4_LOADER_SHOW_DATE_DISPLAY", "The date is shown again."};
const LocText kHidePanel{"HOI4_LOADER_HIDE_TIME_CONTROLS", "The date and the time controls are hidden."};
const LocText kShowPanel{"HOI4_LOADER_SHOW_TIME_CONTROLS", "The date and the time controls are shown again."};
const LocText kIsForced{"HOI4_LOADER_HAS_FORCED_GAME_SPEED", "The game speed is locked"};
const LocText kNotForced{"HOI4_LOADER_HAS_NO_FORCED_GAME_SPEED", "The game speed is not locked"};

// Parameter indices of force_game_speed.
enum { kSpeed, kDays, kHours, kUntil, kLockPause };

// The lock. Effects and the frame callback run on the main thread (logged); triggers read only
// g_lockedSpeed.
struct Lock {
    int speed = 0;  // 1..n, 0 = none
    int prev = 0;   // the speed before the lock (restored at the end), 0 = unknown
    bool pause = false;
    int until = 0;  // game date in hours, 0 = until released
};
Lock g_lock;
std::atomic<int> g_lockedSpeed{0};
bool g_dateHidden = false;
bool g_panelHidden = false;
bool g_sessionOk = false;   // single player, decided on game entry
bool g_applyLock = false;   // frame: request the locked speed
bool g_unpause = false;     // frame: ... and unpause first
int g_restore = 0;          // frame: request this speed (the lock ended)
bool g_refreshDate = false; // frame: re-run the date update
bool g_off = false;         // the hooks faulted: everything released
int g_tokLock = 0, g_tokDate = 0, g_tokPanel = 0;
volatile LONG g_mpLogged = 0, g_threadLogged = 0;

std::string DateText(int hours) {
    int y, m, d, h;
    char buf[32];
    if (!Engine::HoursToDate(hours, &y, &m, &d, &h)) return std::to_string(hours);
    sprintf_s(buf, "%d.%d.%d.%d", y, m, d, h);
    return buf;
}

// "1940.5.10" or "1940.5.10.12" (hour 1-24 as the top bar shows it; default 1).
bool ParseDate(const char* s, int* out) {
    int y = 0, m = 0, d = 0, h = 1;
    char tail = 0;
    int n = sscanf_s(s, "%d.%d.%d.%d%c", &y, &m, &d, &h, &tail, 1);
    return (n == 3 || n == 4) && Engine::DateToHours(y, m, d, h, out);
}

bool Usable(const char* what) {
    if (g_off) return false;
    if (!InterlockedExchange(&g_threadLogged, 1))
        Log("time_control: script effects run on thread %lu (%s)", GetCurrentThreadId(),
            IsMainThread() ? "the main thread" : "NOT the main thread");
    if (g_sessionOk) return true;
    if (!InterlockedExchange(&g_mpLogged, 1))
        Log("time_control: %s does nothing: not single player, or run before the game was entered (on_startup) "
            "(logged once per game)",
            what);
    return false;
}

// --- Saved state ------------------------------------------------------------------------------------
void WriteLockFlag() {
    if (!g_lock.speed) {
        Engine::ClearGlobalFlag(g_tokLock);
        return;
    }
    int days = 0;
    if (g_lock.until) {
        int left = g_lock.until - Engine::GameDateHours();
        days = left > 0 ? (left + 23) / 24 + 1 : 1;  // the engine counts it down daily; ours ends first
        if (days > 32767) days = 32767;
    }
    short value = (short)(g_lock.speed + 10 * g_lock.prev + (g_lock.pause ? 100 : 0));
    Engine::SetGlobalFlagValue(g_tokLock, value, (short)days);
}

void WriteDateFlag() { g_dateHidden ? Engine::SetGlobalFlag(g_tokDate) : Engine::ClearGlobalFlag(g_tokDate); }

void SetLock(const Lock& l) {
    g_lock = l;
    g_lockedSpeed.store(l.speed);
    Engine::SetSpeedLock(l.speed, l.pause);
}

void Release(const char* why) {
    if (!g_lock.speed) return;
    Log("time_control: speed lock at %d ended (%s); back to speed %d", g_lock.speed, why, g_lock.prev);
    g_restore = g_lock.prev;
    SetLock(Lock{});
    g_applyLock = g_unpause = false;
    Engine::ClearGlobalFlag(g_tokLock);
}

// --- Effects and trigger ------------------------------------------------------------------------------
// The lock an effect asks for; false (and *why) if the arguments are not usable.
bool Requested(const Args& a, Lock* l, const char** why) {
    l->speed = a.i(kSpeed);
    l->pause = a.has(kLockPause) ? a.b(kLockPause) : true;
    if (l->speed < 1 || l->speed > Engine::GameSpeedCount()) return *why = "speed is out of range", false;
    int now = Engine::GameDateHours();
    if (a.has(kUntil)) {
        if (!ParseDate(a.key(kUntil), &l->until)) return *why = "until is not a date (year.month.day or year.month.day.hour)", false;
        if (l->until <= now) return *why = "until is not in the future", false;
    } else if (a.has(kDays) || a.has(kHours)) {
        long long h = (long long)a.i(kDays) * 24 + a.i(kHours);
        if (h <= 0 || h > 100000000) return *why = "days / hours must add up to a positive time", false;
        l->until = (int)(now + h);
    }
    return true;
}

void ForceExecute(const Args& a, const Scope&) {
    if (!Usable("force_game_speed")) return;
    Lock l;
    const char* why = "";
    if (!Requested(a, &l, &why)) {
        Log("time_control: force_game_speed ignored: %s", why);
        return;
    }
    l.prev = g_lock.speed ? g_lock.prev : Engine::GameSpeed();  // a new lock keeps the original speed
    SetLock(l);
    g_applyLock = true;
    g_unpause = true;
    g_restore = 0;
    WriteLockFlag();
    Log("time_control: speed locked at %d%s%s, pausing %s (was %d; now %s)", l.speed, l.until ? " until " : " until released",
        l.until ? DateText(l.until).c_str() : "", l.pause ? "blocked" : "allowed", l.prev,
        DateText(Engine::GameDateHours()).c_str());
}

void ForceTooltip(const Args& a, const Scope&, TooltipText& out) {
    std::string speed = std::to_string(a.i(kSpeed));
    if (a.has(kUntil))
        out.add(native_localize(kForceUntil, {{"SPEED", speed}, {"DATE", a.key(kUntil)}}).c_str());
    else if (a.has(kHours))
        out.add(native_localize(kForceHours, {{"SPEED", speed}, {"HOURS", std::to_string(a.i(kDays) * 24 + a.i(kHours))}}).c_str());
    else if (a.has(kDays))
        out.add(native_localize(kForceDays, {{"SPEED", speed}, {"DAYS", std::to_string(a.i(kDays))}}).c_str());
    else
        out.add(native_localize(kForceOpen, {{"SPEED", speed}}).c_str());
    if (!a.has(kLockPause) || a.b(kLockPause)) out.add(" ").add(native_localize(kNoPause).c_str());
}

void ReleaseExecute(const Args& a, const Scope&) {
    if (!a.b(0) || !Usable("release_game_speed")) return;
    if (!g_lock.speed) {
        Log("time_control: release_game_speed: the speed is not locked");
        return;
    }
    Release("release_game_speed");
}

void ReleaseTooltip(const Args& a, const Scope&, TooltipText& out) {
    if (a.b(0)) out.add(native_localize(kRelease).c_str());
}

void HideExecute(const Args& a, const Scope&) {
    if (!Usable("hide_date_display")) return;
    g_dateHidden = a.b(0);
    Engine::SetDateHidden(g_dateHidden);
    WriteDateFlag();
    g_refreshDate = true;
    Log("time_control: date display %s", g_dateHidden ? "hidden" : "shown");
}

void HideTooltip(const Args& a, const Scope&, TooltipText& out) { out.add(native_localize(a.b(0) ? kHide : kShow).c_str()); }

void HidePanelExecute(const Args& a, const Scope&) {
    if (!Usable("hide_time_controls")) return;
    g_panelHidden = a.b(0);
    Engine::SetTimePanelHidden(g_panelHidden);
    g_panelHidden ? Engine::SetGlobalFlag(g_tokPanel) : Engine::ClearGlobalFlag(g_tokPanel);
    g_refreshDate = true;
    Log("time_control: time controls %s", g_panelHidden ? "hidden" : "shown");
}

void HidePanelTooltip(const Args& a, const Scope&, TooltipText& out) {
    out.add(native_localize(a.b(0) ? kHidePanel : kShowPanel).c_str());
}

bool ForcedEval(const Args& a, const Scope&) { return (g_lockedSpeed.load() > 0) == a.b(0); }

void ForcedTooltip(const Args& a, const Scope&, bool negated, TooltipText& out) {
    out.add(native_localize(a.b(0) != negated ? kIsForced : kNotForced).c_str());
}

// --- Main thread ----------------------------------------------------------------------------------------
void Frame() {
    if (g_off || !Engine::GameState()) return;
    if (Engine::TimeHooksFaulted()) {
        g_off = true;
        SetLock(Lock{});
        Engine::SetDateHidden(false);
        Engine::SetTimePanelHidden(false);
        Engine::RefreshDateDisplay();  // does nothing once faulted; the next date update is the game's own
        FeatureDeactivate(kFeature, "a time control hook faulted");
        return;
    }
    if (g_lock.speed && g_lock.until && Engine::GameDateHours() >= g_lock.until) Release("its time is up");
    if (g_applyLock) {
        g_applyLock = false;
        if (g_unpause && Engine::IsPaused()) Engine::TogglePause();
        g_unpause = false;
        if (Engine::GameSpeed() != g_lock.speed) Engine::RequestGameSpeed(g_lock.speed);
    }
    if (g_restore) {
        if (Engine::GameSpeed() != g_restore) Engine::RequestGameSpeed(g_restore);
        g_restore = 0;
    }
    if (g_refreshDate) {
        g_refreshDate = false;
        Engine::RefreshDateDisplay();
    }
}

struct Saved {
    bool lock = false, date = false, panel = false;
    int value = 0, days = 0;
};

void ReadFlag(const Engine::FlagEntry& f, void* ctx) {
    Saved* s = (Saved*)ctx;
    if (f.token == g_tokLock) s->lock = true, s->value = f.value, s->days = f.days;
    if (f.token == g_tokDate) s->date = true;
    if (f.token == g_tokPanel) s->panel = true;
}

void OnEntered(bool loaded) {
    // Nothing carries over from the previous game.
    SetLock(Lock{});
    g_applyLock = g_unpause = false;
    g_restore = 0;
    g_dateHidden = g_panelHidden = false;
    Engine::SetDateHidden(false);
    Engine::SetTimePanelHidden(false);
    g_refreshDate = true;
    const char* why = nullptr;
    g_sessionOk = SessionAllowsLocalWrites(&why);
    InterlockedExchange(&g_mpLogged, 0);
    Saved s;
    if (!loaded || !Engine::ForEachGlobalFlag(&ReadFlag, &s) || (!s.lock && !s.date && !s.panel)) return;
    if (!g_sessionOk) {
        Log("time_control: the save's speed lock / hidden date / hidden time controls are ignored: %s",
            why ? why : "not single player");
        return;
    }
    if (s.date) {
        g_dateHidden = true;
        Engine::SetDateHidden(true);
    }
    if (s.panel) {
        g_panelHidden = true;
        Engine::SetTimePanelHidden(true);
    }
    Lock l;
    l.speed = s.value % 10;
    l.prev = s.value / 10 % 10;
    l.pause = s.value / 100 != 0;
    if (s.lock && l.speed >= 1 && l.speed <= Engine::GameSpeedCount()) {
        int now = Engine::GameDateHours();
        l.until = s.days > 0 ? now + (s.days > 1 ? s.days - 1 : 1) * 24 : 0;
        if (l.prev < 1 || l.prev > Engine::GameSpeedCount()) l.prev = 0;
        SetLock(l);
        g_applyLock = true;  // the speed, but a loaded game stays paused
    }
    Log("time_control: restored from the save: %s%s%s, date %s, time controls %s",
        g_lock.speed ? "speed locked at " : "no speed lock", g_lock.speed ? std::to_string(g_lock.speed).c_str() : "",
        g_lock.until ? (" until about " + DateText(g_lock.until)).c_str() : "", g_dateHidden ? "hidden" : "shown",
        g_panelHidden ? "hidden" : "shown");
}

void InternTokens() {
    g_tokLock = Engine::Intern("hoi4_loader_time_lock");
    g_tokDate = Engine::Intern("hoi4_loader_date_hidden");
    g_tokPanel = Engine::Intern("hoi4_loader_time_controls_hidden");
}

}  // namespace

bool TimeControlInit() {
    if (!Engine::HaveGameSpeed() || !Engine::HavePause() || !Engine::HaveDateDisplay() || !Engine::HaveTimePanel() ||
        !Engine::HaveGameCalendar() || !Engine::HaveFlags() || !Engine::HaveTokens() || !MainThreadQueueAvailable()) {
        Log("time_control: required engine functions missing (speed %d, pause %d, date display %d, time panel %d, "
            "calendar %d, flags %d, main thread %d)",
            Engine::HaveGameSpeed(), Engine::HavePause(), Engine::HaveDateDisplay(), Engine::HaveTimePanel(),
            Engine::HaveGameCalendar(), Engine::HaveFlags(), MainThreadQueueAvailable());
        return false;
    }
    if (!Engine::HookTimeControl()) return false;
    register_native_effect(
        {"force_game_speed",
         "Single player. Locks the game speed at `speed` (1-5, as the speed buttons count): every speed change, by "
         "button, key or console, gives this speed. For `days` and/or `hours`, or `until` a date (\"1940.5.10\" or "
         "\"1940.5.10.12\", hour 1-24 as the top bar shows it), or without them until release_game_speed. With "
         "`lock_pause = yes` (the default) the player cannot pause meanwhile; the game's own automatic pauses (event "
         "popups) and the Esc menu still pause, and unpausing always works. If the game is paused, it is unpaused. "
         "When the lock ends, the speed from before comes back. A new force_game_speed replaces the running lock. "
         "Saved with the game (global flag hoi4_loader_time_lock; after loading, a timed lock's end is rounded to "
         "whole days).",
         Form::Block,
         {{"speed", ParamType::Int, true},
          {"days", ParamType::Int, false},
          {"hours", ParamType::Int, false},
          {"until", ParamType::Text, false},
          {"lock_pause", ParamType::Bool, false}},
         0,
         &ForceExecute,
         &ForceTooltip,
         {kForceUntil, kForceDays, kForceHours, kForceOpen, kNoPause}},
        kFeature);
    register_native_effect({"release_game_speed",
                            "Single player. `release_game_speed = yes` ends force_game_speed's lock now: the speed from "
                            "before comes back and pausing works again.",
                            Form::Value,
                            {{"release", ParamType::Bool, true}},
                            0,
                            &ReleaseExecute,
                            &ReleaseTooltip,
                            {kRelease}},
                           kFeature);
    register_native_effect({"hide_date_display",
                            "Single player. `hide_date_display = yes` hides the date and time in the top bar; `= no` shows "
                            "it again. Saved with the game (global flag hoi4_loader_date_hidden).",
                            Form::Value,
                            {{"hidden", ParamType::Bool, true}},
                            0,
                            &HideExecute,
                            &HideTooltip,
                            {kHide, kShow}},
                           kFeature);
    register_native_effect({"hide_time_controls",
                            "Single player. `hide_time_controls = yes` hides the whole time panel of the top bar: the "
                            "date, the pause button, the speed buttons and the speed steps (a running force_game_speed "
                            "holds either way). `= no` shows it again. Independent of "
                            "hide_date_display. Saved with the game (global flag hoi4_loader_time_controls_hidden).",
                            Form::Value,
                            {{"hidden", ParamType::Bool, true}},
                            0,
                            &HidePanelExecute,
                            &HidePanelTooltip,
                            {kHidePanel, kShowPanel}},
                           kFeature);
    register_native_trigger({"has_forced_game_speed",
                             "`has_forced_game_speed = yes`: force_game_speed's lock is running (`= no`: it is not).",
                             Form::Value,
                             {{"forced", ParamType::Bool, true}},
                             0,
                             &ForcedEval,
                             &ForcedTooltip},
                            kFeature);
    OnLexerReady(&InternTokens);
    OnGameEntered(&OnEntered);
    OnMainThreadFrame(&Frame);
    return true;
}
