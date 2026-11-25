// camera_range (engine tier, needs lifecycle): a mod may raise NFrontend.CAMERA_MAX_HEIGHT (vanilla
// 3000), but the map camera's far plane is a fixed 4000 (a constant in the exe, set when the camera
// is made), so from about 3000 up the map's far side is cut off. Each frame the in-game map camera's
// three engine cameras get a far plane of at least the define times 4/3 (vanilla's ratio); the
// projection is rebuilt from it on the camera's next update. A vanilla height leaves everything
// alone. NOTES.md Part T.
#include <windows.h>

#include <stdint.h>
#include <string.h>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "signatures.h"

namespace {

const float kVanillaRatio = 4000.0f / 3000.0f;
uint32_t g_farOff = 0;               // engine camera: far plane
const float* g_maxHeight = nullptr;  // NFrontend.CAMERA_MAX_HEIGHT (the define's float)
bool g_logged = false;

void Frame() {
    void* cam = Engine::GameState() ? Engine::UiMapCamera() : nullptr;
    if (!cam || !g_maxHeight) return;
    float want = *g_maxHeight * kVanillaRatio;
    for (int k = 0; k < 3; ++k) {
        uint8_t* ec = (uint8_t*)Engine::GlobeEngineCamera(cam, k);
        if (!ec) continue;
        float farPlane = 0;
        memcpy(&farPlane, ec + g_farOff, 4);
        if (farPlane < want) {
            memcpy(ec + g_farOff, &want, 4);
            if (!g_logged) {
                g_logged = true;
                Log("camera_range: CAMERA_MAX_HEIGHT %.0f: the map camera's far plane %.0f instead of %.0f", *g_maxHeight, want, farPlane);
            }
        }
    }
}

}  // namespace

bool CameraRangeInit() {
    if (!Engine::HaveGlobe() || !Engine::HaveAttentionCamera()) {
        Log("camera_range: the map camera's layout was not resolved (globe / attention_area pieces); off");
        return false;
    }
    if (!MainThreadQueueAvailable()) {
        Log("camera_range: needs the main-thread frame hook; off");
        return false;
    }
    uint8_t* init = SigFindUnique("camera_far", kSigCameraFar);
    uint8_t* ctor = SigFindUnique("camera_max_height", kSigCameraMaxHeight);
    if (!init || !ctor) {
        Log("camera_range: the camera code was not found; off (a raised CAMERA_MAX_HEIGHT keeps vanilla's far plane)");
        return false;
    }
    uint32_t farPlane = 0, fov = 0, aspect = 0, nearOff = 0;
    memcpy(&farPlane, init + kSigCameraFarDisp, 4);
    memcpy(&fov, init + kSigCameraFovDisp, 4);
    memcpy(&aspect, init + kSigCameraAspectDisp, 4);
    memcpy(&nearOff, init + kSigCameraNearDisp, 4);
    // Shape: fov, aspect, near, far follow each other (4 bytes apart).
    if (aspect != fov + 4 || nearOff != aspect + 4 || farPlane != nearOff + 4 || farPlane > 0x1000) {
        Log("camera_range: unexpected camera layout (fov +0x%x, aspect +0x%x, near +0x%x, far +0x%x); off", fov, aspect, nearOff, farPlane);
        return false;
    }
    g_farOff = farPlane;
    g_maxHeight = (const float*)SigRipTarget(ctor + kSigCameraMaxHeightInsn, 4, 8);  // cvttss2si eax, [rip+rel32]
    if (!g_maxHeight) {
        Log("camera_range: the CAMERA_MAX_HEIGHT define was not found; off");
        return false;
    }
    OnMainThreadFrame(&Frame);
    Log("camera_range: far plane at engine camera +0x%x, CAMERA_MAX_HEIGHT at %s", farPlane, DescribeAddress((void*)g_maxHeight).c_str());
    return true;
}
