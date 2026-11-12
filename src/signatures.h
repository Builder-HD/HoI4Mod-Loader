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

// sigreport.cpp defines these three macros itself before including this file, to list every
// entry for the read-only scan of unlisted builds.
#ifndef SIG
#define SIG(name, pattern) static const char name[] = pattern;
#endif
#ifndef ANCHOR
#define ANCHOR(name, text) static const char name[] = text;
#endif

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
// void CApplication::DoFrame(bool topLevel): one frame on the main thread (events, current idler,
// whose session executes commands, including the hourly tick). CApplication::Run calls it with
// true; long ticks call it with false to keep the UI alive. Function start. (0x14222eeb0)
SIG(kSigAppDoFrame, "40 53 55 56 57 41 56 48 83 EC 70 0F 29 74 24 60 0F 29 7C 24 50 44 0F 29 44 24 40 44 0F B6 F2 48 8B F1 48 8D 3D ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 74 ?? 80 3D ?? ?? ?? ?? 00 75 ?? E8 ?? ?? ?? ?? 84 C0 75 ??")
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
// std::string* Localize(std::string* out, const char* key, CPdxLocalizeKeyValuePair* args, int n).
// (0x142245e60) Arguments fill $NAME$ (FUN_142248c20 matches the name, FUN_1422479f0 formats):
// pair stride 0x68 = { int type @0 (1 = std::string value), const char* key @8, std::string key
// @0x10, std::string value @0x30, union @0x60 }; an unmatched $NAME$ is localised as a key.
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
// const std::string* CountryTagName(const int* tag): "FRA" etc. (the telemetry's "nation" field
// and many UI texts use it). Function start. (0x140bb4e70)
SIG(kSigCountryTagName, "40 53 48 83 EC 30 48 83 3D ?? ?? ?? ?? 00 48 8B D9 0F 84 ?? ?? ?? ?? 83 39 00 0F 84 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 75")

// CCountryTag from text (FUN_140bb4640): first the static tag database, db = [rip+disp32 at
// +0x15] (insn +0x12, 7 bytes), lookup(db, const std::string*) -> tag or -1 = E8 at +0x1e (a hash
// map read, FUN_14071bc80); then event targets. Used to parse literal tags such as GER.
SIG(kSigCountryTagFromText, "40 53 56 57 48 83 EC 30 33 DB 48 8B F9 89 19 48 8B F2 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? E8 ?? ?? ?? ?? 89 07 85 C0 0F 8F")
static const size_t kSigTagDbInsn = 0x12, kSigTagLookupCall = 0x1e;

// --- Data triggers: region_control (NOTES.md E1/E2) -----------------------------------------
// Region panel air superiority bar: country = TagToCountry(&tag) (E8 at +4); air = CountryAir(country)
// (E8 at +0xc); entry = AirRegionEntry(air, region) (E8 at +0x1b) = { int64 our, int64 enemy }.
// (0x1418811e8)
SIG(kSigAirSuperiorityUi, "48 8D 0C 38 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 49 8B 96 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 89 85 ?? ?? ?? ?? 48 8B 70 08 48 8B 38 4C 8D 04 37 4D 85 C0 7E ?? 48 B9 FE FF FF FF FF FF FF 7F")
static const size_t kSigAirUiTagToCountry = 4, kSigAirUiCountryAir = 0xc, kSigAirUiRegionEntry = 0x1b;
// has_naval_control evaluate: naval = region->[disp32 at +0x16]. (0x140528a50)
SIG(kSigNavalControlEval, "40 53 48 83 EC 30 48 8B DA E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 88 ?? ?? ?? ?? 48 85 C9 74 ?? 48 8D 53 08 E8 ?? ?? ?? ?? 83 F8 01 0F 94 C0")
static const size_t kSigNavalRegionDisp = 0x16;
// Naval control sums (FUN_141003f50): entries naval+0x40 (count +0x4c, 0x50 bytes, tag at +0,
// dominance int64 at +0x20); country = TagToCountry(entry) (E8 at +0x3f); relation =
// country->[disp32 +0x47]->relations(+8)[TagIndex(&tag) (E8 at +0x59)]; IsFriendly(relation)
// (E8 at +0x6c) -> friendly, else at war if relation->[disp32 +0x7e] war && !war[+0x49]. (0x141003f5a)
SIG(kSigNavalSums, "48 8B 71 40 45 33 FF 48 63 41 4C 41 8B FF 48 89 6C 24 58 41 8B DF 48 8B 69 78 4C 89 64 24 60 4C 8D 24 80 4C 89 6C 24 68 49 C1 E4 04 4C 8B EA 4C 03 E6 49 3B F4 74 ?? 4C 89 74 24 20 48 8B CE E8 ?? ?? ?? ?? 4C 8B B0 ?? ?? ?? ?? 45 39 7D 00 75 05 41 8B C7 EB 08 49 8B CD E8 ?? ?? ?? ?? 48 63 C8 49 8B 46 08 4C 8B 34 C8 49 8B CE E8 ?? ?? ?? ?? 84 C0 74 06 48 03 5E 20 EB 16 49 8B 86 ?? ?? ?? ?? 48 85 C0 74 0A 44 38 78 49 75 04 48 03 7E 20 48 83 C6 50")
static const size_t kSigNavalSumsTagToCountry = 0x3f, kSigNavalSumsDiplomacyDisp = 0x47, kSigNavalSumsTagIndex = 0x59,
                    kSigNavalSumsIsFriendly = 0x6c, kSigNavalSumsWarDisp = 0x7e;

// --- Data triggers: military_counts (NOTES.md E1/E2) ----------------------------------------
// divisions_in_state: state = gamestate->[disp32 +0x12][id], count [disp32 +9]. (0x1403ec5b6)
SIG(kSigStateArray, "48 8B 05 ?? ?? ?? ?? 3B 98 ?? ?? ?? ?? 7D 0D 48 8B 80 ?? ?? ?? ?? 48 8B 14 D8 EB 02 33 D2 48 85 D2 0F 85 ?? ?? ?? ?? 41 B9 00 10 00 00 41 B8 ?? ?? 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 ?? E8 ?? ?? ?? ?? 48 8B F0 48 8D 54 24")
static const size_t kSigStateCountDisp = 9, kSigStateArrayDisp = 0x12;
// divisions_in_state unit loop: provinces state->[disp8 +3] (count [disp8 +7]); land units
// province->[disp32 +0x1a] (count [disp32 +0x21]); owner tag unit->[disp32 +0x38]. (0x1403ec7ee)
SIG(kSigStateLandUnits, "4C 8B 72 ?? 48 63 42 ?? 4D 8D 3C C6 4D 3B F7 74 ?? 90 49 8B 06 33 DB 48 8B B8 ?? ?? ?? ?? 48 63 80 ?? ?? ?? ?? 48 8D 34 C7 48 3B FE 74 ?? 0F 1F 40 00 48 8B 0F 48 81 C1 ?? ?? ?? ?? 8B 01 41 8B 55 08 3B C2")
static const size_t kSigLandProvincesDisp = 3, kSigLandProvinceCountDisp = 7, kSigLandUnitsDisp = 0x1a,
                    kSigLandUnitCountDisp = 0x21, kSigLandOwnerDisp = 0x38;
// ships_in_area: region array/count (disp32 +0x19 / +9), provinces region->[disp8 +0x63] (count
// [disp8 +0x32]), naval units province->[disp32 +0x83] (count [disp32 +0x6d]), owner
// unit->[disp32 +0x92]; ships of a naval unit: E8 at +0xb6 -> array, count at [+0xc]. (0x1403eee00)
SIG(kSigShipsInArea, "48 8B 15 ?? ?? ?? ?? 3B 82 ?? ?? ?? ?? 0F 8D ?? ?? ?? ?? 48 8B C8 48 8B 82 ?? ?? ?? ?? 48 8B 0C C8 48 89 4C 24 ?? 48 85 C9 0F 84 ?? ?? ?? ?? 48 63 41 ?? 48 89 84 24 ?? ?? ?? ?? 48 85 C0 0F 8E ?? ?? ?? ?? 48 89 74 24 ?? 4C 89 64 24 ?? 45 33 E4 4C 89 74 24 ?? 4C 89 7C 24 ?? 0F 1F 44 00 00 48 8B 41 ?? 33 F6 4E 8B 34 E0 4D 63 BE ?? ?? ?? ?? 4D 85 FF 0F 8E ?? ?? ?? ?? 66 0F 1F 44 00 00 49 8B 86 ?? ?? ?? ?? 41 8B 55 08 48 8B 3C F0 48 8D 8F ?? ?? ?? ?? 8B 01 3B C2 74 ?? 85 C0 74 ?? 85 D2 74 ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 83 7D 58 00 48 8B CF 74 ?? E8 ?? ?? ?? ?? 4C 63 40 0C")
static const size_t kSigShipsRegionCountDisp = 9, kSigShipsRegionArrayDisp = 0x19, kSigShipsProvinceCountDisp = 0x32,
                    kSigShipsProvincesDisp = 0x63, kSigShipsNavalCountDisp = 0x6d, kSigShipsNavalUnitsDisp = 0x83,
                    kSigShipsOwnerDisp = 0x92, kSigShipsShipsCall = 0xb6;
// province->AddCombat(combat): combats province->[disp32 +0x22], count [disp32 +0x16]. (0x140e797f0)
SIG(kSigProvinceAddCombat, "48 89 6C 24 20 41 56 48 83 EC 20 48 8B E9 4C 8B F2 33 C9 4C 63 8D ?? ?? ?? ?? 45 85 C9 7E ?? 48 8B 85 ?? ?? ?? ?? 33 D2 4C 39 30")
static const size_t kSigProvinceCombatCountDisp = 0x16, kSigProvinceCombatsDisp = 0x22;
// unit->AddCombat(combat): combats unit->[disp32 +0x23], count [disp32 +0xe]. (0x140bf9670)
SIG(kSigUnitAddCombat, "48 89 6C 24 20 41 56 48 83 EC 20 4C 63 89 ?? ?? ?? ?? 45 33 C0 4C 8B F2 48 8B E9 45 85 C9 7E ?? 48 8B 81 ?? ?? ?? ?? 33 D2 0F 1F 80 00 00 00 00 4C 39 30")
static const size_t kSigUnitCombatCountDisp = 0xe, kSigUnitCombatsDisp = 0x23;
// num_planes_stationed_in_regions: air = CountryAir(country) (E8 at +0xa); wings air->[disp8 +0x12]
// (count [disp8 +0x16]); region = WingBaseRegion(wing) (E8 at +0x2f), region id [disp8 +0x3b];
// planes = WingPlanes(wing) (E8 at +0x68). (0x1403d1897)
SIG(kSigPlanesStationed, "E8 ?? ?? ?? ?? 48 8B C8 33 F6 E8 ?? ?? ?? ?? 48 8B 58 ?? 48 63 40 ?? 4C 8D 34 C3 49 3B DE 74 ?? 48 89 6C 24 ?? 0F 1F 40 00 48 8B 2B 48 8B CD E8 ?? ?? ?? ?? 33 D2 33 C9 44 8B 48 ?? 48 63 47 ?? 85 C0 7E ?? 4C 8B C0 48 8B 47 ?? 44 39 08 74 ?? FF C2 48 FF C1 48 83 C0 04 49 3B C8 7C ?? EB ?? 83 FA FF 74 ?? 48 8B CD E8 ?? ?? ?? ?? 03 F0")
static const size_t kSigPlanesTagToCountry = 0, kSigPlanesCountryAir = 0xa, kSigPlanesWingsDisp = 0x12,
                    kSigPlanesWingCountDisp = 0x16, kSigPlanesWingRegion = 0x2f, kSigPlanesRegionIdDisp = 0x3b,
                    kSigPlanesWingPlanes = 0x68;
// CCountry::DailyUpdate "army.daily": the divisions list country->[disp32 at lea+0x17], count
// [disp32 at lea+0x1e]; checked with kCodeArmyDailyLoop at lea+0x14.
ANCHOR(kAnchorArmyDaily, "army.daily")
static const size_t kArmyDailyLoopFromLea = 0x14, kArmyDailyListDisp = 0x17, kArmyDailyCountDisp = 0x1e;

// Flag get-or-create stores the current game date: date = gamestate->[disp32 +9] (hours).
// (0x140cbf8f5)
SIG(kSigGameDate, "48 8B 05 ?? ?? ?? ?? 8B 88 ?? ?? ?? ?? 89 4C 33 18 48 8B 45 08 48 03 C3 E9")
static const size_t kSigGameDateDisp = 9;

// --- Data triggers: production_data (NOTES.md E2) -------------------------------------------
// Equipment reference by name (has_equipment's resolver): if the database global (rip rel32 at
// +0x26, cmp qword ...,0 at +0x23, length 8) is set, def = Lookup(DbGetter() (E8 at +0x2d),
// token) (E8 at +0x37); a def counts only if its byte [disp8 +0x47] is set. (0x1413b0970)
SIG(kSigEquipmentLookup, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 60 49 8B E8 48 8B F2 48 8B F9 8B 1A 85 DB 74 ?? 48 83 3D ?? ?? ?? ?? 00 74 ?? E8 ?? ?? ?? ?? 8B D3 48 8B C8 E8 ?? ?? ?? ?? 48 89 47 10 48 85 C0 74 ?? 80 78 ?? 00")
static const size_t kSigEquipDbCmp = 0x23, kSigEquipDbGetter = 0x2d, kSigEquipLookup = 0x37, kSigEquipValidDisp = 0x47;
// production.daily_serial line loop: lines production->[disp8 +3] ... line->[disp8 +0x2a] is the
// amount still to build (-1 = unlimited; "Deleting non productive line" when 0), NOT factories.
// (0x140e67130)
SIG(kSigProductionLineFactories, "48 8B 43 ?? 48 8B 0C 07 48 8B 01 FF 50 50 48 8B 43 ?? 48 8B 0C 07 48 8B 01 FF 90 F0 00 00 00 48 8B 43 ?? 48 8B 0C 07 44 39 71 ?? 75")
static const size_t kSigLineFactoriesListDisp = 3;
// Production::SetLineFactories (0x140e611b0) tail: the assigned factories are split into two line
// fields, mov [rdi+disp8 +4],edx; mov [rdi+disp8 +7],ecx (and re-zeroed via disp8 +0xe); the rest of
// the requested count (+0x15, disp32) is queued ("nNewQueuedFactories >= 0").
SIG(kSigLineAssignFactories, "2B CA 89 57 ?? 89 4F ?? 85 D2 79 ?? C7 47 ?? 00 00 00 00 8B 9F ?? ?? ?? ?? 2B D9 2B DA")
static const size_t kSigAssignFieldA = 4, kSigAssignFieldB = 7, kSigAssignFieldAAgain = 0xe;
// "production.daily_serial" profiler zone; kCodeProductionDaily at lea+0x13: lines
// production->[disp8 +0x1d] (count [disp8 +0x21]).
ANCHOR(kAnchorProductionDaily, "production.daily_serial")
static const size_t kProductionDailyCodeFromLea = 0x13, kProductionDailyLinesDisp = 0x1d, kProductionDailyCountDisp = 0x21;

// --- Scope blocks and iterators (region_scope, NOTES.md F5) --------------------------------
// CEventScope::SetStrategicRegion(scope, CStrategicRegion*, bool clear): clear the other scope
// fields if asked, then mov [rbx+0x48],rdi (the region field Scope::types reads). (0x14053b670)
SIG(kSigScopeSetRegion, "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 45 84 C0 74 ?? E8 ?? ?? ?? ?? 48 89 7B 48 48 8B 5C 24 30 48 83 C4 20 5F C3 48 8B 5C 24 30 48 89 79 48")
// Console region command: cmp qword [rbx+<sea share>],0; jg; else "Selected region has no sea
// province" (length 0x23). The share (fixed point) of the region's provinces that are sea; disp32
// at +3. (0x140235c46)
SIG(kSigRegionSeaShare, "48 83 BB ?? ?? ?? ?? 00 7F ?? 49 8D 4F 08 41 B8 23 00 00 00 48 8D 15")
static const size_t kSigRegionSeaShareDisp = 3;

// --- Armies (create_army, NOTES.md G1) ----------------------------------------------------
// start_border_war: the country's army manager is **(country+disp32) (its own handle at +8);
// disp32 at +3. Then mov rax,[rip+x] loads the null handle (rel32 at +0xd, insn at +0xa).
// (0x140337197)
SIG(kSigBorderWarManager, "48 8B 80 ?? ?? ?? ?? 48 8B 08 48 8B 05 ?? ?? ?? ?? 48 89 45 ?? 48 85 C9 0F 84 ?? ?? ?? ?? 48 8B 41 08")
static const size_t kSigBorderWarManagerDisp = 3, kSigBorderWarNullHandle = 0xa;
// start_border_war: army = create_army(&null theatre, &manager handle, &unit handles, false), call
// at +0x1b. (0x140337237)
SIG(kSigBorderWarArmy, "48 8B 05 ?? ?? ?? ?? 48 89 45 ?? 45 33 C9 4C 8D 44 24 ?? 48 8D 55 ?? 48 8D 4D ?? E8 ?? ?? ?? ?? 48 8B D8")
static const size_t kSigBorderWarCreateCall = 0x1b;
// --- Railways (railways, NOTES.md Part H) --------------------------------------------------
// Addresses in this section are 1.19.4.2 (v1.19.4.2.2da5), the build they were found on.
// Gamestate setup: new CSupplySystem (E8 at +0) stored at gamestate+[disp32 +0x10] (again at
// +0x27), then new CRailwayManager (constructor E8 at +0x42) stored at gamestate+[disp32 +0x52]
// (again at +0x68). (0x140202a40)
SIG(kSigRailGameStateInit, "E8 ?? ?? ?? ?? 48 8B F8 EB 03 48 8B FD 48 8B 8B ?? ?? ?? ?? 48 85 C9 74 0B 48 8B 01 BA 01 00 00 00 FF 50 08 48 89 BB ?? ?? ?? ?? B9 38 00 00 00 E8 ?? ?? ?? ?? 48 89 44 24 ?? 48 85 C0 74 0D 48 8B C8 E8 ?? ?? ?? ?? 48 8B F8 EB 03 48 8B FD 48 8B 8B ?? ?? ?? ?? 48 85 C9 74 0A 48 8B 01 BA 01 00 00 00 FF 10 48 89 BB ?? ?? ?? ??")
static const size_t kSigRailSupplyDisp = 0x10, kSigRailSupplyStoreDisp = 0x27, kSigRailManagerCtorCall = 0x42,
                    kSigRailManagerDisp = 0x52, kSigRailManagerStoreDisp = 0x68;
// CRailwayManager constructor: sizes its CProvinceRailwayInfo* array (manager+[disp8 +0x21]) to the
// gamestate's province count (gamestate pointer rip rel32 at +3, length 7; count disp32 at +0xa)
// and stores the size at manager+[disp8 +0x11]. (0x140f0532e, inside the constructor 0x140f051e0)
SIG(kSigRailManagerCtor, "48 8B 05 ?? ?? ?? ?? 48 63 A8 ?? ?? ?? ?? 49 63 46 ?? 3B E8 7E ?? 41 B8 01 00 00 00 8B D5 49 8D 4E ?? E8")
static const size_t kSigRailCtorGameStateRel = 3, kSigRailProvinceCountDisp = 0xa, kSigRailInfoCountDisp = 0x11,
                    kSigRailInfoArrayDisp = 0x21;
// CProvinceRailwayInfo::SetRailway(neighbour, level) (0x140f08540), three parts. Neighbour lookup:
// mov rcx,[rcx+<province> disp8 +3]; ...; E8 at +9 = NeighborIndex(province, id). (0x140f085c1)
SIG(kSigRailSetNeighbor, "48 8B 49 ?? 48 89 6C 24 ?? E8 ?? ?? ?? ?? 48 63 E8 85 C0 79")
static const size_t kSigRailProvinceDisp = 3, kSigRailNeighborIndexCall = 9;
// After the MAX_RAILWAY_LEVEL clamp: per-neighbour levels int[] at info+[disp8 +3] (again at +0x15),
// the sorted (neighbour, level) pairs at info+[disp8 +0x19], their count at info+[disp8 +0x11].
// (0x140f0872c)
SIG(kSigRailSetLevels, "48 8B 47 ?? 41 39 0C 06 7E 04 41 89 0C 06 48 63 57 ?? 4C 8B 4F ?? 4C 8B 57 ?? 4D 03 CE 4C 8B 74 24 ?? 4D 8B C2 4C 8D 1C D5 00 00 00 00 48 85 D2 7E ??")
static const size_t kSigRailLevelsDisp = 3, kSigRailPairCountDisp = 0x11, kSigRailLevelsAgainDisp = 0x15,
                    kSigRailPairsDisp = 0x19;
// The mirror rail_way building at info+[disp8 +3] (stored again at +0x28), created on first use;
// E8 at +0x32 = CBuilding::SetLevel(building, short level, bool). (0x140f087ef)
SIG(kSigRailSetMirror, "48 8B 4F ?? 48 85 C9 75 ?? 85 F6 74 ?? 48 8B 5F ?? E8 ?? ?? ?? ?? 48 8B CB 48 8B 90 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 89 47 ?? 48 8B C8 41 B0 01 0F B7 D6 E8")
static const size_t kSigRailMirrorDisp = 3, kSigRailMirrorStoreDisp = 0x28, kSigRailSetLevelCall = 0x32;
// int NeighborIndex(province, id): linear search of province+0x70 (stride 0x30, id at +8, count
// +0x7c); -1 if not a neighbour. Whole function. (0x1414a6290)
SIG(kSigRailNeighborIndex, "48 63 41 ?? 45 33 C9 85 C0 7E ?? 4C 8B D0 45 8B C1 48 8B 41 ?? 48 83 C0 ?? 0F 1F 80 00 00 00 00 3B 10 74 ?? 41 FF C1 49 FF C0 48 83 C0 ?? 4D 3B C2 7C ?? B8 FF FF FF FF C3")
// build_railway's path update (0x140f05780) tail: for each province on the path,
// gamestate(rip rel32 at +3)->[disp32 +0xd] (CSupplySystem)->vtable[disp8 +0x2e](supply, province):
// marks the province and its rail neighbours dirty in the supply system. (0x140f0597c)
SIG(kSigRailSupplyNotify, "48 8B 05 ?? ?? ?? ?? 48 8B 1E 48 8B B8 ?? ?? ?? ?? 48 63 46 0C 48 8D 34 83 48 3B DE 74 1A 66 0F 1F 44 00 00 48 8B 07 48 8B CF 8B 13 FF 50 ??")
static const size_t kSigRailNotifySupplyDisp = 0xd, kSigRailNotifySlotDisp = 0x2e, kSigRailNotifyGameStateRel = 3;
// The supply system's full refresh (after loading) ends with the railway graphics: ui (rip rel32 at
// +3, length 7) -> vtable[disp32 +0x11]() -> [disp8 +0x18] -> [disp32 +0x1c] = the railway
// graphics; Clear(gfx) (E8 at +0x28: every railway piece, the moving trains and their entities),
// then Rebuild(gfx) (E9 at +0x35: a piece for every province with a railway). Vanilla never
// removes a railway; after it does, the loader runs the same pair so nothing on the map still
// refers to a removed piece (NOTES.md H4). (0x140f46dea)
SIG(kSigRailGfxRefresh, "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 FF 90 ?? ?? ?? ?? 48 8B 48 ?? 48 8B 99 ?? ?? ?? ?? 48 85 DB 74 ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B CB 48 83 C4 20 5B E9")
static const size_t kSigRailGfxUiRel = 3, kSigRailGfxUiSlotDisp = 0x11, kSigRailGfxViewDisp = 0x18,
                    kSigRailGfxDisp = 0x1c, kSigRailGfxClearCall = 0x28, kSigRailGfxRebuildJmp = 0x35;

// --- Railway construction lines (railways: queued dismantling, NOTES.md H3) -----------------
// End of CBuildRailwayCommand execute, for the player's country: ui (rip rel32 at +0x10, length 7)
// -> vtable[disp32 +0x19]() -> Post(that, &event 0x11) (E8 at +0x30): the construction view
// refreshes now instead of at its next routine update. (0x1411dcf25)
SIG(kSigRailUiRefresh, "49 8D 4D 28 E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? ?? ?? 48 85 C0 74 ?? C7 45 ?? 11 00 00 00 48 8D 55 ?? 48 8B C8 E8")
static const size_t kSigRailUiRefreshUiRel = 0x10, kSigRailUiRefreshSlotDisp = 0x19, kSigRailUiRefreshPostCall = 0x30;
static const int kRailUiConstructionChanged = 0x11;
// CBuildRailwayCommand execute (slot 10): country = TagToCountry(&command.tag) (E8 at +4), then
// CreateRailwayLine(country->[disp32 +0xc] (construction), path, from, to, flag) (E8 at +0x24).
// (0x1411dcbba)
SIG(kSigRailCreateLine, "49 8D 4D ?? E8 ?? ?? ?? ?? 48 8B 88 ?? ?? ?? ?? 49 8D 55 ?? 41 0F B6 45 ?? 88 44 24 20 45 8B 4D ?? 45 8B C6 E8")
static const size_t kSigRailTagToCountryCall = 4, kSigRailConstructionDisp = 0xc, kSigRailCreateLineCall = 0x24;
// CRailwayProductionLine::Init(path, from, to) tail: base level at line+[disp8 +6], target at
// line+[disp8 +9]. (0x141a96142)
SIG(kSigRailLineInit, "48 8B CE 44 89 7E ?? 89 6E ?? 48 8B 6C 24 ?? 48 83 C4 20 41 5F 41 5E 5E E9")
static const size_t kSigRailLineBaseDisp = 6, kSigRailLineTargetDisp = 9;
// CRailwayProductionLine daily progress (vtable slot 24), function start: the link being worked,
// line+[disp32 +0x21], is reset to -1 first. Hooked. (0x141a944c0)
SIG(kSigRailLineDaily, "40 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 83 EC 70 8B 3D ?? ?? ?? ?? 48 8D 35 ?? ?? ?? ?? C7 81 ?? ?? ?? ?? FF FF FF FF")
static const size_t kSigRailLineCurrentDisp = 0x21;
// Its head: path count line+[disp32 +0xa], manager gamestate+[disp32 +0x11], the line's country
// line+[disp8 +0x18], the country's tag holder country+[disp32 +0x20] (tag at holder+[disp8
// +0x2b]), path line+[disp32 +0x3b]; PathUsable(tag*, first, last) E8 at +0x49; if it fails,
// jmp [vtable+disp32 +0x69] (cancel). (0x141a945cc)
SIG(kSigRailLineDailyHead, "48 8B 05 ?? ?? ?? ?? 49 63 8E ?? ?? ?? ?? 4C 8B B8 ?? ?? ?? ?? 49 8B 46 ?? 4C 89 7D ?? 48 8B 80 ?? ?? ?? ?? 48 89 45 ?? 48 8D 58 ?? 8D 41 FF 48 89 5D ?? 83 F8 01 7C ?? 49 8B 96 ?? ?? ?? ?? 44 8B 44 8A FC 48 8B CB 8B 12 E8 ?? ?? ?? ?? 84 C0 75 ?? 49 8B 06 49 8B CE 48 83 C4 ?? 41 5F 41 5E 41 5C 5F 5E 5B 5D 48 FF A0 ?? ?? ?? ??")
static const size_t kSigRailHeadCountDisp = 0xa, kSigRailHeadManagerDisp = 0x11, kSigRailHeadCountryDisp = 0x18,
                    kSigRailHeadHolderDisp = 0x20, kSigRailHeadTagDisp = 0x2b, kSigRailHeadPathDisp = 0x3b,
                    kSigRailHeadPathUsableCall = 0x49, kSigRailHeadCancelDisp = 0x69;
// The per-link control check in the same loop: path line+[disp32 +3]; ProvinceUsable(tag*,
// province) E8 at +0x14. (0x141a94660)
SIG(kSigRailLineControl, "49 8B 86 ?? ?? ?? ?? 48 8B CB 8B 74 02 04 4C 63 2C 02 8B D6 E8 ?? ?? ?? ?? 84 C0 0F 84")
static const size_t kSigRailControlPathDisp = 3, kSigRailControlCall = 0x14;
// After the link loop: call [vtable+disp8 +0x17], the end-of-day refresh (progress, then the
// output bookkeeping). The production manager removes the line once slot 25 reports it finished.
// (0x141a94aef)
SIG(kSigRailLineAfterDaily, "89 4D ?? 48 89 55 ?? 3B C8 0F 8C ?? ?? ?? ?? 49 8B 06 49 8B CE FF 50 ??")
static const size_t kSigRailAfterDailySlotDisp = 0x17;
// In the link loop: the day's output, call [vtable+disp8 +0xf] (the output getter, slot 14 on the
// checked build; its shape is kCodeRailLineOutput). (0x141a94822)
SIG(kSigRailLineDailyOutput, "49 8B 06 48 8D 55 E0 45 32 E4 49 8B CE FF 50 ?? 48 8B 38")
static const size_t kSigRailDailyOutputSlotDisp = 0xf;
// CRailwayProductionLine progress recompute (called by Init, SetTarget and the daily refresh),
// function start. Hooked. (0x141a961b0)
SIG(kSigRailLineProgress, "4C 8B DC 53 56 57 41 54 41 55 48 81 EC 90 00 00 00 45 33 E4 32 DB 38 1D ?? ?? ?? ?? 4C 8B E9 41 8B F4")
// Its tail: total cost line+[disp8 +3], remaining line+[disp32 +0xd] (saved), done line+[disp8
// +0x1e], then call [vtable+disp32 +0x25](line, levels still to build) (stores the count the
// target-change command checks, then refreshes the UI), then jmp SetSpeed(line, speed) (E9 at
// +0x43): the line's daily progress ("vSpeed"), which slot 14 returns to the daily step.
// (0x141a9650c)
SIG(kSigRailLineProgressTail, "49 8B 4D ?? 49 8B C4 48 2B CE 49 89 B5 ?? ?? ?? ?? 48 85 C9 48 0F 4F C1 49 8B CD 49 89 45 ?? 49 8B 45 00 FF 90 ?? ?? ?? ?? 84 DB 49 8B CD 4C 0F 44 E7 49 8B D4 48 81 C4 ?? ?? ?? ?? 41 5D 41 5C 5F 5E 5B E9")
static const size_t kSigRailTotalDisp = 3, kSigRailRemainingDisp = 0xd, kSigRailDoneDisp = 0x1e,
                    kSigRailRefreshSlotDisp = 0x25, kSigRailSetSpeedJmp = 0x43;
// In the same function, per link still to build: LinkSpeed(line, int64* out, province, next, 0) (E8
// at +0x18): the daily progress on that link. The line's speed is the first such link's; 0 if any
// link reports less than 1. (0x141a96497)
SIG(kSigRailLineSpeed, "44 8B 44 24 ?? 48 8D 54 24 ?? 45 8B CB C6 44 24 20 00 49 8B CD 49 03 F6 E8")
static const size_t kSigRailLinkSpeedCall = 0x18;
// Building one railway level: Cost(int64* out, level, country->[disp32 +0x13] (tag holder)), E8 at
// +0x17; the line's country at line+[disp8 +3]. (0x141a96450)
SIG(kSigRailLineCost, "4D 8B 45 ?? 44 8D 7D 01 41 8B D7 48 8D 4C 24 ?? 4D 8B 80 ?? ?? ?? ?? E8")
static const size_t kSigRailCostCountryDisp = 3, kSigRailCostHolderDisp = 0x13, kSigRailCostCall = 0x17;
// The construction queue's railway entry tooltip builder (item, hovered element, out), function
// start: line = LookupLine(item+[disp8 +0x41]) (E8 at +0x4f); base level 0 -> "RAILWAY_CONSTUCTION_
// TOOLTIP", else "RAILWAY_UPGRADE_TOOLTIP" (START_LEVEL, TARGET_LEVEL) through Localize. Hooked.
// (0x141de2200)
SIG(kSigRailTooltip, "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 0F 29 B4 24 ?? ?? ?? ?? 49 8B F0 4C 8B E2 4C 8B F1 33 C0 89 85 ?? ?? ?? ?? 89 85 ?? ?? ?? ?? 48 83 C1 ?? 39 01 75 09 39 41 04 0F 84 ?? ?? ?? ?? E8")
static const size_t kSigRailTooltipIdDisp = 0x41, kSigRailTooltipLookupCall = 0x4f;
// The upgrade text key; its one lea must lie inside the tooltip builder.
ANCHOR(kAnchorRailUpgradeTooltip, "RAILWAY_UPGRADE_TOOLTIP")
// The railway queue entry's update (item), function start: line = LookupLine(item+[disp32 +0x23])
// (E8 at +0x34; the same lookup as the tooltip builder's). Sets the title ("A - B"), the progress
// bar, the level buttons and the scorched icon. Hooked. (0x141ded950)
SIG(kSigRailEntryUpdate, "48 89 5C 24 18 55 56 57 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 4C 8B F1 33 DB 89 5D ?? 48 81 C1 ?? ?? ?? ?? 39 19 75 09 39 59 04 0F 84 ?? ?? ?? ?? E8")
static const size_t kSigRailEntryIdDisp = 0x23, kSigRailEntryLookupCall = 0x34;
// In it, the title: SetText(item->[disp32 +3] (title text element), std::string* text) (E8 at
// +0x1b; the string is the caller's temporary copy). (0x141deda04)
SIG(kSigRailEntryTitle, "49 8B BE ?? ?? ?? ?? 48 8D 55 ?? 48 8D 4D ?? E8 ?? ?? ?? ?? 48 8D 55 ?? 48 8B CF E8")
static const size_t kSigRailEntryTitleDisp = 3, kSigRailEntrySetTextCall = 0x1b;

// --- Who may dismantle where (railway_queue game rules, NOTES.md H5) ------------------------
// CIsSubjectOfTrigger evaluate: country = TagToCountry(&scope tag) (E8 at +4), its diplomacy at
// country+[disp32 +0x17], relations diplomacy->[disp8 +0x3b][TagIndex(&target) (E8 at +0x30)],
// IsSubject(relation) (E8 at +0x40): the scope country is a subject of the target. (0x1403fa285)
SIG(kSigRailSubjectOf, "48 8D 4A 08 E8 ?? ?? ?? ?? 4C 8B C3 48 8D 54 24 ?? 48 8B CF 48 8B B0 ?? ?? ?? ?? 48 8B 07 FF 90 ?? ?? ?? ?? 83 38 00 75 04 33 C0 EB 08 48 8B C8 E8 ?? ?? ?? ?? 48 63 C8 48 8B 46 ?? 48 8B 0C C8 E8")
static const size_t kSigRailSubjectTagToCountry = 4, kSigRailSubjectDiplomacyDisp = 0x17, kSigRailSubjectTagIndex = 0x30,
                    kSigRailSubjectRelationsDisp = 0x3b, kSigRailSubjectIsSubject = 0x40;
// CIsInFactionWithTrigger evaluate: diplomacy country+[disp32 +3], faction diplomacy+[disp32 +0xa]
// (null: none); same faction = same pointer. (0x1403f911b)
SIG(kSigRailFaction, "48 8B 88 ?? ?? ?? ?? 48 83 B9 ?? ?? ?? ?? 00 74 ?? 48 8D 4C 24 ?? E8 ?? ?? ?? ?? 48 8B 88 ?? ?? ?? ?? 48 83 B9 ?? ?? ?? ?? 00 74")
static const size_t kSigRailFactionDiplomacyDisp = 3, kSigRailFactionDisp = 0xa;
// CBuildRailwayCommand execute: province = gamestate+[disp8 +3] -> vtable[0](id); its controller tag
// at province+[disp32 +0x16]. (0x1411dca83)
SIG(kSigRailController, "48 8D 4F 08 48 89 4D ?? 48 8B 01 8B D3 FF 10 48 89 45 ?? 48 8D 88 ?? ?? ?? ?? 44 8B 01 41 8B 55 ??")
static const size_t kSigRailProvincesDisp = 3, kSigRailControllerDisp = 0x16;

// --- Railway map badges (CRailwayMapIcon: the level circle on each railway segment) ---------
// Left click, function start: Clickable(icon) (E8 at +0x19: the player may build here, railway
// construction view), then the segment's province path at icon+[disp32 +0x29] (CPdxArray<int>,
// count at +0xc). Vanilla: queue an upgrade (Shift: to the maximum). Hooked. (0x141992860)
SIG(kSigRailIconClick, "48 89 5C 24 08 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 30 48 8B F1 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 4C 8D BE ?? ?? ?? ??")
static const size_t kSigRailIconClickableCall = 0x19, kSigRailIconPathDisp = 0x29;
// The badge update, function start (both province objects at icon+[disp32 +0x2e] and +0x3b must
// exist). Hooked. (0x141992ce0)
SIG(kSigRailIconUpdate, "48 89 5C 24 10 55 56 57 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 0F 29 B4 24 ?? ?? ?? ?? 48 8B F9 33 F6 89 B5 ?? ?? ?? ?? 48 39 B1 ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 48 39 B1 ?? ?? ?? ?? 0F 84")
// In it, the level text: Colorize(std::string* out, char 'W' or 'O', string_view*) (E8 at +0x18),
// SetText(icon->[disp32 +0x2b] (text element), that string) (E8 at +0x23), then clear the hidden
// flag (and byte [element+disp8 +0x31], imm8 +0x32: 0x75 and ~0x10 on the checked build) and call
// [vtable+disp8 +0x38] (show). (0x141993061)
SIG(kSigRailIconText, "BA 57 00 00 00 B8 4F 00 00 00 45 3B FE 0F 4F D0 4C 8D 45 ?? 48 8D 4D ?? E8 ?? ?? ?? ?? 48 8B D0 48 8B CB E8 ?? ?? ?? ?? 48 8B 8F ?? ?? ?? ?? 80 61 ?? ?? 48 8B 01 FF 50 ??")
static const size_t kSigRailIconColorizeCall = 0x18, kSigRailIconSetTextCall = 0x23, kSigRailIconTextDisp = 0x2b,
                    kSigRailIconFlagsDisp = 0x31, kSigRailIconShownMask = 0x32, kSigRailIconShowSlotDisp = 0x38;
// FindRailwayLine(path, tag): country(E8 at +3)->[disp32 +0xb] (construction), its lines at
// +[disp8 +0x12], count +[disp8 +0x16]. (0x14157d37f)
SIG(kSigRailLineList, "48 8B CA E8 ?? ?? ?? ?? 48 8B 90 ?? ?? ?? ?? 4C 8B 72 ?? 48 63 42 ?? 4D 8D 24 C6 4D 3B F4")
static const size_t kSigRailListConstructionDisp = 0xb, kSigRailListLinesDisp = 0x12, kSigRailListCountDisp = 0x16;

// --- Time control (time_control, NOTES.md Part K; addresses 1.19.4.2, 1.19.3 unchecked) --------
// Console `gamespeed <0-5>`: n = atoi(arg); if (n > 5) error; paused = idler->IsPaused();
// if (n == 0) { if (!paused) idler->TogglePause(1); ... }. The bound is the number of speeds.
// imm8 at +0xc: speeds; +0x13: mov rcx,[rip+idler] (rel32 at +0x16, insn 7); +0x1f: IsPaused
// vtable disp32; +0x2f: the same idler load again; +0x40: TogglePause vtable disp32. (0x140274878)
SIG(kSigConsoleSpeedPause, "48 8B 0A E8 ?? ?? ?? ?? 8B F0 83 F8 ?? 0F 87 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? ?? ?? 85 F6 0F 85 ?? ?? ?? ?? 84 C0 75 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 BA 01 00 00 00 FF 90 ?? ?? ?? ??")
static const size_t kSigConsoleSpeedCountImm = 0xc, kSigConsoleIdlerLoad = 0x13, kSigConsoleIsPausedDisp = 0x1f,
                    kSigConsoleIdlerLoad2 = 0x2f, kSigConsoleToggleDisp = 0x40;
// Same command, n > 0: if (paused) idler->TogglePause(1); RequestGameSpeed(idler, n - 1, true).
// +4: idler load (rel32 at +7); +0x15: TogglePause disp32; +0x22: idler load; E8 at +0x29: the
// request (refuses for observers / game over and for MP clients, then posts CSetGameSpeedCommand).
// (0x140274a25)
SIG(kSigConsoleSpeedSet, "84 C0 74 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 BA 01 00 00 00 FF 90 ?? ?? ?? ?? 8D 56 FF 41 B8 01 00 00 00 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ??")
static const size_t kSigConsoleSetIdlerLoad = 4, kSigConsoleSetToggleDisp = 0x15, kSigConsoleSetIdlerLoad2 = 0x22,
                    kSigConsoleSetRequestCall = 0x29;
// void CGameState::SetGameSpeed(int speed): clamp to 0..max (imm32 at +0x14), store (disp32 at
// +0x1f), notify the UI. The only writer of the field; the three speed commands call it on the
// main thread (ThreadIsMainThread assert). Function start. (0x140203f10)
SIG(kSigSetGameSpeed, "40 55 48 83 EC 40 4C 89 74 24 30 85 D2 79 04 33 D2 EB 0A B8 ?? 00 00 00 3B D0 0F 4F D0 89 91 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90")
static const size_t kSigSetGameSpeedMaxImm = 0x14, kSigSetGameSpeedFieldDisp = 0x1f;
// CIncreaseGameSpeedCommand::Execute tail: SetGameSpeed(gamestate, gamestate->speed + 1) as a
// tail jmp. +3: gamestate rel32 (insn 7); +0xc: speed field disp32; E9 at +0x27. (0x140f85b86)
SIG(kSigIncreaseSpeedTail, "48 8B 05 ?? ?? ?? ?? 48 8B CF 8B 90 ?? ?? ?? ?? FF C2 48 8B 5C 24 40 48 8B 6C 24 50 48 8B 7C 24 58 48 83 C4 30 41 5E E9 ?? ?? ?? ??")
static const size_t kSigIncreaseGameStateRel = 3, kSigIncreaseSpeedDisp = 0xc, kSigIncreaseSetSpeedJmp = 0x27;
// void CTopBar::UpdateDate(): SetText(this->DateText, FormatDate(&gamestate->date)). Called from
// the top bar's game state listener when the date changes, and from its setup. Function start;
// disp32 at +0x10: the DateText widget member. (0x14192f460)
SIG(kSigTopBarUpdateDate, "40 57 48 83 EC 50 80 3D ?? ?? ?? ?? 00 48 8B B9 ?? ?? ?? ?? 0F 84")
static const size_t kSigTopBarDateTextDisp = 0x10;
// The top bar's pause button (and its Space shortcut): this->idler->TogglePause(1) (idler member
// disp8 at +0x11, TogglePause disp32 at +0x17), then jmp to the pause button refresh at +0x23.
// (0x14192efc0)
SIG(kSigTopBarPauseClick, "40 53 48 83 EC 20 48 8B D9 BA 01 00 00 00 48 8B 49 ?? 48 8B 01 FF 90 ?? ?? ?? ?? 48 8B CB 48 83 C4 20 5B E9")
static const size_t kSigTopBarPauseIdlerDisp = 0x11, kSigTopBarPauseToggleDisp = 0x17, kSigTopBarPauseRefreshJmp = 0x23;
// CTopBar setup hides a whole window (the music player's): window->widget.Hide(), widget at
// window + disp8 (+3, again at +7), Hide vtable disp32 at +0xa; or byte [window+flags],bit (disp32
// at +0x10 = widget + the widget flags byte, imm8 at +0x14); then the next lookup loads the top
// bar's root window (disp8 at +0x18). (0x14192d487)
SIG(kSigTopBarHideWindow, "48 8B 46 ?? 48 8D 4E ?? FF 90 ?? ?? ?? ?? 80 8E ?? ?? ?? ?? ?? 48 8B 4F ??")
static const size_t kSigHideWindowWidgetDisp = 3, kSigHideWindowWidgetDisp2 = 7, kSigHideWindowHideDisp = 0xa,
                    kSigHideWindowFlagsDisp = 0x10, kSigHideWindowBit = 0x14, kSigHideWindowRootDisp = 0x18;

// --- Province building removal (province_building_removal, NOTES.md Part I3) ----------------
// CProvinceBuildingItem update (0x141e462a0), function start, hooked: window = item+[disp8 +0x31],
// its button finder is window vtable +[disp8 +0x5f] ("building_picture"), building type =
// item+[disp8 +0x70].
SIG(kSigPbUpdate, "48 89 5C 24 18 48 89 74 24 20 55 57 41 55 41 56 41 57 48 8D 6C 24 80 48 81 EC 80 01 00 00 4C 8B E9 45 33 FF 41 8B F7 44 89 BD B8 00 00 00 48 8B 49 10 48 8B 01 48 8D 15 ?? ?? ?? ?? 48 89 54 24 30 48 C7 44 24 38 10 00 00 00 0F 28 44 24 30 66 0F 7F 44 24 30 41 B0 01 48 8D 54 24 30 FF 50 68 48 8B D8 48 8B 08 4C 8B 81 B0 00 00 00 49 8B 4D 28 8B 91 E4 02 00 00")
static const size_t kSigPbUpdateWindowDisp = 0x31, kSigPbUpdateFinderSlotDisp = 0x5f, kSigPbUpdateTypeDisp = 0x70;
// CProvinceBuildingItem tooltip (second vtable slot 0, 0x141e41e80), hooked. `this` is the tooltip
// sub-object: type = this+[disp8 +0x2d], state view = this+[disp8 +0x31]; the type is a province
// building if type+[disp32 +0x34] > 0; then province = view+[disp32 +0x3e], its buildings at
// province+[disp32 +0x45]; else state = view+[disp32 +0x4e]. Window = this+[disp8 +0x5c] (-8).
SIG(kSigPbTooltip, "48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8D 6C 24 C0 48 81 EC 40 01 00 00 4D 8B F0 48 8B F2 48 8B F9 C7 45 60 00 00 00 00 48 8B 41 10 48 8B 59 08 83 B8 BC 02 00 00 00 7E 10 48 8B 9B 00 06 00 00 48 81 C3 90 01 00 00 EB 0E 48 8B 9B 08 06 00 00 48 81 C3 20 01 00 00 48 8B 49 F8 48 8B 01")
static const size_t kSigPbTipTypeDisp = 0x2d, kSigPbTipViewDisp = 0x31, kSigPbTipProvinceFlagDisp = 0x34,
                    kSigPbTipProvinceDisp = 0x3e, kSigPbTipBuildingsDisp = 0x45, kSigPbTipStateDisp = 0x4e,
                    kSigPbTipWindowDisp = 0x5c;
// CContainerWindow's button finder (window vtable slot 13 forwards to it; 0x142332250): looks the
// name up in window+[disp32 +0x21] with Lookup (E8 at +0x32), which returns null for a missing name
// and logs nothing. "Could not find" is logged only with the flag set (line 0x3d1 at +0x4e).
SIG(kSigPbButtonFind, "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 60 41 0F B6 D8 48 8B F2 48 8B F9 48 81 C1 ?? ?? ?? ?? 0F 10 02 0F 29 44 24 30 48 8D 54 24 30 E8 ?? ?? ?? ?? 48 85 C0 0F 85 ?? ?? ?? ?? 84 DB 0F 84 ?? ?? ?? ?? 41 B9 00 10 00 00 41 B8 D1 03 00 00")
static const size_t kSigPbButtonMapDisp = 0x21, kSigPbButtonLookupCall = 0x32;
// CButtonWrapper(out, button, std::function<void()>* click, std::function<void()>* other)
// (0x142359ed0), function start; stores its vtable (lea rip rel32 at +0x37) at out+0.
SIG(kSigButtonWrapperCtor, "4C 89 4C 24 20 4C 89 44 24 18 48 89 4C 24 08 55 53 56 57 41 54 41 56 41 57 48 8D AC 24 D0 FA FF FF 48 81 EC 30 06 00 00 49 8B F1 4D 8B F0 4C 8B FA 48 8B F9 48 8D 05 ?? ?? ?? ?? 48 89 01 45 33 E4 4C 89 61 08 48 8D 59 10 48 89 5C 24 20 33 D2 41 B8 80 05 00 00")
static const size_t kSigButtonWrapperVtableLea = 0x34;
// CStateSharedSlotBuildingItem destructor (0x141e41b00): its two CButtonWrapper members at
// this+[disp32 +0x27] and this+[disp8 +0x33], each destroyed by the same function (E8 at +0x2b and
// +0x34). The members' distance is the wrapper's size (0x5d8).
SIG(kSigSharedSlotDtor, "48 89 5C 24 08 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B F9 48 89 01 8B DA 48 8D 05 ?? ?? ?? ?? 48 89 41 18 48 81 C1 28 06 00 00 E8 ?? ?? ?? ?? 48 8D 4F 50 E8")
static const size_t kSigSharedSlotSecondDisp = 0x27, kSigSharedSlotFirstDisp = 0x33, kSigSharedSlotDtorCall1 = 0x2b,
                    kSigSharedSlotDtorCall2 = 0x34;
// Building container lookup by type (0x141202610), the whole function: the province's or state's
// building of a type, or null.
SIG(kSigBuildingLookup, "80 7A 20 00 74 24 83 79 2C 00 74 1E 48 63 92 E0 02 00 00 48 8B 41 20 4C 63 04 90 41 83 F8 FF 74 09 48 8B 41 38 4A 8B 04 C0 C3 33 C0 C3")
// Building removal permission (0x140a25c50): (state, building, std::string* why or null) -> true
// if a level may be removed now (false during the wartime removal cooldown; then *why says until
// when). Function start; BUILDING_SLOT_REMOVE_COOLDOWN is referenced inside.
SIG(kSigRemovePermission, "48 89 5C 24 08 48 89 7C 24 10 4C 89 74 24 18 55 48 8D AC 24 70 FF FF FF 48 81 EC 90 01 00 00 49 8B F8 48 8B D9 C7 44 24 34 00 00 00 00 48 8B CA E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 8D 8B C8 00 00 00 E8")
ANCHOR(kAnchorRemoveCooldown, "BUILDING_SLOT_REMOVE_COOLDOWN")
// CRemoveBuildingLevelCommand execute (0x1411e2e77), before it removes a level: the state's last
// removal date state+[disp32 +0xf] = gamestate (rip rel32 at +3)->[disp32 +9] (today), which the
// permission check's wartime cooldown counts from.
SIG(kSigRemoveCooldownStamp, "48 8B 05 ?? ?? ?? ?? 8B 88 68 04 00 00 89 8E 70 08 00 00 0F B7 53 40 66 85 D2 7E")
static const size_t kSigStampGameStateRel = 3, kSigStampDateDisp = 9, kSigStampStateDisp = 0xf;
// CRemoveBuildingLevelCommand execute, last: when the player's country controls the state, ui (rip
// rel32 at +3, length 7) -> vtable[disp32 +0x3e]() -> Post(that, &event imm32 +0x5c) (E8 at +0x63):
// the state view shows the change now instead of at the next tick. (0x1411e2ead)
SIG(kSigRemoveUiRefresh, "48 8B 1D ?? ?? ?? ?? 48 8B CB 48 8D 78 08 48 8B 03 FF 90 A0 00 00 00 44 8B 07 8B 10 44 3B C2 74 15 45 85 C0 74 42 85 D2 74 3E 48 8B CF E8 ?? ?? ?? ?? 84 C0 74 32 48 8B 03 48 8B CB FF 90 B8 00 00 00 48 85 C0 74 21 48 8B 03 48 8B CB FF 90 B8 00 00 00 48 8D 54 24 50 C7 44 24 50 00 00 00 00 48 8B C8 E8")
static const size_t kSigRemoveUiRel = 3, kSigRemoveUiSlotDisp = 0x3e, kSigRemoveUiEventDisp = 0x5c,
                    kSigRemoveUiPostCall = 0x63;
// The shared slot cross's click (0x141e47d3e): the confirmation dialog. Dialog manager =
// view+[disp8 +3]->[disp32 +7]->[disp32 +0xe]; dialog = new(imm32 +0x13 bytes) (E8 at +0x17);
// CConfirmRemoveBuildingLevel(dialog, ui (rip rel32 at +0x43, length 7), state->[disp8 +0x3f]
// (state id), type->[disp32 +0x38] (type index), mode) (E8 at +0x4a); Open(manager, dialog) (E8
// at +0x58).
SIG(kSigConfirmRemoveOpen, "48 8B 43 38 48 8B 88 F0 05 00 00 4C 8B B1 F8 04 00 00 B9 90 11 00 00 E8 ?? ?? ?? ?? 48 89 44 24 70 48 85 C0 74 2C 48 8B 4B 20 48 8B 91 08 06 00 00 89 74 24 20 44 8B 8F E0 02 00 00 44 8B 42 58 48 8B 15 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B E8 48 8B D5 49 8B CE E8")
static const size_t kSigConfirmViewDisp = 3, kSigConfirmMgrDisp1 = 7, kSigConfirmMgrDisp2 = 0xe, kSigConfirmSizeImm = 0x13,
                    kSigConfirmAllocCall = 0x17, kSigConfirmTypeIndexDisp = 0x38, kSigConfirmStateIdDisp = 0x3f,
                    kSigConfirmUiRel = 0x43, kSigConfirmCtorCall = 0x4a, kSigConfirmOpenCall = 0x58;
// remove_building's loop: level = building+[disp8 +3] (short), then SetLevel(building, level - 1,
// notify = 1) (E8 at +0x12).
SIG(kSigRemoveBuildingLoop, "0F B7 57 40 66 3B F2 7D 14 66 FF CA 41 B0 01 48 8B CF E8 ?? ?? ?? ?? FF CB 85 DB 7F E3")
static const size_t kSigRemoveLevelDisp = 3, kSigRemoveSetLevelCall = 0x12;
// The shared slot's cross, shown and hidden (0x141e4774c): Hide = vtable +[disp32 +2], then
// or byte [element+disp8 +8],imm8 +9 (the hidden flag); Show = and that byte, then vtable +[disp8
// +0x3e].
SIG(kSigGuiHideShow, "FF 92 80 00 00 00 80 4F 75 10 48 8D 4D E0 E8 ?? ?? ?? ?? 41 03 C4 49 8B 0E 4C 8B 81 B0 00 00 00 39 46 34 41 0F 9E C7 41 8D 57 01 49 8B CE 41 FF D0 41 80 66 75 EF 49 8B 06 49 8B CE FF 50 78")
static const size_t kSigGuiHideDisp = 2, kSigGuiFlagsDisp = 8, kSigGuiHiddenBit = 9, kSigGuiShowDisp = 0x3e;
// The shared slot's hover (0x141e44d69): the player's tag (gamestate +0x520 or +0x524), state =
// view+[disp32 +0x1e], then owner = TagToCountry(state+[disp32 +0x2e]) (E8 at +0x32), checked
// against the player before the cross shows. The state's owner tag is read from here; the view's
// state field must be the one the tooltip reads.
SIG(kSigStateOwnerCheck, "BB ?? ?? ?? ?? B9 ?? ?? ?? ?? 83 B8 ?? ?? ?? ?? 00 0F 4F D9 48 03 D8 48 8B 47 ?? 4C 8B B0 ?? ?? ?? ?? 4D 85 F6 0F 84 ?? ?? ?? ?? 49 8D 8E ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B F0 8B 0B")
static const size_t kSigOwnerViewStateDisp = 0x1e, kSigOwnerStateOwnerDisp = 0x2e;
// --- Equipment conversion (equipment_conversion, NOTES.md Part J) --------------------------
// BuildConvertFromList(CEquipmentType*, database) (0x140cf2490), the hook target: count = 0
// (mov [rcx+disp32],r13d, disp at +0x2f), then the equal range of the type's base key (E8 at +0x3e).
// kAnchorConvertFromList's lea lies inside it.
SIG(kSigConvertListBuild, "48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? 0F 29 70 C8 4C 8B E2 48 8B E9 45 33 ED 44 89 A9 ?? ?? ?? ?? 4C 8B C1 48 8D 54 24 ?? 49 8B CC E8")
static const size_t kSigConvListCountDisp = 0x2f;
ANCHOR(kAnchorConvertFromList, " in can_convert_from list for type ")
static const size_t kConvListAnchorWithin = 0x500;  // the anchor's lea: at most this far into the function
// The per-type post-load (0x140cf2110): generation depth = parent hops. parent = [rcx+disp32 +0x1c]
// (root: itself), again in the loop at +0x44; it ends with BuildConvertFromList (E8 within 0x300).
SIG(kSigEquipPostLoad, "40 53 56 57 48 83 EC 40 8B 91 ?? ?? ?? ?? 33 F6 89 91 ?? ?? ?? ?? 48 8B D9 48 8B 81 ?? ?? ?? ?? 48 89 6C 24 68 48 3B C1 74 ?? 66 0F 1F 44 00 00 03 90 ?? ?? ?? ?? 48 8B C8 89 93 ?? ?? ?? ?? FF C6 48 8B 80 ?? ?? ?? ?? 48 3B C1")
static const size_t kSigPostLoadParentDisp = 0x1c, kSigPostLoadParentLoopDisp = 0x44, kPostLoadCallsBuildWithin = 0x300;
// CanConvert(targetDesign, sourceDesign) (0x140c2cb00), the hook target: design->type [disp32 +0xf],
// design->parent design [disp32 +0x16], type "derived" byte [disp32 +0x1d]. The list test inside is
// kCodeCanConvertList.
SIG(kSigCanConvert, "4C 8B CA 48 3B CA 0F 84 ?? ?? ?? ?? 4C 8B 91 ?? ?? ?? ?? 48 8B 81 ?? ?? ?? ?? 41 80 BA ?? ?? ?? ?? 00 74 ?? 48 85 C0 74 ?? 0F 1F 80 00 00 00 00 48 8B C8 48 8B 80 ?? ?? ?? ?? 48 85 C0 75 F1")
static const size_t kSigCanConvertTypeDisp = 0xf, kSigCanConvertParentDisp = 0x16;
// IsNaval(CEquipmentType*) (0x140cf5570): recursion through the archetype [disp32 +0xc] (E8 at
// +0x15 calls itself), archetype again at +0x3c; then the unit-type bits tested against the naval
// mask (mov rcx,0x80004003c1, the mask ConversionCost also picks its naval formula by). Four
// sibling tests (other masks) share everything before the mask.
SIG(kSigEquipIsNaval, "40 53 48 83 EC 20 48 8B D9 48 8B 89 ?? ?? ?? ?? 48 3B CB 74 ?? E8 ?? ?? ?? ?? 84 C0 74 ?? B0 01 48 83 C4 20 5B C3 48 8B 83 ?? ?? ?? ?? 48 8B 8B ?? ?? ?? ?? 48 85 C0 75 ?? 48 8B 83 ?? ?? ?? ?? 48 8B 80 ?? ?? ?? ?? 48 0B C8 E8 ?? ?? ?? ?? 48 B9 C1 03 40 00 80 00 00 00")
static const size_t kSigIsNavalArchetypeDisp = 0xc, kSigIsNavalSelfCall = 0x15, kSigIsNavalArchetypeDisp2 = 0x3c;
// ConversionCost(sourceDesign, out cost, targetDesign, organisation, out breakdown[8]) (0x140c29eb0),
// the hook target: *cost = 0 (mov [rdx],rsi), the resource list at cost+8 constructed. The naval
// chassis part inside is kCodeConvCostChassis.
// A production line's source rebuild RebuildSources(line, const int* tag) (0x1419c2c30), the hook
// target for the role mode: line target design [disp32 +0x2b], its type [disp32 +0x3b], parent
// design [disp32 +0x42], the type's list count [disp32 +0x4b] (convert only if one is set).
SIG(kSigLineRebuildSources, "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 2B E0 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 8B 83 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 88 ?? ?? ?? ?? 48 83 B8 ?? ?? ?? ?? 00 75 ?? 83 B9 ?? ?? ?? ?? 00 0F 84")
static const size_t kSigRebuildTargetDisp = 0x2b, kSigRebuildTypeDisp = 0x3b, kSigRebuildParentDisp = 0x42,
                    kSigRebuildCountDisp = 0x4b;
// CProductionLineMilitaryItem's `convert` click (0x141de8e30), the hook target for Shift+click: the
// item's line [disp32 +0x15] (again at +0x2a), the line's "Convert from Stockpile" byte [disp32 +0x31].
SIG(kSigConvertClick, "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B D9 48 8B 89 ?? ?? ?? ?? 48 85 C9 74 ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8B 8B ?? ?? ?? ?? 0F B6 81 ?? ?? ?? ?? 84 C0 40 0F 94 C7")
static const size_t kSigClickLineDisp = 0x15, kSigClickLineDisp2 = 0x2a, kSigClickToggleDisp = 0x31;
// The line's convert tooltip ConvertTooltip(line, ?, std::string* out) (0x1419c2fd0), the hook target
// for the mode line: line target design [disp32 +0x39]. kAnchorConvertHeader's lea is inside.
SIG(kSigConvertTooltip, "48 89 5C 24 18 48 89 54 24 10 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 C0 48 81 EC ?? ?? ?? ?? 49 8B F8 4C 8B E9 45 33 E4 44 89 A5 ?? ?? ?? ?? 4C 8B 89 ?? ?? ?? ?? 4D 85 C9 0F 84")
static const size_t kSigTooltipTargetDisp = 0x39;
ANCHOR(kAnchorConvertHeader, "PRODUCTION_LINE_CONVERT_HEADER")
// The tooltip's per-source entry (0x141ac3390) names the source with DesignName(design, std::string*
// out) (0x140c30510): kCodeConvVariantName before and after this anchor's lea.
ANCHOR(kAnchorConvertOrderEntry, "PRODUCTION_LINE_CONVERT_ORDER_OF_CONVERSION_ENTRY")
static const size_t kConvOrderEntryNameBefore = 0x180, kConvOrderEntryNameAfter = 0x200;
static const size_t kConvTooltipAnchorWithin = 0x100, kConvTooltipAppendWithin = 0x300;
SIG(kSigConversionCost,"4C 89 4C 24 20 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 49 8B F9 4D 8B F8 4C 8B E2 4C 8B E9 33 F6 89 74 24 ?? 48 89 32 48 8D 4A 08 E8 ?? ?? ?? ?? C7 44 24 ?? 01 00 00 00 48 89 75 ?? 4C 8D 45 ?? BA 4D 00 00 00 49 8B CF E8")

// --- Code shapes at RTTI-found addresses (numeric triggers, NOTES.md E2) -------------------
// CODE(name, "..."): checked with SigMatchAt at an address reached through an RTTI vtable, not
// scanned (so not required to be unique). tools/check_code_shapes.py checks them on exe copies.
#ifndef CODE
#define CODE(name, pattern) static const char name[] = pattern;
#endif
// CTriggerEntry<T>::Create (entry vtable slot 1): new(size) T. Size imm32 at +7, operator new
// E8 at +0xb, constructor E8 at +0x20, lea of T's vtable at +0x25 (rel32 at +0x28, length 7).
CODE(kCodeTriggerFactory, "40 53 48 83 EC 20 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D8 48 89 44 24 38 48 85 C0 74 ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 03")
static const size_t kCodeFactorySizeImm = 7, kCodeFactoryNewCall = 0xb, kCodeFactoryCtorCall = 0x20,
                    kCodeFactoryVtableLea = 0x25;
// CIntTrigger::Evaluate (slot 22): right side = flag +0x70 ? ScriptValue(+0x68)/100000 : int +0x60;
// E8 at +0x30 = ScriptValueEval(holder, int64* out, scope, 0, false).
CODE(kCodeIntCompare, "40 57 48 83 EC 30 80 79 70 00 48 89 5C 24 48 48 8B D9 48 89 74 24 50 48 8B F2 74 ?? 4C 8B C2 C6 44 24 20 00 48 8D 54 24 40 48 83 C1 68 45 33 C9 E8")
static const size_t kCodeIntCompareEvalCall = 0x30;
// Numeric ReadClause (slot 5, shared by CIntTrigger and CValueTrigger): op = parser+0x78 stored
// at this+0x58; imm32 at +0x20 = -token('>'); '<' is the next token. Then slot 4(parser+0xc0).
CODE(kCodeNumericReadClause, "4C 8B DC 49 89 5B 10 49 89 73 18 57 48 81 EC ?? ?? ?? ?? 48 8B FA 48 8B F1 8B 42 78 89 41 58 05 ?? ?? ?? ?? 83 F8 01")
static const size_t kCodeNumericOpImm = 0x20;
// CValueTrigger::ReadValue (slot 4): right side read into the CScopedVariable at this+0x60.
CODE(kCodeValueReadValue, "48 89 5C 24 08 57 48 83 EC 40 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? 84 C0 75 ?? 48 8D 4F 60")
// CTrigger block ReadClause (base slot 5): "cmp dword [parser+0xc0], 3" (3 = block value),
// somewhere in the first 0x80 bytes.
CODE(kCodeBlockValueCheck, "83 BE C0 00 00 00 03")
// CArmy strength / organisation ratio (RTTI .?AVCArmy@@ slots 34 and 39): cur (slot at +0x1f) *
// 100000 / max (slot at +0x34), clamped to 0..1. Both slots must have this shape.
CODE(kCodeArmyRatio, "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B 01 48 8B FA 48 8D 54 24 30 48 8B D9 FF 90 ?? ?? ?? ?? 4C 8B 03 48 8D 54 24 38 48 8B CB 48 8B F0 41 FF 90 ?? ?? ?? ?? 48 8B 08 48 85 C9")
// CArmy fighting strength (slot 35), what the division UI shows as "Strength": the strength ratio
// (call [rax+disp32 at +0x1a], must be slot 34) * the equipment ratio [rbx+disp32 at +0x30] / 1e5,
// clamped to 0..1.
CODE(kCodeArmyFightingStrength, "48 89 5C 24 10 57 48 83 EC 20 48 8B 01 48 8B FA 48 8D 54 24 30 48 8B D9 FF 90 ?? ?? ?? ?? 4C 8B 00 48 B8 09 E1 D1 C6 11 6B F1 29 4C 0F AF 83")
static const size_t kCodeArmyFightingInnerSlotDisp = 0x1a;
// After the "production.daily_serial" lea (+0x13): the production object's line list.
CODE(kCodeProductionDaily, "48 8D 8B ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 83 ?? ?? ?? ?? 4C 8B B0 ?? ?? ?? ?? 48 8B 7B ?? 48 63 43 ?? 48 8D 34 C7 48 3B FE 74 ?? 49 8B D6 48 8B 0F E8")
// CNumOfAvailableMilitaryFactoriesTrigger slot 23: production = TagToCountry(tag)->[disp32 +0x32].
CODE(kCodeAvailableMilitaryFactories, "40 53 48 83 EC 20 48 8B 01 48 8B DA 4C 8B C2 48 8D 54 24 30 FF 50 40 83 7C 24 30 00 7F 07 8B 43 08 89 44 24 30 48 8D 4C 24 30 E8 ?? ?? ?? ?? 48 8B 88 ?? ?? ?? ??")
static const size_t kCodeAvailMilTagToCountry = 0x2a, kCodeAvailMilProductionDisp = 0x32;
// CMilitaryProductionLine slot 11: equipment line->[disp32 +3], its type ->[disp32 +0xa], the
// type's archetype ->[disp32 +0x23].
CODE(kCodeLineEquipment, "48 8B 81 ?? ?? ?? ?? 48 8B 90 ?? ?? ?? ?? 48 8B 82 ?? ?? ?? ?? 48 8B C8 48 C1 E9 20 84 C9 75 ?? 48 8B 82 ?? ?? ?? ??")
static const size_t kCodeLineEquipmentDisp = 3, kCodeLineTypeDisp = 0xa, kCodeLineArchetypeDisp = 0x23;
// CNavalProductionLine slot 11: mov rcx,[rcx+<equipment>]; jmp ... (same equipment field).
CODE(kCodeNavalLineEquipment, "48 8B 89 ?? ?? ?? ?? E9")
// The divisions loop right after the "army.daily" profiler zone (kAnchorArmyDaily, lea+0x14):
// mov rbx,[rdi+list]; movsxd rax,[rdi+count]; ...; call [rax+0x98] per division.
CODE(kCodeArmyDailyLoop, "48 8B 9F ?? ?? ?? ?? 48 63 87 ?? ?? ?? ?? 48 8D 34 C3 48 3B DE 74 ?? 0F 1F 40 00 48 8B 0B 48 8B 01 FF 90 98 00 00 00")

// --- Region scope templates (NOTES.md F5): the vanilla state iterators ----------------------
// CAnyStateTrigger slot 22: child scope = CEventScope copy of the scope (rdx; E8 at +0x26), then
// the parent's ROOT (mov rax,[rsi+0x18]) and FROM.
CODE(kCodeAnyStateEvaluate, "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 81 EC ?? ?? ?? ?? 48 8B F2 48 8B F9 48 8D 4C 24 ?? E8 ?? ?? ?? ?? 90 48 8B 46 18")
static const size_t kCodeAnyStateScopeCopyCall = 0x26;
// CEveryStateEffect / CRandomStateEffect slot 13: candidates.count (+0xdc) = 0, then slot 25
// (collect) via call [rax+0xc8]. The candidates are a CPdxArray at +0xd0 { T** data, int cap,
// int count @+0xc, allocator* @+0x10 }.
CODE(kCodeTargetEffectExecute, "48 89 5C 24 10 48 89 74 24 18 57 48 81 EC ?? ?? ?? ?? 48 8B ?? 48 8B ?? C7 81 DC 00 00 00 00 00 00 00 48 8B 01 FF 90 C8 00 00 00")
// The `limit` key's handler in the every / random item handler: cmp dword [rcx+0x8c],0, else
// "Multiple limits in target effect": the limit trigger is embedded at +0x78 (child count +0x14).
CODE(kCodeTargetEffectLimit, "83 B9 8C 00 00 00 00 0F 84")
// CEveryStateEffect scalar deleting destructor: operator delete(this, size), E8 at +0x21.
CODE(kCodeScalarDeletingDtor, "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ?? ?? ?? ?? F6 C3 01 74 ?? BA ?? ?? ?? ?? 48 8B CF E8")
static const size_t kCodeDtorDeleteCall = 0x21;
// CProvinceBuildingItem scalar deleting destructor (its vtable slot 0, 0x141e41a20), hooked: stores
// the vtable (lea rel32 at +0xd) and the tooltip sub-object's vtable (lea at +0x19) at this+[disp8
// +0x23].
CODE(kCodePbDtor, "48 89 5C 24 08 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B F9 48 89 01 8B DA 48 8D 05 ?? ?? ?? ?? 48 89 41 18 48 83 C1 38 E8")
static const size_t kCodePbDtorVtableLea = 0xa, kCodePbDtorTipVtableLea = 0x19, kCodePbDtorTipSubDisp = 0x23;
// Inside the CProvinceBuildingItem update (kSigPbUpdate, within 0x400 bytes; the custom icon item
// has the same code): the picture's enable test. IsEnabled = vtable +[disp32 +6], Enable +[disp32
// +0x16], Disable +[disp32 +0x2e] (the GUI element's vtable).
CODE(kCodePbUpdateEnable, "84 C0 74 18 FF 92 18 02 00 00 84 C0 ?? ?? 48 8B 03 48 8B CB FF 90 98 02 00 00 EB 16 FF 92 18 02 00 00 84 C0 ?? ?? 48 8B 03 48 8B CB FF 90 A0 02 00 00")
static const size_t kCodePbIsEnabledDisp = 6, kCodePbEnableDisp = 0x16, kCodePbDisableDisp = 0x2e,
                    kCodePbUpdateEnableRange = 0x400;
// CConfirmRemoveBuildingLevel (the "Remove Building" dialog), reached through kSigConfirmRemoveOpen
// and its RTTI vtable (NOTES.md I4):
// constructor (0x141fe6290): vtable lea rel32 at +0x26; state id at this+[disp32 +0x45], type
// index at this+[disp32 +0x4b], mode at this+[disp32 +0x55].
CODE(kCodeConfirmCtor, "48 89 5C 24 10 48 89 74 24 18 48 89 4C 24 08 57 48 83 EC 20 41 8B F9 41 8B D8 48 8B F1 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 06 48 8D 05 ?? ?? ?? ?? 48 89 46 10 48 8D 05 ?? ?? ?? ?? 48 89 46 38 89 9E ?? ?? ?? ?? 89 BE ?? ?? ?? ?? 8B 44 24 50 89 86 ?? ?? ?? ??")
static const size_t kCodeConfirmCtorVtableLea = 0x23, kCodeConfirmStateDisp = 0x45, kCodeConfirmTypeDisp = 0x4b,
                    kCodeConfirmModeDisp = 0x55;
// vtable slot 0, scalar deleting destructor (0x141fe6320), hooked: stores the vtable (lea at +0xa).
CODE(kCodeConfirmDtor, "48 89 5C 24 08 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 8B DA 48 89 01 48 8B F9")
static const size_t kCodeConfirmDtorVtableLea = 0xa;
// vtable slot 1, still valid (0x141fe6bf0; the dialog goes when it says no), hooked.
CODE(kCodeConfirmValid, "48 89 5C 24 20 57 41 54 41 57 48 83 EC 30 80 3D ?? ?? ?? ?? 00 48 8B D9 41 BC E8 20 00 00")
// vtable slot 16, OK (0x141fe6380; posts CRemoveBuildingLevelCommand for the state building), hooked.
CODE(kCodeConfirmAccept, "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 30 48 8B F1 41 BC E8 20 00 00 41 BF 10 00 00 00 44 8B 35")
// Both read the state id the constructor stores: movsxd rax,[reg+disp32] within 0x200 bytes.
CODE(kCodeConfirmReadState, "48 63 ?? ?? ?? 00 00")
static const int kConfirmDtorSlot = 0, kConfirmValidSlot = 1, kConfirmAcceptSlot = 16;
static const size_t kCodeConfirmReadRange = 0x200;

// CRailwayProductionLine slot 14 (shared getter): *out = this->[disp8 +3], the construction output
// assigned to the line today (fixed point). (0x140c17400)
CODE(kCodeRailLineOutput, "48 8B 41 ?? 48 89 02 48 8B C2 C3")
static const size_t kCodeRailLineOutputDisp = 3;
// CRailwayProductionLine cancel (the slot kSigRailLineDailyHead jumps to): mov byte [rcx+disp32 +2],1;
// ret. That byte (saved) makes slot 25 report the line finished. (0x141a944b0)
CODE(kCodeRailLineCancel, "C6 81 ?? ?? ?? ?? 01 C3")
static const size_t kCodeRailLineCancelFlagDisp = 2;
// CRailwayProductionLine slot 25, IsFinished (the production manager deletes a line when it returns
// true): cancelled, or every link at the target. Hooked. Function start. (0x141a958c0)
CODE(kCodeRailLineFinished, "40 55 48 83 EC 50 80 3D ?? ?? ?? ?? 00 40 B5 01 48 89 5C 24 ?? 48 89 7C 24 ?? 48 8B F9")
static const int kRailLineDailySlot = 24, kRailLineFinishedSlot = 25;

// --- Registry layouts ------------------------------------------------------------------------
// The trigger Register (reached through the has_global_flag stub's jmp): the trigger registry
// object keeps its std::map<int, entry*> at +disp8 (0x18 on the checked builds; an array and its
// allocator come first). lea rdi,[rcx+disp8] at +0x1f, disp8 at +0x22. The effect registry is the
// map itself (effect Register reads head = *registry).
CODE(kCodeTriggerRegisterMap, "48 8D 79 ?? 48 8B 37 48 8B 46 08")
static const size_t kCodeTriggerRegisterMapAt = 0x1f, kCodeTriggerRegisterMapDisp = 3;

// --- Armies (create_army, NOTES.md G1) ----------------------------------------------------
// Right after kSigBorderWarArmy (SigFindIn; 1.19.3 checks the army for null first, 1.19.2 does
// not): set_leader(army, leader, true) (call at +9), then the leader's virtual refresh
// (call [reg+disp32], disp at +0x16). The registers differ by build (1.19.3: army in rax, vtable
// in rdx; 1.19.4.2: rbx and rax), so those bytes are wildcards.
CODE(kCodeBorderWarSetLeader, "41 B0 01 48 8B D7 48 8B ?? E8 ?? ?? ?? ?? 48 8B ?? 48 8B CF FF ?? ?? ?? ?? ??")
static const size_t kCodeBorderWarSetLeaderCall = 9, kCodeBorderWarLeaderSlotDisp = 0x16;
// COrderGroupCommand slot 10 (the UI's "create army"): resolves the manager handle (call at +0x31
// = the reference-handle resolver) ...
CODE(kCodeOrderGroupExecute, "48 89 5C 24 10 57 48 83 EC 40 83 79 ?? 00 48 8B D9 7F ?? 80 79 ?? 00 75 ?? 48 83 C1 ?? 48 89 74 24 50 33 FF 39 39 75 ?? 39 79 04 75 ?? 8B F7 EB ?? E8 ?? ?? ?? ??")
static const size_t kCodeOrderGroupResolveCall = 0x31;
// ... and further in (SigFindIn) calls create_army (call at +0x11), the same as start_border_war's.
CODE(kCodeOrderGroupCreate, "44 0F B6 4B ?? 4C 8D 41 ?? 48 8D 51 ?? 48 83 C1 ?? E8 ?? ?? ?? ??")
static const size_t kCodeOrderGroupCreateCall = 0x11;
// CSetArmyLeaderCommand slot 9 (can execute): resolves the army (call at +0x2a), then
// can_change_leader(army, nullptr) (call at +0x6d).
CODE(kCodeSetLeaderCheck, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 50 48 8B D9 48 83 C1 ?? 83 39 00 75 ?? 83 79 04 00 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 0F 84 ?? ?? ?? ?? 33 ED 39 6B ?? 75 ?? 39 6B ?? 75 ?? 8B F5 EB ?? 48 8D 4B ?? E8 ?? ?? ?? ?? 48 8B F0 48 8B CF E8 ?? ?? ?? ?? 48 3B C6 0F 84 ?? ?? ?? ?? 33 D2 48 8B CF E8 ?? ?? ?? ??")
static const size_t kCodeSetLeaderCheckResolveCall = 0x2a, kCodeSetLeaderCheckCanChangeCall = 0x6d;
// 1.19.4.2 restructured that function (new checks first; the army in rsi, the general in rbx).
// When kCodeSetLeaderCheck does not match at its start, this is searched in its first 0x200 bytes
// (SigFindIn): the army's current leader compared with the general, can_change_leader(army,
// nullptr) (call at +0x16), then the general's led group (add rbx,disp32, disp at +0x27) resolved
// (call at +0x37) for the second can_change_leader.
CODE(kCodeSetLeaderCheck1194, "48 8B CE E8 ?? ?? ?? ?? 48 3B C3 0F 84 ?? ?? ?? ?? 33 D2 48 8B CE E8 ?? ?? ?? ?? 84 C0 74 ?? 48 85 DB 74 ?? 48 81 C3 ?? ?? ?? ?? 39 3B 75 ?? 39 7B 04 74 ?? 48 8B CB E8 ?? ?? ?? ??")
static const size_t kCodeSetLeaderCheck1194CanChangeCall = 0x16, kCodeSetLeaderCheck1194LedGroupDisp = 0x27,
                    kCodeSetLeaderCheck1194ResolveCall = 0x37;
// set_leader(army, leader, link) itself: its debug log reads the leader's name (std::string,
// capacity disp8 at +0x4c, text disp8 at +0x51), then the army's current leader (disp32 at +0x76).
CODE(kCodeSetLeaderStart, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 80 3D ?? ?? ?? ?? 00 41 0F B6 E8 48 8B FA 48 8B D9 74 ?? 41 B9 05 00 00 00 48 8D 15 ?? ?? ?? ?? 41 B8 ?? ?? ?? ?? 48 8D 4C 24 20 E8 ?? ?? ?? ?? 48 85 FF 74 ?? 48 83 7F ?? 0F 4C 8D 4F ?? 76 ?? 4D 8B 09 EB ?? 4C 8D 0D ?? ?? ?? ?? 44 8B 43 ?? 48 8D 15 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B B3 ?? ?? ?? ??")
static const size_t kCodeSetLeaderNameCapDisp = 0x4c, kCodeSetLeaderNameDisp = 0x51, kCodeSetLeaderArmyLeaderDisp = 0x76;
// can_change_leader(army, reasonOut) itself (from kCodeSetLeaderCheck), only to tell which of its
// refusals applies: army groups always may (flag disp8 at +0x1c); "in a border war" when a field
// (disp32 at +0x2d) is set and the army has a leader (disp32 at +0x3b) ...
CODE(kCodeCanChangeStart, "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 40 48 8B FA 48 8B D9 80 79 ?? 00 74 ?? B0 01 E9 ?? ?? ?? ?? 40 B5 01 48 83 B9 ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 48 83 B9 ?? ?? ?? ?? 00")
static const size_t kCodeCanChangeGroupDisp = 0x1c, kCodeCanChangeWarDisp = 0x2d, kCodeCanChangeLeaderDisp = 0x3b;
// ... then (SigFindIn) "being deployed" when either of two flags is set (disp32 at +2 and +0xb) ...
CODE(kCodeCanChangeDeploying, "80 BB ?? ?? ?? ?? 00 75 ?? 80 BB ?? ?? ?? ?? 00 75")
static const size_t kCodeCanChangeDeployDisp = 2, kCodeCanChangeDeploy2Disp = 0xb;
// ... and after it "being withdrawn" (flag disp32 at +2).
CODE(kCodeCanChangeWithdrawing, "80 BB ?? ?? ?? ?? 00 0F 84")
static const size_t kCodeCanChangeWithdrawDisp = 2;
// CIsArmyLeader slot 22 (is_army_leader): character -> army leader role (call at +0x34), role
// type disp32 at +0x48 (< 2: field marshal 0, corps commander 1).
CODE(kCodeIsArmyLeaderEval, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F1 48 8B FA 48 8B CA E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 ?? 48 8B CF E8 ?? ?? ?? ?? 84 C0 75 ?? 48 8B CB E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B CB E8 ?? ?? ?? ?? 83 B8 ?? ?? ?? ?? 01")
static const size_t kCodeIsArmyLeaderRoleCall = 0x34, kCodeIsArmyLeaderTypeDisp = 0x48;
// CIsLeadingArmy slot 22 (is_leading_army): role type disp32 at +0x18, the led group's handle
// (lea, disp32 at +0x27), the resolver (call at +0x3e), the group's army-group flag (disp8 at +0x61).
CODE(kCodeIsLeadingArmyEval, "40 56 48 83 EC 20 48 8B F1 48 8B CA E8 ?? ?? ?? ?? 48 85 C0 74 ?? 83 B8 ?? ?? ?? ?? 01 77 ?? 48 89 5C 24 30 48 8D 98 ?? ?? ?? ?? 48 89 7C 24 38 33 FF 39 3B 75 ?? 39 7B 04 74 ?? 48 8B CB E8 ?? ?? ?? ?? 48 85 C0 74 ?? 39 3B 75 ?? 39 7B 04 75 ?? 48 8B C7 EB ?? 48 8B CB E8 ?? ?? ?? ?? 40 38 78 ??")
static const size_t kCodeLeadingTypeDisp = 0x18, kCodeLeadingGroupDisp = 0x27, kCodeLeadingResolveCall = 0x3e,
                    kCodeLeadingArmyGroupDisp = 0x61;
// CIsGeneralCapturedTrigger slot 22 (is_general_captured): the leader's "captured" virtual
// (call [rdx+disp32], disp at +0x27).
CODE(kCodeIsCapturedEval, "40 53 48 83 EC 20 48 8B D9 48 8B CA E8 ?? ?? ?? ?? 48 85 C0 74 ?? 83 B8 ?? ?? ?? ?? 01 77 ?? 48 8B 10 48 8B C8 FF 92 ?? ?? ?? ??")
static const size_t kCodeIsCapturedSlotDisp = 0x27;
// CRecruitCharacterEffect slot 13 (recruit_character), found inside it (SigFindIn): mov rax,
// [rip+gamestate]; mov edx,[r?+0x58] (the token); mov rcx,[rax+<characters>]; call find_character.
// gamestate rel32 at +3 (insn 7), characters disp32 at +0xe, call at +0x12.
CODE(kCodeCharacterLookup, "48 8B 05 ?? ?? ?? ?? 41 8B ?? 58 48 8B 88 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 0F 85 ?? ?? ?? ??")
static const size_t kCodeCharacterGameStateRel = 3, kCodeCharacterManagerDisp = 0xe, kCodeCharacterFindCall = 0x12;
// ... then the owner check: call owner_tag_address(character) (+3); mov r?,rax (rsi on 1.19.3,
// rdi on 1.19.4.2); cmp dword [rax],0; jle.
CODE(kCodeCharacterOwnerCheck, "48 8B CB E8 ?? ?? ?? ?? 48 8B ?? 83 38 00 0F 8E")
static const size_t kCodeCharacterOwnerCall = 3;
// owner_tag_address: lea rax,[rcx+disp8]; ret.
CODE(kCodeCharacterOwnerGetter, "48 8D 41 ?? C3")
// CSetOrderGroupNameCommand slot 10 (rename army): resolves the army (call at +0x1c); the name
// std::string at disp32 +0x3c; the engine's std::string assign (jmp at +0x58).
CODE(kCodeSetArmyName, "40 53 48 83 EC 20 48 8B D9 48 83 C1 ?? 83 39 00 75 ?? 83 79 04 00 75 ?? 33 C0 EB ?? E8 ?? ?? ?? ?? 48 83 7B ?? 00 75 ?? 48 8B C8 48 83 C4 20 5B E9 ?? ?? ?? ?? 48 8D 53 ?? 48 8D 88 ?? ?? ?? ?? 48 3B CA 74 ?? 48 83 7A 18 0F 4C 8B 42 10 76 ?? 48 8B 12 48 83 C4 20 5B E9 ?? ?? ?? ??")
static const size_t kCodeSetArmyNameResolveCall = 0x1c, kCodeSetArmyNameDisp = 0x3c, kCodeSetArmyNameAssign = 0x58;
// CHasCharacterTrigger slot 22 (has_character): resolves its character target (call at +0x38).
// That resolver falls back to a lookup of the character among the scope country's characters,
// which works after a save is loaded (recruit_character's global table above does not).
CODE(kCodeHasCharacterEval, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B D9 48 8B FA 48 8D 4A 08 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8D 4B ?? 45 33 C0 48 8B D7 48 8B E8 E8 ?? ?? ?? ??")
static const size_t kCodeHasCharacterResolveCall = 0x38;
// ... in a function the resolver calls (SigFindIn): intern the name (+0), tag -> country (+8),
// country -> its characters (+0x10), find(characters, token, true) (+0x1f).
CODE(kCodeCountryCharacterLookup, "E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 8B 54 24 ?? 41 B0 01 48 8B C8 E8 ?? ?? ?? ??")
static const size_t kCodeCountryCharactersCall = 0x10, kCodeCountryCharacterFindCall = 0x1f;

// --- Joining an army (add_to_army, NOTES.md G4) ---------------------------------------------
// Addresses are 1.19.4.2 (v1.19.4.2.2da5), the build these were found on.
// In create_army (SigFindIn from its start): the move of each unit into the new army,
// move(army, unit, true, true) (call at +0x12). The unit register differs by build, so its byte is
// a wildcard. (0x140c41a40)
CODE(kCodeCreateArmyMove, "41 B1 01 45 0F B6 C1 49 8B ?? 4C 8B 74 24 ?? 49 8B CE E8 ?? ?? ?? ??")
static const size_t kCodeCreateArmyMoveCall = 0x12;
// ... after it (SigFindIn), the loop over the units' old armies: cleanup(old army) (call at +6),
// which deletes an army left without members. (0x140c41acf)
CODE(kCodeCreateArmyCleanup, "7E ?? 49 8B 0C ?? E8 ?? ?? ?? ??")
static const size_t kCodeCreateArmyCleanupCall = 6;
// move(army, unit, skipIfMember, flag) itself (COrdersGroup), found inside it (SigFindIn):
// the tag assert: the unit's tag (disp32 at +2) against the army's (disp8 at +0x17). (0x140c456ad)
CODE(kCodeArmyMoveTag, "8B 87 ?? ?? ?? ?? 89 44 24 ?? 74 ?? 80 3D ?? ?? ?? ?? 00 75 ?? 8B 53 ??")
static const size_t kCodeArmyMoveUnitTagDisp = 2, kCodeArmyMoveArmyTagDisp = 0x17;
// ... the member test: the unit's entry (lea, disp32 at +6) searched in the army's member list
// (disp8 at +0x24), count disp8 at +0xf. (0x140c45724)
CODE(kCodeArmyMoveMembers, "40 84 F6 48 8D B7 ?? ?? ?? ?? 74 ?? 48 63 43 ?? 33 D2 85 C0 7E ?? 48 85 FF 4C 8B C6 4C 8B C8 8B CA 48 8B 43 ??")
static const size_t kCodeArmyMoveEntryDisp = 6, kCodeArmyMoveCountDisp = 0xf, kCodeArmyMoveListDisp = 0x24;
// ... then the unit's current army (disp32 at +3) leaves it (UngroupMember), the entry is appended
// to the member list (lea, disp8 at +0x25), and the unit's army field is compared (disp32 at
// +0x35) and set (disp32 at +0x41) to the new army. (0x140c4576a)
CODE(kCodeArmyMoveUnitArmy, "48 8B 8F ?? ?? ?? ?? 48 85 C9 74 ?? 44 0F B6 C5 48 8B D7 E8 ?? ?? ?? ?? 48 8D 54 24 ?? 48 89 74 24 ?? 48 8D 4B ?? E8 ?? ?? ?? ?? 48 8D 87 ?? ?? ?? ?? 48 39 9F ?? ?? ?? ?? 74 03 C6 00 01 48 89 9F ?? ?? ?? ??")
static const size_t kCodeArmyMoveUnitArmyDisp = 3, kCodeArmyMoveAppendListDisp = 0x25, kCodeArmyMoveUnitArmyCmpDisp = 0x35,
                    kCodeArmyMoveUnitArmySetDisp = 0x41;
// cleanup(army) itself: nothing unless the member count (disp8 at +0xb) is 0; then the army leaves
// its manager and is deleted. Function start. (0x140f70840)
CODE(kCodeArmyCleanup, "48 83 EC 28 48 85 C9 74 ?? 83 79 ?? 00 75 ?? 48 89 4C 24 ?? 48 8D 54 24 ?? 48 89 5C 24 ?? 48 8B 59 ??")
static const size_t kCodeArmyCleanupCountDisp = 0xb;
// CDestroyUnitEffect slot 13 (destroy_unit) calls the scope's division getter first (call at +9):
// the unit handle in the scope (disp8 at +7), resolved (call at +0x13), minus the reference offset
// (imm8 at +0x20, 0xF0 = -0x10). (0x14057cba0)
static const size_t kDestroyUnitGetterCall = 9;
CODE(kCodeScopeDivision, "48 83 EC 28 48 83 C1 ?? 83 39 00 75 06 83 79 04 00 74 ?? E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 83 C0 ?? 48 83 C4 28 C3")
static const size_t kCodeScopeDivisionDisp = 7, kCodeScopeDivisionResolveCall = 0x13, kCodeScopeDivisionRefImm = 0x20;

// --- Time control (time_control, NOTES.md Part K) ------------------------------------------------
// CInGameIdler::TogglePause(int mode) at the slot the console calls (kSigConsoleSpeedPause): mode 1
// = the player (button, Space, console), mode 0 = automatic (event popups: CAutomaticPause). Single
// player: flips the paused byte; multiplayer: posts CPauseGame. Function start.
CODE(kCodeIdlerTogglePause, "48 89 5C 24 10 48 89 6C 24 20 56 57 41 54 41 55 41 57 48 83 EC 30 8B F2 48 8B D9 48 8B 89 ?? ?? ?? ?? E8")
static const size_t kCodeIdlerToggleCheckDisp = 0x1e;  // the observer / game-over check's member
// CInGameIdler::IsPaused(): return this->paused (disp32 at +3).
CODE(kCodeIdlerIsPaused, "0F B6 81 ?? ?? ?? ?? C3")
static const size_t kCodeIdlerPausedDisp = 3;
// RequestGameSpeed(idler, speed 0-based, bool fromPlayer), the console's call target: the same
// observer / game-over check as TogglePause (idler member disp32 at +0x1d). Function start.
CODE(kCodeGameSpeedRequest, "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 30 45 8B F0 8B F2 48 8B D9 48 8B 89 ?? ?? ?? ?? E8")
static const size_t kCodeGameSpeedRequestCheckDisp = 0x1d;
// End of CTopBar::UpdateDate (SigFindIn from its start): lea rcx,[gamestate+date] (disp32 at +3);
// E8 at +0xa: FormatDate(CGameDate*, out, bool); E8 at +0x15: SetText(widget, text); ret.
CODE(kCodeTopBarDateTail, "48 81 C1 ?? ?? ?? ?? 41 B0 01 E8 ?? ?? ?? ?? 48 8B D0 48 8B CF E8 ?? ?? ?? ?? 48 83 C4 50 5F C3")
static const size_t kCodeTopBarDateObjDisp = 3, kCodeTopBarFormatCall = 0xa;
// FormatDate: the hour count (CGameDate +disp8 at +0x1a) minus the base (imm32 at +0x21) = hours
// since year 0, 8760 per year (no leap years), shown as " %d:00, %d %s, %d" with hour % 24 + 1.
CODE(kCodeGameDateFormat, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 81 EC A0 00 00 00 44 8B 49 ?? 48 8B FA 41 81 E9 ?? ?? ?? ??")
static const size_t kCodeGameDateHoursDisp = 0x1a, kCodeGameDateBaseImm = 0x21;
// The pause button refresh (kSigTopBarPauseClick's jmp target) hides one button and shows the
// other, the engine's idiom for a widget (SigFindIn within 0x200):
//   hide: w->Hide() (vtable disp32 at +5); or byte [w+flags],bit (disp8 at +0xb, imm8 at +0xc)
CODE(kCodeWidgetHide, "48 8B 10 FF 92 ?? ?? ?? ?? 80 4B ?? ??")
static const size_t kCodeWidgetHideDisp = 5, kCodeWidgetFlagsDisp = 0xb, kCodeWidgetHiddenBit = 0xc;
//   show: and byte [w+flags],~bit (disp8 at +2, imm8 at +3); ...; w->Show() (vtable disp8 at +0x18)
CODE(kCodeWidgetShow, "80 60 ?? ?? 48 8B 07 66 0F 7F 44 24 20 FF 50 68 48 8B C8 48 8B 10 FF 52 ??")
static const size_t kCodeWidgetShowFlagsDisp = 2, kCodeWidgetShowMask = 3, kCodeWidgetShowDisp = 0x18;
// The pause button refresh itself starts by looking up "time_control_window": a string view
// { lea of the name (rel32 at +0x1b, insn at +0x18), length imm8 at +0x14 }, the top bar's root
// window (disp8 at +0x2a), its find-window vtable disp32 at +0x43: root->FindWindow(&view, true).
CODE(kCodeTopBarPauseRefresh, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 C7 44 24 28 ?? 00 00 00 48 8D 15 ?? ?? ?? ?? 48 89 54 24 20 48 8B D9 48 8B 49 ?? 48 8D 54 24 20 0F 28 44 24 20 41 B0 01 66 0F 7F 44 24 20 48 8B 01 FF 90 ?? ?? ?? ??")
static const size_t kCodeRefreshNameLen = 0x14, kCodeRefreshNameLea = 0x18, kCodeRefreshRootDisp = 0x2a,
                    kCodeRefreshFindDisp = 0x43;
// The CTopBar destructor (RTTI vtable slot 0) has the kCodeScalarDeletingDtor shape.
// --- Equipment conversion (NOTES.md Part J), found inside the SIG functions with SigFindIn ----
// BuildConvertFromList grows the list: alloc = allocator [disp32 +3] vtable [disp8 +0x18]
// (size, 8); count [+0x1f]; memcpy from list [+0x2a]; free = allocator [+0x39] vtable [disp8
// +0x49] (list [+0x43]); list = new [+0x4d]; capacity = new [+0x53].
CODE(kCodeConvListGrow, "48 8B 8D ?? ?? ?? ?? 48 8B 01 8B D7 48 C1 E2 03 41 B8 08 00 00 00 FF 50 ?? 48 8B D8 4C 63 85 ?? ?? ?? ?? 49 C1 E0 03 48 8B 95 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B 8D ?? ?? ?? ?? 48 8B 01 48 8B 95 ?? ?? ?? ?? FF 50 ?? 48 89 9D ?? ?? ?? ?? 89 BD ?? ?? ?? ??")
static const size_t kCodeConvGrowAlloc = 3, kCodeConvGrowAllocSlot = 0x18, kCodeConvGrowCount = 0x1f, kCodeConvGrowList = 0x2a,
                    kCodeConvGrowAlloc2 = 0x39, kCodeConvGrowList2 = 0x43, kCodeConvGrowFreeSlot = 0x49,
                    kCodeConvGrowList3 = 0x4d, kCodeConvGrowCap = 0x53;
// CanConvert's list test: count = target type [disp32 +3]; source type = design [disp32 +0xc];
// list = target type [disp32 +0x17].
CODE(kCodeCanConvertList, "49 63 82 ?? ?? ?? ?? 33 D2 4D 8B 89 ?? ?? ?? ?? 85 C0 7E ?? 49 8B 8A ?? ?? ?? ??")
static const size_t kCodeCanConvListCount = 3, kCodeCanConvListType = 0xc, kCodeCanConvListList = 0x17;
// The post-load stores the generation depth: mov [rbx+disp32 +0xe],esi (esi = the parent hops).
CODE(kCodePostLoadDepth, "83 BB ?? ?? ?? ?? 00 48 8B 6C 24 68 89 B3 ?? ?? ?? ?? 0F 84")
static const size_t kCodePostLoadDepthDisp = 0xe;
// ConversionCost: the source design's unit-type bits (its role: what its modules'
// add_equipment_type make it) [disp32 +0xd] and the naval mask pick the formula; naval chassis IC =
// source design->type [disp32 +0x1e] ->chassis IC, times the BASE_NAVAL define (imul rcx,[rip]).
CODE(kCodeConvCostChassis, "48 B9 C1 03 40 00 80 00 00 00 49 8B 85 ?? ?? ?? ?? 48 23 C1 48 89 44 24 ?? 74 ?? 49 8B 8D ?? ?? ?? ?? 48 8B 89 ?? ?? ?? ?? 48 0F AF 0D")
static const size_t kCodeConvCostRoleDisp = 0xd, kCodeConvCostTypeDisp = 0x1e;
// ConvertTooltip appends with std::string::append(out, p, n): lea rdx,[rsp+x]; cmp; cmova; mov
// r8,[size]; mov rcx,rdi (out); E8 at +0x19.
CODE(kCodeConvTipAppend, "48 8D 54 24 ?? 48 83 7C 24 ?? 0F 48 0F 47 54 24 ?? 4C 8B 44 24 ?? 48 8B CF E8")
static const size_t kCodeConvTipAppendCall = 0x19;
// The stockpile walk (0x1419bd150, the rebuild's call that reaches CanConvert), after CanConvert
// said yes: a design whose byte [disp32 +2] (obsolete) is 0 is skipped if its owner [disp8 +0xc]
// is the country, or if both tags are set and Friendly(&owner, tag) (E8 at +0x21) is true.
CODE(kCodeStockpileSkip, "80 BB ?? ?? ?? ?? 00 75 ?? 48 8D 4B ?? 8B 01 8B 17 3B C2 0F 84 ?? ?? ?? ?? 85 C0 74 ?? 85 D2 74 ?? E8")
static const size_t kCodeStockpileObsoleteDisp = 2, kCodeStockpileOwnerDisp = 0xc, kCodeStockpileFriendlyCall = 0x21;
// DesignName(design (rsi), out (lea rdx,[rbp+x])): E8 at +7, result kept in rbx.
CODE(kCodeConvVariantName, "48 8D 55 ?? 48 8B CE E8 ?? ?? ?? ?? 48 8B D8")
static const size_t kCodeConvVariantNameCall = 7;
// At kAnchorConvertFromList's lea: the error message's type name, lea rdx,[rbp+disp8 +0xf] (the
// type's name std::string).
CODE(kCodeConvListTypeName, "48 8D 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 55 ?? 48 8B C8 E8")
static const size_t kCodeConvListTypeNameDisp = 0xf;

#undef CODE
#undef SIG
#undef ANCHOR
