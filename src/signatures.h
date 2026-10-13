// Byte signatures and string anchors for engine-tier features.
//
// SIG(name, "hex ?? hex ..."): must match exactly once in hoi4.exe's .text. ?? = wildcard (rel32
//   call targets, RIP displacements, stack/struct offsets read at runtime).
// ANCHOR(name, "text"): a NUL-terminated string in .rdata referenced by exactly one
//   `lea r, [rip+x]` in .text; the referencing code is the anchor.
//
// tools/check_signatures.py reads every SIG/ANCHOR line and checks it against hoi4.exe copies.
// Keep one per line with the pattern as a single string literal. Offsets noted below are from
// the start of the match. Addresses are 1.19.3 for orientation only.
#pragma once

#define SIG(name, pattern) static const char name[] = pattern;
#define ANCHOR(name, text) static const char name[] = text;

// --- PhysFS (engine_probe) -----------------------------------------------------------------
// doOpen(): UTF-8 path -> CreateFileW. Function start. (0x142513a20)
SIG(kSigPhysfsDoOpen, "40 55 41 56 41 57 48 83 EC 40 48 8D 6C 24 40 48 89 5D 20 45 8B F0 48 89 75 28 44 8B FA 48 89 7D 30 48 8B D9 48 85 C9 0F 84")
// __PHYSFS_platformOpenRead(): the call to doOpen is the E8 at +15. (0x142514720)
SIG(kSigPhysfsOpenRead, "48 83 EC 28 BA 00 00 00 80 41 B8 03 00 00 00 E8 ?? ?? ?? ?? 33 C9 48 83 F8 FF 48 0F 44 C1 48 83 C4 28 C3")
static const size_t kSigPhysfsOpenReadCallOffset = 15;

// --- Game lifecycle ------------------------------------------------------------------------
// In-game idler "game entered": runs for a new game and for a loaded save; fires on_startup
// only when !IsLoadedFromSave(gamestate). Function start. (0x140dd9b20)
SIG(kSigGameEntered, "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D 6C 24 C9 48 81 EC 00 01 00 00 48 8B F9 E8 ?? ?? ?? ?? 48 8B 8F ?? ?? ?? ?? E8 ?? ?? ?? ?? E8")
// bool CGameState::IsLoadedFromSave() { return this->_bLoaded; } (0x1401e2930)
SIG(kSigIsLoadedFromSave, "0F B6 81 34 0A 00 00 C3")
// CSetGlobalFlagEffect::Execute: mov rax,[gamestate]; ...; mov rdi,[rax+<flags>].
// +3: rel32 of the gamestate pointer (insn ends at +7). +0x24: disp32 of the flags member.
SIG(kSigGameStateAndFlags, "48 8B 05 ?? ?? ?? ?? 48 8D 8D 50 01 00 00 45 33 C9 C6 44 24 20 00 4C 8B C6 48 8D 94 24 88 00 00 00 48 8B B8 ?? ?? ?? ??")
static const size_t kSigGameStatePtrRel = 3, kSigGameStateFlagsDisp = 0x24;

// --- Lexer tokens --------------------------------------------------------------------------
// Sets the "lexer synchronized initialized" byte at its end; dynamic tokens may be created only
// afterwards. Hooked to register native effects at the right moment. Function start. (0x140176f30)
SIG(kSigLexerSync, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8D AC 24 B0 F2 FF FF 48 81 EC 50 0E 00 00 48 8D 8D 20 05 00 00 E8 ?? ?? ?? ?? 90 33 F6 89 74 24 20 45 33 C9 4C 8D 05")
// int* Intern(int* outToken, const StringView* s): string -> (possibly new) token. (0x1424bb460)
SIG(kSigTokenIntern, "48 89 5C 24 10 57 48 83 EC 40 80 3D ?? ?? ?? ?? 00 48 8B FA 48 8B D9 75 ?? 80 3D ?? ?? ?? ?? 00 74 ?? 80 3D ?? ?? ?? ?? 00 75")
// const std::string* TokenToString(int token). (0x1424bc260)
SIG(kSigTokenToString, "40 53 48 83 EC 30 48 63 D9 80 3D ?? ?? ?? ?? 00 75 05 E8 ?? ?? ?? ?? 3B 1D ?? ?? ?? ?? 7E")

// --- Global flags --------------------------------------------------------------------------
// void SetFlag(CScriptFlags* flags, int token, short value, short days). (0x140cbfea0)
SIG(kSigFlagSet, "48 89 5C 24 08 57 48 83 EC 20 41 0F B7 F9 41 8B D8 E8 ?? ?? ?? ?? 66 89 58 28 66 85 FF 7E 04 66 89 78 2A")
// void ClearFlag(CScriptFlags* flags, int token): linear search (entries 0x30 bytes, token at
// +8) then erase. (0x140cbfe60)
SIG(kSigFlagClear, "48 63 41 14 45 33 C0 85 C0 7E ?? 4C 8D 51 08 4C 8B C8 49 8B 02 41 8B C8 48 83 C0 08 0F 1F 40 00 39 10 74 ?? 41 FF C0 48 FF C1 48 83 C0 30 49 3B C9 7C ?? C3")

// --- Strategic regions ---------------------------------------------------------------------
// const std::string* CStrategicRegion::GetNameKey(): custom name (+0x38, size +0x48) if set,
// else template(+0x30)->nameKey(+0x20). Whole function. (0x140e23410)
SIG(kSigRegionGetNameKey, "48 83 79 48 00 75 09 48 8B 41 30 48 83 C0 20 C3 48 8D 41 38 C3")
// Region by id inside a trigger: if (1 <= id < gs->regionCount) return gs->regions[id].
// +0x0e: disp32 of regionCount, +0x17: disp32 of regions array. (0x1413a78a3)
SIG(kSigRegionArray, "48 8B C2 48 C1 E8 3F 48 03 D0 83 FA 01 7C ?? 3B 93 ?? ?? ?? ?? 7D ?? 48 8B 83 ?? ?? ?? ?? 48 63 CA 48 8B 04 C8")
static const size_t kSigRegionCountDisp = 0x11, kSigRegionArrayDisp = 0x1a;

// --- Map / UI ------------------------------------------------------------------------------
// Map manager getter followed by its "names dirty" setter (*(u16*)(this+0x81) = 0x101), the same
// refresh set_state_name triggers. +3: rel32 of the manager pointer. (0x140e139b0)
SIG(kSigMapManagerDirty, "48 8B 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 66 C7 81 81 00 00 00 01 01 C3")
static const size_t kSigMapManagerPtrRel = 3;

// --- Localisation --------------------------------------------------------------------------
// std::string* Localize(std::string* out, const char* key, void* = 0, int = 0). (0x142245e60)
SIG(kSigLocalize, "48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 50 45 33 FF 41 8B E9 44 38 3D ?? ?? ?? ?? 4D 8B F0 48 8B F2 48 8B F9 74")

// --- Script parsing / engine strings (native effect and trigger API) -----------------------
// void ReadString(CParser* p, std::string* out, bool allowNewline): copies the current value's
// text ("Unreadable String" error for multi-line text unless allowed). (0x1424c0ab0)
SIG(kSigParserReadString, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 41 0F B6 D8 48 8B FA 48 8B F1 0F 57 C0 0F 11 44 24 20 66 0F 6F 0D ?? ?? ?? ?? F3 0F 7F 4C 24 30 C6")
// void Unexpected(CParser* p): reports "Unexpected token" (file/line in error.log) and skips the
// value, as set_province_name does for unknown keys. (0x1424c2060)
SIG(kSigParserUnexpected, "40 53 48 83 EC 40 48 8B D9 0F 57 C0 0F 11 44 24 20 B9 20 00 00 00 E8 ?? ?? ?? ?? 48 89 44 24 20 66 0F 6F 05 ?? ?? ?? ?? F3 0F 7F 44 24 30 0F 10")
// std::string* Construct(std::string* raw, const char* p, size_t n): builds an engine-owned
// string on raw storage (used by Localize for its result). (0x14011d970)
SIG(kSigStringConstruct, "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 41 56 48 83 EC 20 48 BE FF FF FF FF FF FF FF 7F 49 8B D8 48 8B EA 4C 8B F1 4C 3B C6 0F 87")

// --- Effect registry -----------------------------------------------------------------------
// set_province_name's static registration stub references its help text:
//   mov rbx,[registry]; ... lea rcx,[help]; mov [rax+8],rcx; lea rcx,[entry vtable];
//   mov [rax],rcx; mov r8,rax; mov edx,<token>; mov rcx,rbx; add rsp,20; pop rbx; jmp Register
ANCHOR(kAnchorRenameProvinceHelp, "set_province_name = { id = <province id> name = <string> } - Set name for a province")

// --- Trigger registry ----------------------------------------------------------------------
// has_global_flag's registration stub (same shape as the effect stub, 0x18-byte entry with a
// byte flag at +0x10): lea rcx,[help] at A; mov byte [rax+10h],1; mov [rax+8],rcx;
// lea rcx,[entry vtable] at A+0x0f; ...; mov edx,<token> at A+0x1c; ...; jmp Register at A+0x29;
// mov rbx,[registry] at A-0x2c.
ANCHOR(kAnchorGlobalFlagTriggerHelp, "has global flag been set.Check flag val date set and days since set.\nExample: has_global_flag = test_flag\nhas_global_flag = { \n\tflag = <name> (mandatory)\n\tvalue < <int> (optional)\n\tdate > <date> (optional)\n\tdays > <int> (optional)\n}")

// --- On actions / session (game_loaded; NOTES.md Part D) ------------------------------------
// In the game-entered function, new games only: build a scope for the local player, use it,
// destroy it:
//   xor edx,edx; lea rcx,[scope]; call CEventScope::CEventScope(scope, seed)   ; E8 at +0x06
//   mov edx,<observer tag offset>; mov eax,<player tag offset>                ; imm32 at +0x0d / +0x12
//   cmp dword [rsi+<player tag offset>],0; cmovg edx,eax; add rdx,rsi; mov r8b,1
//   lea rcx,[scope]; call SetCountry(scope, const int* tag, bool)              ; E8 at +0x2a
//   call ...; lea rdx,[scope]; mov rcx,rax; call ...; nop
//   lea rcx,[scope]; call CEventScope::~CEventScope(scope)                     ; E8 at +0x45
// (0x140dd9cf2)
SIG(kSigPlayerScope, "33 D2 48 8D 4D ?? E8 ?? ?? ?? ?? 90 BA ?? ?? 00 00 B8 ?? ?? 00 00 83 BE ?? ?? 00 00 00 0F 4F D0 48 03 D6 41 B0 01 48 8D 4D ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 55 ?? 48 8B C8 E8 ?? ?? ?? ?? 90 48 8D 4D ?? E8 ?? ?? ?? ??")
static const size_t kSigPlayerScopeCtorCall = 0x06, kSigPlayerScopeObserverImm = 0x0d, kSigPlayerScopePlayerImm = 0x12,
                    kSigPlayerScopePlayerDisp = 0x18, kSigPlayerScopeSetCountryCall = 0x2a, kSigPlayerScopeDtorCall = 0x45;
// Same function, before on_startup: if (!IsLoadedFromSave(gs)) { seed = SyncedRandom(file, line);
// CEventScope scope(seed & 0x7fffffff); ... }. SyncedRandom is the OOS-checked game RNG.
// E8 at +0x1c: SyncedRandom(const char* file, int line); E8 at +0x2b: CEventScope ctor. (0x140dd9d44)
SIG(kSigOnStartupSeed, "48 8B CE E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? BA ?? ?? 00 00 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F BA F0 1F 8B D0 48 8D 4D ?? E8 ?? ?? ?? ??")
static const size_t kSigOnStartupSeedRandomCall = 0x1c, kSigOnStartupSeedCtorCall = 0x2b;
// Same function: list = Find(db, std::string("on_startup")); Fire(db, list, &scope, false).
// +3: rel32 of the COnActionDataBase* global; +0x21: rel32 of the "on_startup" literal (checked);
// E8 at +0x44: Find(db, const std::string*); E8 at +0x56: Fire(db, list, scope, bool). (0x140dd9dd3)
SIG(kSigOnStartupFire, "48 8B 1D ?? ?? ?? ?? 0F 57 C0 0F 11 44 24 ?? 66 0F 6F 0D ?? ?? ?? ?? F3 0F 7F 4C 24 ?? F2 0F 10 05 ?? ?? ?? ?? F2 0F 11 44 24 ?? 0F B7 05 ?? ?? ?? ?? 66 89 44 24 ?? C6 44 24 ?? 00 48 8D 54 24 ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B D0 45 33 C9 4C 8D 45 ?? 48 8B CB E8 ?? ?? ?? ??")
static const size_t kSigOnStartupFireDbRel = 3, kSigOnStartupFireNameRel = 0x21, kSigOnStartupFireFindCall = 0x44,
                    kSigOnStartupFireFireCall = 0x56;
// playsession_start telemetry: session = idler->vtable[+disp32 at +8](); "hot_join" =
// session->bIsHotjoin (disp32 at +0x1c). (0x14021c06a)
SIG(kSigSessionHotjoin, "48 8B 06 48 8B CE FF 90 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 80 B8 ?? ?? ?? ?? 00 48 0F 45 D1")
static const size_t kSigSessionGetterDisp = 8, kSigSessionHotjoinDisp = 0x1c;
// Same telemetry: server = session->server (disp8 at +0xf), then dynamic_cast to CNetworkServer /
// CProxyServer ("multiplayer"). (0x14021bcad)
SIG(kSigSessionServer, "48 8B 06 48 8B CE FF 90 ?? ?? ?? ?? 48 8B 58 ?? C7 44 24 20 00 00 00 00 4C 8D 0D ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 33 D2 48 8B CB E8")
static const size_t kSigSessionServerDisp8 = 0xf;

#undef SIG
#undef ANCHOR
