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

// --- Code shapes at RTTI-found addresses (numeric triggers, NOTES.md E2) -------------------
// CODE(name, "..."): checked with SigMatchAt at an address reached through an RTTI vtable, not
// scanned (so not required to be unique). tools/check_code_shapes.py checks them on exe copies.
#define CODE(name, pattern) static const char name[] = pattern;
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

#undef CODE
#undef SIG
#undef ANCHOR
