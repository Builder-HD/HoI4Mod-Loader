// Engine access for province_terrain (NOTES.md Part P): a province's terrain category and what the
// game keeps derived from it. The category is the CTerrainType* in the province's static map
// province (map/definition.csv, loaded once at start); the game also keeps a terrain mask per
// state (the OR of its provinces' bits) and per country (the OR of its owned states' masks, read by
// has_terrain), a copy in every battle (taken when the battle starts) and the supply system's
// per-province data. A change here updates all of them. Main thread only: the game changes this
// state on its main thread, and effects run there in serial code.
#include <windows.h>
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

using DbGetter_t = void*(__fastcall*)();

DbGetter_t g_db = nullptr;                       // CTerrainDatabase getter
uint32_t g_dbCount = 0, g_dbArray = 0;           // db -> count, CTerrainType* array (index 0: default)
uint32_t g_token = 0, g_valid = 0, g_bits = 0;  // CTerrainType -> name token, valid byte, mask bits
uint32_t g_terrain = 0;                         // map province -> CTerrainType*
uint32_t g_provMap = 0, g_provState = 0;        // CProvince -> map province, state
uint32_t g_combatProv = 0, g_combatTerrain = 0;  // CCombat -> province, terrain copy
uint32_t g_stateMask = 0;                        // state -> terrain mask
uint32_t g_countryMask = 0, g_countryStates = 0, g_countryStateCount = 0;  // country -> mask, owned states
uint32_t g_waterBit = 0;                         // found per game (TerrainSelfCheck)

bool Fail(const char* why) {
    Log("Province terrain: %s; province_terrain off", why);
    return false;
}

void* Db() { return g_db ? g_db() : nullptr; }

// The categories, index 0 (the default) included. Empty without a database.
void* const* Categories(int* n) {
    *n = 0;
    void* db = Db();
    if (!db) return nullptr;
    void* const* arr = Rd<void* const*>(db, g_dbArray);
    int count = Rd<int>(db, g_dbCount);
    if (!arr || count <= 0 || count > 4096) return nullptr;
    *n = count;
    return arr;
}

uint32_t BitsOf(const void* terrain) { return terrain ? Rd<uint32_t>(terrain, g_bits) : 0; }

const void* TerrainOfMap(const void* map) { return map ? Rd<const void*>(map, g_terrain) : nullptr; }

// The OR of the bits of the state's provinces' categories (what the game builds when provinces join).
uint32_t StateMaskNow(const void* state) {
    void* const* provs = nullptr;
    int n = 0;
    uint32_t mask = 0;
    if (!StateObjectProvinces(state, &provs, &n)) return 0;
    for (int i = 0; i < n; ++i) {
        const void* p = provs[i];
        if (p) mask |= BitsOf(TerrainOfMap(Rd<const void*>(p, g_provMap)));
    }
    return mask;
}

uint32_t CountryMaskNow(const void* country) {
    void* const* states = Rd<void* const*>(country, g_countryStates);
    int n = Rd<int>(country, g_countryStateCount);
    uint32_t mask = 0;
    if (!states || n < 0 || n > 100000) return 0;
    for (int i = 0; i < n; ++i)
        if (states[i]) mask |= Rd<uint32_t>(states[i], g_stateMask);
    return mask;
}

// No C++ objects: SEH contains a fault.
bool SetProvinceTerrainRaw(int id, const void* terrain, bool* faulted, int* battles) {
    *faulted = false;
    *battles = 0;
    __try {
        void* p = ProvinceById(id);
        void* map = p ? Rd<void*>(p, g_provMap) : nullptr;
        if (!map || map != MapProvinceById(id)) return false;
        Wr<const void*>(map, g_terrain, terrain);
        void* state = Rd<void*>(p, g_provState);
        if (state) {
            Wr<uint32_t>(state, g_stateMask, StateMaskNow(state));
            void* owner = CountryByTag(StateObjectOwner(state));
            if (owner) Wr<uint32_t>(owner, g_countryMask, CountryMaskNow(owner));
        }
        void* const* combats = nullptr;
        int n = 0;
        if (ProvinceCombatObjects(p, &combats, &n))
            for (int i = 0; i < n; ++i)
                if (combats[i] && Rd<void*>(combats[i], g_combatProv) == p) {
                    Wr<const void*>(combats[i], g_combatTerrain, terrain);
                    ++*battles;
                }
        SupplyProvinceChanged(id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
        return false;
    }
}

}  // namespace

void ResolveTerrain() {
    uint8_t* combat = SigFindUnique("terrain_combat_location", kSigTerrainCombatLocation);
    uint8_t* provState = SigFindUnique("terrain_province_state", kSigTerrainProvinceState);
    uint8_t* lookup = SigFindUnique("terrain_lookup", kSigTerrainLookup);
    uint8_t* hasEval = SigFindUnique("terrain_has_eval", kSigTerrainHasEval);
    uint8_t* rebuild = SigFindUnique("terrain_country_rebuild", kSigTerrainCountryRebuild);
    if (!combat || !provState || !lookup || !hasEval || !rebuild) return;
    uint32_t map = Rd<uint32_t>(combat, kSigTerrainCombatMap), terrain = Rd<uint32_t>(combat, kSigTerrainCombatTerrain);
    if (Rd<uint32_t>(provState, kSigTerrainStateMap) != map || Rd<uint32_t>(provState, kSigTerrainStateTerrain) != terrain) {
        Fail("the battle and the supply code read different province terrain fields");
        return;
    }
    if (Rd<uint8_t>(lookup, kSigTerrainValid) != Rd<uint8_t>(hasEval, kSigTerrainHasValid) ||
        Rd<uint32_t>(hasEval, kSigTerrainCountryMask) != Rd<uint32_t>(rebuild, kSigTerrainCountryMask2)) {
        Fail("has_terrain's parser, evaluate and the mask rebuild disagree");
        return;
    }
    if (map >= 0x1000 || terrain >= 0x1000 || Rd<uint32_t>(provState, kSigTerrainStateDisp) >= 0x1000 ||
        Rd<uint32_t>(hasEval, kSigTerrainBits) >= 0x1000 || Rd<uint32_t>(rebuild, kSigTerrainStateMask) >= 0x4000) {
        Fail("implausible offsets");
        return;
    }
    g_provMap = map;
    g_terrain = terrain;
    g_provState = Rd<uint32_t>(provState, kSigTerrainStateDisp);
    g_combatProv = Rd<uint8_t>(combat, kSigTerrainCombatProvince);
    g_combatTerrain = Rd<uint8_t>(combat, kSigTerrainCombatCopy);
    g_dbCount = Rd<uint8_t>(lookup, kSigTerrainDbCount);
    g_dbArray = Rd<uint8_t>(lookup, kSigTerrainDbArray);
    g_token = Rd<uint8_t>(lookup, kSigTerrainToken);
    g_valid = Rd<uint8_t>(lookup, kSigTerrainValid);
    g_db = (DbGetter_t)SigFollowRel32(lookup + kSigTerrainDbGetter);
    g_bits = Rd<uint32_t>(hasEval, kSigTerrainBits);
    g_countryMask = Rd<uint32_t>(hasEval, kSigTerrainCountryMask);
    g_countryStates = Rd<uint32_t>(rebuild, kSigTerrainCountryStates);
    g_countryStateCount = Rd<uint32_t>(rebuild, kSigTerrainCountryStateCount);
    g_stateMask = Rd<uint32_t>(rebuild, kSigTerrainStateMask);
    Log("Province terrain: province -> map +0x%x -> terrain +0x%x, state +0x%x; terrain: token +0x%x, valid +0x%x, bits "
        "+0x%x; database %s (+0x%x/+0x%x); masks: state +0x%x, country +0x%x (states +0x%x/+0x%x); battle: province +0x%x, "
        "terrain +0x%x",
        g_provMap, g_terrain, g_provState, g_token, g_valid, g_bits, DescribeAddress((void*)g_db).c_str(), g_dbArray, g_dbCount,
        g_stateMask, g_countryMask, g_countryStates, g_countryStateCount, g_combatProv, g_combatTerrain);
}

bool HaveTerrain() {
    return g_db && g_dbCount && g_dbArray && g_token && g_valid && g_bits && g_terrain && g_provMap && g_provState &&
           g_combatProv && g_combatTerrain && g_stateMask && g_countryMask && g_countryStates && g_countryStateCount &&
           HaveProvinceData() && HaveMilitaryCounts() && HaveSupplyNotify();
}

bool TerrainSelfCheck(const std::vector<int>& skip, std::string* problem) {
    g_waterBit = 0;
    if (!HaveTerrain()) {
        *problem = "not resolved";
        return false;
    }
    int n = 0;
    void* const* cats = Categories(&n);
    if (!cats || n < 2) {
        *problem = "no terrain categories";
        return false;
    }
    // Sea provinces belong to no state; the bits every one of their categories has is is_water.
    int provinces = ProvinceCount(), sea = 0, land = 0;
    uint32_t common = ~0u;
    for (int id = 1; id < provinces; ++id) {
        void* p = ProvinceById(id);
        void* map = p ? Rd<void*>(p, g_provMap) : nullptr;
        if (!map) continue;
        if (map != MapProvinceById(id)) {
            *problem = "a province's map province is not its static map entry (province " + std::to_string(id) + ")";
            return false;
        }
        const void* t = TerrainOfMap(map);
        if (Rd<void*>(p, g_provState)) {
            ++land;
        } else if (t) {
            ++sea;
            common &= BitsOf(t);
        }
    }
    if (sea < 10 || land < 10 || !common || (common & (common - 1))) {
        char buf[96];
        snprintf(buf, sizeof buf, "no single water bit (%d sea and %d land provinces, common bits 0x%x)", sea, land,
                 sea ? common : 0);
        *problem = buf;
        return false;
    }
    // Every state's mask is the OR of its provinces' bits (the masks this feature keeps up to date),
    // except the states of restored provinces: the game built them before the restore.
    std::vector<void*> skipStates;
    for (int id : skip) {
        void* p = ProvinceById(id);
        if (void* st = p ? Rd<void*>(p, g_provState) : nullptr) skipStates.push_back(st);
    }
    int states = StateCount(), wrong = 0, checked = 0;
    for (int s = 1; s < states; ++s) {
        void* st = StateById(s);
        if (!st || std::find(skipStates.begin(), skipStates.end(), st) != skipStates.end()) continue;
        ++checked;
        if (Rd<uint32_t>(st, g_stateMask) != StateMaskNow(st)) ++wrong;
    }
    if (!checked || wrong) {
        *problem = std::to_string(wrong) + " of " + std::to_string(checked) + " state terrain masks differ from their provinces";
        return false;
    }
    g_waterBit = common;
    Log("Province terrain: %d categories, water bit 0x%x (%d sea and %d land provinces), %d state masks match", n - 1, g_waterBit,
        sea, land, checked);
    return true;
}

const void* TerrainByToken(int token) {
    int n = 0;
    void* const* cats = token ? Categories(&n) : nullptr;
    for (int i = 1; i < n; ++i)
        if (cats[i] && Rd<int>(cats[i], g_token) == token && Rd<uint8_t>(cats[i], g_valid)) return cats[i];
    return nullptr;
}

int TerrainToken(const void* terrain) { return terrain && g_token ? Rd<int>(terrain, g_token) : 0; }

bool TerrainIsWater(const void* terrain) { return g_waterBit && (BitsOf(terrain) & g_waterBit) != 0; }

const void* ProvinceTerrain(int province) { return g_terrain ? TerrainOfMap(MapProvinceById(province)) : nullptr; }

bool SetMapTerrain(int province, const void* terrain) {
    void* map = g_terrain && terrain ? MapProvinceById(province) : nullptr;
    if (!map) return false;
    Wr<const void*>(map, g_terrain, terrain);
    return true;
}

bool SetProvinceTerrain(int province, const void* terrain, int* battles) {
    if (!HaveTerrain() || !terrain) return false;
    bool faulted = false;
    int n = 0;
    bool ok = SetProvinceTerrainRaw(province, terrain, &faulted, &n);
    if (battles) *battles += n;
    if (faulted) Log("ERROR: Province terrain: fault while changing province %d", province);
    return ok;
}

bool StateProvinceIds(int stateId, std::vector<int>* out) {
    out->clear();
    void* const* provs = nullptr;
    int n = 0;
    if (!StateObjectProvinces(StateById(stateId), &provs, &n)) return false;
    for (int i = 0; i < n; ++i)
        if (int id = ProvinceIdOf(provs[i])) out->push_back(id);
    return true;
}

bool StateHasTerrain(int stateId, const void* terrain) {
    void* const* provs = nullptr;
    int n = 0;
    if (!terrain || !g_provMap || !StateObjectProvinces(StateById(stateId), &provs, &n)) return false;
    for (int i = 0; i < n; ++i)
        if (provs[i] && TerrainOfMap(Rd<const void*>(provs[i], g_provMap)) == terrain) return true;
    return false;
}

}  // namespace Engine
