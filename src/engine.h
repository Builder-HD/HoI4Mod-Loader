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
// Fires the on_action like the engine fires on_startup: synced-RNG seed, events, random events
// and effect. countryTag > 0: ROOT = that country; 0: no scope. name: at most 15 characters.
// Returns false (and logs) on failure; an exception inside is contained.
bool FireOnAction(const char* name, int countryTag);

}  // namespace Engine
