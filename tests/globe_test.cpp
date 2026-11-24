// globe (NOTES.md Part S): the pure part. Optional arguments:
//   globe_test <game gfx/FX folder>            also patches the real map shaders and checks the shape
//   globe_test <gfx/FX folder> <vs_5_0 cache>  also compiles every cached vertex shader that uses the
//                                              view-projection matrix with the bend spliced in (vs_5_0)
#include <windows.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/globe.h"

static int g_checks = 0, g_failed = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        printf("FAIL: %s\n", what);
    }
}
static bool ReadFile(const std::string& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}
static size_t Count(const std::string& s, const std::string& sub) {
    size_t n = 0;
    for (size_t at = s.find(sub); at != std::string::npos; at = s.find(sub, at + 1)) ++n;
    return n;
}
static bool Near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

static const char kShader[] =
    "Includes = { \"standardfuncsgfx.fxh\" }\n"
    "VertexShader =\n{\n\tMainCode VertexShader\n\t[[\n"
    "\t\tVS_OUTPUT main( const VS_INPUT v )\n\t\t{\n"
    "\t\t\tfloat4 pos = float4( v.p, 1.0f );\n"
    "\t\t\tfloat4 vDistortedPos = pos - float4( vCamLookAtDir * 0.05f, 0.0f );\n"
    "\t\t\tpos = mul( ViewProjectionMatrix, pos );\n"
    "\t\t\tfloat vNewZ = dot( vDistortedPos, float4( GetMatrixData( ViewProjectionMatrix, 2, 0 ), GetMatrixData( ViewProjectionMatrix, 2, 1 ), GetMatrixData( ViewProjectionMatrix, 2, 2 ), GetMatrixData( ViewProjectionMatrix, 2, 3 ) ) );\n"
    "\t\t\tOut.p = mul( ViewProjectionMatrix, float4( Get( a, b ), 1.0f ) );\n"
    "\t\t}\n\t]]\n"
    "\tMainCode VertexShaderShadow\n\t[[\n\t\tOut.p = mul( ViewProjectionMatrix, pos );\n\t]]\n}\n"
    "PixelShader =\n{\n\tMainCode PixelShader\n\t[[\n\t\tfloat4 x = mul( ViewProjectionMatrix, pos );\n\t]]\n}\n";

static void TestMaths() {
    Globe::Settings s;
    s.present = true;
    Check(Globe::Curvature(s, 100) == 0.0f && Globe::Curvature(s, 3000) == 1.0f, "curvature ends");
    float mid = Globe::Curvature(s, (s.flatHeight + s.globeHeight) / 2);
    Check(Near(mid, 0.5f, 1e-4f), "curvature middle");
    Globe::Params p;
    Check(!Globe::MakeParams(s, 5632, 2048, 0.0f, 100, 100, &p) && p.t() == 0.0f, "t = 0: flat");
    float in[3] = {1234, 5, 777}, out[3];
    Globe::Bend(p, in, out);
    Check(out[0] == in[0] && out[1] == in[1] && out[2] == in[2], "flat bend is the identity");
    for (float t : {1.0f, 0.4f, 0.05f}) {
        float lx = 3000, lz = 1500;
        Check(Globe::MakeParams(s, 5632, 2048, t, lx, lz, &p), "params");
        float l[3] = {lx, 0, lz}, lo[3];
        Globe::Bend(p, l, lo);
        Check(Near(lo[0], lx, 0.01f) && Near(lo[1], 0, 0.01f) && Near(lo[2], lz, 0.01f), "the look-at point stays");
        // Near the look-at point: 1 unit east -> cos(latitude) east (meridians meet at the poles).
        float e[3] = {lx + 1, 0, lz}, eo[3];
        Globe::Bend(p, e, eo);
        Check(Near(eo[0] - lx, p.b[2], 0.01f) && Near(eo[1], 0, 0.01f), "one unit east");
        // Round trip: a straight-down ray onto a bent point unbends to the flat point.
        const float pts[][2] = {{3200, 1600}, {2600, 1200}, {3500, 1900}, {2900, 1000}};
        for (const auto& q : pts) {
            float f[3] = {q[0], 0, q[1]}, b[3];
            Globe::Bend(p, f, b);
            float o[3] = {lx, 4000, lz};
            float d[3] = {b[0] - o[0], b[1] - o[1], b[2] - o[2]};
            float flat[2];
            bool hit = false;
            Check(Globe::Unbend(p, o, d, 5632, 2048, flat, &hit) && hit, "unbend hits");
            Check(Near(flat[0], q[0], 0.5f) && Near(flat[1], q[1], 0.5f), "unbend round trip");
        }
    }
    // Whole planet: the map wraps (x = 5620 and x = 12 are 24 units apart across the seam); what lies
    // beyond the map's edges sinks below the surface.
    Globe::MakeParams(s, 5632, 2048, 1.0f, 100, 1024, &p);
    float a[3] = {5620, 0, 1024}, b2[3] = {12, 0, 1024}, ao[3], bo[3];
    Globe::Bend(p, a, ao);
    Globe::Bend(p, b2, bo);
    float gap = sqrtf((ao[0] - bo[0]) * (ao[0] - bo[0]) + (ao[1] - bo[1]) * (ao[1] - bo[1]) + (ao[2] - bo[2]) * (ao[2] - bo[2]));
    Check(Near(gap, 24.0f, 0.5f), "wrap across the seam");
    float out1[3] = {5700, 0, 1024}, oo[3];
    Globe::Bend(p, out1, oo);
    float rr = sqrtf((oo[0] - p.b[0]) * (oo[0] - p.b[0]) + (oo[1] + p.a[1]) * (oo[1] + p.a[1]) + (oo[2] - p.b[1]) * (oo[2] - p.b[1]));
    Check(rr < p.a[1] - 100.0f, "beyond the east edge sinks");
    // Poles: z = 0 and z = height are single points at t = 1.
    float s1[3] = {0, 0, 2048}, s2[3] = {3000, 0, 2048}, o1[3], o2[3];
    Globe::Bend(p, s1, o1);
    Globe::Bend(p, s2, o2);
    Check(Near(o1[0], o2[0], 0.05f) && Near(o1[2], o2[2], 0.05f), "north pole");
    // A ray past the planet: no hit, the closest point.
    float o[3] = {100, 4000, 1024}, d[3] = {1, 0, 0}, flat[2];
    bool hit = true;
    Check(Globe::Unbend(p, o, d, 5632, 2048, flat, &hit) && !hit, "miss gives the closest point");
}

static void TestSettings() {
    Globe::Settings s;
    std::vector<Globe::Problem> pr;
    Globe::ParseFile("globe = { default = yes flat_height = 600 globe_height = 2000.5 north = 70 south = -60 }", "t", &s, &pr);
    Check(pr.empty() && s.present && s.defaultOn && s.flatHeight == 600 && s.globeHeight == 2000.5f && s.north == 70 && s.south == -60,
          "settings read");
    Globe::ParseFile("globe = { default = no wobble = 3 north = 100 }\nfoo = 1", "t", &s, &pr);
    Check(!s.defaultOn && pr.size() == 3 && s.north == 70, "settings problems, later keys replace");
}

static void TestPatch() {
    int n = 0;
    std::string out = Globe::PatchMapShader(kShader, &n);
    Check(n == 3, "three replacements (two mul, one dot)");
    Check(out.find("pos = mul( ViewProjectionMatrix, HL_Globe( pos ) );") != std::string::npos, "mul wrapped");
    Check(out.find("mul( ViewProjectionMatrix, HL_Globe( float4( Get( a, b ), 1.0f ) ) )") != std::string::npos, "nested parens");
    Check(out.find("dot( HL_Globe( vDistortedPos ), float4( GetMatrixData") != std::string::npos, "depth bias bent");
    Check(Count(out, "HL_Globe( pos )") == 1, "shadow and pixel blocks untouched");
    Check(Count(out, "#define HL_GLOBE_DEFINED") == 1, "helper once, in the patched block");
    int again = 0;
    Check(Globe::PatchMapShader(out, &again).empty(), "no second patch");
    Check(Globe::PatchMapShader("PixelShader = { MainCode P [[ x = mul( ViewProjectionMatrix, y ); ]] }", &again).empty(),
          "nothing to do -> empty");
}

static void TestRealFiles(const std::string& dir) {
    const char* const files[] = {"pdxmap", "pdxwater", "border", "river", "tree", "mapname", "maparrow",
                                 "strait", "traderoute", "pdxmesh", "particle"};
    for (const char* f : files) {
        std::string text;
        if (!ReadFile(dir + "/" + f + ".shader", &text)) {
            printf("cannot read %s.shader\n", f);
            ++g_failed;
            continue;
        }
        int n = 0;
        std::string out = Globe::PatchMapShader(text, &n);
        printf("%-12s %d replacement(s)\n", f, n);
        Check(n > 0 && !out.empty(), f);
        Check(Count(out, "HL_Globe(") >= (size_t)n, "every replacement calls HL_Globe");
    }
}

static int TestCompile(const std::string& dir) {
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile compile = m ? (pD3DCompile)GetProcAddress(m, "D3DCompile") : nullptr;
    if (!compile) return printf("no d3dcompiler_47.dll\n"), 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*.scache").c_str(), &fd);
    int compiled = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        std::string src;
        if (!ReadFile(dir + "\\" + fd.cFileName, &src) || src.find("mul( ViewProjectionMatrix") == std::string::npos ||
            src.find("HL_") != std::string::npos)
            continue;
        size_t mainAt = src.rfind(" main(");
        if (mainAt == std::string::npos) continue;
        mainAt = src.rfind('\n', mainAt);
        int n = 0;
        std::string out = src.substr(0, mainAt) + Globe::HlslCode() + Globe::PatchVertexCode(src.substr(mainAt), &n);
        ID3DBlob *code = nullptr, *msg = nullptr;
        HRESULT hr = compile(out.data(), out.size(), fd.cFileName, nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, &msg);
        if (FAILED(hr)) {
            printf("FAIL: %s does not compile:\n%s\n", fd.cFileName, msg ? (const char*)msg->GetBufferPointer() : "");
            ++g_failed;
        } else {
            ++compiled;
        }
        ++g_checks;
        if (code) code->Release();
        if (msg) msg->Release();
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    printf("%d cached vertex shader(s) compiled with the bend\n", compiled);
    return compiled;
}

int main(int argc, char** argv) {
    TestMaths();
    TestSettings();
    TestPatch();
    if (argc >= 2) TestRealFiles(argv[1]);
    if (argc >= 3) TestCompile(argv[2]);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
