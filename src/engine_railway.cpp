// Engine access for the railways feature (NOTES.md Part G): railway levels between provinces and
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
void RefreshRailwayGraphics() {
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

// --- Railway construction lines (queued dismantling, NOTES.md G3) ------------------------------
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
uint32_t g_slotCancel = 0, g_slotAfterDaily = 0, g_slotRefresh = 0;
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

// Dismantling a level costs this fraction of building it (the same work at a third of the time).
const int64_t kDismantleCostDivisor = 3;
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
    return cost > 0 ? cost / kDismantleCostDivisor : 0;
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
    int levelsLeft;
    int64_t costLeft = CostLeft(line, holder, &levelsLeft);
    WriteProgress(line, costLeft, levelsLeft);
}

// Diagnostics: the first daily steps of each session and every level lowered go to loader.log.
int g_dailyLogged = 0;

void DismantleDaily(void* line) {
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
void __fastcall DailyDetour(void* line) {
    if (!IsDismantleLine(line)) return g_dailyOrig(line);
    __try {
        DismantleDaily(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogFaultOnce("daily step");
    }
}
void __fastcall ProgressDetour(void* line) {
    if (!IsDismantleLine(line)) return g_progressOrig(line);
    __try {
        DismantleProgress(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogFaultOnce("progress update");
    }
}
bool __fastcall FinishedDetour(void* line) {
    if (!IsDismantleLine(line)) return g_finishedOrig(line);
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
    uint8_t* progress = SigFindUnique("rail_line_progress", kSigRailLineProgress);
    uint8_t* tail = SigFindUnique("rail_line_progress_tail", kSigRailLineProgressTail);
    uint8_t* speed = SigFindUnique("rail_line_speed", kSigRailLineSpeed);
    uint8_t* cost = SigFindUnique("rail_line_cost", kSigRailLineCost);
    if (!daily || !head || !control || !afterDaily || !progress || !tail || !speed || !cost || !g_managerOffset) return;
    // The daily parts must lie inside the daily function, the progress parts inside the progress one.
    auto inside = [](uint8_t* fn, uint8_t* part, size_t size) { return part > fn && part < fn + size; };
    if (!inside(daily, head, 0x400) || !inside(daily, control, 0x400) || !inside(daily, afterDaily, 0x1000) ||
        !inside(progress, tail, 0x600) || !inside(progress, speed, 0x600)) {
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
    // The vtable: daily is slot 24, finished slot 25; the output getter and the cancel slot have the
    // analysed shapes (cancel sets the flag finished reads).
    size_t slots = 0;
    g_vtLine = FindVtableByRtti(".?AVCRailwayProductionLine@@", &slots);
    bool vtOk = g_vtLine && slots > (size_t)kRailLineFinishedSlot && slots > g_slotCancel && slots > g_slotRefresh &&
                slots > g_slotAfterDaily && (uint8_t*)g_vtLine[kRailLineDailySlot] == daily &&
                SigMatchAt((uint8_t*)g_vtLine[kRailLineFinishedSlot], kCodeRailLineFinished) &&
                SigMatchAt((uint8_t*)g_vtLine[14], kCodeRailLineOutput) &&
                SigMatchAt((uint8_t*)g_vtLine[g_slotCancel], kCodeRailLineCancel);
    if (!agree || !vtOk) {
        Log("ERROR: railways: construction line call sites disagree (fields %d, vtable %d); queued dismantling off", agree,
            vtOk);
        g_vtLine = nullptr;
        return;
    }
    g_lineOutput = Rd<uint8_t>(g_vtLine[14], kCodeRailLineOutputDisp);
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
           g_lineCancelled && g_cost && g_slotCancel && g_slotAfterDaily && g_slotRefresh && g_linkSpeed && g_setSpeed;
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

uint64_t __fastcall TooltipDetour(void* item, void* element, void* out) {
    void* line = item ? g_lookupLine((uint8_t*)item + g_tooltipId) : nullptr;
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
    const char* fallback;  // UTF-8
    std::string text;      // resolved on first use (main thread)
};
Marker g_dismantleMarker{kRailwayDismantlePrefixKey, "\xC2\xA3" "decrease_texticon" "\xC2\xA3" " "};
Marker g_buildMarker{kRailwayBuildPrefixKey, "\xC2\xA3" "increase_texticon" "\xC2\xA3" " "};

const std::string& MarkerText(Marker& m) {
    if (m.text.empty()) {
        const EngineString* custom = LocalizeKey(m.key);
        m.text = custom ? custom->c_str() : m.fallback;
        Log("railway_queue: railway queue titles use %s", custom ? m.key : m.fallback);
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
const uint8_t kHiddenFlag = 0x10;  // element+0x75, cleared before show (kSigRailIconText)

bool DismantleKeyHeld() {
    if (!(GetAsyncKeyState(VK_SHIFT) & 0x8000)) return false;
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

void __fastcall IconUpdateDetour(void* icon) {
    g_iconUpdateOrig(icon);
    if (!DismantleKeyHeld()) return;
    std::vector<int> path;
    void* text = Rd<void*>(icon, g_iconText);
    if (!text || !IconPath(icon, &path) || PathMaxLevel(path) <= 0 || !IconDismantleAllowed(icon, path)) return;
    EngineString s;
    TextView x{"X", 1};
    g_colorize(&s, 'R', &x);
    g_iconSetText(text, &s);  // takes the string (frees it)
    Wr<uint8_t>(text, 0x75, Rd<uint8_t>(text, 0x75) & ~kHiddenFlag);
    ((IconFn_t)(*(void***)text)[g_iconShowSlot])(text);
}

void __fastcall IconClickDetour(void* icon) {
    if (g_onIconClick && DismantleKeyHeld()) {
        std::vector<int> path;
        if (IconPath(icon, &path) && IconDismantleAllowed(icon, path) && g_onIconClick(path)) return;
    }
    g_iconClickOrig(icon);
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

void SetRailwayDismantleRules(RailwayRules_t rules) { g_rulesFn = rules; }

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
    g_lineList = Rd<uint8_t>(list, kSigRailListLinesDisp);
    g_lineListCount = Rd<uint8_t>(list, kSigRailListCountDisp);
    if (g_setTextFn && (uint8_t*)g_iconSetText != g_setTextFn) {
        Log("railway_queue: the badge and the queue title call different SetText functions; no click-to-dismantle");
        return false;
    }
    g_onIconClick = onClick;
    Log("Railway badges: path +0x%x, text +0x%x (show slot %u), construction lines +0x%x/+0x%x", g_iconPath, g_iconText,
        g_iconShowSlot, g_lineList, g_lineListCount);
    return HookQueue(click, (void*)&IconClickDetour, (void**)&g_iconClickOrig, "railway_icon_click") &&
           HookQueue(update, (void*)&IconUpdateDetour, (void**)&g_iconUpdateOrig, "railway_icon_update");
}

int QueueOrDeepenRailwayDismantle(int tag, const std::vector<int>& path) {
    if (!g_lineList || !RailwaysReady()) return 0;
    void* country = g_tagToCountry(&tag);
    void* construction = country ? Rd<void*>(country, g_construction) : nullptr;
    if (!construction) return 0;
    void* const* lines = Rd<void* const*>(construction, g_lineList);
    int n = Rd<int>(construction, g_lineListCount);
    for (int i = 0; lines && i < n && i < 10000; ++i) {
        void* line = lines[i];
        if (!IsDismantleLine(line) || !SamePath(line, path)) continue;
        int target = LineTarget(line);
        void* holder = LineHolder(line);
        if (target <= 0 || !holder) return 0;  // already going down to nothing
        // One level deeper: the work for that level is added to what remains.
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

bool QueueRailwayDismantle(int tag, const std::vector<int>& path, int from, int to) {
    if (!HaveRailwayLines() || !g_dailyOrig || !g_progressOrig || !g_finishedOrig || path.size() < 2 || to >= from || to < 0)
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

bool RailwayPath(int from, int to, std::vector<int>* path) {
    path->clear();
    void* m = HaveRailways() ? Manager() : nullptr;
    if (!m || from == to || !Info(m, from) || !Info(m, to)) return false;
    // Breadth first over links with a railway: the path with the fewest links. Neighbours are
    // visited in the engine's sorted order, so every machine finds the same path.
    int count = InfoCount(m);
    std::vector<int> prev((size_t)count, 0);
    std::vector<int> queue;
    queue.push_back(from);
    prev[from] = from;
    for (size_t head = 0; head < queue.size() && !prev[to]; ++head) {
        void* info = Info(m, queue[head]);
        int n = 0;
        Pair* p = info ? Pairs(info, &n) : nullptr;
        for (int i = 0; p && i < n; ++i) {
            int next = p[i].neighbour;
            if (p[i].level <= 0 || next <= 0 || next >= count || prev[next]) continue;
            prev[next] = queue[head];
            queue.push_back(next);
        }
    }
    if (!prev[to]) return false;
    for (int v = to; v != from; v = prev[v]) path->push_back(v);
    path->push_back(from);
    std::reverse(path->begin(), path->end());
    return true;
}

int DowngradeRailway(const std::vector<int>& path, int levels) {
    void* m = HaveRailways() ? Manager() : nullptr;
    void* gs = GameState();
    void* supply = gs ? Rd<void*>(gs, g_supplyOffset) : nullptr;
    if (!m || !supply || *(void***)supply != g_vtSupply || levels <= 0) return 0;
    int changed = 0;
    std::vector<int> dirty;
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
        dirty.push_back(a);
        dirty.push_back(b);
    }
    // The engine's own refresh after a railway change, once per affected province.
    auto notify = (SupplyNotify_t)(*(void***)supply)[g_notifySlot];
    std::vector<int> done;
    for (int prov : dirty) {
        bool seen = false;
        for (int d : done) seen |= d == prov;
        if (seen) continue;
        done.push_back(prov);
        notify(supply, prov);
    }
    if (changed) RefreshRailwayGraphics();
    return changed;
}

}  // namespace Engine
