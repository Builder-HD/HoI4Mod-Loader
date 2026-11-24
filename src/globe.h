// globe (NOTES.md Part S), the pure part: the mod file's settings, the bend from the flat map onto the
// sphere (the same maths as the HLSL in globe.cpp) and its inverse for picking, and the text patches
// of the game's shader files. No engine access, so it can be unit tested.
#pragma once

#include <string>
#include <vector>

namespace Globe {

// common/hoi4_loader_globe/*.txt:
//   globe = {
//       default = yes          # a game starts as a globe (no: flat until set_globe_view = yes)
//       flat_height = 400      # never a globe below this camera height
//       globe_height = 2500    # always a globe above it; in between, as soon as the flat view would
//                              # reach past the map's north or south edge (where the frame and logo are)
//       transition = 0.35      # seconds the flat map takes to fold into the sphere (0: at once)
//       north = 90             # latitude of the map's north edge (z = map height)
//       south = -90            # latitude of the south edge (z = 0)
//       background = "gfx/my_mod/space.png"   # optional: drawn behind the planet instead of the sky
//       polar_cap = 70         # optional: caps from this latitude (degrees) to both poles, hiding the
//                              # stretched map there (borders, names and units in them too)
//       polar_cap_color = { 0.72 0.77 0.82 }   # the caps' colour (red green blue, 0..1)
//   }
// Several files and blocks: later keys replace earlier ones.
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
    float polarCap = 0.0f;   // degrees; 0 = no caps
    float polarColor[3] = {0.72f, 0.77f, 0.82f};
};
struct Problem {
    std::string file;
    int line;
    std::string message;
};
// Reads one file into *settings (only the keys it writes change). Problems are logged by the caller.
void ParseFile(const std::string& text, const std::string& shown, Settings* settings, std::vector<Problem>* problems);

// When the map is a sphere: always from globe_height, never below flat_height, in between when the
// flat view reaches past the north or south edge. Once a sphere, it stays one until the camera is
// below 85% of the height it became one at (and the edge is out of view), so it does not flicker.
struct SphereState {
    bool sphere = false;
    float enterHeight = 0.0f;
};
bool WantSphere(const Settings& s, float height, bool edgeInView, SphereState* state);

// The shader's constants (five float4, register b13): A = (t, radius, map width, 1 / width),
// B = (look-at x, look-at z, cos(lat of look-at), sin(...)), C = (z of the equator, radians per z
// unit, map height, background on), D = (z where the north cap starts, z where the south cap starts,
// the edge's width in z, caps on), E = (cap colour, 0). t = 0 leaves every position as it is; 1 is the whole planet
// (in between only while it folds). Positions beyond the map's edges (frame, logo, filler water)
// sink into the planet.
struct Params {
    float a[4] = {0, 0, 0, 0};
    float b[4] = {0, 0, 1, 0};
    float c[4] = {0, 0, 0, 0};
    float d[4] = {0, 0, 0, 0};
    float e[4] = {0, 0, 0, 0};
    float t() const { return a[0]; }
    // The flat point (x, z) lies under a polar cap.
    bool InCap(float z) const { return d[3] > 0.5f && (z > d[0] || z < d[1]); }
};
// t: 0..1; (lx, lz): the ground point the camera looks at. False (and t = 0) for t <= 0 or bad map
// sizes.
bool MakeParams(const Settings& s, float mapWidth, float mapHeight, float t, float lx, float lz, Params* out);

// A flat world position bent onto the sphere (as the shader does in the main camera's pass).
void Bend(const Params& p, const float in[3], float out[3]);
// The flat map point (x, z) a ray hits on the sphere (heights ignored). A ray that misses gets the
// sphere's point closest to it (*hit = false). False if t = 0 or the ray is degenerate.
bool Unbend(const Params& p, const float origin[3], const float dir[3], float mapWidth, float mapHeight, float flat[2],
            bool* hit);

// Shader text patches; each returns the patched text, or "" to leave the file as it is. The
// constants and helpers (`HL_Globe...`, DX11 only, guarded) go in front of each patched code block.
//
// A map shader: in every vertex shader MainCode block (not the shadow ones) each
// `mul( ViewProjectionMatrix, X )` becomes `mul( ViewProjectionMatrix, HL_Globe( X ) )` (mode
// kCull: HL_GlobeCull, which also drops what lies beyond the planet's horizon, for things drawn
// without a depth test such as names; kHide: HL_GlobeHide, nothing drawn while bent, for the game's
// water, whose coarse grid would sink under the curved seabed) and each depth bias `dot( P, float4(
// GetMatrixData( ViewProjectionMatrix, 2, 0 ) ...` bends P too. In pixel shader blocks, `X = tex2D(
// WaterRefraction, ... ).rgb;` is followed by `X = HL_GlobeWater( X );`, and the terrain's colour
// before `gradient_border_apply( diffuse.rgb` gets the sea's colour on the seabed (HL_GlobeSea).
// *count: replacements made.
// kTerrain: the ground (HL_Globe); kBend: what lies on it (HL_GlobeSurface: also hidden under the
// polar caps); kCull: also dropped beyond the horizon; kHide: not drawn while bent.
enum : int { kBend = 0, kCull = 1, kHide = 2, kTerrain = 3 };
std::string PatchMapShader(const std::string& shader, int mode, int* count);
// standardfuncsgfx.fxh: the distance fog measures across the wrap line the short way round.
std::string PatchSharedCode(const std::string& fxh);
// sky.shader: the background image behind the planet (when a mod names one).
std::string PatchSkyShader(const std::string& sky);
// The HLSL put in front of each patched block, and the vertex replacements alone on plain HLSL (for
// the offline compile test against the game's cached shader sources).
const char* HlslCode();
std::string PatchVertexCode(const std::string& code, int mode, int* count);
std::string PatchPixelCode(const std::string& code, int* count);

}  // namespace Globe
