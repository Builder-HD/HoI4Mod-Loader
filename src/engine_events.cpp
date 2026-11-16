// Engine access for engine_on_actions (NOTES.md Part Q): the moments vanilla fires no on_action
// for. Each hook records plain data (tags, ids, tokens) right after (or, where the objects are
// about to go, before) the engine function that does the thing. An event fires at once when that
// function was called outside a marked engine pass; inside one (the hourly tick, a country's
// construction pass, its research update) it waits for the end of that pass, where no engine
// loop a mod's effect could break is running (Q5/Q6). Batches fire in a fixed order, so every
// machine fires the same events in the same order whatever thread recorded them.
#include <windows.h>
#include <string.h>

#include <algorithm>
#include <mutex>
#include <vector>

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

using Generic4_t = void*(__fastcall*)(void*, void*, void*, void*);
using Getter_t = void*(__fastcall*)(const void*);

// Resolved.
uint8_t* g_consDone = nullptr;
uint32_t g_consCountry = 0, g_consType = 0, g_tagOffset = 0;
Getter_t g_buildingProvince = nullptr, g_buildingState = nullptr;
uint8_t* g_consPass = nullptr;
uint8_t* g_researchUpdate = nullptr;
uint8_t* g_techLevel = nullptr;
uint32_t g_techTemplate = 0, g_techLevelField = 0, g_techOwner = 0, g_techCountry = 0;
uint8_t* g_addCombat = nullptr;
uint32_t g_sideA = 0, g_sideUnits = 0, g_sideCount = 0, g_combatProvince = 0;
uint8_t* g_battleEnd = nullptr;
uint8_t* g_divisionKilled = nullptr;
uint8_t* g_deploy = nullptr;
uint8_t* g_addUnit = nullptr;
uint8_t* g_hourlyTick = nullptr;
uint32_t g_stateIdDisp = 0;

// Hook state.
EngineEventSink_t g_sink = nullptr;
volatile bool g_running = false;
volatile LONG g_faulted = 0;
int g_passDepth = 0;  // main thread only
int g_deployDepth = 0;
bool g_firing = false;
std::mutex g_lock;
std::vector<EngineEvent> g_queue;

Generic4_t g_origConsDone = nullptr, g_origConsPass = nullptr, g_origResearch = nullptr, g_origTech = nullptr,
           g_origAddCombat = nullptr, g_origBattleEnd = nullptr, g_origKilled = nullptr, g_origDeploy = nullptr,
           g_origAddUnit = nullptr, g_origHourly = nullptr;

void Fault(const char* where) {
    if (InterlockedExchange(&g_faulted, 1) == 0)
        Log("ERROR: engine_on_actions: fault while reading %s; no more engine on_actions this session", where);
}

bool Key(const EngineEvent& a, const EngineEvent& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.tag != b.tag) return a.tag < b.tag;
    if (a.province != b.province) return a.province < b.province;
    if (a.state != b.state) return a.state < b.state;
    if (a.token != b.token) return a.token < b.token;
    if (a.level != b.level) return a.level < b.level;
    return a.fromTag < b.fromTag;
}

// Fires everything queued (main thread). Events an on_action causes are queued too and fire in
// the same loop, up to a cap against runaway chains.
void Flush() {
    if (g_firing || !g_sink || !IsMainThread()) return;
    g_firing = true;
    int fired = 0;
    for (;;) {
        std::vector<EngineEvent> batch;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            batch.swap(g_queue);
        }
        if (batch.empty()) break;
        std::stable_sort(batch.begin(), batch.end(), Key);
        for (const EngineEvent& ev : batch) {
            if (++fired > 20000) {
                Log("ERROR: engine_on_actions: more than 20000 events at one point (a chain of on_actions?); the rest dropped");
                std::lock_guard<std::mutex> guard(g_lock);
                g_queue.clear();
                g_firing = false;
                return;
            }
            if (g_running) g_sink(ev);
        }
    }
    g_firing = false;
}

void Record(const EngineEvent& ev) {
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_queue.push_back(ev);
    }
    if (g_passDepth == 0 && IsMainThread()) Flush();
}

bool Active() { return g_running && g_sink && !g_faulted; }

// --- Reading the engine's objects (SEH: no C++ objects in these) -----------------------------

int TagOf(const void* country) { return country ? Rd<int>(country, g_tagOffset) : 0; }

bool ReadConstruction(void* construction, void* building, EngineEvent* ev) {
    __try {
        void* country = Rd<void*>(construction, g_consCountry);
        const void* type = Rd<const void*>(building, g_consType);
        void* province = g_buildingProvince(building);
        void* state = g_buildingState(building);
        ev->kind = EngineEventKind::Construction;
        ev->tag = TagOf(country);
        ev->token = type ? Rd<int>(type, 8) : 0;
        ev->province = province ? ProvinceIdOf(province) : 0;
        ev->state = state ? Rd<int>(state, g_stateIdDisp) : 0;
        return ev->tag > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault("a finished construction");
        return false;
    }
}

bool ReadTech(void* status, EngineEvent* ev) {
    __try {
        const void* tmpl = Rd<const void*>(status, g_techTemplate);
        void* owner = Rd<void*>(status, g_techOwner);
        void* country = owner ? Rd<void*>(owner, g_techCountry) : nullptr;
        ev->kind = EngineEventKind::Technology;
        ev->tag = TagOf(country);
        ev->token = tmpl ? Rd<int>(tmpl, 8) : 0;
        ev->level = Rd<int>(status, g_techLevelField);
        return ev->tag > 0 && ev->token;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault("a researched technology");
        return false;
    }
}

int TechLevelOf(void* status) {
    __try {
        return Rd<int>(status, g_techLevelField);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The distinct owner tags of a battle side's divisions, in list order (at most 32).
int SideTags(const void* side, int* tags, int max) {
    int n = 0;
    void* const* units = Rd<void* const*>(side, g_sideUnits);
    int count = Rd<int>(side, g_sideCount);
    if (!units || count < 0 || count > 100000) return 0;
    for (int i = 0; i < count && n < max; ++i) {
        int t = UnitOwnerTag(units[i]);
        if (t <= 0) continue;
        bool seen = false;
        for (int k = 0; k < n; ++k) seen = seen || tags[k] == t;
        if (!seen) tags[n++] = t;
    }
    return n;
}

struct BattleRead {
    int a[32], b[32];
    int na = 0, nb = 0, province = 0;
};

bool ReadBattle(const void* combat, const void* sideA, const void* sideB, BattleRead* r) {
    __try {
        r->na = sideA ? SideTags(sideA, r->a, 32) : 0;
        r->nb = sideB ? SideTags(sideB, r->b, 32) : 0;
        const void* province = Rd<const void*>(combat, g_combatProvince);
        r->province = province ? ProvinceIdOf(province) : 0;
        return r->na > 0 || r->nb > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault("a battle");
        return false;
    }
}

bool ReadSides(const void* combat, const void** a, const void** b) {
    __try {
        *a = Rd<const void*>(combat, g_sideA);
        *b = Rd<const void*>(combat, g_sideA + 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Fault("a battle's sides");
        return false;
    }
}

// Each battle's countries as they were when it started: at its end a side may have no divisions
// left (all retreated or destroyed), and FROM still names the other side.
struct BattleStart {
    const void* sideA;
    const void* sideB;
    BattleRead tags;
};
std::vector<std::pair<const void*, BattleStart>> g_battles;  // under g_lock

void RememberBattle(const void* combat, const void* a, const void* b, const BattleRead& r) {
    std::lock_guard<std::mutex> guard(g_lock);
    for (auto& e : g_battles)
        if (e.first == combat) {
            e.second = BattleStart{a, b, r};
            return;
        }
    g_battles.push_back({combat, BattleStart{a, b, r}});
}

// Fills an empty side of r (winner = a, loser = b) from the battle's start, then forgets it.
void CompleteFromStart(const void* combat, const void* winner, const void* loser, BattleRead* r) {
    std::lock_guard<std::mutex> guard(g_lock);
    for (size_t i = 0; i < g_battles.size(); ++i) {
        if (g_battles[i].first != combat) continue;
        const BattleStart& s = g_battles[i].second;
        auto fill = [&](const void* side, int* tags, int* n) {
            if (*n) return;
            const int* src = side == s.sideA ? s.tags.a : side == s.sideB ? s.tags.b : nullptr;
            int count = side == s.sideA ? s.tags.na : side == s.sideB ? s.tags.nb : 0;
            for (int k = 0; src && k < count; ++k) tags[(*n)++] = src[k];
        };
        fill(winner, r->a, &r->na);
        fill(loser, r->b, &r->nb);
        g_battles.erase(g_battles.begin() + i);
        return;
    }
}

void RecordBattle(EngineEventKind kindA, EngineEventKind kindB, const BattleRead& r) {
    for (int i = 0; i < r.na; ++i) {
        EngineEvent ev;
        ev.kind = kindA;
        ev.tag = r.a[i];
        ev.fromTag = r.nb ? r.b[0] : 0;
        ev.province = r.province;
        Record(ev);
    }
    for (int i = 0; i < r.nb; ++i) {
        EngineEvent ev;
        ev.kind = kindB;
        ev.tag = r.b[i];
        ev.fromTag = r.na ? r.a[0] : 0;
        ev.province = r.province;
        Record(ev);
    }
}

// --- Detours ----------------------------------------------------------------------------------

void* __fastcall ConsDoneDetour(void* construction, void* building, void* c, void* d) {
    void* r = g_origConsDone(construction, building, c, d);
    EngineEvent ev;
    if (Active() && ReadConstruction(construction, building, &ev)) Record(ev);
    return r;
}

void Enter() {
    if (IsMainThread()) ++g_passDepth;
}
void Leave() {
    if (!IsMainThread()) return;
    if (g_passDepth > 0) --g_passDepth;
    Flush();  // the end of a pass: no engine loop of it is running any more
}

void* __fastcall ConsPassDetour(void* a, void* b, void* c, void* d) {
    Enter();
    void* r = g_origConsPass(a, b, c, d);
    Leave();
    return r;
}
void* __fastcall ResearchDetour(void* a, void* b, void* c, void* d) {
    Enter();
    void* r = g_origResearch(a, b, c, d);
    Leave();
    return r;
}
void* __fastcall HourlyDetour(void* a, void* b, void* c, void* d) {
    Enter();
    void* r = g_origHourly(a, b, c, d);
    Leave();
    return r;
}

void* __fastcall TechDetour(void* status, void* level, void* c, void* d) {
    int before = Active() ? TechLevelOf(status) : 0;
    void* r = g_origTech(status, level, c, d);
    EngineEvent ev;
    if (Active() && (int)(intptr_t)level > before && (int)(intptr_t)level > 0 && ReadTech(status, &ev)) Record(ev);
    return r;
}

void* __fastcall AddCombatDetour(void* province, void* combat, void* c, void* d) {
    void* r = g_origAddCombat(province, combat, c, d);
    const void *a = nullptr, *b = nullptr;
    BattleRead br;
    if (Active() && combat && ReadSides(combat, &a, &b) && ReadBattle(combat, a, b, &br)) {
        RememberBattle(combat, a, b, br);
        RecordBattle(EngineEventKind::BattleStarted, EngineEventKind::BattleStarted, br);
    }
    return r;
}

// Before the original: the sides are read while the battle still holds them.
void* __fastcall BattleEndDetour(void* combat, void* loser, void* winner, void* d) {
    BattleRead br;
    bool ok = Active() && combat;
    if (ok) {
        if (!ReadBattle(combat, winner, loser, &br)) br = BattleRead();
        CompleteFromStart(combat, winner, loser, &br);
        ok = br.na > 0 || br.nb > 0;
    }
    void* r = g_origBattleEnd(combat, loser, winner, d);
    if (ok) RecordBattle(EngineEventKind::BattleWon, EngineEventKind::BattleLost, br);
    return r;
}

void* __fastcall KilledDetour(void* side, void* unit, void* c, void* d) {
    EngineEvent ev;
    ev.kind = EngineEventKind::DivisionDestroyed;
    ev.tag = Active() && unit ? UnitOwnerTag(unit) : 0;
    void* r = g_origKilled(side, unit, c, d);
    if (ev.tag > 0) Record(ev);
    return r;
}

void* __fastcall DeployDetour(void* a, void* b, void* c, void* d) {
    ++g_deployDepth;
    void* r = g_origDeploy(a, b, c, d);
    --g_deployDepth;
    return r;
}

void* __fastcall AddUnitDetour(void* country, void* unit, void* c, void* d) {
    void* r = g_origAddUnit(country, unit, c, d);
    if (g_deployDepth > 0 && Active()) {
        EngineEvent ev;
        ev.kind = EngineEventKind::DivisionTrained;
        ev.tag = TagOf(country);
        if (ev.tag > 0) Record(ev);
    }
    return r;
}

}  // namespace

void ResolveEngineEvents() {
    uint8_t* p;
    if ((p = SigFindUnique("ev_scope_state_id", kSigEventScopeState)) != nullptr) g_stateIdDisp = Rd<uint8_t>(p, kSigEventScopeStateIdDisp);
    if ((p = SigFindUnique("ev_construction_done", kSigEvConstructionDone)) != nullptr) {
        g_consDone = p;
        g_consCountry = Rd<uint32_t>(p, kSigEvConsCountryDisp);
        g_consType = Rd<uint32_t>(p, kSigEvConsTypeDisp);
        g_tagOffset = Rd<uint8_t>(p, kSigEvConsTagImm);
    }
    if ((p = SigFindUnique("ev_building_location", kSigEvBuildingLocation)) != nullptr &&
        Rd<uint32_t>(p, kSigEvLocationDisp) == Rd<uint32_t>(p, kSigEvLocationDisp2)) {
        g_buildingProvince = (Getter_t)p;
        g_buildingState = (Getter_t)(p + kSigEvStateGetter);
    }
    if ((p = SigFindUnique("ev_construction_pass", kSigEvConstructionPass)) != nullptr) g_consPass = p - kSigEvConstructionPassStart;
    if ((p = SigFindUnique("ev_research_update", kSigEvResearchUpdate)) != nullptr) g_researchUpdate = p - kSigEvResearchUpdateStart;
    if ((p = SigFindUnique("ev_tech_level", kSigEvTechLevel)) != nullptr &&
        Rd<uint32_t>(p, kSigEvTechLevelDisp) == Rd<uint32_t>(p, kSigEvTechLevelDisp2)) {
        g_techLevel = p;
        g_techTemplate = Rd<uint32_t>(p, kSigEvTechTemplateDisp);
        g_techLevelField = Rd<uint32_t>(p, kSigEvTechLevelDisp);
    }
    if ((p = SigFindUnique("ev_tech_owner", kSigEvTechOwner)) != nullptr) {
        g_techOwner = Rd<uint32_t>(p, kSigEvTechOwnerDisp);
        g_techCountry = Rd<uint32_t>(p, kSigEvTechCountryDisp);
    }
    uint8_t* sides = SigFindUnique("ev_battle_sides", kSigEvBattleSides);
    uint8_t* loc = SigFindUnique("ev_combat_location", kSigTerrainCombatLocation);
    if (sides && loc) {
        int adjust = -(int)(int8_t)Rd<uint8_t>(sides, kSigEvSideBaseDisp);  // rsi = battle + adjust
        g_sideA = Rd<uint8_t>(sides, kSigEvSideDisp) + adjust;
        g_sideUnits = Rd<uint8_t>(sides, kSigEvSideUnitsDisp);
        g_sideCount = Rd<uint8_t>(sides, kSigEvSideCountDisp);
        g_combatProvince = Rd<uint8_t>(loc, kSigTerrainCombatProvince);
        if (adjust <= 0 || g_combatProvince != g_sideA + 0x10) {
            Log("ERROR: engine_on_actions: battle layout looks different (sides +0x%x, province +0x%x); no battle events",
                g_sideA, g_combatProvince);
            g_sideA = 0;
        }
    }
    g_addCombat = SigFindUnique("ev_province_add_combat", kSigProvinceAddCombat);
    g_battleEnd = SigFindUnique("ev_battle_end", kSigEvBattleEnd);
    g_divisionKilled = SigFindUnique("ev_division_killed", kSigEvDivisionKilled);
    if ((p = SigFindUnique("ev_deploy", kSigEvDeploy)) != nullptr) g_deploy = p - kSigEvDeployStart;
    g_addUnit = SigFindUnique("ev_add_unit", kSigEvAddUnit);
    size_t slots = 0;
    void** vt = FindVtableByRtti(".?AVCHourlyTickCommand@@", &slots);
    if (vt && slots > 10) g_hourlyTick = (uint8_t*)vt[10];
    Log("Engine events: construction %s (country +0x%x, type +0x%x, tag +0x%x), tech %s (template +0x%x, level +0x%x, owner "
        "+0x%x/+0x%x), battles: sides +0x%x (units +0x%x/+0x%x), province +0x%x; hourly tick %s; state id +0x%x",
        g_consDone ? "yes" : "no", g_consCountry, g_consType, g_tagOffset, g_techLevel ? "yes" : "no", g_techTemplate,
        g_techLevelField, g_techOwner, g_techCountry, g_sideA, g_sideUnits, g_sideCount, g_combatProvince,
        g_hourlyTick ? DescribeAddress(g_hourlyTick).c_str() : "-", g_stateIdDisp);
}

bool HaveEngineEvent(EngineEventKind kind) {
    if (!g_hourlyTick || !g_tagOffset || !HaveOnActions()) return false;
    switch (kind) {
        case EngineEventKind::Construction:
            return g_consDone && g_consPass && g_buildingProvince && g_buildingState && g_stateIdDisp && HaveStateScopes() &&
                   HaveProvinceIds();
        case EngineEventKind::Technology:
            return g_techLevel && g_researchUpdate && g_techOwner && g_techCountry;
        case EngineEventKind::BattleStarted:
            return g_addCombat && g_sideA && HaveMilitaryCounts() && HaveProvinceIds();
        case EngineEventKind::BattleWon:
        case EngineEventKind::BattleLost:
            return g_battleEnd && g_sideA && HaveMilitaryCounts() && HaveProvinceIds();
        case EngineEventKind::DivisionTrained:
            return g_deploy && g_addUnit;
        case EngineEventKind::DivisionDestroyed:
            return g_divisionKilled && HaveMilitaryCounts();
        default:
            return false;
    }
}

bool HookEngineEvents(EngineEventSink_t sink) {
    g_sink = sink;
    bool ok = HookQueue(g_hourlyTick, (void*)&HourlyDetour, (void**)&g_origHourly, "ev_hourly_tick");
    auto hook = [&](EngineEventKind k, void* target, void* detour, void** orig, const char* name) {
        if (HaveEngineEvent(k) && target) ok = HookQueue(target, detour, orig, name) && ok;
    };
    if (HaveEngineEvent(EngineEventKind::Construction)) {
        hook(EngineEventKind::Construction, g_consDone, (void*)&ConsDoneDetour, (void**)&g_origConsDone, "ev_construction_done");
        hook(EngineEventKind::Construction, g_consPass, (void*)&ConsPassDetour, (void**)&g_origConsPass, "ev_construction_pass");
    }
    if (HaveEngineEvent(EngineEventKind::Technology)) {
        hook(EngineEventKind::Technology, g_techLevel, (void*)&TechDetour, (void**)&g_origTech, "ev_tech_level");
        hook(EngineEventKind::Technology, g_researchUpdate, (void*)&ResearchDetour, (void**)&g_origResearch, "ev_research_update");
    }
    hook(EngineEventKind::BattleStarted, g_addCombat, (void*)&AddCombatDetour, (void**)&g_origAddCombat, "ev_add_combat");
    hook(EngineEventKind::BattleWon, g_battleEnd, (void*)&BattleEndDetour, (void**)&g_origBattleEnd, "ev_battle_end");
    hook(EngineEventKind::DivisionDestroyed, g_divisionKilled, (void*)&KilledDetour, (void**)&g_origKilled, "ev_division_killed");
    if (HaveEngineEvent(EngineEventKind::DivisionTrained)) {
        hook(EngineEventKind::DivisionTrained, g_deploy, (void*)&DeployDetour, (void**)&g_origDeploy, "ev_deploy");
        hook(EngineEventKind::DivisionTrained, g_addUnit, (void*)&AddUnitDetour, (void**)&g_origAddUnit, "ev_add_unit");
    }
    return ok;
}

void SetEngineEventsRunning(bool running) {
    g_running = running;
    if (!running) {
        std::lock_guard<std::mutex> guard(g_lock);
        g_queue.clear();
        g_battles.clear();
        g_passDepth = 0;
        g_deployDepth = 0;
    }
}

}  // namespace Engine
