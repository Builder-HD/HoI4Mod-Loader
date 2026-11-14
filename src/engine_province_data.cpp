// Province data for area selection (NOTES.md M4, M14): province objects and ids, pixel rects, the
// coastal flag, the controller and the state owner. Read only; main thread or trigger reads. Used
// by attention_area (area blocks, the camera, the hover and centre hooks) and meant for a general
// area selector feature, so it is resolved on its own: no dependency on the outline or camera code.
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

using GetProvince_t = void*(__fastcall*)(void* container, int id);
uint32_t g_provContainer = 0;  // gamestate -> provinces container (vtable slot 0 = Get(id))
uint32_t g_provId = 0;         // CProvince -> id
uint32_t g_provGfx = 0, g_provRect = 0;
void** g_ppMapData = nullptr;  // static map data: per-province flags
uint32_t g_mapCount = 0, g_mapArray = 0, g_mapFlags = 0;
uint8_t g_coastalBit = 0;
uint32_t g_stateOwner = 0;

void* ProvinceObject(int id) {
    void* gs = GameState();
    if (!gs || !g_provContainer || id <= 0 || id >= ProvinceCount()) return nullptr;
    void* c = (uint8_t*)gs + g_provContainer;
    return ((GetProvince_t)(*(void***)c)[0])(c, id);
}

}  // namespace

void ResolveProvinceData() {
    uint8_t* go = SigFindUnique("goto_state_tail", kSigGotoStateTail);  // the province id offset
    uint8_t* coast = SigFindUnique("state_coastal", kSigStateCoastal);
    uint8_t* rect = SigFindUnique("province_rect", kSigProvinceRect);
    uint8_t* rail = SigFindUnique("rail_controller", kSigRailController);
    uint8_t* owner = SigFindUnique("state_owner_check", kSigStateOwnerCheck);
    if (!go || !coast || !rect || !rail || !owner) return;
    uint8_t bit = Rd<uint8_t>(coast, kSigCoastalBit);
    uint32_t gfx = Rd<uint32_t>(rect, kSigRectGfxDisp), r = Rd<uint32_t>(rect, kSigRectDisp);
    uint32_t id = Rd<uint32_t>(go, kSigGotoProvinceIdDisp);
    bool agree = bit && (bit & (bit - 1)) == 0 && gfx < 0x1000 && r < 0x1000 && id && id < 0x1000;
    if (!agree) {
        Log("ERROR: province data: call sites look different; area selection off");
        return;
    }
    g_provId = id;
    g_ppMapData = (void**)SigRipTarget(coast, kSigCoastalMapRel, kSigCoastalMapRel + 4);
    g_mapCount = Rd<uint32_t>(coast, kSigCoastalCountDisp);
    g_mapArray = Rd<uint32_t>(coast, kSigCoastalArrayDisp);
    g_mapFlags = Rd<uint32_t>(coast, kSigCoastalFlagsDisp);
    g_coastalBit = bit;
    g_provGfx = gfx;
    g_provRect = r;
    g_provContainer = Rd<uint8_t>(rail, kSigRailProvincesDisp);
    g_stateOwner = Rd<uint32_t>(owner, kSigOwnerStateOwnerDisp);
    Log("Province data: id +0x%x, map data %s (provinces +0x%x/+0x%x, flags +0x%x bit 0x%x), rect +0x%x/+0x%x, "
        "provinces +0x%x, state owner +0x%x",
        g_provId, SigDescribe(g_ppMapData).c_str(), g_mapArray, g_mapCount, g_mapFlags, g_coastalBit, g_provGfx, g_provRect,
        g_provContainer, g_stateOwner);
}

bool HaveProvinceIds() { return g_provId != 0; }

bool HaveProvinceData() {
    return g_ppMapData && g_mapCount && g_mapArray && g_mapFlags && g_coastalBit && g_provGfx && g_provRect && g_provContainer &&
           g_provId && g_stateOwner && ProvinceControllerOffset();
}

int ProvinceCount() {
    void* map = g_ppMapData ? *g_ppMapData : nullptr;
    int n = map ? Rd<int>(map, g_mapCount) : 0;
    return n > 0 && n < 1000000 ? n : 0;
}

int ProvinceIdOf(const void* province) { return province && g_provId ? Rd<int>(province, g_provId) : 0; }

bool ProvinceIsCoastal(int id) {
    void* map = g_ppMapData ? *g_ppMapData : nullptr;
    if (!map || id <= 0 || id >= ProvinceCount()) return false;
    void* const* arr = Rd<void* const*>(map, g_mapArray);
    const uint8_t* p = arr ? (const uint8_t*)arr[id] : nullptr;
    return p && (p[g_mapFlags] & g_coastalBit) != 0;
}

int ProvinceController(int id) {
    void* p = ProvinceObject(id);
    return p && ProvinceControllerOffset() ? Rd<int>(p, ProvinceControllerOffset()) : 0;
}

int StateOwner(int stateId) {
    void* s = StateById(stateId);
    return s && g_stateOwner ? Rd<int>(s, g_stateOwner) : 0;
}

bool ProvinceRect(int id, int rect[4]) {
    void* p = ProvinceObject(id);
    const uint8_t* gfx = p ? Rd<const uint8_t*>(p, g_provGfx) : nullptr;
    if (!gfx) return false;
    memcpy(rect, gfx + g_provRect, 4 * sizeof(int));
    return rect[2] >= 0 && rect[3] >= 0;
}

}  // namespace Engine
