// Shader patterns, the pure text part (NOTES.md M11/M12; file reading in shader_patterns_collect.cpp,
// the loader's own patterns in shader_builtins.cpp). The game's .shader/.fxh files are Paradox
// script: HLSL is only legal inside `[[ ... ]]` blocks, so everything the loader adds goes in front
// of the `]]` closing the pixel shader's code.
#include "shader_patterns.h"

#include <stdio.h>

#include <regex>

namespace ShaderPatterns {

namespace {

std::string Line(const std::string& s) { return s + "\n"; }

std::string Num(float v) {  // an HLSL float literal
    char b[48];
    snprintf(b, sizeof b, "%.7g", v);
    std::string s = b;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

// A shader's code with its declared variables: a static float per variable (unique per shader and
// kind) and a #define of the variable's own name around the code, so the shader writes speed.
std::string EmitPattern(const Pattern& p, const char* kind, const char* indent) {
    std::string s = Line(std::string(indent) + "// " + p.name + " (" + p.source + ")");
    for (const Var& v : p.vars)
        s += Line(std::string(indent) + "static float HL_V" + kind + "_" + p.name + "_" + v.name + " = " + Num(v.def) + ";");
    for (const Var& v : p.vars) s += Line("#define " + v.name + " HL_V" + kind + "_" + p.name + "_" + v.name);
    s += p.code + "\n";
    for (const Var& v : p.vars) s += Line("#undef " + v.name);
    return s;
}

// Sets a shader's variables from four sources (HLSL expressions): src[i] is normalised (0..1) where
// norm[i], else the value itself.
std::string AssignVars(const Pattern& p, const char* kind, const std::string src[4], const bool norm[4], const char* indent) {
    std::string s;
    for (size_t i = 0; i < p.vars.size() && i < (size_t)kMaxVars; ++i) {
        const Var& v = p.vars[i];
        std::string target = std::string("HL_V") + kind + "_" + p.name + "_" + v.name;
        s += Line(std::string(indent) + target + " = " +
                  (norm[i] ? Num(v.min) + " + " + src[i] + " * " + Num(v.max - v.min) : src[i]) + ";");
    }
    return s;
}

bool IsReservedVarName(const std::string& n) {
    static const char* const kReserved[] = {"vColor", "vUV", "vMask", "Mask", "vAreaColor", "vPos", "float", "float2",
                                            "float3", "float4", "int", "int2", "int3", "bool", "in", "out", "inout",
                                            "return", "if", "else", "for", "while", "static", "const", "true", "false",
                                            "main", "min", "max", "lerp", "sin", "cos", "frac", "abs", "step", "smoothstep",
                                            "saturate", "floor", "pow", "exp", "length", "dot", "distance", "sampler2D"};
    for (const char* r : kReserved)
        if (n == r) return true;
    return false;
}

// HL_EdgeDistance(Mask, vUV, fMaxPixels): map pixels from vUV to the nearest texel whose code
// (alpha) differs, i.e. to the area's edge, searched on 8 rays at 6 radii up to fMaxPixels. Exact
// texels (Load), as the mask's sampler filters; non-DX11 builds report no edge.
const char kEdgeHelper[] =
    "\tfloat HL_EdgeDistance( in sampler2D Mask, float2 vUV, float fMaxPixels )\n"
    "\t{\n"
    "\t#ifdef PDX_DIRECTX_11\n"
    "\t\tfloat2 vSize;\n"
    "\t\tMask._Texture.GetDimensions( vSize.x, vSize.y );\n"
    "\t\tint2 nCentre = int2( frac( vUV ) * vSize );\n"
    "\t\tfloat fCode = Mask._Texture.Load( int3( nCentre, 0 ) ).a;\n"
    "\t\tfloat2 vDirs[8] = { float2( 1, 0 ), float2( 0.7071f, 0.7071f ), float2( 0, 1 ), float2( -0.7071f, 0.7071f ),\n"
    "\t\t                    float2( -1, 0 ), float2( -0.7071f, -0.7071f ), float2( 0, -1 ), float2( 0.7071f, -0.7071f ) };\n"
    "\t\t[loop] for ( int r = 1; r <= 6; ++r )\n"
    "\t\t{\n"
    "\t\t\tfloat fRadius = fMaxPixels * (float)r / 6.0f;\n"
    "\t\t\t[unroll] for ( int k = 0; k < 8; ++k )\n"
    "\t\t\t{\n"
    "\t\t\t\tint2 nAt = nCentre + int2( round( vDirs[k] * fRadius ) );\n"
    "\t\t\t\tif ( abs( Mask._Texture.Load( int3( nAt, 0 ) ).a - fCode ) > 0.5f / 255.0f )\n"
    "\t\t\t\t\treturn fMaxPixels * (float)( r - 1 ) / 6.0f + fMaxPixels / 12.0f;\n"
    "\t\t\t}\n"
    "\t\t}\n"
    "\t#endif\n"
    "\t\treturn fMaxPixels;\n"
    "\t}\n";

}  // namespace

bool ParseVariables(const std::string& code, std::vector<Var>* out, std::string* error) {
    out->clear();
    const std::string kTag = "hoi4_loader_variables:";
    size_t at = code.find(kTag);
    if (at == std::string::npos) return true;
    size_t end = code.find('\n', at);
    std::string list = code.substr(at + kTag.size(), end == std::string::npos ? std::string::npos : end - at - kTag.size());
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        std::string item = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? list.size() + 1 : comma + 1;
        char name[65] = {};
        float mn = 0, mx = 0, df = 0;
        int got = sscanf_s(item.c_str(), " %64s %f %f %f", name, (unsigned)sizeof name, &mn, &mx, &df);
        if (got <= 0) continue;  // empty item (trailing comma, spaces)
        std::string n = name;
        bool okName = !n.empty() && n[0] >= 'a' && n[0] <= 'z';
        for (char c : n) okName = okName && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_');
        if (got < 3) return *error = "'" + item + "': write <name> <min> <max> [<default>]", false;
        if (!okName || IsReservedVarName(n)) return *error = "'" + n + "' cannot be a variable name (lowercase letters, digits, _; not a shader word)", false;
        if (!(mx > mn)) return *error = "'" + n + "': max must be greater than min", false;
        for (const Var& v : *out)
            if (v.name == n) return *error = "'" + n + "' is declared twice", false;
        if (out->size() >= (size_t)kMaxVars) return *error = "more than 4 variables", false;
        Var v;
        v.name = n, v.min = mn, v.max = mx, v.def = got >= 4 ? (df < mn ? mn : df > mx ? mx : df) : mn;
        out->push_back(v);
    }
    return true;
}

bool ValidName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

// --- The compile check's sources (NOTES.md M15) ------------------------------------------------------
namespace {

// The game's DX11 shader preamble, as far as the documented interface needs it (taken from the
// game's assembled shader sources in its shader cache), plus the loader's own globals.
const char kCheckPreamble[] =
    "#define PDX_DIRECTX_11\n"
    "#define PIXEL_SHADER\n"
    "#define MAP_SIZE_X 5632.000\n"
    "#define MAP_SIZE_Y 2048.000\n"
    "#define mod( X, Y ) ( X % Y )\n"
    "float2 vec2(float vValue) { return float2(vValue, vValue); }\n"
    "float3 vec3(float vValue) { return float3(vValue, vValue, vValue); }\n"
    "float4 vec4(float vValue) { return float4(vValue, vValue, vValue, vValue); }\n"
    "struct STextureSampler2D { Texture2D _Texture; SamplerState _Sampler; };\n"
    "STextureSampler2D CreateSampler2D( Texture2D Texture, SamplerState Sampler ) { STextureSampler2D ret = { Texture, Sampler }; return ret; }\n"
    "#define sampler2D STextureSampler2D\n"
    "#define tex2D(samp,uv) samp._Texture.Sample(samp._Sampler, uv)\n"
    "#define tex2Dlod(samp,uv_lod) samp._Texture.SampleLevel(samp._Sampler, (uv_lod).xy, (uv_lod).w)\n"
    "#define tex2Dlod0(samp,uv_lod) samp._Texture.SampleLevel(samp._Sampler, (uv_lod).xy, 0)\n"
    "#define tex2Dbias(samp,uv_bias) samp._Texture.SampleBias(samp._Sampler, (uv_bias).xy, (uv_bias).w)\n"
    "#define tex2Dgrad(samp,uv,ddx,ddy) samp._Texture.SampleGrad(samp._Sampler, uv, ddx, ddy)\n"
    "float fmod_loop( float x, float y ) { return x - y * floor( x / y ); }\n"
    "Texture2D HL_CheckMask_Texture : register(t0);\n"
    "SamplerState HL_CheckMask_Sampler : register(s0);\n"
    "#define HL_CheckMask CreateSampler2D(HL_CheckMask_Texture, HL_CheckMask_Sampler)\n"
    "cbuffer HL_CheckConstants : register(b0) { float vGlobalTime; };\n"
    // standardfuncsgfx.fxh's own helpers before secondary_color_mask (same signatures; the bodies
    // only have to compile here).
    "float cam_distance( float vMin, float vMax ) { return saturate( ( vMax - vMin ) * 0.0f ); }\n"
    "float CalculateOccupationMask( in float2 uv ) { return cos( uv.x + uv.y ); }\n";

std::string CheckFillPart(const std::vector<Pattern>& fills) {
    std::string s = kEdgeHelper;
    for (const Pattern& p : fills)
        if (p.usable) s += EmitPattern(p, "F", "\t");
    return s;
}

std::string CheckLinePart(const std::vector<Pattern>& lines) {
    std::string s = "static const float BORDER_TILE = 0.4f;\nstatic float HL_LineAlpha = 1.0f;\n";
    for (const Pattern& p : lines)
        if (p.usable) s += EmitPattern(p, "L", "\t");
    return s;
}

std::string CheckMain(const std::vector<Pattern>& fills, const std::vector<Pattern>* lines) {
    std::string s = "float4 main( float4 vHlPos : SV_POSITION, float2 vHlUV : TEXCOORD0 ) : SV_TARGET\n{\n";
    s += "\tfloat4 vHlSum = float4( 0.0f, 0.0f, 0.0f, 0.0f );\n";
    for (const Pattern& p : fills)
        if (p.usable)
            s += "\tvHlSum.rgb += HL_Fill_" + p.name + "( float3( vHlUV, 0.5f ), vHlUV, float4( vHlUV, vHlUV ), HL_CheckMask );\n";
    if (lines)
        for (const Pattern& p : *lines)
            if (p.usable)
                s += "\tvHlSum += HL_Line_" + p.name + "( float4( vHlUV, 0.5f, 1.0f ), float3( vHlUV, 0.5f ), vHlUV, vHlPos.xyz );\n";
    return s + "\treturn vHlSum;\n}\n";
}

}  // namespace

std::string CheckSourceFill(const std::vector<Pattern>& fills) {
    return std::string(kCheckPreamble) + CheckFillPart(fills) + CheckMain(fills, nullptr);
}

std::string CheckSourceBorder(const std::vector<Pattern>& fills, const std::vector<Pattern>& lines) {
    return std::string(kCheckPreamble) + CheckFillPart(fills) + CheckLinePart(lines) + CheckMain(fills, &lines);
}

std::string PatchFillShader(const std::string& fxh, const std::vector<Pattern>& fills) {
    if (fills.empty()) return std::string();
    const std::string kVanilla = "void secondary_color_mask(";
    size_t at = fxh.find(kVanilla);
    if (at == std::string::npos || fxh.find(kVanilla, at + 1) != std::string::npos) return std::string();
    size_t close = fxh.rfind("]]");  // the PixelShader block's code ends at the file's last `]]`
    if (close == std::string::npos || close < at) return std::string();

    std::string add = Line("\t// --- hoi4_loader fill shaders (NOTES.md M11/M12) ---");
    add += kEdgeHelper;
    // HL_DecodeMask: the area's texel -> its four values 0..1 (6 bits each; ShaderPatterns::FillTexelN):
    // the colour and alpha for a shader without declared variables, else its variables normalised.
    add += Line("\tfloat4 HL_DecodeMask( float4 vTexel )");
    add += Line("\t{");
    add += Line("\t\tint3 nB = int3( vTexel.rgb * 255.0f + 0.5f );");
    add += Line("\t\tint3 nLo = nB & 3;");
    add += Line("\t\treturn float4( float3( nB >> 2 ), (float)( nLo.r * 16 + nLo.g * 4 + nLo.b ) ) / 63.0f;");
    add += Line("\t}");
    for (const Pattern& p : fills)
        if (p.usable) add += EmitPattern(p, "F", "\t");
    add += Line("\tfloat3 HL_FillDispatch( int nId, float3 vColor, float2 vUV, float4 vMask, in sampler2D Mask )");
    add += Line("\t{");
    add += Line("\t\tfloat3 vHlOut = vColor;");
    const std::string kFillSrc[4] = {"vMask.r", "vMask.g", "vMask.b", "vMask.a"};
    const bool kFillNorm[4] = {true, true, true, true};
    bool first = true;
    for (size_t i = 0; i < fills.size(); ++i) {
        if (!fills[i].usable) continue;  // its id has no case: the plugin draws a fallback instead
        add += Line(std::string("\t\t") + (first ? "" : "else ") + "if ( nId == " + std::to_string(i + 1) + " )");
        first = false;
        add += Line("\t\t{");
        add += AssignVars(fills[i], "F", kFillSrc, kFillNorm, "\t\t\t");
        add += Line("\t\t\tvHlOut = HL_Fill_" + fills[i].name + "( vColor, vUV, vMask, Mask );");
        add += Line("\t\t}");
    }
    add += Line("\t\treturn vHlOut;");
    add += Line("\t}");
    add += Line("\tvoid secondary_color_mask( inout float3 vColor, float3 vNormal, float2 vUV, in sampler2D TexMaskSampler, inout float vBloomAlpha )");
    add += Line("\t{");
    // The mask's sampler filters (linear, with mipmaps), which blends the shader ids at province edges
    // and when zoomed out into other ids (squares of another pattern or faint stripes: second test).
    // A single exact texel instead gives a staircase of map pixels at close zoom (third test). So:
    // the 4 texels around the pixel, read exactly; the id covering most of the pixel wins, and its
    // bilinear coverage blends the pattern with what the other texels show. That other side is the
    // vanilla stripe look rebuilt from the texels that carry no shader id (the vanilla function itself
    // would read the filtered mask, ids included).
    add += Line("\t\tint nHlId = 0;");
    add += Line("\t\tfloat fHlCover = 1.0f;");
    add += Line("\t#ifdef PDX_DIRECTX_11");
    add += Line("\t\tfloat2 vHlSize;");
    add += Line("\t\tTexMaskSampler._Texture.GetDimensions( vHlSize.x, vHlSize.y );");
    add += Line("\t\tfloat2 vHlPos = frac( vUV ) * vHlSize - 0.5f;");
    add += Line("\t\tint2 nHlBase = int2( floor( vHlPos ) );");
    add += Line("\t\tfloat2 vHlF = vHlPos - floor( vHlPos );");
    add += Line("\t\tfloat4 vHlT[4] = { TexMaskSampler._Texture.Load( int3( nHlBase, 0 ) ), TexMaskSampler._Texture.Load( int3( nHlBase + int2( 1, 0 ), 0 ) ),");
    add += Line("\t\t                   TexMaskSampler._Texture.Load( int3( nHlBase + int2( 0, 1 ), 0 ) ), TexMaskSampler._Texture.Load( int3( nHlBase + int2( 1, 1 ), 0 ) ) };");
    add += Line("\t\tfloat fHlW[4] = { ( 1.0f - vHlF.x ) * ( 1.0f - vHlF.y ), vHlF.x * ( 1.0f - vHlF.y ), ( 1.0f - vHlF.x ) * vHlF.y, vHlF.x * vHlF.y };");
    add += Line("\t\tint nHlIds[4];");
    add += Line("\t\t[unroll] for ( int i = 0; i < 4; ++i )");
    add += Line("\t\t{");
    add += Line("\t\t\tint nK = 255 - (int)( vHlT[i].a * 255.0f + 0.5f );");
    add += Line("\t\t\tnHlIds[i] = ( nK >= 1 && nK <= " + std::to_string(fills.size()) + " ) ? nK : 0;");
    add += Line("\t\t}");
    add += Line("\t\tfHlCover = 0.0f;");
    add += Line("\t\tfloat4 vHlMask = float4( 0.0f, 0.0f, 0.0f, 0.0f );");
    add += Line("\t\t[unroll] for ( int j = 0; j < 4; ++j )");
    add += Line("\t\t{");
    add += Line("\t\t\tfloat fC = 0.0f;");
    add += Line("\t\t\t[unroll] for ( int k = 0; k < 4; ++k ) fC += ( nHlIds[k] == nHlIds[j] ) ? fHlW[k] : 0.0f;");
    add += Line("\t\t\tif ( nHlIds[j] != 0 && fC > fHlCover ) { fHlCover = fC; nHlId = nHlIds[j]; vHlMask = vHlT[j]; }");
    add += Line("\t\t}");
    add += Line("\t#else");
    add += Line("\t\tfloat4 vHlMask = tex2D( TexMaskSampler, vUV );");
    add += Line("\t\tint nHlCode = 255 - (int)( vHlMask.a * 255.0f + 0.5f );");
    add += Line("\t\tif ( nHlCode >= 1 && nHlCode <= " + std::to_string(fills.size()) + " ) nHlId = nHlCode;");
    add += Line("\t#endif");
    add += Line("\t\tif ( nHlId == 0 )");
    add += Line("\t\t{");
    // Outside every area. The vanilla function reads the filtered mask, and farther from the camera
    // its smaller mip levels spread an area's ids several texels out: faint stripes in the area's
    // colour fading away from its border (fourth test). Where the exact texels carry no stripes at
    // all, anything the filtered read finds is such blur, and vanilla would draw nothing: skip it.
    add += Line("\t#ifdef PDX_DIRECTX_11");
    add += Line("\t\t\tif ( vHlT[0].a + vHlT[1].a + vHlT[2].a + vHlT[3].a < 0.5f / 255.0f ) return;");
    // The game's own stripes here (occupation and the like). If the filtered read disagrees with the
    // exact texels, an area's ids have bled in through the mip levels and vanilla would draw these
    // stripes tinted in the area's colour (fifth test: green stripes next to the border). Then the
    // vanilla look comes from the exact texels (none of them carries an id); otherwise vanilla runs.
    add += Line("\t\t\tfloat4 vHlExact = vHlT[0] * fHlW[0] + vHlT[1] * fHlW[1] + vHlT[2] * fHlW[2] + vHlT[3] * fHlW[3];");
    add += Line("\t\t\tfloat4 vHlFiltered = tex2D( TexMaskSampler, vUV );");
    add += Line("\t\t\tif ( abs( vHlFiltered.a - vHlExact.a ) > 6.0f / 255.0f || distance( vHlFiltered.rgb, vHlExact.rgb ) > 0.05f )");
    add += Line("\t\t\t{");
    add += Line("\t\t\t\tfloat fHlOcc = CalculateOccupationMask( vUV ) * vHlExact.a;");
    add += Line("\t\t\t\tvColor = lerp( vColor, vHlExact.rgb, fHlOcc );");
    add += Line("\t\t\t\tvBloomAlpha = vBloomAlpha * ( 1.0f - fHlOcc );");
    add += Line("\t\t\t\treturn;");
    add += Line("\t\t\t}");
    add += Line("\t#endif");
    add += Line("\t\t\tHL_vanilla_secondary_color_mask( vColor, vNormal, vUV, TexMaskSampler, vBloomAlpha );");
    add += Line("\t\t\treturn;");
    add += Line("\t\t}");
    add += Line("\t\tfloat3 vHlPattern = HL_FillDispatch( nHlId, vColor, vUV, HL_DecodeMask( vHlMask ), TexMaskSampler );");
    add += Line("\t#ifdef PDX_DIRECTX_11");
    add += Line("\t\tif ( fHlCover < 0.999f )");
    add += Line("\t\t{");
    add += Line("\t\t\tfloat4 vHlOther = float4( 0.0f, 0.0f, 0.0f, 0.0f );");
    add += Line("\t\t\tfloat fHlOtherW = 0.0f;");
    add += Line("\t\t\t[unroll] for ( int m = 0; m < 4; ++m )");
    add += Line("\t\t\t\tif ( nHlIds[m] == 0 ) { vHlOther += vHlT[m] * fHlW[m]; fHlOtherW += fHlW[m]; }");
    add += Line("\t\t\tfloat3 vHlBase = vColor;");
    add += Line("\t\t\tif ( fHlOtherW > 0.0001f )");
    add += Line("\t\t\t{");
    add += Line("\t\t\t\tvHlOther /= fHlOtherW;");
    add += Line("\t\t\t\tfloat fHlOcc = CalculateOccupationMask( vUV ) * vHlOther.a;");
    add += Line("\t\t\t\tvHlBase = lerp( vColor, vHlOther.rgb, fHlOcc );");
    add += Line("\t\t\t\tvBloomAlpha = vBloomAlpha * ( 1.0f - fHlOcc * ( 1.0f - fHlCover ) );");
    add += Line("\t\t\t}");
    add += Line("\t\t\tvColor = lerp( vHlBase, vHlPattern, fHlCover );");
    add += Line("\t\t\treturn;");
    add += Line("\t\t}");
    add += Line("\t#endif");
    add += Line("\t\tvColor = vHlPattern;");
    add += Line("\t}");

    std::string out = fxh.substr(0, at) + "void HL_vanilla_secondary_color_mask(" + fxh.substr(at + kVanilla.size(), close - at - kVanilla.size());
    out += add + fxh.substr(close);
    return out;
}

std::string PatchLineShader(const std::string& shader, const std::vector<Pattern>& lines) {
    if (lines.empty()) return std::string();
    static const std::regex kMain("float4\\s+main\\s*\\(\\s*VS_OUTPUT_BORDER\\s+Input\\s*\\)\\s*:\\s*PDX_COLOR");
    std::smatch m;
    if (!std::regex_search(shader, m, kMain)) return std::string();
    size_t at = (size_t)m.position(0), len = (size_t)m.length(0);
    size_t close = shader.find("]]", at + len);  // the pixel shader's MainCode block ends here
    if (close == std::string::npos) return std::string();

    std::string add = Line("\t\t// --- hoi4_loader line shaders (NOTES.md M11/M12) ---");
    add += Line("\t\tstatic float HL_LineAlpha = 1.0f;  // the area's alpha, set before each line shader runs");
    for (const Pattern& p : lines)
        if (p.usable) add += EmitPattern(p, "L", "\t\t");
    add += Line("\t\tfloat4 HL_LineDispatch( int nId, float4 vColor, float3 vAreaColor, float2 vUV, float3 vPos )");
    add += Line("\t\t{");
    add += Line("\t\t\tfloat4 vHlRes = vColor;");
    // Lines: the first three variables are the slot's rgb as written (full precision, any value), the
    // fourth the slot alpha's 0..1 part.
    const std::string kLineSrc[4] = {"vAreaColor.r", "vAreaColor.g", "vAreaColor.b", "HL_LineAlpha"};
    const bool kLineNorm[4] = {false, false, false, true};
    bool first = true;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!lines[i].usable) continue;
        add += Line(std::string("\t\t\t") + (first ? "" : "else ") + "if ( nId == " + std::to_string(i + 1) + " )");
        first = false;
        add += Line("\t\t\t{");
        add += AssignVars(lines[i], "L", kLineSrc, kLineNorm, "\t\t\t\t");
        add += Line("\t\t\t\tvHlRes = HL_Line_" + lines[i].name + "( vColor, vAreaColor, vUV, vPos );");
        add += Line("\t\t\t}");
    }
    add += Line("\t\t\treturn vHlRes;");
    add += Line("\t\t}");
    add += Line("\t\tfloat4 main( VS_OUTPUT_BORDER Input ) : PDX_COLOR");
    add += Line("\t\t{");
    add += Line("\t\t\tfloat4 vHlOut = HL_VanillaBorderMain( Input );");
    // vSelectionColor.a = 2 + id + 0.9 alpha (ShaderPatterns::LineAlpha); vanilla outlines use 0..1.
    add += Line("\t\t\tfloat fHlCode = vSelectionColor.a - 2.0f;");
    add += Line("\t\t\tint nHlId = (int)floor( fHlCode );");
    add += Line("\t\t\tif ( vSelectionColor.a >= 2.95f && nHlId >= 1 && nHlId <= " + std::to_string(lines.size()) + " )");
    add += Line("\t\t\t{");
    add += Line("\t\t\t\tHL_LineAlpha = saturate( ( fHlCode - (float)nHlId ) / 0.9f );");
    add += Line("\t\t\t\tvHlOut = HL_LineDispatch( nHlId, vHlOut, vSelectionColor.rgb, Input.uv, Input.pos );");
    add += Line("\t\t\t}");
    add += Line("\t\t\treturn vHlOut;");
    add += Line("\t\t}");

    std::string out = shader.substr(0, at) + "float4 HL_VanillaBorderMain( VS_OUTPUT_BORDER Input )" +
                      shader.substr(at + len, close - at - len);
    out += add + "\t" + shader.substr(close);
    return out;
}

}  // namespace ShaderPatterns
