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
    size_t anchor = PickAnchor(bytes, mask);
    uint8_t* found = nullptr;
    int hits = 0;
    // Candidate starts are [text, last]; the anchor byte sits at start + anchor.
    uint8_t* last = text + size - bytes.size();
    for (uint8_t* a = text + anchor; a <= last + anchor;) {
        a = (uint8_t*)memchr(a, bytes[anchor], last + anchor + 1 - a);
        if (!a) break;
        uint8_t* p = a - anchor;
        size_t i = 0;
        while (i < bytes.size() && (!mask[i] || p[i] == bytes[i])) ++i;
        if (i == bytes.size() && ++hits == 1) found = p;
        ++a;
    }
    ULONGLONG ms = GetTickCount64() - t0;
    if (hits != 1) {
        Log("ERROR: signature '%s' matched %d time(s), expected exactly 1 (%llu ms)", name, hits, ms);
        return nullptr;
    }
    Log("Signature '%s' -> %s (%llu ms)", name, SigDescribe(found).c_str(), ms);
    return found;
}

uint8_t* SigFollowRel32(uint8_t* insn) { return SigRipTarget(insn, 1, 5); }

uint8_t* SigRipTarget(uint8_t* insn, size_t relOffset, size_t insnLen) {
    int32_t rel;
    memcpy(&rel, insn + relOffset, sizeof rel);
    return insn + insnLen + rel;
}

uint8_t* AnchorFindLea(const char* name, const char* text) {
    // The string: exactly one NUL-delimited occurrence in .rdata.
    uint8_t* rdata;
    size_t rsize;
    uint8_t* code;
    size_t csize;
    if (!ImageSection(".rdata", &rdata, &rsize) || !GetTextSection(&code, &csize)) {
        Log("ERROR: anchor '%s': cannot locate hoi4.exe sections", name);
        return nullptr;
    }
    size_t len = strlen(text);
    uint8_t* str = nullptr;
    int strHits = 0;
    for (uint8_t* p = rdata + 1; p + len < rdata + rsize;) {
        p = (uint8_t*)memchr(p, text[0], rdata + rsize - len - p);
        if (!p) break;
        if (p[-1] == 0 && p[len] == 0 && memcmp(p, text, len) == 0 && ++strHits == 1) str = p;
        ++p;
    }
    if (strHits != 1) {
        Log("ERROR: anchor '%s': string found %d time(s), expected 1", name, strHits);
        return nullptr;
    }
    // The code: exactly one `lea r64,[rip+rel32]` (REX.W 48/4C, 8D, modrm 00 rrr 101) targeting it.
    uint8_t* found = nullptr;
    int refs = 0;
    for (uint8_t* p = code; p + 7 <= code + csize;) {
        p = (uint8_t*)memchr(p, 0x8D, code + csize - 6 - p);
        if (!p) break;
        uint8_t* insn = p - 1;
        if (insn >= code && (insn[0] == 0x48 || insn[0] == 0x4C) && (p[1] & 0xC7) == 0x05 &&
            SigRipTarget(insn, 3, 7) == str && ++refs == 1)
            found = insn;
        ++p;
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
