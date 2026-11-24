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
//       flat_height = 800      # camera height up to which the map stays flat
//       globe_height = 2500    # camera height from which it is a whole planet (curves in between)
//       north = 90             # latitude of the map's north edge (z = map height)
//       south = -90            # latitude of the south edge (z = 0)
//   }
// Several files and blocks: later keys replace earlier ones.
struct Settings {
    bool present = false;  // a file had a globe block
    bool defaultOn = false;
    float flatHeight = 800.0f;
    float globeHeight = 2500.0f;
    float north = 90.0f;
    float south = -90.0f;
};
struct Problem {
    std::string file;
    int line;
    std::string message;
};
// Reads one file into *settings (only the keys it writes change). Problems are logged by the caller.
void ParseFile(const std::string& text, const std::string& shown, Settings* settings, std::vector<Problem>* problems);

// 0 (flat) .. 1 (globe) for a camera height: smoothstep between flat_height and globe_height.
float Curvature(const Settings& s, float height);

// The shader's constants (three float4, register b13): A = (t, radius, map width, 1 / width),
// B = (look-at x, look-at z, cos(lat of look-at), sin(...)), C = (z of the equator, radians per z
// unit, map height, 0). t = 0 leaves every position as it is. Positions beyond the north and south
// edges (the map frame and logo) sink into the planet.
struct Params {
    float a[4] = {0, 0, 0, 0};
    float b[4] = {0, 0, 1, 0};
    float c[4] = {0, 0, 0, 0};
    float t() const { return a[0]; }
};
// t: Curvature(); (lx, lz): the ground point the camera looks at. False (and t = 0) for t <= 0 or
// bad map sizes.
bool MakeParams(const Settings& s, float mapWidth, float mapHeight, float t, float lx, float lz, Params* out);

// A flat world position bent onto the sphere (as the shader does).
void Bend(const Params& p, const float in[3], float out[3]);
// The flat map point (x, z) a ray hits on the sphere (heights ignored). A ray that misses gets the
// sphere's point closest to it (*hit = false). False if t = 0 or the ray is degenerate.
bool Unbend(const Params& p, const float origin[3], const float dir[3], float mapWidth, float mapHeight, float flat[2],
            bool* hit);

// Shader text patch; returns the patched text, or "" to leave the file as it is.
// A map shader: in every vertex shader MainCode block (not the shadow ones) each
// `mul( ViewProjectionMatrix, X )` becomes `mul( ViewProjectionMatrix, HL_Globe( X ) )` and each
// depth bias `dot( P, float4( GetMatrixData( ViewProjectionMatrix, 2, 0 ) ...` bends P too.
// *count: replacements made. The constants and HL_Globe() go in front of each patched block's code
// (guarded, DX11 only; other renderers get the identity).
std::string PatchMapShader(const std::string& shader, int* count);
// The HLSL put in front of each patched block, and the replacements alone on plain HLSL (for the
// offline compile test against the game's cached shader sources).
const char* HlslCode();
std::string PatchVertexCode(const std::string& code, int* count);

}  // namespace Globe
