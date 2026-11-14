// Area selection, see area_select.h.
#include "area_select.h"

#include <stdint.h>

#include <algorithm>

#include "engine.h"

namespace AreaSelect {

namespace {

enum Key { kProvince, kState, kRegion, kOwner, kController, kCoastal, kRandomProvinces };

ParamSpec Repeat(const char* name) {
    ParamSpec p{name, ParamType::Int, false};
    p.repeat = true;
    return p;
}

ParamSpec RepeatState() {
    ParamSpec p = Repeat("state");
    p.choices = "this";  // the state in scope (random_owned_state = { add_attention_area = { ... state = this } })
    return p;
}

const std::vector<ParamSpec> kKeys = {
    Repeat("province"),
    RepeatState(),
    Repeat("strategic_region"),
    {"owner", ParamType::Country, false},
    {"controller", ParamType::Country, false},
    {"coastal", ParamType::Bool, false},
    {"random_provinces", ParamType::Int, false},  // keep this many of the block's provinces, picked at random
};

// The random picks of random_provinces must come out the same on every machine in multiplayer, so
// they come from a counter of Select calls, not from the clock. Nothing selected is saved, so a fresh
// session needs no stored state.
uint64_t g_selectCount = 0;
uint64_t Mix(uint64_t x) {  // splitmix64
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// province -> state id (0: not in a state, i.e. sea or wasteland outside states).
std::vector<int> ProvinceStates() {
    std::vector<int> of((size_t)std::max(Engine::ProvinceCount(), 0), 0);
    std::vector<void*> list;
    for (int s = 1; s < Engine::StateCount(); ++s) {
        if (!Engine::AreaProvinceObjects(Engine::AreaKind::State, s, &list)) continue;
        for (void* p : list) {
            int id = Engine::ProvinceIdOf(p);
            if (id > 0 && id < (int)of.size()) of[id] = s;
        }
    }
    return of;
}

void AddIds(Engine::AreaKind kind, int id, std::vector<char>* in) {
    std::vector<void*> list;
    if (!Engine::AreaProvinceObjects(kind, id, &list)) return;
    for (void* p : list) {
        int pid = Engine::ProvinceIdOf(p);
        if (pid > 0 && pid < (int)in->size()) (*in)[pid] = 1;
    }
}

// The provinces one area block selects, added to *out (a flag per province id).
void SelectBlock(const Args& a, const Scope& scope, const std::vector<int>& stateOf, std::vector<char>* out) {
    size_t n = stateOf.size();
    std::vector<char> in(n, 0);
    bool listed = a.has(kProvince) || a.has(kState) || a.has(kRegion);
    if (!listed) {
        for (size_t p = 1; p < n; ++p) in[p] = stateOf[p] != 0;
    } else {
        for (size_t k = 0; k < a.count(kProvince); ++k) {
            int p = a.at(kProvince, k);
            if (p > 0 && p < (int)n) in[p] = 1;
        }
        for (size_t k = 0; k < a.count(kState); ++k) {
            int st = a.at(kState, k);
            if (IntWordIndex(st) == 0) st = scope.stateId();  // `this`: 0 outside a state scope, which selects nothing
            if (st > 0) AddIds(Engine::AreaKind::State, st, &in);
        }
        for (size_t k = 0; k < a.count(kRegion); ++k) AddIds(Engine::AreaKind::StrategicRegion, a.at(kRegion, k), &in);
    }
    int owner = a.has(kOwner) ? native_country(a.i(kOwner), scope) : 0;
    int controller = a.has(kController) ? native_country(a.i(kController), scope) : 0;
    if ((a.has(kOwner) && owner <= 0) || (a.has(kController) && controller <= 0)) return;  // names no country here
    std::vector<int> kept;
    for (size_t p = 1; p < n; ++p) {
        if (!in[p]) continue;
        if (owner && !Engine::IsSameCountry(Engine::StateOwner(stateOf[p]), owner)) continue;
        if (controller && !Engine::IsSameCountry(Engine::ProvinceController((int)p), controller)) continue;
        if (a.has(kCoastal) && Engine::ProvinceIsCoastal((int)p) != a.b(kCoastal)) continue;
        kept.push_back((int)p);
    }
    if (a.has(kRandomProvinces)) {  // a partial Fisher-Yates shuffle: the first `want` are the picks
        size_t want = (size_t)std::max(0, a.i(kRandomProvinces));
        uint64_t r = Mix(g_selectCount * 0x100000001B3ull + kept.size());
        for (size_t i = 0; i < want && i + 1 < kept.size(); ++i) {
            r = Mix(r);
            std::swap(kept[i], kept[i + (size_t)(r % (kept.size() - i))]);
        }
        if (want < kept.size()) kept.resize(want);
    }
    for (int p : kept) (*out)[p] = 1;
}

}  // namespace

const std::vector<ParamSpec>& Keys() { return kKeys; }

ParamSpec BlockParam(const char* name, bool required) {
    ParamSpec p{name, ParamType::Block, required};
    p.repeat = true;
    p.block = &kKeys;
    return p;
}

bool Available() { return Engine::HaveProvinceData() && Engine::HaveRegionData() && Engine::HaveCountries(); }

std::vector<int> Select(const Args& args, size_t index, const Scope& scope) {
    ++g_selectCount;
    std::vector<int> stateOf = ProvinceStates();
    std::vector<char> pick(stateOf.size(), 0);
    for (size_t k = 0; k < args.count(index); ++k) SelectBlock(args.block(index, k), scope, stateOf, &pick);
    std::vector<int> out;
    for (size_t p = 1; p < pick.size(); ++p)
        if (pick[p]) out.push_back((int)p);
    return out;
}

void ResetSession() { g_selectCount = 0; }

}  // namespace AreaSelect
