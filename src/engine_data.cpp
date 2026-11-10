// Engine access for the data-trigger features (NOTES.md Part E): read-only views of game state
// that the engine computes anyway. Everything here may run on any thread (native trigger
// evaluate): plain reads and calls into engine getters that only read; no allocation, no logging
// on the hot path. Resolved once from src/signatures.h.
#include <ctype.h>
#include <string.h>

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

using TagToCountry_t = void*(__fastcall*)(const int* tag);
using TagIndex_t = int(__fastcall*)(const int* tag);
using CountryAir_t = void*(__fastcall*)(void* country);
using AirRegionEntry_t = const int64_t*(__fastcall*)(void* countryAir, void* region);
using IsFriendly_t = bool(__fastcall*)(void* relation);

TagToCountry_t g_tagToCountry = nullptr;
TagIndex_t g_tagIndex = nullptr;
CountryAir_t g_countryAir = nullptr;
AirRegionEntry_t g_airRegionEntry = nullptr;
IsFriendly_t g_isFriendly = nullptr;
using TagLookup_t = int(__fastcall*)(void* db, const EngineString* text);
void* const* g_tagDb = nullptr;  // CCountryTag database global (null until loaded)
TagLookup_t g_tagLookup = nullptr;
uint32_t g_gameDate = 0;           // CGameState -> current date (hours)
uint32_t g_regionNavalOffset = 0;  // CStrategicRegion -> naval dominance object
uint32_t g_diplomacyOffset = 0;    // CCountry -> diplomacy (relations at +8, indexed by tag index)
uint32_t g_relationWarOffset = 0;  // relation -> CWar* (0 = no war)
uint32_t g_regionSeaShare = 0;     // CStrategicRegion -> share of sea provinces (fixed point)

// Layout constants inside matched signatures (a change breaks the match):
const size_t kNavalEntries = 0x40, kNavalCount = 0x4c, kNavalEntrySize = 0x50, kNavalValue = 0x20;  // kSigNavalSums
const size_t kRelations = 0x08, kWarEnded = 0x49;                                                   // kSigNavalSums

using GetNameKey_t = const EngineString*(__fastcall*)(void* region);

// military_counts / army_condition (kSigStateArray ... kSigPlanesStationed, kAnchorArmyDaily).
using ShipsOf_t = const uint8_t*(__fastcall*)(void* navalUnit);  // -> { ships*, ?, count @+0xc }
using WingRegion_t = void*(__fastcall*)(void* wing);
using WingPlanes_t = int(__fastcall*)(void* wing);
uint32_t g_stateArray = 0, g_stateCount = 0;
uint32_t g_provinces = 0, g_provinceCount = 0;    // state / region -> CProvince* list
uint32_t g_landUnits = 0, g_landUnitCount = 0;    // province -> land units
uint32_t g_navalUnits = 0, g_navalUnitCount = 0;  // province -> naval units
uint32_t g_unitOwner = 0;                         // unit -> owner tag
ShipsOf_t g_shipsOf = nullptr;
const size_t kShipCount = 0x0c;                   // kSigShipsInArea: movsxd r8,[rax+0xc]
uint32_t g_provinceCombats = 0, g_provinceCombatCount = 0;
uint32_t g_unitCombats = 0, g_unitCombatCount = 0;
uint32_t g_countryDivisions = 0, g_countryDivisionCount = 0;
CountryAir_t g_countryAirPlanes = nullptr;
WingRegion_t g_wingRegion = nullptr;
WingPlanes_t g_wingPlanes = nullptr;
uint32_t g_airWings = 0, g_airWingCount = 0, g_regionIdField = 0;

// A CPdxArray-style list { T* data @list, int count @count } of pointers.
struct PtrList {
    void* const* data = nullptr;
    int count = 0;
};
PtrList ListAt(const void* obj, uint32_t list, uint32_t count) {
    PtrList l;
    if (!obj) return l;
    l.data = Rd<void* const*>(obj, list);
    l.count = Rd<int>(obj, count);
    if (!l.data || l.count < 0 || l.count > 1000000) l.count = 0;
    return l;
}

}  // namespace

void ResolveData() {
    uint8_t* p;
    TagToCountry_t tagToCountryAir = nullptr;
    if ((p = SigFindUnique("air_superiority_ui", kSigAirSuperiorityUi)) != nullptr) {
        tagToCountryAir = (TagToCountry_t)SigFollowRel32(p + kSigAirUiTagToCountry);
        g_countryAir = (CountryAir_t)SigFollowRel32(p + kSigAirUiCountryAir);
        g_airRegionEntry = (AirRegionEntry_t)SigFollowRel32(p + kSigAirUiRegionEntry);
    }
    if ((p = SigFindUnique("country_tag_from_text", kSigCountryTagFromText)) != nullptr) {
        g_tagDb = (void* const*)SigRipTarget(p + kSigTagDbInsn, 3, 7);
        g_tagLookup = (TagLookup_t)SigFollowRel32(p + kSigTagLookupCall);
    }
    if ((p = SigFindUnique("naval_control_eval", kSigNavalControlEval)) != nullptr)
        g_regionNavalOffset = Rd<uint32_t>(p, kSigNavalRegionDisp);
    if ((p = SigFindUnique("naval_sums", kSigNavalSums)) != nullptr) {
        g_tagToCountry = (TagToCountry_t)SigFollowRel32(p + kSigNavalSumsTagToCountry);
        g_tagIndex = (TagIndex_t)SigFollowRel32(p + kSigNavalSumsTagIndex);
        g_isFriendly = (IsFriendly_t)SigFollowRel32(p + kSigNavalSumsIsFriendly);
        g_diplomacyOffset = Rd<uint32_t>(p, kSigNavalSumsDiplomacyDisp);
        g_relationWarOffset = Rd<uint32_t>(p, kSigNavalSumsWarDisp);
    }
    // Cross-check: both signatures must reach the same tag -> country function.
    if (tagToCountryAir && g_tagToCountry && tagToCountryAir != (TagToCountry_t)g_tagToCountry) {
        Log("ERROR: data triggers: the two tag->country call sites disagree; region_control disabled");
        g_countryAir = nullptr;
        g_tagToCountry = nullptr;
    }
    if (HaveRegionControl())
        Log("Data triggers: region naval +0x%x, diplomacy +0x%x, relation war +0x%x", g_regionNavalOffset, g_diplomacyOffset,
            g_relationWarOffset);

    // military_counts. The state and the region loops must agree on the province list layout,
    // and both unit loops on the owner field.
    uint32_t provinces2 = 0, provinceCount2 = 0, owner2 = 0;
    if ((p = SigFindUnique("state_array", kSigStateArray)) != nullptr) {
        g_stateCount = Rd<uint32_t>(p, kSigStateCountDisp);
        g_stateArray = Rd<uint32_t>(p, kSigStateArrayDisp);
    }
    if ((p = SigFindUnique("state_land_units", kSigStateLandUnits)) != nullptr) {
        g_provinces = Rd<uint8_t>(p, kSigLandProvincesDisp);
        g_provinceCount = Rd<uint8_t>(p, kSigLandProvinceCountDisp);
        g_landUnits = Rd<uint32_t>(p, kSigLandUnitsDisp);
        g_landUnitCount = Rd<uint32_t>(p, kSigLandUnitCountDisp);
        g_unitOwner = Rd<uint32_t>(p, kSigLandOwnerDisp);
    }
    if ((p = SigFindUnique("ships_in_area", kSigShipsInArea)) != nullptr) {
        provinces2 = Rd<uint8_t>(p, kSigShipsProvincesDisp);
        provinceCount2 = Rd<uint8_t>(p, kSigShipsProvinceCountDisp);
        g_navalUnits = Rd<uint32_t>(p, kSigShipsNavalUnitsDisp);
        g_navalUnitCount = Rd<uint32_t>(p, kSigShipsNavalCountDisp);
        owner2 = Rd<uint32_t>(p, kSigShipsOwnerDisp);
        g_shipsOf = (ShipsOf_t)SigFollowRel32(p + kSigShipsShipsCall);
    }
    if ((p = SigFindUnique("province_add_combat", kSigProvinceAddCombat)) != nullptr) {
        g_provinceCombatCount = Rd<uint32_t>(p, kSigProvinceCombatCountDisp);
        g_provinceCombats = Rd<uint32_t>(p, kSigProvinceCombatsDisp);
    }
    if ((p = SigFindUnique("unit_add_combat", kSigUnitAddCombat)) != nullptr) {
        g_unitCombatCount = Rd<uint32_t>(p, kSigUnitCombatCountDisp);
        g_unitCombats = Rd<uint32_t>(p, kSigUnitCombatsDisp);
    }
    if ((p = SigFindUnique("planes_stationed", kSigPlanesStationed)) != nullptr) {
        g_countryAirPlanes = (CountryAir_t)SigFollowRel32(p + kSigPlanesCountryAir);
        g_airWings = Rd<uint8_t>(p, kSigPlanesWingsDisp);
        g_airWingCount = Rd<uint8_t>(p, kSigPlanesWingCountDisp);
        g_wingRegion = (WingRegion_t)SigFollowRel32(p + kSigPlanesWingRegion);
        g_regionIdField = Rd<uint8_t>(p, kSigPlanesRegionIdDisp);
        g_wingPlanes = (WingPlanes_t)SigFollowRel32(p + kSigPlanesWingPlanes);
        if ((TagToCountry_t)SigFollowRel32(p + kSigPlanesTagToCountry) != g_tagToCountry || g_countryAirPlanes != g_countryAir) {
            Log("ERROR: data triggers: the planes call site reaches different country functions; planes unavailable");
            g_wingPlanes = nullptr;
        }
    }
    if (uint8_t* lea = AnchorFindLea("army.daily", kAnchorArmyDaily)) {
        if (SigMatchAt(lea + kArmyDailyLoopFromLea, kCodeArmyDailyLoop)) {
            g_countryDivisions = Rd<uint32_t>(lea, kArmyDailyListDisp);
            g_countryDivisionCount = Rd<uint32_t>(lea, kArmyDailyCountDisp);
        } else {
            Log("ERROR: data triggers: the army.daily loop has an unexpected shape");
        }
    }
    if (g_provinces && (g_provinces != provinces2 || g_provinceCount != provinceCount2 || g_unitOwner != owner2)) {
        Log("ERROR: data triggers: state and region loops disagree on the layout; military_counts disabled");
        g_provinces = 0;
    }
    if ((p = SigFindUnique("region_sea_share", kSigRegionSeaShare)) != nullptr)
        g_regionSeaShare = Rd<uint32_t>(p, kSigRegionSeaShareDisp);
    ResolveArmyCondition();
    ResolveProduction();
    if ((p = SigFindUnique("game_date", kSigGameDate)) != nullptr) g_gameDate = Rd<uint32_t>(p, kSigGameDateDisp);
    if (HaveMilitaryCounts())
        Log("Data triggers: states +0x%x/+0x%x, provinces +0x%x/+0x%x, land units +0x%x/+0x%x, naval units +0x%x/+0x%x, "
            "owner +0x%x, combats province +0x%x unit +0x%x, divisions +0x%x/+0x%x",
            g_stateArray, g_stateCount, g_provinces, g_provinceCount, g_landUnits, g_landUnitCount, g_navalUnits,
            g_navalUnitCount, g_unitOwner, g_provinceCombats, g_unitCombats, g_countryDivisions, g_countryDivisionCount);
}

bool HaveMilitaryCounts() {
    return HaveCountries() && HaveRegions() && g_stateArray && g_stateCount && g_provinces && g_provinceCount && g_landUnits &&
           g_landUnitCount && g_navalUnits && g_navalUnitCount && g_unitOwner && g_shipsOf && g_provinceCombats &&
           g_provinceCombatCount && g_unitCombats && g_unitCombatCount && g_countryDivisions && g_countryDivisionCount &&
           g_relationWarOffset;
}
bool HavePlanes() { return HaveMilitaryCounts() && g_countryAirPlanes && g_wingRegion && g_wingPlanes && g_airWings; }

void* StateById(int id) {
    void* gs = GameState();
    if (!gs || id <= 0 || !g_stateArray) return nullptr;
    int count = Rd<int>(gs, g_stateCount);
    void* const* arr = Rd<void* const*>(gs, g_stateArray);
    return arr && id < count ? arr[id] : nullptr;
}

static bool SameCountry(int a, int b) {
    if (a == b) return a > 0;
    if (a <= 0 || b <= 0) return false;
    return g_tagIndex(&a) == g_tagIndex(&b);  // the engine's own "same country" test (FUN_140bb52f0)
}

static void* Relation(int tagA, int tagB);

bool HaveAlliance() { return g_isFriendly != nullptr; }

static bool AlliedWith(int owner, int tag) {
    if (!g_isFriendly || SameCountry(owner, tag)) return false;
    void* r = Relation(owner, tag);  // owner's relation to tag, as the naval control sums read it
    return r && g_isFriendly(r);
}

static bool OwnerMatches(int owner, const UnitFilter& f) {
    if (f.owner && !SameCountry(owner, f.owner)) return false;
    if (f.enemyOf && !AtWar(f.enemyOf, owner)) return false;
    if (f.alliedWith && !AlliedWith(owner, f.alliedWith)) return false;
    return true;
}

static PtrList AreaProvinces(AreaKind kind, int id) {
    void* area = kind == AreaKind::State ? StateById(id) : RegionById(id);
    return ListAt(area, g_provinces, g_provinceCount);
}

bool CountDivisions(AreaKind kind, int id, const UnitFilter& f, int64_t* out) {
    *out = 0;
    if (!HaveMilitaryCounts()) return false;
    PtrList provinces = AreaProvinces(kind, id);
    if (!provinces.data && !(kind == AreaKind::State ? StateById(id) : RegionById(id))) return false;
    for (int i = 0; i < provinces.count; ++i) {
        PtrList units = ListAt(provinces.data[i], g_landUnits, g_landUnitCount);
        for (int u = 0; u < units.count; ++u)
            if (units.data[u] && OwnerMatches(Rd<int>(units.data[u], g_unitOwner), f)) ++*out;
    }
    return true;
}

bool CountShips(AreaKind kind, int id, const UnitFilter& f, int64_t* out) {
    *out = 0;
    if (!HaveMilitaryCounts()) return false;
    if (!(kind == AreaKind::State ? StateById(id) : RegionById(id))) return false;
    PtrList provinces = AreaProvinces(kind, id);
    for (int i = 0; i < provinces.count; ++i) {
        PtrList units = ListAt(provinces.data[i], g_navalUnits, g_navalUnitCount);
        for (int u = 0; u < units.count; ++u) {
            if (!units.data[u] || !OwnerMatches(Rd<int>(units.data[u], g_unitOwner), f)) continue;
            const uint8_t* ships = g_shipsOf(units.data[u]);
            int n = ships ? Rd<int>(ships, kShipCount) : 0;
            if (n > 0 && n < 100000) *out += n;
        }
    }
    return true;
}

namespace {
// Distinct combats without allocation: a fixed stack buffer. Past its size a combat is still
// counted but no longer remembered (an area with over 1024 battles may count some twice).
struct CombatSet {
    const void* seen[1024];
    int n = 0;
    int64_t count = 0;
    void add(const void* combat) {
        if (!combat) return;
        for (int i = 0; i < n; ++i)
            if (seen[i] == combat) return;
        if (n < 1024) seen[n++] = combat;
        ++count;
    }
};
}  // namespace

bool CountBattles(AreaKind kind, int id, bool locationOnly, int64_t* out) {
    *out = 0;
    if (!HaveMilitaryCounts()) return false;
    if (!(kind == AreaKind::State ? StateById(id) : RegionById(id))) return false;
    PtrList provinces = AreaProvinces(kind, id);
    CombatSet set;
    for (int i = 0; i < provinces.count; ++i) {
        PtrList combats = ListAt(provinces.data[i], g_provinceCombats, g_provinceCombatCount);
        for (int c = 0; c < combats.count; ++c) set.add(combats.data[c]);
    }
    if (!locationOnly) {
        for (int i = 0; i < provinces.count; ++i) {
            PtrList units = ListAt(provinces.data[i], g_landUnits, g_landUnitCount);
            for (int u = 0; u < units.count; ++u) {
                if (!units.data[u]) continue;
                PtrList combats = ListAt(units.data[u], g_unitCombats, g_unitCombatCount);
                for (int c = 0; c < combats.count; ++c) set.add(combats.data[c]);
            }
        }
    }
    *out = set.count;
    return true;
}

bool CountBattlesInvolving(int tag, int64_t* out) {
    *out = 0;
    if (!HaveMilitaryCounts()) return false;
    void* country = CountryByTag(tag);
    if (!country) return false;
    // Distinct combats among the country's divisions.
    CombatSet set;
    PtrList divisions = ListAt(country, g_countryDivisions, g_countryDivisionCount);
    for (int d = 0; d < divisions.count; ++d) {
        if (!divisions.data[d]) continue;
        PtrList combats = ListAt(divisions.data[d], g_unitCombats, g_unitCombatCount);
        for (int c = 0; c < combats.count; ++c) set.add(combats.data[c]);
    }
    *out = set.count;
    return true;
}

bool CountPlanesStationed(int regionId, int tag, int64_t* out) {
    *out = 0;
    if (!HavePlanes() || !RegionById(regionId)) return false;
    void* country = CountryByTag(tag);
    void* air = country ? g_countryAirPlanes(country) : nullptr;
    if (!air) return country != nullptr;  // no air force: 0
    PtrList wings = ListAt(air, g_airWings, g_airWingCount);
    for (int w = 0; w < wings.count; ++w) {
        if (!wings.data[w]) continue;
        void* region = g_wingRegion(wings.data[w]);
        if (region && Rd<int>(region, g_regionIdField) == regionId) *out += g_wingPlanes(wings.data[w]);
    }
    return true;
}

bool HaveCountryDivisions() { return HaveCountries() && g_countryDivisions && g_countryDivisionCount && g_unitOwner; }

bool CountryDivisionList(int tag, std::vector<void*>* out) {
    out->clear();
    void* country = HaveCountryDivisions() ? CountryByTag(tag) : nullptr;
    if (!country) return false;
    PtrList divisions = ListAt(country, g_countryDivisions, g_countryDivisionCount);
    for (int d = 0; d < divisions.count; ++d)
        if (divisions.data[d]) out->push_back(divisions.data[d]);
    return true;
}

int UnitOwnerTag(const void* unit) { return unit && g_unitOwner ? Rd<int>(unit, g_unitOwner) : 0; }

bool ForEachDivision(int tag, void (*fn)(void* division, void* ctx), void* ctx) {
    if (!HaveMilitaryCounts()) return false;
    void* country = CountryByTag(tag);
    if (!country) return false;
    PtrList divisions = ListAt(country, g_countryDivisions, g_countryDivisionCount);
    for (int d = 0; d < divisions.count; ++d)
        if (divisions.data[d]) fn(divisions.data[d], ctx);
    return true;
}

bool ForEachAreaDivision(AreaKind kind, int id, void (*fn)(void* division, int owner, void* ctx), void* ctx) {
    if (!HaveMilitaryCounts() || !(kind == AreaKind::State ? StateById(id) : RegionById(id))) return false;
    PtrList provinces = AreaProvinces(kind, id);
    for (int i = 0; i < provinces.count; ++i) {
        PtrList units = ListAt(provinces.data[i], g_landUnits, g_landUnitCount);
        for (int u = 0; u < units.count; ++u)
            if (units.data[u]) fn(units.data[u], Rd<int>(units.data[u], g_unitOwner), ctx);
    }
    return true;
}

bool IsSameCountry(int a, int b) { return HaveCountries() && SameCountry(a, b); }

// --- army_condition: CArmy strength / organisation ratios (RTTI + kCodeArmyRatio) ---------------
namespace {
void** g_armyVtable = nullptr;
// Strength: slot 35, the fighting strength the division UI shows (manpower ratio, slot 34, times the
// equipment ratio). Organisation: slot 39.
const size_t kArmyManpowerSlot = 34, kArmyStrengthSlot = 35, kArmyOrgSlot = 39;
using Ratio_t = int64_t*(__fastcall*)(void* unit, int64_t* out);
}  // namespace

void ResolveArmyCondition() {
    size_t slots = 0;
    void** vt = FindVtableByRtti(".?AVCArmy@@", &slots);
    if (!vt || slots <= kArmyOrgSlot) return;
    if (!SigMatchAt((uint8_t*)vt[kArmyManpowerSlot], kCodeArmyRatio) || !SigMatchAt((uint8_t*)vt[kArmyOrgSlot], kCodeArmyRatio) ||
        !SigMatchAt((uint8_t*)vt[kArmyStrengthSlot], kCodeArmyFightingStrength) ||
        Rd<int32_t>(vt[kArmyStrengthSlot], kCodeArmyFightingInnerSlotDisp) != (int32_t)(kArmyManpowerSlot * 8)) {
        Log("ERROR: data triggers: CArmy strength/organisation slots have an unexpected shape");
        return;
    }
    g_armyVtable = vt;
}

bool HaveDivisionCondition() { return HaveMilitaryCounts() && g_armyVtable; }

// --- production_data: production lines and equipment (kSigEquipmentLookup ... kCodeLineEquipment) -
namespace {
using EquipDb_t = void*(__fastcall*)();
using EquipLookup_t = const uint8_t*(__fastcall*)(void* db, int token);
EquipDb_t g_equipDb = nullptr;
EquipLookup_t g_equipLookup = nullptr;
void* const* g_equipDbGlobal = nullptr;  // set once the equipment database exists
uint32_t g_equipValid = 0;               // def byte that must be set
uint32_t g_countryProduction = 0, g_lines = 0, g_lineCount = 0;
uint32_t g_lineFactoriesA = 0, g_lineFactoriesB = 0;  // assigned factories = A + B
uint32_t g_lineEquipment = 0, g_equipType = 0, g_typeArchetype = 0;
void** g_militaryLineVt = nullptr;
void** g_navalLineVt = nullptr;
}  // namespace

void ResolveProduction() {
    uint8_t* p;
    if ((p = SigFindUnique("equipment_lookup", kSigEquipmentLookup)) != nullptr) {
        g_equipDbGlobal = (void* const*)SigRipTarget(p + kSigEquipDbCmp, 3, 8);
        g_equipDb = (EquipDb_t)SigFollowRel32(p + kSigEquipDbGetter);
        g_equipLookup = (EquipLookup_t)SigFollowRel32(p + kSigEquipLookup);
        g_equipValid = Rd<uint8_t>(p, kSigEquipValidDisp);
    }
    uint32_t lines2 = 0;
    if ((p = SigFindUnique("production_line_factories", kSigProductionLineFactories)) != nullptr)
        lines2 = Rd<uint8_t>(p, kSigLineFactoriesListDisp);
    if ((p = SigFindUnique("line_assign_factories", kSigLineAssignFactories)) != nullptr &&
        Rd<uint8_t>(p, kSigAssignFieldA) == Rd<uint8_t>(p, kSigAssignFieldAAgain)) {
        g_lineFactoriesA = Rd<uint8_t>(p, kSigAssignFieldA);
        g_lineFactoriesB = Rd<uint8_t>(p, kSigAssignFieldB);
    }
    if (uint8_t* lea = AnchorFindLea("production.daily_serial", kAnchorProductionDaily)) {
        uint8_t* code = lea + kProductionDailyCodeFromLea;
        if (SigMatchAt(code, kCodeProductionDaily)) {
            g_lines = Rd<uint8_t>(code, kProductionDailyLinesDisp);
            g_lineCount = Rd<uint8_t>(code, kProductionDailyCountDisp);
        }
    }
    size_t n = 0;
    void** availVt = FindVtableByRtti(".?AVCNumOfAvailableMilitaryFactoriesTrigger@@", &n);
    if (availVt && n > 23 && SigMatchAt((uint8_t*)availVt[23], kCodeAvailableMilitaryFactories) &&
        (void*)SigFollowRel32((uint8_t*)availVt[23] + kCodeAvailMilTagToCountry) == (void*)g_tagToCountry)
        g_countryProduction = Rd<uint32_t>(availVt[23], kCodeAvailMilProductionDisp);
    g_militaryLineVt = FindVtableByRtti(".?AVCMilitaryProductionLine@@", &n);
    if (g_militaryLineVt && n > 11 && SigMatchAt((uint8_t*)g_militaryLineVt[11], kCodeLineEquipment)) {
        g_lineEquipment = Rd<uint32_t>(g_militaryLineVt[11], kCodeLineEquipmentDisp);
        g_equipType = Rd<uint32_t>(g_militaryLineVt[11], kCodeLineTypeDisp);
        g_typeArchetype = Rd<uint32_t>(g_militaryLineVt[11], kCodeLineArchetypeDisp);
    }
    g_navalLineVt = FindVtableByRtti(".?AVCNavalProductionLine@@", &n);
    if (g_navalLineVt && (n <= 11 || !SigMatchAt((uint8_t*)g_navalLineVt[11], kCodeNavalLineEquipment) ||
                          Rd<uint32_t>(g_navalLineVt[11], 3) != g_lineEquipment)) {
        Log("data triggers: naval production lines have another layout; factories_assigned counts military lines only");
        g_navalLineVt = nullptr;
    }
    if (g_lines && lines2 && g_lines != lines2) {
        Log("ERROR: data triggers: the two production loops disagree on the line list");
        g_lines = 0;
    }
    if (HaveProduction())
        Log("Data triggers: production +0x%x, lines +0x%x/+0x%x, factories +0x%x + +0x%x, equipment +0x%x -> type +0x%x -> "
            "archetype +0x%x",
            g_countryProduction, g_lines, g_lineCount, g_lineFactoriesA, g_lineFactoriesB, g_lineEquipment, g_equipType, g_typeArchetype);
}

bool HaveProduction() {
    return HaveCountries() && g_equipDb && g_equipLookup && g_equipDbGlobal && g_countryProduction && g_lines && g_lineCount &&
           g_lineFactoriesA && g_lineFactoriesB && g_lineEquipment && g_equipType && g_typeArchetype && g_militaryLineVt;
}

const void* EquipmentByToken(int token) {
    if (!HaveProduction() || token <= 0 || !*g_equipDbGlobal) return nullptr;
    const uint8_t* def = g_equipLookup(g_equipDb(), token);
    return def && Rd<uint8_t>(def, g_equipValid) ? def : nullptr;
}

bool FactoriesAssigned(int tag, const void* equipment, int64_t* out) {
    *out = 0;
    if (!HaveProduction()) return false;
    void* country = CountryByTag(tag);
    void* production = country ? Rd<void*>(country, g_countryProduction) : nullptr;
    if (!production) return false;
    PtrList lines = ListAt(production, g_lines, g_lineCount);
    for (int i = 0; i < lines.count; ++i) {
        void* line = lines.data[i];
        void** vt = line ? *(void***)line : nullptr;
        if (!vt || (vt != g_militaryLineVt && vt != g_navalLineVt)) continue;
        if (equipment) {
            void* equip = Rd<void*>(line, g_lineEquipment);
            void* type = equip ? Rd<void*>(equip, g_equipType) : nullptr;
            void* archetype = type ? Rd<void*>(type, g_typeArchetype) : nullptr;
            if (type != equipment && archetype != equipment) continue;
        }
        int f = Rd<int>(line, g_lineFactoriesA) + Rd<int>(line, g_lineFactoriesB);
        if (f > 0) *out += f;
    }
    return true;
}

namespace {
// Diagnostics (LogProductionLines): RTTI class of an object, and an equipment object's name token,
// found as the int field t inside it for which the engine's own lookup gives the object back.
const char* SafeRttiName(const void* obj) {
    __try {
        const uint32_t* col = (const uint32_t*)(*(void* const* const*)obj)[-1];
        if (col[0] != 1) return "?";
        return (const char*)GetModuleHandleW(nullptr) + col[3] + 0x10;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "?";
    }
}

int SafeEquipmentToken(const void* obj) {
    if (!obj) return 0;
    __try {
        for (uint32_t off = 0; off < 0x800; off += 4) {
            int t = Rd<int>(obj, off);
            if (t > 0 && t < 0x40000 && EquipmentByToken(t) == obj) return t;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

const char* EquipName(const void* obj) {
    int t = SafeEquipmentToken(obj);
    return t ? TokenName(t) : obj ? "(no name)" : "-";
}

bool SafeLogLine(int i, void* line) {
    __try {
        void** vt = *(void***)line;
        bool counted = vt == g_militaryLineVt || (g_navalLineVt && vt == g_navalLineVt);
        void* equip = Rd<void*>(line, g_lineEquipment);
        void* type = equip ? Rd<void*>(equip, g_equipType) : nullptr;
        void* archetype = type ? Rd<void*>(type, g_typeArchetype) : nullptr;
        Log("  line %d: %s%s, factories %d; equipment %p (%s) -> type %p (%s) -> archetype %p (%s)", i,
            SafeRttiName(line), counted ? "" : " (not counted)", Rd<int>(line, g_lineFactoriesA) + Rd<int>(line, g_lineFactoriesB), equip, EquipName(equip), type,
            EquipName(type), archetype, EquipName(archetype));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

void LogProductionLines(int tag) {
    if (!HaveProduction()) return;
    void* country = CountryByTag(tag);
    void* production = country ? Rd<void*>(country, g_countryProduction) : nullptr;
    if (!production) {
        Log("Production lines of %s: unreadable", CountryTagName(tag).c_str());
        return;
    }
    PtrList lines = ListAt(production, g_lines, g_lineCount);
    Log("Production lines of %s: %d", CountryTagName(tag).c_str(), lines.count);
    for (int i = 0; i < lines.count; ++i)
        if (!lines.data[i] || !SafeLogLine(i, lines.data[i])) Log("  line %d: unreadable", i);
}

bool DivisionRatios(void* unit, int64_t* strength, int64_t* org) {
    if (!g_armyVtable || !unit || *(void***)unit != g_armyVtable) return false;  // divisions only
    *strength = *((Ratio_t)g_armyVtable[kArmyStrengthSlot])(unit, strength);
    *org = *((Ratio_t)g_armyVtable[kArmyOrgSlot])(unit, org);
    return true;
}

bool HaveCountries() { return g_tagToCountry && g_tagIndex; }  // resolved; a game need not exist yet
bool HaveRegionControl() {
    return g_tagToCountry && g_tagIndex && g_countryAir && g_airRegionEntry && g_isFriendly && g_regionNavalOffset &&
           g_diplomacyOffset && g_relationWarOffset && HaveRegions();
}

int TagFromText(const char* text) {
    if (!g_tagDb || !g_tagLookup || !text) return 0;
    size_t n = strlen(text);
    if (n == 0 || n > 15) return 0;
    EngineString str{};  // inline buffer, like the engine's own short std::string
    for (size_t i = 0; i < n; ++i) str.buf[i] = (char)toupper((unsigned char)text[i]);
    str.size = n;
    str.cap = 15;
    void* db = *g_tagDb;
    if (!db) return 0;
    int tag = g_tagLookup(db, &str);
    return tag > 0 ? tag : 0;
}

void* CountryByTag(int tag) {
    if (tag <= 0 || !g_tagToCountry || !GameState()) return nullptr;
    return g_tagToCountry(&tag);
}

// relation(a -> b): a's diplomatic view of b, or null.
static void* Relation(int tagA, int tagB) {
    void* a = CountryByTag(tagA);
    if (!a || tagB <= 0) return nullptr;
    void* dipl = Rd<void*>(a, g_diplomacyOffset);
    if (!dipl) return nullptr;
    void** rel = Rd<void**>(dipl, kRelations);
    int idx = g_tagIndex(&tagB);
    return rel && idx >= 0 ? rel[idx] : nullptr;
}

bool AtWar(int tagA, int tagB) {
    if (!g_tagToCountry || !g_tagIndex || !g_diplomacyOffset || !g_relationWarOffset || tagA <= 0 || tagB <= 0 || tagA == tagB)
        return false;
    void* rel = Relation(tagA, tagB);
    void* war = rel ? Rd<void*>(rel, g_relationWarOffset) : nullptr;
    return war && Rd<uint8_t>(war, kWarEnded) == 0;
}

bool AirPower(int tag, int regionId, int64_t* our, int64_t* enemy) {
    *our = *enemy = 0;
    if (!HaveRegionControl()) return false;
    void* region = RegionById(regionId);
    void* country = CountryByTag(tag);
    if (!region || !country) return false;
    void* air = g_countryAir(country);
    if (!air) return false;
    const int64_t* e = g_airRegionEntry(air, region);
    if (!e) return false;
    *our = e[0];
    *enemy = e[1];
    return true;
}

bool NavalDominance(int tag, int regionId, int64_t* friendly, int64_t* enemy) {
    *friendly = *enemy = 0;
    if (!HaveRegionControl() || tag <= 0) return false;
    void* region = RegionById(regionId);
    if (!region) return false;
    uint8_t* naval = Rd<uint8_t*>(region, g_regionNavalOffset);
    if (!naval) return true;  // no naval data for this region (land region): zero on both sides
    uint8_t* entries = Rd<uint8_t*>(naval, kNavalEntries);
    int count = Rd<int>(naval, kNavalCount);
    if (count < 0 || count > 4096 || (count && !entries)) return false;
    int scopeIdx = g_tagIndex(&tag);
    for (int i = 0; i < count; ++i) {
        uint8_t* entry = entries + (size_t)i * kNavalEntrySize;
        int64_t value = Rd<int64_t>(entry, kNavalValue);
        void* c = g_tagToCountry((const int*)entry);  // entry starts with the country tag
        if (!c) continue;
        void* dipl = Rd<void*>(c, g_diplomacyOffset);
        void** rel = dipl ? Rd<void**>(dipl, kRelations) : nullptr;
        void* r = rel && scopeIdx >= 0 ? rel[scopeIdx] : nullptr;
        if (!r) continue;
        if (g_isFriendly(r)) {
            *friendly += value;
        } else {
            void* war = Rd<void*>(r, g_relationWarOffset);
            if (war && Rd<uint8_t>(war, kWarEnded) == 0) *enemy += value;
        }
    }
    return true;
}

std::string RegionName(int regionId) {
    void* region = RegionById(regionId);
    const EngineString* s = region && pRegionGetNameKey ? ((GetNameKey_t)pRegionGetNameKey)(region) : nullptr;
    return s && s->size > 0 && s->size < 4096 ? std::string(s->c_str(), (size_t)s->size) : "region " + std::to_string(regionId);
}

// --- Region data (region_scope) ----------------------------------------------------------------
bool HaveRegionData() { return HaveRegions() && g_stateArray && g_stateCount && g_provinces && g_provinceCount && g_regionSeaShare; }

bool RegionIsSea(int regionId, bool* sea) {
    void* region = HaveRegionData() ? RegionById(regionId) : nullptr;
    if (!region) return false;
    *sea = Rd<int64_t>(region, g_regionSeaShare) > 0;
    return true;
}

bool RegionHasState(int regionId, int stateId, bool* has) {
    *has = false;
    if (!HaveRegionData()) return false;
    void* region = RegionById(regionId);
    void* state = StateById(stateId);
    if (!region || !state) return false;
    PtrList inRegion = ListAt(region, g_provinces, g_provinceCount);
    PtrList inState = ListAt(state, g_provinces, g_provinceCount);
    for (int i = 0; i < inState.count && !*has; ++i)
        for (int j = 0; j < inRegion.count && !*has; ++j) *has = inState.data[i] == inRegion.data[j];
    return true;
}

int RegionOfState(int stateId) {
    void* state = HaveRegionData() ? StateById(stateId) : nullptr;
    PtrList inState = ListAt(state, g_provinces, g_provinceCount);
    if (!inState.count) return 0;
    int regions = RegionCount();
    for (int id = 1; id < regions; ++id) {
        PtrList inRegion = ListAt(RegionById(id), g_provinces, g_provinceCount);
        for (int j = 0; j < inRegion.count; ++j)
            if (inRegion.data[j] == inState.data[0]) return id;
    }
    return 0;
}

int GameDateHours() {
    void* gs = GameState();
    return gs && g_gameDate ? Rd<int>(gs, g_gameDate) : 0;
}

}  // namespace Engine
