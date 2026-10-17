// Engine access for engine-tier features: everything inside hoi4.exe that the loader calls or
// reads, resolved once from src/signatures.h (no hardcoded addresses). See NOTES.md, Part B1.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>

// MSVC std::string as laid out inside hoi4.exe (release STL): 16-byte inline buffer or heap
// pointer, then size and capacity. The loader only reads engine strings and only hands out
// strings it owns and never frees while the game runs.
struct EngineString {
    union {
        char buf[16];
        char* ptr;
    };
    uint64_t size;
    uint64_t cap;
    const char* c_str() const { return cap > 15 ? ptr : buf; }
};
static_assert(sizeof(EngineString) == 32, "EngineString must match MSVC std::string");

namespace Engine {

// Resolves every symbol. Returns false only if nothing usable was found; individual groups
// report their own availability below. Logs each failure. Call once, from DllMain.
bool Resolve();

// --- Hook targets (null if not found) ---------------------------------------------------------
extern void* pLexerSync;        // void ()
extern void* pGameEntered;      // void (CInGameIdler*)
extern void* pAppDoFrame;       // void (CApplication*, bool topLevel); main thread, every frame
extern void* pRegionGetNameKey; // const EngineString* (CStrategicRegion*)
extern void* pFlagSet;          // void (CScriptFlags*, int token, short value, short days)
extern void* pFlagClear;        // void (CScriptFlags*, int token)

// --- Groups ----------------------------------------------------------------------------------
bool HaveLifecycle();  // LexerSync + GameEntered + IsLoadedFromSave
bool HaveTokens();     // intern + token -> string
bool HaveFlags();      // gamestate + flags container + set/clear
bool HaveRegions();    // gamestate + region array + GetNameKey
bool HaveLocalize();
bool HaveRegistry();   // effect registry + set_province_name entry layout

// --- Game state ------------------------------------------------------------------------------
void* GameState();  // null before a game exists
bool IsLoadedFromSave(void* gameState);

// --- Tokens (only after the lexer is synchronized, see Lifecycle) ----------------------------
int Intern(const char* s);              // string -> token (creates dynamic tokens)
const char* TokenName(int token);       // token -> string ("" for unknown)

// --- Global flags (main/game thread only) ----------------------------------------------------
struct FlagEntry {
    int token;
    int setDate;  // date of the last set (larger = later)
    short value;
    short days;   // > 0: the engine removes the flag when its daily countdown reaches 0
};
// Calls fn for every global flag. Returns false if flags are unavailable.
bool ForEachGlobalFlag(void (*fn)(const FlagEntry&, void*), void* ctx);
void* GlobalFlags();              // the global CScriptFlags container (null without a game)
bool SetGlobalFlag(int token);    // value 1, no expiry
bool ClearGlobalFlag(int token);

// --- Strategic regions -----------------------------------------------------------------------
int RegionCount();             // valid ids are 1 .. RegionCount()-1
void* RegionById(int id);      // CStrategicRegion* or null
int RegionTemplateId(void* region);  // id stored in the region's template, -1 if unreadable

// --- Localisation / UI -----------------------------------------------------------------------
// The localised text for key (engine Localize()), or null if the localisation has no entry.
// The returned string is never freed: callers cache it and may hand it to the engine.
const EngineString* LocalizeKey(const char* key);
// Localize() with $NAME$ arguments (the engine's CPdxLocalizeKeyValuePair, string values), so the
// localisation's own formatting ($NAME|Y$) applies. False if the key has no entry, the call
// faulted, or the self-test on first use failed (then always false). Main thread (UI) only.
struct LocalizeArg {
    const char* name;
    const char* value;
};
bool LocalizeWithArgs(const char* key, const LocalizeArg* args, int count, std::string* out);
void MarkMapNamesDirty();  // the refresh set_state_name triggers

// --- Effect registry -------------------------------------------------------------------------
struct EffectEntry {          // CEffectEntry<T>: { vtable, help text }
    void** vtable;
    const char* help;
};
bool RegisterEffect(int token, EffectEntry* entry);
// CEffectEntry<CRenameProvinceEffect> vtable (slot 1 = factory) and the effect's own vtable.
extern void** pRenameProvinceEntryVtable;
extern void** pRenameProvinceEffectVtable;
extern size_t renameProvinceEffectVtableSlots;

// RTTI: vtable for a class name such as ".?AVCSetGlobalFlagEffect@@" (complete object, offset 0).
void** FindVtableByRtti(const char* rttiName, size_t* slots);

// --- Native script binding (NOTES.md C1) -----------------------------------------------------
// Everything below is resolved together; HaveNativeScript() is false if any piece is missing.
bool HaveNativeScript();

struct Vtable {
    void** vt = nullptr;
    size_t slots = 0;
};
extern Vtable effectBase;          // .?AVCEffect@@
extern Vtable triggerBase;         // .?AVCTrigger@@
extern Vtable effectTemplate;      // .?AVCRenameProvinceEffect@@ (object 0x80, block, any scope)
extern Vtable effectValueReader;   // .?AVCSetStateNameEffect@@ (slot 3 reads a single value)
extern Vtable triggerTemplate;     // .?AVCAlwaysTrigger@@ (object 0x60; slot 5 reads a single value)
extern void** effectTemplateEntryVtable;   // CEffectEntry<CRenameProvinceEffect> (slot 1 = factory)
extern void** triggerTemplateEntryVtable;  // CTriggerEntry<CAlwaysTrigger>

// Numeric trigger templates (NOTES.md E1/E2): CNumOfFactoriesTrigger (CIntTrigger: op +0x58,
// int +0x60, variable +0x68 with flag +0x70) and CHasStabilityTrigger (CValueTrigger: op +0x58,
// CScopedVariable +0x60, range-checked 0..1). Objects come from operator new + constructor.
struct NumericTemplate {
    Vtable vt;
    size_t size = 0;
    void* (*alloc)(size_t) = nullptr;
    void (*ctor)(void*) = nullptr;
};
extern NumericTemplate numericCount, numericRatio;
// Evaluates a CScopedVariable (number or variable) in a scope: fixed point, 100000 = 1.
using ScriptValueEval_t = int64_t*(__fastcall*)(void* holder, int64_t* out, const void* scope, int, bool);
extern ScriptValueEval_t scriptValueEval;
extern int tokGreater, tokLess;  // parser operator tokens for '>' and '<'
bool HaveNumeric();

// Read-only lookup in the engine's effect / trigger registry (std::map<int, entry*>).
bool EffectRegistered(int token);
bool TriggerRegistered(int token);
bool RegisterTrigger(int token, void* entry);  // entry: { vtable, help, byte flag, ... }

// Parser helpers.
void ParserReadString(void* parser, EngineString* out);  // out: zeroed, cap 15
void ParserUnexpected(void* parser);                      // "Unexpected token" + skip
// Builds an engine-owned std::string on raw storage (the engine frees it with its allocator).
void ConstructEngineString(EngineString* raw, const char* p, size_t n);

// --- On actions and the game session (game_loaded; NOTES.md Part D) --------------------------
bool HaveOnActions();  // on_action database, find, fire, event scope, player tag, synced RNG
bool HaveSession();    // session getter, its server object and hot-join flag

// The local game session as the engine's own telemetry classifies it (main thread, while a game
// is running). known = false if anything was unreadable; callers must then assume multiplayer.
struct SessionInfo {
    bool known = false;
    const char* serverClass = "?";  // class of the session's server object, e.g. "CDummyServer"
    bool singlePlayer = false;      // server is a CDummyServer (no network)
    bool hotJoin = false;           // CSession::bIsHotjoin: this machine joined a running game
};
SessionInfo QuerySession(void* idler);

// Local player's country tag (> 0), or 0 when observing. Main thread only.
int PlayerCountryTag();
// "FRA" etc.; the number as text if the name is unavailable.
std::string CountryTagName(int tag);
// True if some script defined an on_action of this name (on_action keys are dynamic).
bool OnActionDefined(const char* name);
// --- Data triggers (engine_data.cpp, NOTES.md Part E) -----------------------------------------
// Read-only, callable from any thread (native trigger evaluate). Fixed point: 100000 = 1.
void ResolveData();
bool HaveCountries();      // tag -> country, tag -> index
bool HaveRegionControl();  // air power, naval dominance, war relations
void* CountryByTag(int tag);
// "GER" -> tag (> 0) from the engine's static tag database; 0 if unknown or not loaded yet.
// Pure read; safe while scripts are parsed.
int TagFromText(const char* text);
bool AtWar(int tagA, int tagB);  // a war between them that has not ended
// Air power in a strategic region from a country's side: its side and the enemy side.
bool AirPower(int tag, int regionId, int64_t* our, int64_t* enemy);
// Naval dominance in a strategic region: friendly (own + allied) and enemy (at war) sums, as
// has_naval_control adds them. Land regions: true with both 0.
bool NavalDominance(int tag, int regionId, int64_t* friendly, int64_t* enemy);
std::string RegionName(int regionId);  // display name (custom name included); tooltips only

bool HaveMilitaryCounts();  // states, province unit/combat lists, country divisions
bool HavePlanes();          // + air wings and their base regions
enum class AreaKind { State, StrategicRegion };
// Which units count, by their owner. Each set field (a country tag > 0) must hold: owner is that
// country, owner is at war with enemyOf, owner is an ally of alliedWith (the engine's IsFriendly
// relation test that naval control uses; alliedWith itself excluded). All 0: every unit.
struct UnitFilter {
    int owner = 0;
    int enemyOf = 0;
    int alliedWith = 0;
};
void* StateById(int id);
bool IsSameCountry(int a, int b);
bool HaveAlliance();  // alliedWith is available (IsFriendly resolved)
// Counts in a state / strategic region. False if the area does not exist.
bool CountDivisions(AreaKind kind, int id, const UnitFilter& f, int64_t* out);
bool CountShips(AreaKind kind, int id, const UnitFilter& f, int64_t* out);
// Distinct combats located in the area's provinces, plus (unless locationOnly) the combats of the
// divisions standing in its provinces wherever those combats are located.
bool CountBattles(AreaKind kind, int id, bool locationOnly, int64_t* out);
bool CountBattlesInvolving(int tag, int64_t* out);         // distinct combats of its divisions
bool CountPlanesStationed(int regionId, int tag, int64_t* out);  // its planes based in the region
// The country's divisions, and the land units in an area (with their owner tag).
bool ForEachDivision(int tag, void (*fn)(void* division, void* ctx), void* ctx);
bool ForEachAreaDivision(AreaKind kind, int id, void (*fn)(void* division, int owner, void* ctx), void* ctx);
void ResolveArmyCondition();
bool HaveDivisionCondition();  // CArmy strength / organisation getters
// Strength and organisation of a division as ratios (100000 = full). False if not a CArmy.
bool DivisionRatios(void* unit, int64_t* strength, int64_t* org);

int GameDateHours();  // current game date in hours (0 without a game); for diagnostics
void ResolveProduction();
bool HaveProduction();  // production lines, equipment database
// Equipment type or archetype by name token (null if unknown or not loaded yet).
const void* EquipmentByToken(int token);
// Factories assigned to the country's military and naval production lines (dockyards for ships);
// only lines producing `equipment` (a type, or any type of an archetype) unless it is null.
bool FactoriesAssigned(int tag, const void* equipment, int64_t* out);
// Diagnostics ([diagnostics] production_lines = 1): logs the country's lines, their factories and
// equipment names. Main thread.
void LogProductionLines(int tag);

// --- Region data (region_scope; engine_data.cpp) ----------------------------------------------
bool HaveRegionData();  // province lists of states and regions, the region's sea share
// The region has a sea province (the engine's own test in its region console command).
bool RegionIsSea(int regionId, bool* sea);
// The state has a province in the region. False if either does not exist.
bool RegionHasState(int regionId, int stateId, bool* has);
// The region holding the state's first province (0 if none).
int RegionOfState(int stateId);

// --- Scope blocks and iterators (region_scope; engine_scope.cpp, NOTES.md F5) -----------------
// The vanilla state iterators the region iterators are cloned from. keys: the block keys the
// vanilla item handler reads itself (decoded from its compare chain); limitKey: the one whose
// handler reads the embedded `limit` trigger (every / random).
struct IteratorTemplate {
    Vtable vt;
    void** entryVtable = nullptr;  // CTriggerEntry<T> / CEffectEntry<T>: slot 1 = factory
    int keys[8] = {};
    int keyCount = 0;
    int limitKey = 0;
};
extern IteratorTemplate anyStateTemplate, everyStateTemplate, randomStateTemplate;
extern Vtable andTrigger;  // .?AVCAndTrigger@@: slot 21 = empty own line, slot 22 = AND of the children
void ResolveScopeBlocks();
bool HaveScopeBlocks();
// Child scopes as the vanilla iterators build them: a copy of the parent with the parent's ROOT
// and FROM and PREV = the parent. ScopeSetRegion makes it a strategic region scope (the other
// scope fields cleared). The buffer is kScopeBuffer bytes, 16-byte aligned; ScopeEnd destroys it.
const size_t kScopeBuffer = 0x400;
void ScopeBegin(void* buf, const void* parent);
void ScopeSetRegion(void* scope, void* region);
void ScopeEnd(void* buf);
// Frees an engine-owned std::string (tooltip temporaries the engine hands back), leaving it empty.
void FreeEngineString(EngineString* s);
// Object layout of the cloned iterators (checked by kCodeTargetEffectExecute / Limit).
const size_t kIterCount = 0x78;       // any: `count` (CScopedVariable); `tooltip` key at +0x58
const size_t kIterLimit = 0x78;       // every / random: the embedded `limit` trigger
const size_t kIterCandidates = 0xd0;  // every / random: CPdxArray { T** data, int cap, int count, allocator* }
// engine.cpp: the CEventScope destructor (player_scope's), null if unresolved.
void* EventScopeDestructor();

// Fires the on_action like the engine fires on_startup: synced-RNG seed, events, random events
// and effect. countryTag > 0: ROOT = that country; 0: no scope. name: at most 15 characters.
// Returns false (and logs) on failure; an exception inside is contained.
bool FireOnAction(const char* name, int countryTag);

}  // namespace Engine
