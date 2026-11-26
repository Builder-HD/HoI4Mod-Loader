// globe (NOTES.md Part S): the pure part. Optional arguments:
//   globe_test <game gfx/FX folder>            also patches the real map shaders and checks the shape
//   globe_test <gfx/FX folder> <vs_5_0 cache>  also compiles every cached vertex shader that uses the
//                                              view-projection matrix with the bend spliced in (vs_5_0)
#include <windows.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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

// One planet (the whole map, or the settings' planets) at fold t, looking at (lx, lz).
static bool MakeOne(const Globe::Settings& s, float w, float h, float t, float lx, float lz, Globe::Params* p, int current = 0,
                    float overview = 0.0f) {
    std::vector<Globe::Planet> planets = Globe::ResolvePlanets(s, w, h, nullptr);
    Globe::View v;
    v.current = current, v.t = t, v.p = overview, v.lookX = lx, v.lookZ = lz;
    if (!planets.empty()) v.focusX = planets[current].ox, v.focusY = planets[current].oy;
    return Globe::MakeParams(s, planets, w, v, p);
}

static void TestMaths() {
    Globe::Settings s;
    s.present = true;
    {
        Globe::SphereState st;
        Check(!Globe::WantSphere(s, 300, true, &st), "below flat_height: flat even with the edge in view");
        Check(Globe::WantSphere(s, 2600, false, &st), "above globe_height: a sphere");
        st = Globe::SphereState();
        Check(!Globe::WantSphere(s, 1000, false, &st), "edge out of view: flat");
        Check(Globe::WantSphere(s, 1000, true, &st) && st.enterHeight == 1000, "edge in view: a sphere");
        Check(Globe::WantSphere(s, 900, false, &st), "zoomed in a little: stays a sphere");
        Check(!Globe::WantSphere(s, 800, false, &st), "below 85% of where it became one: flat");
    }
    Globe::Params p;
    Check(!MakeOne(s, 5632, 2048, 0.0f, 100, 100, &p) && p.t() == 0.0f, "t = 0: flat");
    float in[3] = {1234, 5, 777}, out[3];
    Globe::Bend(p, 0, in, out);
    Check(out[0] == in[0] && out[1] == in[1] && out[2] == in[2], "flat bend is the identity");
    for (float t : {1.0f, 0.4f, 0.05f}) {
        float lx = 3000, lz = 1500;
        Check(MakeOne(s, 5632, 2048, t, lx, lz, &p), "params");
        Check(p.count() == 1 && p.kind(0) == 1.0f, "one planet, the whole map, drawn");
        float l[3] = {lx, 0, lz}, lo[3];
        Globe::Bend(p, 0, l, lo);
        Check(Near(lo[0], lx, 0.01f) && Near(lo[1], 0, 0.01f) && Near(lo[2], lz, 0.01f), "the look-at point stays");
        // Near the look-at point: 1 unit east -> cos(latitude) east (meridians meet at the poles).
        float e[3] = {lx + 1, 0, lz}, eo[3];
        Globe::Bend(p, 0, e, eo);
        Check(Near(eo[0] - lx, p.look(0)[2], 0.01f) && Near(eo[1], 0, 0.01f), "one unit east");
        // Round trip: a straight-down ray onto a bent point unbends to the flat point.
        const float pts[][2] = {{3200, 1600}, {2600, 1200}, {3500, 1900}, {2900, 1000}};
        for (const auto& q : pts) {
            float f[3] = {q[0], 0, q[1]}, b[3];
            Globe::Bend(p, 0, f, b);
            float o[3] = {lx, 4000, lz};
            float d[3] = {b[0] - o[0], b[1] - o[1], b[2] - o[2]};
            float flat[2];
            bool hit = false;
            Check(Globe::Unbend(p, 0, o, d, flat, &hit) && hit, "unbend hits");
            Check(Near(flat[0], q[0], 0.5f) && Near(flat[1], q[1], 0.5f), "unbend round trip");
            float dist = 0;
            Check(Globe::RayPlanet(p, o, d, &dist) == 0 && dist > 0, "the ray finds the planet");
        }
    }
    // Whole planet: the map wraps (x = 5620 and x = 12 are 24 units apart across the seam); what lies
    // beyond the map's edges is on no planet (the shader drops it).
    MakeOne(s, 5632, 2048, 1.0f, 100, 1024, &p);
    float a[3] = {5620, 0, 1024}, b2[3] = {12, 0, 1024}, ao[3], bo[3];
    Globe::Bend(p, 0, a, ao);
    Globe::Bend(p, 0, b2, bo);
    float gap = sqrtf((ao[0] - bo[0]) * (ao[0] - bo[0]) + (ao[1] - bo[1]) * (ao[1] - bo[1]) + (ao[2] - bo[2]) * (ao[2] - bo[2]));
    Check(Near(gap, 24.0f, 0.5f), "wrap across the seam");
    std::vector<Globe::Planet> one = Globe::ResolvePlanets(s, 5632, 2048, nullptr);
    Check(Globe::PlanetAt(one, 5700, 1024) == -1 && Globe::PlanetAt(one, 100, 2100) == -1 && Globe::PlanetAt(one, 100, 1024) == 0,
          "beyond the map's edges: no planet");
    // Poles: z = 0 and z = height are single points at t = 1.
    float s1[3] = {0, 0, 2048}, s2[3] = {3000, 0, 2048}, o1[3], o2[3];
    Globe::Bend(p, 0, s1, o1);
    Globe::Bend(p, 0, s2, o2);
    Check(Near(o1[0], o2[0], 0.05f) && Near(o1[2], o2[2], 0.05f), "north pole");
    // A ray past the planet: no hit, the closest point.
    float o[3] = {100, 4000, 1024}, d[3] = {1, 0, 0}, flat[2];
    bool hit = true;
    Check(Globe::Unbend(p, 0, o, d, flat, &hit) && !hit, "miss gives the closest point");
    Check(Globe::RayPlanet(p, o, d, nullptr) == -1, "the ray finds no planet");
}

// Two Earths stacked as the planets test map makes them (5632 x 4352, a gap of 256 rows).
static void TestPlanets() {
    Globe::Settings s;
    std::vector<Globe::Problem> pr;
    Globe::ParseFile("globe = { north = 82 south = -60 overview_height = 2600 overview_full_height = 3000\n"
                     " planet = { name = A x = 0 y = 0 width = 5632 height = 2048 overview = { x = -0.7 y = 0 size = 1 } }\n"
                     " planet = { name = B x = 0 y = 2304 width = 5632 height = 2048 overview = { x = 0.7 y = 0 size = 1 } "
                     "image = \"gfx/x/b.png\" polar_cap_north = 85 }\n"
                     " planet = { width = 10 }\n"
                     " planet = { name = C width = 100 }\n}",
                     "t", &s, &pr);
    Check(s.planets.size() == 3 && pr.size() == 2, "planets read (a nameless one refused, half a rectangle reported)");
    Check(s.planets[1].image == "gfx/x/b.png" && s.planets[1].setCapNorth && s.planets[0].hasOverview && s.planets[0].overviewX == -0.7f,
          "planet keys");
    Globe::ParseFile("globe = { planet = { name = C x = 0 y = 0 width = 1000 height = 500 } }", "t", &s, &pr);
    Check(s.planets.size() == 3 && s.planets[2].width == 1000, "a later planet of the same name replaces it");
    s.planets.pop_back();
    std::vector<std::string> probs;
    std::vector<Globe::Planet> pl = Globe::ResolvePlanets(s, 5632, 4352, &probs);
    Check(pl.size() == 2 && probs.empty() && pl[1].z0 == 2304 && pl[1].capNorth == 85 && pl[0].capNorth == 90 && pl[1].north == 82,
          "planets resolved (keys not given take the globe's)");
    Check(Globe::PlanetAt(pl, 1000, 3000) == 1 && Globe::PlanetAt(pl, 1000, 1000) == 0 && Globe::PlanetAt(pl, 1000, 2200) == -1,
          "the gap is on no planet");
    // Globe of B: only B drawn, at the camera.
    Globe::View v;
    v.current = 1, v.t = 1, v.lookX = 2800, v.lookZ = 3300, v.focusX = 0.7f;
    Globe::Params p;
    Check(Globe::MakeParams(s, pl, 5632, v, &p) && p.kind(0) == 0.0f && p.kind(1) == 1.0f && p.current() == 1, "globe: the current planet only");
    float rs = 5632.0f / (2.0f * 3.14159265f);
    Check(Near(p.centre(1)[3], rs, 0.1f) && Near(p.centre(1)[0], 2800, 0.01f) && Near(p.centre(1)[1], -rs, 0.1f), "its sphere under the camera");
    float lo[3] = {2800, 0, 3300}, lb[3];
    Globe::Bend(p, 1, lo, lb);
    Check(Near(lb[0], 2800, 0.05f) && Near(lb[1], 0, 0.05f) && Near(lb[2], 3300, 0.05f), "its look-at on top");
    // Overview just begun: the same picture (the current planet as the globe), the other beside it.
    Globe::Params q;
    v.p = 0.0001f;
    Check(Globe::MakeParams(s, pl, 5632, v, &q) && q.kind(0) == 1.0f && q.kind(1) == 1.0f, "overview: every planet drawn");
    Check(Near(q.centre(1)[0], p.centre(1)[0], 1.0f) && Near(q.centre(1)[3], p.centre(1)[3], 1.0f) && q.centre(0)[0] < q.centre(1)[0] - 1.3f * rs,
          "from the globe to the overview without a jump; A to the west");
    // The whole overview: both fit the view at its full height.
    v.p = 1.0f, v.focusX = 0.0f;
    Check(Globe::MakeParams(s, pl, 5632, v, &q), "full overview");
    float halfH = 3000 * 0.41421356f, halfW = halfH * 16.0f / 9.0f;
    bool fits = true;
    for (int i = 0; i < 2; ++i)
        fits = fits && fabsf(q.centre(i)[0] - v.lookX) + q.centre(i)[3] <= halfW && fabsf(q.centre(i)[2] - v.lookZ) + q.centre(i)[3] <= halfH;
    Check(fits && q.centre(0)[3] < rs, "the layout fits the view (smaller than the globe)");
    Check(Near(q.overview(), 1.0f, 1e-6f) && q.b[2] > 0.99f, "names at full strength");
    // Picking in the overview: straight down onto A's top finds A's look-at point.
    float o[3] = {q.centre(0)[0], 3000, q.centre(0)[2]}, d[3] = {0, -1, 0}, flat[2];
    bool hit = false;
    Check(Globe::RayPlanet(q, o, d, nullptr) == 0, "the ray finds A");
    Check(Globe::Unbend(q, 0, o, d, flat, &hit) && hit && Near(flat[0], 2816, 1.0f) && flat[1] > 0 && flat[1] < 2048, "and A's map under it");
    // B with its image: shown as the image (a disc) from half way.
    pl[1].image = true;
    v.p = 0.7f;
    Globe::MakeParams(s, pl, 5632, v, &q);
    Check(q.kind(1) == 2.0f && q.kind(0) == 1.0f, "an image planet becomes its image");
    float ob[3] = {q.centre(1)[0], 3000, q.centre(1)[2]};
    Check(Globe::RayPlanet(q, ob, d, nullptr) == 1 && !Globe::Unbend(q, 1, ob, d, flat, &hit), "its disc is hit, but holds no map");
    // The space map (Part U): every planet with an orbit as its sphere in its hole there.
    Globe::Settings sp = s;
    std::vector<Globe::Problem> pp;
    Globe::ParseFile("globe = { space = { x = 0 y = 4608 width = 5376 height = 2048 } space_height = 2600\n"
                     " planet = { name = A orbit = { x = 1872 y = 5632 radius = 250 } }\n"
                     " planet = { name = B orbit = { x = 3600 y = 5632 radius = 250 } } }",
                     "t", &sp, &pp);
    Check(pp.empty() && sp.hasSpace && sp.spaceY == 4608 && sp.planets[1].hasOrbit && sp.planets[1].orbitRadius == 250,
          "space and orbits read");
    std::vector<Globe::Planet> spl = Globe::ResolvePlanets(sp, 5632, 6656, nullptr);
    Globe::View sv;
    sv.space = true, sv.lookX = 2000, sv.lookZ = 5000;
    sv.lastLook = {2816, 1000, 2816, 3300};
    Globe::Params sq;
    Check(Globe::MakeParams(sp, spl, 5632, sv, &sq) && sq.space() && sq.t() == 1.0f && sq.kind(0) == 1.0f && sq.kind(1) == 1.0f,
          "over space: both planets drawn");
    Check(sq.centre(1)[0] == 3600 && sq.centre(1)[2] == 5632 && sq.centre(1)[3] == 250 && sq.centre(1)[1] == -250 && sq.s[1] == 4608,
          "each in its orbit, its top at the map's level");
    float top[3] = {2816, 0, 3300}, tq[3];
    Globe::Bend(sq, 1, top, tq);
    Check(Near(tq[0], 3600, 0.05f) && Near(tq[1], 0, 0.05f) && Near(tq[2], 5632, 0.05f), "turned as it was last looked at");
    float so[3] = {3600, 1500, 5632}, sd[3] = {0, -1, 0}, sf[2];
    Check(Globe::RayPlanet(sq, so, sd, nullptr) == 1 && Globe::Unbend(sq, 1, so, sd, sf, &hit) && hit && Near(sf[0], 2816, 1.0f),
          "a ray onto its disc finds its province");
}

static void TestSettings() {
    Globe::Settings s;
    std::vector<Globe::Problem> pr;
    Globe::ParseFile("globe = { default = yes flat_height = 600 globe_height = 2000.5 north = 70 south = -60 transition = 0 "
                     "background = \"gfx/test/space.png\" }", "t", &s, &pr);
    Check(pr.empty() && s.present && s.defaultOn && s.flatHeight == 600 && s.globeHeight == 2000.5f && s.north == 70 && s.south == -60 &&
              s.transition == 0 && s.background == "gfx/test/space.png",
          "settings read");
    Globe::ParseFile("globe = { default = no wobble = 3 north = 100 }\nfoo = 1", "t", &s, &pr);
    {
        Globe::Settings c;
        std::vector<Globe::Problem> pc;
        Globe::ParseFile("globe = { polar_cap = 70 polar_cap_color = { 0.5 0.6 0.7 } }", "t", &c, &pc);
        Check(pc.empty() && c.polarCapNorth == 70 && c.polarCapSouth == -70 && c.polarColor[0] == 0.5f && c.polarColor[2] == 0.7f,
              "polar caps read");
        Globe::Params cp;
        MakeOne(c, 5632, 2048, 1.0f, 1000, 1024, &cp);
        Check(cp.pl[7][1] == 0.0f, "no polar seas while the map reaches both poles");
        Globe::ParseFile("globe = { north = 80 south = -60 polar_cap_north = 84 polar_cap_south = -68 "
                         "polar_sea_color = { 0.1 0.2 0.3 } }", "t", &c, &pc);
        Check(pc.empty() && c.polarSeaColor[1] == 0.2f, "polar seas read");
        MakeOne(c, 5632, 2048, 1.0f, 1000, 1024, &cp);
        float zPerDeg = 2048.0f / 140.0f;
        Check(cp.pl[7][1] == 1.0f && Near(cp.pl[4][0], 144 * zPerDeg, 0.5) && Near(cp.pl[4][1], -8 * zPerDeg, 0.5) && cp.f[2] == 0.3f,
              "polar seas and ice placed beyond the map's edges");
        Globe::ParseFile("globe = { polar_cap_north = 75 polar_cap_south = -50 }", "t", &c, &pc);
        MakeOne(c, 5632, 2048, 1.0f, 1000, 1024, &cp);
        Check(cp.pl[4][0] == 2048.0f && cp.pl[4][1] == 0.0f, "ice never over the map");
        Globe::ParseFile("globe = { polar_cap_color = { 2 0 0 } }", "t", &c, &pc);
        Check(pc.size() == 1, "bad cap colour refused");
    }
    Check(!s.defaultOn && pr.size() == 3 && s.north == 70, "settings problems, later keys replace");
}

static void TestPatch() {
    int n = 0;
    std::string out = Globe::PatchMapShader(kShader, Globe::kTerrain, &n);
    Check(n == 3, "three replacements (two mul, one dot)");
    Check(out.find("pos = mul( ViewProjectionMatrix, HL_Globe( pos ) );") != std::string::npos, "mul wrapped");
    Check(out.find("mul( ViewProjectionMatrix, HL_Globe( float4( Get( a, b ), 1.0f ) ) )") != std::string::npos, "nested parens");
    Check(out.find("dot( HL_Globe( vDistortedPos ), float4( GetMatrixData") != std::string::npos, "depth bias bent");
    Check(Count(out, "HL_Globe( pos )") == 1, "shadow and pixel blocks untouched");
    Check(Count(out, "#define HL_GLOBE_DEFINED") == 1, "helper once, in the patched block");
    int again = 0;
    Check(Globe::PatchMapShader(out, Globe::kTerrain, &again).empty(), "no second patch");
    Check(Globe::PatchMapShader("PixelShader = { MainCode P [[ x = mul( ViewProjectionMatrix, y ); ]] }", Globe::kBend, &again).empty(),
          "nothing to do -> empty");
    std::string culled = Globe::PatchMapShader(kShader, Globe::kCull, &n);
    Check(culled.find("mul( ViewProjectionMatrix, HL_GlobeCull( pos ) )") != std::string::npos &&
              culled.find("dot( HL_GlobeCull( vDistortedPos )") != std::string::npos,
          "cull variant (the depth bias uses the same position)");
    std::string surface = Globe::PatchMapShader(kShader, Globe::kBend, &n);
    Check(surface.find("mul( ViewProjectionMatrix, HL_GlobeSurface( pos ) )") != std::string::npos, "surface variant");
    std::string hidden = Globe::PatchMapShader(kShader, Globe::kHide, &n);
    Check(hidden.find("mul( ViewProjectionMatrix, HL_GlobeHide( pos ) )") != std::string::npos, "hide variant");
    std::string water = Globe::PatchMapShader(
        "PixelShader =\n{\n\tMainCode P\n\t[[\n\t\tfloat3 refractiveColor = tex2D( WaterRefraction, uv - d ).rgb;\n\t]]\n}\n", false, &n);
    Check(n == 1 && water.find("tex2D( WaterRefraction, uv - d ).rgb; refractiveColor = HL_GlobeWater( refractiveColor );") != std::string::npos,
          "water refraction replaced");
    std::string fx = Globe::PatchSharedCode(
        "PixelShader =\n{\n\tCode\n\t[[\n\tfloat CalculateDistanceFogFactor(float3 vPos)\n\t{\n\t\tfloat3 vDiff = vCamPos - vPos;\n\t}\n\t]]\n}\n");
    Check(fx.find("float3 vDiff = HL_GlobeFogDiff( vCamPos - vPos );") != std::string::npos &&
              fx.find("#define HL_GLOBE_DEFINED") < fx.find("float CalculateDistanceFogFactor"),
          "fog patched, helpers before it");
    Check(Globe::PatchSharedCode("nothing here").empty(), "fog: no anchor, no patch");
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
        bool cull = std::string(f) == "mapname" || std::string(f) == "maparrow" || std::string(f) == "strait" ||
                    std::string(f) == "traderoute" || std::string(f) == "particle";
        std::string out = Globe::PatchMapShader(text, std::string(f) == "pdxmap" ? Globe::kTerrain : std::string(f) == "pdxwater" ? Globe::kHide : cull ? Globe::kCull : Globe::kBend, &n);
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
        int mode = compiled % 5;  // every variant gets compiled across the files
        std::string out = src.substr(0, mainAt) + Globe::HlslCode() + Globe::PatchVertexCode(src.substr(mainAt), mode, &n);
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

// The pixel side as the game assembles it: the helpers in front of the fog function (standardfuncsgfx),
// the fog, the water refraction and the sky's return patched; ps_5_0.
static int TestCompilePixel(const std::string& dir) {
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile compile = m ? (pD3DCompile)GetProcAddress(m, "D3DCompile") : nullptr;
    if (!compile) return 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*.scache").c_str(), &fd);
    int compiled = 0, water = 0, sky = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        std::string s;
        if (!ReadFile(dir + "\\" + fd.cFileName, &s) || s.find("HL_") != std::string::npos) continue;
        size_t a = s.find("float CalculateDistanceFogFactor");
        size_t f = s.find("float3 vDiff = vCamPos - vPos;");
        if (a == std::string::npos || f == std::string::npos) continue;
        s.replace(f, strlen("float3 vDiff = vCamPos - vPos;"), "float3 vDiff = HL_GlobeFogDiff( vCamPos - vPos );");
        size_t line = s.rfind('\n', a);
        s.insert(line == std::string::npos ? 0 : line + 1, Globe::HlslCode());
        int n = 0;
        s = Globe::PatchPixelCode(s, &n);
        water += n;
        size_t r = s.find("return float4( color, 1.0f );");
        if (s.find("VS_OUTPUT_SKY") != std::string::npos && r != std::string::npos) {
            s.replace(r, strlen("return float4( color, 1.0f );"), "return HL_GlobeSky( color, Input.pos );");
            ++sky;
        }
        ID3DBlob *code = nullptr, *msg = nullptr;
        HRESULT hr = compile(s.data(), s.size(), fd.cFileName, nullptr, nullptr, "main", "ps_5_0", 0, 0, &code, &msg);
        ++g_checks;
        if (FAILED(hr)) {
            printf("FAIL: %s does not compile:\n%s\n", fd.cFileName, msg ? (const char*)msg->GetBufferPointer() : "");
            ++g_failed;
        } else {
            ++compiled;
        }
        if (code) code->Release();
        if (msg) msg->Release();
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    printf("%d cached pixel shader(s) compiled with the fog patch (%d water, %d sky)\n", compiled, water, sky);
    return compiled;
}

int main(int argc, char** argv) {
    TestMaths();
    TestSettings();
    TestPlanets();
    TestPatch();
    if (argc >= 2) TestRealFiles(argv[1]);
    if (argc >= 3) TestCompile(argv[2]);
    if (argc >= 4) TestCompilePixel(argv[3]);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
