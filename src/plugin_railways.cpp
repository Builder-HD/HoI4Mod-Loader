// railways (engine tier): lower railway levels, which vanilla script cannot do.
//
//   downgrade_railway = { start_province = 6521 target_province = 3312 }                   one level
//   downgrade_railway = { start_province = 6521 target_province = 3312 levels = all }      remove it
//   downgrade_railway = { start_province = 6521 target_province = 3312 all_routes = yes }  disconnect
//   has_railway_between = { start_province = 6521 target_province = 3312 min_level = 3 }   trigger
//
// Follows the existing railway with the fewest links from start_province to target_province and
// lowers every link on it by `levels` (default 1; a link at 0 has no railway). all_routes removes
// the shortest route completely, again and again, until no railway connects them. Both ends of each
// link, the provinces' rail_way buildings and the supply system are updated, as the engine does
// when it builds a railway. vanilla remove_building = { type = rail_way } only changes that
// building, never the railway itself (NOTES.md Part H).
//
// The railway_queue feature in this file adds queue_railway_dismantle: the same lowering as a
// construction queue entry (NOTES.md H3).
#include <atomic>
#include <climits>
#include <cstring>
#include <string>
#include <vector>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "native_script.h"

namespace {

const char* const kFeature = "railways";
std::atomic<bool> g_ok{true};

const LocText kDowngradeText{"HOI4_LOADER_DOWNGRADE_RAILWAY",
                             "Railway from province $FROM|Y$ to province $TO|Y$ ($LINKS|Y$ links) loses $LEVELS|Y$ level(s)."};
const LocText kNoRailwayText{"HOI4_LOADER_DOWNGRADE_RAILWAY_NONE",
                             "No railway connects province $FROM|Y$ to province $TO|Y$: nothing changes."};
const LocText kRemoveText{"HOI4_LOADER_REMOVE_RAILWAY",
                          "Railway from province $FROM|Y$ to province $TO|Y$ ($LINKS|Y$ links) is removed."};
const LocText kRemoveAllText{"HOI4_LOADER_REMOVE_ALL_RAILWAYS",
                             "Every railway route between province $FROM|Y$ and province $TO|Y$ is removed (the "
                             "shortest has $LINKS|Y$ links)."};

// Parameters: 0 start_province, 1 target_province, 2 levels (a number or `all`), 3 all_routes.
// `levels = all` removes the path whatever its levels: more than any railway level.
const int kAllLevels = 1000;
int Levels(const Args& a) {
    if (!a.has(2)) return 1;
    return IntWordIndex(a.i(2)) == 0 ? kAllLevels : a.i(2);
}
bool AllRoutes(const Args& a) { return a.b(3); }

void Exec(const Args& a, const Scope&) {
    if (!g_ok.load(std::memory_order_relaxed)) return;
    int from = a.i(0), to = a.i(1), levels = Levels(a);
    if (levels <= 0) {
        Log("downgrade_railway: levels = %d ignored (must be at least 1)", levels);
        return;
    }
    if (!Engine::RailwaysReady()) {
        Log("downgrade_railway: railway data unavailable; nothing changed");
        return;
    }
    if (AllRoutes(a)) {
        if (a.has(2) && levels != kAllLevels)
            Log("downgrade_railway: all_routes = yes removes every route completely; levels = %d ignored", levels);
        int routes = 0;
        int changed = Engine::RemoveAllRailwayRoutes(from, to, &routes);
        Log("downgrade_railway: provinces %d -> %d, all routes: %d route(s), %d link(s) removed", from, to, routes,
            changed);
        return;
    }
    std::vector<int> path;
    if (!Engine::RailwayPath(from, to, &path)) {
        Log("downgrade_railway: no railway connects provinces %d and %d; nothing changed", from, to);
        return;
    }
    int changed = Engine::DowngradeRailway(path, levels);
    Log("downgrade_railway: provinces %d -> %d, %zu links on the path, %d lowered by up to %d level(s)", from, to,
        path.size() - 1, changed, levels);
}

void Tooltip(const Args& a, const Scope&, TooltipText& out) {
    std::vector<int> path;
    std::vector<LocArg> args = {{"FROM", std::to_string(a.i(0))}, {"TO", std::to_string(a.i(1))}};
    if (!Engine::RailwayPath(a.i(0), a.i(1), &path)) {
        out.add(native_localize(kNoRailwayText, args).c_str());
        return;
    }
    args.push_back({"LINKS", std::to_string(path.size() - 1)});
    if (AllRoutes(a)) {
        out.add(native_localize(kRemoveAllText, args).c_str());
        return;
    }
    if (Levels(a) == kAllLevels) {
        out.add(native_localize(kRemoveText, args).c_str());
        return;
    }
    args.push_back({"LEVELS", std::to_string(Levels(a))});
    out.add(native_localize(kDowngradeText, args).c_str());
}

// --- has_railway_between: is there a railway (of at least min_level) between two provinces? ---
// Parameters: 0 start_province, 1 target_province, 2 min_level (default 1).
const LocText kBetweenText{"HOI4_LOADER_HAS_RAILWAY_BETWEEN",
                           "A railway connects province $FROM|Y$ to province $TO|Y$"};
const LocText kBetweenLevelText{"HOI4_LOADER_HAS_RAILWAY_BETWEEN_LEVEL",
                                "A railway of level $LEVEL|Y$ or higher connects province $FROM|Y$ to province $TO|Y$"};
const LocText kNotBetweenText{"HOI4_LOADER_HAS_NO_RAILWAY_BETWEEN",
                              "No railway connects province $FROM|Y$ to province $TO|Y$"};
const LocText kNotBetweenLevelText{"HOI4_LOADER_HAS_NO_RAILWAY_BETWEEN_LEVEL",
                                   "No railway of level $LEVEL|Y$ or higher connects province $FROM|Y$ to province "
                                   "$TO|Y$"};

int MinLevel(const Args& a) { return a.has(2) && a.i(2) > 1 ? a.i(2) : 1; }

bool EvalBetween(const Args& a, const Scope&) {
    if (!g_ok.load(std::memory_order_relaxed)) return false;
    return Engine::RailwayConnected(a.i(0), a.i(1), MinLevel(a));
}

void TooltipBetween(const Args& a, const Scope&, bool negated, TooltipText& out) {
    std::vector<LocArg> args = {{"FROM", std::to_string(a.i(0))}, {"TO", std::to_string(a.i(1))}};
    bool level = MinLevel(a) > 1;
    if (level) args.push_back({"LEVEL", std::to_string(MinLevel(a))});
    const LocText& t = negated ? (level ? kNotBetweenLevelText : kNotBetweenText)
                               : (level ? kBetweenLevelText : kBetweenText);
    out.add(native_localize(t, args).c_str());
}

// No C++ objects with destructors here, so SEH can contain a fault while reading.
bool SafeCheck(int* links, std::string* problem) {
    __try {
        return Engine::RailwaySelfCheck(links, problem);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void OnEntered(bool) {
    int links = 0;
    std::string problem = "fault while reading";
    if (!SafeCheck(&links, &problem)) {
        // Off for the rest of the session, everything that changes railways with it: the effects
        // (g_ok), and in the engine the queue's hooks (its entries pause) and the map badges.
        g_ok.store(false);
        Engine::DisableRailways();
        FeatureDeactivate(kFeature, ("self-check failed: " + problem).c_str());
        FeatureDeactivate("railway_queue", "requires railways, whose self-check failed");
        return;
    }
    if (!g_ok.load()) return;  // switched off earlier this session: stays off
    g_ok.store(true);
    Log("railways: self-check passed (%d railway links consistent on both ends)", links);
}

// --- railway_queue: dismantling as a construction queue entry -------------------------------
//
//   queue_railway_dismantle = { start_province = 6521 target_province = 6282 }   one level
//
// Country scope. Adds a line to the country's construction queue that lowers every link of the same
// path as downgrade_railway, one level at a time, down to (highest level on the path - levels).
// Each level costs 30% of building it by default (a game rule can change that, see ReadCostPercent).
const char* const kQueueFeature = "railway_queue";

const LocText kQueueText{"HOI4_LOADER_QUEUE_RAILWAY_DISMANTLE",
                         "Adds dismantling the railway from province $FROM|Y$ to province $TO|Y$ ($LINKS|Y$ links) to the "
                         "construction queue, down to level $LEVEL|Y$."};

// Parameters: 0 start_province, 1 target_province, 2 levels.
bool QueuePlan(const Args& a, std::vector<int>* path, int* from, int* to) {
    int levels = Levels(a);
    if (levels <= 0 || !Engine::RailwayPath(a.i(0), a.i(1), path)) return false;
    *from = 0;
    for (size_t i = 1; i < path->size(); ++i) {
        int l = Engine::RailwayLevel((*path)[i - 1], (*path)[i]);
        if (l > *from) *from = l;
    }
    *to = *from > levels ? *from - levels : 0;
    return *from > 0;
}

void ExecQueue(const Args& a, const Scope& s) {
    int tag = s.countryTag();
    std::vector<int> path;
    int from, to;
    if (tag <= 0 || !Engine::RailwaysReady() || !QueuePlan(a, &path, &from, &to)) {
        Log("queue_railway_dismantle: provinces %d -> %d: no railway, no country scope or no railway data; nothing queued",
            a.i(0), a.i(1));
        return;
    }
    bool ok = Engine::QueueRailwayDismantle(tag, path, from, to);
    Log("queue_railway_dismantle: %s: provinces %d -> %d, %zu links, level %d down to %d: %s",
        Engine::CountryTagName(tag).c_str(), a.i(0), a.i(1), path.size() - 1, from, to, ok ? "queued" : "FAILED");
    // The entry's daily step cancels it where the country may not dismantle (the game rules, or the
    // build permission): say so now instead of letting it vanish silently the next day.
    if (ok && Engine::RailwayDismantleAllowed(tag, path) == 0)
        Log("queue_railway_dismantle: %s may not dismantle along this path (it does not control every province, or the "
            "game rules do not allow it there); the entry will be cancelled on its first day",
            Engine::CountryTagName(tag).c_str());
}

const LocText kQueueNotAllowedText{"HOI4_LOADER_QUEUE_RAILWAY_DISMANTLE_NOT_ALLOWED",
                                   "\x11RThe country may not dismantle along this railway: the entry would be cancelled on "
                                   "its first day.\x11!"};

void TooltipQueue(const Args& a, const Scope& s, TooltipText& out) {
    std::vector<int> path;
    int from, to;
    std::vector<LocArg> args = {{"FROM", std::to_string(a.i(0))}, {"TO", std::to_string(a.i(1))}};
    if (!QueuePlan(a, &path, &from, &to)) {
        out.add(native_localize(kNoRailwayText, args).c_str());
        return;
    }
    args.push_back({"LINKS", std::to_string(path.size() - 1)});
    args.push_back({"LEVEL", std::to_string(to)});
    out.add(native_localize(kQueueText, args).c_str());
    int tag = s.countryTag();
    if (tag > 0 && Engine::RailwayDismantleAllowed(tag, path) == 0) out.add(native_localize(kQueueNotAllowedText, {}).c_str());
}

// Where the player may dismantle besides its own provinces: global flags a mod sets at game start
// from its game rules (MODDING.md), or later from script. Bits as Engine::kRailRule*.
const char* const kRuleFlags[] = {"hoi4_loader_railway_dismantle_allies", "hoi4_loader_railway_dismantle_subjects",
                                  "hoi4_loader_railway_dismantle_overlord"};
int g_ruleTokens[3] = {};

void InternRuleFlags() {
    for (int i = 0; i < 3; ++i) g_ruleTokens[i] = Engine::Intern(kRuleFlags[i]);
}

int ReadRules() {
    int mask = 0;
    Engine::ForEachGlobalFlag(
        [](const Engine::FlagEntry& f, void* ctx) {
            for (int i = 0; i < 3; ++i)
                if (g_ruleTokens[i] && f.token == g_ruleTokens[i]) *(int*)ctx |= 1 << i;
        },
        &mask);
    return mask;
}

// The cost of dismantling, in percent of building: a global flag hoi4_loader_railway_dismantle_cost_<N>
// (N = 0..1000), which a mod sets from a game rule. With several, the most recently set one counts.
// None: -1, the engine's default (30).
const char kCostPrefix[] = "hoi4_loader_railway_dismantle_cost_";

int ReadCostPercent() {
    struct Best {
        int percent = -1;
        int date = INT_MIN;
    } best;
    Engine::ForEachGlobalFlag(
        [](const Engine::FlagEntry& f, void* ctx) {
            const char* name = Engine::TokenName(f.token);
            if (!name || strncmp(name, kCostPrefix, sizeof(kCostPrefix) - 1) != 0) return;
            const char* digits = name + sizeof(kCostPrefix) - 1;
            int n = 0, len = 0;
            for (; digits[len] >= '0' && digits[len] <= '9' && len < 5; ++len) n = n * 10 + (digits[len] - '0');
            if (len == 0 || digits[len] != '\0' || n > 1000) return;
            Best* b = (Best*)ctx;
            if (f.setDate >= b->date) *b = {n, f.setDate};
        },
        &best);
    return best.percent;
}

// Clicks on a railway map badge: Alt+Shift+click one more dismantle level on that segment; on a
// segment being dismantled, a click or right click one level less, with Shift no dismantling at
// all. The click comes from the UI, so the change waits for the next safe point on the main
// thread. Only where local writes are allowed (single player): a click happens on one machine only.
struct IconClick {
    int tag;
    std::vector<int> path;
    Engine::RailwayClick action;
};

void QueueClicked(void* ctx) {
    IconClick* c = (IconClick*)ctx;
    const char* what;
    if (c->action == Engine::RailwayClick::kDeepen) {
        int r = Engine::QueueOrDeepenRailwayDismantle(c->tag, c->path);
        what = r == 2 ? "dismantle entry one level deeper" : r == 1 ? "dismantle entry queued" : "nothing to dismantle";
    } else {
        int r = Engine::LessenRailwayDismantle(c->tag, c->path, c->action == Engine::RailwayClick::kStop);
        what = r == 2 ? "dismantle entry cancelled" : r == 1 ? "dismantle entry one level less" : "no dismantle entry";
    }
    Log("railway_queue: badge click on %d -> %d (%zu provinces): %s", c->path.front(), c->path.back(), c->path.size(),
        what);
    delete c;
}

bool OnIconClick(const std::vector<int>& path, Engine::RailwayClick action) {
    const char* why = nullptr;
    if (!CurrentSessionSinglePlayer(&why)) {
        static bool logged = false;
        if (!logged) Log("railway_queue: badge clicks on dismantling are single player only (%s); click ignored", why);
        logged = true;
        return true;  // swallowed: no surprise vanilla click either
    }
    int tag = Engine::PlayerCountryTag();
    if (tag <= 0) return false;
    IconClick* c = new IconClick{tag, path, action};
    if (!RunOnMainThread(&QueueClicked, c)) {
        delete c;
        return false;
    }
    return true;
}

}  // namespace

bool RailwayQueueInit() {
    if (!Engine::HaveRailwayLines() || !Engine::HookRailwayLines()) {
        Log("railway_queue: construction line functions or hooks unavailable");
        return false;
    }
    // Optional: without them the queue shows the vanilla upgrade text and an unmarked title.
    if (Engine::HookRailwayTooltip()) Engine::HookRailwayEntryTitle();
    // Optional too: Alt+Shift on the railway map badges (needs the main-thread queue to defer clicks).
    if (MainThreadQueueAvailable()) Engine::HookRailwayMapIcons(&OnIconClick);
    // Allies / subjects / overlord: the game rules' global flags.
    OnLexerReady(&InternRuleFlags);
    Engine::SetRailwayDismantleRules(&ReadRules);
    Engine::SetRailwayDismantleCost(&ReadCostPercent);
    register_native_effect({"queue_railway_dismantle",
                            "Add dismantling a railway to the scope country's construction queue (hoi4_loader). Uses "
                            "the same path as downgrade_railway (the existing railway with the fewest links from "
                            "start_province to target_province) and lowers its links one level at a time down to the "
                            "path's highest level minus `levels` (default 1; `all` removes it). Each level costs 30% "
                            "of building it, or what the global flag hoi4_loader_railway_dismantle_cost_<percent> "
                            "says. Nothing is queued if no railway connects the provinces.",
                            Form::Block,
                            {{"start_province", ParamType::Int, true},
                             {"target_province", ParamType::Int, true},
                             {"levels", ParamType::Int, false, ScopeDefault::None, "all"}},
                            ScopeMask::Country, &ExecQueue, &TooltipQueue, {kQueueText, kNoRailwayText, kQueueNotAllowedText}},
                           kQueueFeature);
    return true;
}

bool RailwaysInit() {
    if (!Engine::HaveRailways()) {
        Log("railways: required engine functions or fields missing");
        return false;
    }
    register_native_effect({"downgrade_railway",
                            "Lower the railway between two provinces (hoi4_loader). Follows the existing railway with "
                            "the fewest links from start_province to target_province and lowers every link on it by "
                            "`levels` (default 1; `all` removes it); a link lowered to 0 is removed. `all_routes = "
                            "yes` instead removes every route until no railway connects the two provinces, which can "
                            "include long detours. Updates both ends of each link, the provinces' rail_way buildings "
                            "and supply. Does nothing if no railway connects them.",
                            Form::Block,
                            {{"start_province", ParamType::Int, true},
                             {"target_province", ParamType::Int, true},
                             {"levels", ParamType::Int, false, ScopeDefault::None, "all"},
                             {"all_routes", ParamType::Bool, false}},
                            ScopeMask::Any, &Exec, &Tooltip,
                            {kDowngradeText, kRemoveText, kRemoveAllText, kNoRailwayText}},
                           kFeature);
    register_native_trigger({"has_railway_between",
                             "True if a railway connects start_province to target_province (hoi4_loader), over any "
                             "route. With `min_level`, only links of at least that level count. The same railway "
                             "data downgrade_railway uses. Guard it with hoi4_loader_feature_railways = yes in the "
                             "same AND block.",
                             Form::Block,
                             {{"start_province", ParamType::Int, true},
                              {"target_province", ParamType::Int, true},
                              {"min_level", ParamType::Int, false}},
                             ScopeMask::Any, &EvalBetween, &TooltipBetween},
                            kFeature);
    OnGameEntered(&OnEntered);
    return true;
}
