// Engine access for the globe view (NOTES.md Part S): the map camera's per-frame update, its three
// engine cameras (frustum planes, screen rays), the visible area and the north/south edge clamp.
// Everything found by signatures; the offsets come from the code and are cross-checked where two
// places use them.
#include <windows.h>
#include <intrin.h>
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

using Update_t = void(__fastcall*)(void* mapCamera);
using Ray_t = void(__fastcall*)(void* camera, const int* px, float depth, float* dir, float* origin);
using Clamp_t = float*(__fastcall*)(void* mapCamera, float* out, const float* in);

uint8_t* g_area = nullptr;   // FUN_1412f3c90
uint8_t* g_ray = nullptr;    // FUN_142358dc0
uint8_t* g_clamp = nullptr;  // FUN_1412f4550
Update_t g_areaOrig = nullptr;
Ray_t g_rayOrig = nullptr;
Clamp_t g_clampOrig = nullptr;
GlobeHooks g_hooks = {};
void* g_cameraRayReturns[2] = {nullptr, nullptr};  // the map camera's own calls of the screen ray
void* g_cameraPointReturns[2] = {nullptr, nullptr};  // its calls of the screen-to-map helper (drag, zoom)
thread_local int t_cameraPoint = 0;                   // inside one of those
uint8_t* g_point = nullptr;                           // the screen-to-map helper
uint8_t* g_icons = nullptr;                           // the map icons' update
using Point_t = void*(__fastcall*)(void* a, void* out, void* screen, char flag);
using Icons_t = void(__fastcall*)(void* manager, void* ui, float x, void* r9);
Point_t g_pointOrig = nullptr;
Icons_t g_iconsOrig = nullptr;
uint32_t g_camPos = 0;                                // engine camera position (x, y, z)

void** g_ppMap = nullptr;                      // the map (width, height)
uint32_t g_mapW = 0, g_mapH = 0;               // int offsets in the map
uint32_t g_cam[3] = {0, 0, 0};                 // engine cameras in the map camera
uint32_t g_copyFlag[2] = {0, 0};               // their "in use" bytes
uint32_t g_eye = 0, g_look = 0;                // map camera eye / look-at
uint32_t g_eyeTarget = 0, g_lookTarget = 0;    // where they are moving to
uint32_t g_areaPts = 0;                        // the five area points
uint32_t g_planes = 0;                         // engine camera frustum planes
bool g_ok = false;

// --- Hooks: no C++ objects, faults in the plugin's callbacks are contained --------------------------

void CallAfterUpdate(void* mapCamera) {
    __try {
        g_hooks.afterUpdate(mapCamera);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall HookArea(void* mapCamera) {
    g_areaOrig(mapCamera);
    if (g_hooks.afterUpdate) CallAfterUpdate(mapCamera);
}

void CallScreenRay(void* camera, float* dir, float* origin, bool fromCamera) {
    __try {
        g_hooks.screenRay(camera, dir, origin, fromCamera);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall HookRay(void* camera, const int* px, float depth, float* dir, float* origin) {
    g_rayOrig(camera, px, depth, dir, origin);
    void* ret = _ReturnAddress();
    bool fromCamera = ret == g_cameraRayReturns[0] || ret == g_cameraRayReturns[1] || t_cameraPoint > 0;
    if (g_hooks.screenRay && dir && origin) CallScreenRay(camera, dir, origin, fromCamera);
}

void* __fastcall HookPoint(void* a, void* out, void* screen, char flag) {
    void* ret = _ReturnAddress();
    bool camera = ret == g_cameraPointReturns[0] || ret == g_cameraPointReturns[1];
    if (camera) ++t_cameraPoint;
    void* r = g_pointOrig(a, out, screen, flag);
    if (camera) --t_cameraPoint;
    return r;
}

bool CallHideIcons() {
    __try {
        return g_hooks.hideIcons();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// While bent: the icons see their camera(s) far up for the length of their update, so each hides by
// its own cutoff; the heights are put back right after (the matrices were made already).
void __fastcall HookIcons(void* manager, void* ui, float x, void* r9) {
    void* mapCamera = g_hooks.hideIcons && CallHideIcons() ? UiMapCamera() : nullptr;
    if (!mapCamera) {
        g_iconsOrig(manager, ui, x, r9);
        return;
    }
    float saved[3];
    for (int k = 0; k < 3; ++k) {
        uint8_t* cam = (uint8_t*)mapCamera + g_cam[k];
        saved[k] = Rd<float>(cam, g_camPos + 4);
        Wr<float>(cam, g_camPos + 4, 1.0e7f);
    }
    g_iconsOrig(manager, ui, x, r9);
    for (int k = 0; k < 3; ++k) Wr<float>((uint8_t*)mapCamera + g_cam[k], g_camPos + 4, saved[k]);
}

bool CallSkipClamp(void* mapCamera) {
    __try {
        return g_hooks.skipEdgeClamp(mapCamera);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float* __fastcall HookClamp(void* mapCamera, float* out, const float* in) {
    if (g_hooks.skipEdgeClamp && out && in && CallSkipClamp(mapCamera)) {
        float v[3] = {in[0], in[1], in[2]};
        out[0] = v[0], out[1] = v[1], out[2] = v[2];
        return out;
    }
    return g_clampOrig(mapCamera, out, in);
}

bool Fail(const char* why) {
    Log("Globe: %s; globe off", why);
    return false;
}

}  // namespace

void ResolveGlobe() {
    g_area = SigFindUnique("globe_area", kSigGlobeArea);
    g_ray = SigFindUnique("globe_screen_ray", kSigGlobeScreenRay);
    g_clamp = SigFindUnique("globe_edge_clamp", kSigGlobeEdgeClamp);
    uint8_t* frustum = SigFindUnique("globe_frustum", kSigGlobeFrustum);
    uint8_t* pick = SigFindUnique("globe_pick_cams", kSigGlobePickCams);
    uint8_t* camClamp = SigFindUnique("globe_camera_clamp", kSigGlobeCameraClamp);
    uint8_t* lookAt = SigFindUnique("camera_look_at", kSigCameraLookAt);  // attention_area's: the targets
    uint8_t* panRay = SigFindUnique("globe_pan_ray", kSigGlobePanRay);
    uint8_t* zoomRay = SigFindUnique("globe_zoom_ray", kSigGlobeZoomRay);
    uint8_t* camPoint1 = SigFindUnique("globe_camera_point_1", kSigGlobeCamPoint1);
    uint8_t* camPoint2 = SigFindUnique("globe_camera_point_2", kSigGlobeCamPoint2);
    g_icons = SigFindUnique("globe_icons", kSigGlobeIcons);
    if (!g_area || !g_ray || !g_clamp || !frustum || !pick || !camClamp || !lookAt || !panRay || !zoomRay || !camPoint1 ||
        !camPoint2 || !g_icons)
        return;
    g_point = SigFollowRel32(camPoint1 + kSigGlobeCamPoint1Call);
    if (g_point != SigFollowRel32(camPoint2 + kSigGlobeCamPoint2Call)) {
        Fail("the map camera's two map-point calls reach different functions");
        return;
    }
    g_cameraPointReturns[0] = camPoint1 + kSigGlobeCamPoint1Call + 5;
    g_cameraPointReturns[1] = camPoint2 + kSigGlobeCamPoint2Call + 5;
    if (SigFollowRel32(panRay + kSigGlobePanRayCall) != g_ray || SigFollowRel32(zoomRay + kSigGlobeZoomRayCall) != g_ray) {
        Fail("the map camera's own screen rays do not call the screen ray function");
        return;
    }
    g_cameraRayReturns[0] = panRay + kSigGlobePanRayCall + 5;
    g_cameraRayReturns[1] = zoomRay + kSigGlobeZoomRayCall + 5;
    uint8_t* store = SigFindIn(g_area, 0x400, kCodeGlobeAreaStore);
    if (!store) {
        Fail("the visible area's points were not found in its function");
        return;
    }
    uint8_t mainA = Rd<uint8_t>(g_area, kSigGlobeAreaMainCam), mainP = Rd<uint8_t>(pick, kSigGlobePickMain);
    void** mapA = (void**)SigRipTarget(g_clamp, kSigGlobeEdgeMapRel, kSigGlobeEdgeMapEnd);
    void** mapB = (void**)SigRipTarget(camClamp, kSigGlobeClampMapRel, kSigGlobeClampMapRel + 4);
    if (mainA != mainP) {
        Fail("the visible area and the picking disagree on the main camera");
        return;
    }
    if (mapA != mapB) {
        Fail("the edge clamp and the camera clamp read different maps");
        return;
    }
    g_ppMap = mapA;
    g_mapW = Rd<uint8_t>(camClamp, kSigGlobeClampWidthDisp);
    g_mapH = Rd<uint8_t>(camClamp, kSigGlobeClampHeightDisp);
    g_cam[0] = mainA;
    g_cam[1] = Rd<uint32_t>(pick, kSigGlobePickCopy1);
    g_cam[2] = Rd<uint32_t>(pick, kSigGlobePickCopy2);
    g_copyFlag[0] = Rd<uint32_t>(pick, kSigGlobePickCopy1Flag);
    g_copyFlag[1] = Rd<uint32_t>(pick, kSigGlobePickCopy2Flag);
    g_eye = Rd<uint32_t>(camClamp, kSigGlobeClampEyeDisp);
    g_look = Rd<uint32_t>(camClamp, kSigGlobeClampLookDisp);
    g_areaPts = Rd<uint32_t>(store, kCodeGlobeAreaStoreDisp);
    g_eyeTarget = Rd<uint32_t>(lookAt, 21);   // movsd [rcx+0x698], the eye target
    g_lookTarget = Rd<uint32_t>(lookAt, 45);  // movsd [rcx+0x6a4], the look-at target
    g_planes = Rd<uint32_t>(frustum, kSigGlobeFrustumPlanesDisp);
    uint32_t pos = Rd<uint32_t>(g_ray, kSigGlobeRayPosDisp);
    g_camPos = pos;
    // Shape checks: the copies follow each other at the main camera's stride; the flags, eye, look-at
    // and area lie past the three cameras; the planes past the camera's position.
    uint32_t stride = g_cam[1] - g_cam[0];
    bool shape = g_cam[1] > g_cam[0] && g_cam[2] - g_cam[1] == stride && stride > g_planes + 6 * 16 &&
                 g_copyFlag[1] == g_copyFlag[0] + 1 && g_copyFlag[0] > g_cam[2] + stride - 0x40 && g_look == g_eye + 12 && g_eyeTarget == g_eye + 0x18 && g_lookTarget == g_look + 0x18 &&
                 g_eye > g_copyFlag[1] && g_areaPts > g_look && g_areaPts < 0x1000 && g_planes > pos && g_mapH == g_mapW + 4;
    if (!shape) {
        Log("Globe: cameras +0x%x/+0x%x/+0x%x, flags +0x%x/+0x%x, eye +0x%x, look +0x%x, area +0x%x, planes +0x%x, pos +0x%x",
            g_cam[0], g_cam[1], g_cam[2], g_copyFlag[0], g_copyFlag[1], g_eye, g_look, g_areaPts, g_planes, pos);
        Fail("the camera layout does not look as analysed");
        return;
    }
    g_ok = true;
    Log("Globe: map %s (+0x%x/+0x%x), cameras +0x%x/+0x%x/+0x%x (in use +0x%x/+0x%x), eye +0x%x, look-at +0x%x, area +0x%x, "
        "planes +0x%x",
        SigDescribe(g_ppMap).c_str(), g_mapW, g_mapH, g_cam[0], g_cam[1], g_cam[2], g_copyFlag[0], g_copyFlag[1], g_eye, g_look,
        g_areaPts, g_planes);
}

bool HaveGlobe() { return g_ok; }

bool GlobeInstall(const GlobeHooks& hooks) {
    if (!g_ok) return false;
    g_hooks = hooks;
    bool ok = HookQueue(g_area, (void*)&HookArea, (void**)&g_areaOrig, "globe_area");
    ok = ok && HookQueue(g_ray, (void*)&HookRay, (void**)&g_rayOrig, "globe_screen_ray");
    ok = ok && HookQueue(g_clamp, (void*)&HookClamp, (void**)&g_clampOrig, "globe_edge_clamp");
    ok = ok && HookQueue(g_point, (void*)&HookPoint, (void**)&g_pointOrig, "globe_map_point");
    ok = ok && HookQueue(g_icons, (void*)&HookIcons, (void**)&g_iconsOrig, "globe_icons");
    if (!ok) {
        HookUnqueue(g_area);
        HookUnqueue(g_ray);
        HookUnqueue(g_clamp);
        HookUnqueue(g_point);
        HookUnqueue(g_icons);
    }
    return ok;
}

void* GlobeEngineCamera(void* mapCamera, int which) {
    return mapCamera && which >= 0 && which < 3 ? (uint8_t*)mapCamera + g_cam[which] : nullptr;
}

bool GlobeMapSize(float* w, float* h) {
    void* map = g_ppMap ? *g_ppMap : nullptr;
    if (!map) return false;
    int iw = Rd<int>(map, g_mapW), ih = Rd<int>(map, g_mapH);
    if (iw <= 0 || ih <= 0 || iw > 100000 || ih > 100000) return false;
    *w = (float)iw, *h = (float)ih;
    return true;
}

bool GlobeCameraPose(void* mapCamera, float eye[3], float look[3]) {
    if (!mapCamera) return false;
    memcpy(eye, (uint8_t*)mapCamera + g_eye, 12);
    memcpy(look, (uint8_t*)mapCamera + g_look, 12);
    return true;
}

void GlobeHideCopies(void* mapCamera) {
    Wr<uint8_t>(mapCamera, g_copyFlag[0], 0);
    Wr<uint8_t>(mapCamera, g_copyFlag[1], 0);
}

void GlobeShiftCamera(void* mapCamera, float dx, float dz) {
    for (uint32_t off : {g_eye, g_look, g_eyeTarget, g_lookTarget}) {
        Wr<float>(mapCamera, off, Rd<float>(mapCamera, off) + dx);
        Wr<float>(mapCamera, off + 8, Rd<float>(mapCamera, off + 8) + dz);
    }
}

void GlobeSetCameraHeight(void* mapCamera, float height) {
    // The eye (and its target) moved along the line from the look-at, so the camera keeps its angle.
    float eyeY = Rd<float>(mapCamera, g_eye + 4), lookY = Rd<float>(mapCamera, g_look + 4);
    if (!(eyeY - lookY > 1.0f)) return;
    float s = (height - lookY) / (eyeY - lookY);
    for (int k = 0; k < 3; ++k) {
        float e = Rd<float>(mapCamera, g_eye + 4 * k), l = Rd<float>(mapCamera, g_look + 4 * k);
        Wr<float>(mapCamera, g_eye + 4 * k, l + (e - l) * s);
        float et = Rd<float>(mapCamera, g_eyeTarget + 4 * k), lt = Rd<float>(mapCamera, g_lookTarget + 4 * k);
        Wr<float>(mapCamera, g_eyeTarget + 4 * k, lt + (et - lt) * s);
    }
}

void GlobeOpenFrustum(void* engineCamera) {
    // The planes point outwards (the update negates them): a point is outside when n.p + d > 0. A
    // zero normal with a very negative d keeps everything inside.
    const float open[4] = {0.0f, 0.0f, 0.0f, -1.0e30f};
    for (int k = 0; k < 6; ++k) memcpy((uint8_t*)engineCamera + g_planes + 16 * k, open, 16);
}

void GlobeAreaXRange(void* mapCamera, float* xmin, float* xmax) {
    *xmin = 1e30f, *xmax = -1e30f;
    for (int k = 0; k < 4; ++k) {
        float x = Rd<float>(mapCamera, g_areaPts + 12 * k);
        *xmin = x < *xmin ? x : *xmin;
        *xmax = x > *xmax ? x : *xmax;
    }
}

void GlobeAreaZRange(void* mapCamera, float* zmin, float* zmax) {
    *zmin = 1e30f, *zmax = -1e30f;
    for (int k = 0; k < 4; ++k) {
        float z = Rd<float>(mapCamera, g_areaPts + 12 * k + 8);
        *zmin = z < *zmin ? z : *zmin;
        *zmax = z > *zmax ? z : *zmax;
    }
}

void GlobeWidenArea(void* mapCamera, float cx, float cz, float factor) {
    for (int k = 0; k < 4; ++k) {
        float* p = (float*)((uint8_t*)mapCamera + g_areaPts + 12 * k);
        p[0] = cx + (p[0] - cx) * factor;
        p[2] = cz + (p[2] - cz) * factor;
    }
}

}  // namespace Engine
