// Shader patterns (NOTES.md M11): the pure text part against vanilla-shaped shader files.
// Optional: shader_patterns_test <game gfx/FX folder> also patches the real files and checks the shape.
#include <stdio.h>

#include <fstream>
#include <sstream>
#include <string>

#include "../src/shader_patterns.h"

using namespace ShaderPatterns;

static int g_checks = 0, g_failed = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        printf("FAIL: %s\n", what);
    }
}
static size_t Count(const std::string& s, const std::string& sub) {
    size_t n = 0;
    for (size_t at = s.find(sub); at != std::string::npos; at = s.find(sub, at + 1)) ++n;
    return n;
}
static bool ReadFile(const std::string& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

static const char kFxh[] =
    "ConstantBuffer( 0, 0 )\n{\n\tfloat vGlobalTime;\n};\n\nCode\n[[\n\tfloat x;\n]]\n\n"
    "PixelShader = \n{\n\tCode\n\t[[\n"
    "\tvoid secondary_color_mask( inout float3 vColor, float3 vNormal, float2 vUV, in sampler2D TexMaskSampler, inout float vBloomAlpha )\n"
    "\t{\n\t\tvColor = vColor;\n\t}\n"
    "\tvoid dominance_fx_apply() {}\n"
    "\t]]\n}\n";

static const char kBorder[] =
    "Includes = {\n\t\"standardfuncsgfx.fxh\"\n}\n"
    "VertexShader =\n{\n\tMainCode VertexShader\n\t[[\n"
    "\t\tVS_OUTPUT_BORDER main( const VS_INPUT_BORDER VertexIn )\n\t\t{\n\t\t\treturn VertexOut;\n\t\t}\n\t]]\n}\n"
    "PixelShader =\n{\n\tMainCode PixelShader\n\t[[\n"
    "\t\tfloat4 main( VS_OUTPUT_BORDER Input ) : PDX_COLOR\n\t\t{\n\t\t\treturn float4( 1, 1, 1, 1 );\n\t\t}\n\t]]\n}\n"
    "Effect border\n{\n\tVertexShader = \"VertexShader\"\n\tPixelShader = \"PixelShader\"\n}\n";

static void CheckFill(const std::string& out, size_t patterns, const char* what) {
    std::string w = what;
    Check(!out.empty(), (w + ": patched").c_str());
    Check(Count(out, "void secondary_color_mask(") == 1, (w + ": one new secondary_color_mask").c_str());
    Check(Count(out, "void HL_vanilla_secondary_color_mask(") == 1, (w + ": vanilla renamed once").c_str());
    Check(out.find("HL_vanilla_secondary_color_mask(") < out.find("void secondary_color_mask("),
          (w + ": vanilla before the new one").c_str());
    size_t lastClose = out.rfind("]]");
    Check(out.find("HL_FillDispatch") < lastClose && out.find("void secondary_color_mask(") < lastClose,
          (w + ": added code inside the last code block").c_str());
    Check(Count(out, "[[") == Count(out, "]]"), (w + ": blocks balanced").c_str());
    Check(out.find("nK <= " + std::to_string(patterns) + " )") != std::string::npos, (w + ": dispatcher bound").c_str());
}

static void CheckBorder(const std::string& out, const char* what) {
    std::string w = what;
    Check(!out.empty(), (w + ": patched").c_str());
    Check(Count(out, "float4 HL_VanillaBorderMain( VS_OUTPUT_BORDER Input )") == 1, (w + ": vanilla pixel main renamed").c_str());
    Check(Count(out, "float4 main( VS_OUTPUT_BORDER Input ) : PDX_COLOR") == 1, (w + ": one new pixel main").c_str());
    Check(out.find("VS_OUTPUT_BORDER main( const VS_INPUT_BORDER VertexIn )") != std::string::npos,
          (w + ": vertex main untouched").c_str());
    size_t effect = out.find("Effect border");
    Check(effect == std::string::npos || out.find("HL_LineDispatch") < effect, (w + ": added code before the Effect").c_str());
    Check(Count(out, "[[") == Count(out, "]]"), (w + ": blocks balanced").c_str());
}

// Developer aid: shader_patterns_test --splice <fill|line> <game shader cache .scache> <out.hlsl> [<pattern.hlsl> ...]
// applies the patch with every built-in of that kind plus the given files (name = file name) to one of
// the game's cached, fully assembled shader sources, so the result can be compiled with fxc (ps_5_0,
// entry main) before trying it in game. Use a cache entry from an unpatched run (no HL_vanilla in it).
static int Splice(int argc, char** argv) {
    std::string kind = argv[2], src;
    if (!ReadFile(argv[3], &src)) return printf("cannot read %s\n", argv[3]), 2;
    Kind k = kind == "fill" ? Kind::Fill : Kind::Line;
    std::vector<Pattern> p = Builtins(k);
    for (int i = 5; i < argc; ++i) {
        std::string code, path = argv[i];
        if (!ReadFile(path, &code)) return printf("cannot read %s\n", argv[i]), 2;
        size_t slash = path.find_last_of("/\\"), dot = path.rfind('.');
        std::vector<Var> vars;
        std::string why;
        if (!ParseVariables(code, &vars, &why)) return printf("%s: %s\n", argv[i], why.c_str()), 2;
        p.push_back({path.substr(slash + 1, dot - slash - 1), code, path, vars});
    }
    std::string out;
    if (k == Kind::Fill) {
        // In the game the .fxh's code comes before the shader's own: splice in front of its main.
        size_t mainAt = src.rfind(" main(");
        mainAt = mainAt == std::string::npos ? src.size() : src.rfind('\n', mainAt);
        std::string head = src.substr(0, mainAt), tail = src.substr(mainAt);
        out = PatchFillShader("PixelShader = { Code [[\n" + head + "\n]] }", p);
        if (out.empty()) return printf("not patched\n"), 1;
        out = out.substr(out.find("[[\n") + 3);
        out = out.substr(0, out.rfind("]]")) + tail;
    } else {
        out = PatchLineShader(src + "\n]]", p);
        if (out.empty()) return printf("not patched\n"), 1;
        out = out.substr(0, out.rfind("]]"));
    }
    std::ofstream(argv[4], std::ios::binary) << out;
    return 0;
}
int main(int argc, char** argv) {
    if (argc >= 5 && std::string(argv[1]) == "--splice") return Splice(argc, argv);
    Check(ValidName("matrix_rain") && ValidName("A1_b") && !ValidName("") && !ValidName("a-b") && !ValidName("a b"),
          "ValidName");
    Check(FillAlpha(1) == 254 && FillAlpha(32) == 223, "fill id code");
    Check(LineAlpha(3, 0.0f) == 5.0f && LineAlpha(3, 1.0f) > 5.89f && LineAlpha(3, 1.0f) < 5.91f && LineAlpha(1, 2.0f) == LineAlpha(1, 1.0f), "line code");
    {
        unsigned t = FillTexel(2, 1.0f, 0.5f, 0.0f, 1.0f);  // red 255, green 128, blue 0, alpha 63
        unsigned B = t & 255, G = t >> 8 & 255, R = t >> 16 & 255, A = t >> 24;
        Check(A == 253 && (R & 3) == 3 && (G & 3) == 3 && (B & 3) == 3 && (R & ~3u) == 252 && (G & ~3u) == 128 && B == 3, "fill texel, alpha 1");
        unsigned h = FillTexel(1, 0.0f, 0.0f, 0.0f, 0.5f);  // 0.5 * 63 = 31.5 -> 32 = 10 00 00
        Check((h >> 16 & 3) == 2 && (h >> 8 & 3) == 0 && (h & 3) == 0, "fill texel, alpha 0.5");
    }

    auto mods = EnabledMods("{\n  \"enabled_mods\": [\n    \"mod/ugc_1.mod\",\n    \"mod/local.mod\"\n  ],\n  \"disabled_dlcs\": [ \"x\" ]\n}");
    Check(mods.size() == 2 && mods[0] == "mod/ugc_1.mod" && mods[1] == "mod/local.mod", "EnabledMods");
    Check(EnabledMods("{}").empty() && EnabledMods("{\"enabled_mods\": []}").empty(), "EnabledMods empty");
    Check(ModPath("version=\"1.0\"\nname=\"x\"\npath=\"C:/a b/mod/x\"\n") == "C:/a b/mod/x", "ModPath");
    Check(ModPath("name=\"x\"\nreplace_path=\"gfx\"\n").empty(), "ModPath: replace_path is not path");

    std::vector<Pattern> two = {{"alpha", "\tfloat3 HL_Fill_alpha( float3 vColor, float2 vUV, float4 vMask, in sampler2D Mask ) { return vMask.rgb; }\n", "t"},
                                {"beta", "\tfloat3 HL_Fill_beta( float3 vColor, float2 vUV, float4 vMask, in sampler2D Mask ) { return vColor; }\n", "t"}};
    Check(PatchFillShader(kFxh, {}).empty(), "fill: no patterns, no patch");
    std::string fill = PatchFillShader(kFxh, two);
    CheckFill(fill, 2, "fill");
    Check(fill.find("else if ( nId == 2 )") != std::string::npos && fill.find("vHlOut = HL_Fill_beta( vColor, vUV, vMask, Mask );") != std::string::npos, "fill: second pattern dispatched");
    Check(fill.find("float HL_EdgeDistance(") < fill.find("HL_Fill_alpha("), "fill: edge helper before the patterns");
    Check(Builtins(Kind::Line).size() == 3 && Builtins(Kind::Fill).size() == 4, "built-ins");
    {
        std::vector<Var> v;
        std::string why;
        Check(ParseVariables("float x;", &v, &why) && v.empty(), "variables: none");
        Check(ParseVariables("\t// hoi4_loader_variables: speed 0 10 4, red 0 1 1 , reach 4 36\nfloat x;", &v, &why) && v.size() == 3 &&
                  v[0].name == "speed" && v[0].max == 10.0f && v[0].def == 4.0f && v[2].name == "reach" && v[2].def == 4.0f,
              "variables: parsed, default = min when left out");
        Check(ParseVariables("// hoi4_loader_variables: a 0 1 5", &v, &why) && v[0].def == 1.0f, "variables: default clamped");
        Check(!ParseVariables("// hoi4_loader_variables: speed 0", &v, &why), "variables: missing max refused");
        Check(!ParseVariables("// hoi4_loader_variables: Speed 0 1", &v, &why), "variables: uppercase refused");
        Check(!ParseVariables("// hoi4_loader_variables: vUV 0 1", &v, &why) && !ParseVariables("// hoi4_loader_variables: max 0 1", &v, &why),
              "variables: shader words refused");
        Check(!ParseVariables("// hoi4_loader_variables: a 1 1", &v, &why), "variables: empty range refused");
        Check(!ParseVariables("// hoi4_loader_variables: a 0 1, a 0 2", &v, &why), "variables: duplicate refused");
        Check(!ParseVariables("// hoi4_loader_variables: a 0 1, b 0 1, c 0 1, d 0 1, e 0 1", &v, &why), "variables: more than 4 refused");
        std::vector<Pattern> withVars = {{"fx", "\tfloat3 HL_Fill_fx( float3 vColor, float2 vUV, float4 vMask, in sampler2D Mask ) { return vColor * speed; }\n", "t", {{"speed", 0.0f, 10.0f, 4.0f}}}};
        std::string out = PatchFillShader(kFxh, withVars);
        Check(out.find("static float HL_VF_fx_speed = 4.0;") != std::string::npos && out.find("#define speed HL_VF_fx_speed") != std::string::npos &&
                  out.find("#undef speed") != std::string::npos && out.find("HL_VF_fx_speed = 0.0 + vMask.r * 10.0;") != std::string::npos,
              "variables: fill emission");
        std::vector<Pattern> lineVars = {{"lx", "\t\tfloat4 HL_Line_lx( float4 vColor, float3 vAreaColor, float2 vUV, float3 vPos ) { return vColor; }\n", "t",
                                          {{"a", 0.0f, 1.0f, 0.0f}, {"b", 0.0f, 1.0f, 0.0f}, {"c", 0.0f, 1.0f, 0.0f}, {"d", 2.0f, 4.0f, 2.0f}}}};
        std::string lo = PatchLineShader(kBorder, lineVars);
        Check(lo.find("HL_VL_lx_a = vAreaColor.r;") != std::string::npos && lo.find("HL_VL_lx_d = 2.0 + HL_LineAlpha * 2.0;") != std::string::npos,
              "variables: line emission (3 raw, 4th scaled)");
    }
    for (Kind k : {Kind::Line, Kind::Fill})
        for (const Pattern& p : Builtins(k)) {
            Check(ValidName(p.name) && p.code.find("[[") == std::string::npos && p.code.find("]]") == std::string::npos, "built-in shape");
            Check(p.code.find(std::string(k == Kind::Line ? "HL_Line_" : "HL_Fill_") + p.name + "(") != std::string::npos, "built-in function name");
        }
    Check(PatchFillShader("PixelShader = { Code [[ void other() {} ]] }", two).empty(), "fill: unexpected shape, no patch");

    std::vector<Pattern> one = {{"ants", "\t\tfloat4 HL_Line_ants( float4 vColor, float3 vAreaColor, float2 vUV, float3 vPos ) { return vColor; }\n", "t"}};
    Check(PatchLineShader(kBorder, {}).empty(), "border: no patterns, no patch");
    std::string border = PatchLineShader(kBorder, one);
    CheckBorder(border, "border");
    Check(PatchLineShader("PixelShader = { MainCode PixelShader [[ float4 main() { } ]] }", one).empty(),
          "border: unexpected shape, no patch");

    if (argc > 1) {  // the real files
        std::string dir = argv[1], fxh, shader;
        if (ReadFile(dir + "/standardfuncsgfx.fxh", &fxh)) CheckFill(PatchFillShader(fxh, two), 2, "real standardfuncsgfx.fxh");
        else Check(false, "read standardfuncsgfx.fxh");
        if (ReadFile(dir + "/border.shader", &shader)) CheckBorder(PatchLineShader(shader, one), "real border.shader");
        else Check(false, "read border.shader");
    }

    printf("shader_patterns_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
