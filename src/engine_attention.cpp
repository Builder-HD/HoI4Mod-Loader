// Engine access for attention areas (NOTES.md Part M): the game's gradient borders (the coloured,
// inward-fading borders of map modes), the camera, and the province data the area selector reads.
//
// Drawing: the map draws two gradient border layers at a time (manager +0x64 / +0x68, set by every
// map mode). The loader adds its own border objects (one per area) to the layer that is drawn on
// top (the second, else the first), so they show in every map mode. A province belongs to one
// border per layer, so inside an area the area's border replaces the layer's own one. The game
// rebuilds layers (map mode changes, daily refreshes); the loader takes its borders out before
// each rebuild of that layer and puts them back after it (hooks on RebuildAll / RebuildOne), and
// moves them when a map mode picks other layers (hook on SetActiveLayers).
//
// Hooks (global, DEVELOPING.md "Global hooks"):
//   SetActiveLayers(manager, pair)  map mode setup, main thread: records the manager, moves borders;
//   RebuildAll(manager, bool)       every layer: borders out before, back in after;
//   Rebuild(manager, type, bool)    one layer: the same when it is the layer holding them.
// A fault inside the loader's part is contained (SEH), logged once; then the loader's borders are
// forgotten (never freed again) and the hooks only pass through.
#include <math.h>
#include <string.h>

#include <atomic>
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
template <typename T>
void Wr(void* base, size_t offset, T v) {
    memcpy((uint8_t*)base + offset, &v, sizeof v);
}

// --- Gradient borders -----------------------------------------------------------------------------
using AddProvince_t = void(__fastcall*)(void* border, int id);
using SetColor_t = void(__fastcall*)(void* border, const void* color);
using SetMode_t = void(__fastcall*)(void* border, short mode);
using SetThickness_t = void(__fastcall*)(void* border, float thickness);
using Clear_t = void(__fastcall*)(void* border);
using Reset_t = void(__fastcall*)(void* border);
using New_t = void*(__fastcall*)(size_t bytes);
using Delete_t = void(__fastcall*)(void* p, size_t bytes);
using Ctor_t = void*(__fastcall*)(void* mem, void* layer);
using SetPair_t = void(__fastcall*)(void* manager, int* pair);
using RebuildAll_t = void(__fastcall*)(void* manager, char force);
using RebuildOne_t = void(__fastcall*)(void* manager, int type, char force);
using Alloc_t = void*(__fastcall*)(void* allocator, size_t bytes, size_t align);
using Free_t = void(__fastcall*)(void* allocator, void* p);
using WaitFill_t = void(__fastcall*)();

WaitFill_t g_waitFill = nullptr;  // waits for the layers' fill job (every border setter calls it first)
const size_t kLayerFillJob = 0x78;  // part of the setters' patterns

AddProvince_t g_addProvince = nullptr;
SetColor_t g_setColor = nullptr;
SetMode_t g_setMode = nullptr;
SetThickness_t g_setThickness = nullptr;
Clear_t g_clear = nullptr;
Reset_t g_reset = nullptr;
New_t g_new = nullptr;
Delete_t g_delete = nullptr;
Ctor_t g_ctor = nullptr;
SetPair_t g_setPair = nullptr, g_setPairOrig = nullptr;
RebuildAll_t g_rebuildAll = nullptr, g_rebuildAllOrig = nullptr;
RebuildOne_t g_rebuildOne = nullptr, g_rebuildOneOrig = nullptr;
uint32_t g_pairLo = 0, g_pairHi = 0;  // manager -> the two drawn layer types
uint32_t g_layers = 0;                // manager -> layer pointers [type]
int g_layerTypes = 0;                 // 23
// Layer: CPdxArray of borders { data, int cap, int count, allocator* }; border fields.
const size_t kArrData = 0, kArrCap = 8, kArrCount = 0xc, kArrAlloc = 0x10;
const size_t kBorderBytes = 0x88, kBorderHighlight = 0x57, kBorderDirty = 0x48;
// Border +0x4c (a short, read like the layer's outline cutoff define): -1 = the layer's own value
// (the soft fade), 0xf4 = what scripted map modes set without `thickness = yes` (to verify in game
// which edge each one gives; NOTES.md M5).
const short kCutoffLayer = -1, kCutoffSharp = 0xf4;

// State (main thread; the hooks run there, NOTES.md M2).
void* g_mgr = nullptr;             // the manager the last hook call was about
std::vector<void*> g_ours;         // borders the loader added
void* g_oursMgr = nullptr;
int g_oursHost = -1;               // the layer type holding them
std::vector<AttentionDraw> g_want; // what should be drawn
std::atomic<bool> g_faulted{false};
void (*g_onFault)(const char*) = nullptr;
volatile LONG g_threadLogged = 0;
bool g_frameSeen = false;  // AttentionTick ran: the main thread is known

void Fault(const char* where) {
    if (g_faulted.exchange(true)) return;
    Log("ERROR: attention_area: fault in %s; the loader's borders are forgotten and drawing is off", where);
    g_ours.clear();
    if (g_onFault) g_onFault(where);
}

void LogThreadOnce() {
    // Before the first frame the main thread is not known yet (IsMainThread() is false there, also
    // on the main thread): log only from the first call after it.
    if (MainThreadQueueAvailable() && !IsMainThread() && !g_frameSeen) return;
    if (InterlockedExchange(&g_threadLogged, 1)) return;
    Log("attention_area: gradient border hooks run on thread %lu (%s)", GetCurrentThreadId(),
        !MainThreadQueueAvailable() ? "main thread unknown: no frame hook" : IsMainThread() ? "the main thread" : "NOT the main thread");
}

void* LayerOf(void* mgr, int type) {
    if (!mgr || type < 0 || type >= g_layerTypes) return nullptr;
    void* const* layers = Rd<void* const*>(mgr, g_layers);
    return layers ? layers[type] : nullptr;
}

// The layer drawn on top: the second slot, else the first; -1 if the map draws none.
int HostType(void* mgr) {
    int hi = Rd<int>(mgr, g_pairHi), lo = Rd<int>(mgr, g_pairLo);
    if (hi >= 0 && hi < g_layerTypes) return hi;
    if (lo >= 0 && lo < g_layerTypes) return lo;
    return -1;
}

// --- Raw parts (no C++ objects: SEH) ---------------------------------------------------------------
// Takes our borders out of the host layer and frees them, as the game's own rebuild does. A border
// no longer in that layer's list is not ours any more (the game freed it): only forgotten.
bool RemoveRaw(void* layer, void* const* ours, int n, int* removed) {
    __try {
        *removed = 0;
        if (Rd<void*>(layer, kLayerFillJob)) g_waitFill();  // the list may be read by a fill job
        for (int k = 0; k < n; ++k) {
            void** data = Rd<void**>(layer, kArrData);
            int count = Rd<int>(layer, kArrCount);
            int at = -1;
            for (int i = 0; i < count && at < 0; ++i)
                if (data[i] == ours[k]) at = i;
            if (at < 0) continue;
            memmove(data + at, data + at + 1, (size_t)(count - at - 1) * sizeof(void*));
            Wr<int>(layer, kArrCount, count - 1);
            g_clear(ours[k]);
            g_delete(ours[k], kBorderBytes);
            ++*removed;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct RawColor {  // what the colour setter reads: CColor's floats at +0x10..+0x1c
    void* vt;
    void* pad;
    float r, g, b, a;
};

// Appends one border with the given look and provinces to the layer's list (grown with the
// list's own allocator, as the game grows it).
void* AddRaw(void* layer, const AttentionLook* look, const int* provinces, int n) {
    __try {
        if (Rd<void*>(layer, kLayerFillJob)) g_waitFill();  // the list may be read by a fill job
        void* b = g_new(kBorderBytes);
        if (!b) return nullptr;
        memset(b, 0, kBorderBytes);
        g_ctor(b, layer);
        int count = Rd<int>(layer, kArrCount), cap = Rd<int>(layer, kArrCap);
        if (count >= cap) {
            void* alloc = Rd<void*>(layer, kArrAlloc);
            int ncap = cap < 8 ? 8 : cap * 2;
            void** fresh = (void**)((Alloc_t)(*(void***)alloc)[1])(alloc, (size_t)ncap * sizeof(void*), 8);
            void** old = Rd<void**>(layer, kArrData);
            if (!fresh) {
                g_clear(b);
                g_delete(b, kBorderBytes);
                return nullptr;
            }
            if (count) memcpy(fresh, old, (size_t)count * sizeof(void*));
            Wr<void**>(layer, kArrData, fresh);
            Wr<int>(layer, kArrCap, ncap);
            if (old) ((Free_t)(*(void***)alloc)[2])(alloc, old);
        }
        Rd<void**>(layer, kArrData)[count] = b;
        Wr<int>(layer, kArrCount, count + 1);
        RawColor c = {nullptr, nullptr, look->r, look->g, look->b, look->a};
        g_setColor(b, &c);
        g_setMode(b, look->glow ? kCutoffLayer : kCutoffSharp);
        g_setThickness(b, look->width);
        ((uint8_t*)b)[kBorderHighlight] = look->pulse ? 1 : 0;
        ((uint8_t*)b)[kBorderDirty] = 1;
        for (int i = 0; i < n; ++i) g_addProvince(b, provinces[i]);
        return b;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The layer's own borders forget their provinces, so the game's next rebuild of the layer adds
// every province again. Needed after our borders leave: a province we took keeps no owner, and
// the game's rebuild skips borders whose province list is unchanged (FUN_140fb82c0), which left
// those provinces without their country colour (first in-game test, NOTES.md M6).
bool ResetLayerRaw(void* layer) {
    __try {
        if (Rd<void*>(layer, kLayerFillJob)) g_waitFill();
        void** data = Rd<void**>(layer, kArrData);
        int count = Rd<int>(layer, kArrCount);
        for (int i = 0; i < count; ++i)
            if (data[i]) g_reset(data[i]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RebuildOneRaw(void* mgr, int type) {
    __try {
        g_rebuildOneOrig(mgr, type, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- Taking borders out and putting them back ------------------------------------------------------
// restore: the provinces our borders took are not taken again right away (areas removed or the
// layer changes), so the layer's own borders are reset for the rebuild that follows every call.
void RemoveOurs(bool restore) {
    if (g_ours.empty()) return;
    void* layer = LayerOf(g_oursMgr, g_oursHost);
    int removed = 0;
    if (!layer || !RemoveRaw(layer, g_ours.data(), (int)g_ours.size(), &removed)) {
        if (layer) Fault("removing borders");
        g_ours.clear();
        return;
    }
    if (restore && !ResetLayerRaw(layer)) {
        Fault("resetting the layer");
        return;
    }
    if (removed != (int)g_ours.size())
        Log("attention_area: %d of %zu borders were no longer in layer %d (the game freed them)", (int)g_ours.size() - removed,
            g_ours.size(), g_oursHost);
    g_ours.clear();
}

void AddOurs(void* mgr) {
    if (g_faulted.load() || g_want.empty() || !mgr) return;
    int host = HostType(mgr);
    void* layer = LayerOf(mgr, host);
    if (!layer) return;
    for (const AttentionDraw& a : g_want) {
        void* b = AddRaw(layer, &a.look, a.provinces.data(), (int)a.provinces.size());
        if (!b) {
            Fault("adding a border");
            return;
        }
        g_ours.push_back(b);
    }
    g_oursMgr = mgr;
    g_oursHost = host;
}

// --- Detours ---------------------------------------------------------------------------------------
void __fastcall SetPairDetour(void* mgr, int* pair) {
    LogThreadOnce();
    if (g_faulted.load()) return g_setPairOrig(mgr, pair);
    g_mgr = mgr;
    int oldHost = g_ours.empty() ? -1 : g_oursHost;
    int newHi = pair ? pair[1] : -1, newLo = pair ? pair[0] : -1;
    int newHost = newHi >= 0 && newHi < g_layerTypes ? newHi : newLo >= 0 && newLo < g_layerTypes ? newLo : -1;
    bool move = !g_ours.empty() && (newHost != oldHost || g_oursMgr != mgr);
    // The old layer is rebuilt below (it keeps no provinces of ours); same layer: re-added at once.
    if (move) RemoveOurs(oldHost >= 0 && oldHost != newHost && g_oursMgr == mgr);
    g_setPairOrig(mgr, pair);
    if (move) {
        // The old layer lost provinces to our borders: let the game redo it before it is drawn again.
        if (oldHost >= 0 && oldHost != newHost && !RebuildOneRaw(mgr, oldHost)) Fault("rebuilding the old layer");
        AddOurs(mgr);
    }
}

void __fastcall RebuildAllDetour(void* mgr, char force) {
    LogThreadOnce();
    if (g_faulted.load()) return g_rebuildAllOrig(mgr, force);
    g_mgr = mgr;
    bool ours = !g_ours.empty() || !g_want.empty();
    if (ours) RemoveOurs(false);  // the same areas go back right after
    g_rebuildAllOrig(mgr, force);
    if (ours) AddOurs(mgr);
}

void __fastcall RebuildOneDetour(void* mgr, int type, char force) {
    if (g_faulted.load()) return g_rebuildOneOrig(mgr, type, force);
    g_mgr = mgr;
    bool host = (!g_ours.empty() && type == g_oursHost && mgr == g_oursMgr) || (g_ours.empty() && !g_want.empty() && type == HostType(mgr));
    if (host) RemoveOurs(false);
    g_rebuildOneOrig(mgr, type, force);
    if (host) AddOurs(mgr);
}

// --- Camera ------------------------------------------------------------------------------------------
using CentreOn_t = void(__fastcall*)(void* ui, int province);
using Camera_t = uint8_t*(__fastcall*)(void* ui);
using LookAt_t = void(__fastcall*)(void* camera, const float* pos);
using SetHeight_t = void(__fastcall*)(void* camera, float height);
void** g_ppUi = nullptr;
uint32_t g_centreSlot = 0, g_cameraSlot = 0;
LookAt_t g_lookAt = nullptr;
SetHeight_t g_setHeight = nullptr;
uint32_t g_heightMin = 0, g_heightMax = 0;
// Camera target fields (part of kSigCameraLookAt's pattern): eye x, height, z; look-at x, y, z.
const size_t kEyeX = 0x698, kEyeY = 0x69c, kLookX = 0x6a4, kLookZ = 0x6ac;

struct FocusIn {
    int box[4];
    int ref, ref2;
    float height, fit;
    int rect1[4], rect2[4];
};

bool FocusRaw(const FocusIn* in, float out[6]) {
    __try {
        void* ui = *g_ppUi;
        if (!ui) return false;
        uint8_t* cam = ((Camera_t)(*(void***)ui)[g_cameraSlot])(ui);
        if (!cam) return false;
        CentreOn_t centre = (CentreOn_t)(*(void***)ui)[g_centreSlot];
        float before = Rd<float>(cam, kLookX) + Rd<float>(cam, kLookZ);
        centre(ui, in->ref);
        float lx = Rd<float>(cam, kLookX), lz = Rd<float>(cam, kLookZ);
        float sign = 1.0f;
        float p1y = in->rect1[1] + in->rect1[3] * 0.5f, p1x = in->rect1[0] + in->rect1[2] * 0.5f;
        if (in->ref2 > 0) {
            float p2y = in->rect2[1] + in->rect2[3] * 0.5f;
            centre(ui, in->ref2);
            float lz2 = Rd<float>(cam, kLookZ);
            if (fabsf(p2y - p1y) > 4.0f) sign = (lz2 - lz) / (p2y - p1y) < 0 ? -1.0f : 1.0f;
        }
        float bx = in->box[0] + in->box[2] * 0.5f, by = in->box[1] + in->box[3] * 0.5f;
        float pos[3] = {lx + (bx - p1x), Rd<float>(cam, kEyeY), lz + sign * (by - p1y)};
        g_lookAt(cam, pos);
        float h = in->height;
        if (h <= 0) {
            float w = (float)in->box[2] / 1.78f, t = (float)in->box[3];
            h = (w > t ? w : t) * in->fit;
        }
        g_setHeight(cam, h);
        out[0] = lx, out[1] = lz, out[2] = sign, out[3] = pos[0], out[4] = pos[2], out[5] = Rd<float>(cam, kEyeY);
        (void)before;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- Province data ---------------------------------------------------------------------------------
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

// --- Resolution --------------------------------------------------------------------------------------
void ResolveBorders() {
    uint8_t* add = SigFindUnique("border_add_province", kSigBorderAddProvince);
    uint8_t* color = SigFindUnique("border_set_color", kSigBorderSetColor);
    uint8_t* mode = SigFindUnique("border_set_mode", kSigBorderSetMode);
    uint8_t* thick = SigFindUnique("border_set_thickness", kSigBorderSetThickness);
    uint8_t* clear = SigFindUnique("border_clear", kSigBorderClear);
    uint8_t* reset = SigFindUnique("border_reset", kSigBorderReset);
    uint8_t* alloc = SigFindUnique("border_alloc", kSigBorderAlloc);
    uint8_t* fre = SigFindUnique("border_free", kSigBorderFree);
    uint8_t* pair = SigFindUnique("border_set_pair", kSigBorderSetPair);
    uint8_t* all = SigFindUnique("border_rebuild_all", kSigBorderRebuildAll);
    uint8_t* one = SigFindUnique("border_rebuild_one", kSigBorderRebuildOne);
    if (!add || !color || !mode || !thick || !clear || !reset || !alloc || !fre || !pair || !all || !one) return;
    uint8_t* ctor = SigFollowRel32(alloc + kSigBorderAllocCtor);
    bool ctorOk = SigMatchAt(ctor, kCodeBorderCtor) && SigFindIn(ctor, 0x98, kCodeBorderCtorLayer) != nullptr;
    bool clearOk = SigFollowRel32(fre + kSigBorderFreeClear) == clear &&
                   SigFollowRel32(color + kSigBorderColorWaitCall) == SigFollowRel32(mode + 0x24);  // the same fill wait
    uint32_t layers = Rd<uint8_t>(all, kSigRebuildAllLayersDisp);
    int last = Rd<uint8_t>(all, kSigRebuildAllLastImm);
    bool agree = ctorOk && clearOk && layers == Rd<uint8_t>(one, kSigRebuildOneLayersDisp) &&
                 last == Rd<uint8_t>(one, kSigRebuildOneLastImm) && last > 0 && last < 64;
    if (!agree) {
        Log("ERROR: attention_area: the gradient border call sites disagree (constructor %d, clear %d); drawing off", ctorOk, clearOk);
        return;
    }
    g_addProvince = (AddProvince_t)add;
    g_setColor = (SetColor_t)color;
    g_setMode = (SetMode_t)mode;
    g_setThickness = (SetThickness_t)thick;
    g_clear = (Clear_t)clear;
    g_reset = (Reset_t)reset;
    g_new = (New_t)SigFollowRel32(alloc + kSigBorderAllocNew);
    g_delete = (Delete_t)SigFollowRel32(fre + kSigBorderFreeDelete);
    g_ctor = (Ctor_t)ctor;
    g_waitFill = (WaitFill_t)SigFollowRel32(color + kSigBorderColorWaitCall);
    g_setPair = (SetPair_t)pair;
    g_rebuildAll = (RebuildAll_t)all;
    g_rebuildOne = (RebuildOne_t)one;
    g_pairLo = Rd<uint8_t>(pair, kSigBorderPairLoDisp);
    g_pairHi = Rd<uint8_t>(pair, kSigBorderPairHiDisp);
    g_layers = layers;
    g_layerTypes = last + 1;
    Log("Attention areas: %d border layers at manager +0x%x, drawn pair +0x%x/+0x%x, new %s, delete %s", g_layerTypes, g_layers,
        g_pairLo, g_pairHi, SigDescribe((void*)g_new).c_str(), SigDescribe((void*)g_delete).c_str());
}

void ResolveCamera() {
    uint8_t* go = SigFindUnique("goto_state_tail", kSigGotoStateTail);
    uint8_t* zoom = SigFindUnique("debug_zoom", kSigDebugZoom);
    uint8_t* look = SigFindUnique("camera_look_at", kSigCameraLookAt);
    if (!go || !zoom || !look) return;
    uint32_t cam = Rd<uint32_t>(go, kSigGotoCameraDisp), centre = Rd<uint32_t>(go, kSigGotoCentreDisp);
    uint32_t mn = Rd<uint32_t>(zoom, kSigZoomMinDisp), mx = Rd<uint32_t>(zoom, kSigZoomMaxDisp);
    bool agree = cam == Rd<uint32_t>(zoom, kSigZoomCameraDisp) && cam % 8 == 0 && centre % 8 == 0 && cam < 0x1000 &&
                 centre < 0x1000 && mx == mn + 4 && mn > kLookZ && mn < 0x1000;
    if (!agree) {
        Log("ERROR: attention_area: goto_state and debug_zoom disagree on the camera; camera focus off");
        return;
    }
    g_ppUi = (void**)SigRipTarget(go, kSigGotoUiRel, kSigGotoUiRel + 4);
    g_centreSlot = centre / 8;
    g_cameraSlot = cam / 8;
    g_setHeight = (SetHeight_t)SigFollowRel32(go + kSigGotoSetHeight);
    g_lookAt = (LookAt_t)look;
    g_heightMin = mn;
    g_heightMax = mx;
    g_provId = Rd<uint32_t>(go, kSigGotoProvinceIdDisp);
    Log("Attention areas: UI %s, centre slot %u, camera slot %u, height limits +0x%x/+0x%x, province id +0x%x",
        SigDescribe(g_ppUi).c_str(), g_centreSlot, g_cameraSlot, g_heightMin, g_heightMax, g_provId);
}

void ResolveProvinces() {
    uint8_t* coast = SigFindUnique("state_coastal", kSigStateCoastal);
    uint8_t* rect = SigFindUnique("province_rect", kSigProvinceRect);
    uint8_t* rail = SigFindUnique("rail_controller", kSigRailController);
    uint8_t* owner = SigFindUnique("state_owner_check", kSigStateOwnerCheck);
    if (!coast || !rect || !rail || !owner) return;
    uint8_t bit = Rd<uint8_t>(coast, kSigCoastalBit);
    uint32_t gfx = Rd<uint32_t>(rect, kSigRectGfxDisp), r = Rd<uint32_t>(rect, kSigRectDisp);
    bool agree = bit && (bit & (bit - 1)) == 0 && gfx < 0x1000 && r < 0x1000 && g_provId && g_provId < 0x1000;
    if (!agree) {
        Log("ERROR: attention_area: province data call sites look different; area selection off");
        return;
    }
    g_ppMapData = (void**)SigRipTarget(coast, kSigCoastalMapRel, kSigCoastalMapRel + 4);
    g_mapCount = Rd<uint32_t>(coast, kSigCoastalCountDisp);
    g_mapArray = Rd<uint32_t>(coast, kSigCoastalArrayDisp);
    g_mapFlags = Rd<uint32_t>(coast, kSigCoastalFlagsDisp);
    g_coastalBit = bit;
    g_provGfx = gfx;
    g_provRect = r;
    g_provContainer = Rd<uint8_t>(rail, kSigRailProvincesDisp);
    g_stateOwner = Rd<uint32_t>(owner, kSigOwnerStateOwnerDisp);
    Log("Attention areas: map data %s (provinces +0x%x/+0x%x, flags +0x%x bit 0x%x), province rect +0x%x/+0x%x, "
        "provinces +0x%x, state owner +0x%x",
        SigDescribe(g_ppMapData).c_str(), g_mapArray, g_mapCount, g_mapFlags, g_coastalBit, g_provGfx, g_provRect,
        g_provContainer, g_stateOwner);
}

// --- Province group outlines (CProvinceGroupHighlight) ---------------------------------------------
using CreateHighlight_t = void(__fastcall*)(void* owner, void** sharedOut, const void* provinces, const int* style);
CreateHighlight_t g_createHighlight = nullptr;
uint32_t g_hlOwner = 0;            // ui -> highlight owner (list of weak references at +0x430)
int g_hlSecond = 0;                // construction mode's priority (logged for comparison)
const int kOutlinePriority = 100;  // the loader's outline: above the game's own outlines
const int* g_paletteCount = nullptr;   // float count of BORDER_COLOR_CUSTOM_HIGHLIGHTS
void* g_hlShared[2] = {nullptr, nullptr};  // our shared_ptr { object, control block }
// The renderer's colour table: palette colour kWhiteSlot is made white there (vanilla colour 14,
// FACTION_THEATER_HIGHLIGHT_COLOR_INDEX, the faction theatre map icon hover: that outline turns
// white too). A colour past the vanilla 15 is drawn as colour 14 (second in-game test).
const int kWhiteSlot = 14;
uint8_t* g_tableFn = nullptr;
void(__fastcall* g_tableOrig)(void*) = nullptr;
int g_tableFixed = 0;        // fixed colours before the palette in the table (10)
uint32_t g_tableCount = 0;   // renderer -> the table's count field
bool g_tableWhite = false;   // the slot was made white (the table was built after the hook)

struct IntArray {  // CPdxArray<int> as the highlight constructor reads it: data, count at +0xc
    const int* data;
    int cap;
    int count;
    void* allocator;
};

// std::shared_ptr release in the game's own MSVC layout: uses +8, weaks +0xc; vtable slot 0
// destroys the object, slot 1 the control block.
bool ReleaseRaw(void* ctrl) {
    __try {
        if (InterlockedDecrement((volatile LONG*)((uint8_t*)ctrl + 8)) == 0) {
            ((void(__fastcall*)(void*))(*(void***)ctrl)[0])(ctrl);
            if (InterlockedDecrement((volatile LONG*)((uint8_t*)ctrl + 0xc)) == 0)
                ((void(__fastcall*)(void*))(*(void***)ctrl)[1])(ctrl);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CreateRaw(void* owner, const IntArray* provinces, const int* style, void** out) {
    __try {
        g_createHighlight(owner, out, provinces, style);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ResolveHighlight() {
    uint8_t* create = SigFindUnique("highlight_create", kSigHighlightCreate);
    uint8_t* site = SigFindUnique("highlight_create_site", kSigHighlightCreateSite);
    uint8_t* pal = SigFindUnique("highlight_palette_count", kSigHighlightPaletteCount);
    if (!create || !site || !pal) return;
    const int* style = (const int*)SigRipTarget(site, kSigHlStyleRel, kSigHlStyleRel + 4);
    uint32_t owner = Rd<uint32_t>(site, kSigHlOwnerDisp);
    bool agree = SigFollowRel32(site + kSigHlCreateCall) == create &&
                 (void**)SigRipTarget(site, kSigHlUiRel, kSigHlUiRel + 4) == g_ppUi && g_ppUi && owner < 0x2000 && owner % 8 == 0 &&
                 style[0] >= 0 && style[0] < 64 && style[1] >= 0 && style[1] < 64;
    if (!agree) {
        Log("ERROR: attention_area: the province outline call site disagrees; outlines off");
        return;
    }
    g_createHighlight = (CreateHighlight_t)create;
    g_hlOwner = owner;
    g_hlSecond = style[1];
    g_paletteCount = (const int*)SigRipTarget(pal, kSigPaletteCountRel, kSigPaletteCountRel + 4);
    Log("Attention areas: province outlines: owner ui+0x%x, construction style {colour id %d, %d}, palette %s", g_hlOwner, style[0],
        style[1], SigDescribe(g_paletteCount).c_str());
    uint8_t* table = SigFindUnique("highlight_colour_table", kSigHighlightColourTable);
    int fixed = table ? Rd<uint8_t>(table, kSigTableFixedImm) : 0;
    uint32_t count = table ? Rd<uint32_t>(table, kSigTableCountDisp) : 0;
    if (!table || (const int*)SigRipTarget(table, kSigTablePaletteRel, kSigTablePaletteRel + 4) != g_paletteCount || fixed <= 0 ||
        fixed > 32 || count < 0x10 || count > 0x10000) {
        Log("attention_area: the outline colour table was not found where expected; outlines keep vanilla colours");
        return;
    }
    g_tableFn = table;
    g_tableFixed = fixed;
    g_tableCount = count;
}

// The renderer's colour table (renderer + count field - 0xc: float4 data; count at the field):
// right after the game builds it, the loader's slot becomes white. The game builds it once, in
// the map graphics setup, from the built-in palette (the defines are read later), so a defines
// change never reaches it (third in-game test, NOTES.md M7).
bool WhiteRaw(void* renderer) {
    __try {
        int count = Rd<int>(renderer, g_tableCount);
        float* data = Rd<float*>(renderer, g_tableCount - 0xc);
        int slot = g_tableFixed + kWhiteSlot;
        if (!data || slot >= count) return false;
        for (int k = 0; k < 4; ++k) data[slot * 4 + k] = 1.0f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall TableDetour(void* renderer) {
    g_tableOrig(renderer);
    bool ok = WhiteRaw(renderer);
    g_tableWhite = ok;
    Log("attention_area: outline colour %d %s", kWhiteSlot, ok ? "made white in the renderer's colour table" : "NOT changed (table not as expected)");
}

}  // namespace

void ResolveAttention() {
    ResolveBorders();
    ResolveCamera();  // before ResolveProvinces: the province id offset comes from goto_state
    ResolveProvinces();
    ResolveHighlight();  // after ResolveCamera: cross-checks the UI global
}

bool HaveHighlight() { return g_createHighlight && g_hlOwner && g_paletteCount && g_ppUi && g_tableFixed; }

int HighlightPaletteSize() {
    int n = g_paletteCount ? *g_paletteCount : 0;
    return n > 0 && n < 4096 ? n / 4 : 0;
}

int HighlightWhiteIndex() { return g_tableWhite ? kWhiteSlot : -1; }

// --- The owner's hovered item (drawn before every outline, colour id 5) ----------------------------
namespace {
using SetHovered_t = void(__fastcall*)(void* owner, void* province);
uint8_t* g_setHovered = nullptr;
SetHovered_t g_setHoveredOrig = nullptr;
uint32_t g_hoverType = 0, g_hoverItem = 0;  // owner -> type (0 province, 1 state), item
HoverFilter_t g_hoverFilter = nullptr;

bool HoverFilterRaw(void* province, bool* drop) {
    __try {
        *drop = g_hoverFilter(ProvinceIdOf(province));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall SetHoveredDetour(void* owner, void* province) {
    if (g_hoverFilter && province && !g_faulted.load()) {
        bool drop = false;
        if (!HoverFilterRaw(province, &drop)) Fault("the hovered-province hook");
        else if (drop) province = nullptr;  // nothing hovered: the loader's outline shows instead
    }
    g_setHoveredOrig(owner, province);
}

void* HoverOwner() {
    void* ui = g_ppUi ? *g_ppUi : nullptr;
    return ui && g_hlOwner ? Rd<void*>(ui, g_hlOwner) : nullptr;
}

bool ClearHoveredRaw(void* owner, int (*idOf)(const void*), HoverFilter_t filter, bool* cleared) {
    __try {
        *cleared = false;
        if (Rd<int>(owner, g_hoverType) != 0) return true;  // a state, not a province
        void* p = Rd<void*>(owner, g_hoverItem);
        if (p && filter(idOf(p))) {
            g_setHoveredOrig(owner, nullptr);
            *cleared = true;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

bool HookHoveredProvince(HoverFilter_t filter) {
    uint8_t* p = SigFindUnique("hover_setters", kSigHoverSetters);
    if (!p || !g_provId) return false;
    uint32_t type = Rd<uint32_t>(p, kSigHoverTypeDisp), item = Rd<uint32_t>(p, kSigHoverItemDisp);
    if (type != Rd<uint32_t>(p, kSigHoverStateTypeDisp) || item != Rd<uint32_t>(p, kSigHoverStateItemDisp) || item != type + 8 ||
        type > 0x2000) {
        Log("attention_area: the hovered-item setters disagree; the game's start-province mark stays");
        return false;
    }
    g_setHovered = p;
    g_hoverType = type;
    g_hoverItem = item;
    g_hoverFilter = filter;
    Log("Attention areas: hovered item at owner +0x%x/+0x%x", type, item);
    return HookQueue(p, (void*)&SetHoveredDetour, (void**)&g_setHoveredOrig, "set_hovered_province");
}

bool ClearHoveredProvinceIf(HoverFilter_t filter) {
    void* owner = g_setHoveredOrig ? HoverOwner() : nullptr;
    bool cleared = false;
    if (owner && !ClearHoveredRaw(owner, &ProvinceIdOf, filter, &cleared)) Fault("clearing the hovered province");
    return cleared;
}

bool HookHighlightWhite() {
    return g_tableFn && HookQueue(g_tableFn, (void*)&TableDetour, (void**)&g_tableOrig, "highlight_colour_table");
}

void HideHighlight() {
    void* ctrl = g_hlShared[1];
    g_hlShared[0] = g_hlShared[1] = nullptr;
    if (ctrl && !ReleaseRaw(ctrl)) Log("ERROR: attention_area: releasing a province outline faulted (contained)");
}

bool ShowHighlight(const std::vector<int>& provinces, int paletteIndex) {
    HideHighlight();
    if (!HaveHighlight() || provinces.empty() || paletteIndex < 0 || paletteIndex >= HighlightPaletteSize()) return false;
    void* ui = *g_ppUi;
    void* owner = ui ? Rd<void*>(ui, g_hlOwner) : nullptr;
    if (!owner) return false;
    IntArray arr = {provinces.data(), (int)provinces.size(), (int)provinces.size(), nullptr};
    // The style's first byte is the colour id in the renderer's table (FUN_14130f0e0 reads it): ids
    // 0-9 are the fixed border colours, 10 + i palette colour i. Construction mode's {10, 2} is
    // palette 0 (teal); the earlier tests drew {15, 2} as palette 5 (green) and {10, x} as teal.
    // The second number is the priority: the owner's list is sorted by it, highest first, and an edge
    // shared by several outlines gets the first one's colour (FUN_140bb2cd0 / FUN_14130f0e0). The game's
    // own start-province outline of a hovered railway entry ranks above construction mode's 2 and took
    // the start province's edges (sixth test), so the loader's outline ranks high.
    int style[2] = {g_tableFixed + paletteIndex, kOutlinePriority};
    void* out[2] = {nullptr, nullptr};
    if (!CreateRaw(owner, &arr, style, out)) {
        Log("ERROR: attention_area: creating a province outline faulted (contained)");
        return false;
    }
    g_hlShared[0] = out[0];
    g_hlShared[1] = out[1];
    return out[1] != nullptr;
}

bool HaveAttentionBorders() {
    return g_addProvince && g_setColor && g_setMode && g_setThickness && g_clear && g_reset && g_new && g_delete && g_ctor && g_waitFill && g_setPair &&
           g_rebuildAll && g_rebuildOne && g_layers && g_layerTypes > 0;
}
bool HaveAttentionCamera() { return g_ppUi && g_centreSlot && g_cameraSlot && g_setHeight && g_lookAt && g_heightMin; }
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

bool HookAttention(void (*onFault)(const char*)) {
    if (!HaveAttentionBorders()) return false;
    g_onFault = onFault;
    return HookQueue((void*)g_setPair, (void*)&SetPairDetour, (void**)&g_setPairOrig, "border_set_pair") &&
           HookQueue((void*)g_rebuildAll, (void*)&RebuildAllDetour, (void**)&g_rebuildAllOrig, "border_rebuild_all") &&
           HookQueue((void*)g_rebuildOne, (void*)&RebuildOneDetour, (void**)&g_rebuildOneOrig, "border_rebuild_one");
}

void SetAttentionDraw(const std::vector<AttentionDraw>& areas) {
    if (g_faulted.load() || !g_rebuildOneOrig) return;
    int oldHost = g_ours.empty() ? -1 : g_oursHost;
    void* oldMgr = g_oursMgr;
    RemoveOurs(oldHost >= 0 && oldMgr == g_mgr && g_mgr);  // rebuilt just below
    g_want = areas;
    // The layer gets its provinces back from the game's own rebuild, then ours go on top.
    void* mgr = g_mgr;
    if (oldHost >= 0 && oldMgr == mgr && mgr && !RebuildOneRaw(mgr, oldHost)) return Fault("rebuilding the layer");
    AttentionTick();
}

void AttentionTick() {
    g_frameSeen = true;
    // Areas wanted but not drawn yet (added before the map's first layer setup this game).
    if (!g_faulted.load() && g_ours.empty() && !g_want.empty() && g_mgr) AddOurs(g_mgr);
}

void ForgetAttentionDraw() {
    g_ours.clear();
    g_oursMgr = nullptr;
    g_oursHost = -1;
    g_want.clear();
    g_mgr = nullptr;
}

// --- The UI's centre-on-province (right-click on a queue entry, goto_state, ...) -------------------
namespace {
using CentreObj_t = void(__fastcall*)(void* ui, void* province);
CentreObj_t g_centreObj = nullptr, g_centreObjOrig = nullptr;
CentreFilter_t g_centreFilter = nullptr;
bool g_inOwnFocus = false;  // FocusCamera calibrates with the same function: no filter then

bool FilterRaw(void* province, bool* take) {
    __try {
        *take = g_centreFilter(ProvinceIdOf(province));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall CentreObjDetour(void* ui, void* province) {
    if (g_centreFilter && province && !g_inOwnFocus && !g_faulted.load()) {
        bool take = false;
        if (!FilterRaw(province, &take)) Fault("the centre-on-province hook");
        else if (take) return;  // the filter moves the camera itself
    }
    g_centreObjOrig(ui, province);
}
}  // namespace

bool HookCentreOnProvince(CentreFilter_t filter) {
    if (!HaveAttentionCamera() || !g_provId) return false;
    size_t n = 0;
    void** vt = FindVtableByRtti(".?AVCInGameIdler@@", &n);
    uint8_t* tail = vt && n > g_centreSlot ? SigFindIn((uint8_t*)vt[g_centreSlot], 0x200, kCodeIdlerCentreTail) : nullptr;
    uint32_t disp = tail ? Rd<uint32_t>(tail, kCodeCentreTailDisp) : 0;
    if (!tail || disp % 8 || disp / 8 >= n) {
        Log("attention_area: the UI's centre-on-province has another shape; right-click focus off");
        return false;
    }
    g_centreObj = (CentreObj_t)vt[disp / 8];
    g_centreFilter = filter;
    Log("Attention areas: centre on province object = idler slot %u (%s)", disp / 8, SigDescribe((void*)g_centreObj).c_str());
    return HookQueue((void*)g_centreObj, (void*)&CentreObjDetour, (void**)&g_centreObjOrig, "idler_centre_province");
}

bool FocusCamera(const int box[4], int ref, int ref2, float height, float fitFactor) {
    if (!HaveAttentionCamera() || !GameState() || ref <= 0) return false;
    FocusIn in{};
    memcpy(in.box, box, sizeof in.box);
    in.ref = ref;
    in.ref2 = ref2;
    in.height = height;
    in.fit = fitFactor > 0 ? fitFactor : 1.0f;
    if (!ProvinceRect(ref, in.rect1)) return false;
    if (ref2 > 0 && !ProvinceRect(ref2, in.rect2)) in.ref2 = 0;
    float out[6] = {};
    g_inOwnFocus = true;
    bool ok = FocusRaw(&in, out);
    g_inOwnFocus = false;
    if (!ok) {
        Log("ERROR: attention_area: the camera focus faulted (contained)");
        return false;
    }
    Log("attention_area: focus box %d,%d %dx%d: province %d centred at look-at %.0f/%.0f, z sign %.0f; look-at %.0f/%.0f, "
        "height %.0f",
        box[0], box[1], box[2], box[3], ref, out[0], out[1], out[2], out[3], out[4], out[5]);
    return true;
}

}  // namespace Engine
