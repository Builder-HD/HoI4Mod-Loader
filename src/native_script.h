// Native script API: lets loader features add their own script effects and triggers.
//
//   register_native_effect({ name, help, form, params, scopes, execute, tooltip }, feature)
//   register_native_trigger({ name, help, form, params, scopes, evaluate, tooltip }, feature)
//
// Call from the feature's init (DllMain). Commands are added to the engine once the lexer is
// ready (lifecycle feature), before any script is parsed; later calls are refused. A name the
// game already has is refused too. See README "Writing a native effect or trigger" for the rules
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
enum class ParamType { Int, Bool, Key, Country, Choice };
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
    const char* choices = nullptr;  // Choice only: "a|b|c"
};

// Country parameter values below 1 are scope keywords, resolved per evaluation.
enum : int { kCountryThis = -1, kCountryRoot = -2, kCountryFrom = -3, kCountryPrev = -4 };

struct ArgValue {
    bool present = false;
    int i = 0;
    bool b = false;
    const char* s = "";  // Key text (owned by the parsed command), "" if absent
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

private:
    std::string text_;
};

struct NativeEffect {
    const char* name;
    const char* help;
    Form form;
    std::vector<ParamSpec> params;
    uint32_t scopes;  // ScopeMask bits, 0 = any
    void (*execute)(const Args&, const Scope&);
    void (*tooltip)(const Args&, const Scope&, TooltipText&);  // optional; none = no tooltip line
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
    // Optional; none = generic line "<name>: <param>=<value> ...".
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
    void (*subject)(const Args&, const Scope&, TooltipText&);
};

// Returns false (and logs why) if the definition is invalid, the window is closed, or the name
// is already registered by the loader. Game-name conflicts are detected at engine registration.
bool register_native_effect(const NativeEffect& def, const char* feature);
bool register_native_trigger(const NativeTrigger& def, const char* feature);
bool register_native_numeric_trigger(const NativeNumericTrigger& def, const char* feature);

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
enum class CommandKind { Effect, Trigger };
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

// Timing diagnostics for numeric triggers (loader_config.ini [diagnostics] trigger_timing = 1).
void NativeSetTiming(bool on);
bool NativeTimingEnabled();

// Test hook: forget everything (smoke test only).
void NativeScriptResetForTests();

// native_script_engine.cpp: the real backend. Called once from the lexer-ready callback.
void NativeScriptRegisterWithEngine();
