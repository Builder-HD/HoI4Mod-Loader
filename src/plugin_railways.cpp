// railways (engine tier): lower railway levels, which vanilla script cannot do.
//
//   downgrade_railway = { start_province = 6521 target_province = 3312 }             one level
//   downgrade_railway = { start_province = 6521 target_province = 3312 levels = 5 }  remove it
//
// Follows the existing railway with the fewest links from start_province to target_province and
// lowers every link on it by `levels` (default 1; a link at 0 has no railway). Both ends of each
// link, the provinces' rail_way buildings and the supply system are updated, as the engine does
// when it builds a railway. vanilla remove_building = { type = rail_way } only changes that
// building, never the railway itself (NOTES.md Part G).
//
// The railway_queue feature in this file adds queue_railway_dismantle: the same lowering as a
// construction queue entry (NOTES.md G3).
#include <atomic>
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

// Parameters: 0 start_province, 1 target_province, 2 levels.
int Levels(const Args& a) { return a.has(2) ? a.i(2) : 1; }

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
    args.push_back({"LEVELS", std::to_string(Levels(a))});
    out.add(native_localize(kDowngradeText, args).c_str());
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
        g_ok.store(false);
        FeatureDeactivate(kFeature, ("self-check failed: " + problem).c_str());
        return;
    }
    g_ok.store(true);
    Log("railways: self-check passed (%d railway links consistent on both ends)", links);
}

// --- railway_queue: dismantling as a construction queue entry -------------------------------
//
//   queue_railway_dismantle = { start_province = 6521 target_province = 6282 }   one level
//
// Country scope. Adds a line to the country's construction queue that lowers every link of the same
// path as downgrade_railway, one level at a time, down to (highest level on the path - levels).
// Each level costs a third of building it, so it takes a third of the time.
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
}

void TooltipQueue(const Args& a, const Scope&, TooltipText& out) {
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

// Shift+click on a railway map badge: one more dismantle level on that segment. The click comes
// from the UI, so the change waits for the next safe point on the main thread. Only where local
// writes are allowed (single player): a click happens on one machine only.
struct IconClick {
    int tag;
    std::vector<int> path;
};

void QueueClicked(void* ctx) {
    IconClick* c = (IconClick*)ctx;
    int r = Engine::QueueOrDeepenRailwayDismantle(c->tag, c->path);
    Log("railway_queue: badge click on %d -> %d (%zu provinces): %s", c->path.front(), c->path.back(), c->path.size(),
        r == 2 ? "dismantle entry one level deeper" : r == 1 ? "dismantle entry queued" : "nothing to dismantle");
    delete c;
}

bool OnIconClick(const std::vector<int>& path) {
    const char* why = nullptr;
    if (!CurrentSessionSinglePlayer(&why)) {
        static bool logged = false;
        if (!logged) Log("railway_queue: Shift+click dismantling is single player only (%s); click ignored", why);
        logged = true;
        return true;  // swallowed: no surprise "upgrade to maximum" either
    }
    int tag = Engine::PlayerCountryTag();
    if (tag <= 0) return false;
    IconClick* c = new IconClick{tag, path};
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
    // Optional too: Shift on the railway map badges (needs the main-thread queue to defer clicks).
    if (MainThreadQueueAvailable()) Engine::HookRailwayMapIcons(&OnIconClick);
    // Allies / subjects / overlord: the game rules' global flags.
    OnLexerReady(&InternRuleFlags);
    Engine::SetRailwayDismantleRules(&ReadRules);
    register_native_effect({"queue_railway_dismantle",
                            "Add dismantling a railway to the scope country's construction queue (hoi4_loader). Uses "
                            "the same path as downgrade_railway (the existing railway with the fewest links from "
                            "start_province to target_province) and lowers its links one level at a time down to the "
                            "path's highest level minus `levels` (default 1). Each level costs a third of building it. "
                            "Nothing is queued if no railway connects the provinces.",
                            Form::Block,
                            {{"start_province", ParamType::Int, true},
                             {"target_province", ParamType::Int, true},
                             {"levels", ParamType::Int, false}},
                            ScopeMask::Country, &ExecQueue, &TooltipQueue, {kQueueText, kNoRailwayText}},
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
                            "`levels` (default 1); a link lowered to 0 is removed. Updates both ends of each link, the "
                            "provinces' rail_way buildings and supply. Does nothing if no railway connects them.",
                            Form::Block,
                            {{"start_province", ParamType::Int, true},
                             {"target_province", ParamType::Int, true},
                             {"levels", ParamType::Int, false}},
                            ScopeMask::Any, &Exec, &Tooltip, {kDowngradeText, kNoRailwayText}},
                           kFeature);
    OnGameEntered(&OnEntered);
    return true;
}
