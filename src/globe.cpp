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
            else if (ScriptTree::EqualsNoCase(n.key, "north")) ok = NumberOf(n, -90, 90, &s->north);
            else if (ScriptTree::EqualsNoCase(n.key, "south")) ok = NumberOf(n, -90, 90, &s->south);
            else {
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

float Curvature(const Settings& s, float height) {
    if (height <= s.flatHeight) return 0.0f;
    if (s.globeHeight <= s.flatHeight || height >= s.globeHeight) return 1.0f;
    float x = (height - s.flatHeight) / (s.globeHeight - s.flatHeight);
    return x * x * (3.0f - 2.0f * x);
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
    return true;
}

// --- Shader text ---------------------------------------------------------------------------------

namespace {

// Put in front of each patched vertex shader's code (MainCode blocks are global HLSL scope). The
// guard keeps one copy if two such blocks ever share a compile. Not DX11: the identity.
const char kHlsl[] =
    "\n#ifndef HL_GLOBE_DEFINED\n"
    "#define HL_GLOBE_DEFINED\n"
    "#ifdef PDX_DIRECTX_11\n"
    "cbuffer HL_GlobeConstants : register( b13 )\n"
    "{\n"
    "\tfloat4 HL_GlobeA;\t// curvature (0 = flat), radius, map width, 1 / map width\n"
    "\tfloat4 HL_GlobeB;\t// look-at x, look-at z, cos and sin of its latitude\n"
    "\tfloat4 HL_GlobeC;\t// z of the equator, radians per z unit, map height\n"
    "};\n"
    "float4 HL_Globe( float4 vPos )\n"
    "{\n"
    "\tif ( HL_GlobeA.x <= 0.0f || vPos.w == 0.0f )\n"
    "\t\treturn vPos;\n"
    "\tfloat3 p = vPos.xyz / vPos.w;\n"
    "\tfloat dx = p.x - HL_GlobeB.x;\n"
    "\tdx -= HL_GlobeA.z * round( dx * HL_GlobeA.w );\n"
    "\tfloat fLon = dx / HL_GlobeA.y;\n"
    "\tfloat fLat = clamp( ( p.z - HL_GlobeC.x ) * HL_GlobeC.y, -1.5707963f, 1.5707963f );\n"
    "\tfloat3 d = float3( cos( fLat ) * sin( fLon ), cos( fLat ) * cos( fLon ), sin( fLat ) );\n"
    "\tfloat3 r = float3( d.x, d.y * HL_GlobeB.z + d.z * HL_GlobeB.w, d.z * HL_GlobeB.z - d.y * HL_GlobeB.w );\n"
    "\tfloat fBeyond = max( -p.z, 0.0f ) + max( p.z - HL_GlobeC.z, 0.0f ) + max( -p.x, 0.0f ) + max( p.x - HL_GlobeA.z, 0.0f );\t// beyond the map's edges sinks\n"
    "\tfloat3 q = float3( HL_GlobeB.x, -HL_GlobeA.y, HL_GlobeB.y ) + r * ( HL_GlobeA.y + p.y - fBeyond * 4.0f * HL_GlobeA.x );\n"
    "\t// The wrap copies draw the map again a map width away: each point belongs to one pass (the copy\n"
    "\t// whose offset matches how far the point is from the camera); in the others it sinks, so nothing\n"
    "\t// is drawn twice (twice-blended water and borders would wash the planet out). The boundary is the\n"
    "\t// far side's middle meridian. The copy's pass draws the planet in its own frame.\n"
    "\tfloat fPass = round( ( vCamPos.x - HL_GlobeB.x ) * HL_GlobeA.w );\n"
    "\tif ( round( ( p.x - HL_GlobeB.x ) * HL_GlobeA.w ) != fPass )\n"
    "\t\tq = float3( HL_GlobeB.x, -HL_GlobeA.y, HL_GlobeB.y ) + r * ( HL_GlobeA.y * 0.5f );\n"
    "\tq.x += HL_GlobeA.z * fPass;\n"
    "\treturn float4( q * vPos.w, vPos.w );\n"
    "}\n"
    "#else\n"
    "float4 HL_Globe( float4 vPos ) { return vPos; }\n"
    "#endif\n"
    "#endif\n";

bool IsIdentChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

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

std::string PatchCode(const std::string& code, int* count) {
    std::string out = code;
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
        out.insert(k->first, "HL_Globe( ");
        ++*count;
    }
    // Depth bias rows: dot( P, float4( GetMatrixData( ViewProjectionMatrix, 2, 0 ), ... ) )
    static const std::regex dotRx(R"(dot\s*\(\s*([A-Za-z_]\w*)(\s*,\s*float4\s*\(\s*GetMatrixData\s*\(\s*ViewProjectionMatrix\s*,\s*2\s*,))");
    std::string res;
    size_t pos = 0;
    for (auto it = std::sregex_iterator(out.begin(), out.end(), dotRx); it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        res.append(out, pos, (size_t)m.position() - pos);
        res += "dot( HL_Globe( " + m[1].str() + " )" + m[2].str();
        pos = (size_t)m.position() + (size_t)m.length();
        ++*count;
    }
    res.append(out, pos, std::string::npos);
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

}  // namespace

const char* HlslCode() { return kHlsl; }

std::string PatchVertexCode(const std::string& code, int* count) {
    *count = 0;
    return PatchCode(code, count);
}

std::string PatchMapShader(const std::string& shader, int* count) {
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
        if (!vertex || name.find("Shadow") != std::string::npos) continue;
        int n = 0;
        std::string code = PatchCode(shader.substr(codeStart, codeEnd - codeStart), &n);
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

}  // namespace Globe
