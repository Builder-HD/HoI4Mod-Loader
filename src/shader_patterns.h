// Shader patterns (NOTES.md M11/M12): every look of an attention area is a shader. An area has a
// line shader (its outline) and a fill shader (its provinces), chosen by name. The loader ships
// built-ins (shader_builtins.cpp); mods add their own, or replace a built-in, as plain HLSL files:
//
//   <mod>/gfx/FX/hoi4_loader_patterns/line/<name>.hlsl   defines
//       float4 HL_Line_<name>( float4 vColor, float3 vAreaColor, float2 vUV, float3 vPos )
//   <mod>/gfx/FX/hoi4_loader_patterns/fill/<name>.hlsl   defines
//       float3 HL_Fill_<name>( float3 vColor, float2 vUV, float4 vMask, in sampler2D Mask )
//
// Line: vColor is the game's own border colour there (alpha: its fade with zoom), vAreaColor the
// area's colour (its alpha: the static global HL_LineAlpha), vUV.x across the border strip (0..1), vUV.y along it (border texture units, see
// BORDER_TILE), vPos the world position. Returns colour and alpha. The strip is the game's border
// geometry: a line shader colours inside it, it cannot make it wider (wide effects are fills).
// Fill: vColor is the map colour there, vUV the map position (0..1 over the whole map, v growing
// southwards), vMask the area's colour (rgb) and alpha (a), Mask the stripe texture, for HL_EdgeDistance( Mask, vUV, fMaxPixels ): map pixels to the area's edge (up to the
// maximum). Returns the new map colour.
// Both may use the game's shader globals and helpers (vGlobalTime, cam_distance, ...). border.shader
// includes standardfuncsgfx.fxh, so its pixel shader holds the fill code as well as the line code:
// helper names must be unique across both kinds.
//
// Ids are 1-based, in name order of the built-ins and all enabled mods' files (a name in several
// places: the later mod in the load order wins over earlier ones and over the built-in), so every
// machine with the same mods agrees.
#pragma once

#include <string>
#include <vector>

namespace ShaderPatterns {

enum class Kind { Fill, Line };

// A shader's variables (shader_variables in script): declared in its file by a line
//   // hoi4_loader_variables: <name> <min> <max> [<default>], ...
// up to kMaxVars, names [a-z][a-z0-9_]*. In the shader each is a float of that name, set before the
// shader runs. A shader declaring none has the implicit red, green, blue, alpha (the area's colour).
// Variables named red/green/blue/alpha default to the area's colour. Channels per area: fills 4 x 6
// bits (64 steps across min..max); lines the first 3 at full precision (any value), the 4th 0..1
// scaled (the slot alpha shares it with the shader id).
const int kMaxVars = 4;
struct Var {
    std::string name;
    float min = 0, max = 1, def = 0;
};

struct Pattern {
    std::string name;    // [A-Za-z0-9_]
    std::string code;    // the HLSL
    std::string source;  // where it came from (for the log)
    std::vector<Var> vars;  // declared variables (empty: the implicit colour)
};

// The // hoi4_loader_variables: line of a shader's code: false (with the reason) if it is malformed.
// No such line: true, no variables.
bool ParseVariables(const std::string& code, std::vector<Var>* out, std::string* error);

// Encodings (pure, shared by the loader and the shader dispatchers):
// fill id n -> stripe texture alpha byte 255 - n (the vanilla shader shows near-full stripes); the
//   area's alpha (6 bits, 64 levels) in the low 2 bits of red, green and blue (rr gg bb), the colour
//   rounded to steps of 4 (invisible). The dispatcher hands the shader vMask.rgb = the colour,
//   vMask.a = the area's alpha.
// line id n, alpha a -> outline colour alpha 2 + n + 0.9 a (vanilla shows the plain colour); the
//   dispatcher sets the static global HL_LineAlpha = a before calling the line shader.
const int kMaxFill = 32;
const int kMaxLine = 32;
inline int FillAlpha(int id) { return 255 - id; }
inline float LineAlpha(int id, float alpha) { return 2.0f + (float)id + 0.9f * (alpha < 0 ? 0.0f : alpha > 1 ? 1.0f : alpha); }
// The stripe texture texel (bytes b, g, r, a in memory: BGRA) for a fill from four values 0..1
// (the shader's variables, normalised over their ranges; the implicit ones are r, g, b, a): 6 bits
// each, v0..v2 in the high 6 bits of red, green, blue, v3 split over their low 2 bits.
inline unsigned FillTexelN(int id, const float v[4]) {
    unsigned n[4];
    for (int i = 0; i < 4; ++i) n[i] = (unsigned)((v[i] < 0 ? 0.0f : v[i] > 1 ? 1.0f : v[i]) * 63.0f + 0.5f);
    unsigned R = n[0] << 2 | (n[3] >> 4 & 3u), G = n[1] << 2 | (n[3] >> 2 & 3u), B = n[2] << 2 | (n[3] & 3u);
    return B | G << 8 | R << 16 | (unsigned)FillAlpha(id) << 24;
}
inline unsigned FillTexel(int id, float r, float g, float b, float alpha) {
    float v[4] = {r, g, b, alpha};
    return FillTexelN(id, v);
}

// The loader's own patterns (shader_builtins.cpp). Default line: "solid".
const std::vector<Pattern>& Builtins(Kind kind);

// Pure text work (tested by tests/shader_patterns_test.cpp):
bool ValidName(const std::string& name);
// The enabled mods' .mod paths ("mod/x.mod") from dlc_load.json's text, in order.
std::vector<std::string> EnabledMods(const std::string& dlcLoadJson);
// The `path="..."` of a .mod descriptor's text, or "".
std::string ModPath(const std::string& descriptor);
// standardfuncsgfx.fxh with HL_EdgeDistance, the fill patterns and the dispatcher added (vanilla
// secondary_color_mask renamed and called for everything else), or "" if the file is not shaped as
// expected.
std::string PatchFillShader(const std::string& fxh, const std::vector<Pattern>& fills);
// border.shader with the line patterns and the dispatcher added (the vanilla pixel shader main
// renamed and called first), or "" if the file is not shaped as expected.
std::string PatchLineShader(const std::string& shader, const std::vector<Pattern>& lines);

// Reads dlc_load.json and the enabled mods' pattern folders once (later calls return the same).
void Collect();
const std::vector<Pattern>& Patterns(Kind kind);
// 1-based id of a pattern name (any case), 0 if there is none by that name.
int Id(Kind kind, const std::string& name);
// Whether the game's shader file for this kind was patched (set by the file patch when the game
// reads it; until then, and if the patch failed, the dispatch is not there).
void SetPatched(Kind kind, bool patched);
bool Patched(Kind kind);

}  // namespace ShaderPatterns
