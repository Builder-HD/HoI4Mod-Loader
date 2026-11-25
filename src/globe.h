// globe (NOTES.md Parts S and T), the pure part: the mod file's settings, the planet table the shaders
// read (the bend from the flat map onto each planet's sphere, the overview of all planets), the same
// maths for picking, and the text patches of the game's shader files. No engine access, so it can be
// unit tested.
#pragma once

#include <string>
#include <vector>

namespace Globe {

const int kMaxPlanets = 16;

// One planet: a rectangle of the flat map (NOTES.md Part T). Keys not given take the globe block's.
//   planet = {
//       name = PLANET_EARTH_A      # localisation key: its name in the overview (and in messages)
//       x = 0  y = 0  width = 5632  height = 2048   # map pixels, y from the map's bottom (as z);
//                                                   # no rectangle: the whole map
//       north = 82  south = -60    # its latitudes; polar_cap_north / polar_cap_south as below
//       overview = { x = -0.7 y = 0 size = 1 }      # its place and diameter in the overview
//       image = "gfx/my_mod/planet_a.png"           # shown instead of its map in the overview
//   }
struct PlanetSettings {
    std::string name;
    float x = 0, y = 0, width = 0, height = 0;  // width / height 0: the whole map
    bool setNorth = false, setSouth = false, setCapNorth = false, setCapSouth = false;
    float north = 90.0f, south = -90.0f, polarCapNorth = 90.0f, polarCapSouth = -90.0f;
    bool hasOverview = false;
    float overviewX = 0, overviewY = 0, overviewSize = 1;
    std::string image, imageFrom;
};

// common/hoi4_loader_globe/*.txt:
//   globe = {
//       default = yes          # a game starts as a globe (no: flat until set_globe_view = yes)
//       flat_height = 400      # never a globe below this camera height
//       globe_height = 2500    # always a globe above it; in between, as soon as the flat view would
//                              # reach past the planet's edges
//       transition = 0.35      # seconds the flat map takes to fold into the sphere (0: at once)
//       north = 90             # latitude of the map's north edge (z = map height)
//       south = -90            # latitude of the south edge (z = 0)
//       background = "gfx/my_mod/space.png"   # optional: drawn behind the planet instead of the sky
//       # When north / south stop short of the poles, the planet beyond the map's edges is polar sea,
//       # with ice from these latitudes to the poles (never over the map):
//       polar_cap = 84         # optional: ice from 84 degrees north and south (or polar_cap_north = 84
//                              # and polar_cap_south = -70 separately); without it only sea
//       polar_cap_color = { 0.72 0.77 0.82 }   # the ice's colour (red green blue, 0..1)
//       polar_sea_color = { 0.14 0.19 0.27 }   # the polar seas' colour
//       overview_height = 2600       # optional: from this height all planets are shown (0: never)
//       overview_full_height = 3000  # the whole overview fits the view here (a mod may raise
//                                    # NFrontend.CAMERA_MAX_HEIGHT; camera_range follows it)
//       name_font = "gfx/fonts/hoi_mapfont4"   # the planets' names (the map names' font)
//       planet = { ... }       # several planets (see PlanetSettings); none: the whole map is one
//   }
// Several files and blocks: later keys replace earlier ones; planets add up (a later planet with
// the same name replaces the earlier one).
struct Settings {
    bool present = false;  // a file had a globe block
    bool defaultOn = false;
    float flatHeight = 400.0f;
    float globeHeight = 2500.0f;
    float transition = 0.35f;
    float north = 90.0f;
    float south = -90.0f;
    std::string background;  // path inside the game or a mod; "" = the game's sky
    std::string backgroundFrom;  // the file that named it (for messages)
    float polarCapNorth = 90.0f;  // degrees: the ice from here to the north pole (90: none)
    float polarCapSouth = -90.0f;  // and from here to the south pole (-90: none)
    float polarColor[3] = {0.72f, 0.77f, 0.82f};
    float polarSeaColor[3] = {0.14f, 0.19f, 0.27f};
    float overviewHeight = 0.0f;        // 0: no overview
    float overviewFullHeight = 3000.0f;
    std::string nameFont = "gfx/fonts/hoi_mapfont4";  // the planets' names: a game bitmap font (no extension)
    std::vector<PlanetSettings> planets;
};
struct Problem {
    std::string file;
    int line;
    std::string message;
};
// Reads one file into *settings (only the keys it writes change). Problems are logged by the caller.
void ParseFile(const std::string& text, const std::string& shown, Settings* settings, std::vector<Problem>* problems);

// When the map is a sphere: always from globe_height, never below flat_height, in between when the
// flat view reaches past the planet's edges. Once a sphere, it stays one until the camera is below
// 85% of the height it became one at (and the edge is out of view), so it does not flicker.
struct SphereState {
    bool sphere = false;
    float enterHeight = 0.0f;
};
bool WantSphere(const Settings& s, float height, bool edgeInView, SphereState* state);

// A planet resolved against the loaded map (the rectangle filled in, every key settled).
struct Planet {
    std::string name;
    int source = 0;  // its index in Settings::planets (0 for the implicit whole map)
    float x0 = 0, z0 = 0, w = 0, h = 0;
    float north = 90, south = -90, capNorth = 90, capSouth = -90;
    float ox = 0, oy = 0, size = 1;  // overview place and diameter
    bool image = false;
    float imageCell[4] = {0, 0, 0, 0};  // u0, v0, du, dv in the images texture
    float nameRow[2] = {0, 0};          // v0, dv of its row in the names texture (dv 0: no name)
    float nameAspect = 0;               // the text's width / its row's height (pixels)
    float nameFrac = 0;                 // the text's width / the texture's width
};
// The planets of the settings on a map of w x h (no planet blocks: one, the whole map). Planets
// outside the map are cut to it; empty ones dropped (with a problem).
std::vector<Planet> ResolvePlanets(const Settings& s, float mapW, float mapH, std::vector<std::string>* problems);
// Index of the planet whose rectangle holds flat (x, z), else -1.
int PlanetAt(const std::vector<Planet>& planets, float x, float z);

// What the frame shows (main thread, from the camera).
struct View {
    int current = 0;         // the planet the camera is on
    float t = 0;             // its fold: 0 flat .. 1 sphere
    float p = 0;             // overview: 0 none .. 1 the whole layout
    float lookX = 0, lookZ = 0;      // the camera's look-at on the ground
    float focusX = 0, focusY = 0;    // the point of the overview layout under the camera (p > 0)
    float fullHeight = 3000;         // overview_full_height
    float aspect = 16.0f / 9.0f;     // of the view (for fitting the layout)
    std::vector<float> lastLook;     // per planet: x, z where it was last looked at
    bool background = false;
};

// The shader's constants (register b13): four globals and 8 float4 per planet (NOTES.md T9).
//   A = (fold t of the current planet, overview p, map width, 1 / map width)
//   B = (planet count, current planet, name alpha, background on)
//   E = (ice colour, 0), F = (polar sea colour, 0)
//   per planet: 0 rectangle (x0, z0, w, h); 1 centre xyz, radius; 2 look-at x, z, cos, sin of its
//   latitude; 3 equator z, radians per z, radians per x, kind (0 hidden, 1 its map, 2 its image);
//   4 ice north z, ice south z, ice edge width, height scale; 5 name row v0, dv, length, height (in
//   radii); 6 image cell u0, v0, du, dv; 7 name width fraction, polar seas on, 0, 0.
struct Params {
    float a[4] = {0, 0, 0, 0};
    float b[4] = {0, 0, 0, 0};
    float e[4] = {0, 0, 0, 0};
    float f[4] = {0, 0, 0, 0};
    float pl[kMaxPlanets * 8][4] = {};
    float t() const { return a[0]; }
    float overview() const { return a[1]; }
    int count() const { return (int)b[0]; }
    int current() const { return (int)b[1]; }
    const float* rect(int i) const { return pl[i * 8]; }
    const float* centre(int i) const { return pl[i * 8 + 1]; }
    const float* look(int i) const { return pl[i * 8 + 2]; }
    const float* bend(int i) const { return pl[i * 8 + 3]; }
    float kind(int i) const { return pl[i * 8 + 3][3]; }
};
// False (and an empty table, t = 0) when there is nothing to bend (t <= 0 and p <= 0) or bad input.
bool MakeParams(const Settings& s, const std::vector<Planet>& planets, float mapW, const View& v, Params* out);
// The overview's scale (world units per layout unit) at progress p, and the layout's centre.
float OverviewScale(const std::vector<Planet>& planets, const View& v, float* cx, float* cy);

// A flat world position bent onto planet i's sphere (as the shader does in the main camera's pass).
void Bend(const Params& p, int planet, const float in[3], float out[3]);
// The nearest planet a ray hits (kind 1 its sphere, kind 2 its image disc): its index (or -1) and
// the distance along the ray.
int RayPlanet(const Params& p, const float origin[3], const float dir[3], float* dist);
// The flat map point (x, z) a ray hits on planet i's sphere (heights ignored). *hit = false for a
// miss (the sphere's point closest to the ray instead) and for the polar seas beyond the planet's
// map. False if the planet is not drawn as a sphere or the ray is degenerate.
bool Unbend(const Params& p, int planet, const float origin[3], const float dir[3], float flat[2], bool* hit);

// Shader text patches; each returns the patched text, or "" to leave the file as it is. The
// constants and helpers (`HL_Globe...`, DX11 only, guarded) go in front of each patched code block.
//
// A map shader: in every vertex shader MainCode block (not the shadow ones) each
// `mul( ViewProjectionMatrix, X )` becomes `mul( ViewProjectionMatrix, HL_Globe( X ) )` (mode
// kCull: HL_GlobeCull, which also drops what lies beyond the planet's horizon, for things drawn
// without a depth test such as names; kHide: HL_GlobeHide, nothing drawn while bent, for the game's
// water, whose coarse grid would sink under the curved seabed) and each depth bias `dot( P, float4(
// GetMatrixData( ViewProjectionMatrix, 2, 0 ) ...` bends P too. In pixel shader blocks, `X = tex2D(
// WaterRefraction, ... ).rgb;` is followed by `X = HL_GlobeWater( X );`, the terrain's colour before
// `gradient_border_apply( diffuse.rgb` gets the sea's colour on the seabed (HL_GlobeSea), and its
// `return float4( vOut, ... );` the planet names over it (HL_GlobeNameOut).
// sky.shader (PatchSkyShader): the background, the polar seas and ice, the planets' images and names
// (HL_GlobeSky).
// *count: replacements made.
// kTerrain: the ground (HL_Globe); kBend: what lies on it (HL_GlobeSurface, the same); kCull: also
// dropped beyond the horizon; kHide: not drawn while bent; kLabel: as kCull, and not drawn in the
// overview (the game's map names). What lies on no planet is dropped.
enum : int { kBend = 0, kCull = 1, kHide = 2, kTerrain = 3, kLabel = 4 };
std::string PatchMapShader(const std::string& shader, int mode, int* count);
// standardfuncsgfx.fxh: the distance fog measures across the wrap line the short way round.
std::string PatchSharedCode(const std::string& fxh);
// sky.shader: see above.
std::string PatchSkyShader(const std::string& sky);
// The HLSL put in front of each patched block, and the vertex replacements alone on plain HLSL (for
// the offline compile test against the game's cached shader sources).
const char* HlslCode();
std::string PatchVertexCode(const std::string& code, int mode, int* count);
std::string PatchPixelCode(const std::string& code, int* count);

}  // namespace Globe
