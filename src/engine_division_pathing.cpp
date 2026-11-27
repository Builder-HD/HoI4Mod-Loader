// Engine access for division_pathing (NOTES.md Part W): the game's own route and travel time for a
// division, as the unit tooltip's "arrives in N days" computes them, and the owner's capital as
// "home" (Design A). Only game functions are called; nothing is hooked or written.
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

// The engine's CPdxArray<int> as ui_time lays it out on its stack (kCodePathUiTime): data +0,
// capacity +8, count +0xc, allocator +0x10, a dword +0x18.
struct IntArray {
    int* data;
    int cap;
    int count;
    void* allocator;
    int extra;
    int pad;
};
static_assert(sizeof(IntArray) == 0x20, "path array layout");

using PathTime_t = int64_t*(__fastcall*)(void* unit, int64_t* out, void* from, void* to, IntArray* path, bool p6, void* cache,
                                         bool strategic, bool useCache);
using Free_t = void(__fastcall*)(void* allocator, void* data);
using Getter_t = void*(__fastcall*)(void* country);

PathTime_t g_pathTime = nullptr;
void* g_allocator = nullptr;   // the allocator object ui_time gives its path array
Getter_t g_capitalState = nullptr;
Getter_t g_capitalProvince = nullptr;
uint32_t g_unitProvince = 0;    // unit -> the CProvince it stands in
uint32_t g_seaInfo = 0, g_seaFlags = 0;  // province -> info -> flags; flags & 3 == 0: sea
uint32_t g_strategic = 0;       // unit -> in strategic redeployment (only logged)

const int kMaxPath = 100000;

uint8_t* Slot(const char* rtti, size_t slot) {
    size_t n = 0;
    void** vt = FindVtableByRtti(rtti, &n);
    return vt && n > slot ? (uint8_t*)vt[slot] : nullptr;
}

bool Fail(const char* what) {
    Log("ERROR: division_pathing: %s; division pathing triggers unavailable", what);
    return false;
}

// The capital getters, from the volunteer transfer's arrival choice (the chain send_home uses).
bool ResolveCapital() {
    uint8_t* exec = Slot(".?AVCRecallVolunteersFromEffect@@", 13);
    if (!exec || !SigMatchAt(exec, kCodeRecallVolunteersExecute)) return Fail("recall_volunteers_from's execute has an unexpected shape");
    uint8_t* recall = SigFollowRel32(exec + kCodeRecallVolunteersCall);
    if (!SigMatchAt(recall, kCodeRecallVolunteers)) return Fail("the recall has an unexpected shape");
    uint8_t* cc = SigFindIn(recall, 0x300, kCodeRecallCreateCall);
    uint8_t* create = cc ? SigFollowRel32(cc + kCodeRecallCreateCallAt) : nullptr;
    if (!create || !SigMatchAt(create, kCodeTransferCreate)) return Fail("the transfer's create not found");
    uint8_t* choose = SigFollowRel32(create + kCodeTransferChooseCall);
    uint8_t* cap = SigFindIn(choose, kPathChooseRange, kCodeChooseCapital);
    if (!cap) return Fail("the arrival choice's capital lookup not found");
    uint8_t* state = SigFollowRel32(cap + kCodeChooseCapitalStateCall);
    uint8_t* province = SigFollowRel32(cap + kCodeChooseCapitalProvinceCall);
    if (!SigMatchAt(province, kCodeCapitalProvince)) return Fail("capital_province has an unexpected shape");
    if (SigFollowRel32(province + kCodeCapitalProvinceStateCall) != state)
        return Fail("capital_province does not start from the capital state the arrival choice reads");
    g_capitalState = (Getter_t)state;
    g_capitalProvince = (Getter_t)province;
    return true;
}

bool ResolveAll() {
    if (!HaveProvinceData() || !HaveCountries() || !HaveJoinArmy())
        return Fail("needs the province data, countries and the division scope getter");
    uint8_t* lea = AnchorFindLea("division_pathing_tooltip", kAnchorUnitMovingDaysHours);
    if (!lea) return Fail("the unit tooltip's arrival text not found");
    uint8_t* ui = SigFindIn(lea - kPathUiBlockRange, kPathUiBlockRange, kCodePathUiBlock);
    if (!ui) return Fail("the unit tooltip's time call not found");
    uint8_t* hours = SigFollowRel32(ui + kCodePathUiHoursCall);
    if (!SigMatchAt(hours, kCodePathHours)) return Fail("the tooltip's hours are not ceil(t / 100000)");
    uint8_t* uiTime = SigFollowRel32(ui + kCodePathUiTimeCall);
    if (!SigMatchAt(uiTime, kCodePathUiTime)) return Fail("ui_time has an unexpected shape");
    uint8_t* pathTime = SigFollowRel32(uiTime + kCodePathUiTimePathCall);
    if (!SigMatchAt(pathTime, kCodePathTime)) return Fail("path_time has an unexpected shape");
    uint8_t* sea = SigFindIn(pathTime, kPathTimeRange, kCodePathSeaTest);
    uint8_t* tc = SigFindIn(pathTime, kPathTimeRange, kCodePathTimeCall);
    if (!sea || !tc) return Fail("path_time's sea test or time call not found");
    uint8_t* along = SigFollowRel32(tc + kCodePathTimeCallAt);
    if (!SigMatchAt(along, kCodePathAlong)) return Fail("the time along the path has an unexpected shape");
    uint32_t unitProvince = Rd<uint32_t>(ui, kCodePathUiProvinceDisp);
    // Two game functions read the unit's province: the tooltip and the time along the path. And
    // send_home's transfer, when resolved.
    if (unitProvince != Rd<uint32_t>(along, kCodePathAlongProvinceDisp))
        return Fail("the tooltip and the time along the path read different unit province fields");
    if (UnitProvinceOffset() && UnitProvinceOffset() != unitProvince)
        return Fail("the unit province field differs from send_home's");
    if (unitProvince < 0x40 || unitProvince > 0x2000) return Fail("the unit province field is out of range");
    if (!ResolveCapital()) return false;
    g_allocator = SigRipTarget(uiTime + kCodePathUiTimeAllocLea, 3, 7);
    g_seaInfo = Rd<uint32_t>(sea, kCodePathSeaInfoDisp);
    g_seaFlags = Rd<uint32_t>(sea, kCodePathSeaFlagsDisp);
    g_strategic = Rd<uint32_t>(pathTime, kCodePathTimeStrategicDisp);
    g_unitProvince = unitProvince;
    g_pathTime = (PathTime_t)pathTime;
    return true;
}

bool ProvinceSea(const void* province, bool* sea) {
    const void* info = province ? Rd<const void*>(province, g_seaInfo) : nullptr;
    if (!info) return false;
    *sea = (Rd<uint8_t>(info, g_seaFlags) & 3) == 0;
    return true;
}

}  // namespace

void ResolveDivisionPathing() {
    if (!ResolveAll()) {
        g_pathTime = nullptr;
        return;
    }
    Log("division_pathing: path_time %s, allocator %s, capital %s / %s; unit province +0x%x, strategic +0x%x, sea test "
        "+0x%x -> +0x%x & 3",
        SigDescribe((void*)g_pathTime).c_str(), SigDescribe(g_allocator).c_str(), SigDescribe((void*)g_capitalState).c_str(),
        SigDescribe((void*)g_capitalProvince).c_str(), g_unitProvince, g_strategic, g_seaInfo, g_seaFlags);
}

bool HaveDivisionPathing() { return g_pathTime && g_allocator && g_capitalProvince && g_capitalState; }

void* DivisionProvince(const void* unit) { return unit && g_unitProvince ? Rd<void*>(unit, g_unitProvince) : nullptr; }

bool ProvinceIsSea(const void* province, bool* sea) { return HaveDivisionPathing() && ProvinceSea(province, sea); }

void* HomeProvince(int owner) {
    void* country = HaveDivisionPathing() ? CountryByTag(owner) : nullptr;
    if (!country) return nullptr;
    void* capital = g_capitalProvince(country);
    if (capital && IsSameCountry(ProvinceController(ProvinceIdOf(capital)), owner)) return capital;
    // As the arrival choice: the first province of the capital state the owner controls.
    void* state = g_capitalState(country);
    void* const* data = nullptr;
    int count = 0;
    if (!state || !StateObjectProvinces(state, &data, &count)) return nullptr;
    for (int i = 0; i < count; ++i)
        if (data[i] && IsSameCountry(ProvinceController(ProvinceIdOf(data[i])), owner)) return data[i];
    return nullptr;
}

Route DivisionRoute(void* unit, void* to, int64_t* time, int* steps, int* lastId) {
    *time = 0;
    if (steps) *steps = 0;
    if (lastId) *lastId = 0;
    void* from = DivisionProvince(unit);
    bool sea = false;
    if (!HaveDivisionPathing() || !from || !to || !ProvinceSea(from, &sea) || sea) return Route::Unknown;
    if (from == to) return Route::Land;
    IntArray path{nullptr, 0, 0, g_allocator, 0, 0};
    int64_t t = 0;
    g_pathTime(unit, &t, from, to, &path, false, nullptr, false, false);
    Route r = Route::None;
    if (path.count > 0 && path.count <= kMaxPath && path.data) {
        r = Route::Land;
        for (int i = 0; i < path.count; ++i) {
            void* p = ProvinceById(path.data[i]);
            if (!p || !ProvinceSea(p, &sea)) {
                r = Route::Unknown;
                break;
            }
            if (sea) r = Route::Naval;
        }
        if (steps) *steps = path.count;
        if (lastId) *lastId = path.data[path.count - 1];
    } else if (path.count != 0) {
        r = Route::Unknown;
    }
    if (path.data) {  // as ui_time frees it
        path.count = 0;
        Free_t free = (*(Free_t**)g_allocator)[2];
        free(g_allocator, path.data);
    }
    if (r == Route::Land || r == Route::Naval) *time = t < 0 ? 0 : t;
    return r;
}

}  // namespace Engine
