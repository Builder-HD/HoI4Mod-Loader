// globe (engine tier): the map as a planet, or several planets (NOTES.md Parts S and T).
//
//   # in a mod: common/hoi4_loader_globe/*.txt (turns the feature on; see globe.h for the keys)
//   globe = { default = yes flat_height = 400 globe_height = 2500 transition = 0.35 background = "gfx/x/space.png" }
//
//   set_globe_view = yes      # country scope: that country's players see the globe (no: flat map)
//   is_globe_view = yes       # country scope: true if that country's view is the globe
//
// Zoomed out far enough that the flat view would reach past the map's north or south edge (where
// the frame and logo are), or above globe_height, the flat map folds into a sphere; the point the
// camera looks at stays on top, so panning turns the planet. The vertex shaders of the map are
// patched to bend every world position (globe.cpp); the values reach them through the loader's own
// constant buffer (globe_d3d.cpp). While the map is bent, the game's culling is opened (it only
// draws what a flat camera would see), the two wrap copies of the camera are off (the bend wraps
// itself), the north/south edge clamp is skipped (so the poles can be reached) and the screen rays
// the game picks with are turned back into rays onto the flat map point under the cursor.
//
// set_globe_view is saved: global flag hoi4_globe_<TAG>_1 (globe) or _0 (flat). The view follows
// the local player's country: its flag, else the mod file's default. Graphics only: every machine
// sets the same flags, each shows its own player's choice, nothing in the game state changes.
#include <windows.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <map>
#include <string>
#include <vector>

#include "engine.h"
#include "globe.h"
#include "globe_d3d.h"
#include "lifecycle.h"
#include "loader.h"
#include "mod_list.h"
#include "native_script.h"

namespace {

const char* const kFeature = "globe";
const char kFolder[] = "common/hoi4_loader_globe";
const char kFlagPrefix[] = "hoi4_globe_";
const LocText kOn{"HOI4_LOADER_SET_GLOBE_VIEW_ON", "The map is shown as a globe."};
const LocText kOff{"HOI4_LOADER_SET_GLOBE_VIEW_OFF", "The map is shown flat."};

Globe::Settings g_settings;  // read once at init

// Per-country choice (from flags), read by the trigger on any thread: a small table of atomics.
const int kViews = 512;
std::atomic<int> g_viewTag[kViews];
std::atomic<int> g_viewOn[kViews];  // 1 globe, 0 flat
std::atomic<int> g_viewCount{0};

// Per frame (main thread), read by the hooks.
std::atomic<void*> g_gameCam{nullptr};  // the in-game map camera while a game runs
std::atomic<bool> g_wanted{false};      // the local player's view is the globe
SRWLOCK g_paramsLock = SRWLOCK_INIT;
Globe::Params g_params;                 // last published (t = 0: flat)
float g_mapW = 0, g_mapH = 0;
bool g_loggedActive = false;

// --- Choices ------------------------------------------------------------------------------------

int FindView(int tag) {
    int n = g_viewCount.load();
    for (int i = 0; i < n && i < kViews; ++i)
        if (g_viewTag[i].load() == tag) return i;
    return -1;
}

// Main thread only (effects, game entry): the trigger may read meanwhile.
void SetView(int tag, bool on) {
    int i = FindView(tag);
    if (i < 0) {
        i = g_viewCount.load();
        if (i >= kViews) return;
        g_viewTag[i].store(tag);
        g_viewOn[i].store(on ? 1 : 0);
        g_viewCount.store(i + 1);
        return;
    }
    g_viewOn[i].store(on ? 1 : 0);
}

bool ViewOf(int tag) {
    int i = tag > 0 ? FindView(tag) : -1;
    return i >= 0 ? g_viewOn[i].load() == 1 : g_settings.defaultOn;
}

std::string FlagName(int tag, bool on) { return std::string(kFlagPrefix) + Engine::CountryTagName(tag) + (on ? "_1" : "_0"); }

void OnEntering() { g_viewCount.store(0); }

void OnEntered(bool) {
    struct Found {
        std::string tag;
        bool on;
        int date;
    };
    std::vector<Found> found;
    Engine::ForEachGlobalFlag(
        [](const Engine::FlagEntry& f, void* ctx) {
            const char* s = Engine::TokenName(f.token);
            size_t n = strlen(kFlagPrefix), len = strlen(s);
            if (strncmp(s, kFlagPrefix, n) != 0 || len < n + 3 || s[len - 2] != '_' || (s[len - 1] != '0' && s[len - 1] != '1')) return;
            ((std::vector<Found>*)ctx)->push_back({std::string(s + n, len - n - 2), s[len - 1] == '1', f.setDate});
        },
        &found);
    // Both flags of a country (written by hand): the later one wins.
    for (const Found& f : found) {
        int tag = Engine::TagFromText(f.tag.c_str());
        if (tag <= 0) continue;
        bool latest = true;
        for (const Found& g : found)
            if (g.tag == f.tag && g.on != f.on && g.date > f.date) latest = false;
        if (latest) SetView(tag, f.on);
    }
    if (!found.empty()) Log("globe: %zu saved view choice(s) read", found.size());
}

void ExecSet(const Args& a, const Scope& s) {
    int tag = s.countryTag();
    if (tag <= 0) return;
    bool on = a.b(0);
    Engine::ClearGlobalFlag(Engine::Intern(FlagName(tag, !on).c_str()));
    Engine::SetGlobalFlag(Engine::Intern(FlagName(tag, on).c_str()));
    SetView(tag, on);
    if (!g_settings.present) {
        static bool logged = false;
        if (!logged) Log("globe: set_globe_view ran, but no enabled mod has a %s file: the map stays flat", kFolder);
        logged = true;
    }
}

void TipSet(const Args& a, const Scope&, TooltipText& t) { t.add(native_localize(a.b(0) ? kOn : kOff).c_str()); }

bool EvalIs(const Args& a, const Scope& s) {
    int tag = s.countryTag();
    return tag > 0 && ViewOf(tag) == a.b(0);
}


// --- Per frame ------------------------------------------------------------------------------------
// The game updates its camera only while it moves (the area hook); the frame callback runs every
// frame. Both end in Apply. Main thread.
//
// Three levels by the camera's height (NOTES.md T9): flat (the current planet's map; the camera kept
// inside its rectangle), globe (the current planet folds into its sphere) and the overview (every
// planet at its place, the layout fitting the view at overview_full_height).

Globe::SphereState g_sphere;
float g_t = 0.0f;              // 0 flat .. 1 sphere, moved toward the target over `transition`
float g_p = 0.0f;              // overview progress
bool g_edgeInView = false;     // the flat view reaches past the current planet's edges (last fresh area)
float g_area[4] = {0, 0, 0, 0};  // the last fresh visible area: x min, x max, z min, z max
float g_height = 0.0f;         // the camera's height (eye y)
float g_prevHeight = 0.0f;
ULONGLONG g_lastTick = 0;
std::vector<Globe::Planet> g_planets;  // resolved once the map is loaded
int g_cur = 0;                         // the planet the camera is on
std::vector<float> g_lastLook;         // per planet x, z
float g_focus[2] = {0, 0};             // the overview layout point under the camera
bool g_othersVisible = false;          // in the overview, another planet is on screen
float g_aspect = 16.0f / 9.0f;         // the view's, last frame
bool g_namesBuilt = false;
std::vector<int> g_imageIndex;         // per planet: its index in the images given to GlobeD3D, -1 none
// Hover (the game's pointer ray, main thread, each frame): the planet under the cursor and the
// layout point there (for zooming in from the overview).
int g_hoverPlanet = -1;
float g_hoverLayout[2] = {0, 0};
bool g_hoverLayoutOk = false;
ULONGLONG g_hoverTick = 0;
float g_scale = 1.0f;                  // the overview's scale, last frame

void Publish(const Globe::Params& p) {
    AcquireSRWLockExclusive(&g_paramsLock);
    g_params = p;
    ReleaseSRWLockExclusive(&g_paramsLock);
    GlobeD3D::Publish(p.a);
}

Globe::Params Current() {
    AcquireSRWLockShared(&g_paramsLock);
    Globe::Params p = g_params;
    ReleaseSRWLockShared(&g_paramsLock);
    return p;
}

float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The ground point (y = 0) the camera looks at.
bool LookAt(void* mapCamera, float* lx, float* lz, float* height) {
    float eye[3], look[3];
    if (!Engine::GlobeCameraPose(mapCamera, eye, look)) return false;
    float d[3] = {look[0] - eye[0], look[1] - eye[1], look[2] - eye[2]};
    *lx = look[0], *lz = look[2];
    if (d[1] < -1e-4f) {
        float s = -eye[1] / d[1];
        *lx = eye[0] + s * d[0], *lz = eye[2] + s * d[2];
    }
    *height = eye[1];
    return true;
}

void BuildNames();
void BuildOutline(const std::vector<uint8_t>& fill, int W, int H, int RH, std::vector<uint8_t>* rgba);

// The planets on the loaded map (once), with their images' cells.
bool EnsurePlanets() {
    if (!g_planets.empty()) return true;
    float w = 0, h = 0;
    if (!Engine::GlobeMapSize(&w, &h)) return false;
    std::vector<std::string> problems;
    g_planets = Globe::ResolvePlanets(g_settings, w, h, &problems);
    for (const std::string& m : problems) Log("globe: %s", m.c_str());
    for (Globe::Planet& p : g_planets) {
        int img = p.source < (int)g_imageIndex.size() ? g_imageIndex[p.source] : -1;
        p.image = img >= 0 && GlobeD3D::ImageCell((size_t)img, p.imageCell);
    }
    g_lastLook.assign(g_planets.size() * 2, 0.0f);
    for (size_t i = 0; i < g_planets.size(); ++i) {
        const Globe::Planet& p = g_planets[i];
        g_lastLook[i * 2] = p.x0 + p.w * 0.5f;
        g_lastLook[i * 2 + 1] = p.z0 + p.h * (0.0f - p.south) / (p.north - p.south);
    }
    std::string list;
    for (const Globe::Planet& p : g_planets)
        list += (list.empty() ? "" : ", ") + (p.name.empty() ? std::string("the map") : p.name) + " (" + std::to_string((int)p.x0) +
                "," + std::to_string((int)p.z0) + " " + std::to_string((int)p.w) + "x" + std::to_string((int)p.h) + (p.image ? ", image" : "") + ")";
    Log("globe: %zu planet(s) on the %.0f x %.0f map: %s", g_planets.size(), w, h, list.c_str());
    return !g_planets.empty();
}

// The camera kept on the current planet while flat: its look-at inside the planet's rectangle
// north-south, east-west wrapped within it (whole widths, so the map looks the same). A planet's
// edge then looks as vanilla's map edge does (the frame beyond it).
void Confine(void* mapCamera, float lx, float lz, bool wrapX) {
    const Globe::Planet& p = g_planets[g_cur];
    float dx = 0, dz = 0;
    if (wrapX && (lx < p.x0 || lx >= p.x0 + p.w)) dx = -p.w * floorf((lx - p.x0) / p.w);
    if (lz < p.z0) dz = p.z0 - lz;
    else if (lz > p.z0 + p.h) dz = p.z0 + p.h - lz;
    if (dx != 0.0f || dz != 0.0f) Engine::GlobeShiftCamera(mapCamera, dx, dz);
}

void ReadArea(void* mapCamera) {
    Engine::GlobeAreaXRange(mapCamera, &g_area[0], &g_area[1]);
    Engine::GlobeAreaZRange(mapCamera, &g_area[2], &g_area[3]);
}

// fresh: right after the game's camera update (the visible area is the game's own, for the flat map).
void Apply(void* mapCamera, bool fresh) {
    float w = 0, h = 0, lx = 0, lz = 0, height = 0;
    if (g_planets.empty() || !LookAt(mapCamera, &lx, &lz, &height) || !Engine::GlobeMapSize(&w, &h)) {
        Publish(Globe::Params());
        return;
    }
    g_height = height;
    const Globe::Planet& cur = g_planets[g_cur];
    if (fresh) {
        ReadArea(mapCamera);
        bool narrow = cur.w < w - 0.5f;
        g_edgeInView = g_area[2] < cur.z0 || g_area[3] > cur.z0 + cur.h || (narrow && (g_area[0] < cur.x0 || g_area[1] > cur.x0 + cur.w));
    }
    lz = Clampf(lz, cur.z0, cur.z0 + cur.h);
    Globe::View v;
    v.current = g_cur, v.t = g_t, v.p = g_p, v.lookX = lx, v.lookZ = lz;
    v.focusX = g_focus[0], v.focusY = g_focus[1];
    v.fullHeight = g_settings.overviewFullHeight;
    v.lastLook = g_lastLook;
    v.background = GlobeD3D::HaveBackground();
    RECT rc;
    HWND wnd = GetForegroundWindow();
    if (wnd && GetClientRect(wnd, &rc) && rc.bottom > 0) g_aspect = (float)rc.right / (float)rc.bottom;
    v.aspect = g_aspect;
    Globe::Params p;
    if (!Globe::MakeParams(g_settings, g_planets, w, v, &p)) {
        Publish(Globe::Params());
        return;
    }
    if (g_p > 0.0f) {
        float cx, cy;
        g_scale = Globe::OverviewScale(g_planets, v, &cx, &cy);
    }
    Publish(p);
    // The main camera draws every planet: the wrap copies are off, every frustum plane open.
    Engine::GlobeHideCopies(mapCamera);
    for (int k = 0; k < 3; ++k) Engine::GlobeOpenFrustum(Engine::GlobeEngineCamera(mapCamera, k));
    if (fresh) Engine::GlobeWidenArea(mapCamera, lx, lz, 50.0f);
    if (!g_loggedActive) {
        g_loggedActive = true;
        Log("globe: first globe frame (camera height %.0f, map %.0f x %.0f, background %s)", height, w, h,
            p.b[3] > 0.5f ? "on" : "the game's sky");
    }
}

void AfterUpdate(void* mapCamera) {
    void* game = g_gameCam.load();
    if (mapCamera != game) {
        if (!game) Publish(Globe::Params());  // the main menu's camera: flat
        return;
    }
    if (g_planets.empty()) return;
    if (!g_wanted.load() && g_t <= 0.0f) {
        Publish(Globe::Params());
        // Keep the edge state current for the moment the view is wanted.
        float w = 0, h = 0;
        if (Engine::GlobeMapSize(&w, &h)) {
            const Globe::Planet& cur = g_planets[g_cur];
            ReadArea(mapCamera);
            g_edgeInView = g_area[2] < cur.z0 || g_area[3] > cur.z0 + cur.h ||
                           (cur.w < w - 0.5f && (g_area[0] < cur.x0 || g_area[1] > cur.x0 + cur.w));
        }
        return;
    }
    Apply(mapCamera, true);
}

// Zooming in from the overview: the planet to go to (under the cursor, else the closest to the
// cursor's point of the layout, else the closest to the focus).
int ZoomTarget() {
    bool fresh = GetTickCount64() - g_hoverTick < 500;
    if (fresh && g_hoverPlanet >= 0) return g_hoverPlanet;
    float px = g_focus[0], py = g_focus[1];
    if (fresh && g_hoverLayoutOk) px = g_hoverLayout[0], py = g_hoverLayout[1];
    int best = g_cur;
    float bestD = 1e30f;
    for (size_t i = 0; i < g_planets.size(); ++i) {
        float dx = g_planets[i].ox - px, dy = g_planets[i].oy - py;
        float d = sqrtf(dx * dx + dy * dy) - g_planets[i].size * 0.5f;
        if (d < bestD) bestD = d, best = (int)i;
    }
    return best;
}

void Frame() {
    void* cam = Engine::GameState() ? Engine::UiMapCamera() : nullptr;
    g_gameCam.store(cam);
    int player = cam ? Engine::PlayerCountryTag() : 0;
    bool wanted = cam && g_settings.present && GlobeD3D::Ready() && (player > 0 ? ViewOf(player) : g_settings.defaultOn);
    g_wanted.store(wanted);
    ULONGLONG now = GetTickCount64();
    float dt = g_lastTick ? (float)(now - g_lastTick) / 1000.0f : 0.0f;
    if (dt > 0.25f) dt = 0.25f;
    g_lastTick = now;
    if (!cam || !EnsurePlanets()) {
        g_t = 0.0f, g_p = 0.0f, g_sphere = Globe::SphereState(), g_othersVisible = false;
        return;
    }
    if (!g_namesBuilt && g_settings.overviewHeight > 0 && GlobeD3D::Ready()) BuildNames();
    float lx = 0, lz = 0, height = 0;
    if (!LookAt(cam, &lx, &lz, &height)) return;
    g_height = height;
    // The overview: from overview_height to overview_full_height.
    float p = 0.0f;
    if (wanted && g_settings.overviewHeight > 0.0f)
        p = Clampf((height - g_settings.overviewHeight) / (g_settings.overviewFullHeight - g_settings.overviewHeight), 0.0f, 1.0f);
    bool sphere = wanted && (p > 0.0f || Globe::WantSphere(g_settings, height, g_edgeInView, &g_sphere));
    if (!wanted) g_sphere = Globe::SphereState();
    float target = sphere ? 1.0f : 0.0f;
    float step = g_settings.transition > 0.0f ? dt / g_settings.transition : 1.0f;
    g_t = g_t < target ? (g_t + step > target ? target : g_t + step) : (g_t - step < target ? target : g_t - step);
    if (p > 0.0f && g_t < 1.0f) g_t = 1.0f;
    // The current planet: where the flat camera is; kept inside it.
    if (g_t <= 0.0f && p <= 0.0f) {
        int at = Globe::PlanetAt(g_planets, lx, lz);
        if (at >= 0) g_cur = at;
        Confine(cam, lx, lz, true);
    } else if (p <= 0.0f) {
        Confine(cam, lx, lz, false);  // the globe: north-south within the planet
    }
    g_othersVisible = false;
    if (p > 0.0f) {
        const Globe::Planet& c0 = g_planets[g_cur];
        if (g_p <= 0.0f) g_focus[0] = c0.ox, g_focus[1] = c0.oy;  // entering: the current planet under the camera
        if (height < g_prevHeight - 0.5f) {
            // Zooming in: to the planet under the cursor (or the closest). It is shown as it was last
            // looked at, and the camera goes there: nothing turns or jumps.
            int j = ZoomTarget();
            if (j != g_cur) {
                g_lastLook[g_cur * 2] = lx, g_lastLook[g_cur * 2 + 1] = lz;
                float tx = g_lastLook[j * 2], tz = g_lastLook[j * 2 + 1];
                Engine::GlobeShiftCamera(cam, tx - lx, tz - lz);
                lx = tx, lz = tz;
                g_cur = j;
            }
        }
        // The focus glides toward the current planet as the overview closes, toward the layout's
        // centre as it opens.
        float cx = 0, cy = 0;
        Globe::View v;
        v.current = g_cur, v.p = p, v.fullHeight = g_settings.overviewFullHeight, v.aspect = g_aspect;
        float scale = Globe::OverviewScale(g_planets, v, &cx, &cy);
        float e = p * p * (3.0f - 2.0f * p);
        const Globe::Planet& c = g_planets[g_cur];
        float fx = c.ox + (cx - c.ox) * e, fy = c.oy + (cy - c.oy) * e;
        float k = dt * 6.0f > 1.0f ? 1.0f : dt * 6.0f;
        g_focus[0] += (fx - g_focus[0]) * k, g_focus[1] += (fy - g_focus[1]) * k;
        // Another planet on screen: the camera holds still (dragging would turn the current planet
        // under the others); once they are out of sight it moves as on the globe.
        float halfH = height * 0.41421356f, halfW = halfH * g_aspect;
        for (size_t i = 0; i < g_planets.size(); ++i) {
            if ((int)i == g_cur) continue;
            float r = g_planets[i].size * 0.5f * scale;
            float dx = fabsf((g_planets[i].ox - g_focus[0]) * scale), dz = fabsf((g_planets[i].oy - g_focus[1]) * scale);
            if (dx - r < halfW && dz - r < halfH) g_othersVisible = true;
        }
    }
    g_lastLook[g_cur * 2] = lx, g_lastLook[g_cur * 2 + 1] = lz;
    g_p = p;
    g_prevHeight = height;
    if (g_t <= 0.0f) {
        Publish(Globe::Params());
        return;
    }
    Apply(cam, false);
}

void ScreenRay(void* engineCamera, float dir[3], float origin[3], bool fromCamera) {
    void* game = g_gameCam.load();
    if (!game) return;
    bool ours = false;
    for (int k = 0; k < 3; ++k) ours = ours || engineCamera == Engine::GlobeEngineCamera(game, k);
    if (!ours) return;
    Globe::Params p = Current();
    if (!(p.t() > 0.0f) || p.count() < 1) return;
    const float* look = p.look(p.current());
    if (fromCamera) {
        // The map camera's own rays (drag, edge panning, zoom to the cursor) stay the game's: the
        // camera moves as over the flat map, smoothly; on the planet that turns it. In the overview,
        // while another planet is on screen, they point straight at the look-at: the camera stays.
        if (p.overview() > 0.0f && g_othersVisible) {
            float d[3] = {look[0] - origin[0], -origin[1], look[1] - origin[2]};
            float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > 1e-3f) dir[0] = d[0] / len, dir[1] = d[1] / len, dir[2] = d[2] / len;
        }
        return;
    }
    // The wrap copies cast the same ray shifted by a map width: shift it back, and the flat point
    // into the copy's frame again, so all three cameras find the same place.
    float mapW = p.a[2];
    float shift = mapW > 0 ? mapW * roundf((origin[0] - look[0]) / mapW) : 0.0f;
    float o[3] = {origin[0] - shift, origin[1], origin[2]};
    float dist = 0;
    int i = Globe::RayPlanet(p, o, dir, &dist);
    // For zooming in from the overview: the planet under the cursor and the layout point there.
    bool main = engineCamera == Engine::GlobeEngineCamera(game, 0);
    if (main) {
        g_hoverPlanet = i, g_hoverTick = GetTickCount64(), g_hoverLayoutOk = false;
        if (fabsf(dir[1]) > 1e-6f && g_scale > 0.0f) {
            float s = -o[1] / dir[1];
            if (s > 0) {
                g_hoverLayout[0] = g_focus[0] + (o[0] + s * dir[0] - look[0]) / g_scale;
                g_hoverLayout[1] = g_focus[1] + (o[2] + s * dir[2] - look[1]) / g_scale;
                g_hoverLayoutOk = true;
            }
        }
    }
    float flat[2];
    bool hit = false;
    if (i >= 0 && Globe::Unbend(p, i, o, dir, flat, &hit) && hit) {
        // The copy of the point on the camera's side of the wrap line (5832, not 200, for a camera at
        // 5620) for a planet as wide as the map: the direction must point the way the cursor is; the
        // game's wrap copies find the province.
        const float* rc = p.rect(i);
        if (i == p.current() && rc[2] >= mapW - 0.5f) flat[0] += mapW * roundf((o[0] - flat[0]) / mapW);
    } else {
        // Picking in space, on the polar seas or on a planet's image: a ray onto a point far off the
        // map, which holds nothing.
        flat[0] = o[0], flat[1] = -100000.0f;
    }
    flat[0] += shift;
    // From the camera onto the flat point (y = 0): the game's own hit test finds what lies there.
    float d[3] = {flat[0] - origin[0], -origin[1], flat[1] - origin[2]};
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-3f) || !(origin[1] > 1.0f)) return;
    dir[0] = d[0] / len, dir[1] = d[1] / len, dir[2] = d[2] / len;
}

bool SkipEdgeClamp(void* mapCamera) { return mapCamera == g_gameCam.load() && Current().t() > 0.0f; }

// While bent, the 2D map icons (city names, ports, units, ...) hide by their own cutoffs: they are
// placed for the flat map.
bool HideIcons() { return g_gameCam.load() && Current().t() > 0.0f; }

// Things drawn without a depth test (names, arrows, routes, particles) drop what lies beyond the
// planet's horizon; the rest is hidden by the planet itself.
std::string PatchCulled(const std::string& original) {
    int n = 0;
    std::string out = Globe::PatchMapShader(original, Globe::kCull, &n);
    Log("globe: a map shader %s (%d position(s) bent, beyond the horizon dropped)", out.empty() ? "had nothing to bend" : "was patched", n);
    return out;
}

// The game's map names (countries, states): also gone in the overview.
std::string PatchLabels(const std::string& original) {
    int n = 0;
    std::string out = Globe::PatchMapShader(original, Globe::kLabel, &n);
    Log("globe: mapname.shader %s (%d position(s))", out.empty() ? "had nothing to bend" : "was patched", n);
    return out;
}

std::string PatchTerrain(const std::string& original) {
    int n = 0;
    std::string out = Globe::PatchMapShader(original, Globe::kTerrain, &n);
    Log("globe: pdxmap.shader %s (%d replacement(s))", out.empty() ? "NOT patched: the map stays flat" : "patched", n);
    return out;
}

std::string PatchShader(const std::string& original) {
    int n = 0;
    std::string out = Globe::PatchMapShader(original, Globe::kBend, &n);
    Log("globe: a map shader %s (%d replacement(s))", out.empty() ? "had nothing to bend" : "was patched", n);
    return out;
}

// The game's water: not drawn while bent (its coarse grid would sink under the curved seabed).
std::string PatchWater(const std::string& original) {
    int n = 0;
    std::string out = Globe::PatchMapShader(original, Globe::kHide, &n);
    Log("globe: pdxwater.shader %s", out.empty() ? "NOT patched: the water may cover the sea oddly on the planet" : "patched (hidden while bent; the seabed shows the sea)");
    return out;
}

std::string PatchShared(const std::string& original) {
    std::string out = Globe::PatchSharedCode(original);
    Log("globe: standardfuncsgfx.fxh %s", out.empty() ? "NOT patched (the distance fog was not found as expected)" : "patched (distance fog across the wrap line)");
    return out;
}

std::string PatchSky(const std::string& original) {
    std::string out = Globe::PatchSkyShader(original);
    Log("globe: sky.shader %s", out.empty() ? "NOT patched (its main was not found as expected): the game's sky stays" : "patched (background, polar seas, planet images and names)");
    return out;
}

void LoadSettings() {
    ModList::FolderScan scan = ModList::ScanFolder(GameFolder(), UserFolder(), kFolder);
    if (!scan.modListRead) Log("globe: the launcher's mod list (dlc_load.json) is unreadable; only the game folder is searched");
    for (const std::string& note : scan.notes) Log("globe: %s", note.c_str());
    for (const ModList::FolderFile& f : scan.files) {
        std::string text;
        if (!ModList::ReadWholeFile(f.path, &text)) {
            Log("globe: %s unreadable", f.shown.c_str());
            continue;
        }
        std::vector<Globe::Problem> problems;
        Globe::ParseFile(text, f.shown, &g_settings, &problems);
        for (const Globe::Problem& p : problems) Log("globe: %s line %d: %s", p.file.c_str(), p.line, p.message.c_str());
    }
}

// A file named in the settings, as the game would find it: the last enabled mod that has it, else the
// game folder. "" if none has it (logged as `what`, named in `from`).
std::wstring FindFile(const std::string& path, const char* what, const std::string& from) {
    if (path.empty()) return L"";
    std::wstring rel = ModList::Wide(path);
    for (wchar_t& c : rel)
        if (c == L'/') c = L'\\';
    std::vector<ModList::EnabledMod> mods;
    ModList::Enabled(UserFolder(), &mods);
    for (auto it = mods.rbegin(); it != mods.rend(); ++it) {
        if (it->status != ModList::EnabledMod::Status::Ok) continue;
        std::wstring p = it->root + L"\\" + rel;
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    std::wstring p = GameFolder() + L"\\" + rel;
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    Log("globe: the %s %s (named in %s) is in no enabled mod nor the game folder", what, path.c_str(), from.c_str());
    return L"";
}

// The game's bitmap fonts (BMFont text .fnt with a 32-bit .dds page, glyphs in the alpha channel), as
// the map names use (gfx/fonts/hoi_mapfont4).
struct Glyph {
    int x = 0, y = 0, w = 0, h = 0, xo = 0, yo = 0, adv = 0;
};
struct BmFont {
    int lineH = 0, base = 0;
    std::map<uint32_t, Glyph> chars;
    std::map<uint64_t, int> kerning;
    std::vector<uint8_t> rgba;  // the page, RGBA (the glyphs carry their own colours: a dark core, a light rim)
    int tw = 0, th = 0;
};

int FntValue(const std::string& line, const char* key) {
    std::string k = std::string(" ") + key + "=";
    size_t at = (" " + line).find(k);
    if (at == std::string::npos) return 0;
    return atoi(line.c_str() + at - 1 + k.size());
}

bool ReadBytes(const std::wstring& path, std::vector<uint8_t>* out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out->insert(out->end(), buf, buf + n);
    fclose(f);
    return true;
}

bool LoadBmFont(const std::string& base, BmFont* font) {
    std::wstring fnt = FindFile(base + ".fnt", "name font", "the globe settings");
    std::wstring dds = FindFile(base + ".dds", "name font page", "the globe settings");
    std::vector<uint8_t> text, page;
    if (fnt.empty() || dds.empty() || !ReadBytes(fnt, &text) || !ReadBytes(dds, &page)) return false;
    std::string all(text.begin(), text.end());
    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find('\n', pos);
        if (end == std::string::npos) end = all.size();
        std::string line = all.substr(pos, end - pos);
        pos = end + 1;
        if (line.compare(0, 7, "common ") == 0) {
            font->lineH = FntValue(line, "lineHeight"), font->base = FntValue(line, "base");
        } else if (line.compare(0, 5, "char ") == 0) {
            Glyph g;
            g.x = FntValue(line, "x"), g.y = FntValue(line, "y"), g.w = FntValue(line, "width"), g.h = FntValue(line, "height");
            g.xo = FntValue(line, "xoffset"), g.yo = FntValue(line, "yoffset"), g.adv = FntValue(line, "xadvance");
            font->chars[(uint32_t)FntValue(line, "id")] = g;
        } else if (line.compare(0, 8, "kerning ") == 0) {
            uint64_t key = ((uint64_t)(uint32_t)FntValue(line, "first") << 32) | (uint32_t)FntValue(line, "second");
            font->kerning[key] = FntValue(line, "amount");
        }
    }
    // The page: an uncompressed 32-bit DDS (as the game's map font is); the alpha mask says which byte.
    if (page.size() < 128 || memcmp(page.data(), "DDS ", 4) != 0) return false;
    uint32_t h, w, pfFlags, bits, mask[4];
    memcpy(&h, &page[12], 4), memcpy(&w, &page[16], 4), memcpy(&pfFlags, &page[80], 4), memcpy(&bits, &page[88], 4);
    memcpy(mask, &page[92], 16);  // red, green, blue, alpha masks
    int at[4];
    for (int c = 0; c < 4; ++c) {
        at[c] = mask[c] == 0xffu ? 0 : mask[c] == 0xff00u ? 1 : mask[c] == 0xff0000u ? 2 : mask[c] == 0xff000000u ? 3 : -1;
        if (at[c] < 0) return false;
    }
    if (!(pfFlags & 0x40) || bits != 32 || w == 0 || h == 0 || page.size() < 128 + (size_t)w * h * 4) return false;
    font->tw = (int)w, font->th = (int)h;
    font->rgba.resize((size_t)w * h * 4);
    for (size_t k = 0; k < (size_t)w * h; ++k)
        for (int c = 0; c < 4; ++c) font->rgba[k * 4 + c] = page[128 + k * 4 + at[c]];
    return font->lineH > 0 && !font->chars.empty();
}

// The page bilinearly at (x, y): RGBA 0..255.
void FontSample(const BmFont& f, float x, float y, float out[4]) {
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    for (int c = 0; c < 4; ++c) {
        auto at = [&](int xx, int yy) -> float {
            if (xx < 0 || yy < 0 || xx >= f.tw || yy >= f.th) return 0.0f;
            return f.rgba[((size_t)yy * f.tw + xx) * 4 + c];
        };
        out[c] = (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
    }
}

// Writes text (UTF-16 units, upper case) centred into row `row` (rh pixels) of a w-wide RGBA buffer,
// in the font's own colours; returns the text's width in pixels.
float DrawBmText(const BmFont& f, const std::wstring& text, uint8_t* rgba, int w, int rh, int row) {
    float spacing = f.lineH * 0.08f;  // a little wider than the font, as the map names are spread
    float width = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        auto it = f.chars.find((uint32_t)text[i]);
        if (it == f.chars.end()) continue;
        width += it->second.adv + spacing;
        if (i + 1 < text.size()) {
            auto kt = f.kerning.find(((uint64_t)(uint32_t)text[i] << 32) | (uint32_t)text[i + 1]);
            if (kt != f.kerning.end()) width += kt->second;
        }
    }
    if (!(width > 0)) return 0;
    float scale = fminf(1.0f, fminf((w - 32) / width, (rh - 8) / (float)f.lineH));
    float pen = (w - width * scale) * 0.5f;
    float top = row * rh + (rh - f.lineH * scale) * 0.5f;
    for (size_t i = 0; i < text.size(); ++i) {
        auto it = f.chars.find((uint32_t)text[i]);
        if (it == f.chars.end()) continue;
        const Glyph& g = it->second;
        int dx0 = (int)floorf(pen + g.xo * scale), dy0 = (int)floorf(top + g.yo * scale);
        int dw = (int)ceilf(g.w * scale) + 1, dh = (int)ceilf(g.h * scale) + 1;
        for (int y = 0; y < dh; ++y) {
            int yy = dy0 + y;
            if (yy < row * rh || yy >= (row + 1) * rh) continue;
            for (int x = 0; x < dw; ++x) {
                int xx = dx0 + x;
                if (xx < 0 || xx >= w) continue;
                float sx = g.x + (xx + 0.5f - (pen + g.xo * scale)) / scale - 0.5f;
                float sy = g.y + (yy + 0.5f - (top + g.yo * scale)) / scale - 0.5f;
                if (sx < g.x - 1 || sy < g.y - 1 || sx > g.x + g.w || sy > g.y + g.h) continue;
                float c[4];
                FontSample(f, sx, sy, c);
                uint8_t* d = &rgba[((size_t)yy * w + xx) * 4];
                if (c[3] > d[3])
                    for (int k = 0; k < 4; ++k) d[k] = (uint8_t)(c[k] > 255.0f ? 255.0f : c[k]);
            }
        }
        pen += (g.adv + spacing) * scale;
        if (i + 1 < text.size()) {
            auto kt = f.kerning.find(((uint64_t)(uint32_t)text[i] << 32) | (uint32_t)text[i + 1]);
            if (kt != f.kerning.end()) pen += kt->second * scale;
        }
    }
    return width * scale;
}

std::wstring PlanetName(const Globe::Planet& pl) {
    std::string text = native_localize_key(pl.name.c_str());
    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &wide[0], len);
    CharUpperBuffW(&wide[0], (DWORD)wide.size());
    return wide;
}

// The names of the planets in one texture: a row of 128 pixels per planet, the text centred, in the
// game's map font (name_font; GDI's Georgia if it cannot be read), the fill in red, an outline (the
// fill widened) in green.
void BuildNames() {
    g_namesBuilt = true;
    const int W = 2048, RH = 128;
    int n = (int)g_planets.size();
    if (n < 1) return;
    int H = RH * n;
    std::vector<uint8_t> fill((size_t)W * H, 0);
    std::vector<uint8_t> rgba((size_t)W * H * 4, 0);
    BmFont font;
    bool bm = LoadBmFont(g_settings.nameFont, &font);
    if (!bm) Log("globe: the name font %s could not be read (a BMFont .fnt with a 32-bit .dds); Georgia instead", g_settings.nameFont.c_str());
    int named = 0;
    if (bm) {
        for (int i = 0; i < n; ++i) {
            Globe::Planet& pl = g_planets[i];
            if (pl.name.empty()) continue;
            float textW = DrawBmText(font, PlanetName(pl), rgba.data(), W, RH, i) + 8.0f;
            if (textW <= 8.0f) continue;
            pl.nameRow[0] = (float)(i * RH) / H, pl.nameRow[1] = (float)RH / H;
            pl.nameAspect = textW / RH, pl.nameFrac = textW / W;
            ++named;
        }
    } else {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = W, bi.bmiHeader.biHeight = -H, bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (!bmp || !bits) {
            if (dc) DeleteDC(dc);
            Log("globe: the planet names could not be drawn (GDI)");
            return;
        }
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        memset(bits, 0, (size_t)W * H * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        for (int i = 0; i < n; ++i) {
            Globe::Planet& pl = g_planets[i];
            if (pl.name.empty()) continue;
            std::wstring wide = PlanetName(pl);
            if (wide.empty()) continue;
            SIZE sz = {};
            HFONT gf = nullptr;
            for (int px = 96; px >= 24; px -= 8) {  // the largest that fits the row's width
                if (gf) DeleteObject(gf);
                gf = CreateFontW(-px, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_ROMAN, L"Georgia");
                SelectObject(dc, gf);
                SetTextCharacterExtra(dc, px / 6);
                GetTextExtentPoint32W(dc, wide.c_str(), (int)wide.size(), &sz);
                if (sz.cx <= W - 32) break;
            }
            TextOutW(dc, (W - sz.cx) / 2, i * RH + (RH - sz.cy) / 2, wide.c_str(), (int)wide.size());
            SelectObject(dc, GetStockObject(SYSTEM_FONT));
            if (gf) DeleteObject(gf);
            float textW = (float)(sz.cx + 16);
            pl.nameRow[0] = (float)(i * RH) / H, pl.nameRow[1] = (float)RH / H;
            pl.nameAspect = textW / RH, pl.nameFrac = textW / W;
            ++named;
        }
        GdiFlush();
        const uint8_t* src = (const uint8_t*)bits;
        for (size_t k = 0; k < fill.size(); ++k) fill[k] = src[k * 4 + 1];
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
    }
    if (!bm) BuildOutline(fill, W, H, RH, &rgba);
    if (named && GlobeD3D::SetNames(rgba.data(), W, H))
        Log("globe: %d planet name(s) drawn (%s)", named, bm ? g_settings.nameFont.c_str() : "Georgia");
}

// GDI's text as the map font's glyphs look: a dark core, a light rim (the text widened by 5 pixels,
// within each row).
void BuildOutline(const std::vector<uint8_t>& fill, int W, int H, int RH, std::vector<uint8_t>* rgbaOut) {
    std::vector<uint8_t> wide1((size_t)W * H), outline((size_t)W * H);
    const int r = 5;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t m = 0;
            for (int dx = -r; dx <= r; ++dx) {
                int xx = x + dx;
                if (xx >= 0 && xx < W && fill[(size_t)y * W + xx] > m) m = fill[(size_t)y * W + xx];
            }
            wide1[(size_t)y * W + x] = m;
        }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t m = 0;
            int row = y / RH;
            for (int dy = -r; dy <= r; ++dy) {
                int yy = y + dy;
                if (yy >= row * RH && yy < (row + 1) * RH && wide1[(size_t)yy * W + x] > m) m = wide1[(size_t)yy * W + x];
            }
            outline[(size_t)y * W + x] = m;
        }
    std::vector<uint8_t>& rgba = *rgbaOut;
    for (size_t k = 0; k < fill.size(); ++k) {
        float f = fill[k] / 255.0f;
        uint8_t grey = (uint8_t)(224.0f * (1.0f - f) + 16.0f * f);
        rgba[k * 4] = rgba[k * 4 + 1] = rgba[k * 4 + 2] = grey;
        rgba[k * 4 + 3] = (uint8_t)(fill[k] > outline[k] * 0.5f ? 190 : outline[k] * 0.5f);
    }
}

}  // namespace

bool GlobeInit() {
    if (!Engine::HaveGlobe() || !Engine::HaveAttentionCamera() || !Engine::HaveFlags() || !Engine::HaveTokens() ||
        !Engine::HaveCountries()) {
        Log("globe: engine pieces missing (camera %d, ui camera %d, flags %d, tokens %d, countries %d)", Engine::HaveGlobe(),
            Engine::HaveAttentionCamera(), Engine::HaveFlags(), Engine::HaveTokens(), Engine::HaveCountries());
        return false;
    }
    if (!MainThreadQueueAvailable()) {
        Log("globe: needs the main-thread frame hook; off");
        return false;
    }
    LoadSettings();
    bool ok = register_native_effect({"set_globe_view",
                                      "Country scope: that country's players see the map as a globe (yes) or flat (no). "
                                      "Saved with the game. Needs a mod with a common/hoi4_loader_globe file.",
                                      Form::Value,
                                      {{"enabled", ParamType::Bool, true}},
                                      ScopeMask::Country,
                                      &ExecSet,
                                      &TipSet,
                                      {kOn, kOff}},
                                     kFeature);
    ok = register_native_trigger({"is_globe_view",
                                  "Country scope: true if that country's map view is the globe (its set_globe_view, else "
                                  "the mod file's default).",
                                  Form::Value,
                                  {{"enabled", ParamType::Bool, true}},
                                  ScopeMask::Country,
                                  &EvalIs,
                                  nullptr},
                                 kFeature) &&
         ok;
    if (!ok) return false;
    OnGameEntering(&OnEntering);
    OnGameEntered(&OnEntered);
    if (!g_settings.present) {
        Log("globe: no enabled mod has a %s file: the map shaders are left alone and the map stays flat", kFolder);
        return true;
    }
    std::wstring background = g_settings.background.empty() ? L"" : FindFile(g_settings.background, "background", g_settings.backgroundFrom);
    // The planets' images, in the order of the planets (resolved against the map later).
    std::vector<std::wstring> images;
    for (const Globe::PlanetSettings& pl : g_settings.planets) {
        std::wstring img = pl.image.empty() ? L"" : FindFile(pl.image, "planet image", pl.imageFrom);
        g_imageIndex.push_back(img.empty() ? -1 : (int)images.size());
        images.push_back(img);
    }
    Log("globe: on (default %s, flat below height %.0f, a globe from %.0f or once the view reaches past the planet's edge, "
        "folding in %.2f s, latitudes %.0f to %.0f, ice from %.0f / %.0f, background %s, %zu planet block(s), overview %s)",
        g_settings.defaultOn ? "globe" : "flat", g_settings.flatHeight, g_settings.globeHeight, g_settings.transition, g_settings.south,
        g_settings.north, g_settings.polarCapNorth, g_settings.polarCapSouth,
        background.empty() ? "the game's sky" : ModList::Narrow(background).c_str(), g_settings.planets.size(),
        g_settings.overviewHeight > 0 ? (std::to_string((int)g_settings.overviewHeight) + " to " + std::to_string((int)g_settings.overviewFullHeight)).c_str() : "off");
    if (!register_file_patch(L"gfx/FX/pdxmap.shader", &PatchTerrain)) {
        Log("globe: the shader patch for pdxmap.shader could not be registered; globe off");
        return false;
    }
    static const wchar_t* const kShaders[] = {L"gfx/FX/border.shader",
                                              L"gfx/FX/river.shader",  L"gfx/FX/tree.shader",     L"gfx/FX/pdxmesh.shader"};
    static const wchar_t* const kCulled[] = {L"gfx/FX/maparrow.shader", L"gfx/FX/strait.shader", L"gfx/FX/traderoute.shader",
                                             L"gfx/FX/particle.shader"};
    if (!register_file_patch(L"gfx/FX/mapname.shader", &PatchLabels)) {
        Log("globe: the shader patch for mapname.shader could not be registered; globe off");
        return false;
    }
    for (const wchar_t* f : kShaders)
        if (!register_file_patch(f, &PatchShader)) {
            Log("globe: the shader patch for %ls could not be registered; globe off", f);
            return false;
        }
    for (const wchar_t* f : kCulled)
        if (!register_file_patch(f, &PatchCulled)) {
            Log("globe: the shader patch for %ls could not be registered; globe off", f);
            return false;
        }
    if (!register_file_patch(L"gfx/FX/pdxwater.shader", &PatchWater)) {
        Log("globe: the shader patch for pdxwater.shader could not be registered; globe off");
        return false;
    }
    if (!register_file_patch(L"gfx/FX/standardfuncsgfx.fxh", &PatchShared) || !register_file_patch(L"gfx/FX/sky.shader", &PatchSky)) {
        Log("globe: the fog or sky patch could not be registered; globe off");
        return false;
    }
    if (!GlobeD3D::Install(background, images) || !Engine::GlobeInstall({&AfterUpdate, &ScreenRay, &SkipEdgeClamp, &HideIcons})) {
        Log("globe: hooks could not be queued; globe off");
        return false;
    }
    OnMainThreadFrame(&Frame);
    return true;
}
