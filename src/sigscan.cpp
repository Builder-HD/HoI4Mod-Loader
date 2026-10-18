// Byte-signature scanning over hoi4.exe's .text section (engine tier helpers).
#include "loader.h"

#include <stdio.h>
#include <string.h>
#include <vector>

bool ImageSection(const char* name, uint8_t** begin, size_t* size) {
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)base;
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    size_t len = strlen(name);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (len <= 8 && memcmp(sec[i].Name, name, len) == 0 && (len == 8 || sec[i].Name[len] == 0)) {
            *begin = base + sec[i].VirtualAddress;
            *size = sec[i].Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

static bool GetTextSection(uint8_t** begin, size_t* size) { return ImageSection(".text", begin, size); }

// "48 8B ?? 05" -> bytes + mask (true = must match).
static bool ParsePattern(const char* pattern, std::vector<uint8_t>* bytes, std::vector<bool>* mask) {
    for (const char* p = pattern; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (p[0] == '?') {
            bytes->push_back(0);
            mask->push_back(false);
            p += p[1] == '?' ? 2 : 1;
            continue;
        }
        unsigned v;
        if (sscanf_s(p, "%2x", &v) != 1) return false;
        bytes->push_back((uint8_t)v);
        mask->push_back(true);
        p += 2;
    }
    for (bool fixed : *mask)
        if (fixed) return true;
    return false;  // all wildcards
}

// Index of a fixed byte to memchr for: the first one that isn't among the most common x64
// code bytes, so the scan stops less often.
static size_t PickAnchor(const std::vector<uint8_t>& bytes, const std::vector<bool>& mask) {
    static const uint8_t kCommon[] = {0x00, 0xFF, 0x48, 0x8B, 0x89, 0x4C, 0x44, 0x24, 0xCC, 0x8D, 0x0F, 0x83, 0xC0, 0x45, 0x41, 0x49};
    size_t firstFixed = bytes.size();
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (!mask[i]) continue;
        if (firstFixed == bytes.size()) firstFixed = i;
        if (!memchr(kCommon, bytes[i], sizeof kCommon)) return i;
    }
    return firstFixed;
}

// Matches of a parsed pattern in [text, text + size); the first one goes to *first.
static int CountMatches(const std::vector<uint8_t>& bytes, const std::vector<bool>& mask, uint8_t* text, size_t size,
                        uint8_t** first) {
    size_t anchor = PickAnchor(bytes, mask);
    int hits = 0;
    // Candidate starts are [text, last]; the anchor byte sits at start + anchor.
    uint8_t* last = text + size - bytes.size();
    for (uint8_t* a = text + anchor; a <= last + anchor;) {
        a = (uint8_t*)memchr(a, bytes[anchor], last + anchor + 1 - a);
        if (!a) break;
        uint8_t* p = a - anchor;
        size_t i = 0;
        while (i < bytes.size() && (!mask[i] || p[i] == bytes[i])) ++i;
        if (i == bytes.size() && ++hits == 1) *first = p;
        ++a;
    }
    return hits;
}

uint8_t* SigFindUnique(const char* name, const char* pattern) {
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;
    if (!ParsePattern(pattern, &bytes, &mask)) {
        Log("ERROR: signature '%s' is malformed", name);
        return nullptr;
    }
    uint8_t* text;
    size_t size;
    if (!GetTextSection(&text, &size) || size < bytes.size()) {
        Log("ERROR: signature '%s': cannot locate hoi4.exe .text", name);
        return nullptr;
    }
    ULONGLONG t0 = GetTickCount64();
    uint8_t* found = nullptr;
    int hits = CountMatches(bytes, mask, text, size, &found);
    ULONGLONG ms = GetTickCount64() - t0;
    if (hits != 1) {
        Log("ERROR: signature '%s' matched %d time(s), expected exactly 1 (%llu ms)", name, hits, ms);
        return nullptr;
    }
    Log("Signature '%s' -> %s (%llu ms)", name, SigDescribe(found).c_str(), ms);
    return found;
}

int SigCount(const char* pattern) {
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;
    uint8_t* text;
    size_t size;
    if (!ParsePattern(pattern, &bytes, &mask) || !GetTextSection(&text, &size) || size < bytes.size()) return -1;
    uint8_t* first = nullptr;
    return CountMatches(bytes, mask, text, size, &first);
}

// Code shape results while recording (sigreport.cpp): pattern text -> matched at least once.
// Recording runs only during the startup scan, on the loader's thread.
static bool g_recordShapes = false;
static std::vector<std::pair<std::string, bool>> g_shapes;

void SigRecordShapes(bool on) {
    if (on) g_shapes.clear();
    g_recordShapes = on;
}

int SigShapeResult(const char* pattern) {
    for (const auto& s : g_shapes)
        if (s.first == pattern) return s.second ? 1 : 0;
    return -1;
}

static bool MatchAt(const uint8_t* p, const char* pattern) {
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;
    uint8_t* text;
    size_t size;
    if (!p || !ParsePattern(pattern, &bytes, &mask) || !GetTextSection(&text, &size)) return false;
    if (p < text || p + bytes.size() > text + size) return false;
    for (size_t i = 0; i < bytes.size(); ++i)
        if (mask[i] && p[i] != bytes[i]) return false;
    return true;
}

bool SigMatchAt(const uint8_t* p, const char* pattern) {
    bool ok = MatchAt(p, pattern);
    if (g_recordShapes) {
        auto it = g_shapes.begin();
        while (it != g_shapes.end() && it->first != pattern) ++it;
        if (it == g_shapes.end()) g_shapes.emplace_back(pattern, ok);
        else it->second = it->second || ok;
    }
    return ok;
}

uint8_t* SigFollowRel32(uint8_t* insn) { return SigRipTarget(insn, 1, 5); }

uint8_t* SigRipTarget(uint8_t* insn, size_t relOffset, size_t insnLen) {
    int32_t rel;
    memcpy(&rel, insn + relOffset, sizeof rel);
    return insn + insnLen + rel;
}

// The anchor's string occurrences in .rdata (*strHits) and, if exactly one, the number of
// `lea r64,[rip+rel32]` (REX.W 48/4C, 8D, modrm 00 rrr 101) in .text targeting it (the first in
// *found). Returns -1 if the sections are missing.
static int AnchorRefs(const char* text, int* strHits, uint8_t** found) {
    uint8_t* rdata;
    size_t rsize;
    uint8_t* code;
    size_t csize;
    *strHits = 0;
    if (!ImageSection(".rdata", &rdata, &rsize) || !GetTextSection(&code, &csize)) return -1;
    size_t len = strlen(text);
    uint8_t* str = nullptr;
    for (uint8_t* p = rdata + 1; p + len < rdata + rsize;) {
        p = (uint8_t*)memchr(p, text[0], rdata + rsize - len - p);
        if (!p) break;
        if (p[-1] == 0 && p[len] == 0 && memcmp(p, text, len) == 0 && ++*strHits == 1) str = p;
        ++p;
    }
    if (*strHits != 1) return 0;
    int refs = 0;
    for (uint8_t* p = code; p + 7 <= code + csize;) {
        p = (uint8_t*)memchr(p, 0x8D, code + csize - 6 - p);
        if (!p) break;
        uint8_t* insn = p - 1;
        if (insn >= code && (insn[0] == 0x48 || insn[0] == 0x4C) && (p[1] & 0xC7) == 0x05 &&
            SigRipTarget(insn, 3, 7) == str && ++refs == 1)
            *found = insn;
        ++p;
    }
    return refs;
}

int AnchorCount(const char* text, int* strHits) {
    uint8_t* found = nullptr;
    return AnchorRefs(text, strHits, &found);
}

uint8_t* AnchorFindLea(const char* name, const char* text) {
    // The string: exactly one NUL-delimited occurrence in .rdata; the code: exactly one lea of it.
    int strHits = 0;
    uint8_t* found = nullptr;
    int refs = AnchorRefs(text, &strHits, &found);
    if (refs < 0) {
        Log("ERROR: anchor '%s': cannot locate hoi4.exe sections", name);
        return nullptr;
    }
    if (strHits != 1) {
        Log("ERROR: anchor '%s': string found %d time(s), expected 1", name, strHits);
        return nullptr;
    }
    if (refs != 1) {
        Log("ERROR: anchor '%s': %d lea reference(s), expected 1", name, refs);
        return nullptr;
    }
    Log("Anchor '%s' -> %s", name, SigDescribe(found).c_str());
    return found;
}

std::string SigDescribe(const void* p) {
    char buf[64];
    sprintf_s(buf, "hoi4.exe+0x%llx", (unsigned long long)((const uint8_t*)p - (const uint8_t*)GetModuleHandleW(nullptr)));
    return buf;
}

std::string DescribeAddress(const void* p) {
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH] = L"?";
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)p, &m) ||
        !m) {
        char buf[32];
        sprintf_s(buf, "0x%p", p);
        return buf;
    }
    GetModuleFileNameW(m, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    char buf[160];
    sprintf_s(buf, "%ls+0x%llx", name ? name + 1 : path, (unsigned long long)((const uint8_t*)p - (const uint8_t*)m));
    return buf;
}
