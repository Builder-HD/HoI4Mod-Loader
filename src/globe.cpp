// globe (NOTES.md Part S), the pure part. See globe.h.
#include "globe.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <regex>

#include "script_tree.h"

namespace Globe {

namespace {

const float kPi = 3.14159265358979f;
const float kMinT = 0.0005f;  // below this the sphere is so large that the map counts as flat

bool NumberOf(const ScriptTree::Node& n, float lo, float hi, float* out) {
    if (n.block || n.value.empty()) return false;
    char* end = nullptr;
    double v = strtod(n.value.c_str(), &end);
    if (!end || *end || v < lo || v > hi) return false;
    *out = (float)v;
    return true;
}

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// key = { r g b } with each 0..1.
bool ColorOf(const ScriptTree::Node& n, float out[3]) {
    std::vector<std::string> v;
    std::string why;
    if (!ScriptTree::ListOf(n, &v, &why) || v.size() != 3) return false;
    float c[3];
    for (size_t k = 0; k < 3; ++k) {
        char* end = nullptr;
        double x = strtod(v[k].c_str(), &end);
        if (!end || *end || x < 0 || x > 1) return false;
        c[k] = (float)x;
    }
    out[0] = c[0], out[1] = c[1], out[2] = c[2];
    return true;
}

}  // namespace

void ParseFile(const std::string& text, const std::string& shown, Settings* s, std::vector<Problem>* problems) {
    std::vector<ScriptTree::Node> top;
    std::string error;
    int errorLine = 0;
    if (!ScriptTree::Parse(text, &top, &error, &errorLine)) problems->push_back({shown, errorLine, error});
    for (const ScriptTree::Node& g : top) {
        if (!ScriptTree::EqualsNoCase(g.key, "globe") || !g.block) {
            problems->push_back({shown, g.line, "unknown entry '" + ScriptTree::Label(g) + "' (expected globe = { ... })"});
            continue;
        }
        s->present = true;
        for (const ScriptTree::Node& n : g.children) {
            bool ok = true;
            if (ScriptTree::EqualsNoCase(n.key, "default")) ok = ScriptTree::BoolOf(n, &s->defaultOn);
            else if (ScriptTree::EqualsNoCase(n.key, "flat_height")) ok = NumberOf(n, 0, 100000, &s->flatHeight);
            else if (ScriptTree::EqualsNoCase(n.key, "globe_height")) ok = NumberOf(n, 0, 100000, &s->globeHeight);
            else if (ScriptTree::EqualsNoCase(n.key, "transition")) ok = NumberOf(n, 0, 10, &s->transition);
            else if (ScriptTree::EqualsNoCase(n.key, "north")) ok = NumberOf(n, -90, 90, &s->north);
            else if (ScriptTree::EqualsNoCase(n.key, "south")) ok = NumberOf(n, -90, 90, &s->south);
            else if (ScriptTree::EqualsNoCase(n.key, "polar_cap")) {
                float v = 0;
                ok = NumberOf(n, 0, 90, &v);
                if (ok) s->polarCapNorth = v, s->polarCapSouth = -v;
            } else if (ScriptTree::EqualsNoCase(n.key, "polar_cap_north")) ok = NumberOf(n, -90, 90, &s->polarCapNorth);
            else if (ScriptTree::EqualsNoCase(n.key, "polar_cap_south")) ok = NumberOf(n, -90, 90, &s->polarCapSouth);
            else if (ScriptTree::EqualsNoCase(n.key, "polar_sea_color")) ok = ColorOf(n, s->polarSeaColor);
            else if (ScriptTree::EqualsNoCase(n.key, "polar_cap_color")) ok = ColorOf(n, s->polarColor);
            else if (ScriptTree::EqualsNoCase(n.key, "background")) {
                ok = !n.block && !n.value.empty() && n.value.find("..") == std::string::npos && n.value.find(':') == std::string::npos;
                if (ok) s->background = n.value, s->backgroundFrom = shown;
            } else {
                problems->push_back({shown, n.line, "unknown key '" + ScriptTree::Label(n) + "'"});
                continue;
            }
            if (!ok) problems->push_back({shown, n.line, "bad value for " + n.key});
        }
    }
    if (s->present && s->north <= s->south) {
        problems->push_back({shown, 0, "north must be above south; using 90 / -90"});
        s->north = 90.0f, s->south = -90.0f;
    }
}

bool WantSphere(const Settings& s, float height, bool edgeInView, SphereState* st) {
    bool want;
    if (height < s.flatHeight) want = false;
    else if (height >= s.globeHeight) want = true;
    else want = edgeInView || (st->sphere && height >= st->enterHeight * 0.85f);
    if (want && !st->sphere) st->enterHeight = height;
    st->sphere = want;
    return want;
}

bool MakeParams(const Settings& s, float w, float h, float t, float lx, float lz, Params* out) {
    *out = Params();
    if (!(t > kMinT) || !(w > 1.0f) || !(h > 1.0f)) return false;
    t = t > 1.0f ? 1.0f : t;
    float rs = w / (2.0f * kPi);          // the whole planet's radius: the map's width is the equator
    float r = rs / t;                     // the sphere at this curvature
    float span = (s.north - s.south) * kPi / 180.0f;
    float stretch = 1.0f + (rs * span / h - 1.0f) * t;  // z units per map unit: 1 flat, the latitude band's at t = 1
    float radPerZ = stretch / r;
    float zEq = h * (0.0f - s.south) / (s.north - s.south);
    float latL = Clamp((Clamp(lz, 0.0f, h) - zEq) * radPerZ, -kPi / 2, kPi / 2);
    out->a[0] = t, out->a[1] = r, out->a[2] = w, out->a[3] = 1.0f / w;
    out->b[0] = lx, out->b[1] = lz, out->b[2] = cosf(latL), out->b[3] = sinf(latL);
    out->c[0] = zEq, out->c[1] = radPerZ, out->c[2] = h;
    if (s.north < 90.0f || s.south > -90.0f) {
        // The map stops short of a pole: the polar seas, and the ice from its latitude (in flat z, past
        // the map's edges; never over the map), with a soft edge of 2 degrees. Without ice: far away.
        float zPerDeg = h / (s.north - s.south);
        out->d[0] = s.polarCapNorth < 90.0f ? (s.polarCapNorth - s.south) * zPerDeg : 1.0e9f;
        out->d[1] = s.polarCapSouth > -90.0f ? (s.polarCapSouth - s.south) * zPerDeg : -1.0e9f;
        if (out->d[0] < h) out->d[0] = h;
        if (out->d[1] > 0.0f) out->d[1] = 0.0f;
        out->d[2] = 2.0f * zPerDeg, out->d[3] = 1.0f;
        out->e[0] = s.polarColor[0], out->e[1] = s.polarColor[1], out->e[2] = s.polarColor[2];
        out->f[0] = s.polarSeaColor[0], out->f[1] = s.polarSeaColor[1], out->f[2] = s.polarSeaColor[2];
    }
    return true;
}

void Bend(const Params& p, const float in[3], float out[3]) {
    if (!(p.a[0] > 0.0f)) {
        out[0] = in[0], out[1] = in[1], out[2] = in[2];
        return;
    }
    float dx = in[0] - p.b[0];
    dx -= p.a[2] * roundf(dx * p.a[3]);
    float lon = dx / p.a[1];
    float lat = Clamp((in[2] - p.c[0]) * p.c[1], -kPi / 2, kPi / 2);
    float d[3] = {cosf(lat) * sinf(lon), cosf(lat) * cosf(lon), sinf(lat)};
    float r[3] = {d[0], d[1] * p.b[2] + d[2] * p.b[3], d[2] * p.b[2] - d[1] * p.b[3]};
    // Beyond the map's edges (the frame and logo past the poles, the filler water past the east and
    // west edges, which would cover the wrapped map) sinks into the planet.
    float beyond = (in[2] < 0 ? -in[2] : 0) + (in[2] > p.c[2] ? in[2] - p.c[2] : 0) + (in[0] < 0 ? -in[0] : 0) +
                   (in[0] > p.a[2] ? in[0] - p.a[2] : 0);
    float len = p.a[1] + in[1] - beyond * 4.0f * p.a[0];
    out[0] = p.b[0] + r[0] * len;
    out[1] = -p.a[1] + r[1] * len;
    out[2] = p.b[1] + r[2] * len;
}

bool Unbend(const Params& p, const float o[3], const float dir[3], float w, float h, float flat[2], bool* hit) {
    *hit = false;
    double dl = sqrt((double)dir[0] * dir[0] + (double)dir[1] * dir[1] + (double)dir[2] * dir[2]);
    if (!(p.a[0] > 0.0f) || !(dl > 1e-9)) return false;
    double R = p.a[1];
    double c[3] = {p.b[0], -R, p.b[1]};
    double d[3] = {dir[0] / dl, dir[1] / dl, dir[2] / dl};
    double m[3] = {o[0] - c[0], o[1] - c[1], o[2] - c[2]};
    double b = m[0] * d[0] + m[1] * d[1] + m[2] * d[2];
    double cc = m[0] * m[0] + m[1] * m[1] + m[2] * m[2] - R * R;
    double disc = b * b - cc;
    double v[3];
    if (disc >= 0 && -b - sqrt(disc) > 0) {
        double s = -b - sqrt(disc);
        for (int k = 0; k < 3; ++k) v[k] = (m[k] + s * d[k]) / R;
        *hit = true;
    } else {
        double s = -b > 0 ? -b : 0;  // the ray's point closest to the centre, pushed onto the sphere
        double q[3], ql = 0;
        for (int k = 0; k < 3; ++k) q[k] = m[k] + s * d[k], ql += q[k] * q[k];
        ql = sqrt(ql);
        if (!(ql > 1e-9)) return false;
        for (int k = 0; k < 3; ++k) v[k] = q[k] / ql;
    }
    double cl = p.b[2], sl = p.b[3];
    double y = v[1] * cl - v[2] * sl, z = v[2] * cl + v[1] * sl;
    double lat = asin(z < -1 ? -1 : (z > 1 ? 1 : z));
    double lon = atan2(v[0], y);
    double x = p.b[0] + lon * R;
    double fz = p.c[0] + lat / p.c[1];
    x = fmod(x, (double)w);
    if (x < 0) x += w;
    flat[0] = (float)x;
    flat[1] = (float)(fz < 0 ? 0 : (fz > h ? h : fz));
    if (fz < 0 || fz > h) *hit = false;  // the polar seas: no map there
    return true;
}

// --- Shader text ---------------------------------------------------------------------------------

namespace {

// Put in front of each patched code block (MainCode and Code blocks are global HLSL scope, after the
// game's constant buffers, so vCamPos and vCamLookAtDir are known). The guard keeps one copy per
// compile. Not DX11: the identity.
const char kHlsl[] =
    "\n"
    "#ifndef HL_GLOBE_DEFINED\n"
    "#define HL_GLOBE_DEFINED\n"
    "#ifdef PDX_DIRECTX_11\n"
    "cbuffer HL_GlobeConstants : register( b13 )\n"
    "{\n"
    "\tfloat4 HL_GlobeA;\t// curvature (0 = flat), radius, map width, 1 / map width\n"
    "\tfloat4 HL_GlobeB;\t// look-at x, look-at z, cos and sin of its latitude\n"
    "\tfloat4 HL_GlobeC;\t// z of the equator, radians per z unit, map height, background on\n"
    "\tfloat4 HL_GlobeD;\t// flat z where the north / south ice starts (beyond the map), the ice edge's width, polar seas on\n"
    "\tfloat4 HL_GlobeE;\t// the ice's colour\n"
    "\tfloat4 HL_GlobeF;\t// the polar seas' colour\n"
    "};\n"
    "Texture2D HL_GlobeSpace : register( t20 );\n"
    "// A point to drop: pushed along its view ray past the far plane, so it is clipped where it was seen\n"
    "// (a triangle partly dropped is cut there instead of smeared across the screen).\n"
    "float3 HL_GlobeAway( float3 q )\n"
    "{\n"
    "\treturn vCamPos + normalize( q - vCamPos ) * 1000000.0f;\n"
    "}\n"
    "// The sphere's centre in this pass's frame (a wrap copy's camera is a map width away).\n"
    "float3 HL_GlobeCentre()\n"
    "{\n"
    "\tfloat fPass = round( ( vCamPos.x - HL_GlobeB.x ) * HL_GlobeA.w );\n"
    "\treturn float3( HL_GlobeB.x + HL_GlobeA.z * fPass, -HL_GlobeA.y, HL_GlobeB.y );\n"
    "}\n"
    "// The bent position of flat point p. Dropped: what lies beyond the map's edges (frame, logo, filler\n"
    "// water; the polar seas are drawn in their place) and everything in a wrap copy's pass (the main\n"
    "// camera draws the whole planet).\n"
    "float3 HL_GlobePoint( float3 p, out float3 vCentre )\n"
    "{\n"
    "\tvCentre = HL_GlobeCentre();\n"
    "\tfloat dx = p.x - HL_GlobeB.x;\n"
    "\tdx -= HL_GlobeA.z * round( dx * HL_GlobeA.w );\n"
    "\tfloat fLon = dx / HL_GlobeA.y;\n"
    "\tfloat fLat = clamp( ( p.z - HL_GlobeC.x ) * HL_GlobeC.y, -1.5707963f, 1.5707963f );\n"
    "\tfloat3 d = float3( cos( fLat ) * sin( fLon ), cos( fLat ) * cos( fLon ), sin( fLat ) );\n"
    "\tfloat3 r = float3( d.x, d.y * HL_GlobeB.z + d.z * HL_GlobeB.w, d.z * HL_GlobeB.z - d.y * HL_GlobeB.w );\n"
    "\tfloat3 q = vCentre + r * ( HL_GlobeA.y + p.y );\n"
    "\tif ( p.z < 0.0f || p.z > HL_GlobeC.z || p.x < 0.0f || p.x > HL_GlobeA.z || vCentre.x != HL_GlobeB.x )\n"
    "\t\tq = HL_GlobeAway( q );\n"
    "\treturn q;\n"
    "}\n"
    "float4 HL_Globe( float4 vPos )\n"
    "{\n"
    "\tif ( HL_GlobeA.x <= 0.0f || vPos.w == 0.0f )\n"
    "\t\treturn vPos;\n"
    "\tfloat3 vCentre;\n"
    "\tfloat3 q = HL_GlobePoint( vPos.xyz / vPos.w, vCentre );\n"
    "\treturn float4( q * vPos.w, vPos.w );\n"
    "}\n"
    "// What lies on the ground (borders, rivers, trees, units): as the ground.\n"
    "float4 HL_GlobeSurface( float4 vPos )\n"
    "{\n"
    "\treturn HL_Globe( vPos );\n"
    "}\n"
    "// Also drops what lies beyond the planet's horizon: for things drawn without a depth test, such as\n"
    "// the names.\n"
    "float4 HL_GlobeCull( float4 vPos )\n"
    "{\n"
    "\tif ( HL_GlobeA.x <= 0.0f || vPos.w == 0.0f )\n"
    "\t\treturn vPos;\n"
    "\tfloat3 vCentre;\n"
    "\tfloat3 q = HL_GlobePoint( vPos.xyz / vPos.w, vCentre );\n"
    "\tif ( dot( q - vCentre, vCamPos - q ) < 0.0f )\n"
    "\t\tq = HL_GlobeAway( q );\n"
    "\treturn float4( q * vPos.w, vPos.w );\n"
    "}\n"
    "// Nothing of it is drawn while bent (every vertex in one point): the game's water, whose coarse grid\n"
    "// would sink under the curved seabed; the seabed shows the sea instead (HL_GlobeSea).\n"
    "float4 HL_GlobeHide( float4 vPos )\n"
    "{\n"
    "\tif ( HL_GlobeA.x <= 0.0f )\n"
    "\t\treturn vPos;\n"
    "\treturn float4( HL_GlobeB.x, -HL_GlobeA.y, HL_GlobeB.y, 1.0f );\n"
    "}\n"
    "// Distance fog: across the wrap line the short way round.\n"
    "float3 HL_GlobeFogDiff( float3 vDiff )\n"
    "{\n"
    "\tif ( HL_GlobeA.x > 0.0f )\n"
    "\t\tvDiff.x -= HL_GlobeA.z * round( vDiff.x * HL_GlobeA.w );\n"
    "\treturn vDiff;\n"
    "}\n"
    "// Water: the refraction texture would show what lies behind the planet (the sky); the game's own\n"
    "// colour for water without refractions instead.\n"
    "float3 HL_GlobeWater( float3 vColor )\n"
    "{\n"
    "\treturn HL_GlobeA.x > 0.0f ? lerp( vColor, float3( 0.0f, 0.1f, 0.2f ), saturate( HL_GlobeA.x ) ) : vColor;\n"
    "}\n"
    "// The sea: on the sphere the game's water (a coarse grid) sinks under the curved seabed, so the\n"
    "// seabed takes the water's colour instead: the colour map's sea tint, as the water shader shows it\n"
    "// (its refraction lightness, darkened), blended in at the coast.\n"
    "float3 HL_GlobeSea( float3 vDiffuse, float3 vTint, float fHeight )\n"
    "{\n"
    "\tif ( HL_GlobeA.x <= 0.0f )\n"
    "\t\treturn vDiffuse;\n"
    "\tfloat fSea = saturate( ( WATER_HEIGHT - fHeight ) * 1.5f ) * saturate( HL_GlobeA.x );\n"
    "\treturn lerp( vDiffuse, vTint * WATER_COLOR_LIGHTNESS * 0.8f, fSea );\n"
    "}\n"
    "float3 HL_GlobeSpaceTexel( int2 i, int2 vSize )\n"
    "{\n"
    "\treturn HL_GlobeSpace.Load( int3( clamp( i, int2( 0, 0 ), vSize - 1 ), 0 ) ).rgb;\n"
    "}\n"
    "// The background behind the planet: the mod's image, fixed to the screen (the camera looks almost\n"
    "// straight down), drifting a little as the planet turns, mirrored where it repeats.\n"
    "float3 HL_GlobeBackground( float3 vColor, float3 d )\n"
    "{\n"
    "\tif ( HL_GlobeC.w <= 0.5f )\n"
    "\t\treturn vColor;\n"
    "\tuint w, h;\n"
    "\tHL_GlobeSpace.GetDimensions( w, h );\n"
    "\tif ( w == 0 || h == 0 )\n"
    "\t\treturn vColor;\n"
    "\tfloat2 uv = d.xz / max( -d.y, 0.05f ) * 0.68f;\n"
    "\tuv = float2( 0.5f + uv.x + HL_GlobeB.x * HL_GlobeA.w * 0.5f, 0.5f - uv.y );\n"
    "\tuv = 1.0f - abs( frac( uv * 0.5f ) * 2.0f - 1.0f );\n"
    "\tfloat2 p = uv * float2( w, h ) - 0.5f;\n"
    "\tint2 i = int2( floor( p ) );\n"
    "\tfloat2 f = p - floor( p );\n"
    "\tint2 vSize = int2( w, h );\n"
    "\treturn lerp( lerp( HL_GlobeSpaceTexel( i, vSize ), HL_GlobeSpaceTexel( i + int2( 1, 0 ), vSize ), f.x ),\n"
    "\t             lerp( HL_GlobeSpaceTexel( i + int2( 0, 1 ), vSize ), HL_GlobeSpaceTexel( i + int2( 1, 1 ), vSize ), f.x ), f.y );\n"
    "}\n"
    "// Behind the planet the background; where the planet has no ground (beyond the map's north and south\n"
    "// edges) the polar seas and ice, traced on the sphere: sea from the map's edge, ice from the ice\n"
    "// latitude to the pole, its edge wandering a little with the longitude (whole waves round the planet).\n"
    "// The colours are scaled to about what the sun makes of a terrain of that colour. The alpha is the\n"
    "// glow (bloom): the sky's 1, none on the planet.\n"
    "float4 HL_GlobeSky( float3 vColor, float3 vPos )\n"
    "{\n"
    "\tif ( HL_GlobeA.x <= 0.0f )\n"
    "\t\treturn float4( vColor, 1.0f );\n"
    "\tfloat3 d = normalize( vPos - vCamPos );\n"
    "\tfloat4 vBack = float4( lerp( vColor, HL_GlobeBackground( vColor, d ), saturate( HL_GlobeA.x ) ), 1.0f );\n"
    "\tif ( HL_GlobeD.w <= 0.5f )\n"
    "\t\treturn vBack;\n"
    "\tfloat3 c = HL_GlobeCentre();\n"
    "\tfloat3 m = vCamPos - c;\n"
    "\tfloat b = dot( m, d );\n"
    "\tfloat fDisc = b * b - ( dot( m, m ) - HL_GlobeA.y * HL_GlobeA.y );\n"
    "\tif ( fDisc < 0.0f )\n"
    "\t\treturn vBack;\n"
    "\tfloat s = -b - sqrt( fDisc );\n"
    "\tif ( s <= 0.0f )\n"
    "\t\treturn vBack;\n"
    "\tfloat3 v = ( m + d * s ) / HL_GlobeA.y;\n"
    "\tfloat y = v.y * HL_GlobeB.z - v.z * HL_GlobeB.w;\n"
    "\tfloat z = v.z * HL_GlobeB.z + v.y * HL_GlobeB.w;\n"
    "\tfloat fZ = HL_GlobeC.x + asin( clamp( z, -1.0f, 1.0f ) ) / HL_GlobeC.y;\n"
    "\tfloat fTurn = ( HL_GlobeB.x * HL_GlobeA.w ) * 6.2831853f + atan2( v.x, y );\n"
    "\tfloat fWander = ( sin( fTurn * 5.0f ) * 0.6f + sin( fTurn * 9.0f + 1.3f ) * 0.4f ) * HL_GlobeD.z * 0.35f;\n"
    "\tfloat fIce = max( saturate( ( fZ - HL_GlobeD.x - fWander ) / HL_GlobeD.z + 0.5f ),\n"
    "\t                  saturate( ( HL_GlobeD.y - fWander - fZ ) / HL_GlobeD.z + 0.5f ) );\n"
    "\tfloat3 vSurface = lerp( HL_GlobeF.rgb, HL_GlobeE.rgb, fIce ) * 0.35f;\n"
    "\tfloat fOn = saturate( HL_GlobeA.x );\n"
    "\treturn float4( lerp( vBack.rgb, vSurface, fOn ), 1.0f - fOn );\n"
    "}\n"
    "#else\n"
    "float4 HL_Globe( float4 vPos ) { return vPos; }\n"
    "float4 HL_GlobeCull( float4 vPos ) { return vPos; }\n"
    "float4 HL_GlobeSurface( float4 vPos ) { return vPos; }\n"
    "float4 HL_GlobeHide( float4 vPos ) { return vPos; }\n"
    "float3 HL_GlobeFogDiff( float3 vDiff ) { return vDiff; }\n"
    "float3 HL_GlobeWater( float3 vColor ) { return vColor; }\n"
    "float4 HL_GlobeSky( float3 vColor, float3 vPos ) { return float4( vColor, 1.0f ); }\n"
    "float3 HL_GlobeSea( float3 vDiffuse, float3 vTint, float fHeight ) { return vDiffuse; }\n"
    "#endif\n"
    "#endif\n";

// Index just past the ')' closing the expression that starts at `from`, or npos.
size_t ExpressionEnd(const std::string& s, size_t from) {
    int depth = 0;
    for (size_t i = from; i < s.size(); ++i) {
        char ch = s[i];
        if (ch == '(') ++depth;
        else if (ch == ')') {
            if (depth == 0) return i;
            --depth;
        } else if (ch == ';' || ch == '{' || ch == '}') {
            return std::string::npos;
        }
    }
    return std::string::npos;
}

std::string PatchCode(const std::string& code, int mode, int* count) {
    std::string out = code;
    const char* fn = mode == kHide ? "HL_GlobeHide( " : mode == kCull ? "HL_GlobeCull( " : mode == kBend ? "HL_GlobeSurface( " : "HL_Globe( ";
    static const std::regex mulRx(R"(mul\s*\(\s*ViewProjectionMatrix\s*,\s*)");
    // mul( ViewProjectionMatrix, X ) -> mul( ViewProjectionMatrix, HL_Globe( X ) ), from the back so
    // earlier positions stay valid.
    std::vector<std::pair<size_t, size_t>> spans;
    for (auto it = std::sregex_iterator(out.begin(), out.end(), mulRx); it != std::sregex_iterator(); ++it) {
        size_t start = (size_t)it->position() + (size_t)it->length();
        if (out.compare(start, 8, "HL_Globe") == 0) continue;
        size_t end = ExpressionEnd(out, start);
        if (end == std::string::npos) continue;
        size_t last = end;
        while (last > start && (out[last - 1] == ' ' || out[last - 1] == '\t')) --last;
        if (last > start) spans.push_back({start, last});
    }
    for (auto k = spans.rbegin(); k != spans.rend(); ++k) {
        out.insert(k->second, " )");
        out.insert(k->first, fn);
        ++*count;
    }
    // Depth bias rows: dot( P, float4( GetMatrixData( ViewProjectionMatrix, 2, 0 ), ... ) )
    static const std::regex dotRx(R"(dot\s*\(\s*([A-Za-z_]\w*)(\s*,\s*float4\s*\(\s*GetMatrixData\s*\(\s*ViewProjectionMatrix\s*,\s*2\s*,))");
    std::string res;
    size_t pos = 0;
    for (auto it = std::sregex_iterator(out.begin(), out.end(), dotRx); it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        res.append(out, pos, (size_t)m.position() - pos);
        res += std::string("dot( ") + fn + m[1].str() + " )" + m[2].str();  // the same position as the mul's
        pos = (size_t)m.position() + (size_t)m.length();
        ++*count;
    }
    res.append(out, pos, std::string::npos);
    return res;
}

// Pixel shader code: `X = tex2D( WaterRefraction, ... ).rgb;` gets `X = HL_GlobeWater( X );` after it.
std::string PixelCode(const std::string& code, int* count) {
    static const std::regex rx(R"((?:float3\s+)?([A-Za-z_]\w*)\s*=\s*tex2D\s*\(\s*WaterRefraction\s*,[^;]*;)");
    std::string res;
    size_t pos = 0;
    for (auto it = std::sregex_iterator(code.begin(), code.end(), rx); it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        size_t end = (size_t)m.position() + (size_t)m.length();
        res.append(code, pos, end - pos);
        res += " " + m[1].str() + " = HL_GlobeWater( " + m[1].str() + " );";
        pos = end;
        ++*count;
    }
    res.append(code, pos, std::string::npos);
    // The terrain's colour before the map modes are laid over it: the seabed gets the sea's colour on
    // the sphere (`diffuse.rgb = HL_GlobeSea( diffuse.rgb, TerrainColor.rgb, Input.prepos.y );`).
    static const std::regex gbRx(R"(\n([ \t]*)gradient_border_apply\s*\(\s*diffuse\.rgb)");
    std::smatch g;
    if (res.find("TerrainColor") != std::string::npos && res.find("Input.prepos") != std::string::npos &&
        std::regex_search(res, g, gbRx)) {
        size_t at = (size_t)g.position() + 1;
        res.insert(at, g[1].str() + "diffuse.rgb = HL_GlobeSea( diffuse.rgb, TerrainColor.rgb, Input.prepos.y );\n");
        ++*count;
    }
    return res;
}

// Top-level section starts: (position, true for VertexShader).
std::vector<std::pair<size_t, bool>> Sections(const std::string& s) {
    std::vector<std::pair<size_t, bool>> out;
    static const std::regex rx(R"((^|\n)(VertexShader|PixelShader)[ \t]*=[ \t\r\n]*\{)");
    for (auto it = std::sregex_iterator(s.begin(), s.end(), rx); it != std::sregex_iterator(); ++it)
        out.push_back({(size_t)it->position(2), (*it)[2].str() == "VertexShader"});
    return out;
}

// Inserts the helpers right before `anchor` (the first match) and replaces `from` with `to` after
// it; "" if either is missing or the file has the helpers already.
std::string InsertAndReplace(const std::string& text, const std::string& anchor, const std::string& from, const std::string& to) {
    if (text.find("HL_GLOBE_DEFINED") != std::string::npos) return "";
    size_t a = text.find(anchor);
    if (a == std::string::npos) return "";
    size_t f = text.find(from, a);
    if (f == std::string::npos) return "";
    std::string out = text.substr(0, f) + to + text.substr(f + from.size());
    // The helpers go at the start of the anchor's line (global scope inside the Code block).
    size_t line = out.rfind('\n', a);
    line = line == std::string::npos ? 0 : line + 1;
    return out.substr(0, line) + kHlsl + out.substr(line);
}

}  // namespace

const char* HlslCode() { return kHlsl; }

std::string PatchVertexCode(const std::string& code, int mode, int* count) {
    *count = 0;
    return PatchCode(code, mode, count);
}

std::string PatchPixelCode(const std::string& code, int* count) {
    *count = 0;
    return PixelCode(code, count);
}

std::string PatchMapShader(const std::string& shader, int mode, int* count) {
    *count = 0;
    if (shader.find("HL_GLOBE_DEFINED") != std::string::npos) return "";
    std::vector<std::pair<size_t, bool>> sections = Sections(shader);
    std::string out;
    size_t pos = 0;
    static const std::regex mainRx(R"(MainCode[ \t]+([A-Za-z_]\w*)[ \t\r\n]*\[\[)");
    for (auto it = std::sregex_iterator(shader.begin(), shader.end(), mainRx); it != std::sregex_iterator(); ++it) {
        size_t at = (size_t)it->position();
        bool vertex = false;
        for (const auto& sec : sections)
            if (sec.first < at) vertex = sec.second;
        std::string name = (*it)[1].str();
        size_t codeStart = at + (size_t)it->length();
        size_t codeEnd = shader.find("]]", codeStart);
        if (codeEnd == std::string::npos) break;
        if (vertex && name.find("Shadow") != std::string::npos) continue;
        int n = 0;
        std::string block = shader.substr(codeStart, codeEnd - codeStart);
        std::string code = vertex ? PatchCode(block, mode, &n) : PixelCode(block, &n);
        if (n == 0) continue;
        out.append(shader, pos, codeStart - pos);
        out += kHlsl;
        out += code;
        pos = codeEnd;
        *count += n;
    }
    if (*count == 0) return "";
    out.append(shader, pos, std::string::npos);
    return out;
}

std::string PatchSharedCode(const std::string& fxh) {
    return InsertAndReplace(fxh, "float CalculateDistanceFogFactor", "float3 vDiff = vCamPos - vPos;",
                            "float3 vDiff = HL_GlobeFogDiff( vCamPos - vPos );");
}

std::string PatchSkyShader(const std::string& sky) {
    // The sky's pixel shader main returns HL_GlobeSky (its colour and glow) instead.
    return InsertAndReplace(sky, "float4 main( VS_OUTPUT_SKY Input ) : PDX_COLOR", "return float4( color, 1.0f );",
                            "return HL_GlobeSky( color, Input.pos );");
}

}  // namespace Globe
