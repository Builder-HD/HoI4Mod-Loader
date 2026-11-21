// Engine access for send_home (NOTES.md Part V): the game's own return of volunteers
// (CVolunteerForceTransfer), reached from recall_volunteers_from's execute, with every offset it
// uses cross-checked against the one another feature resolved from different code.
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
template <typename T>
void Wr(void* base, size_t offset, T v) {
    memcpy((uint8_t*)base + offset, &v, sizeof v);
}

// The engine's CPdxArray { data, capacity, count, allocator }; create only reads it.
struct PdxArray {
    void* const* data;
    int cap;
    int count;
    void* allocator;
};

using IsVolunteer_t = bool(__fastcall*)(const void* unit);
using Create_t = void*(__fastcall*)(void* owner, const int* from, const int* to, const PdxArray* divisions, bool sending);

IsVolunteer_t g_isVolunteer = nullptr;
Create_t g_create = nullptr;
void* g_start = nullptr;          // start(transfer, province), only for the log
void* g_choose = nullptr;         // choose(&from, &to), only for the log
void** g_transferVt = nullptr;    // CVolunteerForceTransfer
const int* g_defaultDays = nullptr;  // VOLUNTEERS_TRANSFER_SPEED (the define's value as the game reads it)
uint32_t g_list = 0, g_count = 0;  // country -> transfer list, count
uint32_t g_days = 0, g_arrival = 0, g_armyRecorded = 0;  // transfer -> days left, arrival province id, army recorded
uint32_t g_unitProvince = 0;      // unit -> CProvince it stands in
uint32_t g_secondTag = 0;         // unit -> a second tag the transfer refuses when set (NOTES.md V2.2)
uint32_t g_fightingFor = 0;       // unit -> "fighting for" tag (only logged; the game's test reads it)

const size_t kUnitKind = 8;  // the transfer takes units with 0 here (land); matched in kCodeTransferUnitFilter

uint8_t* Slot(const char* rtti, size_t slot) {
    size_t n = 0;
    void** vt = FindVtableByRtti(rtti, &n);
    return vt && n > slot ? (uint8_t*)vt[slot] : nullptr;
}

bool Fail(const char* what) {
    Log("ERROR: send_home: %s; sending divisions home unavailable", what);
    return false;
}

bool ResolveAll() {
    if (!HaveCountryDivisions() || !HaveProvinceIds() || !DivisionArmyOffset() || !HaveMilitaryCounts())
        return Fail("needs the division list, province ids, the division army field and the province unit lists");
    uint8_t* exec = Slot(".?AVCRecallVolunteersFromEffect@@", 13);
    if (!exec || !SigMatchAt(exec, kCodeRecallVolunteersExecute)) return Fail("recall_volunteers_from's execute has an unexpected shape");
    uint8_t* recall = SigFollowRel32(exec + kCodeRecallVolunteersCall);
    if (!SigMatchAt(recall, kCodeRecallVolunteers)) return Fail("the recall has an unexpected shape");
    uint32_t divisions = 0, divisionCount = 0;
    CountryDivisionOffsets(&divisions, &divisionCount);
    if (Rd<uint32_t>(recall, kCodeRecallDivisionsDisp) != divisions || Rd<uint32_t>(recall, kCodeRecallDivisionCountDisp) != divisionCount)
        return Fail("the recall reads another division list than army.daily");
    uint8_t* cc = SigFindIn(recall, 0x300, kCodeRecallCreateCall);
    if (!cc) return Fail("the recall's create call not found");
    uint8_t* create = SigFollowRel32(cc + kCodeRecallCreateCallAt);
    if (!SigMatchAt(create, kCodeTransferCreate)) return Fail("create has an unexpected shape");
    uint8_t* filter = SigFindIn(create, 0x100, kCodeTransferUnitFilter);
    uint8_t* append = SigFindIn(create, 0x500, kCodeTransferAppend);
    if (!filter || !append) return Fail("create's unit filter or list append not found");
    if (Rd<uint32_t>(filter, kCodeTransferUnitArmyDisp) != DivisionArmyOffset())
        return Fail("create groups by another army field than add_to_army's");
    uint32_t list = Rd<uint32_t>(append, kCodeTransferListDisp), count = Rd<uint32_t>(append, kCodeTransferCountDisp);
    if (count != list + 0xc || list < 0x100 || list > 0x10000) return Fail("the transfer list's layout is unexpected");
    uint8_t* start = SigFollowRel32(append + kCodeTransferStartCall);
    if (!SigMatchAt(start, kCodeTransferStart)) return Fail("start has an unexpected shape");
    if (Rd<uint32_t>(start, kCodeTransferProvinceIdDisp) != ProvinceIdOffset())
        return Fail("start reads another province id than the province data");
    uint8_t* leave = SigFindIn(start, 0x800, kCodeTransferLeave);
    if (!leave || Rd<uint32_t>(leave, kCodeTransferLeaveArmyDisp) != DivisionArmyOffset())
        return Fail("start's province and army step not found or reads another army field");
    uint32_t days = Rd<uint8_t>(start, kCodeTransferDaysDisp), arrival = Rd<uint8_t>(start, kCodeTransferArrivalDisp),
             recorded = Rd<uint8_t>(leave, kCodeTransferArmyRecordedDisp);
    // The transfer object is 0xa0 bytes (create allocates it); the three fields differ and lie in it.
    if (days == arrival || days == recorded || arrival == recorded || days >= 0xa0 || arrival >= 0xa0 || recorded >= 0xa0)
        return Fail("the transfer's fields are unexpected");
    size_t n = 0;
    g_transferVt = FindVtableByRtti(".?AVCVolunteerForceTransfer@@", &n);
    if (!g_transferVt) return Fail("CVolunteerForceTransfer vtable not found");
    g_isVolunteer = (IsVolunteer_t)SigFollowRel32(recall + kCodeRecallIsVolunteerCall);
    g_fightingFor = Rd<uint32_t>(recall, kCodeRecallFightingForDisp);
    g_create = (Create_t)create;
    g_start = start;
    g_choose = SigFollowRel32(create + kCodeTransferChooseCall);
    g_defaultDays = (const int*)SigRipTarget(start + kCodeTransferDaysDefineRel - 2, 2, 6);
    g_list = list;
    g_count = count;
    g_days = days;
    g_arrival = arrival;
    g_armyRecorded = recorded;
    g_unitProvince = Rd<uint32_t>(leave, kCodeTransferUnitProvinceDisp);
    g_secondTag = Rd<uint32_t>(filter, kCodeTransferSecondTagDisp);
    return true;
}

// The transfer list of a country, checked: a sane count and a list when the count is not 0.
bool TransferList(void* country, void* const** data, int* count) {
    *data = Rd<void* const*>(country, g_list);
    *count = Rd<int>(country, g_count);
    return *count >= 0 && *count < 100000 && (*count == 0 || *data);
}

}  // namespace

void ResolveSendHome() {
    if (!ResolveAll()) {
        g_create = nullptr;
        return;
    }
    Log("send_home: create %s, start %s, choose %s, is volunteer %s; transfers +0x%x/+0x%x, days +0x%x (default %d), arrival "
        "+0x%x, army recorded +0x%x; unit province +0x%x, second tag +0x%x, fighting for +0x%x",
        SigDescribe((void*)g_create).c_str(), SigDescribe(g_start).c_str(), SigDescribe(g_choose).c_str(),
        SigDescribe((void*)g_isVolunteer).c_str(), g_list, g_count, g_days, SendHomeDefaultDays(), g_arrival, g_armyRecorded,
        g_unitProvince, g_secondTag, g_fightingFor);
}

bool HaveSendHome() { return g_create && g_isVolunteer && g_transferVt && g_defaultDays && HaveJoinArmy(); }

int SendHomeDefaultDays() {
    int d = g_defaultDays ? *g_defaultDays : 0;
    return d > 0 && d < 100000 ? d : 0;
}

void* UnitProvince(const void* unit) { return unit && g_unitProvince ? Rd<void*>(unit, g_unitProvince) : nullptr; }

bool UnitIsVolunteer(const void* unit) { return unit && g_isVolunteer && g_isVolunteer(unit); }

bool UnitTransferable(const void* unit) { return unit && Rd<int>(unit, kUnitKind) == 0 && Rd<int>(unit, g_secondTag) < 1; }

int SendHome(int owner, int fromTag, const std::vector<void*>& divisions, int days, int arrivalProvince, bool keepArmy) {
    void* country = HaveSendHome() ? CountryByTag(owner) : nullptr;
    void* const* list = nullptr;
    int before = 0, after = 0;
    if (!country || !TransferList(country, &list, &before)) return -1;
    if (divisions.empty()) return 0;
    PdxArray units{divisions.data(), (int)divisions.size(), (int)divisions.size(), nullptr};
    int from = fromTag, to = owner;
    g_create(country, &from, &to, &units, false);
    if (!TransferList(country, &list, &after)) return -1;
    int made = 0;
    for (int i = before; i < after; ++i) {
        uint8_t* t = (uint8_t*)list[i];
        if (!t || *(void***)t != g_transferVt) continue;
        ++made;
        if (days > 0) Wr<int>(t, g_days, days);
        if (arrivalProvince > 0) Wr<int>(t, g_arrival, arrivalProvince);
        if (!keepArmy) Wr<uint8_t>(t, g_armyRecorded, 0);
    }
    return made;
}

bool SendHomeSelfCheck(int tag, int* onMap, int* groups, const char** why) {
    *onMap = *groups = 0;
    std::vector<void*> units;
    void* country = CountryByTag(tag);
    if (!country || !CountryDivisionList(tag, &units)) {
        *why = "the country's division list is unreadable";
        return false;
    }
    // Divisions carried at sea may not be in a province's land list: the offset is only wrong
    // if none of them is found where its province field says.
    int withProvince = 0;
    for (void* unit : units) {
        void* province = UnitProvince(unit);
        if (!province) continue;
        ++withProvince;
        if (ProvinceHoldsLandUnit(province, unit)) ++*onMap;
    }
    if (withProvince > 0 && *onMap == 0) {
        *why = "no division is in the land unit list of the province its province field names";
        return false;
    }
    void* const* list = nullptr;
    int count = 0;
    if (!TransferList(country, &list, &count)) {
        *why = "the transfer list is unreadable";
        return false;
    }
    for (int i = 0; i < count; ++i) {
        const uint8_t* t = (const uint8_t*)list[i];
        if (!t || *(void** const*)t != g_transferVt) {
            *why = "an entry of the transfer list is not a CVolunteerForceTransfer";
            return false;
        }
        int d = Rd<int>(t, g_days);
        if (d < -1 || d > 100000) {
            *why = "a travelling group's days are out of range";
            return false;
        }
        ++*groups;
    }
    return true;
}

}  // namespace Engine
