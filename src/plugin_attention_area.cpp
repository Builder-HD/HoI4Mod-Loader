// attention_area feature (NOTES.md Part M): mark provinces on the map, every look a shader, and
// move the camera to them.
//
//   add_attention_area = { id = <name> line = <shader> fill = <shader> color = <name>
//                          red/green/blue/alpha = <0..1> area = { ... } area = { ... } }
//
// line: the shader for the area's outline (the game's thin province group outline, drawn by
// border.shader); fill: the shader for its provinces (through the map's stripe texture, drawn by
// secondary_color_mask). Built-ins: lines solid, pulse, dashed; fills stripes, solid, band, glow;
// mods add or replace shaders (shader_patterns.h, NOTES.md M12). Neither given: line = solid.
//   remove_attention_area = <name> | all
//   focus_attention_area = { id = <name> zoom = <camera height> }     (the player's country only)
//   has_attention_area = <name>                                        (trigger)
//
// An area block selects provinces: the listed ones (province, state, strategic_region; any number
// of each) or, without a list, every land province; then the filters (owner, controller, coastal)
// keep those that match all of them. Several area blocks add up. The selection is made when the
// effect runs, from game state only, in province id order: the same on every machine.
//
// Areas are display state of this session: not saved, dropped when another game is entered (mods
// add them again from on_startup / on_game_loaded). The drawing happens on the main thread at the
// next frame.
#include <math.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "native_script.h"
#include "shader_patterns.h"

namespace {

const char kFeature[] = "attention_area";
const float kFitFactor = 1.6f;  // camera height per pixel of the box's larger side (see Frame)

struct Area {
    int token = 0;          // id (engine token)
    std::string name;
    int lineId = 0;         // line shader id (0: no outline)
    int fillId = 0;         // fill shader id (0: no fill)
    float r = 1, g = 0.9f, b = 0.1f, a = 1;  // the area's colour
    std::vector<std::pair<std::string, float>> vars;  // shader_variables given in script
    std::vector<int> provinces;
    int box[4] = {};        // bounding box in map pixels {x, y, w, h}
    int ref = 0, ref2 = 0;  // camera calibration provinces
    float refDist = 0;      // map pixels from the box centre to the nearest province centre
};

std::vector<Area> g_areas;  // main thread only (effects, the frame)
// What has_attention_area reads: the marked ids, a copy kept under its own lock. Native triggers can
// run on other threads than the effects that change g_areas (#17 review), so the trigger never
// touches g_areas. SyncMarked() runs after every change.
SRWLOCK g_markedLock = SRWLOCK_INIT;
std::vector<int> g_marked;  // guarded by g_markedLock
void SyncMarked() {
    std::vector<int> ids;
    ids.reserve(g_areas.size());
    for (const Area& a : g_areas) ids.push_back(a.token);
    AcquireSRWLockExclusive(&g_markedLock);
    g_marked.swap(ids);
    ReleaseSRWLockExclusive(&g_markedLock);
}
std::vector<Engine::OutlineRef> g_outlines;  // the outlines shown for areas with a line
const int kAreaPriority = 90;                // below the railway hover mark (100)
volatile long g_noColourLogged = 0, g_noOutlineLogged = 0, g_noStripesLogged = 0, g_unpatchedLogged = 0;
bool g_dirty = false;
int g_focusToken = 0;
float g_focusHeight = 0;
bool g_focusPending = false;
volatile long g_missingFocusLogged = 0;

// --- Hover mark: the railway queue entry the player points at (UI only, this machine) ------------
// The game builds the entry's tooltip while it is hovered (railway_queue's tooltip hook reports the
// line's path). The mark stays until no tooltip build came for kHoverHoldMs. How often the tooltip
// is built during one hover is logged for the first hovers (NOTES.md M7: does the game rebuild it
// every frame, or only once?).
const ULONGLONG kHoverHoldMs = 250;
std::vector<int> g_hoverPath;  // provinces of the hovered line, sorted, unique
ULONGLONG g_hoverLast = 0, g_hoverStart = 0;
int g_hoverCalls = 0, g_hoverLogs = 0;
bool g_hoverShown = false, g_hoverChanged = false;
volatile long g_whiteMissingLogged = 0;
int g_hoverEnds[2] = {0, 0};  // the hovered line's first and last province (path order)
bool g_lineFocusPending = false;

void OnRailwayHover(const int* path, int count) {
    g_hoverEnds[0] = path[0];
    g_hoverEnds[1] = path[count - 1];
    std::vector<int> p(path, path + count);
    std::sort(p.begin(), p.end());
    p.erase(std::unique(p.begin(), p.end()), p.end());
    ULONGLONG now = GetTickCount64();
    if (p != g_hoverPath || !g_hoverShown) {
        g_hoverPath = std::move(p);
        g_hoverChanged = true;
        g_hoverStart = now;
        g_hoverCalls = 0;
    }
    ++g_hoverCalls;
    g_hoverLast = now;
}

// Right-click on a railway queue entry: the game centres on the line's start (its centre-on-province).
// While that line is hovered, the loader takes the call over and fits the whole line in the next frame
// instead; a line too long to fit ends up centred at the camera's highest zoom (the height is clamped).
bool OnCentre(int province) {
    if (!g_hoverShown || province <= 0 || (province != g_hoverEnds[0] && province != g_hoverEnds[1])) return false;
    g_lineFocusPending = true;
    return true;
}

// An end of the railway line hovered right now (its tooltip came within the hold time). The game marks
// the start province as the hovered item, which is drawn over every outline: while the loader's outline
// is up that mark is dropped, so the outline is complete (sixth in-game test, NOTES.md M7).
bool IsHoveredLineEnd(int province) {
    return province > 0 && GetTickCount64() - g_hoverLast <= kHoverHoldMs &&
           (province == g_hoverEnds[0] || province == g_hoverEnds[1]);
}
void HoverFrame() {
    ULONGLONG now = GetTickCount64();
    if (g_hoverChanged) {
        g_hoverChanged = false;
        g_hoverShown = true;
        // The game's thin province group outline (as construction mode draws build targets), in white:
        // the loader's own colour slot appended to the renderer's table (no vanilla colour changes),
        // else the palette's first colour.
        Engine::ClearHoveredProvinceIf(&IsHoveredLineEnd);  // set before the first tooltip of this hover
        int white = Engine::HoverWhiteId();
        if (!Engine::ShowHighlight(g_hoverPath, white >= 0 ? white : Engine::HighlightPaletteColourId(0)) && g_hoverLogs < 5)
            Log("attention_area: the railway hover outline could not be created");
        if (white < 0 && InterlockedExchange(&g_whiteMissingLogged, 1) == 0)
            Log("attention_area: no white outline colour (the renderer's colour table was not extended): hover "
                "outlines use palette colour 0");
    } else if (g_hoverShown && now - g_hoverLast > kHoverHoldMs) {
        g_hoverShown = false;
        if (g_hoverLogs < 5) {
            ++g_hoverLogs;
            Log("attention_area: railway hover mark (%zu provinces) shown %llu ms; the tooltip was built %d time(s) meanwhile%s",
                g_hoverPath.size(), g_hoverLast - g_hoverStart + kHoverHoldMs, g_hoverCalls,
                g_hoverLogs == 5 ? " (last such line)" : "");
        }
        g_hoverPath.clear();
        Engine::HideHighlight();
    }
}

// --- Named colours ----------------------------------------------------------------------------------
const char kColors[] = "yellow|white|black|red|green|blue|cyan|magenta|orange|purple|grey|pink";
const float kRgb[][3] = {{1, 0.9f, 0.1f}, {1, 1, 1},  {0, 0, 0},       {0.9f, 0.1f, 0.1f}, {0.1f, 0.8f, 0.2f}, {0.15f, 0.35f, 1},
                         {0, 0.9f, 0.9f}, {0.9f, 0, 0.9f}, {1, 0.55f, 0}, {0.55f, 0.2f, 0.85f}, {0.6f, 0.6f, 0.6f}, {1, 0.5f, 0.7f}};

// --- Parameters -------------------------------------------------------------------------------------
enum AddParam { kId, kLine, kFill, kColor, kRed, kGreen, kBlue, kAlpha, kVars, kArea };
enum AreaKey { kProvince, kState, kRegion, kOwner, kController, kCoastal, kRandomProvinces };

ParamSpec Repeat(const char* name) {
    ParamSpec p{name, ParamType::Int, false};
    p.repeat = true;
    return p;
}

ParamSpec RepeatState() {
    ParamSpec p = Repeat("state");
    p.choices = "this";  // the state in scope (random_owned_state = { add_attention_area = { ... state = this } })
    return p;
}

const std::vector<ParamSpec> kAreaKeys = {
    Repeat("province"),
    RepeatState(),
    Repeat("strategic_region"),
    {"owner", ParamType::Country, false},
    {"controller", ParamType::Country, false},
    {"coastal", ParamType::Bool, false},
    {"random_provinces", ParamType::Int, false},  // keep this many of the block's provinces, picked at random
};

// The random picks of random_provinces must come out the same on every machine in multiplayer, so
// they come from a counter of add_attention_area runs (every machine runs the same effects in the
// same order), not from the clock. Areas are not saved, so a fresh start needs no stored state.
uint64_t g_addCount = 0;
uint64_t Mix(uint64_t x) {  // splitmix64
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// shader_variables = { <name> = <number> ... color = <name> }: the keys are every variable the
// collected shaders declare (known before the effect is registered, so the game's own parser checks
// them), plus color. Built once in AttentionAreaInit; the names must outlive the registration.
std::vector<std::string> g_varNames;   // index i + 1 in g_varKeys
std::vector<ParamSpec> g_varKeys;      // [0] color, then one Number per name

void BuildVarKeys() {
    for (ShaderPatterns::Kind k : {ShaderPatterns::Kind::Line, ShaderPatterns::Kind::Fill})
        for (const ShaderPatterns::Pattern& p : ShaderPatterns::Patterns(k))
            for (const ShaderPatterns::Var& v : p.vars)
                if (v.name != "color" && std::find(g_varNames.begin(), g_varNames.end(), v.name) == g_varNames.end())
                    g_varNames.push_back(v.name);
    std::sort(g_varNames.begin(), g_varNames.end());
    g_varKeys.push_back({"color", ParamType::Choice, false, ScopeDefault::None, kColors});
    for (const std::string& n : g_varNames) g_varKeys.push_back({n.c_str(), ParamType::Number, false});
}

ParamSpec VarsParam() {
    ParamSpec p{"shader_variables", ParamType::Block, false};
    p.block = &g_varKeys;
    return p;
}

// A declared variable's value for an area: shader_variables, else the area's colour for
// red/green/blue/alpha, else the declared default.
float VarValue(const Area& a, const ShaderPatterns::Var& v) {
    for (const auto& kv : a.vars)
        if (kv.first == v.name) return kv.second < v.min ? v.min : kv.second > v.max ? v.max : kv.second;
    if (v.name == "red") return a.r;
    if (v.name == "green") return a.g;
    if (v.name == "blue") return a.b;
    if (v.name == "alpha") return a.a;
    return v.def;
}
float VarNorm(const Area& a, const ShaderPatterns::Var& v) { return (VarValue(a, v) - v.min) / (v.max - v.min); }

ParamSpec AreaParam() {
    ParamSpec p{"area", ParamType::Block, true};
    p.repeat = true;
    p.block = &kAreaKeys;
    return p;
}

// --- The area selector (kept apart from the drawing: NOTES.md M4) -----------------------------------
// province -> state id (0: not in a state, i.e. sea or wasteland outside states).
std::vector<int> ProvinceStates() {
    std::vector<int> of((size_t)std::max(Engine::ProvinceCount(), 0), 0);
    std::vector<void*> list;
    for (int s = 1; s < Engine::StateCount(); ++s) {
        if (!Engine::AreaProvinceObjects(Engine::AreaKind::State, s, &list)) continue;
        for (void* p : list) {
            int id = Engine::ProvinceIdOf(p);
            if (id > 0 && id < (int)of.size()) of[id] = s;
        }
    }
    return of;
}

void AddIds(Engine::AreaKind kind, int id, std::vector<char>* in) {
    std::vector<void*> list;
    if (!Engine::AreaProvinceObjects(kind, id, &list)) return;
    for (void* p : list) {
        int pid = Engine::ProvinceIdOf(p);
        if (pid > 0 && pid < (int)in->size()) (*in)[pid] = 1;
    }
}

// The provinces one area block selects, added to *out (a flag per province id).
void SelectArea(const Args& a, const Scope& scope, const std::vector<int>& stateOf, std::vector<char>* out) {
    size_t n = stateOf.size();
    std::vector<char> in(n, 0);
    bool listed = a.has(kProvince) || a.has(kState) || a.has(kRegion);
    if (!listed) {
        for (size_t p = 1; p < n; ++p) in[p] = stateOf[p] != 0;
    } else {
        for (size_t k = 0; k < a.count(kProvince); ++k) {
            int p = a.at(kProvince, k);
            if (p > 0 && p < (int)n) in[p] = 1;
        }
        for (size_t k = 0; k < a.count(kState); ++k) {
            int st = a.at(kState, k);
            if (IntWordIndex(st) == 0) st = scope.stateId();  // `this`: 0 outside a state scope, which selects nothing
            if (st > 0) AddIds(Engine::AreaKind::State, st, &in);
        }
        for (size_t k = 0; k < a.count(kRegion); ++k) AddIds(Engine::AreaKind::StrategicRegion, a.at(kRegion, k), &in);
    }
    int owner = a.has(kOwner) ? native_country(a.i(kOwner), scope) : 0;
    int controller = a.has(kController) ? native_country(a.i(kController), scope) : 0;
    if ((a.has(kOwner) && owner <= 0) || (a.has(kController) && controller <= 0)) return;  // names no country here
    std::vector<int> kept;
    for (size_t p = 1; p < n; ++p) {
        if (!in[p]) continue;
        if (owner && !Engine::IsSameCountry(Engine::StateOwner(stateOf[p]), owner)) continue;
        if (controller && !Engine::IsSameCountry(Engine::ProvinceController((int)p), controller)) continue;
        if (a.has(kCoastal) && Engine::ProvinceIsCoastal((int)p) != a.b(kCoastal)) continue;
        kept.push_back((int)p);
    }
    if (a.has(kRandomProvinces)) {  // a partial Fisher-Yates shuffle: the first `want` are the picks
        size_t want = (size_t)std::max(0, a.i(kRandomProvinces));
        uint64_t r = Mix(g_addCount * 0x100000001B3ull + kept.size());
        for (size_t i = 0; i < want && i + 1 < kept.size(); ++i) {
            r = Mix(r);
            std::swap(kept[i], kept[i + (size_t)(r % (kept.size() - i))]);
        }
        if (want < kept.size()) kept.resize(want);
    }
    for (int p : kept) (*out)[p] = 1;
}

// Bounding box and the two calibration provinces: the one nearest the box centre, and the one
// farthest from it vertically (so the camera's z direction can be measured).
void Measure(Area* area) {
    bool any = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::vector<std::pair<int, std::pair<float, float>>> centres;
    for (int p : area->provinces) {
        int r[4];
        if (!Engine::ProvinceRect(p, r)) continue;
        if (!any) x0 = r[0], y0 = r[1], x1 = r[0] + r[2], y1 = r[1] + r[3], any = true;
        x0 = std::min(x0, r[0]), y0 = std::min(y0, r[1]);
        x1 = std::max(x1, r[0] + r[2]), y1 = std::max(y1, r[1] + r[3]);
        centres.push_back({p, {r[0] + r[2] * 0.5f, r[1] + r[3] * 0.5f}});
    }
    area->box[0] = x0, area->box[1] = y0, area->box[2] = x1 - x0, area->box[3] = y1 - y0;
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, best = 1e30f, farthest = -1;
    for (const auto& c : centres) {
        float d = (c.second.first - cx) * (c.second.first - cx) + (c.second.second - cy) * (c.second.second - cy);
        if (d < best) best = d, area->ref = c.first;
    }
    area->refDist = centres.empty() ? 0 : sqrtf(best);
    float refY = cy;
    for (const auto& c : centres)
        if (c.first == area->ref) refY = c.second.second;
    for (const auto& c : centres)
        if (fabsf(c.second.second - refY) > farthest) farthest = fabsf(c.second.second - refY), area->ref2 = c.first;
}

Area* FindArea(int token) {
    for (Area& a : g_areas)
        if (a.token == token) return &a;
    return nullptr;
}

float Clamp01(double v) { return v < 0 ? 0.0f : v > 1 ? 1.0f : (float)v; }

// --- Effects and trigger ----------------------------------------------------------------------------
// A line / fill shader name -> its id (0 when not given). An unknown name falls back to the built-in
// solid line or stripes fill, so the area still shows; logged once per name.
int PatternId(ShaderPatterns::Kind kind, const Args& a, int param) {
    if (!a.has(param)) return 0;
    std::string name = a.key(param);
    int id = ShaderPatterns::Id(kind, name);
    if (id) return id;
    bool line = kind == ShaderPatterns::Kind::Line;
    static std::vector<std::string> logged;
    if (std::find(logged.begin(), logged.end(), name) == logged.end()) {
        logged.push_back(name);
        Log("attention_area: no %s shader named '%s'; using '%s' (shaders: the built-ins and the enabled mods' "
            "gfx/FX/hoi4_loader_patterns folders; see the shader_patterns lines at startup)",
            line ? "line" : "fill", name.c_str(), line ? "solid" : "stripes");
    }
    return ShaderPatterns::Id(kind, line ? "solid" : "stripes");
}

void ExecAdd(const Args& a, const Scope& s) {
    Area area;
    area.token = a.i(kId);
    area.name = a.key(kId);
    area.lineId = PatternId(ShaderPatterns::Kind::Line, a, kLine);
    area.fillId = PatternId(ShaderPatterns::Kind::Fill, a, kFill);
    if (!a.has(kLine) && !a.has(kFill)) area.lineId = ShaderPatterns::Id(ShaderPatterns::Kind::Line, "solid");
    int c = a.has(kColor) ? a.i(kColor) : 0;
    area.r = kRgb[c][0], area.g = kRgb[c][1], area.b = kRgb[c][2];
    if (a.has(kRed)) area.r = Clamp01(a.num(kRed));
    if (a.has(kGreen)) area.g = Clamp01(a.num(kGreen));
    if (a.has(kBlue)) area.b = Clamp01(a.num(kBlue));
    area.a = a.has(kAlpha) ? Clamp01(a.num(kAlpha)) : 1.0f;
    if (a.has(kVars)) {
        Args v = a.block(kVars, 0);
        if (v.has(0)) {  // color inside the block
            int vc = v.i(0);
            area.r = kRgb[vc][0], area.g = kRgb[vc][1], area.b = kRgb[vc][2];
        }
        for (size_t i = 0; i < g_varNames.size(); ++i)
            if (v.has((int)i + 1)) area.vars.push_back({g_varNames[i], (float)v.num((int)i + 1)});
    }
    ++g_addCount;
    std::vector<int> stateOf = ProvinceStates();
    std::vector<char> pick(stateOf.size(), 0);
    for (size_t k = 0; k < a.count(kArea); ++k) SelectArea(a.block(kArea, k), s, stateOf, &pick);
    for (size_t p = 1; p < pick.size(); ++p)
        if (pick[p]) area.provinces.push_back((int)p);
    Measure(&area);
    Log("attention_area: '%s' marks %zu provinces (box %d,%d %dx%d)", area.name.c_str(), area.provinces.size(), area.box[0],
        area.box[1], area.box[2], area.box[3]);
    if (Area* old = FindArea(area.token)) *old = std::move(area);
    else g_areas.push_back(std::move(area));
    g_dirty = true;
    SyncMarked();
}

void ExecRemove(const Args& a, const Scope&) {
    if (strcmp(a.key(0), "all") == 0) {
        g_dirty = !g_areas.empty();
        g_areas.clear();
        SyncMarked();
        return;
    }
    for (size_t i = 0; i < g_areas.size(); ++i)
        if (g_areas[i].token == a.i(0)) {
            g_areas.erase(g_areas.begin() + (long long)i);
            g_dirty = true;
            SyncMarked();
            return;
        }
}

void ExecFocus(const Args& a, const Scope& s) {
    // The camera of this machine's player only, as vanilla goto_state does.
    int player = Engine::PlayerCountryTag();
    if (player <= 0 || !Engine::IsSameCountry(s.countryTag(), player)) return;
    g_focusToken = a.i(0);
    g_focusHeight = a.has(1) ? (float)a.num(1) : 0.0f;
    g_focusPending = true;
}

bool EvalHas(const Args& a, const Scope&) {
    AcquireSRWLockShared(&g_markedLock);
    bool found = std::find(g_marked.begin(), g_marked.end(), a.i(0)) != g_marked.end();
    ReleaseSRWLockShared(&g_markedLock);
    return found;
}

// --- Frame, game entry ------------------------------------------------------------------------------
// The id to draw: a shader that is not usable (failed the compile check) gives the built-in solid
// line or stripes fill; logged once per shader.
int Fallback(ShaderPatterns::Kind kind, int id) {
    if (!id || ShaderPatterns::Usable(kind, id)) return id;
    bool line = kind == ShaderPatterns::Kind::Line;
    static std::vector<std::string> logged;
    const std::string& name = ShaderPatterns::Patterns(kind)[(size_t)id - 1].name;
    if (std::find(logged.begin(), logged.end(), name) == logged.end()) {
        logged.push_back(name);
        Log("attention_area: the %s shader '%s' failed the compile check (see the shader_patterns lines): '%s' is drawn instead",
            line ? "line" : "fill", name.c_str(), line ? "solid" : "stripes");
    }
    return ShaderPatterns::Id(kind, line ? "solid" : "stripes");
}

void ReleaseOutlines() {
    for (Engine::OutlineRef& o : g_outlines) Engine::ReleaseOutline(&o);
    g_outlines.clear();
}

// All areas drawn again (adds and removes are rare): bands through the gradient borders, outlines as
// one game outline each. Identical colours share a colour slot. Areas added later rank lower, so on an
// edge two outlines share, the earlier area's colour shows.
void Redraw() {
    ReleaseOutlines();
    Engine::ResetOutlineColours();
    std::vector<Engine::StripeArea> stripes;
    auto byte = [](float v) { return (uint32_t)(v * 255.0f + 0.5f); };
    bool fillPatched = ShaderPatterns::Patched(ShaderPatterns::Kind::Fill);
    bool linePatched = ShaderPatterns::Patched(ShaderPatterns::Kind::Line);
    for (size_t i = 0; i < g_areas.size(); ++i) {
        const Area& a = g_areas[i];
        if (a.provinces.empty()) continue;
        // A shader that failed the compile check (NOTES.md M15) is not in the game's shaders: the
        // built-in solid line or stripes fill is drawn in its place.
        int fillId = Fallback(ShaderPatterns::Kind::Fill, a.fillId);
        int lineId = Fallback(ShaderPatterns::Kind::Line, a.lineId);
        if ((fillId && !fillPatched) || (lineId && !linePatched)) {
            if (InterlockedExchange(&g_unpatchedLogged, 1) == 0)
                Log("attention_area: a shader file was not patched (fill %d, line %d): such areas show plain stripes and a plain "
                    "outline (logged once)", fillPatched, linePatched);
        }
        // Fill: the area's colour in the stripe texture, the fill shader's id in the alpha byte
        // (unpatched: full-strength vanilla stripes). The texture is BGRA (byte 0 blue, byte 2 red).
        if (fillId && Engine::HaveStripes()) {
            Engine::StripeArea s;
            // Patched: the fill id in the alpha byte, the shader's four values in the colour bytes
            // (its declared variables normalised, else the colour and alpha; FillTexelN).
            // Unpatched: the plain colour at full vanilla strength.
            const ShaderPatterns::Pattern& fp = ShaderPatterns::Patterns(ShaderPatterns::Kind::Fill)[(size_t)fillId - 1];
            float n[4] = {a.r, a.g, a.b, a.a};
            if (!fp.vars.empty())
                for (int k = 0; k < 4; ++k) n[k] = k < (int)fp.vars.size() ? VarNorm(a, fp.vars[(size_t)k]) : 0.0f;
            s.rgba = fillPatched ? ShaderPatterns::FillTexelN(fillId, n)
                                 : byte(a.b) | byte(a.g) << 8 | byte(a.r) << 16 | 255u << 24;
            s.provinces = a.provinces;
            stripes.push_back(std::move(s));
        } else if (fillId && InterlockedExchange(&g_noStripesLogged, 1) == 0) {
            Log("attention_area: the stripe hook is not there: fills are not drawn");
        }
        // Line: one game outline in the area's colour, the line shader's id in the colour's alpha
        // (unpatched: the plain outline in the area's colour).
        if (!lineId) continue;
        if (!Engine::HaveHighlight()) {
            if (InterlockedExchange(&g_noOutlineLogged, 1) == 0)
                Log("attention_area: the game's outline functions are not there: lines are not drawn");
            continue;
        }
        // Patched: the line id and the 4th value in the slot alpha; the first three values in rgb as
        // they are (the declared variables, else the colour).
        const ShaderPatterns::Pattern& lp = ShaderPatterns::Patterns(ShaderPatterns::Kind::Line)[(size_t)lineId - 1];
        float lv[3] = {a.r, a.g, a.b}, l4 = a.a;
        if (linePatched && !lp.vars.empty()) {
            for (int k = 0; k < 3; ++k) lv[k] = k < (int)lp.vars.size() ? VarValue(a, lp.vars[(size_t)k]) : 0.0f;
            l4 = lp.vars.size() > 3 ? VarNorm(a, lp.vars[3]) : 1.0f;
        }
        float alpha = linePatched ? ShaderPatterns::LineAlpha(lineId, l4) : a.a;
        int colour = Engine::OutlineColourId(lv[0], lv[1], lv[2], alpha);
        if (colour < 0) {
            colour = Engine::HighlightPaletteColourId(0);
            if (InterlockedExchange(&g_noColourLogged, 1) == 0)
                Log("attention_area: no free outline colour (the renderer's colour table was not extended, or more than 31 "
                    "colours are in use): such outlines use colour %d (logged once)", colour);
        }
        Engine::OutlineRef ref;
        int priority = kAreaPriority - (int)std::min<size_t>(i, 80);
        if (Engine::CreateOutline(a.provinces, colour, priority, &ref)) g_outlines.push_back(ref);
        else Log("attention_area: the outline of '%s' could not be created", a.name.c_str());
    }
    if (Engine::HaveStripes()) Engine::SetStripes(stripes);
}

void Frame() {
    HoverFrame();
    if (g_lineFocusPending) {
        g_lineFocusPending = false;
        Area line;
        line.provinces = g_hoverPath;
        Measure(&line);
        static int logged = 0;
        if (logged < 3 && ++logged)
            Log("attention_area: right-click on a railway entry: fitting the whole line (%zu provinces, box %dx%d)%s",
                line.provinces.size(), line.box[2], line.box[3], logged == 3 ? " (last such line)" : "");
        if (!line.provinces.empty() && line.ref > 0)
            Engine::FocusCamera(line.box, line.ref, line.ref2 == line.ref ? 0 : line.ref2, 0.0f, kFitFactor);
    }
    if (g_dirty) {
        g_dirty = false;
        Redraw();
    }
    if (g_focusPending) {
        g_focusPending = false;
        const Area* a = FindArea(g_focusToken);
        if (!a || a->provinces.empty()) {
            if (InterlockedExchange(&g_missingFocusLogged, 1) == 0)
                Log("attention_area: focus_attention_area names no area with provinces (logged once)");
            return;
        }
        // Fit factor: at camera height h the tilted view shows clearly less than h of map vertically
        // (1.0 cut China's coast at the bottom in the first test, NOTES.md M6); 1.6 leaves a margin.
        // Scattered areas (Gotland and Hainan): the box centre is far from every marked province, so
        // centring would show empty map. Then the camera stays where it is (owner's call, M6).
        float side = (float)std::max(a->box[2], a->box[3]);
        if (a->refDist > std::max(40.0f, side * 0.25f)) {
            Log("attention_area: '%s' not focused: its provinces are scattered (box %dx%d, nearest province %.0f px from the "
                "centre); the camera stays",
                a->name.c_str(), a->box[2], a->box[3], a->refDist);
            return;
        }
        Engine::FocusCamera(a->box, a->ref, a->ref2 == a->ref ? 0 : a->ref2, g_focusHeight, kFitFactor);
    }
}

void OnEntering() {
    g_areas.clear();
    SyncMarked();
    g_addCount = 0;  // every machine of a multiplayer game enters it fresh
    g_dirty = false;
    g_focusPending = false;
    g_hoverPath.clear();
    g_hoverShown = g_hoverChanged = false;
    Engine::HideHighlight();
    ReleaseOutlines();
    Engine::ResetOutlineColours();
    Engine::ForgetStripes();
}

}  // namespace

// The game's shader files as they are read (NOTES.md M10-M12): the line and fill shaders stitched in.
static std::string PatchFillFile(const std::string& original) {
    ShaderPatterns::Validate();  // once, before the first patch: the mod shaders' compile check
    std::string out = ShaderPatterns::PatchFillShader(original, ShaderPatterns::Patterns(ShaderPatterns::Kind::Fill));
    ShaderPatterns::SetPatched(ShaderPatterns::Kind::Fill, !out.empty());
    // From now on the game's own stripe alphas are kept out of the fill ids' range (NOTES.md M15).
    Engine::SetStripeIdRange(out.empty() ? 0 : (int)ShaderPatterns::Patterns(ShaderPatterns::Kind::Fill).size());
    Log("shader_patterns: standardfuncsgfx.fxh %s", out.empty() ? "NOT patched (secondary_color_mask not found as expected): fills show as plain stripes"
                                                                 : "patched with the fill shaders");
    g_dirty = true;  // areas drawn before this (none in practice: the map shaders load first) are redone
    return out;
}
static std::string PatchLineFile(const std::string& original) {
    ShaderPatterns::Validate();
    std::string out = ShaderPatterns::PatchLineShader(original, ShaderPatterns::Patterns(ShaderPatterns::Kind::Line));
    ShaderPatterns::SetPatched(ShaderPatterns::Kind::Line, !out.empty());
    Log("shader_patterns: border.shader %s", out.empty() ? "NOT patched (its pixel shader main not found as expected): lines show as plain outlines"
                                                         : "patched with the line shaders");
    g_dirty = true;
    return out;
}

bool AttentionAreaInit() {
    if (!Engine::HaveAttentionCamera() || !Engine::HaveProvinceData() || !Engine::HaveRegionData() || !Engine::HaveCountries()) {
        Log("attention_area: engine pieces missing (camera %d, provinces %d, regions %d)", Engine::HaveAttentionCamera(),
            Engine::HaveProvinceData(), Engine::HaveRegionData());
        return false;
    }
    if (!MainThreadQueueAvailable()) {
        Log("attention_area: needs the main-thread frame hook; off");
        return false;
    }
    ShaderPatterns::Collect();  // the built-ins at least
    BuildVarKeys();
    // The game's map shaders are patched only when an enabled mod uses attention areas (ships
    // shaders or the marker patch_map_shaders.txt): every other player keeps the game's own shaders and
    // has no shader recompile (NOTES.md M15). Without the patch, areas show as plain outlines and
    // stripes.
    std::string usedBy;
    if (!ShaderPatterns::UsedByMods(&usedBy)) {
        Log("shader_patterns: no enabled mod asks for attention area shaders (none ships files in "
            "gfx/FX/hoi4_loader_patterns, such as its own shaders or patch_map_shaders.txt): the game's map shaders are "
            "left alone; areas show as plain outlines and stripes");
    } else {
        Log("shader_patterns: %s: the map shaders will be patched", usedBy.c_str());
        if (!register_file_patch(L"gfx/FX/standardfuncsgfx.fxh", &PatchFillFile))
            Log("shader_patterns: the standardfuncsgfx.fxh patch could not be registered: fills show as plain stripes");
        if (!register_file_patch(L"gfx/FX/border.shader", &PatchLineFile))
            Log("shader_patterns: the border.shader patch could not be registered: lines show as plain outlines");
    }
    std::vector<ParamSpec> add = {
        {"id", ParamType::Token, true},
        {"line", ParamType::Token, false},
        {"fill", ParamType::Token, false},
        {"color", ParamType::Choice, false, ScopeDefault::None, kColors},
        {"red", ParamType::Number, false},
        {"green", ParamType::Number, false},
        {"blue", ParamType::Number, false},
        {"alpha", ParamType::Number, false},
        VarsParam(),
        AreaParam(),
    };
    bool ok = register_native_effect(
        {"add_attention_area",
         "Marks provinces on the map; every look is a shader. id names the area (adding it again replaces it). line: the "
         "shader of its outline (built-ins solid, pulse, dashed); fill: the shader of its provinces (built-ins stripes, "
         "solid, band, glow); mods add more (gfx/FX/hoi4_loader_patterns). Neither given: line = solid. color: a named "
         "colour, or red/green/blue/alpha from 0 to 1, passed to the shaders. shader_variables = { <name> = <number> ... }: "
         "values for the variables the shaders declare (up to 4 per shader; see the shader_patterns lines at startup). "
         "Each area = { } block selects provinces: "
         "province, state (or state = this) and strategic_region ids (any number, added up; none: every land province), "
         "then owner, controller and coastal keep those that match, and random_provinces = n keeps n of them. Several "
         "area blocks add up. Not saved: add areas again after loading (on_game_loaded).",
         Form::Block, add, ScopeMask::Any, &ExecAdd, nullptr},
        kFeature);
    ok = register_native_effect({"remove_attention_area", "Removes the attention area with this id, or every area with `all`.",
                                 Form::Value, {{"id", ParamType::Token, true}}, ScopeMask::Any, &ExecRemove, nullptr},
                                kFeature) &&
         ok;
    ok = register_native_effect({"focus_attention_area",
                                 "Moves the camera of the player whose country is in scope so the area is in view: centred, "
                                 "zoomed to fit (zoom = <camera height> to choose the height; larger is farther out).",
                                 Form::Block,
                                 {{"id", ParamType::Token, true}, {"zoom", ParamType::Number, false}},
                                 ScopeMask::Country,
                                 &ExecFocus,
                                 nullptr},
                                kFeature) &&
         ok;
    ok = register_native_trigger({"has_attention_area", "True while an attention area with this id is marked.", Form::Value,
                                  {{"id", ParamType::Token, true}}, ScopeMask::Any, &EvalHas, nullptr},
                                 kFeature) &&
         ok;
    if (!ok) return false;
    if (!Engine::HookStripes()) Log("attention_area: the stripe paint hook is not there: fills are not drawn");
    if (!Engine::HaveHighlight()) Log("attention_area: the game's outline functions are not there: lines are not drawn");
    OnMainThreadFrame(&Frame);
    OnGameEntering(&OnEntering);
    // Optional: the hovered railway queue entry is marked (railway_queue's tooltip hook).
    bool hover = Engine::HaveHighlight() && FeatureActive("railway_queue") && Engine::SetRailwayHoverHandler(&OnRailwayHover);
    // The outline colours: the loader's slots (the areas' line colours and the hover mark's white),
    // appended to the renderer's outline colour table when it is built.
    if (Engine::HaveHighlight() && !Engine::HookOutlineColourTable())
        Log("attention_area: the outline colour hook is not there; lines use the game's palette colours");
    bool rightClick = hover && Engine::HookCentreOnProvince(&OnCentre);
    if (hover && !Engine::HookHoveredProvince(&IsHoveredLineEnd))
        Log("attention_area: the hovered-item hook is not there; the game's start-province mark may cover the outline");
    Log("attention_area: railway queue right-click fits the whole line: %s", rightClick ? "on" : "off");
    Log("attention_area: railway queue hover mark %s", hover ? "on" : "off (needs railway_queue's tooltip hook and the outline functions)");
    return true;
}
