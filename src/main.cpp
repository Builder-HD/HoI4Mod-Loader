// hoi4_loader: version.dll proxy for Hearts of Iron IV.
//
// 1. Forwards every version.dll export to System32\version.dll (stubs in proxy.asm).
// 2. Inside hoi4.exe only:
//    - hashes the executable (cached) and looks the hash up in loader_config.ini;
//    - file-tier features (virtual files) always run;
//    - engine-tier features (code inside hoi4.exe) run only on a supported hash.
//
// Everything runs from DllMain, before the game opens any script file. Any failure is
// logged and leaves the affected feature (or the whole game) unmodded.
#include "loader.h"

#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#include "MinHook.h"

// ---------------------------------------------------------------------------
// version.dll forwarding
// ---------------------------------------------------------------------------
#define VERSION_EXPORTS(X)                                                        \
    X(GetFileVersionInfoA) X(GetFileVersionInfoByHandle) X(GetFileVersionInfoExA) \
    X(GetFileVersionInfoExW) X(GetFileVersionInfoSizeA) X(GetFileVersionInfoSizeExA) \
    X(GetFileVersionInfoSizeExW) X(GetFileVersionInfoSizeW) X(GetFileVersionInfoW)   \
    X(VerFindFileA) X(VerFindFileW) X(VerInstallFileA) X(VerInstallFileW)            \
    X(VerLanguageNameA) X(VerLanguageNameW) X(VerQueryValueA) X(VerQueryValueW)

// Used for any export the real DLL failed to provide: report failure instead of crashing.
extern "C" UINT_PTR ProxyMissing() {
    SetLastError(ERROR_PROC_NOT_FOUND);
    return 0;
}

#define DEFINE_REAL_PTR(name) extern "C" void* g_real_##name = (void*)&ProxyMissing;
VERSION_EXPORTS(DEFINE_REAL_PTR)
#undef DEFINE_REAL_PTR

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logLock;

void Log(const char* fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    char buf[2048];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = _snprintf_s(buf, _TRUNCATE, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    int m = _vsnprintf_s(buf + n, sizeof(buf) - n - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    n = m < 0 ? (int)strlen(buf) : n + m;
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&g_logLock);
    DWORD written;
    WriteFile(g_log, buf, (DWORD)n, &written, nullptr);
    LeaveCriticalSection(&g_logLock);
}

// <Documents>, resolved from the registry (same source as SHGetFolderPath(CSIDL_PERSONAL),
// so OneDrive/redirected folders work) without pulling shell32 into DllMain.
static bool GetDocumentsDir(wchar_t* out, DWORD cch) {
    DWORD bytes = cch * sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
                     L"Personal", RRF_RT_REG_SZ, nullptr, out, &bytes) == ERROR_SUCCESS)
        return true;
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", out, cch);
    return n && n < cch && wcscat_s(out, cch, L"\\Documents") == 0;
}

static void OpenLog() {
    wchar_t path[MAX_PATH * 2];
    if (!GetDocumentsDir(path, MAX_PATH)) return;
    wcscat_s(path, L"\\Paradox Interactive");
    CreateDirectoryW(path, nullptr);
    wcscat_s(path, L"\\Hearts of Iron IV");
    CreateDirectoryW(path, nullptr);
    size_t dirLen = wcslen(path);
    wcscat_s(path, L"\\loader.log");
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) {
        // Another hoi4.exe instance holds loader.log.
        swprintf_s(path + dirLen, ARRAYSIZE(path) - dirLen, L"\\loader.%lu.log", GetCurrentProcessId());
        g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------
static const wchar_t* BaseName(const wchar_t* path) {
    const wchar_t* b = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') b = p + 1;
    return b;
}

static void LoadRealVersionDll() {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n > MAX_PATH - 16) {
        Log("ERROR: GetSystemDirectoryW failed");
        return;
    }
    wcscat_s(path, L"\\version.dll");
    HMODULE real = LoadLibraryW(path);
    if (!real) {
        Log("ERROR: cannot load %ls (error %lu); version.dll calls will fail", path, GetLastError());
        return;
    }
#define RESOLVE(name)                                                           \
    if (FARPROC p = GetProcAddress(real, #name)) g_real_##name = (void*)p;     \
    else Log("WARNING: %ls has no export " #name, path);
    VERSION_EXPORTS(RESOLVE)
#undef RESOLVE
    Log("Forwarding version.dll exports to %ls", path);
}

// True if engine-tier features may run: hoi4.exe's hash is listed in loader_config.ini.
static bool CheckSupportedBuild(HMODULE self, const wchar_t* exePath) {
    char hash[65];
    if (!GetExeHash(exePath, hash)) {
        Log("Engine-tier features disabled (hoi4.exe hash unavailable): %s", FeatureNames(Tier::Engine).c_str());
        return false;
    }

    wchar_t iniPath[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(self, iniPath, ARRAYSIZE(iniPath));
    if (!n || n >= ARRAYSIZE(iniPath)) {
        Log("ERROR: cannot get own module path");
        return false;
    }
    size_t dirLen = BaseName(iniPath) - iniPath;
    wcscpy_s(iniPath + dirLen, ARRAYSIZE(iniPath) - dirLen, L"loader_config.ini");

    wchar_t label[128];
    int entries = 0;
    if (GetFileAttributesW(iniPath) == INVALID_FILE_ATTRIBUTES) {
        Log("WARNING: %ls not found", iniPath);
    } else if (IsHashSupported(iniPath, hash, label, ARRAYSIZE(label), &entries)) {
        Log("Supported hoi4.exe build: %ls", label[0] ? label : L"(unnamed entry)");
        return true;
    }
    Log("Unknown hoi4.exe build (checked %d hash(es) in loader_config.ini).", entries);
    Log("Engine-tier features disabled: %s", FeatureNames(Tier::Engine).c_str());
    Log("File-tier features stay active: %s", FeatureNames(Tier::File).c_str());
    Log("If this build is known to work, add under [supported_hashes] in loader_config.ini:");
    Log("    my_build = %s", hash);
    return false;
}

static void Init(HMODULE self) {
    wchar_t exePath[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath));
    bool inGame = n && n < ARRAYSIZE(exePath) && _wcsicmp(BaseName(exePath), L"hoi4.exe") == 0;

    // Other executables in the game folder (launcher helpers, crash reporter) only get forwarding.
    if (inGame) {
        InitializeCriticalSection(&g_logLock);
        OpenLog();
        Log("hoi4_loader v%d starting in %ls (pid %lu)", LOADER_VERSION, exePath, GetCurrentProcessId());
    }
    LoadRealVersionDll();
    if (!inGame) return;

    ULONGLONG t0 = GetTickCount64();
    bool engineAllowed = CheckSupportedBuild(self, exePath);

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK) {
        Log("ERROR: MH_Initialize failed: %s; loader inactive", MH_StatusToString(s));
        return;
    }
    FeaturesInit(engineAllowed);
    if (!VfsStart()) {
        Log("ERROR: virtual files unavailable; script cannot detect the loader");
        return;
    }
    Log("Loader ready in %llu ms: hoi4_loader_version = %d, engine tier %s", GetTickCount64() - t0, LOADER_VERSION,
        engineAllowed ? "enabled" : "disabled");
}

// No C++ objects here, so SEH can catch anything Init throws at the OS level.
static void SafeInit(HMODULE self) {
    __try {
        Init(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("ERROR: exception 0x%08lX during init; game continues unmodded", GetExceptionCode());
    }
}

BOOL WINAPI DllMain(HMODULE self, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        SafeInit(self);
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        // Explicit FreeLibrary: take all hooks out. At process exit, leave everything alone.
        MH_Uninitialize();
    }
    return TRUE;
}
