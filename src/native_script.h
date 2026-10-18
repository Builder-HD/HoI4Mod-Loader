// Native script API: lets loader features add their own script effects and triggers.
//
//   register_native_effect({ name, help, form, params, scopes, execute, tooltip }, feature)
//   register_native_trigger({ name, help, form, params, scopes, evaluate, tooltip }, feature)
//
// Call from the feature's init (DllMain). Commands are added to the engine once the lexer is
// ready (lifecycle feature), before any script is parsed; later calls are refused. A name the
// game already has is refused too. See DEVELOPING.md "Writing a native effect or trigger" for the rules
// (determinism, trigger performance, guard triggers).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

// ===========================================================================================
// Public API for plugins
// ===========================================================================================
// Key: one identifier-like word (loc keys, names). Country: a tag (GER) or THIS/ROOT/FROM/PREV,
// resolved per evaluation. Choice: one of ParamSpec::choices ("any|own|enemy"), Args::i = index.
// Token: a key interned as an engine token at parse time (Args::i = token, Args::key = text), for
// names the evaluate callback looks up without allocating (equipment names). Text: free text,
// quoted in script if it has spaces (army names); 1-255 bytes, no braces or control characters;
// Args::key = the text.
enum class ParamType { Int, Bool, Key, Country, Choice, Token, Text };
enum class CommandKind { Effect, Trigger };
enum class Form { Value, Block };          // name = <value>   |   name = { key = value ... }

// Where an omitted parameter comes from (evaluated per use). If the scope does not provide it,
// effects do nothing and triggers are false (so NOT = { ... } around them is true); logged once.
enum class ScopeDefault : uint8_t {
    None,
    StrategicRegion,  // the strategic region scope's id
    State,            // the state scope's id
    Country,          // the country scope's tag
    RootCountry,      // ROOT if it is a country, else the current country scope
};

// Engine scope bits (see NOTES.md C1, scope table). 0 = any scope.
namespace ScopeMask {
enum : uint32_t {
    Any = 0,
    State = 0x2,
    Country = 0x4,
    Character = 0x8,
    Combatant = 0x10,
    Ace = 0x20,
    StrategicRegion = 0x40,
    Operation = 0x80,
    Unit = 0x100,
    IndustrialOrg = 0x200,
    PurchaseContract = 0x400,
    RaidInstance = 0x800,
    SpecialProject = 0x1000,
    Faction = 0x2000,
};
}

struct ParamSpec {
    const char* name;  // [a-z0-9_]+
    ParamType type;
    bool required;     // must be written in script (false when fromScope is set)
    ScopeDefault fromScope = ScopeDefault::None;
    const char* choices = nullptr;  // Choice: "a|b|c". Country: "any" also accepts `any`.
    // Declared but not supported yet: writing it is a parse error ("not supported yet"), so the
    // name stays reserved for a later version instead of reading as a typo.
    bool reserved = false;
};

// Country parameter values below 1 are scope keywords, resolved per evaluation; kCountryAny
// (only where the parameter's choices are "any") stays as it is.
enum : int { kCountryThis = -1, kCountryRoot = -2, kCountryFrom = -3, kCountryPrev = -4, kCountryAny = -5 };

struct ArgValue {
    bool present = false;
    int i = 0;
    bool b = false;
    const char* s = "";  // Key text (owned by the parsed command), "" if absent
    bool fromScope = false;  // filled in from the scope (ParamSpec::fromScope), not written in script
};

// Typed arguments in schema order. Plain reads: safe in trigger evaluate (no allocation).
class Args {
public:
    Args(const ArgValue* v, size_t n) : v_(v), n_(n) {}
    size_t size() const { return n_; }
    bool has(size_t i) const { return i < n_ && v_[i].present; }
    int i(size_t i) const { return i < n_ ? v_[i].i : 0; }
    bool b(size_t i) const { return i < n_ && v_[i].b; }
    const char* key(size_t i) const { return i < n_ ? v_[i].s : ""; }
    // True when the value came from the scope, e.g. a region inside a strategic region block.
    // Tooltips use it to leave out what the scope's header already names.
    bool fromScope(size_t i) const { return i < n_ && v_[i].fromScope; }

private:
    const ArgValue* v_;
    size_t n_;
};

// Read-only view of the engine's current script scope (CEventScope). Invalid views read as 0.
class Scope {
public:
    explicit Scope(const void* engineScope) : p_((const uint8_t*)engineScope) {}
    bool valid() const { return p_ != nullptr; }
    uint32_t types() const;          // ScopeMask bits present in this scope
    int stateId() const;             // 0 if not a state scope
    int countryTag() const;          // engine tag index, 0 if not a country scope
    int strategicRegionId() const;   // 0 if not a strategic region scope
    Scope root() const;              // ROOT / FROM / PREV; invalid if unset
    Scope from() const;
    Scope prev() const;
    const void* raw() const { return p_; }

private:
    Scope link(size_t offset) const;
    const uint8_t* p_;
};

// Tooltip text builder (UTF-8). Allocation is fine here: tooltips run in UI code only.
class TooltipText {
public:
    TooltipText& add(const char* s) { text_ += s ? s : ""; return *this; }
    TooltipText& add(int v) { text_ += std::to_string(v); return *this; }
    const std::string& text() const { return text_; }
    // Numeric subjects: the scope lacks what the value is about (e.g. no area under an
    // iterator's header), so the line shows no current value.
    void markScopeMissing() { scopeMissing_ = true; }
    bool scopeMissing() const { return scopeMissing_; }

private:
    std::string text_;
    bool scopeMissing_ = false;
};

// A tooltip text a mod may translate: its localisation key and the English text used when no
// loaded localisation defines the key. Placeholders use the game's syntax: $NAME$, $NAME|Y$.
struct LocText {
    const char* key;
    const char* english;
};
struct LocArg {
    const char* name;   // placeholder name without $
    std::string value;  // text put in its place
};
// `text` with its placeholders filled in: the game's localisation of text.key if one exists
// (formatted by the game), else text.english. UI code only (tooltips): it allocates.
std::string native_localize(const LocText& text, const std::vector<LocArg>& args = {});
// The localisation of a plain key (e.g. a name key written in script), or the key itself.
std::string native_localize_key(const char* key);

struct NativeEffect {
    const char* name;
    const char* help;
    Form form;
    std::vector<ParamSpec> params;
    uint32_t scopes;  // ScopeMask bits, 0 = any
    void (*execute)(const Args&, const Scope&);
    void (*tooltip)(const Args&, const Scope&, TooltipText&);  // optional; none = no tooltip line
    std::vector<LocText> texts = {};  // the tooltip texts it uses (listed in the docs)
};

struct NativeTrigger {
    const char* name;
    const char* help;
    Form form;
    std::vector<ParamSpec> params;
    uint32_t scopes;
    // Hot path: called by AI and UI, possibly on several threads at once. No locks, no
    // allocation, no logging, game state only.
    bool (*evaluate)(const Args&, const Scope&);
    // Optional; none = generic line "<name>: <param>=<value> ...". A parameter whose scope
    // default the scope lacks (e.g. no region under an iterator's header) is absent (Args::has).
    void (*tooltip)(const Args&, const Scope&, bool negated, TooltipText&);
};

// Numeric data trigger, compared like vanilla numeric triggers (NOTES.md E1):
//   Count: whole numbers; `>`, `<`, and `=` meaning equal.
//   Ratio: fixed point 0..1 (100000 = 1.0); `<` means below, `>` and `=` both mean above (vanilla).
// Forms: `name > 0.6` when every parameter can come from scope (or there are none), and
// `name = { <params> <compareKey> > 0.6 }`. The right side may be a number or a variable.
// Commands whose parameters all come from scope and whose scopes include Country or State are
// also registered as game variables of the same name (set_variable = { x = name }).
enum class NumericKind { Count, Ratio };
struct NativeNumericTrigger {
    const char* name;
    const char* help;
    NumericKind kind;
    std::vector<ParamSpec> params;  // may be empty (value form only)
    uint32_t scopes;
    const char* compareKey;  // block form key holding the comparison: "count" / "value"
    // Hot path, same rules as NativeTrigger::evaluate. Count: the count; Ratio: 0..100000.
    // Return false if the value does not exist (unknown region etc.): the trigger is false.
    bool (*value)(const Args&, const Scope&, int64_t* out);
    // Optional; the subject of the tooltip line, e.g. "Air superiority over Bay of Biscay (42)".
    // A parameter whose scope default the scope lacks is absent (Args::has): leave it out, e.g.
    // "Air superiority of FRA" under an iterator's header. The line then shows no current value.
    void (*subject)(const Args&, const Scope&, TooltipText&);
};

// Returns false (and logs why) if the definition is invalid, the window is closed, or the name
// is already registered by the loader. Game-name conflicts are detected at engine registration.
bool register_native_effect(const NativeEffect& def, const char* feature);
bool register_native_trigger(const NativeTrigger& def, const char* feature);
bool register_native_numeric_trigger(const NativeNumericTrigger& def, const char* feature);

// --- Scope blocks and iterators (NOTES.md F5) -------------------------------------------------
// The scope a block runs its children in: one ScopeMask bit (only StrategicRegion is supported,
// id = the region id).
struct ScopeTarget {
    uint32_t type = 0;
    int id = 0;
};

// Scope block: `name = { <params> <children> }`. The block's other keys are triggers (or
// effects), vanilla or native, run in the scope `target` picks per use; when the script is parsed
// they are checked against `childScope`. A trigger is true if all children are true there (false
// without a target, so true under NOT); an effect runs them there (nothing without a target).
// Tooltip: the target's name and ":", then the children indented and evaluated in the target.
struct NativeScopeBlock {
    const char* name;
    const char* help;
    CommandKind kind;
    std::vector<ParamSpec> params;  // the block's own keys (optional ones); may be empty
    uint32_t scopes;                // where the block may be used, 0 = any
    uint32_t childScope;            // ScopeMask bit the children run in (StrategicRegion)
    // Hot path, same rules as NativeTrigger::evaluate. False: no target in this scope.
    bool (*target)(const Args&, const Scope&, ScopeTarget* out);
};

// Iterator over every object of one scope type, in id order, cloned from the game's state
// iterators (block keys and tooltips as there):
//   Any (trigger):   any_x = { count = <int> tooltip = <key> <triggers> }                as any_state
//   Every (effect):  every_x = { limit = { <triggers> } random_select_amount = <int>
//                      display_individual_scopes = yes tooltip = <key> <effects> }      as every_state
//   Random (effect): random_x = { limit = { <triggers> } tooltip = <key> <effects> }    as random_state;
//                      prioritize (state ids) is a parse error
enum class IteratorKind { Any, Every, Random };
struct NativeIterator {
    const char* name;
    const char* help;
    IteratorKind kind;
    uint32_t elements;       // ScopeMask bit of the objects iterated (StrategicRegion)
    const char* headerKey;   // Any: tooltip header localisation key (a mod may define it) ...
    const char* headerText;  // ... and the text shown when it is not localised
};

bool register_native_scope_block(const NativeScopeBlock& def, const char* feature);
bool register_native_iterator(const NativeIterator& def, const char* feature);

// True once engine registration ran and every native command of `feature` was registered
// (true for features without native commands). Drives hoi4_loader_feature_<name>.
bool NativeFeatureComplete(const char* feature);
// True once NativeScriptFinalize ran (the registration window is closed).
bool NativeScriptFinalized();

// Documents an on_action that the loader fires (section "On actions" of loader_script_docs.md).
// On_action keys are dynamic in the engine, so the on_action itself needs no registration.
// Call from the feature's init, before engine registration writes the docs.
void document_on_action(const char* name, const char* feature, const char* help);

// ===========================================================================================
// Internal: core <-> engine binding (native_script.cpp / native_script_engine.cpp)
// ===========================================================================================
enum class CommandStatus { Pending, Registered, Refused };

struct Command {
    CommandKind kind;
    std::string feature;
    std::string name;
    std::string help;
    Form form;
    std::vector<ParamSpec> params;
    std::vector<std::string> paramNames;  // owns ParamSpec::name storage
    uint32_t scopes;
    void (*execute)(const Args&, const Scope&) = nullptr;
    void (*effectTooltip)(const Args&, const Scope&, TooltipText&) = nullptr;
    bool (*evaluate)(const Args&, const Scope&) = nullptr;
    void (*triggerTooltip)(const Args&, const Scope&, bool, TooltipText&) = nullptr;
    // Numeric triggers (kind == Trigger).
    bool numeric = false;
    NumericKind numericKind = NumericKind::Count;
    std::string compareKey;
    bool (*value)(const Args&, const Scope&, int64_t*) = nullptr;
    void (*subject)(const Args&, const Scope&, TooltipText&) = nullptr;
    std::vector<std::string> choiceStorage;  // owns ParamSpec::choices
    // Scope blocks and iterators (bound by native_scope_engine.cpp).
    bool scopeBlock = false;
    uint32_t childScope = 0;
    bool (*target)(const Args&, const Scope&, ScopeTarget*) = nullptr;
    bool iterator = false;
    IteratorKind iteratorKind = IteratorKind::Any;
    std::string headerKey, headerText;
    std::string keysDoc;  // iterators: the block keys the engine's template reads (set when bound)
    std::vector<LocText> texts;  // translatable tooltip texts (docs)

    CommandStatus status = CommandStatus::Pending;
    std::string reason;               // why refused
    int token = 0;                    // engine token of `name`
    std::vector<int> paramTokens;     // engine tokens of the parameter names
    int compareToken = 0;             // engine token of compareKey
    bool variable = false;            // also registered as a game variable
    volatile long failureLogged = 0;  // one log line per command for runtime failures
    volatile long invalidLogged = 0;
    volatile long scopeLogged = 0;    // a scope default was unavailable (logged once)
    // Timing diagnostics (NativeSetTiming): evaluations and QueryPerformanceCounter ticks.
    volatile long long evalCount = 0;
    volatile long long evalTicks = 0;

    bool ValueFormAllowed() const;    // every parameter optional or from scope
};

// Arguments parsed for one script occurrence of a command. Allocated at parse time, never freed
// (the engine may keep or copy its objects; bounded by script size).
struct ParsedCommand {
    const Command* cmd;
    std::vector<ArgValue> args;
    std::vector<std::string> storage;  // Key text
    bool parseError = false;
    // Numeric triggers: which form was written; block form needs its compare key.
    bool valueForm = false;
    bool comparePresent = false;
};

// What the core needs from the engine; the smoke test provides a fake.
class NativeBackend {
public:
    virtual ~NativeBackend() = default;
    virtual int Intern(const char* s) = 0;                       // 0 on failure
    virtual bool EngineHas(CommandKind kind, int token) = 0;     // vanilla (or any) registration
    virtual bool Bind(Command& cmd, std::string* reason) = 0;    // add to the engine
    virtual int CountryTag(const char* text) { (void)text; return 0; }  // "GER" -> tag, 0 if unknown
    // The game's localisation of key with $NAME$ arguments; false if it has no entry.
    virtual bool Localize(const char* key, const std::vector<LocArg>& args, std::string* out) {
        (void)key, (void)args, (void)out;
        return false;
    }
};

// Engine registration for all pending commands; closes the window; writes the docs file
// (docsPath may be empty to skip). Returns the number registered.
int NativeScriptFinalize(NativeBackend& backend, const std::wstring& docsPath);
std::string NativeScriptDocs();          // the docs file content
const std::vector<Command*>& NativeCommands();

// Parse-time helpers (binding): find the parameter for a key token; set it from value text.
int NativeParamIndex(const Command& cmd, int keyToken);
ParsedCommand* NativeNewParsed(const Command& cmd);
// Converts text by the parameter's type. Returns false and fills *error on a type error.
bool NativeSetArg(ParsedCommand& pc, size_t index, const char* text, std::string* error);
// All required parameters present and no parse error. Logs the first problem per command.
bool NativeValidate(ParsedCommand& pc);

// Runtime helpers (binding): run callbacks under SEH; log once per command on failure.
void NativeExecute(const ParsedCommand* pc, const void* engineScope);
bool NativeEvaluate(const ParsedCommand* pc, const void* engineScope);
// Tooltip text (UTF-8, no trailing newline). Triggers without a tooltip callback get the
// generic line; effects without one get "".
std::string NativeTooltip(const ParsedCommand* pc, const void* engineScope, bool negated);

// Scope blocks: the target for this use. False if a scope default is missing, the callback finds
// none or crashed (logged once).
bool NativeScopeTarget(const ParsedCommand* pc, const void* engineScope, ScopeTarget* out);

// Numeric triggers. Value: false if a scope default is missing, the callback says the value does
// not exist, or it crashed (all logged once).
bool NativeNumericValue(const ParsedCommand* pc, const void* engineScope, int64_t* out);
enum class CompareOp { Greater, Less, Equal };
// The engine's comparison semantics (see NativeNumericTrigger).
bool NativeCompare(NumericKind kind, CompareOp op, int64_t value, int64_t rhs);
// "Air superiority over Bay of Biscay (42) above 60% (currently 64%)". rhsKnown = false when the
// right side could not be evaluated (shown as "?").
std::string NativeNumericTooltip(const ParsedCommand* pc, const void* engineScope, bool negated, CompareOp op, int64_t rhs,
                                 bool rhsKnown);
// Fixed point (100000 = 1) as text: Ratio -> "64%", Count -> "3".
std::string NativeFormatNumber(NumericKind kind, int64_t v);

// Timing diagnostics for numeric triggers, scope blocks and iterators (loader_config.ini
// [diagnostics] trigger_timing = 1). NativeTimingAdd records one evaluation of `ticks`
// QueryPerformanceCounter ticks.
void NativeSetTiming(bool on);
bool NativeTimingEnabled();
void NativeTimingAdd(const Command& c, long long ticks);

// Test hook: forget everything (smoke test only).
void NativeScriptResetForTests();

// native_script_engine.cpp: the real backend. Called once from the lexer-ready callback.
void NativeScriptRegisterWithEngine();
