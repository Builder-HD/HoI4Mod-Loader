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
}

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
    if (!HaveRegionControl() || tagA <= 0 || tagB <= 0 || tagA == tagB) return false;
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
