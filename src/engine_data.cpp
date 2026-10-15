// Engine access for the data-trigger features (NOTES.md Part E): read-only views of game state
// that the engine computes anyway. Everything here may run on any thread (native trigger
// evaluate): plain reads and calls into engine getters that only read; no allocation, no logging
// on the hot path. Resolved once from src/signatures.h.
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
uint32_t g_regionNavalOffset = 0;  // CStrategicRegion -> naval dominance object
uint32_t g_diplomacyOffset = 0;    // CCountry -> diplomacy (relations at +8, indexed by tag index)
uint32_t g_relationWarOffset = 0;  // relation -> CWar* (0 = no war)

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

static bool OwnerMatches(int owner, int tag, UnitOwner who) {
    switch (who) {
        case UnitOwner::Own: return SameCountry(owner, tag);
        case UnitOwner::Enemy: return AtWar(tag, owner);
        default: return true;
    }
}

static PtrList AreaProvinces(AreaKind kind, int id) {
    void* area = kind == AreaKind::State ? StateById(id) : RegionById(id);
    return ListAt(area, g_provinces, g_provinceCount);
}

bool CountDivisions(AreaKind kind, int id, int tag, UnitOwner who, int64_t* out) {
    *out = 0;
    if (!HaveMilitaryCounts()) return false;
    PtrList provinces = AreaProvinces(kind, id);
    if (!provinces.data && !(kind == AreaKind::State ? StateById(id) : RegionById(id))) return false;
    for (int i = 0; i < provinces.count; ++i) {
        PtrList units = ListAt(provinces.data[i], g_landUnits, g_landUnitCount);
        for (int u = 0; u < units.count; ++u)
            if (units.data[u] && OwnerMatches(Rd<int>(units.data[u], g_unitOwner), tag, who)) ++*out;
    }
    return true;
}

bool CountShips(AreaKind kind, int id, int tag, UnitOwner who, int64_t* out) {
    *out = 0;
    if (!HaveMilitaryCounts()) return false;
    if (!(kind == AreaKind::State ? StateById(id) : RegionById(id))) return false;
    PtrList provinces = AreaProvinces(kind, id);
    for (int i = 0; i < provinces.count; ++i) {
        PtrList units = ListAt(provinces.data[i], g_navalUnits, g_navalUnitCount);
        for (int u = 0; u < units.count; ++u) {
            if (!units.data[u] || !OwnerMatches(Rd<int>(units.data[u], g_unitOwner), tag, who)) continue;
            const uint8_t* ships = g_shipsOf(units.data[u]);
            int n = ships ? Rd<int>(ships, kShipCount) : 0;
            if (n > 0 && n < 100000) *out += n;
        }
    }
    return true;
}

bool CountBattles(AreaKind kind, int id, int64_t* out) {
    *out = 0;
    if (!HaveMilitaryCounts()) return false;
    if (!(kind == AreaKind::State ? StateById(id) : RegionById(id))) return false;
    PtrList provinces = AreaProvinces(kind, id);
    for (int i = 0; i < provinces.count; ++i) *out += ListAt(provinces.data[i], g_provinceCombats, g_provinceCombatCount).count;
    return true;
}

bool CountBattlesInvolving(int tag, int64_t* out) {
    *out = 0;
    if (!HaveMilitaryCounts()) return false;
    void* country = CountryByTag(tag);
    if (!country) return false;
    // Distinct combats among the country's divisions; a fixed buffer, no allocation.
    const void* seen[1024];
    int nSeen = 0;
    PtrList divisions = ListAt(country, g_countryDivisions, g_countryDivisionCount);
    for (int d = 0; d < divisions.count; ++d) {
        PtrList combats = ListAt(divisions.data[d], g_unitCombats, g_unitCombatCount);
        for (int c = 0; c < combats.count; ++c) {
            const void* combat = combats.data[c];
            bool dup = !combat;
            for (int s = 0; s < nSeen && !dup; ++s) dup = seen[s] == combat;
            if (dup) continue;
            if (nSeen < 1024) seen[nSeen++] = combat;
            ++*out;
        }
    }
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

bool HaveCountries() { return g_tagToCountry && g_tagIndex && GameState(); }
bool HaveRegionControl() {
    return g_tagToCountry && g_tagIndex && g_countryAir && g_airRegionEntry && g_isFriendly && g_regionNavalOffset &&
           g_diplomacyOffset && g_relationWarOffset && HaveRegions();
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

}  // namespace Engine
