// Engine access for engine-tier features: everything inside hoi4.exe that the loader calls or
// reads, resolved once from src/signatures.h (no hardcoded addresses). See NOTES.md, Part B1.
#pragma once
#include <stddef.h>
#include <stdint.h>

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
extern void* pRegionGetNameKey; // const EngineString* (CStrategicRegion*)

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
    int setDate;  // CDate as stored by the engine (larger = later)
};
// Calls fn for every global flag. Returns false if flags are unavailable.
bool ForEachGlobalFlag(void (*fn)(const FlagEntry&, void*), void* ctx);
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

// Read-only lookup in the engine's effect / trigger registry (std::map<int, entry*>).
bool EffectRegistered(int token);
bool TriggerRegistered(int token);
bool RegisterTrigger(int token, void* entry);  // entry: { vtable, help, byte flag, ... }

// Parser helpers.
void ParserReadString(void* parser, EngineString* out);  // out: zeroed, cap 15
void ParserUnexpected(void* parser);                      // "Unexpected token" + skip
// Builds an engine-owned std::string on raw storage (the engine frees it with its allocator).
void ConstructEngineString(EngineString* raw, const char* p, size_t n);

}  // namespace Engine
