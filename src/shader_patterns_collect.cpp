// Shader patterns, collecting and checking the files (NOTES.md M11, M15): the enabled mods come from
// the shared mod list (mod_list.h: dlc_load.json and each mod's descriptor), each mod's shaders from
// its gfx/FX/hoi4_loader_patterns folders. Mod shaders are compile-checked before the game's shader
// files are patched.
#include <windows.h>

#include <map>

#include "loader.h"
#include "mod_list.h"
#include "shader_patterns.h"

namespace ShaderPatterns {

namespace {

bool g_collected = false, g_validated = false;
std::vector<Pattern> g_fill, g_line;
bool g_patched[2] = {false, false};
std::string g_usedBy;  // empty: no enabled mod uses attention areas

// The marker a mod ships to have the map shaders patched for the built-in shaders (NOTES.md M15).
const wchar_t kMarker[] = L"\\gfx\\FX\\hoi4_loader_patterns\\patch_map_shaders.txt";

using ModList::Narrow;

// One kind's files in one mod folder; a later mod's file of the same name replaces an earlier one.
// Returns how many files were taken.
int Scan(const std::wstring& modDir, const wchar_t* sub, const char* kind, std::map<std::string, Pattern>* into) {
    std::wstring dir = modDir + L"\\gfx\\FX\\hoi4_loader_patterns\\" + sub + L"\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.hlsl").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int taken = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring file = fd.cFileName;
        std::string name = Narrow(file.substr(0, file.size() - 5));
        std::string code;
        if (!ValidName(name)) {
            Log("shader_patterns: %s pattern file '%s' skipped: the name must be letters, digits and _", kind, Narrow(file).c_str());
            continue;
        }
        if (!ModList::ReadWholeFile(dir + file, &code)) {
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
        p.fromMod = true;
        auto it = into->find(name);
        if (it != into->end()) Log("shader_patterns: %s pattern '%s' of %s replaces the one of %s", kind, name.c_str(), p.source.c_str(), it->second.source.c_str());
        (*into)[name] = std::move(p);
        ++taken;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return taken;
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

const Pattern* BuiltinNamed(Kind kind, const std::string& name) {
    for (const Pattern& b : Builtins(kind))
        if (b.name == name) return &b;
    return nullptr;
}

// A rejected mod shader: the built-in of that name comes back if there is one, else it is left out.
void Reject(Kind kind, Pattern* p, const std::string& why) {
    const Pattern* b = BuiltinNamed(kind, p->name);
    Log("shader_patterns: %s shader '%s' (%s) failed the compile check, %s: %s", kind == Kind::Fill ? "fill" : "line",
        p->name.c_str(), p->source.c_str(), b ? "the built-in of that name is used" : "left out (areas using it fall back)",
        why.c_str());
    if (b) {
        *p = *b;
    } else {
        p->usable = false;
    }
}

}  // namespace

void Collect() {
    if (g_collected) return;
    g_collected = true;
    std::wstring user = LoaderOutputPath(L"");
    while (!user.empty() && user.back() == L'\\') user.pop_back();
    std::vector<ModList::EnabledMod> mods;
    if (!ModList::Enabled(user, &mods)) Log("shader_patterns: dlc_load.json not found in the game's user folder: built-in shaders only");
    std::map<std::string, Pattern> fill, line;
    for (const Pattern& p : Builtins(Kind::Fill)) fill[p.name] = p;  // mods may replace them
    for (const Pattern& p : Builtins(Kind::Line)) line[p.name] = p;
    for (const ModList::EnabledMod& m : mods) {
        if (m.status != ModList::EnabledMod::Status::Ok) continue;
        int files = Scan(m.root, L"fill", "fill", &fill) + Scan(m.root, L"line", "line", &line);
        if (!g_usedBy.empty()) continue;
        if (files) g_usedBy = "mod " + m.label() + " ships attention area shaders";
        else if (GetFileAttributesW((m.root + kMarker).c_str()) != INVALID_FILE_ATTRIBUTES)
            g_usedBy = "mod " + m.label() + " ships gfx/FX/hoi4_loader_patterns/patch_map_shaders.txt";
    }
    Finish(fill, (size_t)kMaxFill, "fill", &g_fill);
    Finish(line, (size_t)kMaxLine, "line", &g_line);
}

bool UsedByMods(std::string* why) {
    if (why) *why = g_usedBy;
    return !g_usedBy.empty();
}

void Validate() {
    if (g_validated) return;
    g_validated = true;
    bool anyMod = false;
    for (const std::vector<Pattern>* list : {&g_fill, &g_line})
        for (const Pattern& p : *list) anyMod = anyMod || p.fromMod;
    if (!anyMod) return;
    ULONGLONG t0 = GetTickCount64();
    // The built-ins alone first: if they fail, the check itself does not fit this game, so no mod
    // shader can be trusted.
    std::vector<Pattern> fills, lines;
    for (const Pattern& p : g_fill) {
        fills.push_back(p);
        fills.back().usable = !p.fromMod;
    }
    for (const Pattern& p : g_line) {
        lines.push_back(p);
        lines.back().usable = !p.fromMod;
    }
    std::string errors;
    bool available = false;
    bool baseOk = CompileCheck(CheckSourceFill(fills), &errors, &available) &&
                  CompileCheck(CheckSourceBorder(fills, lines), &errors, &available);
    if (!baseOk) {
        Log("shader_patterns: %s; every mod shader is left out (built-ins only)%s%s",
            available ? "the built-in shaders fail the compile check, so the check does not fit this game"
                      : "no shader compiler (d3dcompiler_47.dll) to check mod shaders with",
            errors.empty() ? "" : ": ", errors.c_str());
        for (size_t i = 0; i < g_fill.size(); ++i)
            if (g_fill[i].fromMod) Reject(Kind::Fill, &g_fill[i], "not checked");
        for (size_t i = 0; i < g_line.size(); ++i)
            if (g_line[i].fromMod) Reject(Kind::Line, &g_line[i], "not checked");
        return;
    }
    // Then each mod shader on top of what passed so far, in id order (fills, then lines): a failure
    // is its own (a syntax error, an undeclared name, or a helper name another shader already uses).
    int checked = 0, failed = 0;
    for (size_t i = 0; i < g_fill.size(); ++i) {
        if (!g_fill[i].fromMod) continue;
        ++checked;
        fills[i].usable = true;
        if (!CompileCheck(CheckSourceFill(fills), &errors, &available) || !CompileCheck(CheckSourceBorder(fills, lines), &errors, &available)) {
            fills[i].usable = false;
            ++failed;
            Reject(Kind::Fill, &g_fill[i], errors);
            if (g_fill[i].usable) fills[i] = g_fill[i];  // the built-in came back: it is in the next checks
        }
    }
    for (size_t i = 0; i < g_line.size(); ++i) {
        if (!g_line[i].fromMod) continue;
        ++checked;
        lines[i].usable = true;
        if (!CompileCheck(CheckSourceBorder(fills, lines), &errors, &available)) {
            lines[i].usable = false;
            ++failed;
            Reject(Kind::Line, &g_line[i], errors);
            if (g_line[i].usable) lines[i] = g_line[i];
        }
    }
    Log("shader_patterns: compile check of %d mod shader(s): %d passed, %d failed (%llu ms)", checked, checked - failed, failed,
        GetTickCount64() - t0);
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

bool Usable(Kind kind, int id) {
    const std::vector<Pattern>& list = Patterns(kind);
    return id >= 1 && id <= (int)list.size() && list[(size_t)id - 1].usable;
}

}  // namespace ShaderPatterns
