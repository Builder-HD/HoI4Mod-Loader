// hoi4.exe hashing, hash cache and supported-hash lookup.
#include "loader.h"

#include <stdio.h>
#include <wchar.h>

#include "sha256.h"

static bool HashFile(const wchar_t* path, char hexOut[65]) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const DWORD kChunk = 1 << 20;
    uint8_t* buf = (uint8_t*)VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = buf != nullptr;
    Sha256 ctx;
    sha256_init(&ctx);
    DWORD got = 0;
    while (ok) {
        if (!ReadFile(f, buf, kChunk, &got, nullptr)) ok = false;
        else if (got == 0) break;
        else sha256_update(&ctx, buf, got);
    }
    CloseHandle(f);
    if (buf) VirtualFree(buf, 0, MEM_RELEASE);
    if (!ok) return false;
    uint8_t digest[32];
    sha256_final(&ctx, digest);
    for (int i = 0; i < 32; ++i) sprintf_s(hexOut + i * 2, 3, "%02x", digest[i]);
    return true;
}

static bool IsSha256Hex(const wchar_t* s) {
    for (int i = 0; i < 64; ++i)
        if (!iswxdigit(s[i])) return false;
    return s[64] == 0;
}

// %LOCALAPPDATA%\hoi4_loader\hash_cache.ini, created as UTF-16 so non-ASCII paths survive
// WritePrivateProfileStringW. Returns false if it can't be used.
static bool GetCachePath(wchar_t* out, DWORD cch) {
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", out, cch);
    if (!n || n >= cch || wcscat_s(out, cch, L"\\hoi4_loader") != 0) return false;
    CreateDirectoryW(out, nullptr);
    if (wcscat_s(out, cch, L"\\hash_cache.ini") != 0) return false;
    if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(out, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        static const char kHeader[] = "\xFF\xFE";  // UTF-16LE BOM
        DWORD w;
        WriteFile(f, kHeader, 2, &w, nullptr);
        CloseHandle(f);
    }
    return true;
}

bool GetExeHash(const wchar_t* exePath, char hexOut[65]) {
    ULONGLONG t0 = GetTickCount64();
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(exePath, GetFileExInfoStandard, &fa)) {
        Log("ERROR: cannot stat %ls (error %lu)", exePath, GetLastError());
        return false;
    }
    unsigned long long size = (unsigned long long)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
    unsigned long long mtime = (unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime;
    wchar_t sizeStr[32], mtimeStr[32];
    swprintf_s(sizeStr, L"%llu", size);
    swprintf_s(mtimeStr, L"%llu", mtime);

    // One section per executable path. ']' can't appear in a section name, so such paths
    // simply aren't cached.
    wchar_t cache[MAX_PATH * 2];
    bool useCache = GetCachePath(cache, ARRAYSIZE(cache)) && !wcschr(exePath, L']');
    const char* reason = "no usable cache file";
    if (useCache) {
        wchar_t cSize[32], cMtime[32], cHash[80];
        GetPrivateProfileStringW(exePath, L"size", L"", cSize, ARRAYSIZE(cSize), cache);
        GetPrivateProfileStringW(exePath, L"mtime", L"", cMtime, ARRAYSIZE(cMtime), cache);
        GetPrivateProfileStringW(exePath, L"sha256", L"", cHash, ARRAYSIZE(cHash), cache);
        if (!cHash[0]) reason = "no entry for this path";
        else if (wcscmp(cSize, sizeStr) != 0) reason = "size changed";
        else if (wcscmp(cMtime, mtimeStr) != 0) reason = "last-write time changed";
        else if (!IsSha256Hex(cHash)) reason = "entry is corrupt";
        else {
            for (int i = 0; i < 65; ++i) hexOut[i] = (char)towlower(cHash[i]);
            Log("hoi4.exe SHA-256: %s (from hash cache, %llu ms)", hexOut, GetTickCount64() - t0);
            return true;
        }
    }

    if (!HashFile(exePath, hexOut)) {
        Log("ERROR: cannot hash %ls (error %lu)", exePath, GetLastError());
        return false;
    }
    Log("hoi4.exe SHA-256: %s (computed, %llu ms; cache not used: %s)", hexOut, GetTickCount64() - t0, reason);

    if (useCache) {
        wchar_t wHash[65];
        for (int i = 0; i < 65; ++i) wHash[i] = (wchar_t)hexOut[i];
        bool ok = WritePrivateProfileStringW(exePath, L"size", sizeStr, cache) &&
                  WritePrivateProfileStringW(exePath, L"mtime", mtimeStr, cache) &&
                  WritePrivateProfileStringW(exePath, L"sha256", wHash, cache);
        if (!ok) Log("WARNING: cannot update %ls (error %lu)", cache, GetLastError());
    }
    return true;
}

bool IsHashSupported(const wchar_t* iniPath, const char* hash, wchar_t* label, size_t labelCch, int* entries) {
    static wchar_t section[32768];
    *entries = 0;
    label[0] = 0;
    DWORD n = GetPrivateProfileSectionW(L"supported_hashes", section, ARRAYSIZE(section), iniPath);
    for (wchar_t* line = section; n && *line; line += wcslen(line) + 1) {
        wchar_t* eq = wcschr(line, L'=');
        wchar_t* value = eq ? eq + 1 : line;
        while (*value == L' ' || *value == L'\t') ++value;
        if (*value == L';' || *value == L'#') continue;
        int i = 0;
        while (i < 64 && towlower(value[i]) == (wchar_t)hash[i]) ++i;
        ++*entries;
        wchar_t after = value[i];
        if (i == 64 && (after == 0 || after == L' ' || after == L'\t' || after == L';')) {
            if (eq) {
                size_t len = eq - line;
                while (len && (line[len - 1] == L' ' || line[len - 1] == L'\t')) --len;
                if (len >= labelCch) len = labelCch - 1;
                wmemcpy(label, line, len);
                label[len] = 0;
            }
            return true;
        }
    }
    return false;
}
