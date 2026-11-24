// globe (engine tier): the map as a planet (NOTES.md Part S).
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

Globe::SphereState g_sphere;
float g_t = 0.0f;              // 0 flat .. 1 sphere, moved toward the target over `transition`
bool g_edgeInView = false;     // the flat view reaches past the north or south edge (last fresh area)
float g_height = 0.0f;         // the camera's height (eye y)
ULONGLONG g_lastTick = 0;

void Publish(const Globe::Params& p) {
    AcquireSRWLockExclusive(&g_paramsLock);
    g_params = p;
    ReleaseSRWLockExclusive(&g_paramsLock);
    static_assert(sizeof(Globe::Params) == 24 * sizeof(float), "the shader constants are 24 floats");
    GlobeD3D::Publish(p.a);
}

Globe::Params Current() {
    AcquireSRWLockShared(&g_paramsLock);
    Globe::Params p = g_params;
    ReleaseSRWLockShared(&g_paramsLock);
    return p;
}

// fresh: right after the game's camera update (the visible area is the game's own, for the flat map).
void Apply(void* mapCamera, bool fresh) {
    float eye[3], look[3], w = 0, h = 0;
    if (!Engine::GlobeCameraPose(mapCamera, eye, look) || !Engine::GlobeMapSize(&w, &h)) {
        Publish(Globe::Params());
        return;
    }
    g_height = eye[1];
    if (fresh) {
        float zmin, zmax;
        Engine::GlobeAreaZRange(mapCamera, &zmin, &zmax);
        g_edgeInView = zmin < 0.0f || zmax > h;
    }
    // The ground point the camera looks at (y = 0).
    float d[3] = {look[0] - eye[0], look[1] - eye[1], look[2] - eye[2]};
    float lx = look[0], lz = look[2];
    if (d[1] < -1e-4f) {
        float s = -eye[1] / d[1];
        lx = eye[0] + s * d[0], lz = eye[2] + s * d[2];
    }
    lz = lz < 0 ? 0 : (lz > h ? h : lz);
    Globe::Params p;
    if (!Globe::MakeParams(g_settings, w, h, g_t, lx, lz, &p)) {
        Publish(Globe::Params());
        return;
    }
    p.c[3] = GlobeD3D::HaveBackground() ? 1.0f : 0.0f;
    g_mapW = w, g_mapH = h;
    Publish(p);
    // The main camera draws the whole planet: the wrap copies are off, every frustum plane open.
    Engine::GlobeHideCopies(mapCamera);
    for (int k = 0; k < 3; ++k) Engine::GlobeOpenFrustum(Engine::GlobeEngineCamera(mapCamera, k));
    if (fresh) Engine::GlobeWidenArea(mapCamera, lx, lz, 50.0f);
    // Panning turns the planet without end; when it folds back into the flat map, the camera goes
    // back over the map (whole map widths: the planet looks the same) so the flat map is under it.
    // Not while it is a sphere: a drag would jump by the width. From the next frame on: this frame's
    // matrices are made already.
    if (!g_sphere.sphere && (lx < 0.0f || lx >= w)) Engine::GlobeShiftCamera(mapCamera, -w * floorf(lx / w));
    if (!g_loggedActive) {
        g_loggedActive = true;
        Log("globe: first globe frame (camera height %.0f, map %.0f x %.0f, background %s)", eye[1], w, h,
            p.c[3] > 0.5f ? "on" : "the game's sky");
    }
}

// Flat again: the camera back over the map if a turn of the planet left it beyond the wrap line.
void ShiftIntoMap(void* mapCamera) {
    float eye[3], look[3], w = 0, h = 0;
    if (!Engine::GlobeCameraPose(mapCamera, eye, look) || !Engine::GlobeMapSize(&w, &h)) return;
    if (look[0] < 0.0f || look[0] >= w) Engine::GlobeShiftCamera(mapCamera, -w * floorf(look[0] / w));
}

void AfterUpdate(void* mapCamera) {
    void* game = g_gameCam.load();
    if (mapCamera != game) {
        if (!game) Publish(Globe::Params());  // the main menu's camera: flat
        return;
    }
    if (!g_wanted.load() && g_t <= 0.0f) {
        Publish(Globe::Params());
        float zmin, zmax;  // keep the edge state current for the moment the view is wanted
        float w = 0, h = 0;
        if (Engine::GlobeMapSize(&w, &h)) {
            Engine::GlobeAreaZRange(mapCamera, &zmin, &zmax);
            g_edgeInView = zmin < 0.0f || zmax > h;
        }
        return;
    }
    Apply(mapCamera, true);
}

void Frame() {
    void* cam = Engine::GameState() ? Engine::UiMapCamera() : nullptr;
    g_gameCam.store(cam);
    int player = cam ? Engine::PlayerCountryTag() : 0;
    bool wanted = cam && g_settings.present && GlobeD3D::Ready() && (player > 0 ? ViewOf(player) : g_settings.defaultOn);
    g_wanted.store(wanted);
    ULONGLONG now = GetTickCount64();
    float dt = g_lastTick ? (float)(now - g_lastTick) / 1000.0f : 0.0f;
    g_lastTick = now;
    if (!cam) {
        g_t = 0.0f, g_sphere = Globe::SphereState();
        return;
    }
    float eye[3], look[3];
    if (Engine::GlobeCameraPose(cam, eye, look)) g_height = eye[1];
    bool sphere = wanted && Globe::WantSphere(g_settings, g_height, g_edgeInView, &g_sphere);
    if (!wanted) g_sphere = Globe::SphereState();
    float target = sphere ? 1.0f : 0.0f;
    float step = g_settings.transition > 0.0f ? dt / g_settings.transition : 1.0f;
    g_t = g_t < target ? (g_t + step > target ? target : g_t + step) : (g_t - step < target ? target : g_t - step);
    if (g_t <= 0.0f) {
        Publish(Globe::Params());
        ShiftIntoMap(cam);
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
    // The map camera's own rays (drag, edge panning, zoom to the cursor) stay the game's: the camera
    // moves as over the flat map, smoothly; on the planet that turns it.
    if (!(p.t() > 0.0f) || fromCamera) return;
    // The wrap copies cast the same ray shifted by a map width: shift it back, and the flat point
    // into the copy's frame again, so all three cameras find the same place.
    float shift = g_mapW > 0 ? g_mapW * roundf((origin[0] - p.b[0]) / g_mapW) : 0.0f;
    float o[3] = {origin[0] - shift, origin[1], origin[2]};
    float flat[2];
    bool hit = false;
    if (!Globe::Unbend(p, o, dir, g_mapW, g_mapH, flat, &hit)) return;
    if (!hit) {
        // Picking past the planet's rim or on the polar seas: a ray onto a point far off the map, which
        // holds nothing.
        flat[0] = o[0], flat[1] = -100000.0f;
    } else {
        // The copy of the point on the camera's side of the wrap line (5832, not 200, for a camera at
        // 5620): the direction must point the way the cursor is; the game's wrap copies find the province.
        flat[0] += g_mapW * roundf((o[0] - flat[0]) / g_mapW);
    }
    flat[0] += shift;
    // From the camera onto the flat point (y = 0): the game's own hit test finds what lies there, and
    // the camera's own rays (edge panning, zoom to the cursor) keep a direction that points the way the
    // cursor is (on the rim for a cursor past it).
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
    Log("globe: sky.shader %s", out.empty() ? "NOT patched (its main was not found as expected): the game's sky stays" : "patched (background behind the planet)");
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

// The background named in the settings, as the game would find it: the last enabled mod that has the
// file, else the game folder. "" if none has it.
std::wstring FindBackground() {
    if (g_settings.background.empty()) return L"";
    std::wstring rel = ModList::Wide(g_settings.background);
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
    Log("globe: the background %s (named in %s) is in no enabled mod nor the game folder; the game's sky stays",
        g_settings.background.c_str(), g_settings.backgroundFrom.c_str());
    return L"";
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
    std::wstring background = FindBackground();
    Log("globe: on (default %s, flat below height %.0f, a globe from %.0f or once the view reaches past the map's edge, "
        "folding in %.2f s, latitudes %.0f to %.0f, ice from %.0f / %.0f, background %s)",
        g_settings.defaultOn ? "globe" : "flat", g_settings.flatHeight, g_settings.globeHeight, g_settings.transition, g_settings.south,
        g_settings.north, g_settings.polarCapNorth, g_settings.polarCapSouth,
        background.empty() ? "the game's sky" : ModList::Narrow(background).c_str());
    if (!register_file_patch(L"gfx/FX/pdxmap.shader", &PatchTerrain)) {
        Log("globe: the shader patch for pdxmap.shader could not be registered; globe off");
        return false;
    }
    static const wchar_t* const kShaders[] = {L"gfx/FX/border.shader",
                                              L"gfx/FX/river.shader",  L"gfx/FX/tree.shader",     L"gfx/FX/pdxmesh.shader"};
    static const wchar_t* const kCulled[] = {L"gfx/FX/mapname.shader", L"gfx/FX/maparrow.shader", L"gfx/FX/strait.shader",
                                             L"gfx/FX/traderoute.shader", L"gfx/FX/particle.shader"};
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
    if (!GlobeD3D::Install(background) || !Engine::GlobeInstall({&AfterUpdate, &ScreenRay, &SkipEdgeClamp, &HideIcons})) {
        Log("globe: hooks could not be queued; globe off");
        return false;
    }
    OnMainThreadFrame(&Frame);
    return true;
}
