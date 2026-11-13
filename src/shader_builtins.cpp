// The loader's own attention area shaders (NOTES.md M12): what used to be hardcoded styles. Mods
// can replace any of them with a file of the same name (shader_patterns.h). Checked with fxc by
// tests/shader_patterns_test.cpp --splice-builtins.
#include "shader_patterns.h"

namespace ShaderPatterns {

namespace {

const char kSrc[] = "hoi4_loader built-in";

// --- Lines (the area's alpha: HL_LineAlpha, set by the dispatcher) ------------------------------------
// solid: the area's colour across the strip, soft at its sides; fades with zoom as the game's own
// borders do (the vanilla alpha).
const char kLineSolid[] =
    "\t\tfloat4 HL_Line_solid( float4 vColor, float3 vAreaColor, float2 vUV, float3 vPos )\n"
    "\t\t{\n"
    "\t\t\tfloat fA = 1.0f - smoothstep( 0.3f, 0.5f, abs( vUV.x - 0.5f ) );\n"
    "\t\t\treturn float4( vAreaColor, fA * saturate( vColor.a ) * HL_LineAlpha );\n"
    "\t\t}\n";

// pulse: solid, its brightness breathing.
const char kLinePulse[] =
    "\t\tfloat4 HL_Line_pulse( float4 vColor, float3 vAreaColor, float2 vUV, float3 vPos )\n"
    "\t\t{\n"
    "\t\t\tfloat fA = 1.0f - smoothstep( 0.3f, 0.5f, abs( vUV.x - 0.5f ) );\n"
    "\t\t\tfloat fBeat = 0.55f + 0.45f * sin( vGlobalTime * 3.0f );\n"
    "\t\t\treturn float4( vAreaColor * ( 0.6f + 0.6f * fBeat ), fA * saturate( vColor.a ) * ( 0.5f + 0.5f * fBeat ) * HL_LineAlpha );\n"
    "\t\t}\n";

// dashed: dashes of the area's colour and black, marching along the line.
const char kLineDashed[] =
    "\t\tfloat4 HL_Line_dashed( float4 vColor, float3 vAreaColor, float2 vUV, float3 vPos )\n"
    "\t\t{\n"
    "\t\t\tfloat fDash = step( 0.5f, frac( vUV.y * BORDER_TILE * 2.0f - vGlobalTime ) );\n"
    "\t\t\tfloat fA = 1.0f - smoothstep( 0.35f, 0.5f, abs( vUV.x - 0.5f ) );\n"
    "\t\t\treturn float4( vAreaColor * fDash, fA * saturate( vColor.a ) * HL_LineAlpha );\n"
    "\t\t}\n";

// --- Fills (the area's alpha: vMask.a) ---------------------------------------------------------------
// stripes: the game's diagonal occupation stripes, in the area's colour.
const char kFillStripes[] =
    "\tfloat3 HL_Fill_stripes( float3 vColor, float2 vUV, float4 vMask, in sampler2D Mask )\n"
    "\t{\n"
    "\t\treturn lerp( vColor, vMask.rgb, CalculateOccupationMask( vUV ) * vMask.a );\n"
    "\t}\n";

// solid: a translucent wash of the area's colour.
const char kFillSolid[] =
    "\tfloat3 HL_Fill_solid( float3 vColor, float2 vUV, float4 vMask, in sampler2D Mask )\n"
    "\t{\n"
    "\t\treturn lerp( vColor, vMask.rgb, 0.45f * vMask.a );\n"
    "\t}\n";

// band: the area's colour along the inside of its edge, fading inwards over 12 map pixels.
const char kFillBand[] =
    "\tfloat3 HL_Fill_band( float3 vColor, float2 vUV, float4 vMask, in sampler2D Mask )\n"
    "\t{\n"
    "\t\tfloat fBand = 1.0f - smoothstep( 1.0f, 12.0f, HL_EdgeDistance( Mask, vUV, 12.0f ) );\n"
    "\t\treturn lerp( vColor, vMask.rgb, fBand * 0.85f * vMask.a );\n"
    "\t}\n";

// glow: a wider, softer band that breathes (between 45 % and 100 %; first test: down to 20 % and a
// squared falloff read as barely there over the political map's colours), with a bright rim.
const char kFillGlow[] =
    "\tfloat3 HL_Fill_glow( float3 vColor, float2 vUV, float4 vMask, in sampler2D Mask )\n"
    "\t{\n"
    "\t\tfloat fBand = 1.0f - smoothstep( 0.0f, 20.0f, HL_EdgeDistance( Mask, vUV, 20.0f ) );\n"
    "\t\tfloat fBeat = ( 0.725f + 0.275f * sin( vGlobalTime * 2.5f ) ) * vMask.a;\n"
    "\t\tfloat3 c = lerp( vColor, vMask.rgb, saturate( fBand * fBeat * 1.1f ) );\n"
    "\t\treturn c + vMask.rgb * fBand * fBand * fBeat * 0.6f;\n"
    "\t}\n";

}  // namespace

const std::vector<Pattern>& Builtins(Kind kind) {
    static const std::vector<Pattern> kLines = {
        {"dashed", kLineDashed, kSrc}, {"pulse", kLinePulse, kSrc}, {"solid", kLineSolid, kSrc}};
    static const std::vector<Pattern> kFills = {
        {"band", kFillBand, kSrc}, {"glow", kFillGlow, kSrc}, {"solid", kFillSolid, kSrc}, {"stripes", kFillStripes, kSrc}};
    return kind == Kind::Line ? kLines : kFills;
}

}  // namespace ShaderPatterns
