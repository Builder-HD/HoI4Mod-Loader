// Engine symbol resolution for engine-tier features. Everything here comes from
// src/signatures.h (byte signatures, a string anchor) or RTTI names; see NOTES.md Part B1.
#include "engine.h"

#include <string.h>

#include "loader.h"
#include "signatures.h"

namespace Engine {

void* pLexerSync = nullptr;
void* pGameEntered = nullptr;
void* pRegionGetNameKey = nullptr;
void** pRenameProvinceEntryVtable = nullptr;
void** pRenameProvinceEffectVtable = nullptr;
size_t renameProvinceEffectVtableSlots = 0;

namespace {

struct StringView {
    const char* p;
    size_t n;
};

using IsLoaded_t = bool(__fastcall*)(void* gameState);
using Intern_t = int*(__fastcall*)(int* out, const StringView* s);
using TokenToString_t = const EngineString*(__fastcall*)(int token);
using FlagSet_t = void(__fastcall*)(void* flags, int token, short value, short days);
using FlagClear_t = void(__fastcall*)(void* flags, int token);
using Localize_t = EngineString*(__fastcall*)(EngineString* out, const char* key, void* p3, int p4);
using Register_t = void(__fastcall*)(void* registry, int token, EffectEntry* entry);

IsLoaded_t g_isLoaded = nullptr;
Intern_t g_intern = nullptr;
TokenToString_t g_tokenToString = nullptr;
FlagSet_t g_flagSet = nullptr;
FlagClear_t g_flagClear = nullptr;
Localize_t g_localize = nullptr;
Register_t g_register = nullptr;

void** g_ppGameState = nullptr;   // address of the engine's CGameState* global
uint32_t g_flagsOffset = 0;       // CGameState -> global flags container
uint32_t g_regionCountOffset = 0;
uint32_t g_regionArrayOffset = 0;
void** g_ppMapManager = nullptr;  // address of the map manager global
void** g_ppRegistry = nullptr;    // address of _EffectEntries (std::map*) global

// Engine layout constants that are part of a matched signature (so a change breaks the match):
const size_t kFlagEntrySize = 0x30;      // kSigFlagClear: add rax,0x30
const size_t kFlagEntryToken = 0x08;     // kSigFlagClear: lea r10,[rcx+8]; add rax,8
const size_t kFlagsData = 0x08, kFlagsCount = 0x14;  // kSigFlagClear: movsxd rax,[rcx+0x14]
const size_t kRegionTemplate = 0x30;     // kSigRegionGetNameKey: mov rax,[rcx+0x30]
const size_t kMapNamesDirty = 0x81;      // kSigMapManagerDirty
// Not part of a signature: CScriptFlag set date (NOTES: save token 0x284a) and the template id
// (save token 0xb). RegionTemplateId() is validated against the region array before use.
const size_t kFlagEntryDate = 0x20;
const size_t kTemplateId = 0xa0;

template <typename T>
T Read(const void* base, size_t offset) {
    T v;
    memcpy(&v, (const uint8_t*)base + offset, sizeof v);
    return v;
}

bool In(const void* p, const uint8_t* begin, size_t size) {
    return (const uint8_t*)p >= begin && (const uint8_t*)p < begin + size;
}

void ResolveRegistry() {
    // set_province_name registration stub (layout in signatures.h), anchored on its help text.
    uint8_t* lea = AnchorFindLea("set_province_name help", kAnchorRenameProvinceHelp);
    if (!lea) return;
    uint8_t* movReg = lea - 0x2c;  // mov rbx,[registry]
    uint8_t* leaVt = lea + 0x0b;   // lea rcx,[entry vtable]
    uint8_t* movTok = lea + 0x18;  // mov edx,<token>
    uint8_t* jmpReg = lea + 0x25;  // jmp Register
    if (!(movReg[0] == 0x48 && movReg[1] == 0x8B && movReg[2] == 0x1D) ||
        !(leaVt[0] == 0x48 && leaVt[1] == 0x8D && leaVt[2] == 0x0D) || movTok[0] != 0xBA || jmpReg[0] != 0xE9) {
        Log("ERROR: effect registry: set_province_name stub has an unexpected layout");
        return;
    }
    g_ppRegistry = (void**)SigRipTarget(movReg, 3, 7);
    pRenameProvinceEntryVtable = (void**)SigRipTarget(leaVt, 3, 7);
    g_register = (Register_t)SigFollowRel32(jmpReg);
    pRenameProvinceEffectVtable = FindVtableByRtti(".?AVCRenameProvinceEffect@@", &renameProvinceEffectVtableSlots);
    Log("Effect registry at %s, register %s, set_province_name token 0x%x", SigDescribe(g_ppRegistry).c_str(),
        SigDescribe((void*)g_register).c_str(), Read<uint32_t>(movTok, 1));
}

}  // namespace

void** FindVtableByRtti(const char* rttiName, size_t* slots) {
    uint8_t *data, *rdata, *text;
    size_t dsize, rsize, tsize;
    if (!ImageSection(".data", &data, &dsize) || !ImageSection(".rdata", &rdata, &rsize) ||
        !ImageSection(".text", &text, &tsize))
        return nullptr;
    uint8_t* image = (uint8_t*)GetModuleHandleW(nullptr);
    size_t len = strlen(rttiName) + 1;  // include NUL
    // TypeDescriptor { vfptr, spare, name[] } lives in .data.
    for (uint8_t* p = data + 0x10; p + len <= data + dsize; ++p) {
        p = (uint8_t*)memchr(p, rttiName[0], data + dsize - len - p + 1);
        if (!p) break;
        if (memcmp(p, rttiName, len) != 0) continue;
        uint32_t tdRva = (uint32_t)(p - 0x10 - image);
        // CompleteObjectLocator { sig=1, offset, cdOffset, tdRva, cdRva, selfRva } in .rdata.
        for (uint8_t* c = rdata; c + 24 <= rdata + rsize; c += 4) {
            if (Read<uint32_t>(c, 12) != tdRva || Read<uint32_t>(c, 0) != 1 || Read<uint32_t>(c, 4) != 0 ||
                Read<uint32_t>(c, 20) != (uint32_t)(c - image))
                continue;
            // vtable[-1] points at the COL.
            uint64_t colVa = (uint64_t)c;
            for (uint8_t* v = rdata; v + 16 <= rdata + rsize; v += 8) {
                if (Read<uint64_t>(v, 0) != colVa) continue;
                void** vt = (void**)(v + 8);
                size_t n = 0;
                while (n < 128 && In(vt[n], text, tsize)) ++n;
                if (slots) *slots = n;
                Log("RTTI %s -> vtable %s (%zu slots)", rttiName, SigDescribe(vt).c_str(), n);
                return vt;
            }
        }
    }
    Log("ERROR: RTTI vtable for %s not found", rttiName);
    return nullptr;
}

bool Resolve() {
    uint8_t* p;

    pLexerSync = SigFindUnique("lexer_sync", kSigLexerSync);
    pGameEntered = SigFindUnique("game_entered", kSigGameEntered);
    g_isLoaded = (IsLoaded_t)SigFindUnique("is_loaded_from_save", kSigIsLoadedFromSave);

    g_intern = (Intern_t)SigFindUnique("token_intern", kSigTokenIntern);
    g_tokenToString = (TokenToString_t)SigFindUnique("token_to_string", kSigTokenToString);

    if ((p = SigFindUnique("gamestate_and_flags", kSigGameStateAndFlags)) != nullptr) {
        g_ppGameState = (void**)SigRipTarget(p, kSigGameStatePtrRel, kSigGameStatePtrRel + 4);
        g_flagsOffset = Read<uint32_t>(p, kSigGameStateFlagsDisp);
    }
    g_flagSet = (FlagSet_t)SigFindUnique("flag_set", kSigFlagSet);
    g_flagClear = (FlagClear_t)SigFindUnique("flag_clear", kSigFlagClear);

    pRegionGetNameKey = SigFindUnique("region_get_name_key", kSigRegionGetNameKey);
    if ((p = SigFindUnique("region_array", kSigRegionArray)) != nullptr) {
        g_regionCountOffset = Read<uint32_t>(p, kSigRegionCountDisp);
        g_regionArrayOffset = Read<uint32_t>(p, kSigRegionArrayDisp);
    }

    if ((p = SigFindUnique("map_manager_dirty", kSigMapManagerDirty)) != nullptr)
        g_ppMapManager = (void**)SigRipTarget(p, kSigMapManagerPtrRel, kSigMapManagerPtrRel + 4);
    g_localize = (Localize_t)SigFindUnique("localize", kSigLocalize);

    ResolveRegistry();

    Log("Engine: gamestate %s, flags +0x%x, regions +0x%x/+0x%x", g_ppGameState ? SigDescribe(g_ppGameState).c_str() : "-",
        g_flagsOffset, g_regionArrayOffset, g_regionCountOffset);
    return HaveLifecycle() || HaveFlags() || HaveRegions();
}

bool HaveLifecycle() { return pLexerSync && pGameEntered && g_isLoaded; }
bool HaveTokens() { return g_intern && g_tokenToString; }
bool HaveFlags() { return g_ppGameState && g_flagsOffset && g_flagSet && g_flagClear; }
bool HaveRegions() { return g_ppGameState && g_regionCountOffset && g_regionArrayOffset && pRegionGetNameKey; }
bool HaveLocalize() { return g_localize != nullptr; }
bool HaveRegistry() {
    return g_ppRegistry && g_register && pRenameProvinceEntryVtable && pRenameProvinceEffectVtable &&
           renameProvinceEffectVtableSlots >= 14;
}

void* GameState() { return g_ppGameState ? *g_ppGameState : nullptr; }

bool IsLoadedFromSave(void* gameState) { return gameState && g_isLoaded && g_isLoaded(gameState); }

int Intern(const char* s) {
    if (!g_intern) return 0;
    StringView v{s, strlen(s)};
    int token = 0;
    g_intern(&token, &v);
    return token;
}

const char* TokenName(int token) {
    if (!g_tokenToString) return "";
    const EngineString* s = g_tokenToString(token);
    return s ? s->c_str() : "";
}

static void* Flags() {
    void* gs = GameState();
    return gs ? *(void**)((uint8_t*)gs + g_flagsOffset) : nullptr;
}

bool ForEachGlobalFlag(void (*fn)(const FlagEntry&, void*), void* ctx) {
    if (!HaveFlags()) return false;
    void* flags = Flags();
    if (!flags) return false;
    uint8_t* data = Read<uint8_t*>(flags, kFlagsData);
    int count = Read<int>(flags, kFlagsCount);
    for (int i = 0; i < count && data; ++i) {
        uint8_t* e = data + (size_t)i * kFlagEntrySize;
        fn(FlagEntry{Read<int>(e, kFlagEntryToken), Read<int>(e, kFlagEntryDate)}, ctx);
    }
    return true;
}

bool SetGlobalFlag(int token) {
    void* flags = HaveFlags() ? Flags() : nullptr;
    if (!flags || !token) return false;
    g_flagSet(flags, token, 1, 0);
    return true;
}

bool ClearGlobalFlag(int token) {
    void* flags = HaveFlags() ? Flags() : nullptr;
    if (!flags || !token) return false;
    g_flagClear(flags, token);
    return true;
}

int RegionCount() {
    void* gs = GameState();
    return gs && HaveRegions() ? Read<int>(gs, g_regionCountOffset) : 0;
}

void* RegionById(int id) {
    void* gs = GameState();
    if (!gs || !HaveRegions() || id < 1 || id >= Read<int>(gs, g_regionCountOffset)) return nullptr;
    void** arr = Read<void**>(gs, g_regionArrayOffset);
    return arr ? arr[id] : nullptr;
}

int RegionTemplateId(void* region) {
    if (!region) return -1;
    void* tmpl = Read<void*>(region, kRegionTemplate);
    return tmpl ? Read<int>(tmpl, kTemplateId) : -1;
}

const EngineString* LocalizeKey(const char* key) {
    if (!g_localize || !key || !*key) return nullptr;
    auto* out = new EngineString{};
    g_localize(out, key, nullptr, 0);
    // A missing entry localises to the key itself or to nothing; both mean "missing". The in-game
    // test (2026-10-13) detected a missing key this way.
    if (out->size == 0 || strcmp(out->c_str(), key) == 0) return nullptr;
    return out;
}

void MarkMapNamesDirty() {
    void* mgr = g_ppMapManager ? *g_ppMapManager : nullptr;
    if (mgr) *(uint16_t*)((uint8_t*)mgr + kMapNamesDirty) = 0x101;
}

bool RegisterEffect(int token, EffectEntry* entry) {
    void* registry = g_ppRegistry ? *g_ppRegistry : nullptr;
    if (!registry || !g_register || !token) return false;
    g_register(registry, token, entry);
    return true;
}

}  // namespace Engine
