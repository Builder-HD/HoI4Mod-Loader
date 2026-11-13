// Shader patterns, collecting the files (NOTES.md M11): the enabled mods come from dlc_load.json in
// the game's user folder (the launcher writes it before the game starts), each mod's folder from its
// .mod descriptor's `path`.
#include <windows.h>

#include <algorithm>
#include <map>

#include "loader.h"
#include "shader_patterns.h"

namespace ShaderPatterns {

namespace {

bool g_collected = false;
std::vector<Pattern> g_fill, g_line;
bool g_patched[2] = {false, false};

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)std::max(n, 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)std::max(n, 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool ReadText(const std::wstring& path, std::string* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (16 << 20);
    if (ok) {
        out->resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = size.QuadPart == 0 || (ReadFile(h, &(*out)[0], (DWORD)size.QuadPart, &got, nullptr) && got == size.QuadPart);
    }
    CloseHandle(h);
    if (ok && out->size() >= 3 && (unsigned char)(*out)[0] == 0xEF && (unsigned char)(*out)[1] == 0xBB &&
        (unsigned char)(*out)[2] == 0xBF)
        out->erase(0, 3);
    return ok;
}

bool IsAbsolute(const std::wstring& p) { return p.size() > 2 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\')); }

// One kind's files in one mod folder; a later mod's file of the same name replaces an earlier one.
void Scan(const std::wstring& modDir, const wchar_t* sub, const char* kind, std::map<std::string, Pattern>* into) {
    std::wstring dir = modDir + L"\\gfx\\FX\\hoi4_loader_patterns\\" + sub + L"\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.hlsl").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring file = fd.cFileName;
        std::string name = Narrow(file.substr(0, file.size() - 5));
        std::string code;
        if (!ValidName(name)) {
            Log("shader_patterns: %s pattern file '%s' skipped: the name must be letters, digits and _", kind, Narrow(file).c_str());
            continue;
        }
        if (!ReadText(dir + file, &code)) {
            Log("shader_patterns: %s pattern '%s' could not be read", kind, name.c_str());
            continue;
        }
        if (code.find("[[") != std::string::npos || code.find("]]") != std::string::npos) {
            Log("shader_patterns: %s pattern '%s' skipped: it contains [[ or ]], which would end the game's code block", kind,
                name.c_str());
            continue;
        }
        std::vector<Var> vars;
        std::string why;
        if (!ParseVariables(code, &vars, &why)) {
            Log("shader_patterns: %s pattern '%s' skipped: its hoi4_loader_variables line is malformed: %s", kind, name.c_str(), why.c_str());
            continue;
        }
        Pattern p{name, code, Narrow(dir + file), vars};
        auto it = into->find(name);
        if (it != into->end()) Log("shader_patterns: %s pattern '%s' of %s replaces the one of %s", kind, name.c_str(), p.source.c_str(), it->second.source.c_str());
        (*into)[name] = std::move(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void Finish(std::map<std::string, Pattern>& found, size_t max, const char* kind, std::vector<Pattern>* out) {
    for (auto& kv : found) {  // name order: the ids every machine with these mods agrees on
        if (out->size() >= max) {
            Log("shader_patterns: more than %zu %s patterns: '%s' and later names are left out", max, kind, kv.first.c_str());
            break;
        }
        out->push_back(std::move(kv.second));
    }
    for (size_t i = 0; i < out->size(); ++i) {
        std::string vars;
        for (const Var& v : (*out)[i].vars) vars += (vars.empty() ? "" : ", ") + v.name;
        Log("shader_patterns: %s pattern %zu = '%s' (%s)%s%s", kind, i + 1, (*out)[i].name.c_str(), (*out)[i].source.c_str(),
            vars.empty() ? "" : "; variables: ", vars.c_str());
    }
}

}  // namespace

void Collect() {
    if (g_collected) return;
    g_collected = true;
    std::wstring user = LoaderOutputPath(L"");
    std::string json;
    if (user.empty() || !ReadText(user + L"dlc_load.json", &json)) {
        Log("shader_patterns: dlc_load.json not found in the game's user folder: built-in shaders only");
        json.clear();
    }
    std::map<std::string, Pattern> fill, line;
    for (const Pattern& p : Builtins(Kind::Fill)) fill[p.name] = p;  // mods may replace them
    for (const Pattern& p : Builtins(Kind::Line)) line[p.name] = p;
    for (const std::string& mod : EnabledMods(json)) {
        std::wstring descPath = Wide(mod);
        std::replace(descPath.begin(), descPath.end(), L'/', L'\\');
        if (!IsAbsolute(descPath)) descPath = user + descPath;
        std::string desc;
        if (!ReadText(descPath, &desc)) continue;
        std::wstring dir = Wide(ModPath(desc));
        if (dir.empty()) continue;
        std::replace(dir.begin(), dir.end(), L'/', L'\\');
        if (!IsAbsolute(dir)) dir = user + dir;
        while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
        Scan(dir, L"fill", "fill", &fill);
        Scan(dir, L"line", "line", &line);
    }
    Finish(fill, (size_t)kMaxFill, "fill", &g_fill);
    Finish(line, (size_t)kMaxLine, "line", &g_line);
}

const std::vector<Pattern>& Patterns(Kind kind) { return kind == Kind::Fill ? g_fill : g_line; }

void SetPatched(Kind kind, bool patched) { g_patched[kind == Kind::Fill ? 0 : 1] = patched; }
bool Patched(Kind kind) { return g_patched[kind == Kind::Fill ? 0 : 1]; }

int Id(Kind kind, const std::string& name) {
    const std::vector<Pattern>& list = Patterns(kind);
    for (size_t i = 0; i < list.size(); ++i)
        if (_stricmp(list[i].name.c_str(), name.c_str()) == 0) return (int)i + 1;
    return 0;
}

}  // namespace ShaderPatterns
