// large_map: maps larger than vanilla's (NOTES.md Part T). The engine builds its map textures (e.g. the
// terrain type per pixel, FUN_1412f2550 in 1.20: 4 bytes per map pixel) in one scratch buffer that the
// application allocates once at start with a fixed 50 MB (`mov dword [app+0x350], 0x3200000;
// mov ecx, 0x3200000; call new; mov [app+0x348], rax`, 1.20 0x1422963aa) and never checks: a map of
// more than 13,107,200 pixels (vanilla: 5632 x 2048 = 11,534,336) writes past its end and crashes
// while the game starts. The loader runs before that code, so it sets both sizes to what the map that
// will be loaded needs (its provinces bitmap, from the last enabled mod that has one, else the game).
// The texture file loader also refuses files over 16 MB (the heightmap, world_normal.bmp and the colour
// map of a larger map are bigger): its limit is raised to 4 bytes per map pixel. The terrain renderer
// handles at most 256 blocks (256 x 256 pixels each, a fixed array and byte indices): a larger map gets
// larger blocks (512, 1024, ...) in both places that count them. A vanilla-sized map leaves the code
// alone. Runtime only: hoi4.exe on disk is never touched.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "loader.h"
#include "mod_list.h"
#include "signatures.h"

namespace {

const uint32_t kVanillaScratch = 0x3200000;  // 50 MB
const uint32_t kVanillaTextureFile = 0x1000000;  // 16 MB

// The file the game will read for `rel` (backslashes): the last enabled mod that has it, else the game.
std::wstring Winning(const std::wstring& rel) {
    std::vector<ModList::EnabledMod> mods;
    ModList::Enabled(UserFolder(), &mods);
    for (auto it = mods.rbegin(); it != mods.rend(); ++it) {
        if (it->status != ModList::EnabledMod::Status::Ok) continue;
        std::wstring p = it->root + L"\\" + rel;
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    std::wstring p = GameFolder() + L"\\" + rel;
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES ? p : L"";
}

// map/default.map's `provinces = "x"` (default "provinces.bmp").
std::wstring ProvincesName() {
    std::wstring def = Winning(L"map\\default.map");
    if (def.empty()) return L"provinces.bmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, def.c_str(), L"rb") != 0 || !f) return L"provinces.bmp";
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    for (size_t at = text.find("provinces"); at != std::string::npos; at = text.find("provinces", at + 1)) {
        if (at && (isalnum((unsigned char)text[at - 1]) || text[at - 1] == '_')) continue;
        size_t q = text.find_first_not_of(" \t", at + 9);
        if (q == std::string::npos || text[q] != '=') continue;
        size_t a = text.find('"', q), b = a == std::string::npos ? a : text.find('"', a + 1);
        if (b == std::string::npos) break;
        return ModList::Wide(text.substr(a + 1, b - a - 1));
    }
    return L"provinces.bmp";
}

bool BitmapSize(const std::wstring& path, int* w, int* h) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    unsigned char hdr[26];
    bool ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr && hdr[0] == 'B' && hdr[1] == 'M';
    fclose(f);
    if (!ok) return false;
    int32_t ww, hh;
    memcpy(&ww, hdr + 18, 4);
    memcpy(&hh, hdr + 22, 4);
    *w = ww, *h = hh < 0 ? -hh : hh;
    return ww > 0 && *h > 0;
}

bool WriteCode8(uint8_t* at, uint8_t value) {
    DWORD old;
    if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
    *at = value;
    DWORD dummy;
    VirtualProtect(at, 1, old, &dummy);
    FlushInstructionCache(GetCurrentProcess(), at, 1);
    return true;
}

// The terrain blocks: the smallest shift (8 = vanilla's 256 pixels) that keeps the map within 256 blocks.
void TerrainBlocks(int w, int h) {
    int shift = 8;
    while (shift < 12 && (w >> shift) * (h >> shift) > 256) ++shift;
    if (shift == 8) return;
    if ((w >> shift) < 1 || (h >> shift) < 1 || (w >> shift) * (h >> shift) > 256) {
        Log("large_map: map %d x %d has too many terrain blocks even at %d pixels; the game will crash drawing it", w, h,
            1 << shift);
        return;
    }
    uint8_t* build = SigFindUnique("terrain_blocks_build", kSigTerrainBlocksBuild);
    uint8_t* cam = SigFindUnique("terrain_blocks_camera", kSigTerrainBlocksCamera);
    uint8_t* at[4] = {build ? build + kSigTerrainBlocksBuildShift1 : nullptr, build ? build + kSigTerrainBlocksBuildShift2 : nullptr,
                      cam ? cam + kSigTerrainBlocksCameraShift1 : nullptr, cam ? cam + kSigTerrainBlocksCameraShift2 : nullptr};
    for (uint8_t* p : at)
        if (!p || *p != 8) {
            Log("large_map: the terrain block code was not found; a map of %d blocks will crash while drawing",
                (w >> 8) * (h >> 8));
            return;
        }
    for (uint8_t* p : at)
        if (!WriteCode8(p, (uint8_t)shift)) {
            Log("large_map: could not write the terrain block size (error %lu)", GetLastError());
            return;
        }
    Log("large_map: terrain blocks of %d pixels (%d x %d = %d blocks; vanilla 256 pixels, at most 256 blocks)", 1 << shift,
        w >> shift, h >> shift, (w >> shift) * (h >> shift));
}

bool WriteCode32(uint8_t* at, uint32_t value) {
    DWORD old;
    if (!VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, &value, 4);
    DWORD dummy;
    VirtualProtect(at, 4, old, &dummy);
    FlushInstructionCache(GetCurrentProcess(), at, 4);
    return true;
}

}  // namespace

bool LargeMapInit() {
    std::wstring rel = L"map\\" + ProvincesName();
    std::wstring path = Winning(rel);
    int w = 0, h = 0;
    if (path.empty() || !BitmapSize(path, &w, &h)) {
        Log("large_map: cannot read the map's size from %s; left alone", ModList::Narrow(rel).c_str());
        return true;
    }
    uint64_t need = (uint64_t)w * (uint64_t)h * 4;
    if (need <= kVanillaScratch) {
        Log("large_map: map %d x %d fits the engine's map scratch buffer (%llu of %u bytes); left alone", w, h,
            (unsigned long long)need, kVanillaScratch);
        return true;
    }
    uint8_t* at = SigFindUnique("map_scratch_size", kSigMapScratchSize);
    if (!at) {
        Log("large_map: map %d x %d needs %llu bytes of map scratch buffer, the engine has 50 MB and its code was not "
            "found: the game will crash while loading this map", w, h, (unsigned long long)need);
        return false;
    }
    // Shape: the size store's field is the pointer store's + 8, and both immediates are the vanilla size.
    uint32_t sizeDisp, ptrDisp, imm1, imm2;
    memcpy(&sizeDisp, at + 2, 4);
    memcpy(&imm1, at + kSigMapScratchSizeImm1, 4);
    memcpy(&imm2, at + kSigMapScratchSizeImm2, 4);
    memcpy(&ptrDisp, at + kSigMapScratchSizePtrDisp, 4);
    if (sizeDisp != ptrDisp + 8 || imm1 != kVanillaScratch || imm2 != kVanillaScratch) {
        Log("large_map: the scratch buffer's code has an unexpected shape (+0x%x / +0x%x, %u / %u); left alone", sizeDisp,
            ptrDisp, imm1, imm2);
        return false;
    }
    if (need > 0x7FFFFFFFull) {
        Log("large_map: map %d x %d needs %llu bytes, more than the engine's 32-bit size; left alone", w, h,
            (unsigned long long)need);
        return false;
    }
    uint32_t size = (uint32_t)((need + 0xFFFFF) & ~0xFFFFFull);  // whole megabytes
    if (!WriteCode32(at + kSigMapScratchSizeImm1, size) || !WriteCode32(at + kSigMapScratchSizeImm2, size)) {
        Log("large_map: could not write the scratch buffer's size (error %lu)", GetLastError());
        return false;
    }
    Log("large_map: map %d x %d (%s): the engine's map scratch buffer is %u MB instead of 50 MB", w, h,
        ModList::Narrow(path).c_str(), size >> 20);
    TerrainBlocks(w, h);
    // Texture files: up to 4 bytes per map pixel (the largest map texture at full size), plus headers.
    uint8_t* lim = SigFindUnique("texture_file_limit", kSigTextureFileLimit);
    uint32_t old = 0;
    if (lim) memcpy(&old, lim + kSigTextureFileLimitImm, 4);
    if (!lim || old != kVanillaTextureFile) {
        Log("large_map: the texture file limit was not found (%s); map textures over 16 MB will not load",
            lim ? "unexpected value" : "no match");
        return true;  // the scratch buffer alone still helps maps up to 16 MB textures
    }
    uint32_t texLimit = size + 0x100000;
    if (!WriteCode32(lim + kSigTextureFileLimitImm, texLimit)) {
        Log("large_map: could not write the texture file limit (error %lu)", GetLastError());
        return true;
    }
    Log("large_map: texture files up to %u MB load (vanilla 16 MB)", texLimit >> 20);
    return true;
}
