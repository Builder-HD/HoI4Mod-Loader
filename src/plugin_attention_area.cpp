// attention_area feature (NOTES.md Part M): mark provinces on the map with an outline in a chosen
// colour, width and effect, and move the camera to them.
//
//   add_attention_area = { id = <name> style = outline|band|stripes color = <name> red/green/blue/alpha = <0..1>
//                          width = <number> pulse = yes/no glow = yes/no area = { ... } area = { ... } }
//
// style = outline (the default): the game's thin province group outline (as construction mode marks
// build targets), in every map mode. style = band: the gradient border band (width, pulse and glow
// apply to it), drawn in the political map's border layer; special map modes (supply) draw it badly.
// style = stripes: the diagonal stripes of occupation and battle-plan orders (the map's secondary
// colour texture, NOTES.md M9), in every map mode; alpha is the stripe strength.
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
// next frame (Engine::SetAttentionDraw).
#include <math.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "native_script.h"

namespace {

const char kFeature[] = "attention_area";
const float kFitFactor = 1.6f;  // camera height per pixel of the box's larger side (see Frame)

struct Area {
    int token = 0;          // id (engine token)
    std::string name;
    int style = 0;          // 0 outline (group outline), 1 band (gradient borders), 2 stripes
    Engine::AttentionLook look;
    std::vector<int> provinces;
    int box[4] = {};        // bounding box in map pixels {x, y, w, h}
    int ref = 0, ref2 = 0;  // camera calibration provinces
    float refDist = 0;      // map pixels from the box centre to the nearest province centre
};

std::vector<Area> g_areas;  // written by effects (main thread), read by the trigger
std::vector<Engine::OutlineRef> g_outlines;  // the outlines shown for style = outline areas
const int kAreaPriority = 90;                // below the railway hover mark (100)
volatile long g_noColourLogged = 0, g_noOutlineLogged = 0, g_noStripesLogged = 0;
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
        // The game's thin province group outline (as construction mode draws build targets), in the
        // palette's white (the loader's defines patch), else the palette's first colour.
        Engine::ClearHoveredProvinceIf(&IsHoveredLineEnd);  // set before the first tooltip of this hover
        int white = Engine::HighlightWhiteIndex();
        if (!Engine::ShowHighlight(g_hoverPath, white >= 0 ? white : 0) && g_hoverLogs < 5)
            Log("attention_area: the railway hover outline could not be created");
        if (white < 0 && InterlockedExchange(&g_whiteMissingLogged, 1) == 0)
            Log("attention_area: no white outline colour (the renderer's colour table was not changed): hover "
                "outlines use colour 0");
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
enum AddParam { kId, kStyle, kColor, kRed, kGreen, kBlue, kAlpha, kWidth, kPulse, kGlow, kArea };
enum AreaKey { kProvince, kState, kRegion, kOwner, kController, kCoastal };

ParamSpec Repeat(const char* name) {
    ParamSpec p{name, ParamType::Int, false};
    p.repeat = true;
    return p;
}

const std::vector<ParamSpec> kAreaKeys = {
    Repeat("province"),
    Repeat("state"),
    Repeat("strategic_region"),
    {"owner", ParamType::Country, false},
    {"controller", ParamType::Country, false},
    {"coastal", ParamType::Bool, false},
};

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
        for (size_t k = 0; k < a.count(kState); ++k) AddIds(Engine::AreaKind::State, a.at(kState, k), &in);
        for (size_t k = 0; k < a.count(kRegion); ++k) AddIds(Engine::AreaKind::StrategicRegion, a.at(kRegion, k), &in);
    }
    int owner = a.has(kOwner) ? native_country(a.i(kOwner), scope) : 0;
    int controller = a.has(kController) ? native_country(a.i(kController), scope) : 0;
    if ((a.has(kOwner) && owner <= 0) || (a.has(kController) && controller <= 0)) return;  // names no country here
    for (size_t p = 1; p < n; ++p) {
        if (!in[p]) continue;
        if (owner && !Engine::IsSameCountry(Engine::StateOwner(stateOf[p]), owner)) continue;
        if (controller && !Engine::IsSameCountry(Engine::ProvinceController((int)p), controller)) continue;
        if (a.has(kCoastal) && Engine::ProvinceIsCoastal((int)p) != a.b(kCoastal)) continue;
        (*out)[p] = 1;
    }
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
void ExecAdd(const Args& a, const Scope& s) {
    Area area;
    area.token = a.i(kId);
    area.name = a.key(kId);
    area.style = a.has(kStyle) ? a.i(kStyle) : 0;
    int c = a.has(kColor) ? a.i(kColor) : 0;
    area.look.r = kRgb[c][0], area.look.g = kRgb[c][1], area.look.b = kRgb[c][2];
    if (a.has(kRed)) area.look.r = Clamp01(a.num(kRed));
    if (a.has(kGreen)) area.look.g = Clamp01(a.num(kGreen));
    if (a.has(kBlue)) area.look.b = Clamp01(a.num(kBlue));
    area.look.a = a.has(kAlpha) ? Clamp01(a.num(kAlpha)) : 1.0f;
    area.look.width = a.has(kWidth) ? (float)std::max(0.0, std::min(a.num(kWidth), 100.0)) : 1.0f;
    area.look.pulse = a.has(kPulse) && a.b(kPulse);
    area.look.glow = !a.has(kGlow) || a.b(kGlow);
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
}

void ExecRemove(const Args& a, const Scope&) {
    if (strcmp(a.key(0), "all") == 0) {
        g_dirty = !g_areas.empty();
        g_areas.clear();
        return;
    }
    for (size_t i = 0; i < g_areas.size(); ++i)
        if (g_areas[i].token == a.i(0)) {
            g_areas.erase(g_areas.begin() + (long long)i);
            g_dirty = true;
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
    for (const Area& area : g_areas)
        if (area.token == a.i(0)) return true;
    return false;
}

// --- Frame, game entry ------------------------------------------------------------------------------
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
    std::vector<Engine::AttentionDraw> draw;
    std::vector<Engine::StripeArea> stripes;
    for (size_t i = 0; i < g_areas.size(); ++i) {
        const Area& a = g_areas[i];
        if (a.provinces.empty()) continue;
        if (a.style == 2 && Engine::HaveStripes()) {
            auto byte = [](float v) { return (uint32_t)(v * 255.0f + 0.5f); };
            Engine::StripeArea s;
            // The texture is BGRA: byte 0 blue, byte 2 red (first in-game test: red came out blue).
            s.rgba = byte(a.look.b) | byte(a.look.g) << 8 | byte(a.look.r) << 16 | byte(a.look.a) << 24;
            s.provinces = a.provinces;
            stripes.push_back(std::move(s));
            continue;
        }
        if (a.style == 2 && InterlockedExchange(&g_noStripesLogged, 1) == 0)
            Log("attention_area: the stripe hook is not there: style = stripes areas are drawn as outlines");
        bool band = a.style == 1;
        if (band || !Engine::HaveHighlight()) {
            if (!band && InterlockedExchange(&g_noOutlineLogged, 1) == 0)
                Log("attention_area: the game's outline functions are not there: style = outline areas are drawn as bands");
            draw.push_back({a.look, a.provinces});
            continue;
        }
        int colour = Engine::OutlineColourId(a.look.r, a.look.g, a.look.b, a.look.a);
        if (colour < 0) {
            colour = Engine::OutlineWhiteId();
            if (colour < 0) colour = 10;  // palette colour 0
            if (InterlockedExchange(&g_noColourLogged, 1) == 0)
                Log("attention_area: no free outline colour (the renderer's colour table was not extended, or more than 32 "
                    "colours are in use): such outlines use colour %d (logged once)", colour);
        }
        Engine::OutlineRef ref;
        int priority = kAreaPriority - (int)std::min<size_t>(i, 80);
        if (Engine::CreateOutline(a.provinces, colour, priority, &ref)) g_outlines.push_back(ref);
        else Log("attention_area: the outline of '%s' could not be created", a.name.c_str());
    }
    Engine::SetAttentionDraw(draw);
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
    Engine::AttentionTick();
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
    g_dirty = false;
    g_focusPending = false;
    g_hoverPath.clear();
    g_hoverShown = g_hoverChanged = false;
    Engine::HideHighlight();
    ReleaseOutlines();
    Engine::ResetOutlineColours();
    Engine::ForgetAttentionDraw();
    Engine::ForgetStripes();
}

void OnFault(const char* where) { FeatureDeactivate(kFeature, where); }

}  // namespace

bool AttentionAreaInit() {
    if (!Engine::HaveAttentionBorders() || !Engine::HaveAttentionCamera() || !Engine::HaveProvinceData() ||
        !Engine::HaveRegionData() || !Engine::HaveCountries()) {
        Log("attention_area: engine pieces missing (borders %d, camera %d, provinces %d, regions %d)", Engine::HaveAttentionBorders(),
            Engine::HaveAttentionCamera(), Engine::HaveProvinceData(), Engine::HaveRegionData());
        return false;
    }
    if (!MainThreadQueueAvailable()) {
        Log("attention_area: needs the main-thread frame hook; off");
        return false;
    }    std::vector<ParamSpec> add = {
        {"id", ParamType::Token, true},
        {"style", ParamType::Choice, false, ScopeDefault::None, "outline|band|stripes"},
        {"color", ParamType::Choice, false, ScopeDefault::None, kColors},
        {"red", ParamType::Number, false},
        {"green", ParamType::Number, false},
        {"blue", ParamType::Number, false},
        {"alpha", ParamType::Number, false},
        {"width", ParamType::Number, false},
        {"pulse", ParamType::Bool, false},
        {"glow", ParamType::Bool, false},
        AreaParam(),
    };
    bool ok = register_native_effect(
        {"add_attention_area",
         "Marks provinces on the map. id names the area (adding it again replaces it). style = outline (default): the "
         "game's thin province outline, in every map mode. style = band: a gradient border band in the political map "
         "(width: its thickness, pulse: it pulses, glow = no: a sharp edge). style = stripes: diagonal stripes like "
         "occupation, in every map mode (alpha: stripe strength). color: a named colour, or red/green/blue/alpha "
         "from 0 to 1. Each area = { } block "
         "selects provinces: province, state and strategic_region ids (any number, added up; none: every land province), "
         "then owner, controller and coastal keep those that match. Several area blocks add up. Not saved: add areas again "
         "after loading (on_game_loaded).",
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
    if (!Engine::HookAttention(&OnFault)) {
        Log("attention_area: the gradient border hooks could not be queued; off");
        return false;
    }
    if (!Engine::HookStripes()) Log("attention_area: style = stripes is drawn as an outline (no stripe hook)");
    OnMainThreadFrame(&Frame);
    OnGameEntering(&OnEntering);
    // Optional: the hovered railway queue entry is marked (railway_queue's tooltip hook).
    bool hover = Engine::HaveHighlight() && FeatureActive("railway_queue") && Engine::SetRailwayHoverHandler(&OnRailwayHover);
    // The outline colours: white (colour 14) and the extra colours of style = outline areas, written into
    // the renderer's outline colour table when it is built.
    if (Engine::HaveHighlight() && !Engine::HookHighlightWhite())
        Log("attention_area: the outline colour hook is not there; outlines use the game's palette colours");
    bool rightClick = hover && Engine::HookCentreOnProvince(&OnCentre);
    if (hover && !Engine::HookHoveredProvince(&IsHoveredLineEnd))
        Log("attention_area: the hovered-item hook is not there; the game's start-province mark may cover the outline");
    Log("attention_area: railway queue right-click fits the whole line: %s", rightClick ? "on" : "off");
    Log("attention_area: railway queue hover mark %s", hover ? "on" : "off (needs railway_queue's tooltip hook and the outline functions)");
    return true;
}
