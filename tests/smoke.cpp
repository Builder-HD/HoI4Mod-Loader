// Smoke test for the version.dll proxy. Built as "hoi4.exe" and linked against version.lib so the
// loader picks up the proxy from this exe's folder, exactly like the game does.
// Usage: hoi4.exe engine|file
//   engine: expect the engine_probe flag to be set (supported hash + signatures present)
//   file:   expect the file tier only (engine_probe flag cleared)
#include <windows.h>
#include <stdio.h>
#include <string>

#ifdef WITH_FAKE_PHYSFS
extern "C" void fake_openRead();
#endif

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) printf("  ok   " __VA_ARGS__); else { printf("  FAIL " __VA_ARGS__); ++g_fail; } printf("\n"); } while (0)

static std::string ReadW(const wchar_t* p) {
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "<open failed>";
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    std::string s((size_t)sz.QuadPart, '\0'); DWORD got = 0;
    ReadFile(h, s.data(), (DWORD)s.size(), &got, nullptr); CloseHandle(h); s.resize(got); return s;
}
static std::string ReadA(const char* p) {
    HANDLE h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "<open failed>";
    char buf[4096]; DWORD got = 0; ReadFile(h, buf, sizeof buf, &got, nullptr); CloseHandle(h); return std::string(buf, got);
}
static std::string ReadCrt(const wchar_t* p) {
    FILE* f = nullptr; if (_wfopen_s(&f, p, L"rb") || !f) return "<open failed>";
    char buf[4096]; size_t got = fread(buf, 1, sizeof buf, f); fclose(f); return std::string(buf, got);
}
static void Write(const std::wstring& p, const char* s) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w; WriteFile(h, s, (DWORD)strlen(s), &w, nullptr); CloseHandle(h);
}
static std::string Narrow(const std::wstring& w) {
    std::string s; for (wchar_t c : w) s += (char)c; return s;  // test paths are ASCII
}

int main(int argc, char** argv) {
    bool expectEngine = argc > 1 && strcmp(argv[1], "engine") == 0;
    printf("mode: expect %s\n", expectEngine ? "engine tier active" : "file tier only");
#ifdef WITH_FAKE_PHYSFS
    printf("  (fake PhysFS code at %p)\n", (void*)&fake_openRead);
#endif

    // 1. Proxy is the module that got loaded, and it forwards correctly.
    wchar_t mod[MAX_PATH] = L"", exe[MAX_PATH] = L"";
    GetModuleFileNameW(GetModuleHandleW(L"version.dll"), mod, MAX_PATH);
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe); dir.resize(dir.find_last_of(L'\\'));
    CHECK(_wcsnicmp(mod, dir.c_str(), dir.size()) == 0, "proxy loaded from exe folder");
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring k32 = std::wstring(sys) + L"\\kernel32.dll";
    DWORD handle = 0, sz = GetFileVersionInfoSizeW(k32.c_str(), &handle);
    std::string blob(sz, '\0');
    VS_FIXEDFILEINFO* ffi = nullptr; UINT len = 0;
    BOOL ok = sz && GetFileVersionInfoW(k32.c_str(), 0, sz, blob.data()) && VerQueryValueW(blob.data(), L"\\", (void**)&ffi, &len);
    CHECK(ok && ffi && ffi->dwSignature == 0xFEEF04BD, "version.dll exports forwarded");

    // 2. Fake mod with the two stubs plus an unrelated file.
    std::wstring m = dir + L"\\fake_mod";
    CreateDirectoryW(m.c_str(), nullptr); CreateDirectoryW((m + L"\\common").c_str(), nullptr);
    CreateDirectoryW((m + L"\\common\\scripted_triggers").c_str(), nullptr);
    CreateDirectoryW((m + L"\\common\\scripted_effects").c_str(), nullptr);
    std::wstring trig = m + L"\\common\\scripted_triggers\\hoi4_loader_detect.txt";
    std::wstring eff = m + L"\\common\\scripted_effects\\hoi4_loader_detect.txt";
    std::wstring other = m + L"\\common\\scripted_triggers\\other.txt";
    Write(trig, "hoi4_loader_present = { always = no }\n");
    Write(eff, "hoi4_loader_init = { }\n");
    Write(other, "other_trigger = { always = no }\n");

    auto has = [](const std::string& s, const char* x) { return s.find(x) != std::string::npos; };
    std::string fwd = Narrow(trig);
    for (char& c : fwd) if (c == '\\') c = '/';
    std::wstring upper = m + L"/COMMON/Scripted_Triggers/HOI4_Loader_Detect.TXT";
    std::string a = ReadW(trig.c_str()), b = ReadA(fwd.c_str()), c = ReadCrt(upper.c_str()), d = ReadCrt(eff.c_str());

    // File tier runs on every build.
    CHECK(has(a, "always = yes"), "CreateFileW trigger replaced");
    CHECK(has(b, "always = yes"), "CreateFileA (forward slashes) trigger replaced");
    CHECK(has(c, "always = yes"), "CRT _wfopen (mixed case) trigger replaced");
    CHECK(has(d, "set_global_flag = hoi4_loader_present") && has(d, "set_variable = { global.hoi4_loader_version = 2 }"),
          "effect replaced (presence flag + version 2)");
    if (expectEngine)
        CHECK(has(d, "\tset_global_flag = hoi4_loader_plugin_engine_probe"), "effect SETS hoi4_loader_plugin_engine_probe");
    else
        CHECK(has(d, "\tclr_global_flag = hoi4_loader_plugin_engine_probe") && !has(d, "set_global_flag = hoi4_loader_plugin_engine_probe"),
              "effect CLEARS hoi4_loader_plugin_engine_probe (and never sets it)");
    CHECK(ReadW(other.c_str()) == "other_trigger = { always = no }\n", "unrelated file untouched");
    std::wstring missing = dir + L"\\no_such_root\\common\\scripted_triggers\\hoi4_loader_detect.txt";
    CHECK(ReadW(missing.c_str()) == "<open failed>" && GetLastError() == ERROR_PATH_NOT_FOUND,
          "nonexistent stub path not redirected (still fails)");

    // Feature guard triggers: lazy virtual files, generated on first read.
    std::wstring guardProbe = m + L"\\common\\scripted_triggers\\hoi4_loader_feature_engine_probe.txt";
    std::wstring guardRegions = m + L"\\common\\scripted_triggers\\hoi4_loader_feature_region_names.txt";
    Write(guardProbe, "hoi4_loader_feature_engine_probe = { always = no }\n");
    Write(guardRegions, "hoi4_loader_feature_region_names = { always = no }\n");
    std::string gp = ReadW(guardProbe.c_str()), gr = ReadW(guardRegions.c_str());
    if (expectEngine)
        CHECK(has(gp, "hoi4_loader_feature_engine_probe = { hidden_trigger = { always = yes } }"),
              "guard engine_probe = yes (active, no native commands)");
    else
        CHECK(has(gp, "hoi4_loader_feature_engine_probe = { hidden_trigger = { always = no } }") && has(gp, "Served by hoi4_loader"),
              "guard engine_probe = no (served, feature inactive)");
    CHECK(has(gr, "hoi4_loader_feature_region_names = { hidden_trigger = { always = no } }") && has(gr, "Served by hoi4_loader"),
          "guard region_names = no (served; feature cannot start without the game)");
    std::wstring guardLoaded = m + L"\\common\\scripted_triggers\\hoi4_loader_feature_game_loaded.txt";
    Write(guardLoaded, "hoi4_loader_feature_game_loaded = { always = no }\n");
    std::string gl = ReadW(guardLoaded.c_str());
    CHECK(has(gl, "hoi4_loader_feature_game_loaded = { hidden_trigger = { always = no } }") && has(gl, "Served by hoi4_loader"),
          "guard game_loaded = no (served; feature cannot start without the game)");

    // 3. Writes to the stub path must go to the real file, never the generated one.
    Write(trig, "hoi4_loader_present = { always = no } # rewritten\n");
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::string r = ReadW((std::wstring(tmp) + L"hoi4_loader\\00_common_scripted_triggers_hoi4_loader_detect.txt").c_str());
    CHECK(has(r, "always = yes") && !has(r, "rewritten"), "write-open not redirected (generated file intact)");

    printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
