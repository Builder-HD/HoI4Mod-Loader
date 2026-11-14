// Engine access for the railways feature (NOTES.md Part H): railway levels between provinces and
// the supply system refresh. Every railway is stored on both of its ends, in the
// CProvinceRailwayInfo of each province: a level per neighbour index (int[]) plus a sorted list of
// (neighbour, level) pairs holding only levels > 0. The engine itself only ever raises levels
// (SetRailway keeps the maximum), so lowering one writes both structures directly, then marks the
// provinces dirty in the supply system exactly as build_railway does.
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "signatures.h"

namespace Engine {

namespace {

// False once the plugin switches railways off (its self-check failed): no railway is changed any
// more, dismantle entries pause (no progress, never finished), and the badges are vanilla again.
volatile bool g_enabled = true;

template <typename T>
T Rd(const void* base, size_t offset) {
    T v;
    memcpy(&v, (const uint8_t*)base + offset, sizeof v);
    return v;
}
template <typename T>
void Wr(void* base, size_t offset, T v) {
    memcpy((uint8_t*)base + offset, &v, sizeof v);
}

using NeighborIndex_t = int(__fastcall*)(void* province, int neighbour);
using GfxFn_t = void(__fastcall*)(void* gfx);
using UiView_t = void*(__fastcall*)(void* ui);
using SetLevel_t = void(__fastcall*)(void* building, int level, bool notify);
using SupplyNotify_t = void(__fastcall*)(void* supply, int province);

void** g_ppGameState = nullptr;   // the gamestate global, as both railway call sites read it
uint32_t g_supplyOffset = 0;      // gamestate -> CSupplySystem
uint32_t g_managerOffset = 0;     // gamestate -> CRailwayManager
uint32_t g_infoArray = 0, g_infoCount = 0;  // manager -> CProvinceRailwayInfo* [province id]
uint32_t g_infoProvince = 0;      // info -> map province (neighbour list)
uint32_t g_levels = 0;            // info -> int level per neighbour index
uint32_t g_pairs = 0, g_pairCount = 0;  // info -> sorted { int neighbour, int level }
uint32_t g_mirror = 0;            // info -> rail_way building (mirror of the level, or null)
uint32_t g_notifySlot = 0;        // CSupplySystem vtable slot: mark province dirty
NeighborIndex_t g_neighborIndex = nullptr;
SetLevel_t g_setLevel = nullptr;
void** g_vtManager = nullptr;
void** g_vtSupply = nullptr;
void** g_vtInfo = nullptr;
// The railway graphics (pieces, moving trains, bridges): cleared and rebuilt after a removal.
void** g_ppUi = nullptr;
uint32_t g_uiViewSlot = 0, g_uiViewGfxHolder = 0, g_gfxOffset = 0;
GfxFn_t g_gfxClear = nullptr, g_gfxRebuild = nullptr;

// --- Who may dismantle where --------------------------------------------------------------------
// Every province on the segment must be controlled by the country itself, or by one the rules
// allow: same faction, its subject, its overlord (the plugin reads them from global flags a mod
// sets from its game rules). The engine's own tests, as is_in_faction_with / is_subject_of use them.
using CountryOf_t = void*(__fastcall*)(const int* tag);
using TagIndexOf_t = int(__fastcall*)(const int* tag);
using IsSubject_t = bool(__fastcall*)(void* relation);
using ProvinceOf_t = void*(__fastcall*)(void* provinces, int id);
CountryOf_t g_relCountry = nullptr;
TagIndexOf_t g_relTagIndex = nullptr;
IsSubject_t g_relIsSubject = nullptr;
uint32_t g_relDiplomacy = 0, g_relRelations = 0, g_relFaction = 0, g_provinces = 0, g_provController = 0;
RailwayRules_t g_rulesFn = nullptr;

bool HaveRelations() {
    return g_relCountry && g_relTagIndex && g_relIsSubject && g_relDiplomacy && g_relRelations && g_relFaction &&
           g_provinces && g_provController;
}

void* Diplomacy(int tag) {
    void* country = tag > 0 ? g_relCountry(&tag) : nullptr;
    return country ? Rd<void*>(country, g_relDiplomacy) : nullptr;
}

bool SubjectOf(int subject, int overlord) {
    void* diplomacy = Diplomacy(subject);
    void* const* relations = diplomacy ? Rd<void* const*>(diplomacy, g_relRelations) : nullptr;
    int idx = relations ? g_relTagIndex(&overlord) : -1;
    void* relation = idx >= 0 ? relations[idx] : nullptr;
    return relation && g_relIsSubject(relation);
}

bool SameFaction(int a, int b) {
    void* da = Diplomacy(a);
    void* db = Diplomacy(b);
    void* fa = da ? Rd<void*>(da, g_relFaction) : nullptr;
    return fa && db && fa == Rd<void*>(db, g_relFaction);
}

int ProvinceController(int province) {
    uint8_t* gs = (uint8_t*)GameState();
    if (!gs || province <= 0) return 0;
    void* provinces = gs + g_provinces;  // an object inside the gamestate; its vtable[0] looks up by id
    void* p = ((ProvinceOf_t)(*(void***)provinces)[0])(provinces, province);
    return p ? Rd<int>(p, g_provController) : 0;
}

bool DismantleAllowed(int tag, const int* path, size_t count) {
    if (tag <= 0 || !HaveRelations()) return false;
    int rules = g_rulesFn ? g_rulesFn() : 0;
    for (size_t i = 0; i < count; ++i) {
        int c = ProvinceController(path[i]);
        if (c <= 0) return false;
        if (IsSameCountry(c, tag)) continue;
        if ((rules & kRailRuleAllies) && SameFaction(c, tag)) continue;
        if ((rules & kRailRuleSubjects) && SubjectOf(c, tag)) continue;
        if ((rules & kRailRuleOverlord) && SubjectOf(tag, c)) continue;
        return false;
    }
    return count > 0;
}

void ResolveRelations() {
    uint8_t* p;
    if ((p = SigFindUnique("rail_subject_of", kSigRailSubjectOf)) != nullptr) {
        g_relCountry = (CountryOf_t)SigFollowRel32(p + kSigRailSubjectTagToCountry);
        g_relDiplomacy = Rd<uint32_t>(p, kSigRailSubjectDiplomacyDisp);
        g_relTagIndex = (TagIndexOf_t)SigFollowRel32(p + kSigRailSubjectTagIndex);
        g_relRelations = Rd<uint8_t>(p, kSigRailSubjectRelationsDisp);
        g_relIsSubject = (IsSubject_t)SigFollowRel32(p + kSigRailSubjectIsSubject);
    }
    if ((p = SigFindUnique("rail_faction", kSigRailFaction)) != nullptr) {
        if (Rd<uint32_t>(p, kSigRailFactionDiplomacyDisp) == g_relDiplomacy)
            g_relFaction = Rd<uint32_t>(p, kSigRailFactionDisp);
        else
            Log("ERROR: railways: the faction and subject triggers read different diplomacy fields");
    }
    if ((p = SigFindUnique("rail_controller", kSigRailController)) != nullptr) {
        g_provinces = Rd<uint8_t>(p, kSigRailProvincesDisp);
        g_provController = Rd<uint32_t>(p, kSigRailControllerDisp);
    }
    if (HaveRelations())
        Log("Railway rules: diplomacy +0x%x (relations +0x%x, faction +0x%x), provinces +0x%x, controller +0x%x",
            g_relDiplomacy, g_relRelations, g_relFaction, g_provinces, g_provController);
    else
        Log("railway_queue: relation tests not found; dismantling uses the engine's build permission");
}

// The supply system's after-load refresh of the railway graphics, as the engine does it. Main
// thread. Without it, something on the map (a train on the removed railway) can still use the
// removed piece and the next frame crashes.
void RefreshRailwayGraphicsNow() {
    if (!g_ppUi || !g_gfxClear || !g_gfxRebuild) return;
    void* ui = *g_ppUi;
    if (!ui) return;
    void* view = ((UiView_t)(*(void***)ui)[g_uiViewSlot])(ui);
    void* holder = view ? Rd<void*>(view, g_uiViewGfxHolder) : nullptr;
    void* gfx = holder ? Rd<void*>(holder, g_gfxOffset) : nullptr;
    if (!gfx) return;
    g_gfxClear(gfx);
    g_gfxRebuild(gfx);
}

// Every caller runs on the main thread (effects, and the construction line's daily step from the
// production manager's serial daily update, NOTES.md H4), which the first daily steps log as
// evidence. Should a call ever come from another thread, the rebuild waits for the next safe point
// instead of touching the graphics while a frame may be drawing them.
volatile LONG g_gfxDeferred = 0;
void DeferredGraphics(void*) {
    InterlockedExchange(&g_gfxDeferred, 0);
    RefreshRailwayGraphicsNow();
}
void RefreshRailwayGraphics() {
    // Without the frame hook there is no main-thread test and no queue: the callers' own thread, as
    // before (the evidence says it is the main thread).
    if (!MainThreadQueueAvailable() || IsMainThread()) return RefreshRailwayGraphicsNow();
    static volatile LONG logged = 0;
    if (!InterlockedExchange(&logged, 1))
        Log("railways: railway graphics refresh requested off the main thread (thread %lu); deferred to the next safe point",
            GetCurrentThreadId());
    if (!InterlockedExchange(&g_gfxDeferred, 1) && !RunOnMainThread(&DeferredGraphics, nullptr))
        InterlockedExchange(&g_gfxDeferred, 0);
}

struct Pair {
    int neighbour;
    int level;
};

void* Manager() {
    void* gs = GameState();
    if (!gs || !g_ppGameState || *g_ppGameState != gs) return nullptr;
    void* m = Rd<void*>(gs, g_managerOffset);
    return m && *(void***)m == g_vtManager ? m : nullptr;
}

int InfoCount(void* manager) {
    int n = Rd<int>(manager, g_infoCount);
    return n > 0 && n < 1000000 ? n : 0;
}

// The province's railway info, or null if it has none (never created here: a province without
// one has no railway).
void* Info(void* manager, int province) {
    if (province <= 0 || province >= InfoCount(manager)) return nullptr;
    void* const* arr = Rd<void* const*>(manager, g_infoArray);
    void* info = arr ? arr[province] : nullptr;
    return info && *(void***)info == g_vtInfo ? info : nullptr;
}

Pair* Pairs(void* info, int* count) {
    *count = Rd<int>(info, g_pairCount);
    Pair* p = Rd<Pair*>(info, g_pairs);
    if (!p || *count < 0 || *count > 256) *count = 0;
    return p;
}

int LevelTowards(void* info, int neighbour) {
    int n;
    Pair* p = Pairs(info, &n);
    for (int i = 0; i < n; ++i)
        if (p[i].neighbour == neighbour) return p[i].level;
    return 0;
}

// Lowers one end of a link to `level` (>= 0). False if the link is not stored on this end.
bool LowerEnd(void* info, int neighbour, int level) {
    void* province = Rd<void*>(info, g_infoProvince);
    int idx = province ? g_neighborIndex(province, neighbour) : -1;
    int* levels = Rd<int*>(info, g_levels);
    if (idx < 0 || !levels) return false;
    levels[idx] = level;
    int n;
    Pair* p = Pairs(info, &n);
    int maxLeft = 0;
    for (int i = 0; i < n; ++i) {
        if (p[i].neighbour != neighbour) continue;
        if (level > 0) {
            p[i].level = level;
        } else {  // only levels > 0 are stored: erase, keeping the order
            memmove(&p[i], &p[i + 1], (size_t)(n - i - 1) * sizeof(Pair));
            Wr<int>(info, g_pairCount, --n);
        }
        break;
    }
    for (int i = 0; i < n; ++i)
        if (p[i].level > maxLeft) maxLeft = p[i].level;
    // The rail_way building mirrors the province's railway level (has_railway_level reads it).
    if (void* building = Rd<void*>(info, g_mirror)) g_setLevel(building, maxLeft, true);
    return true;
}

// --- Railway construction lines (queued dismantling, NOTES.md H3) ------------------------------
// A dismantle line is an ordinary CRailwayProductionLine whose target level is below its base
// level. The engine never makes one (the build command and SetTarget keep target >= base) and the
// line's save keeps both as they are, so that alone marks it. The engine would see no link below the
// target and report it finished at once; the hooks below run such lines instead:
//   daily (slot 24)     work the first link above the target, lower a level when its cost is paid
//   progress            the totals the construction queue shows (and the levels still to do)
//   finished (slot 25)  cancelled, or no link above the target: the manager then removes it
// Plain railway lines go straight to the engine's functions.

struct PathArray {  // CPdxArray<int> as CreateRailwayLine / Init read it: data, count at +0xc
    int* data;
    int capacity;
    int count;
};

using TagToCountry_t = void*(__fastcall*)(const int* tag);
using CreateLine_t = void(__fastcall*)(void* construction, const PathArray* path, int from, int to, char insert);
using Cost_t = int64_t*(__fastcall*)(int64_t* out, int level, void* holder);
using PathUsable_t = bool(__fastcall*)(const int* tag, int first, int last);
using ProvinceUsable_t = bool(__fastcall*)(const int* tag, int province);
using LineFn_t = void(__fastcall*)(void* line);
using LineBool_t = bool(__fastcall*)(void* line);
using LineRefresh_t = void(__fastcall*)(void* line, int levelsLeft);
using LinkSpeed_t = int64_t*(__fastcall*)(void* line, int64_t* out, int province, int next, char flag);
using SetSpeed_t = void(__fastcall*)(void* line, int64_t speed);

TagToCountry_t g_tagToCountry = nullptr;
CreateLine_t g_createLine = nullptr;
Cost_t g_cost = nullptr;
PathUsable_t g_pathUsable = nullptr;
ProvinceUsable_t g_provinceUsable = nullptr;
LinkSpeed_t g_linkSpeed = nullptr;
SetSpeed_t g_setSpeed = nullptr;
LineFn_t g_dailyOrig = nullptr, g_progressOrig = nullptr;
LineBool_t g_finishedOrig = nullptr;
uint8_t* g_dailyFn = nullptr;     // hook targets
uint8_t* g_progressFn = nullptr;
uint8_t* g_finishedFn = nullptr;
void** g_vtLine = nullptr;        // .?AVCRailwayProductionLine@@
uint32_t g_construction = 0;      // country -> construction (production lines)
uint32_t g_lineBase = 0, g_lineTarget = 0, g_lineCurrent = 0, g_linePath = 0, g_lineCount = 0, g_lineCountry = 0;
uint32_t g_lineTotal = 0, g_lineDone = 0, g_lineRemaining = 0, g_lineOutput = 0, g_lineCancelled = 0;
uint32_t g_holder = 0, g_holderTag = 0;  // country -> tag holder (cost modifiers), holder -> tag
uint32_t g_slotCancel = 0, g_slotAfterDaily = 0, g_slotRefresh = 0, g_slotOutput = 0;
// The construction view's "changed" event, as the build command sends it after adding a line.
using UiPost_t = void(__fastcall*)(void* target, const int* event);
UiPost_t g_uiPost = nullptr;
uint32_t g_uiRefreshSlot = 0;

void RefreshConstructionView() {
    void* ui = g_ppUi ? *g_ppUi : nullptr;
    if (!ui || !g_uiPost) return;
    void* target = ((UiView_t)(*(void***)ui)[g_uiRefreshSlot])(ui);
    int event = kRailUiConstructionChanged;
    if (target) g_uiPost(target, &event);
}

// Dismantling a level costs this percentage of building it: a game rule's global flag, read by the
// plugin, refreshed at each entry point below (daily step, progress, queueing) so StepCost stays cheap.
RailwayCost_t g_costFn = nullptr;
int g_costPercent = kRailwayDismantleDefaultPercent;
void RefreshCostPercent() {
    int p = g_costFn ? g_costFn() : -1;
    g_costPercent = p >= 0 ? p : kRailwayDismantleDefaultPercent;
}
const char kInsertAtEnd = 1;  // CreateRailwayLine reorders the queue only for 0, 2 and 3

int LineBase(void* line) { return Rd<int>(line, g_lineBase); }
int LineTarget(void* line) { return Rd<int>(line, g_lineTarget); }
bool IsDismantleLine(void* line) {
    return line && g_vtLine && *(void***)line == g_vtLine && LineTarget(line) < LineBase(line);
}
const int* LinePath(void* line, int* count) {
    *count = Rd<int>(line, g_lineCount);
    const int* p = Rd<const int*>(line, g_linePath);
    if (!p || *count < 2 || *count > 4096) *count = 0;
    return p;
}
void* LineHolder(void* line) {
    void* country = Rd<void*>(line, g_lineCountry);
    return country ? Rd<void*>(country, g_holder) : nullptr;
}
void* Slot(void* line, uint32_t slot) { return (*(void***)line)[slot]; }

// Cost of dismantling `level` (the step from level to level - 1), fixed point.
int64_t StepCost(void* holder, int level) {
    int64_t cost = 0;
    g_cost(&cost, level, holder);
    return cost > 0 ? cost * g_costPercent / 100 : 0;
}

// Index of the first link above the target (in path order) and its level, or -1.
int FirstLinkAbove(const int* path, int count, int target, int* level) {
    for (int i = 0; i + 1 < count; ++i) {
        *level = RailwayLevel(path[i], path[i + 1]);
        if (*level > target) return i;
    }
    return -1;
}

// What is still to do at the current railway levels: every level above the target on every link.
int64_t CostLeft(void* line, void* holder, int* levelsLeft) {
    int count;
    const int* path = LinePath(line, &count);
    int target = LineTarget(line);
    int64_t sum = 0;
    *levelsLeft = 0;
    for (int i = 0; i + 1 < count; ++i)
        for (int l = RailwayLevel(path[i], path[i + 1]); l > target; --l) {
            sum += StepCost(holder, l);
            ++*levelsLeft;
        }
    return sum;
}

// `remaining` (saved with the line) is the only progress state: it starts at the full cost and the
// daily step lowers a level whenever it has dropped to what the levels after the next one cost.
// The total and done fields are what the construction queue shows; the total is not saved, so
// after loading it restarts from what is left. Last, the line's speed (its daily progress, which
// the engine's end-of-day bookkeeping and the next daily step use): the engine's own value for the
// link being dismantled, as it sets it for the link being built.
void WriteProgress(void* line, int64_t costLeft, int levelsLeft) {
    int64_t remaining = Rd<int64_t>(line, g_lineRemaining);
    if (remaining <= 0 || remaining > costLeft) remaining = costLeft;  // new line, or levels changed
    int64_t total = Rd<int64_t>(line, g_lineTotal);
    if (total < remaining) total = remaining;
    Wr<int64_t>(line, g_lineRemaining, remaining);
    Wr<int64_t>(line, g_lineTotal, total);
    Wr<int64_t>(line, g_lineDone, total - remaining);
    ((LineRefresh_t)Slot(line, g_slotRefresh))(line, levelsLeft);
    int count, level;
    const int* path = LinePath(line, &count);
    int link = FirstLinkAbove(path, count, LineTarget(line), &level);
    int64_t speed = 0;
    if (link >= 0) g_linkSpeed(line, &speed, path[link], path[link + 1], 0);
    g_setSpeed(line, speed > 0 ? speed : 0);
}

void DismantleProgress(void* line) {
    void* holder = LineHolder(line);
    if (!holder) return;
    RefreshCostPercent();
    int levelsLeft;
    int64_t costLeft = CostLeft(line, holder, &levelsLeft);
    WriteProgress(line, costLeft, levelsLeft);
}

// Diagnostics: the first daily steps of each session and every level lowered go to loader.log.
int g_dailyLogged = 0;

void DismantleDaily(void* line) {
    RefreshCostPercent();
    Wr<int>(line, g_lineCurrent, -1);
    int count;
    const int* path = LinePath(line, &count);
    void* holder = LineHolder(line);
    const int* tag = holder ? (const int*)((uint8_t*)holder + g_holderTag) : nullptr;
    int target = LineTarget(line), level = 0;
    int link = FirstLinkAbove(path, count, target, &level);
    // Cancelled, as the engine cancels a line whose path is lost, once the country may no longer
    // dismantle there (game rules), or without the relation tests where it may no longer build.
    bool usable = count && tag;
    if (usable && HaveRelations()) {
        usable = DismantleAllowed(*tag, path, (size_t)count);
    } else if (usable) {
        usable = g_pathUsable(tag, path[0], path[count - 1]);
        for (int i = 0; usable && link >= 0 && i <= link; ++i) usable = g_provinceUsable(tag, path[i + 1]);
    }
    if (!usable) {
        ((LineFn_t)Slot(line, g_slotCancel))(line);
        return;
    }
    if (link >= 0) {
        Wr<int>(line, g_lineCurrent, link);
        int levelsLeft;
        int64_t costLeft = CostLeft(line, holder, &levelsLeft);
        int64_t remaining = Rd<int64_t>(line, g_lineRemaining);
        if (remaining <= 0 || remaining > costLeft) remaining = costLeft;
        int64_t output = Rd<int64_t>(line, g_lineOutput);
        remaining -= output;
        if (remaining < 0) remaining = 0;
        if (g_dailyLogged < 10) {
            // Evidence for the thread rule (NOTES.md H4): the daily step lowers railways and rebuilds
            // their graphics, which must happen on the main thread.
            if (g_dailyLogged == 0)
                Log("railway_queue: daily step on thread %lu (%s)", GetCurrentThreadId(),
                    !MainThreadQueueAvailable() ? "main thread unknown: no frame hook"
                    : IsMainThread()           ? "the main thread"
                                               : "NOT the main thread");
            ++g_dailyLogged;
            Log("railway_queue: daily step: link %d-%d at level %d (target %d), progress today %.3f, remaining %.3f of "
                "%.3f, next level costs %.3f",
                path[link], path[link + 1], level, target, output / 1e5, remaining / 1e5, costLeft / 1e5,
                StepCost(holder, level) / 1e5);
        }
        // Lower every level whose cost the work done so far covers.
        while (link >= 0 && remaining <= costLeft - StepCost(holder, level)) {
            costLeft -= StepCost(holder, level);
            DowngradeRailway({path[link], path[link + 1]}, 1);
            Log("railway_queue: link %d-%d dismantled to level %d", path[link], path[link + 1], level - 1);
            link = FirstLinkAbove(path, count, target, &level);
        }
        Wr<int64_t>(line, g_lineRemaining, remaining);
        Wr<int>(line, g_lineCurrent, link);
    }
    // The engine's end-of-day refresh, as after its own daily step; it runs the progress hook.
    ((LineFn_t)Slot(line, g_slotAfterDaily))(line);
}

bool DismantleFinished(void* line) {
    if (Rd<uint8_t>(line, g_lineCancelled)) return true;
    int count, level;
    const int* path = LinePath(line, &count);
    return FirstLinkAbove(path, count, LineTarget(line), &level) < 0;
}

// The detours. A fault in the dismantle logic is logged once and leaves the line as it is (no C++
// objects in these functions, so SEH applies).
volatile LONG g_faultLogged = 0;
void LogFaultOnce(const char* where) {
    if (!InterlockedExchange(&g_faultLogged, 1)) Log("ERROR: railways: fault in the dismantle %s; the line is left as it is", where);
}
// While railways is off, a dismantle entry pauses: no daily work, its progress stays as it is, and
// it never reports finished, so the engine neither runs it as a railway line nor removes it. It
// stays in the queue (and in saves) for a later session where the feature works.
volatile LONG g_pausedLogged = 0;
bool Paused() {
    if (g_enabled) return false;
    if (!InterlockedExchange(&g_pausedLogged, 1))
        Log("railway_queue: railways is off; dismantle entries in the queue are paused (no progress, not finished)");
    return true;
}
void __fastcall DailyDetour(void* line) {
    if (!IsDismantleLine(line)) return g_dailyOrig(line);
    if (Paused()) return;
    __try {
        DismantleDaily(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogFaultOnce("daily step");
    }
}
void __fastcall ProgressDetour(void* line) {
    if (!IsDismantleLine(line)) return g_progressOrig(line);
    if (Paused()) return;
    __try {
        DismantleProgress(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogFaultOnce("progress update");
    }
}
bool __fastcall FinishedDetour(void* line) {
    if (!IsDismantleLine(line)) return g_finishedOrig(line);
    if (Paused()) return false;
    __try {
        return DismantleFinished(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogFaultOnce("finished check");
        return false;
    }
}

void ResolveRailwayLines() {
    uint8_t* p;
    if ((p = SigFindUnique("rail_ui_refresh", kSigRailUiRefresh)) != nullptr &&
        (void**)SigRipTarget(p + kSigRailUiRefreshUiRel - 3, 3, 7) == g_ppUi) {  // the same ui global
        g_uiRefreshSlot = Rd<uint32_t>(p, kSigRailUiRefreshSlotDisp) / 8;
        g_uiPost = (UiPost_t)SigFollowRel32(p + kSigRailUiRefreshPostCall);
    }
    if ((p = SigFindUnique("rail_create_line", kSigRailCreateLine)) != nullptr) {
        g_tagToCountry = (TagToCountry_t)SigFollowRel32(p + kSigRailTagToCountryCall);
        g_construction = Rd<uint32_t>(p, kSigRailConstructionDisp);
        g_createLine = (CreateLine_t)SigFollowRel32(p + kSigRailCreateLineCall);
    }
    if ((p = SigFindUnique("rail_line_init", kSigRailLineInit)) != nullptr) {
        g_lineBase = Rd<uint8_t>(p, kSigRailLineBaseDisp);
        g_lineTarget = Rd<uint8_t>(p, kSigRailLineTargetDisp);
    }
    uint8_t* daily = SigFindUnique("rail_line_daily", kSigRailLineDaily);
    uint8_t* head = SigFindUnique("rail_line_daily_head", kSigRailLineDailyHead);
    uint8_t* control = SigFindUnique("rail_line_control", kSigRailLineControl);
    uint8_t* afterDaily = SigFindUnique("rail_line_after_daily", kSigRailLineAfterDaily);
    uint8_t* output = SigFindUnique("rail_line_daily_output", kSigRailLineDailyOutput);
    uint8_t* progress = SigFindUnique("rail_line_progress", kSigRailLineProgress);
    uint8_t* tail = SigFindUnique("rail_line_progress_tail", kSigRailLineProgressTail);
    uint8_t* speed = SigFindUnique("rail_line_speed", kSigRailLineSpeed);
    uint8_t* cost = SigFindUnique("rail_line_cost", kSigRailLineCost);
    if (!daily || !head || !control || !afterDaily || !output || !progress || !tail || !speed || !cost ||
        !g_managerOffset)
        return;
    // The daily parts must lie inside the daily function, the progress parts inside the progress one.
    auto inside = [](uint8_t* fn, uint8_t* part, size_t size) { return part > fn && part < fn + size; };
    if (!inside(daily, head, 0x400) || !inside(daily, control, 0x400) || !inside(daily, afterDaily, 0x1000) ||
        !inside(daily, output, 0x1000) || !inside(progress, tail, 0x600) || !inside(progress, speed, 0x600)) {
        Log("ERROR: railways: the construction line signatures are not in the expected functions");
        return;
    }
    g_lineCurrent = Rd<uint32_t>(daily, kSigRailLineCurrentDisp);
    g_lineCount = Rd<uint32_t>(head, kSigRailHeadCountDisp);
    g_lineCountry = Rd<uint8_t>(head, kSigRailHeadCountryDisp);
    g_holder = Rd<uint32_t>(head, kSigRailHeadHolderDisp);
    g_holderTag = Rd<uint8_t>(head, kSigRailHeadTagDisp);
    g_linePath = Rd<uint32_t>(head, kSigRailHeadPathDisp);
    g_pathUsable = (PathUsable_t)SigFollowRel32(head + kSigRailHeadPathUsableCall);
    g_slotCancel = Rd<uint32_t>(head, kSigRailHeadCancelDisp) / 8;
    g_provinceUsable = (ProvinceUsable_t)SigFollowRel32(control + kSigRailControlCall);
    g_slotAfterDaily = Rd<uint8_t>(afterDaily, kSigRailAfterDailySlotDisp) / 8;
    g_slotOutput = Rd<uint8_t>(output, kSigRailDailyOutputSlotDisp) / 8;  // the slot the daily step reads the output from
    g_lineTotal = Rd<uint8_t>(tail, kSigRailTotalDisp);
    g_lineRemaining = Rd<uint32_t>(tail, kSigRailRemainingDisp);
    g_lineDone = Rd<uint8_t>(tail, kSigRailDoneDisp);
    g_slotRefresh = Rd<uint32_t>(tail, kSigRailRefreshSlotDisp) / 8;
    g_setSpeed = (SetSpeed_t)SigFollowRel32(tail + kSigRailSetSpeedJmp);
    g_linkSpeed = (LinkSpeed_t)SigFollowRel32(speed + kSigRailLinkSpeedCall);
    g_cost = (Cost_t)SigFollowRel32(cost + kSigRailCostCall);
    // Cross-checks between the call sites.
    bool agree = Rd<uint32_t>(head, kSigRailHeadManagerDisp) == g_managerOffset &&
                 Rd<uint8_t>(cost, kSigRailCostCountryDisp) == g_lineCountry &&
                 Rd<uint32_t>(cost, kSigRailCostHolderDisp) == g_holder &&
                 Rd<uint32_t>(control, kSigRailControlPathDisp) == g_linePath && g_lineCount == g_linePath + 0xc;
    // The vtable: daily is slot 24, finished slot 25; the output getter (the slot the daily step
    // calls) and the cancel slot have the analysed shapes (cancel sets the flag finished reads).
    size_t slots = 0;
    g_vtLine = FindVtableByRtti(".?AVCRailwayProductionLine@@", &slots);
    bool vtOk = g_vtLine && slots > (size_t)kRailLineFinishedSlot && slots > g_slotCancel && slots > g_slotRefresh &&
                slots > g_slotAfterDaily && slots > g_slotOutput && (uint8_t*)g_vtLine[kRailLineDailySlot] == daily &&
                SigMatchAt((uint8_t*)g_vtLine[kRailLineFinishedSlot], kCodeRailLineFinished) &&
                SigMatchAt((uint8_t*)g_vtLine[g_slotOutput], kCodeRailLineOutput) &&
                SigMatchAt((uint8_t*)g_vtLine[g_slotCancel], kCodeRailLineCancel);
    if (!agree || !vtOk) {
        Log("ERROR: railways: construction line call sites disagree (fields %d, vtable %d); queued dismantling off", agree,
            vtOk);
        g_vtLine = nullptr;
        return;
    }
    g_lineOutput = Rd<uint8_t>(g_vtLine[g_slotOutput], kCodeRailLineOutputDisp);
    g_lineCancelled = Rd<uint32_t>(g_vtLine[g_slotCancel], kCodeRailLineCancelFlagDisp);
    g_dailyFn = daily;
    g_progressFn = progress;
    g_finishedFn = (uint8_t*)g_vtLine[kRailLineFinishedSlot];
    Log("Railway lines: construction +0x%x, base +0x%x, target +0x%x, current +0x%x, path +0x%x/+0x%x, country +0x%x, "
        "holder +0x%x (tag +0x%x), total/done/remaining +0x%x/+0x%x/+0x%x, output +0x%x, cancelled +0x%x, slots cancel %u "
        "after-daily %u refresh %u",
        g_construction, g_lineBase, g_lineTarget, g_lineCurrent, g_linePath, g_lineCount, g_lineCountry, g_holder,
        g_holderTag, g_lineTotal, g_lineDone, g_lineRemaining, g_lineOutput, g_lineCancelled, g_slotCancel,
        g_slotAfterDaily, g_slotRefresh);
}

}  // namespace

bool HaveRailwayLines() {
    return HaveRailways() && g_tagToCountry && g_construction && g_createLine && g_lineBase && g_lineTarget && g_vtLine &&
           g_dailyFn && g_progressFn && g_finishedFn && g_lineCurrent && g_linePath && g_lineCountry && g_holder &&
           g_holderTag && g_pathUsable && g_provinceUsable && g_lineTotal && g_lineDone && g_lineRemaining && g_lineOutput &&
           g_lineCancelled && g_cost && g_slotCancel && g_slotAfterDaily && g_slotRefresh && g_slotOutput && g_linkSpeed &&
           g_setSpeed;
}

bool HookRailwayLines() {
    if (!HaveRailwayLines()) return false;
    return HookQueue(g_dailyFn, (void*)&DailyDetour, (void**)&g_dailyOrig, "railway_line_daily") &&
           HookQueue(g_progressFn, (void*)&ProgressDetour, (void**)&g_progressOrig, "railway_line_progress") &&
           HookQueue(g_finishedFn, (void*)&FinishedDetour, (void**)&g_finishedOrig, "railway_line_finished");
}

// --- Wording in the construction queue ---------------------------------------------------------
// The railway entry's tooltip says "Upgrading ... to level ..." for any line with a base level. For
// a dismantle line it should say what happens, so while the builder runs for one, Localize gets a
// mod-defined key instead of RAILWAY_UPGRADE_TOOLTIP (same placeholders). Without that key in the
// loaded localisation the vanilla text stays.
namespace {
using Localize_t = EngineString*(__fastcall*)(EngineString* out, const char* key, void* args, int count);
using TooltipBuilder_t = uint64_t(__fastcall*)(void* item, void* element, void* out);
using LookupLine_t = void*(__fastcall*)(const void* id);
Localize_t g_localizeOrig = nullptr;
TooltipBuilder_t g_tooltipOrig = nullptr;
LookupLine_t g_lookupLine = nullptr;
uint8_t* g_tooltipFn = nullptr;
uint8_t* g_localizeFn = nullptr;
uint32_t g_tooltipId = 0;  // item -> line id
const char kUpgradeKey[] = "RAILWAY_UPGRADE_TOOLTIP";
thread_local int g_inDismantleTooltip = 0;
int g_dismantleKeyKnown = -1;  // the mod's key exists: -1 not checked yet

bool DismantleKeyKnown() {
    if (g_dismantleKeyKnown < 0) {
        g_dismantleKeyKnown = LocalizeKey(kRailwayDismantleTooltipKey) != nullptr;
        Log("railway_queue: %s %s; dismantle entries %s", kRailwayDismantleTooltipKey,
            g_dismantleKeyKnown ? "found" : "not in the loaded localisation",
            g_dismantleKeyKnown ? "use it" : "keep the vanilla upgrade text");
    }
    return g_dismantleKeyKnown == 1;
}

EngineString* __fastcall LocalizeDetour(EngineString* out, const char* key, void* args, int count) {
    if (g_inDismantleTooltip && key && strcmp(key, kUpgradeKey) == 0) {
        g_inDismantleTooltip = 0;  // LocalizeKey below goes through here too
        bool swap = DismantleKeyKnown();
        g_inDismantleTooltip = 1;
        if (swap) key = kRailwayDismantleTooltipKey;
    }
    return g_localizeOrig(out, key, args, count);
}

RailwayHover_t g_hoverHandler = nullptr;

uint64_t __fastcall TooltipDetour(void* item, void* element, void* out) {
    void* line = item ? g_lookupLine((uint8_t*)item + g_tooltipId) : nullptr;
    // Hover: the tooltip is built while the player points at the entry (UI, main thread).
    if (g_hoverHandler && line && *(void***)line == g_vtLine) {
        int count = 0;
        const int* path = LinePath(line, &count);
        if (path && count > 0) g_hoverHandler(path, count);
    }
    if (!IsDismantleLine(line)) return g_tooltipOrig(item, element, out);
    ++g_inDismantleTooltip;
    uint64_t r = g_tooltipOrig(item, element, out);
    --g_inDismantleTooltip;
    return r;
}

// The entry's title gets a marker in front ("▼ Berlin - Stettin"): while the entry update runs,
// SetText on that entry's title element prefixes the text. Dismantle lines get vanilla's red down
// triangle text icon, the engine's own railway lines (building, upgrading) its green up triangle;
// a mod can define HOI4_LOADER_RAILWAY_DISMANTLE_PREFIX / _BUILD_PREFIX instead.
using EntryUpdate_t = void(__fastcall*)(void* item);
using SetText_t = void(__fastcall*)(void* element, EngineString* text);
EntryUpdate_t g_entryUpdateOrig = nullptr;
SetText_t g_setTextOrig = nullptr;
uint8_t* g_entryUpdateFn = nullptr;
uint8_t* g_setTextFn = nullptr;
uint32_t g_entryId = 0, g_entryTitle = 0;
thread_local void* g_markTitle = nullptr;  // the title element of the entry being updated
thread_local bool g_markDismantle = false;

struct Marker {
    const char* key;
    const char* icon;   // the built-in text icon's name
    bool resolved;
    std::string text;   // resolved on first use (main thread); may stay empty (no marker)
};
Marker g_dismantleMarker{kRailwayDismantlePrefixKey, "decrease_texticon", false, {}};
Marker g_buildMarker{kRailwayBuildPrefixKey, "increase_texticon", false, {}};

// The engine does not keep "£" from a localisation file as typed: it converts the icon delimiter
// on load to its own byte sequence (a literal "£", in UTF-8 or Windows-1252, shows as text or
// blanks the title). So the delimiter is read back from vanilla's TRIGGER_FULLFILLED_PREFIX,
// "£trigger_yes£" in the file. Empty when that key does not have that shape.
std::string IconDelimiter() {
    const EngineString* v = LocalizeKey("TRIGGER_FULLFILLED_PREFIX");
    if (!v) return {};
    std::string s = v->c_str();
    size_t at = s.find("trigger_yes");
    if (at == 0 || at == std::string::npos) return {};
    std::string d = s.substr(0, at);
    if (s.compare(at + 11, std::string::npos, d) != 0) return {};
    return d;
}

const std::string& MarkerText(Marker& m) {
    if (!m.resolved) {
        m.resolved = true;
        if (const EngineString* custom = LocalizeKey(m.key)) {
            m.text = custom->c_str();
            Log("railway_queue: railway queue titles use %s", m.key);
        } else if (std::string d = IconDelimiter(); !d.empty()) {
            m.text = d + m.icon + d + " ";
            std::string hex;
            char b[4];
            for (unsigned char c : d) { snprintf(b, sizeof b, "%02X", c); hex += b; }
            Log("railway_queue: railway queue titles use %s (icon delimiter bytes %s)", m.icon, hex.c_str());
        } else {
            Log("railway_queue: icon delimiter not found (TRIGGER_FULLFILLED_PREFIX); railway queue titles stay unmarked");
        }
    }
    return m.text;
}

void __fastcall SetTextDetour(void* element, EngineString* text) {
    if (g_markTitle && element == g_markTitle && text) {
        g_markTitle = nullptr;  // once per update; LocalizeKey may set texts too
        std::string s = MarkerText(g_markDismantle ? g_dismantleMarker : g_buildMarker) + text->c_str();
        FreeEngineString(text);
        ConstructEngineString(text, s.data(), s.size());
    }
    g_setTextOrig(element, text);
}

void __fastcall EntryUpdateDetour(void* item) {
    void* line = item ? g_lookupLine((uint8_t*)item + g_entryId) : nullptr;
    if (!line || *(void***)line != g_vtLine) return g_entryUpdateOrig(item);
    g_markTitle = Rd<void*>(item, g_entryTitle);
    g_markDismantle = IsDismantleLine(line);
    g_entryUpdateOrig(item);
    g_markTitle = nullptr;
}
}  // namespace

bool HookRailwayTooltip() {
    if (!HaveRailwayLines()) return false;
    uint8_t* p = SigFindUnique("rail_tooltip", kSigRailTooltip);
    uint8_t* lea = AnchorFindLea("rail_upgrade_tooltip", kAnchorRailUpgradeTooltip);
    g_localizeFn = SigFindUnique("rail_localize", kSigLocalize);
    if (!p || !lea || !g_localizeFn || lea < p || lea > p + 0x1000) {
        Log("railway_queue: tooltip builder not found or not where expected; the queue keeps the vanilla text");
        return false;
    }
    g_tooltipFn = p;
    g_tooltipId = Rd<uint8_t>(p, kSigRailTooltipIdDisp);
    g_lookupLine = (LookupLine_t)SigFollowRel32(p + kSigRailTooltipLookupCall);
    return HookQueue(g_tooltipFn, (void*)&TooltipDetour, (void**)&g_tooltipOrig, "railway_tooltip") &&
           HookQueue(g_localizeFn, (void*)&LocalizeDetour, (void**)&g_localizeOrig, "localize");
}

bool SetRailwayHoverHandler(RailwayHover_t handler) {
    if (!g_tooltipFn) return false;  // the tooltip hook is not queued
    g_hoverHandler = handler;
    return true;
}

bool HookRailwayEntryTitle() {
    if (!g_lookupLine) return false;  // HookRailwayTooltip resolves the lookup
    uint8_t* update = SigFindUnique("rail_entry_update", kSigRailEntryUpdate);
    uint8_t* title = SigFindUnique("rail_entry_title", kSigRailEntryTitle);
    if (!update || !title || title < update || title > update + 0x400 ||
        (LookupLine_t)SigFollowRel32(update + kSigRailEntryLookupCall) != g_lookupLine) {
        Log("railway_queue: queue entry update not found or not where expected; titles stay unmarked");
        return false;
    }
    g_entryUpdateFn = update;
    g_entryId = Rd<uint32_t>(update, kSigRailEntryIdDisp);
    g_entryTitle = Rd<uint32_t>(title, kSigRailEntryTitleDisp);
    g_setTextFn = SigFollowRel32(title + kSigRailEntrySetTextCall);
    return HookQueue(g_entryUpdateFn, (void*)&EntryUpdateDetour, (void**)&g_entryUpdateOrig, "railway_entry_update") &&
           HookQueue(g_setTextFn, (void*)&SetTextDetour, (void**)&g_setTextOrig, "set_text");
}

// --- Railway map badges ---------------------------------------------------------------------
// The level circle on each railway segment (CRailwayMapIcon). While the dismantle key is held, its
// text becomes a red X wherever the player could dismantle; a click with the key held goes to the
// plugin's handler instead of vanilla's upgrade.
namespace {
using IconFn_t = void(__fastcall*)(void* icon);
using IconBool_t = bool(__fastcall*)(void* icon);
using Colorize_t = EngineString*(__fastcall*)(EngineString* out, char color, const void* view);
struct TextView {  // what Colorize reads: data, then the length (int at +8)
    const char* data;
    int64_t size;
};
IconFn_t g_iconClickOrig = nullptr, g_iconUpdateOrig = nullptr;
IconBool_t g_iconClickable = nullptr;
Colorize_t g_colorize = nullptr;
SetText_t g_iconSetText = nullptr;
uint32_t g_iconPath = 0, g_iconText = 0, g_iconShowSlot = 0;
uint32_t g_lineList = 0, g_lineListCount = 0;  // construction -> its lines
RailwayIconClick_t g_onIconClick = nullptr;
// The text element's flags byte and the mask vanilla ands it with before showing it (clearing the
// hidden flag), both read from kSigRailIconText: +0x75 and ~0x10 on the checked build.
uint32_t g_iconFlags = 0;
uint8_t g_iconShownMask = 0;
uint32_t g_iconLevel = 0, g_iconTarget = 0;  // the segment's level and construction target
uint32_t g_iconHammer = 0;                   // the under-construction mark element (H7; 0: none)
// The badge tooltip (H7): this = icon + g_tipSelfToIcon; the click hints string at out+g_tipHints.
using BadgeTip_t = char(__fastcall*)(void* self, void* arg, void* out);
using StrNewline_t = void(__fastcall*)(void* str);
using StrAppend_t = void(__fastcall*)(void* str, const char* p, size_t n);
BadgeTip_t g_badgeTipOrig = nullptr;
StrNewline_t g_strNewline = nullptr;
StrAppend_t g_strAppend = nullptr;
uint32_t g_tipSelfToIcon = 0, g_tipHints = 0;
using IconVoid_t = void(__fastcall*)(void* icon);
IconVoid_t g_iconRightClickOrig = nullptr;

// --- Lines that cover only part of a segment ------------------------------------------------
// The engine works a railway line link by link along its own path, but the badge shows a line, and
// a click changes it, only when its path is exactly the badge's segment (FindRailwayLine). Removing
// a railway can take away a junction, so two segments merge and a line queued on one of them no
// longer matches anything: its brown target goes and a click would queue a second, overlapping line.
// The line keeps its own stretch; the badge shows it and a click changes it (NOTES.md H3).
using FindLine_t = void*(__fastcall*)(const void* path, const int* tag);
FindLine_t g_findLineOrig = nullptr;
uint8_t* g_findLineFn = nullptr;

bool SharesLink(const int* a, int na, const int* b, int nb) {
    for (int i = 0; i + 1 < na; ++i)
        for (int j = 0; j + 1 < nb; ++j)
            if ((a[i] == b[j] && a[i + 1] == b[j + 1]) || (a[i] == b[j + 1] && a[i + 1] == b[j])) return true;
    return false;
}

// The country's first railway line (queue order) of the given kind that has a link in `path`.
void* OverlappingLine(int tag, const int* path, int count, bool dismantle) {
    if (tag <= 0 || !path || count < 2 || !g_lineList) return nullptr;
    void* country = g_tagToCountry(&tag);
    void* construction = country ? Rd<void*>(country, g_construction) : nullptr;
    if (!construction) return nullptr;
    void* const* lines = Rd<void* const*>(construction, g_lineList);
    int n = Rd<int>(construction, g_lineListCount);
    for (int i = 0; lines && i < n && i < 10000; ++i) {
        void* line = lines[i];
        if (!line || *(void***)line != g_vtLine || IsDismantleLine(line) != dismantle) continue;
        if (Rd<uint8_t>(line, g_lineCancelled)) continue;  // leaves the queue at the next daily update
        int lc;
        const int* lp = LinePath(line, &lc);
        if (lc && SharesLink(lp, lc, path, count)) return line;
    }
    return nullptr;
}

const int* IconPathRaw(void* icon, int* n) {
    const uint8_t* arr = (const uint8_t*)icon + g_iconPath;
    const int* data = Rd<const int*>(arr, 0);
    *n = Rd<int>(arr, 0xc);
    return data && *n >= 2 && *n <= 4096 ? data : nullptr;
}

// The target to show on a badge whose segment has no line of its own but shares links with one of
// the player's upgrade lines; 0 to leave the badge as the engine set it.
int PartialTarget(void* icon) {
    if (!g_enabled || !g_iconTarget || (MainThreadQueueAvailable() && !IsMainThread())) return 0;
    int level = Rd<int>(icon, g_iconLevel);
    if (Rd<int>(icon, g_iconTarget) > level) return 0;  // the engine already shows a line here
    int n;
    const int* path = IconPathRaw(icon, &n);
    int tag = PlayerCountryTag();
    if (!path || tag <= 0 || !g_lineList) return 0;
    // The highest target among the player's upgrade lines that share a link with the segment.
    void* country = g_tagToCountry(&tag);
    void* construction = country ? Rd<void*>(country, g_construction) : nullptr;
    void* const* lines = construction ? Rd<void* const*>(construction, g_lineList) : nullptr;
    int count = construction ? Rd<int>(construction, g_lineListCount) : 0, target = 0;
    for (int i = 0; lines && i < count && i < 10000; ++i) {
        void* line = lines[i];
        if (!line || *(void***)line != g_vtLine || IsDismantleLine(line) || LineTarget(line) <= target) continue;
        int lc;
        const int* lp = LinePath(line, &lc);
        if (lc && SharesLink(lp, lc, path, n)) target = LineTarget(line);
    }
    return target > level ? target : 0;
}

volatile LONG g_partialFaultLogged = 0;
void LogPartialFaultOnce(const char* where) {
    if (!InterlockedExchange(&g_partialFaultLogged, 1))
        Log("ERROR: railway_queue: fault in the partial line lookup (%s); the badge stays vanilla", where);
}

// Vanilla's lookup first; without an exact match, an upgrade line sharing a link with the segment.
void* __fastcall FindLineDetour(const void* path, const int* tag) {
    void* line = g_findLineOrig(path, tag);
    if (line || !g_enabled || !tag) return line;
    __try {
        const PathArray* p = (const PathArray*)path;
        if (p && p->data && p->count >= 2 && p->count <= 4096) line = OverlappingLine(*tag, p->data, p->count, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogPartialFaultOnce("click");
        line = nullptr;
    }
    if (line) Log("railway_queue: badge click: no line for exactly this segment; changing line %p (shares a link)", line);
    return line;
}

// What the badge shows while the dismantle key is held: vanilla's removal cross (the sprite the
// state view's cross uses) as a text icon, or a red X when the icon delimiter is unknown.
// Resolved on first use (main thread).
const char kBadgeIcon[] = "cancel_conversion_button";  // GFX_cancel_conversion_button
bool g_badgeResolved = false;
std::string g_badgeText;
const std::string& BadgeText() {
    if (!g_badgeResolved) {
        g_badgeResolved = true;
        if (std::string d = IconDelimiter(); !d.empty()) {
            g_badgeText = d + kBadgeIcon + d;
            Log("railway_queue: dismantle badges show GFX_%s", kBadgeIcon);
        } else {
            Log("railway_queue: icon delimiter not found; dismantle badges show a red X");
        }
    }
    return g_badgeText;
}

// Alt+Shift. Vanilla's badge click reads Shift (to the maximum level), Ctrl (first in the queue)
// and Alt (last in the queue); Alt+Shift costs the least: "maximum level, last in the queue".
// Ctrl must be up, so vanilla's Ctrl+Alt+Shift still reaches the engine.
bool DismantleKeyHeld() {
    if (!(GetAsyncKeyState(VK_SHIFT) & 0x8000) || !(GetAsyncKeyState(VK_MENU) & 0x8000) ||
        (GetAsyncKeyState(VK_CONTROL) & 0x8000))
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

bool IconPath(void* icon, std::vector<int>* out) {
    const uint8_t* arr = (const uint8_t*)icon + g_iconPath;  // CPdxArray<int>: data, count at +0xc
    const int* data = Rd<const int*>(arr, 0);
    int n = Rd<int>(arr, 0xc);
    if (!data || n < 2 || n > 4096) return false;
    out->assign(data, data + n);
    return true;
}

int PathMaxLevel(const std::vector<int>& path) {
    int max = 0;
    for (size_t i = 1; i < path.size(); ++i) max = std::max(max, RailwayLevel(path[i - 1], path[i]));
    return max;
}

// The player may dismantle this segment: the game rules, or without the relation tests the engine's
// build permission (what a vanilla click on the badge checks).
bool IconDismantleAllowed(void* icon, const std::vector<int>& path) {
    if (HaveRelations()) return DismantleAllowed(PlayerCountryTag(), path.data(), path.size());
    return g_iconClickable(icon);
}

int PartialTargetSafe(void* icon) {
    __try {
        return PartialTarget(icon);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogPartialFaultOnce("badge");
        return 0;
    }
}

// --- Segments being dismantled (NOTES.md H7) ------------------------------------------------
// As vanilla marks a pending upgrade (target number in orange, the under-construction mark, a
// "Currently upgrading" line, its own click hints), a segment with the player's dismantle entry
// shows the entry's target in red with that mark, a "Currently dismantling" line and its own hints.
// Clicks on it change the entry: one level less, with Shift none at all. Vanilla's own click or
// right click there would post a level change that clamps the target up to the entry's base and
// so turns the dismantling into a rebuild.
bool SameLinePath(void* line, const int* path, int n) {
    int count;
    const int* p = LinePath(line, &count);
    if (count != n || !p) return false;
    bool same = true, reversed = true;
    for (int i = 0; i < count; ++i) {
        same &= p[i] == path[i];
        reversed &= p[i] == path[count - 1 - i];
    }
    return same || reversed;
}

// The player's dismantle entry for this badge's segment: the one with exactly its path, else one
// sharing a link with it. Main thread only (null elsewhere).
void* DismantleLineOnIcon(void* icon) {
    if (!g_enabled || !g_lineList || !g_iconLevel || (MainThreadQueueAvailable() && !IsMainThread())) return nullptr;
    int n;
    const int* path = IconPathRaw(icon, &n);
    int tag = path ? PlayerCountryTag() : 0;
    if (tag <= 0) return nullptr;
    void* country = g_tagToCountry(&tag);
    void* construction = country ? Rd<void*>(country, g_construction) : nullptr;
    void* const* lines = construction ? Rd<void* const*>(construction, g_lineList) : nullptr;
    int count = construction ? Rd<int>(construction, g_lineListCount) : 0;
    for (int i = 0; lines && i < count && i < 10000; ++i)
        if (IsDismantleLine(lines[i]) && !Rd<uint8_t>(lines[i], g_lineCancelled) && SameLinePath(lines[i], path, n))
            return lines[i];
    return OverlappingLine(tag, path, n, true);
}

void* DismantleLineOnIconSafe(void* icon) {
    __try {
        return DismantleLineOnIcon(icon);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogPartialFaultOnce("dismantle lookup");
        return nullptr;
    }
}

// Shows a badge element hidden by the update (the text's flags, mask and show slot).
void ShowElement(void* element) {
    Wr<uint8_t>(element, g_iconFlags, Rd<uint8_t>(element, g_iconFlags) & g_iconShownMask);
    ((IconFn_t)(*(void***)element)[g_iconShowSlot])(element);
}

// After the update: the dismantle entry's target in red and the under-construction mark, on a
// badge whose number is showing (not in cooldown). A railway going away entirely shows its current
// level in red instead of a 0.
void MarkDismantle(void* icon) {
    void* text = Rd<void*>(icon, g_iconText);
    if (!text || (Rd<uint8_t>(text, g_iconFlags) & (uint8_t)~g_iconShownMask)) return;  // number hidden
    void* line = DismantleLineOnIconSafe(icon);
    int target = line ? LineTarget(line) : 0, level = Rd<int>(icon, g_iconLevel);
    if (!line || target >= level) return;
    char num[16];
    int len = snprintf(num, sizeof num, "%d", target > 0 ? target : level);  // going away: the level it has
    TextView v{num, len};
    EngineString s;
    g_colorize(&s, 'R', &v);
    g_iconSetText(text, &s);  // takes the string (frees it)
    if (void* hammer = g_iconHammer ? Rd<void*>(icon, g_iconHammer) : nullptr) ShowElement(hammer);
}

void __fastcall IconUpdateDetour(void* icon) {
    // A line covering part of this segment: the update shows its target ("level -> target", the
    // under-construction mark), then the engine's own value goes back, so nothing else sees ours.
    if (int shown = PartialTargetSafe(icon)) {
        int saved = Rd<int>(icon, g_iconTarget);
        Wr<int>(icon, g_iconTarget, shown);
        g_iconUpdateOrig(icon);
        Wr<int>(icon, g_iconTarget, saved);
    } else {
        g_iconUpdateOrig(icon);
    }
    if (!g_enabled) return;
    if (!DismantleKeyHeld()) return MarkDismantle(icon);
    std::vector<int> path;
    void* text = Rd<void*>(icon, g_iconText);
    if (!text || !IconPath(icon, &path) || PathMaxLevel(path) <= 0 || !IconDismantleAllowed(icon, path)) return;
    EngineString s;
    const std::string& badge = BadgeText();
    if (!badge.empty()) {
        ConstructEngineString(&s, badge.data(), badge.size());
    } else {
        TextView x{"X", 1};
        g_colorize(&s, 'R', &x);
    }
    g_iconSetText(text, &s);  // takes the string (frees it)
    ShowElement(text);
}

// The tooltip texts: the mod's localisation when it has the keys, else built-in English with the
// engine's colour codes (0x11 + letter, 0x11 '!' to end, as Colorize writes them).
std::string DismantleTooltipLine(int target) {
    const EngineString* custom = LocalizeKey(kRailwayDismantlingKey);
    std::string s = custom ? custom->c_str()
                           : "Currently dismantling \x11YRailway Connection\x11! to level \x11Y$LEVEL$\x11!";
    std::string level = std::to_string(target);
    for (size_t at; (at = s.find("$LEVEL$")) != std::string::npos;) s.replace(at, 7, level);
    return s;
}
std::string DismantleHints() {
    const EngineString* custom = LocalizeKey(kRailwayDismantleHintsKey);
    return custom ? custom->c_str()
                  : "\x11GClick\x11! or \x11GRight-Click\x11! to dismantle one level less\n"
                    "\x11GShift+Click\x11! to stop dismantling\n"
                    "\x11GAlt+Shift+Click\x11! to dismantle one more level";
}

void AppendDismantleTooltip(void* out, int target) {
    std::string line = DismantleTooltipLine(target);
    g_strNewline(out);
    g_strAppend(out, line.data(), line.size());
    std::string hints = DismantleHints();
    EngineString* h = (EngineString*)((uint8_t*)out + g_tipHints);
    FreeEngineString(h);
    ConstructEngineString(h, hints.data(), hints.size());
}

char __fastcall BadgeTooltipDetour(void* self, void* arg, void* out) {
    char r = g_badgeTipOrig(self, arg, out);
    if (!r || !out || !g_enabled) return r;
    void* icon = (uint8_t*)self - g_tipSelfToIcon;
    void* line = DismantleLineOnIconSafe(icon);
    if (line && LineTarget(line) < Rd<int>(icon, g_iconLevel)) AppendDismantleTooltip(out, LineTarget(line));
    return r;
}

bool ShiftOnlyHeld() {
    if (!(GetAsyncKeyState(VK_SHIFT) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// A click on a segment being dismantled: one level less, with Shift none. True if handled.
bool LessenClick(void* icon) {
    if (!g_enabled || !g_onIconClick || !DismantleLineOnIconSafe(icon)) return false;
    std::vector<int> path;
    return IconPath(icon, &path) &&
           g_onIconClick(path, ShiftOnlyHeld() ? RailwayClick::kStop : RailwayClick::kLessen);
}

void __fastcall IconClickDetour(void* icon) {
    if (g_enabled && g_onIconClick && DismantleKeyHeld()) {
        std::vector<int> path;
        if (IconPath(icon, &path) && IconDismantleAllowed(icon, path) && g_onIconClick(path, RailwayClick::kDeepen))
            return;
    } else if (LessenClick(icon)) {
        return;
    }
    g_iconClickOrig(icon);
}

void __fastcall IconRightClickDetour(void* icon) {
    if (!DismantleKeyHeld() && LessenClick(icon)) return;
    g_iconRightClickOrig(icon);
}

bool SamePath(void* line, const std::vector<int>& path) {
    int count;
    const int* p = LinePath(line, &count);
    if ((size_t)count != path.size()) return false;
    bool same = true, reversed = true;
    for (int i = 0; i < count; ++i) {
        same &= p[i] == path[i];
        reversed &= p[i] == path[count - 1 - i];
    }
    return same || reversed;
}
}  // namespace

// --- Badge segments split where the railway level changes (NOTES.md H6) ----------------------
// The engine's segment builder walks the railway from node to node (a node: a province with a
// supply flag in the supply system's per-province array, or with three or more railway links) and
// gives each segment the level of its lowest link. Vanilla segments always have one level, since an
// upgrade covers a whole segment; after a removal joins two stretches at different levels, the
// merged badge shows the lower one. While the builder runs, it gets a copy of that array in which
// every province with exactly two railway links at different levels has a node flag set, so the
// stretches stay apart. The supply system's own data is never changed. Main thread (the map icon
// update).
namespace {
using SegmentBuilder_t = void(__fastcall*)(void*, void*, void*, void*, void*, void*, void*, void*, void*, uintptr_t,
                                           uintptr_t);
SegmentBuilder_t g_builderOrig = nullptr;
uint32_t g_nodeFlag = 0;  // the supply entry byte the node test reads (+0x15 on the checked build)
const size_t kSupplyEntrySize = 0x20;
std::vector<int> g_levelChanges;       // provinces where the level changes (rebuilt per call)
std::vector<uint8_t> g_supplyCopy;     // the array copy handed to the builder
volatile LONG g_splitFaultLogged = 0;

struct EngineArray {  // CPdxArray as the builder reads it: data, capacity, count at +0xc, allocator
    void* data;
    int capacity;
    int count;
    void* allocator;
};

// Provinces with exactly two railway links whose levels differ. False on a fault.
bool CollectLevelChanges(int* count) {
    *count = 0;
    __try {
        void* m = Manager();
        int n = m ? InfoCount(m) : 0;
        for (int p = 1; p < n; ++p) {
            void* info = Info(m, p);
            if (!info) continue;
            int pairs;
            Pair* pr = Pairs(info, &pairs);
            if (pairs == 2 && pr && pr[0].level != pr[1].level) {
                if ((size_t)*count < g_levelChanges.size()) g_levelChanges[*count] = p;
                ++*count;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The segment records the badges are made from (NOTES.md H7): the badge table (the builder's
// seventh argument) is a hash table of entries keyed by a path's middle province, each with an
// array of 0x30-byte records (level, target, ...).
uint32_t g_recArray = 0, g_recLevel = 0, g_recTarget = 0;  // 0 = not resolved
const size_t kTableEntrySize = 0x28, kRecordSize = 0x30;
volatile LONG g_recFaultLogged = 0;

// A dismantle entry going to 0 gives its segment the target 0, and the map icon refresh makes no
// badge for a record whose target is below 1 (vanilla never has one). Such a record gets its level
// as the target, so the badge stays until the railway is gone; the update hook then marks it.
void KeepRemovalBadges(void* table) {
    if (!g_recTarget || !table) return;
    __try {
        uint8_t* entries = Rd<uint8_t*>(table, 0x8);
        uint32_t mask = Rd<uint32_t>(table, 0x14);
        size_t slots = (size_t)mask + 1 + Rd<uint8_t>(table, 0x18);  // entries past the mask: probe limit
        if (!entries || mask >= (1u << 24)) return;
        for (size_t i = 0; i < slots; ++i) {
            uint8_t* e = entries + i * kTableEntrySize;
            if (e[4] == 0 || e[4] == 0xff) continue;  // empty, end marker
            uint8_t* recs = Rd<uint8_t*>(e, g_recArray);
            int count = Rd<int>(e, g_recArray + 0xc);
            for (int r = 0; recs && r < count && r < 100000; ++r) {
                uint8_t* rec = recs + (size_t)r * kRecordSize;
                int level = Rd<int>(rec, g_recLevel);
                if (level >= 1 && Rd<int>(rec, g_recTarget) < 1) Wr<int>(rec, g_recTarget, level);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!InterlockedExchange(&g_recFaultLogged, 1))
            Log("ERROR: railway_queue: fault in the badge table; a railway dismantled to 0 may lose its badge");
    }
}

void SplitAndBuild(void* a1, void* supplyArray, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8,
                   void* a9, uintptr_t a10, uintptr_t a11) {
    const EngineArray* src = (const EngineArray*)supplyArray;
    if (!g_enabled || !g_nodeFlag || !src || !src->data || src->count <= 0 || src->count > 1000000)
        return g_builderOrig(a1, supplyArray, a3, a4, a5, a6, a7, a8, a9, a10, a11);
    // Two passes when the first one overflows the buffer (more level changes than last time).
    int found = 0;
    bool ok = CollectLevelChanges(&found);
    if (ok && (size_t)found > g_levelChanges.size()) {
        g_levelChanges.resize((size_t)found + 64);
        ok = CollectLevelChanges(&found);
    }
    if (!ok || found == 0 || (size_t)found > g_levelChanges.size()) {
        if (!ok && !InterlockedExchange(&g_splitFaultLogged, 1))
            Log("ERROR: railway_queue: fault while looking for railway level changes; badges are not split");
        return g_builderOrig(a1, supplyArray, a3, a4, a5, a6, a7, a8, a9, a10, a11);
    }
    size_t bytes = (size_t)src->count * kSupplyEntrySize;
    if (g_supplyCopy.size() < bytes) g_supplyCopy.resize(bytes);
    memcpy(g_supplyCopy.data(), src->data, bytes);
    for (int i = 0; i < found; ++i) {
        int p = g_levelChanges[i];
        if (p > 0 && p < src->count) g_supplyCopy[(size_t)p * kSupplyEntrySize + g_nodeFlag] = 1;
    }
    EngineArray copy = *src;
    copy.data = g_supplyCopy.data();
    g_builderOrig(a1, &copy, a3, a4, a5, a6, a7, a8, a9, a10, a11);
}

void __fastcall SegmentBuilderDetour(void* a1, void* supplyArray, void* a3, void* a4, void* a5, void* a6, void* a7,
                                     void* a8, void* a9, uintptr_t a10, uintptr_t a11) {
    SplitAndBuild(a1, supplyArray, a3, a4, a5, a6, a7, a8, a9, a10, a11);
    if (g_enabled) KeepRemovalBadges(a7);
}
}  // namespace

void SetRailwayDismantleRules(RailwayRules_t rules) { g_rulesFn = rules; }

void DisableRailways() {
    if (g_enabled) Log("railways: switched off for this session; no railway is changed any more");
    g_enabled = false;
}

int RailwayDismantleAllowed(int tag, const std::vector<int>& path) {
    if (!HaveRailwayLines() || path.size() < 2 || tag <= 0) return -1;
    if (HaveRelations()) return DismantleAllowed(tag, path.data(), path.size()) ? 1 : 0;
    // Without the relation tests the daily step uses the engine's build permission.
    if (!g_pathUsable(&tag, path.front(), path.back())) return 0;
    for (size_t i = 1; i < path.size(); ++i)
        if (!g_provinceUsable(&tag, path[i])) return 0;
    return 1;
}
void SetRailwayDismantleCost(RailwayCost_t cost) { g_costFn = cost; }

bool HookRailwayMapIcons(RailwayIconClick_t onClick) {
    if (!HaveRailwayLines()) return false;
    uint8_t* click = SigFindUnique("rail_icon_click", kSigRailIconClick);
    uint8_t* update = SigFindUnique("rail_icon_update", kSigRailIconUpdate);
    uint8_t* text = SigFindUnique("rail_icon_text", kSigRailIconText);
    uint8_t* list = SigFindUnique("rail_line_list", kSigRailLineList);
    if (!click || !update || !text || !list || text < update || text > update + 0x600 ||
        Rd<uint32_t>(list, kSigRailListConstructionDisp) != g_construction) {
        Log("railway_queue: map badge code not found or not where expected; no click-to-dismantle");
        return false;
    }
    g_iconClickable = (IconBool_t)SigFollowRel32(click + kSigRailIconClickableCall);
    g_iconPath = Rd<uint32_t>(click, kSigRailIconPathDisp);
    g_colorize = (Colorize_t)SigFollowRel32(text + kSigRailIconColorizeCall);
    g_iconSetText = (SetText_t)SigFollowRel32(text + kSigRailIconSetTextCall);
    g_iconText = Rd<uint32_t>(text, kSigRailIconTextDisp);
    g_iconShowSlot = Rd<uint8_t>(text, kSigRailIconShowSlotDisp) / 8;
    g_iconFlags = Rd<uint8_t>(text, kSigRailIconFlagsDisp);
    g_iconShownMask = Rd<uint8_t>(text, kSigRailIconShownMask);
    // A flags byte inside the element and a mask that clears exactly one bit, as analysed.
    uint8_t cleared = (uint8_t)~g_iconShownMask;
    if (g_iconFlags < 0x10 || cleared == 0 || (cleared & (cleared - 1)) != 0) {
        Log("railway_queue: the badge's show code does not look as analysed; no click-to-dismantle");
        return false;
    }
    g_lineList = Rd<uint8_t>(list, kSigRailListLinesDisp);
    g_lineListCount = Rd<uint8_t>(list, kSigRailListCountDisp);
    if (g_setTextFn && (uint8_t*)g_iconSetText != g_setTextFn) {
        Log("railway_queue: the badge and the queue title call different SetText functions; no click-to-dismantle");
        return false;
    }
    g_onIconClick = onClick;
    Log("Railway badges: path +0x%x, text +0x%x (show slot %u), construction lines +0x%x/+0x%x", g_iconPath, g_iconText,
        g_iconShowSlot, g_lineList, g_lineListCount);
    // Lines covering part of a segment (optional): the level fields inside the update, and the click
    // must call FindRailwayLine (the function kSigRailLineList starts).
    uint8_t* levels = SigFindUnique("rail_icon_levels", kSigRailIconLevels);
    bool clickCallsFind = false;
    for (size_t i = 0; i + 5 <= 0x270; ++i)
        if (click[i] == 0xE8 && SigFollowRel32(click + i) == list) clickCallsFind = true;
    if (levels && levels > update && levels < update + 0x200 && clickCallsFind) {
        g_iconLevel = Rd<uint32_t>(levels, kSigRailIconLevelDisp);
        g_iconTarget = Rd<uint32_t>(levels, kSigRailIconTargetDisp);
        g_findLineFn = list;
        Log("Railway badges: level +0x%x, target +0x%x; lines covering part of a segment are shown and changed",
            g_iconLevel, g_iconTarget);
    } else {
        Log("railway_queue: badge level fields or FindRailwayLine not as analysed; only exact segment lines show on badges");
    }
    // Segments being dismantled (optional, needs the level fields): the under-construction mark (in
    // the update, shown like the text), the tooltip (its target read relative to the icon's) and
    // the right click (same Clickable and path as the left click).
    if (g_iconLevel) {
        uint8_t* hammer = SigFindUnique("rail_icon_hammer", kSigRailIconHammer);
        if (hammer && hammer > update && hammer < update + 0x600 &&
            Rd<uint8_t>(hammer, kSigRailHammerFlagsDisp) == g_iconFlags &&
            Rd<uint8_t>(hammer, kSigRailHammerMask) == g_iconShownMask &&
            Rd<uint8_t>(hammer, kSigRailHammerShowDisp) / 8u == g_iconShowSlot)
            g_iconHammer = Rd<uint32_t>(hammer, kSigRailHammerDisp);
        uint8_t* tip = SigFindUnique("rail_badge_tooltip", kSigRailBadgeTooltip);
        uint32_t tipTarget = tip ? Rd<uint32_t>(tip, kSigRailTipTargetDisp) : 0;
        if (tip && tipTarget && tipTarget < g_iconTarget && g_iconTarget - tipTarget < 0x200) {
            g_tipSelfToIcon = g_iconTarget - tipTarget;
            g_tipHints = Rd<uint8_t>(tip, kSigRailTipHintsImm);
            g_strNewline = (StrNewline_t)SigFollowRel32(tip + kSigRailTipNewlineCall);
            g_strAppend = (StrAppend_t)SigFollowRel32(tip + kSigRailTipAppendCall);
            if (!HookQueue(tip, (void*)&BadgeTooltipDetour, (void**)&g_badgeTipOrig, "railway_badge_tooltip"))
                g_tipSelfToIcon = 0;
        }
        uint8_t* right = SigFindUnique("rail_icon_right_click", kSigRailIconRightClick);
        bool rightOk = right && (IconBool_t)SigFollowRel32(right + kSigRailRightClickableCall) == g_iconClickable &&
                       Rd<uint32_t>(right, kSigRailRightPathDisp) == g_iconPath &&
                       HookQueue(right, (void*)&IconRightClickDetour, (void**)&g_iconRightClickOrig,
                                 "railway_icon_right_click");
        Log("Railway badges: dismantling shown (mark +0x%x, tooltip %s), right click %s", g_iconHammer,
            g_tipSelfToIcon ? "yes" : "no", rightOk ? "changes the dismantle entry" : "vanilla");
    }
    // Badge segments split where the level changes (optional): the node test must lie inside the
    // builder, read its second argument's home slot, and step 0x20-byte entries.
    uint8_t* builder = SigFindUnique("rail_segment_builder", kSigRailSegmentBuilder);
    uint8_t* nodeTest = SigFindUnique("rail_segment_node_test", kSigRailSegmentNodeTest);
    int32_t rbpFromEntry = builder ? Rd<int32_t>(builder, kSigRailBuilderRbpDisp) : 0;  // lea rbp,[rax+disp]
    if (builder && nodeTest && nodeTest > builder && nodeTest < builder + 0x1400) {
        int32_t home = Rd<int32_t>(nodeTest, kSigRailNodeTestHomeDisp);  // [rbp+home] = rdx home
        if (rbpFromEntry < 0 && (int64_t)home + rbpFromEntry == 0x10) {
            g_nodeFlag = Rd<uint8_t>(nodeTest, kSigRailNodeTestFlagDisp);
            if (g_nodeFlag >= kSupplyEntrySize) g_nodeFlag = 0;
        }
    }
    // Badges for railways dismantled to 0 (optional): the record emission must lie inside the
    // builder, read the table from the seventh argument's home slot (+0x38), and insert through the
    // table shape analysed.
    uint8_t* record = builder ? SigFindUnique("rail_segment_record", kSigRailSegmentRecord) : nullptr;
    if (record && record > builder && record < builder + 0x1400 && rbpFromEntry < 0 &&
        (int64_t)Rd<int32_t>(record, kSigRailRecTableHome) + rbpFromEntry == 0x38 &&
        SigMatchAt(SigFollowRel32(record + kSigRailRecInsertCall), kShapeRailSegmentTableInsert)) {
        g_recArray = Rd<uint8_t>(record, kSigRailRecArrayImm);
        g_recLevel = Rd<uint8_t>(record, kSigRailRecLevelDisp);
        g_recTarget = Rd<uint8_t>(record, kSigRailRecTargetDisp);
        if (g_recLevel + 4 > kRecordSize || g_recTarget + 4 > kRecordSize || g_recArray + 0x10 > kTableEntrySize)
            g_recTarget = 0;
    }
    if ((g_nodeFlag || g_recTarget) &&
        !HookQueue(builder, (void*)&SegmentBuilderDetour, (void**)&g_builderOrig, "railway_segment_builder"))
        g_nodeFlag = g_recTarget = 0;
    if (g_nodeFlag)
        Log("Railway badges: segments split where the level changes (supply entry flag +0x%x)", g_nodeFlag);
    else
        Log("railway_queue: segment builder not found or not as analysed; badges are not split by level");
    if (!g_recTarget) Log("railway_queue: badge records not as analysed; a railway dismantled to 0 shows no badge");
    if (g_findLineFn && !HookQueue(g_findLineFn, (void*)&FindLineDetour, (void**)&g_findLineOrig, "railway_find_line")) {
        g_findLineFn = nullptr;
        g_iconTarget = 0;
    }
    return HookQueue(click, (void*)&IconClickDetour, (void**)&g_iconClickOrig, "railway_icon_click") &&
           HookQueue(update, (void*)&IconUpdateDetour, (void**)&g_iconUpdateOrig, "railway_icon_update");
}

int QueueOrDeepenRailwayDismantle(int tag, const std::vector<int>& path) {
    if (!g_enabled || !g_lineList || !RailwaysReady()) return 0;
    void* country = g_tagToCountry(&tag);
    void* construction = country ? Rd<void*>(country, g_construction) : nullptr;
    if (!construction) return 0;
    void* const* lines = Rd<void* const*>(construction, g_lineList);
    int n = Rd<int>(construction, g_lineListCount);
    // The entry for exactly this segment, else one covering part of it (segments merged after it
    // was queued): that entry goes a level deeper on its own stretch.
    void* match = nullptr;
    for (int i = 0; lines && i < n && i < 10000 && !match; ++i)
        if (IsDismantleLine(lines[i]) && !Rd<uint8_t>(lines[i], g_lineCancelled) && SamePath(lines[i], path))
            match = lines[i];
    if (!match) {  // one already going down to nothing leaves the rest of the segment to a new entry
        match = OverlappingLine(tag, path.data(), (int)path.size(), true);
        if (match && LineTarget(match) <= 0) match = nullptr;
    }
    if (void* line = match) {
        int target = LineTarget(line);
        void* holder = LineHolder(line);
        if (target <= 0 || !holder) return 0;  // already going down to nothing
        // One level deeper: the work for that level is added to what remains.
        RefreshCostPercent();
        int levels;
        int64_t before = CostLeft(line, holder, &levels);
        Wr<int>(line, g_lineTarget, target - 1);
        int64_t after = CostLeft(line, holder, &levels);
        int64_t remaining = Rd<int64_t>(line, g_lineRemaining);
        if (remaining > 0) Wr<int64_t>(line, g_lineRemaining, remaining + (after - before));
        Wr<int64_t>(line, g_lineTotal, Rd<int64_t>(line, g_lineTotal) + (after - before));
        ((LineFn_t)g_progressFn)(line);  // hooked: our progress update
        RefreshConstructionView();
        return 2;
    }
    int top = PathMaxLevel(path);
    return top > 0 && QueueRailwayDismantle(tag, path, top, top - 1) ? 1 : 0;
}

int LessenRailwayDismantle(int tag, const std::vector<int>& path, bool stop) {
    if (!g_enabled || !g_lineList || !RailwaysReady() || path.size() < 2) return 0;
    void* country = g_tagToCountry(&tag);
    void* construction = country ? Rd<void*>(country, g_construction) : nullptr;
    if (!construction) return 0;
    void* const* lines = Rd<void* const*>(construction, g_lineList);
    int n = Rd<int>(construction, g_lineListCount);
    void* line = nullptr;
    for (int i = 0; lines && i < n && i < 10000 && !line; ++i)
        if (IsDismantleLine(lines[i]) && !Rd<uint8_t>(lines[i], g_lineCancelled) && SamePath(lines[i], path))
            line = lines[i];
    if (!line) line = OverlappingLine(tag, path.data(), (int)path.size(), true);
    void* holder = line ? LineHolder(line) : nullptr;
    if (!holder) return 0;
    int target = LineTarget(line), count, level;
    const int* lp = LinePath(line, &count);
    // Nothing left to dismantle one level up (or the marker target < base would go): cancel, as the
    // engine cancels a line; the production manager removes it at the next daily update.
    if (stop || target + 1 >= LineBase(line) || FirstLinkAbove(lp, count, target + 1, &level) < 0) {
        ((LineFn_t)Slot(line, g_slotCancel))(line);
        RefreshConstructionView();
        return 2;
    }
    // One level less: that level's work comes off what remains (the work already done stays).
    RefreshCostPercent();
    int levels;
    int64_t before = CostLeft(line, holder, &levels);
    Wr<int>(line, g_lineTarget, target + 1);
    int64_t less = before - CostLeft(line, holder, &levels);
    int64_t remaining = Rd<int64_t>(line, g_lineRemaining);
    if (remaining > 0) Wr<int64_t>(line, g_lineRemaining, remaining - less > 1 ? remaining - less : 1);
    int64_t total = Rd<int64_t>(line, g_lineTotal) - less;
    Wr<int64_t>(line, g_lineTotal, total > 0 ? total : 0);
    ((LineFn_t)g_progressFn)(line);  // hooked: our progress update
    RefreshConstructionView();
    return 1;
}

bool QueueRailwayDismantle(int tag, const std::vector<int>& path, int from, int to) {
    if (!g_enabled || !HaveRailwayLines() || !g_dailyOrig || !g_progressOrig || !g_finishedOrig || path.size() < 2 || to >= from || to < 0)
        return false;
    void* country = g_tagToCountry(&tag);
    void* construction = country ? Rd<void*>(country, g_construction) : nullptr;
    if (!construction) return false;
    PathArray arr{const_cast<int*>(path.data()), (int)path.size(), (int)path.size()};
    g_createLine(construction, &arr, from, to, kInsertAtEnd);  // Init copies the path
    RefreshConstructionView();
    return true;
}

void ResolveRailways() {
    uint8_t* p;
    uint8_t* managerCtor = nullptr;
    uint32_t supplyOffset2 = 0;
    void** gs2 = nullptr;
    if ((p = SigFindUnique("rail_gamestate_init", kSigRailGameStateInit)) != nullptr) {
        g_supplyOffset = Rd<uint32_t>(p, kSigRailSupplyDisp);
        g_managerOffset = Rd<uint32_t>(p, kSigRailManagerDisp);
        managerCtor = SigFollowRel32(p + kSigRailManagerCtorCall);
        if (Rd<uint32_t>(p, kSigRailSupplyStoreDisp) != g_supplyOffset ||
            Rd<uint32_t>(p, kSigRailManagerStoreDisp) != g_managerOffset) {
            Log("ERROR: railways: gamestate setup reads and stores different offsets");
            g_supplyOffset = g_managerOffset = 0;
        }
    }
    if ((p = SigFindUnique("rail_manager_ctor", kSigRailManagerCtor)) != nullptr) {
        // Must lie inside the constructor the gamestate setup calls.
        if (!managerCtor || p < managerCtor || p > managerCtor + 0x400) {
            Log("ERROR: railways: the manager array code is not in the manager constructor");
        } else {
            g_ppGameState = (void**)SigRipTarget(p, kSigRailCtorGameStateRel, kSigRailCtorGameStateRel + 4);
            g_infoArray = Rd<uint8_t>(p, kSigRailInfoArrayDisp);
            g_infoCount = Rd<uint8_t>(p, kSigRailInfoCountDisp);
        }
    }
    uint8_t* neighborSite = SigFindUnique("rail_set_neighbor", kSigRailSetNeighbor);
    uint8_t* levelsSite = SigFindUnique("rail_set_levels", kSigRailSetLevels);
    uint8_t* mirrorSite = SigFindUnique("rail_set_mirror", kSigRailSetMirror);
    uint8_t* neighborIndex = SigFindUnique("rail_neighbor_index", kSigRailNeighborIndex);
    // The three SetRailway parts must come in this order within one function.
    if (neighborSite && levelsSite && mirrorSite && neighborIndex && neighborSite < levelsSite &&
        levelsSite < mirrorSite && mirrorSite - neighborSite < 0x400) {
        g_infoProvince = Rd<uint8_t>(neighborSite, kSigRailProvinceDisp);
        if (SigFollowRel32(neighborSite + kSigRailNeighborIndexCall) == neighborIndex)
            g_neighborIndex = (NeighborIndex_t)neighborIndex;
        else
            Log("ERROR: railways: SetRailway calls a different neighbour lookup");
        g_levels = Rd<uint8_t>(levelsSite, kSigRailLevelsDisp);
        g_pairs = Rd<uint8_t>(levelsSite, kSigRailPairsDisp);
        g_pairCount = Rd<uint8_t>(levelsSite, kSigRailPairCountDisp);
        g_mirror = Rd<uint8_t>(mirrorSite, kSigRailMirrorDisp);
        g_setLevel = (SetLevel_t)SigFollowRel32(mirrorSite + kSigRailSetLevelCall);
        if (Rd<uint8_t>(levelsSite, kSigRailLevelsAgainDisp) != g_levels ||
            Rd<uint8_t>(mirrorSite, kSigRailMirrorStoreDisp) != g_mirror) {
            Log("ERROR: railways: SetRailway reads and writes different fields");
            g_levels = 0;
        }
    } else if (neighborSite && levelsSite && mirrorSite && neighborIndex) {
        Log("ERROR: railways: the SetRailway signatures are not in one function");
    }
    if ((p = SigFindUnique("rail_supply_notify", kSigRailSupplyNotify)) != nullptr) {
        gs2 = (void**)SigRipTarget(p, kSigRailNotifyGameStateRel, kSigRailNotifyGameStateRel + 4);
        supplyOffset2 = Rd<uint32_t>(p, kSigRailNotifySupplyDisp);
        g_notifySlot = Rd<uint8_t>(p, kSigRailNotifySlotDisp) / 8;
    }
    if ((p = SigFindUnique("rail_gfx_refresh", kSigRailGfxRefresh)) != nullptr) {
        g_ppUi = (void**)SigRipTarget(p, kSigRailGfxUiRel, kSigRailGfxUiRel + 4);
        g_uiViewSlot = Rd<uint32_t>(p, kSigRailGfxUiSlotDisp) / 8;
        g_uiViewGfxHolder = Rd<uint8_t>(p, kSigRailGfxViewDisp);
        g_gfxOffset = Rd<uint32_t>(p, kSigRailGfxDisp);
        g_gfxClear = (GfxFn_t)SigFollowRel32(p + kSigRailGfxClearCall);
        g_gfxRebuild = (GfxFn_t)SigFollowRel32(p + kSigRailGfxRebuildJmp);
    }
    if (g_ppGameState && (gs2 != g_ppGameState || supplyOffset2 != g_supplyOffset)) {
        Log("ERROR: railways: the supply refresh reads a different gamestate or supply field");
        g_supplyOffset = 0;
    }
    if (g_managerOffset && g_supplyOffset && g_levels) {
        g_vtManager = FindVtableByRtti(".?AVCRailwayManager@@", nullptr);
        g_vtSupply = FindVtableByRtti(".?AVCSupplySystem@@", nullptr);
        g_vtInfo = FindVtableByRtti(".?AVCProvinceRailwayInfo@@", nullptr);
    }
    ResolveRailwayLines();
    ResolveRelations();
    if (HaveRailways())
        Log("Railways: gamestate %s, manager +0x%x (infos +0x%x/+0x%x), supply +0x%x (slot %u), info: province +0x%x, "
            "levels +0x%x, pairs +0x%x/+0x%x, building +0x%x",
            SigDescribe(g_ppGameState).c_str(), g_managerOffset, g_infoArray, g_infoCount, g_supplyOffset, g_notifySlot,
            g_infoProvince, g_levels, g_pairs, g_pairCount, g_mirror);
}

bool HaveRailways() {
    return g_ppGameState && g_supplyOffset && g_managerOffset && g_infoArray && g_infoCount && g_infoProvince && g_levels &&
           g_pairs && g_pairCount && g_mirror && g_notifySlot && g_neighborIndex && g_setLevel && g_vtManager && g_vtSupply &&
           g_vtInfo && g_ppUi && g_uiViewSlot && g_gfxOffset && g_gfxClear && g_gfxRebuild;
}

bool RailwaysReady() { return HaveRailways() && Manager() != nullptr; }

uint32_t ProvinceControllerOffset() { return g_provController; }

int RailwayLevel(int a, int b) {
    void* m = HaveRailways() ? Manager() : nullptr;
    void* info = m ? Info(m, a) : nullptr;
    return info ? LevelTowards(info, b) : 0;
}

bool RailwaySelfCheck(int* linksChecked, std::string* problem) {
    *linksChecked = 0;
    void* m = HaveRailways() ? Manager() : nullptr;
    if (!m) {
        *problem = "no railway manager";
        return false;
    }
    char buf[160];
    int count = InfoCount(m);
    for (int a = 1; a < count && *linksChecked < 2000; ++a) {
        void* info = Info(m, a);
        if (!info) continue;
        void* province = Rd<void*>(info, g_infoProvince);
        const int* levels = Rd<const int*>(info, g_levels);
        int n;
        Pair* p = Pairs(info, &n);
        for (int i = 0; i < n; ++i, ++*linksChecked) {
            int b = p[i].neighbour, idx = province ? g_neighborIndex(province, b) : -1;
            void* other = Info(m, b);
            if (p[i].level <= 0 || idx < 0 || !levels || levels[idx] != p[i].level || !other ||
                LevelTowards(other, a) != p[i].level) {
                sprintf_s(buf, "link %d-%d: pair level %d, neighbour index %d, level array %d, other end %d", a, b,
                          p[i].level, idx, idx >= 0 && levels ? levels[idx] : -1, other ? LevelTowards(other, a) : -1);
                *problem = buf;
                return false;
            }
        }
    }
    if (*linksChecked == 0) {
        *problem = "no railway found";
        return false;
    }
    return true;
}

namespace {
// Breadth first search over links of at least minLevel: the path with the fewest links. Neighbours
// are visited in the engine's sorted order, so every machine finds the same path. Triggers call it
// too (several threads, no allocation per call), so the work arrays are per thread, grow once and
// are reset by a generation stamp instead of being cleared.
bool FindPath(int from, int to, int minLevel, std::vector<int>* path) {
    if (path) path->clear();
    void* m = HaveRailways() ? Manager() : nullptr;
    if (!m || from == to || !Info(m, from) || !Info(m, to)) return false;
    if (minLevel < 1) minLevel = 1;
    struct Work {
        std::vector<uint32_t> stamp;
        std::vector<int> prev, queue;
        uint32_t generation = 0;
    };
    thread_local Work w;
    int count = InfoCount(m);
    if ((int)w.stamp.size() < count) {
        w.stamp.assign((size_t)count, 0);
        w.prev.assign((size_t)count, 0);
        w.queue.assign((size_t)count, 0);
        w.generation = 0;
    }
    if (++w.generation == 0) {  // wrapped: old stamps could match again
        std::fill(w.stamp.begin(), w.stamp.end(), 0u);
        w.generation = 1;
    }
    const uint32_t gen = w.generation;
    int head = 0, tail = 0;
    w.queue[tail++] = from;
    w.stamp[from] = gen;
    w.prev[from] = from;
    bool found = false;
    while (head < tail && !found) {
        int cur = w.queue[head++];
        void* info = Info(m, cur);
        int n = 0;
        Pair* p = info ? Pairs(info, &n) : nullptr;
        for (int i = 0; p && i < n; ++i) {
            int next = p[i].neighbour;
            if (p[i].level < minLevel || next <= 0 || next >= count || w.stamp[next] == gen) continue;
            w.stamp[next] = gen;
            w.prev[next] = cur;
            if (tail < count) w.queue[tail++] = next;
            if (next == to) {
                found = true;
                break;
            }
        }
    }
    if (!found || !path) return found;
    for (int v = to; v != from; v = w.prev[v]) path->push_back(v);
    path->push_back(from);
    std::reverse(path->begin(), path->end());
    return true;
}

// Lowers every link of `path` by `levels`; collects the provinces whose supply must refresh.
int LowerPath(void* m, const std::vector<int>& path, int levels, std::vector<int>* dirty) {
    int changed = 0;
    for (size_t i = 1; i < path.size(); ++i) {
        int a = path[i - 1], b = path[i];
        void* infoA = Info(m, a);
        void* infoB = Info(m, b);
        int level = infoA && infoB ? LevelTowards(infoA, b) : 0;
        if (level <= 0) continue;
        int lowered = level > levels ? level - levels : 0;
        bool okA = LowerEnd(infoA, b, lowered);
        bool okB = LowerEnd(infoB, a, lowered);
        if (!okA || !okB) Log("railways: link %d-%d is stored on %s end only", a, b, okA ? "one" : "the other");
        ++changed;
        dirty->push_back(a);
        dirty->push_back(b);
    }
    return changed;
}

// The engine's own refresh after a railway change, once per affected province, then the graphics.
void FinishChange(void* supply, const std::vector<int>& dirty, int changed) {
    auto notify = (SupplyNotify_t)(*(void***)supply)[g_notifySlot];
    std::vector<int> done;
    for (int prov : dirty) {
        if (std::find(done.begin(), done.end(), prov) != done.end()) continue;
        done.push_back(prov);
        notify(supply, prov);
    }
    if (changed) RefreshRailwayGraphics();
}

void* Supply() {
    void* gs = GameState();
    void* supply = gs ? Rd<void*>(gs, g_supplyOffset) : nullptr;
    return supply && *(void***)supply == g_vtSupply ? supply : nullptr;
}
}  // namespace

bool RailwayPath(int from, int to, std::vector<int>* path) { return FindPath(from, to, 1, path); }

bool RailwayConnected(int from, int to, int minLevel) { return FindPath(from, to, minLevel, nullptr); }

int RemoveAllRailwayRoutes(int from, int to, int* routes) {
    *routes = 0;
    void* m = g_enabled && HaveRailways() ? Manager() : nullptr;
    void* supply = Supply();
    if (!m || !supply) return 0;
    // Each round removes the current shortest route completely, so at least one link goes and the
    // loop ends once nothing connects the two provinces. The cap only guards against bad data.
    int changed = 0;
    std::vector<int> path, dirty;
    while (*routes < 10000 && FindPath(from, to, 1, &path)) {
        int lowered = LowerPath(m, path, 1000, &dirty);
        if (!lowered) break;
        changed += lowered;
        ++*routes;
    }
    FinishChange(supply, dirty, changed);
    return changed;
}

int DowngradeRailway(const std::vector<int>& path, int levels) {
    void* m = g_enabled && HaveRailways() ? Manager() : nullptr;
    void* supply = Supply();
    if (!m || !supply || levels <= 0) return 0;
    std::vector<int> dirty;
    int changed = LowerPath(m, path, levels, &dirty);
    FinishChange(supply, dirty, changed);
    return changed;
}

}  // namespace Engine
