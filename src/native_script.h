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
enum class ParamType { Int, Bool, Key };  // Key: one identifier-like word (loc keys, names)
enum class Form { Value, Block };          // name = <value>   |   name = { key = value ... }

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
    bool required;
};

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

// Returns false (and logs why) if the definition is invalid, the window is closed, or the name
// is already registered by the loader. Game-name conflicts are detected at engine registration.
bool register_native_effect(const NativeEffect& def, const char* feature);
bool register_native_trigger(const NativeTrigger& def, const char* feature);

// True once engine registration ran and every native command of `feature` was registered
// (true for features without native commands). Drives hoi4_loader_feature_<name>.
bool NativeFeatureComplete(const char* feature);
// True once NativeScriptFinalize ran (the registration window is closed).
bool NativeScriptFinalized();

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

    CommandStatus status = CommandStatus::Pending;
    std::string reason;               // why refused
    int token = 0;                    // engine token of `name`
    std::vector<int> paramTokens;     // engine tokens of the parameter names
    volatile long failureLogged = 0;  // one log line per command for runtime failures
    volatile long invalidLogged = 0;
};

// Arguments parsed for one script occurrence of a command. Allocated at parse time, never freed
// (the engine may keep or copy its objects; bounded by script size).
struct ParsedCommand {
    const Command* cmd;
    std::vector<ArgValue> args;
    std::vector<std::string> storage;  // Key text
    bool parseError = false;
};

// What the core needs from the engine; the smoke test provides a fake.
class NativeBackend {
public:
    virtual ~NativeBackend() = default;
    virtual int Intern(const char* s) = 0;                       // 0 on failure
    virtual bool EngineHas(CommandKind kind, int token) = 0;     // vanilla (or any) registration
    virtual bool Bind(Command& cmd, std::string* reason) = 0;    // add to the engine
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

// Test hook: forget everything (smoke test only).
void NativeScriptResetForTests();

// native_script_engine.cpp: the real backend. Called once from the lexer-ready callback.
void NativeScriptRegisterWithEngine();
