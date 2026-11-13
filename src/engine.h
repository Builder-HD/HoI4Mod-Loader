// Engine access for engine-tier features: everything inside hoi4.exe that the loader calls or
// reads, resolved once from src/signatures.h (no hardcoded addresses). See NOTES.md, Part B1.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

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
void** GameStateSlot();  // where the engine keeps the gamestate pointer (cross-checks), or null
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

int GameDateHours();  // current game date in hours (0 without a game); time_control's lock ends by it
uint32_t GameDateOffset();  // CGameState -> that hour count (0 if unresolved; cross-checks)
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
// Field offsets production_data resolved (0 if not), for other resolvers' cross-checks.
uint32_t EquipmentTypeOffset();  // design (line equipment) -> CEquipmentType
uint32_t TypeArchetypeOffset();  // CEquipmentType -> its archetype

// --- Equipment conversion (equipment_conversion; engine_conversion.cpp, NOTES.md Part J) -------
// Types are CEquipmentType (infantry_equipment_1, light_tank_chassis_2); designs are the variants
// stockpiles and production lines hold (each has a type).
void ResolveConversion();
bool HaveConversion();
extern void* pConvertListBuild;  // void (CEquipmentType*, database): builds the type's convert-from list
extern void* pCanConvert;        // bool (targetDesign, sourceDesign)
// int64_t* (sourceDesign, out cost {int64 IC, resources...}, targetDesign, organisation, int64 breakdown[8] or null)
extern void* pConversionCost;
const void* DesignType(const void* design);
const void* DesignRoot(const void* design);   // the first design of its parent chain
const void* TypeParent(const void* type);     // the previous generation (`parent =`), null for a root
const void* TypeArchetype(const void* type);
bool TypeIsArchetype(const void* type);
int TypeDepth(const void* type);              // parent hops, as the engine computes them at load
bool TypeIsNaval(const void* type);           // the engine's own test (naval refits require it)
const char* TypeName(const void* type);
// How many generations source is behind target along target's parent chain; 0 if it is not on it.
int GenerationsBack(const void* target, const void* source);
// The convert-from list: types whose designs may be converted into designs of this type.
int ConvertListCount(const void* type);
const void* ConvertListAt(const void* type, int i);
// Appends with the list's own allocator (the type's destructor frees it); may reallocate and free
// the old buffer, so only during the database load (no conversion test runs yet).
bool ConvertListAppend(void* type, const void* const* add, int n);
int ConvertListCapacity(const void* type);
// Sets the count within the capacity, without allocating: entries already written beyond the count
// become visible, or entries are hidden again. The buffer itself never changes.
bool ConvertListSetCount(void* type, int count);
// The role mode (Shift+click on a line's convert button), optional:
bool HaveRoleMode();
extern void* pLineRebuild;     // void (line, const int* tag): rebuilds the line's conversion sources
extern void* pConvertClick;    // void (CProductionLineMilitaryItem*): the convert button's click
extern void* pConvertTooltip;  // void (line, ?, std::string* out): the convert button's tooltip
uint64_t DesignRole(const void* design);  // unit-type bits from its modules (fighter, cas, ...)
const void* LineTarget(const void* line);  // the design a production line makes
void* ItemLine(const void* item);          // the production line of a line UI item
// std::string::append in the game's own STL (the string may reallocate with the game's heap).
void AppendEngineString(EngineString* s, const char* p, size_t n);
// The convert tooltip's list of left-out designs (optional on top of the role mode):
bool HaveSkippedList();
// Would the line's stockpile walk use this design once CanConvert says yes? (the game's rule:
// marked for conversion (obsolete), or foreign and not friendly). Any thread.
bool StockpileUses(const void* design, int tag);
std::string DesignName(const void* design);  // the name the game shows; main thread only

// --- Region data (region_scope; engine_data.cpp) ----------------------------------------------
bool HaveRegionData();  // province lists of states and regions, the region's sea share
// The region has a sea province (the engine's own test in its region console command).
bool RegionIsSea(int regionId, bool* sea);
// The state has a province in the region. False if either does not exist.
bool RegionHasState(int regionId, int stateId, bool* has);
// The region holding the state's first province (0 if none).
int RegionOfState(int stateId);
// Valid state ids are 1 .. StateCount()-1 (0 without a game). Main thread or trigger reads.
int StateCount();
// The CProvince objects of a state or strategic region, in the engine's list order. False if the
// area does not exist or the list layout is unresolved.
bool AreaProvinceObjects(AreaKind kind, int id, std::vector<void*>* out);

// --- Attention areas (attention_area; engine_attention.cpp, NOTES.md Part M) -------------------
void ResolveAttention();
bool HaveAttentionBorders();  // gradient border API, layer slots, allocation, rebuild hooks
bool HaveAttentionCamera();   // UI, camera, centre on province, look-at, target height
bool HaveProvinceData();      // province objects, ids, rects, controller, coastal, state owner
// Province data (main thread or trigger reads; ids 1 .. ProvinceCount()-1).
int ProvinceCount();
int ProvinceIdOf(const void* province);      // CProvince -> id, 0 if unreadable
bool ProvinceIsCoastal(int id);
int ProvinceController(int id);              // tag, 0 if unknown
int StateOwner(int stateId);                 // tag, 0 if unknown
bool ProvinceRect(int id, int rect[4]);      // map pixels {x, y, width, height}
// One drawn area: the look and the provinces (ids, ascending).
struct AttentionLook {
    float r = 1, g = 1, b = 0, a = 1;
    float width = 1;     // border thickness (outline mode)
    bool pulse = false;  // the engine's "highlighted" pulsing
    bool glow = true;    // the engine's soft gradient (no fixed thickness)
};
struct AttentionDraw {
    AttentionLook look;
    std::vector<int> provinces;
};
// Queues the three hooks (layer pair, rebuild all, rebuild one). onFault: a fault in the loader's
// part switched the drawing off for the session. Call from the feature's init.
bool HookAttention(void (*onFault)(const char* where));
// Main thread, at a safe point: replaces what is drawn (empty: nothing). Borders go into the
// layer the map currently draws (NOTES.md M5); they are re-added after the game rebuilds it.
void SetAttentionDraw(const std::vector<AttentionDraw>& areas);
// Main thread, every frame (cheap): draws wanted areas once the map's layers are known.
void AttentionTick();
// The game entry: forget the borders of the previous game without touching them.
void ForgetAttentionDraw();
// Stripes (NOTES.md M9): the diagonal stripes the map draws for occupation and battle-plan orders,
// in a chosen colour per province set, in every map mode. Colour bytes b, g, r, a (a: stripe strength).
struct StripeArea {
    uint32_t rgba = 0;
    std::vector<int> provinces;
};
bool HookStripes();   // queues the stripe paint hook; false if its signature was not found
bool HaveStripes();   // the hook is in place
void SetStripes(const std::vector<StripeArea>& areas);  // replaces the set; applied at the next paint
void ForgetStripes();  // a new game is entered
// Province group outlines (the thin lines construction mode draws around build targets): one
// outline at a time for the UI. Main thread. Palette index: BORDER_COLOR_CUSTOM_HIGHLIGHTS.
bool HaveHighlight();
int HighlightPaletteSize();          // colours in the palette (0 if unknown)
int HighlightWhiteIndex();           // the colour made white in the renderer's table, else -1
// Queues the hook that makes one outline colour white when the renderer builds its colour table
// (once, at the map's graphics setup). Call from a feature's init.
bool HookHighlightWhite();
// The outline owner's hovered item (a hovered queue entry's province): drawn before every outline,
// so it covers an outline's edges. The filter sees each province the game sets (UI, main thread)
// and returns true to set nothing instead. Queues a hook.
using HoverFilter_t = bool (*)(int provinceId);
bool HookHoveredProvince(HoverFilter_t filter);
// Main thread: clears the hovered item now if it is a province the filter accepts. True if cleared.
bool ClearHoveredProvinceIf(HoverFilter_t filter);
bool ShowHighlight(const std::vector<int>& provinces, int paletteIndex);  // replaces the previous one
// Any number of outlines (script areas): one game outline per handle, shown while it is held.
struct OutlineRef {
    void* obj = nullptr;
    void* ctrl = nullptr;  // the shared_ptr's control block
};
bool CreateOutline(const std::vector<int>& provinces, int colourId, int priority, OutlineRef* out);  // replaces *out
void ReleaseOutline(OutlineRef* ref);
// Colour ids: the white slot (-1 before the colour table is built), and an extra slot holding
// this colour (-1 if none is free or the table could not be extended). Main thread.
int OutlineWhiteId();
int OutlineColourId(float r, float g, float b, float a);
void ResetOutlineColours();  // the extra slots may be handed out again
void HideHighlight();
// Main thread, at a safe point: centre the camera on the box (map pixels) and set the target
// height (height <= 0: fit the box, times fitFactor). `ref` and `ref2` are provinces of the area
// used to calibrate the map -> camera mapping through the game's own centre-on-province. False if
// not possible now (no game UI).
bool FocusCamera(const int box[4], int ref, int ref2, float height, float fitFactor);
// The UI's centre-on-province (right-click on a construction queue entry, goto_state, ...): the
// filter sees each call's province id (UI, main thread) and returns true to take it over (the
// game's centring is then skipped). Not called for FocusCamera's own calibration. Queues a hook.
using CentreFilter_t = bool (*)(int provinceId);
bool HookCentreOnProvince(CentreFilter_t filter);

// --- Railways (railways; engine_railway.cpp, NOTES.md Part H) ---------------------------------
// Main thread only: the effect that uses them runs there, and DowngradeRailway writes game state.
void ResolveRailways();
bool HaveRailways();   // every railway field and function resolved (no game needed)
bool RailwaysReady();  // HaveRailways() and a game with a railway manager exists
// Switches railways off for the rest of the session (the plugin's self-check failed): nothing
// changes a railway any more, dismantle entries in the queue pause (no progress, not finished,
// kept in the queue and in saves), and the map badges are vanilla again.
void DisableRailways();
// May the country dismantle along the path: the game rules (or without the relation tests, the
// engine's build permission), as a queued entry's daily step checks it. -1 if unknown.
int RailwayDismantleAllowed(int tag, const std::vector<int>& path);
// Read-only consistency check of the railway data as resolved: every stored link agrees with the
// per-neighbour level and with its other end. False (with the first problem) if anything is off.
bool RailwaySelfCheck(int* linksChecked, std::string* problem);
// The railway level of the link between neighbouring provinces a and b (0: none).
int RailwayLevel(int a, int b);
// The existing railway path with the fewest links from `from` to `to` (both ends included);
// false if no railway connects them. Deterministic: the same on every machine.
bool RailwayPath(int from, int to, std::vector<int>* path);
// True if railway links of at least minLevel connect the two provinces. Read only and safe in a
// trigger: no allocation per call (per-thread work arrays that grow once).
bool RailwayConnected(int from, int to, int minLevel);
// Lowers every link along the path by `levels` (not below 0), on both ends, updates the provinces'
// rail_way buildings and marks the provinces dirty in the supply system. Returns the links changed.
int DowngradeRailway(const std::vector<int>& path, int levels);
// Removes the shortest route completely, again and again, until no railway connects the two
// provinces. Returns the links removed; *routes = the number of routes removed.
int RemoveAllRailwayRoutes(int from, int to, int* routes);
// Queued dismantling: a railway construction line with its target below its base level, run by
// hooks on the line's daily step, progress and finished check (NOTES.md H3).
bool HaveRailwayLines();   // every line field, function and vtable slot resolved
bool HookRailwayLines();   // queues the three hooks; call from a feature's init
// Adds a dismantle line to the country's construction queue (at the end): every link of `path`
// above `to` is lowered one level at a time down to `to`, each level costing a share of building
// it (SetRailwayDismantleCost). `from` is the highest level on the path. Main thread (an effect).
bool QueueRailwayDismantle(int tag, const std::vector<int>& path, int from, int to);
// The construction queue's tooltip for a dismantle entry uses this key (placeholders as
// RAILWAY_UPGRADE_TOOLTIP: $START_LEVEL$, $RAILWAY_NAME$, $TARGET_LEVEL$) when the loaded
// localisation defines it. Queues the tooltip and Localize hooks; false if not found.
const char kRailwayDismantleTooltipKey[] = "HOI4_LOADER_RAILWAY_DISMANTLE_TOOLTIP";
bool HookRailwayTooltip();
// Called with a railway queue entry's province path each time the game builds its tooltip (the
// player points at the entry; UI, main thread). Needs HookRailwayTooltip first; false otherwise.
using RailwayHover_t = void (*)(const int* path, int count);
bool SetRailwayHoverHandler(RailwayHover_t handler);
// Railway entries' titles in the queue start with a marker: vanilla's red down triangle text icon
// for dismantling, its green up triangle for building and upgrading, or the localisation of these
// keys if a mod defines them. Queues the entry-update and SetText hooks; needs HookRailwayTooltip
// first (the shared line lookup).
const char kRailwayDismantlePrefixKey[] = "HOI4_LOADER_RAILWAY_DISMANTLE_PREFIX";
const char kRailwayBuildPrefixKey[] = "HOI4_LOADER_RAILWAY_BUILD_PREFIX";
// The railway badge tooltip on a segment being dismantled: a line after the engine's text
// ($LEVEL$ = the target level) and the click hints that replace vanilla's. Built-in English
// without these keys.
const char kRailwayDismantlingKey[] = "HOI4_LOADER_RAILWAY_DISMANTLING";
const char kRailwayDismantleHintsKey[] = "HOI4_LOADER_RAILWAY_DISMANTLE_HINTS";
bool HookRailwayEntryTitle();
// Where the player's dismantling (map badges, the queued line's daily check) is allowed besides
// provinces the country controls itself: a bit mask the plugin computes from global flags a mod
// sets from its game rules. Without the relation tests (signatures missing) the engine's build
// permission applies instead.
enum : int { kRailRuleAllies = 1, kRailRuleSubjects = 2, kRailRuleOverlord = 4 };
using RailwayRules_t = int (*)();
void SetRailwayDismantleRules(RailwayRules_t rules);
// What dismantling a level costs, in percent of building it (default 30). `cost` is asked once per
// dismantle line per day and per queue change (main thread); it returns -1 for the default.
const int kRailwayDismantleDefaultPercent = 30;
using RailwayCost_t = int (*)();
void SetRailwayDismantleCost(RailwayCost_t cost);
// Railway map badges (the level circle on each segment): while Alt+Shift is held they show a red
// X where the player may dismantle, and a Alt+Shift+click calls onClick(segment path, deepen)
// instead of vanilla's upgrade when onClick returns true. On a segment with the player's dismantle
// entry the badge shows its target in red with the under-construction mark, and a click or right
// click calls onClick(path, lessen), Shift+click or Shift+right click onClick(path, stop), instead
// of vanilla (whose level change would turn the entry into a rebuild). UI thread; onClick must
// defer game-state changes.
enum class RailwayClick : int { kDeepen, kLessen, kStop };
using RailwayIconClick_t = bool (*)(const std::vector<int>& path, RailwayClick action);
bool HookRailwayMapIcons(RailwayIconClick_t onClick);
// One dismantle level less on a segment (kLessen), or no dismantling there at all (kStop): the
// country's dismantle entry for this path (or one sharing a link with it) goes one level up, or is
// cancelled when nothing would be left to dismantle (the queue drops it at the next daily update).
// Returns 2 cancelled, 1 one level less, 0 no entry. Main thread, at a safe point.
int LessenRailwayDismantle(int tag, const std::vector<int>& path, bool stop);
// One more dismantle level on a segment: deepens the country's dismantle entry for exactly this
// path by one level (adding that level's cost), or queues a new one (top level -> top - 1).
// Returns 2 deepened, 1 queued, 0 nothing to do. Main thread, at a safe point.
int QueueOrDeepenRailwayDismantle(int tag, const std::vector<int>& path);

// The province object's controller tag offset (kSigRailController; 0 if unresolved). Resolved by
// ResolveRailways, also used by province building removal.
uint32_t ProvinceControllerOffset();

// --- Province building removal (province_building_removal; engine_province_buildings.cpp, I3) ---
// A cross on the state view's province building entries (supply hub, naval base, bunkers, ...):
// the entry's `remove` button, which the loader adds to countrystateview.gui through a file patch.
// Resolved after ResolveRailways (needs the controller offset).
void ResolveProvinceBuildings();
bool HaveProvinceBuildingRemoval();  // every function, field and vtable resolved (no game needed)
// UI thread. `allowed`: may the cross show at all (the plugin: single player only). `onClick`: the
// player clicked the cross of `type` (a CBuildingType*) in `province` (a province object) of `state`;
// must defer the change (RemoveProvinceBuildingLevel) and return true if it did.
using ProvinceRemoveAllowed_t = bool (*)();
using ProvinceRemoveClick_t = bool (*)(void* province, void* type, void* state);
// `onFault`: a fault inside the loader's part of a hook or the removal (contained) has switched
// the feature off for the session; `where` names the hook. Main thread.
using ProvinceRemoveFault_t = void (*)(const char* where);
// Queues the update, tooltip and destructor hooks. Call from a feature's init.
bool HookProvinceBuildingRemoval(ProvinceRemoveAllowed_t allowed, ProvinceRemoveClick_t onClick,
                                 ProvinceRemoveFault_t onFault);
// Init only: takes out every hook HookProvinceBuildingRemoval queued (a later init step failed),
// so nothing of the feature is applied.
void UnhookProvinceBuildingRemoval();
// Main thread, at a safe point: removes one level of the province's building of `type` as
// remove_building does (SetLevel with notify, so supply follows), if it still has a level, the local
// player controls the province and the removal cooldown allows it. Returns the new level, or -1
// (and *why) if nothing was removed.
int RemoveProvinceBuildingLevel(void* province, void* type, void* state, std::string* why);
// The patch that adds the `remove` button to province_building_entry (countrystateview.gui). Empty
// if the file has no such entry or already has the button. Pure text; tests call it directly.
std::string PatchCountryStateViewGui(const std::string& original);
const char kProvinceBuildingRemoveTooltipKey[] = "BUILDING_SLOT_REMOVE_BUILDING_LEVEL";
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

// --- Armies (create_army; engine_army.cpp, NOTES.md G1) ---------------------------------------
// Game state writes: main thread only (effect execution), never from triggers or tooltips.
void ResolveArmies();
bool HaveArmies();
// The country's divisions in the engine's list order (engine_data.cpp, the army.daily list).
bool HaveCountryDivisions();
bool CountryDivisionList(int tag, std::vector<void*>* out);
int UnitOwnerTag(const void* unit);
// The character with this id token among the country's own characters (has_character's lookup),
// or null. Works in new games and after loading a save.
void* CountryCharacterByToken(int tag, int token);
// The character with this id token from recruit_character's table, or null. Only filled for new
// games (in the 1.19.3 test it found nothing after loading a save). Used to tell why a character
// is not available.
void* CharacterByToken(int token);
int CharacterCountry(const void* character);   // tag of the country that has recruited it, 0 if none
void* ArmyLeaderRole(const void* character);   // its army leader role (CUnitLeader), or null
// Which of the game's can-change-leader refusals applies (read only to choose a message; the
// refusal itself is the engine's check). Unknown: the fields were not found, or none is set.
enum class LeaveBlock { None, Unknown, Deploying, Withdrawing, BorderWar };
struct LeaderState {
    int roleType = -1;          // 0 field marshal, 1 corps commander (general); >= 2 is not an army leader
    bool captured = false;
    void* leads = nullptr;      // the army or army group it leads, or null
    bool leadsArmyGroup = false;
    bool canLeave = true;       // its army may change leader now (the game's own check)
    LeaveBlock whyNot = LeaveBlock::None;  // when !canLeave: why
};
bool ReadLeader(void* leader, LeaderState* out);  // false if the leader is unreadable (logged)
std::string LeaderName(const void* leader);       // the leader's name text, "" if unreadable
// A new army of the country from these of its divisions, as the UI's "create army" (divisions
// leave their old army; old armies left empty are deleted). Null on failure (logged).
void* CreateArmy(int tag, const std::vector<void*>& divisions);
// Makes `leader` the army's general, as start_border_war does: links both ways (his previous army
// keeps its divisions without a general), then the leader's refresh. True if the army then has him.
bool SetArmyLeader(void* army, void* leader);
void SetArmyName(void* army, const char* name);
std::string ArmyName(const void* army);
// The layout create_army relies on, checked with a country's own data: its army manager carries
// its tag, and its divisions' handles resolve back to them. False and *why on a mismatch.
bool ArmySelfCheck(int tag, int* divisions, const char** why);

// --- Joining an army (add_to_army; engine_army.cpp, NOTES.md G4) -------------------------------
uint32_t UnitOwnerOffset();  // engine_data.cpp: unit -> owner tag (cross-check)
// The game's move into an existing army and its cleanup of emptied armies (as the UI's "assign
// to army" command uses them), the division getter of division scopes, and the border war field.
bool HaveJoinArmy();
// The land division (CArmy) of a division scope, or null (no unit, or not a land division).
void* ScopeDivision(const void* engineScope);
void* DivisionArmy(const void* division);  // the army it belongs to, or null
bool ArmyInBorderWar(const void* army);
// Moves the division into `army` of its own country, as the UI's assignment does: it leaves its
// old army, which is deleted if left without divisions (*oldDeleted). The old army's name is read
// first (*oldName). False if refused (army group, other country) or if the move did not happen.
bool JoinArmy(void* army, void* division, std::string* oldName, bool* oldDeleted);
// The layout add_to_army relies on: each of the country's divisions that has an army is in that
// army's member list, and the army carries the division's tag. False and *why on a mismatch.
bool JoinSelfCheck(int tag, int* armies, const char** why);

// --- Air scenarios (air_scenarios; engine_air.cpp, NOTES.md Part L) ------------------------------
// The 3D air animations on the map. The game builds one template per scenario slot
// (fighter_1_patrol, bomber_1_CAS, ...) from the entity "<slot>_entity"; the loader builds more
// templates from other entities with the game's own loader and swaps one into a scenario when it
// starts. Graphics only: nothing here reads or writes game state beyond equipment data.
void ResolveAirScenarios();
bool HaveAirScenarios();
int AirSlotCount();
const char* AirSlotName(int slot);  // the game's name table entry
bool AirEntityExists(const char* name);  // the entity database's lookup (logs nothing)
void* AirVanillaTemplate(int slot);      // the game's template for the slot, once built
int AirTemplateNodes(const void* tmpl, bool defenders);
const void* AirTemplateEntity(const void* tmpl);
// Builds a template for `entity` (ending in "_entity") in `slot` with the game's loader: the
// slot's name table entry is swapped for that call only. Call only from the templates-built
// callback. Null on failure (logged).
void* AirBuildTemplate(int slot, const std::string& entity);
// A scenario side: the game's list of plane designs (may be null).
int AirListCount(const void* list);
const void* AirListDesign(const void* list, int i);
// A plane design's equipment type name, its archetype's name ("" if unknown) and the creator tag
// the game uses for the plane model (0 if unknown).
bool AirReadDesign(const void* design, std::string* equipment, std::string* archetype, int* creatorTag);
using AirBuilt_t = void (*)();  // after the game built its templates (every time it does)
// When a scenario starts: a template to play instead of the slot's own, or null for the game's.
using AirPick_t = void* (*)(int slot, const void* attackers, const void* defenders);
bool HookAirScenarios(AirBuilt_t built, AirPick_t pick);  // all hooks or none
void DisableAirScenarios();  // after a fault: the game's own templates from now on

// --- Time control (time_control; engine_time.cpp, NOTES.md Part K) -----------------------------
void ResolveTimeControl();
bool HaveGameSpeed();    // game speed field, its only writer, the speed request, the in-game idler
bool HavePause();        // the idler's pause toggle and paused getter
bool HaveDateDisplay();  // the top bar's date update, widget hide/show, the top bar destructor
bool HaveGameCalendar(); // the game date's hour count and its base (year 0)
bool HaveTimePanel();    // the top bar's time_control_window (date, pause, speed buttons) can be hidden
// Queues the four hooks (top bar destructor, date update, speed writer, pause toggle). Call from
// the feature's init. False if a part is unresolved or a hook could not be queued.
bool HookTimeControl();
// Speeds as the UI shows them: 1..GameSpeedCount(). 0 = unknown / no game.
int GameSpeedCount();
int GameSpeed();
bool IsPaused();  // false without a game
// Main thread, in game: what the speed buttons / the pause button do (single player: applied at
// once or by the next command run). False if not possible now.
bool RequestGameSpeed(int speed);
bool TogglePause();
// Policies the hooks apply (any thread may set them; main thread reads them):
//   lock 1..n: every speed change sets this speed instead (0 = off);
//   blockPause: the player's pause (mode 1: button, Space, console) is refused while running;
//   automatic pauses (event popups, mode 0) and the Esc menu still pause;
//   dateHidden: the top bar's date text is hidden;
//   timePanelHidden: the whole time panel (date, pause and speed buttons) is hidden.
void SetSpeedLock(int speed, bool blockPause);
void SetDateHidden(bool hidden);
void SetTimePanelHidden(bool hidden);
// Main thread: re-runs the live top bar's date update now (so a change shows while paused).
bool RefreshDateDisplay();
// A detour faulted (logged once); the hooks then do what the game does. The feature switches off.
bool TimeHooksFaulted();
// Game date in hours <-> year.month.day.hour (365-day years as the game counts, hour 1..24 as the
// top bar shows it). False on an invalid date or without the calendar.
bool DateToHours(int year, int month, int day, int hour, int* out);
bool HoursToDate(int hours, int* year, int* month, int* day, int* hour);
// Global flag with a value and an engine countdown in days (0 = none). Main/game thread.
bool SetGlobalFlagValue(int token, short value, short days);

// Fires the on_action like the engine fires on_startup: synced-RNG seed, events, random events
// and effect. countryTag > 0: ROOT = that country; 0: no scope. name: at most 15 characters.
// Returns false (and logs) on failure; an exception inside is contained.
bool FireOnAction(const char* name, int countryTag);

}  // namespace Engine
