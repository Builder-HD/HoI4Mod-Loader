// Engine symbol resolution for engine-tier features. Everything here comes from
// src/signatures.h (byte signatures, a string anchor) or RTTI names; see NOTES.md Part B1.
#include "engine.h"

#include <stdio.h>
#include <string.h>

#include "loader.h"
#include "signatures.h"

namespace Engine {

void* pLexerSync = nullptr;
void* pGameEntered = nullptr;
void* pAppDoFrame = nullptr;
void* pRegionGetNameKey = nullptr;
void* pFlagSet = nullptr;
void* pFlagClear = nullptr;
void** pRenameProvinceEntryVtable = nullptr;
void** pRenameProvinceEffectVtable = nullptr;
size_t renameProvinceEffectVtableSlots = 0;
Vtable effectBase, triggerBase, effectTemplate, effectValueReader, triggerTemplate;
NumericTemplate numericCount, numericRatio;
ScriptValueEval_t scriptValueEval = nullptr;
int tokGreater = 0, tokLess = 0;
void** effectTemplateEntryVtable = nullptr;
void** triggerTemplateEntryVtable = nullptr;

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
using RegisterTrigger_t = void(__fastcall*)(void* registry, int token, void* entry);
using ReadString_t = void(__fastcall*)(void* parser, EngineString* out, bool allowNewline);
using Unexpected_t = void(__fastcall*)(void* parser);
using Construct_t = EngineString*(__fastcall*)(EngineString* raw, const char* p, size_t n);
using CountryTagName_t = const EngineString*(__fastcall*)(const int* tag);

IsLoaded_t g_isLoaded = nullptr;
Intern_t g_intern = nullptr;
TokenToString_t g_tokenToString = nullptr;
FlagSet_t g_flagSet = nullptr;
FlagClear_t g_flagClear = nullptr;
Localize_t g_localize = nullptr;
Register_t g_register = nullptr;
RegisterTrigger_t g_registerTrigger = nullptr;
ReadString_t g_readString = nullptr;
Unexpected_t g_unexpected = nullptr;
Construct_t g_construct = nullptr;
CountryTagName_t g_countryTagName = nullptr;
void** g_ppTriggerRegistry = nullptr;

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
// Not part of a signature: CScriptFlag fields and the template id (save token 0xb).
// RegionTemplateId() is validated against the region array before use. A flag entry is
// { vtable, token @+8, CGameDate @+0x10 (set date int @+0x18; its second vtable @+0x20 is the
// save-reader interface for token 0x284a), short value @+0x28, short days @+0x2a }: get-or-create
// stores the current date at +0x18 on every set; flag_manager.daily counts days > 0 down and
// removes the flag at 0 (NOTES.md Part D, region_names follow-up).
const size_t kFlagEntryDate = 0x18;
const size_t kFlagEntryValue = 0x28, kFlagEntryDays = 0x2a;
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

void ResolveTriggerRegistry() {
    // has_global_flag registration stub (layout in signatures.h), anchored on its help text.
    uint8_t* lea = AnchorFindLea("has_global_flag help", kAnchorGlobalFlagTriggerHelp);
    if (!lea) return;
    uint8_t* movReg = lea - 0x2c;  // mov rbx,[registry]
    uint8_t* leaVt = lea + 0x0f;   // lea rcx,[entry vtable]
    uint8_t* movTok = lea + 0x1c;  // mov edx,<token>
    uint8_t* jmpReg = lea + 0x29;  // jmp Register
    if (!(movReg[0] == 0x48 && movReg[1] == 0x8B && movReg[2] == 0x1D) ||
        !(leaVt[0] == 0x48 && leaVt[1] == 0x8D && leaVt[2] == 0x0D) || movTok[0] != 0xBA || jmpReg[0] != 0xE9) {
        Log("ERROR: trigger registry: has_global_flag stub has an unexpected layout");
        return;
    }
    g_ppTriggerRegistry = (void**)SigRipTarget(movReg, 3, 7);
    g_registerTrigger = (RegisterTrigger_t)SigFollowRel32(jmpReg);
    Log("Trigger registry at %s, register %s, has_global_flag token 0x%x", SigDescribe(g_ppTriggerRegistry).c_str(),
        SigDescribe((void*)g_registerTrigger).c_str(), Read<uint32_t>(movTok, 1));
}

Vtable RttiVtable(const char* name) {
    Vtable v;
    v.vt = FindVtableByRtti(name, &v.slots);
    return v;
}

// Numeric templates (NOTES.md E2): the class vtable by RTTI, its object size, operator new and
// constructor decoded from CTriggerEntry<T>::Create, which must have the expected shape.
bool ResolveNumericTemplate(const char* cls, const char* entryCls, NumericTemplate* t) {
    t->vt = RttiVtable(cls);
    size_t n = 0;
    void** entry = FindVtableByRtti(entryCls, &n);
    if (!t->vt.vt || !entry || n < 2) return false;
    uint8_t* f = (uint8_t*)entry[1];
    if (!SigMatchAt(f, kCodeTriggerFactory)) {
        Log("ERROR: numeric template %s: factory %s has an unexpected shape", cls, SigDescribe(f).c_str());
        return false;
    }
    if ((void**)SigRipTarget(f + kCodeFactoryVtableLea, 3, 7) != t->vt.vt) {
        Log("ERROR: numeric template %s: factory builds a different class", cls);
        return false;
    }
    t->size = Read<uint32_t>(f, kCodeFactorySizeImm);
    t->alloc = (void* (*)(size_t))SigFollowRel32(f + kCodeFactoryNewCall);
    t->ctor = (void (*)(void*))SigFollowRel32(f + kCodeFactoryCtorCall);
    return t->size >= 0x60 && t->size <= 0x400;
}

void ResolveNumeric() {
    bool ok = ResolveNumericTemplate(".?AVCNumOfFactoriesTrigger@@", ".?AV?$CTriggerEntry@VCNumOfFactoriesTrigger@@@@",
                                     &numericCount) &&
              ResolveNumericTemplate(".?AVCHasStabilityTrigger@@", ".?AV?$CTriggerEntry@VCHasStabilityTrigger@@@@",
                                     &numericRatio);
    if (ok && (numericCount.vt.slots < 24 || numericRatio.vt.slots < 26)) {
        Log("ERROR: numeric templates have too few vtable slots (%zu, %zu)", numericCount.vt.slots, numericRatio.vt.slots);
        ok = false;
    }
    if (ok) {
        uint8_t* cmp = (uint8_t*)numericCount.vt.vt[22];
        uint8_t* clause = (uint8_t*)numericCount.vt.vt[5];
        uint8_t* blockClause = triggerBase.vt ? (uint8_t*)triggerBase.vt[5] : nullptr;
        bool block = false;
        for (size_t i = 0; blockClause && i < 0x80 && !block; ++i) block = SigMatchAt(blockClause + i, kCodeBlockValueCheck);
        if (!SigMatchAt(cmp, kCodeIntCompare) || !SigMatchAt(clause, kCodeNumericReadClause) ||
            numericRatio.vt.vt[5] != (void*)clause || !SigMatchAt((uint8_t*)numericRatio.vt.vt[4], kCodeValueReadValue) ||
            !block) {
            Log("ERROR: numeric trigger code does not have the expected shape (compare %d, clause %d, shared %d, read %d, "
                "block %d)",
                SigMatchAt(cmp, kCodeIntCompare), SigMatchAt(clause, kCodeNumericReadClause),
                numericRatio.vt.vt[5] == (void*)clause, SigMatchAt((uint8_t*)numericRatio.vt.vt[4], kCodeValueReadValue),
                block);
            ok = false;
        } else {
            scriptValueEval = (ScriptValueEval_t)SigFollowRel32(cmp + kCodeIntCompareEvalCall);
            tokGreater = -Read<int32_t>(clause, kCodeNumericOpImm);
            tokLess = tokGreater + 1;
        }
    }
    if (ok)
        Log("Numeric triggers: count template %zu bytes, ratio template %zu bytes, script value %s, '>' token 0x%x",
            numericCount.size, numericRatio.size, SigDescribe((void*)scriptValueEval).c_str(), tokGreater);
    else
        numericCount = numericRatio = NumericTemplate{};
}

void ResolveNativeScript() {
    g_readString = (ReadString_t)SigFindUnique("parser_read_string", kSigParserReadString);
    g_unexpected = (Unexpected_t)SigFindUnique("parser_unexpected", kSigParserUnexpected);
    g_construct = (Construct_t)SigFindUnique("string_construct", kSigStringConstruct);
    ResolveTriggerRegistry();
    effectBase = RttiVtable(".?AVCEffect@@");
    triggerBase = RttiVtable(".?AVCTrigger@@");
    effectTemplate = RttiVtable(".?AVCRenameProvinceEffect@@");
    effectValueReader = RttiVtable(".?AVCSetStateNameEffect@@");
    triggerTemplate = RttiVtable(".?AVCAlwaysTrigger@@");
    size_t n = 0;
    triggerTemplateEntryVtable = FindVtableByRtti(".?AV?$CTriggerEntry@VCAlwaysTrigger@@@@", &n);
    effectTemplateEntryVtable = pRenameProvinceEntryVtable;
    ResolveNumeric();
}

// MSVC std::map<int, T*> lookup, read-only: map object { head*, size }; node { left, parent,
// right, color, isnil @+0x19, key @+0x20, value @+0x28 } (as walked by the engine's Register).
bool MapHasKey(void* mapObj, int key) {
    if (!mapObj) return false;
    uint8_t* head = Read<uint8_t*>(mapObj, 0);
    if (!head) return false;
    uint8_t* node = Read<uint8_t*>(head, 8);  // root
    for (int guard = 0; node && Read<uint8_t>(node, 0x19) == 0 && guard < 64; ++guard) {
        int k = Read<int>(node, 0x20);
        if (key < k) node = Read<uint8_t*>(node, 0);
        else if (k < key) node = Read<uint8_t*>(node, 0x10);
        else return true;
    }
    return false;
}

// --- On actions and the game session (Part D) --------------------------------------------------
using SyncedRandom_t = uint32_t(__fastcall*)(const char* file, int line);
using ScopeCtor_t = void*(__fastcall*)(void* scope, int seed);
using ScopeDtor_t = void(__fastcall*)(void* scope);
using ScopeSetCountry_t = void(__fastcall*)(void* scope, const int* tag, bool);
using OnActionFind_t = void*(__fastcall*)(void* db, const EngineString* name);
using OnActionFire_t = void(__fastcall*)(void* db, void* list, void* scope, bool ignoreGameRunningCheck);
using SessionGetter_t = uint8_t*(__fastcall*)(void* idler);

SyncedRandom_t g_syncedRandom = nullptr;
ScopeCtor_t g_scopeCtor = nullptr;
ScopeDtor_t g_scopeDtor = nullptr;
ScopeSetCountry_t g_scopeSetCountry = nullptr;
OnActionFind_t g_onActionFind = nullptr;
OnActionFire_t g_onActionFire = nullptr;
void** g_ppOnActionDb = nullptr;      // address of the COnActionDataBase* global
uint32_t g_playerTagOffset = 0;       // CGameState -> local player's country tag
uint32_t g_sessionGetterDisp = 0;     // idler vtable byte offset of GetSession()
uint32_t g_sessionServerOffset = 0;   // CSession -> CServer*
uint32_t g_sessionHotjoinOffset = 0;  // CSession -> bIsHotjoin
const char* const kServerClasses[] = {"CDummyServer", "CNetworkServer", "CProxyServer", "CDedicatedServer"};
void** g_serverVtables[4] = {};
// CEventScope is 0xb0 bytes in 1.19.x (the engine's stack slot); generous room for growth.
const size_t kEventScopeBuffer = 0x400;
const char* const kNoSuchOnAction = "hoi4_loader_nx";  // never defined: Find returns the null object

void ResolveOnActions() {
    uint8_t* p;
    if ((p = SigFindUnique("player_scope", kSigPlayerScope)) != nullptr) {
        uint32_t player = Read<uint32_t>(p, kSigPlayerScopePlayerImm);
        if (player == Read<uint32_t>(p, kSigPlayerScopePlayerDisp)) {
            g_playerTagOffset = player;
            g_scopeCtor = (ScopeCtor_t)SigFollowRel32(p + kSigPlayerScopeCtorCall);
            g_scopeSetCountry = (ScopeSetCountry_t)SigFollowRel32(p + kSigPlayerScopeSetCountryCall);
            g_scopeDtor = (ScopeDtor_t)SigFollowRel32(p + kSigPlayerScopeDtorCall);
        } else {
            Log("ERROR: player_scope: player tag offsets disagree (0x%x / 0x%x)", player,
                Read<uint32_t>(p, kSigPlayerScopePlayerDisp));
        }
    }
    if ((p = SigFindUnique("on_startup_seed", kSigOnStartupSeed)) != nullptr) {
        if ((void*)SigFollowRel32(p + kSigOnStartupSeedCtorCall) == (void*)g_scopeCtor)
            g_syncedRandom = (SyncedRandom_t)SigFollowRel32(p + kSigOnStartupSeedRandomCall);
        else
            Log("ERROR: on_startup_seed: event scope constructor differs from player_scope's");
    }
    if ((p = SigFindUnique("on_startup_fire", kSigOnStartupFire)) != nullptr) {
        // movsd xmm0,[rip+x] loads the first 8 bytes of "on_startup": opcode at NameRel-4, length 8.
        const char* literal = (const char*)SigRipTarget(p + kSigOnStartupFireNameRel - 4, 4, 8);
        if (memcmp(literal, "on_startup", 11) == 0) {
            g_ppOnActionDb = (void**)SigRipTarget(p, kSigOnStartupFireDbRel, kSigOnStartupFireDbRel + 4);
            g_onActionFind = (OnActionFind_t)SigFollowRel32(p + kSigOnStartupFireFindCall);
            g_onActionFire = (OnActionFire_t)SigFollowRel32(p + kSigOnStartupFireFireCall);
        } else {
            Log("ERROR: on_startup_fire: the matched code does not fire on_startup");
        }
    }
    uint8_t* hj = SigFindUnique("session_hotjoin", kSigSessionHotjoin);
    uint8_t* sv = SigFindUnique("session_server", kSigSessionServer);
    if (hj && sv) {
        uint32_t getter = Read<uint32_t>(hj, kSigSessionGetterDisp);
        if (getter == Read<uint32_t>(sv, kSigSessionGetterDisp) && getter % 8 == 0 && getter < 0x800) {
            g_sessionGetterDisp = getter;
            g_sessionHotjoinOffset = Read<uint32_t>(hj, kSigSessionHotjoinDisp);
            g_sessionServerOffset = Read<uint8_t>(sv, kSigSessionServerDisp8);
        } else {
            Log("ERROR: session_hotjoin / session_server disagree on the session getter");
        }
    }
    for (size_t i = 0; i < 4; ++i) {
        char rtti[64];
        sprintf_s(rtti, ".?AV%s@@", kServerClasses[i]);
        size_t n = 0;
        g_serverVtables[i] = FindVtableByRtti(rtti, &n);
    }
}

// RTTI class name of a vtable (".?AVCFoo@@" -> "CFoo" is left to the caller), or null.
const char* VtableRttiName(void** vt) {
    uint8_t *rdata, *data;
    size_t rsize, dsize;
    if (!vt || !ImageSection(".rdata", &rdata, &rsize) || !ImageSection(".data", &data, &dsize)) return nullptr;
    if (!In(vt - 1, rdata, rsize)) return nullptr;
    uint8_t* col = (uint8_t*)vt[-1];
    if (!In(col, rdata, rsize - 24)) return nullptr;
    uint8_t* td = (uint8_t*)GetModuleHandleW(nullptr) + Read<uint32_t>(col, 12);
    return In(td + 0x10, data, dsize) ? (const char*)(td + 0x10) : nullptr;
}

// No C++ objects in the next three, so SEH can contain a fault.
bool ReadSessionRaw(void* idler, void*** serverVt, uint8_t* hotjoin) {
    __try {
        void** vt = *(void***)idler;
        uint8_t* session = ((SessionGetter_t)vt[g_sessionGetterDisp / 8])(idler);
        if (!session) return false;
        void* server = *(void**)(session + g_sessionServerOffset);
        *hotjoin = session[g_sessionHotjoinOffset];
        *serverVt = server ? *(void***)server : nullptr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool FindOnActionRaw(void* db, const EngineString* name, const EngineString* none, bool* defined) {
    __try {
        *defined = g_onActionFind(db, name) != g_onActionFind(db, none);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool FireOnActionRaw(void* db, const EngineString* name, int tag, DWORD* code) {
    alignas(16) uint8_t scope[kEventScopeBuffer];
    __try {
        void* list = g_onActionFind(db, name);
        uint32_t seed = g_syncedRandom("hoi4_loader on_game_loaded", 1);
        g_scopeCtor(scope, (int)(seed & 0x7fffffff));
        if (tag > 0) g_scopeSetCountry(scope, &tag, true);
        g_onActionFire(db, list, scope, false);
        g_scopeDtor(scope);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

bool ShortEngineString(const char* s, EngineString* out) {
    size_t n = strlen(s);
    if (n > 15) return false;
    memset(out, 0, sizeof *out);
    memcpy(out->buf, s, n);
    out->size = n;
    out->cap = 15;
    return true;
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
    pAppDoFrame = SigFindUnique("app_do_frame", kSigAppDoFrame);
    g_isLoaded = (IsLoaded_t)SigFindUnique("is_loaded_from_save", kSigIsLoadedFromSave);

    g_intern = (Intern_t)SigFindUnique("token_intern", kSigTokenIntern);
    g_tokenToString = (TokenToString_t)SigFindUnique("token_to_string", kSigTokenToString);

    if ((p = SigFindUnique("gamestate_and_flags", kSigGameStateAndFlags)) != nullptr) {
        g_ppGameState = (void**)SigRipTarget(p, kSigGameStatePtrRel, kSigGameStatePtrRel + 4);
        g_flagsOffset = Read<uint32_t>(p, kSigGameStateFlagsDisp);
    }
    g_flagSet = (FlagSet_t)SigFindUnique("flag_set", kSigFlagSet);
    g_flagClear = (FlagClear_t)SigFindUnique("flag_clear", kSigFlagClear);
    pFlagSet = (void*)g_flagSet;
    pFlagClear = (void*)g_flagClear;
    g_countryTagName = (CountryTagName_t)SigFindUnique("country_tag_name", kSigCountryTagName);

    pRegionGetNameKey = SigFindUnique("region_get_name_key", kSigRegionGetNameKey);
    if ((p = SigFindUnique("region_array", kSigRegionArray)) != nullptr) {
        g_regionCountOffset = Read<uint32_t>(p, kSigRegionCountDisp);
        g_regionArrayOffset = Read<uint32_t>(p, kSigRegionArrayDisp);
    }

    if ((p = SigFindUnique("map_manager_dirty", kSigMapManagerDirty)) != nullptr)
        g_ppMapManager = (void**)SigRipTarget(p, kSigMapManagerPtrRel, kSigMapManagerPtrRel + 4);
    g_localize = (Localize_t)SigFindUnique("localize", kSigLocalize);

    ResolveRegistry();
    ResolveNativeScript();
    ResolveOnActions();

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
        fn(FlagEntry{Read<int>(e, kFlagEntryToken), Read<int>(e, kFlagEntryDate), Read<short>(e, kFlagEntryValue),
                     Read<short>(e, kFlagEntryDays)},
           ctx);
    }
    return true;
}

void* GlobalFlags() { return HaveFlags() ? Flags() : nullptr; }

std::string CountryTagName(int tag) {
    if (!g_countryTagName || tag <= 0 || !GameState()) return std::to_string(tag);
    const EngineString* s = g_countryTagName(&tag);
    return s && s->size > 0 && s->size < 16 ? std::string(s->c_str(), (size_t)s->size) : std::to_string(tag);
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

bool HaveNumeric() {
    return HaveNativeScript() && numericCount.ctor && numericRatio.ctor && scriptValueEval && tokGreater > 0;
}

bool HaveNativeScript() {
    // Slots the binding relies on (NOTES.md C1): effect value reader slot 3 and trigger value
    // reader slot 5 must be real overrides of the base implementations.
    return HaveRegistry() && g_ppTriggerRegistry && g_registerTrigger && g_readString && g_unexpected && g_construct &&
           effectBase.slots >= 25 && triggerBase.slots >= 23 && effectTemplate.slots >= 25 &&
           effectValueReader.slots >= 25 && triggerTemplate.slots >= 23 && effectTemplateEntryVtable &&
           triggerTemplateEntryVtable && effectValueReader.vt[3] != effectBase.vt[3] &&
           triggerTemplate.vt[5] != triggerBase.vt[5];
}

bool EffectRegistered(int token) { return g_ppRegistry && MapHasKey(*g_ppRegistry, token); }
bool TriggerRegistered(int token) { return g_ppTriggerRegistry && MapHasKey(*g_ppTriggerRegistry, token); }

bool RegisterTrigger(int token, void* entry) {
    void* registry = g_ppTriggerRegistry ? *g_ppTriggerRegistry : nullptr;
    if (!registry || !g_registerTrigger || !token) return false;
    g_registerTrigger(registry, token, entry);
    return true;
}

void ParserReadString(void* parser, EngineString* out) { g_readString(parser, out, false); }
void ParserUnexpected(void* parser) { g_unexpected(parser); }

void ConstructEngineString(EngineString* raw, const char* p, size_t n) {
    memset(raw, 0, sizeof *raw);
    g_construct(raw, p, n);
}

bool HaveOnActions() {
    return g_ppGameState && g_ppOnActionDb && g_onActionFind && g_onActionFire && g_scopeCtor && g_scopeDtor &&
           g_scopeSetCountry && g_syncedRandom && g_playerTagOffset;
}

bool HaveSession() { return g_sessionGetterDisp && g_sessionServerOffset && g_sessionHotjoinOffset && g_serverVtables[0]; }

SessionInfo QuerySession(void* idler) {
    SessionInfo info;
    void** serverVt = nullptr;
    uint8_t hotjoin = 0;
    if (!HaveSession() || !idler || !ReadSessionRaw(idler, &serverVt, &hotjoin) || !serverVt) return info;
    info.known = true;
    info.hotJoin = hotjoin != 0;
    info.serverClass = "unknown";
    for (size_t i = 0; i < 4; ++i)
        if (g_serverVtables[i] && serverVt == g_serverVtables[i]) info.serverClass = kServerClasses[i];
    if (strcmp(info.serverClass, "unknown") == 0) {
        const char* rtti = VtableRttiName(serverVt);
        Log("Session: server object has an unexpected class (%s)", rtti ? rtti : "no RTTI");
    }
    info.singlePlayer = serverVt == g_serverVtables[0];
    return info;
}

int PlayerCountryTag() {
    void* gs = GameState();
    int tag = gs && g_playerTagOffset ? Read<int>(gs, g_playerTagOffset) : 0;
    return tag > 0 ? tag : 0;
}

bool OnActionDefined(const char* name) {
    EngineString s, none;
    void* db = g_ppOnActionDb ? *g_ppOnActionDb : nullptr;
    bool defined = false;
    if (!HaveOnActions() || !db || !ShortEngineString(name, &s) || !ShortEngineString(kNoSuchOnAction, &none)) return false;
    if (!FindOnActionRaw(db, &s, &none, &defined)) {
        Log("ERROR: looking up on_action %s crashed", name);
        return false;
    }
    return defined;
}

bool FireOnAction(const char* name, int countryTag) {
    EngineString s;
    void* db = g_ppOnActionDb ? *g_ppOnActionDb : nullptr;
    if (!HaveOnActions() || !db || !ShortEngineString(name, &s)) {
        Log("ERROR: cannot fire on_action %s (engine functions or database unavailable)", name);
        return false;
    }
    DWORD code = 0;
    if (!FireOnActionRaw(db, &s, countryTag, &code)) {
        Log("ERROR: firing on_action %s crashed (exception 0x%08lX)", name, code);
        return false;
    }
    return true;
}

}  // namespace Engine
