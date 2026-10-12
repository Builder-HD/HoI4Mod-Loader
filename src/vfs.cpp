// Virtual file API: serves generated content in place of existing game/mod files by
// redirecting CreateFileW/A to a generated file in %TEMP%\hoi4_loader.
//
// HOI4 reads every script file through PhysFS, whose Windows backend opens files with
// CreateFileW and probes each search root in turn (see NOTES.md).
#include "loader.h"

#include <wchar.h>
#include <type_traits>
#include <vector>

#include "MinHook.h"

struct VirtualFile {
    std::wstring suffix;      // lowercase, backslashes, no leading separator
    ContentGenerator generator;
    std::wstring servedPath;  // generated file, set by VfsStart
};

// Written only before the hooks are enabled; read-only afterwards.
static std::vector<VirtualFile> g_files;
static bool g_started = false;

static wchar_t FoldChar(wchar_t c) {
    if (c == L'/') return L'\\';
    if (c >= L'A' && c <= L'Z') return (wchar_t)(c - L'A' + L'a');
    return c;
}

// True if `path` ends with `suffix` (already folded) on a path-component boundary, ignoring
// ASCII case and slash direction. Works for wide and ANSI paths (suffixes are ASCII).
template <typename Ch>
static bool EndsWithPath(const Ch* path, const std::wstring& suffix) {
    size_t len = 0;
    while (path[len]) ++len;
    if (len < suffix.size()) return false;
    const Ch* tail = path + len - suffix.size();
    auto fold = [](Ch c) { return FoldChar((wchar_t)(std::make_unsigned_t<Ch>)c); };
    if (tail != path && fold(tail[-1]) != L'\\') return false;
    for (size_t i = 0; i < suffix.size(); ++i)
        if (fold(tail[i]) != suffix[i]) return false;
    return true;
}

static bool FileExists(const wchar_t* path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }
static bool FileExists(const char* path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

template <typename Ch>
static bool ContainsAscii(const Ch* s, const char* needle) {
    for (; *s; ++s) {
        size_t i = 0;
        while (needle[i] && s[i] == (Ch)needle[i]) ++i;
        if (!needle[i]) return true;
    }
    return false;
}

bool register_virtual_file(const wchar_t* pathSuffix, ContentGenerator generator) {
    if (g_started) {
        Log("ERROR: register_virtual_file(%ls) called after start; ignored", pathSuffix ? pathSuffix : L"(null)");
        return false;
    }
    if (!pathSuffix || !generator) return false;
    std::wstring s;
    for (const wchar_t* p = pathSuffix; *p; ++p) s += FoldChar(*p);
    size_t start = s.find_first_not_of(L'\\');
    s = start == std::wstring::npos ? L"" : s.substr(start);
    // Relative suffixes only, no empty components or "..".
    if (s.empty() || s.back() == L'\\' || s.find(L':') != std::wstring::npos || s.find(L"\\\\") != std::wstring::npos ||
        s.find(L"..") != std::wstring::npos) {
        Log("ERROR: register_virtual_file: invalid path suffix '%ls'", pathSuffix);
        return false;
    }
    for (const VirtualFile& f : g_files) {
        if (f.suffix == s) {
            Log("ERROR: register_virtual_file: '%ls' is already registered", pathSuffix);
            return false;
        }
    }
    g_files.push_back({s, generator, L""});
    return true;
}

static bool WriteWholeFile(const std::wstring& path, const std::string& content) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, content.data(), (DWORD)content.size(), &written, nullptr);
    CloseHandle(f);
    return ok && written == content.size();
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileW_t g_origCreateFileW = nullptr;
static CreateFileA_t g_origCreateFileA = nullptr;
static volatile LONG g_scriptOpensLogged = 0;

// Virtual file to serve for this open, or nullptr. Only plain read opens of existing files qualify.
template <typename Ch>
static const VirtualFile* FindVirtualFile(const Ch* name, DWORD access, DWORD disposition, const char* api) {
    if (!name || disposition != OPEN_EXISTING || (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)))
        return nullptr;
    // Diagnostic: show that ordinary script files reach this API too.
    if (g_scriptOpensLogged < 3 && ContainsAscii(name, "common\\scripted_triggers\\") &&
        InterlockedIncrement(&g_scriptOpensLogged) <= 3)
        Log(sizeof(Ch) == 1 ? "Observed script open via %s: %hs" : "Observed script open via %s: %ls", api, name);
    for (const VirtualFile& f : g_files) {
        if (f.servedPath.empty() || !EndsWithPath(name, f.suffix)) continue;
        // The engine probes every search root (game dir, DLC, tweakergui_assets, mods) for each
        // file; only a copy that really exists gets replaced.
        if (!FileExists(name)) return nullptr;
        Log(sizeof(Ch) == 1 ? "Serving virtual file for %hs (via %s)" : "Serving virtual file for %ls (via %s)", name, api);
        return &f;
    }
    return nullptr;
}

static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                      DWORD disposition, DWORD flags, HANDLE templ) {
    if (const VirtualFile* f = FindVirtualFile(name, access, disposition, "CreateFileW")) name = f->servedPath.c_str();
    return g_origCreateFileW(name, access, share, sa, disposition, flags, templ);
}

// KernelBase!CreateFileA does not go through CreateFileW, so it needs its own hook.
static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                      DWORD disposition, DWORD flags, HANDLE templ) {
    if (const VirtualFile* f = FindVirtualFile(name, access, disposition, "CreateFileA"))
        return g_origCreateFileW(f->servedPath.c_str(), access, share, sa, disposition, flags, templ);
    return g_origCreateFileA(name, access, share, sa, disposition, flags, templ);
}

// Hooks the KernelBase implementation (what kernel32's export forwards to), else kernel32's.
static void* CreateApiHook(const char* name, void* detour, void** orig) {
    const wchar_t* modules[] = {L"kernelbase.dll", L"kernel32.dll"};
    for (const wchar_t* m : modules) {
        HMODULE mod = GetModuleHandleW(m);
        void* target = mod ? (void*)GetProcAddress(mod, name) : nullptr;
        if (!target) continue;
        MH_STATUS s = MH_CreateHook(target, detour, orig);
        if (s != MH_OK) {
            Log("ERROR: MH_CreateHook(%ls!%s) failed: %s", m, name, MH_StatusToString(s));
            return nullptr;
        }
        Log("Hook prepared: %ls!%s at %p", m, name, target);
        return target;
    }
    Log("ERROR: %s not found", name);
    return nullptr;
}

bool VfsStart() {
    g_started = true;
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (!n || n > MAX_PATH - 1) {
        Log("ERROR: GetTempPathW failed");
        return false;
    }
    std::wstring dir = std::wstring(tmp) + L"hoi4_loader";
    CreateDirectoryW(dir.c_str(), nullptr);

    // Generate every file exactly once. A file that fails is simply not served.
    int ready = 0;
    for (size_t i = 0; i < g_files.size(); ++i) {
        VirtualFile& f = g_files[i];
        std::string content;
        try {
            content = f.generator();
        } catch (...) {
            Log("ERROR: content generator for '%ls' threw; file not served", f.suffix.c_str());
            continue;
        }
        // e.g. %TEMP%\hoi4_loader\00_common_scripted_triggers_hoi4_loader_detect.txt
        wchar_t prefix[8];
        swprintf_s(prefix, L"%02zu_", i);
        std::wstring flat = f.suffix;
        for (wchar_t& c : flat)
            if (c == L'\\') c = L'_';
        std::wstring path = dir + L"\\" + prefix + flat;
        if (!WriteWholeFile(path, content)) {
            Log("ERROR: cannot write %ls (error %lu); file not served", path.c_str(), GetLastError());
            continue;
        }
        f.servedPath = path;
        ++ready;
        Log("Virtual file '%ls' (%zu bytes) -> %ls", f.suffix.c_str(), content.size(), path.c_str());
    }
    if (!ready) {
        Log("ERROR: no virtual files could be generated");
        return false;
    }

    void* w = CreateApiHook("CreateFileW", (void*)&Hook_CreateFileW, (void**)&g_origCreateFileW);
    void* a = w ? CreateApiHook("CreateFileA", (void*)&Hook_CreateFileA, (void**)&g_origCreateFileA) : nullptr;
    // Queue both and apply once: each apply suspends all threads, which is the slow part.
    MH_STATUS s = MH_UNKNOWN;
    if (w && a && (s = MH_QueueEnableHook(w)) == MH_OK && (s = MH_QueueEnableHook(a)) == MH_OK &&
        (s = MH_ApplyQueued()) == MH_OK) {
        Log("Virtual file hooks enabled (%d file(s))", ready);
        return true;
    }
    if (s != MH_UNKNOWN) Log("ERROR: MH_EnableHook failed: %s", MH_StatusToString(s));
    if (w) MH_RemoveHook(w);
    if (a) MH_RemoveHook(a);
    return false;
}
