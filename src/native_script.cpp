// Native script API, engine-free core: schema validation, the registration window, conflict
// checks, typed argument parsing, scope defaults, SEH-guarded callbacks, numeric comparisons,
// tooltips and the generated docs. The engine side lives in native_script_engine.cpp; the core
// test links this file with a fake NativeBackend and a fake Scope.
#include "native_script.h"

#include <windows.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loader.h"

namespace {

std::vector<Command*> g_commands;
bool g_finalized = false;
NativeBackend* g_backend = nullptr;  // set by NativeScriptFinalize; resolves country tags at parse time
bool g_timing = false;

const size_t kMaxParams = 8;  // runtime argument resolution uses a fixed stack array

const char* KindName(CommandKind k) { return k == CommandKind::Effect ? "effect" : "trigger"; }
const char* AKind(CommandKind k) { return k == CommandKind::Effect ? "an effect" : "a trigger"; }

bool ValidIdentifier(const char* s) {
    if (!s || !*s || strlen(s) > 64) return false;
    for (const char* p = s; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return true;
}

const char* TypeName(ParamType t) {
    switch (t) {
        case ParamType::Int: return "int";
        case ParamType::Bool: return "bool";
        case ParamType::Country: return "country";
        case ParamType::Choice: return "choice";
        case ParamType::Token: return "name";
        case ParamType::Text: return "text";
        default: return "key";
    }
}

const char* DefaultName(ScopeDefault d) {
    switch (d) {
        case ScopeDefault::StrategicRegion: return "the strategic region scope";
        case ScopeDefault::State: return "the state scope";
        case ScopeDefault::Country: return "the country scope";
        case ScopeDefault::RootCountry: return "ROOT (or the country scope)";
        default: return "";
    }
}

// Index of `text` in "a|b|c", or -1.
int ChoiceIndex(const char* choices, const char* text) {
    int i = 0;
    for (const char* p = choices; p && *p; ++i) {
        const char* bar = strchr(p, '|');
        size_t n = bar ? (size_t)(bar - p) : strlen(p);
        if (strlen(text) == n && strncmp(p, text, n) == 0) return i;
        if (!bar) break;
        p = bar + 1;
    }
    return -1;
}

std::string CheckParams(const std::vector<ParamSpec>& params) {
    if (params.size() > kMaxParams) return "at most 8 parameters";
    for (size_t i = 0; i < params.size(); ++i) {
        const ParamSpec& p = params[i];
        if (!ValidIdentifier(p.name)) return "parameter names must be 1-64 characters of a-z 0-9 _";
        for (size_t j = 0; j < i; ++j)
            if (strcmp(p.name, params[j].name) == 0) return std::string("duplicate parameter '") + p.name + "'";
        if (p.type == ParamType::Choice) {
            if (!p.choices || !*p.choices) return std::string("choice parameter '") + p.name + "' needs choices";
            for (const char* c = p.choices; *c; ++c)
                if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '|'))
                    return std::string("choices of '") + p.name + "' must be a-z 0-9 _ separated by |";
        }
        if (p.type == ParamType::Country && p.choices && strcmp(p.choices, "any") != 0)
            return std::string("country parameter '") + p.name + "' accepts only \"any\" as extra choice";
        if (p.reserved && (p.required || p.fromScope != ScopeDefault::None))
            return std::string("reserved parameter '") + p.name + "' cannot be required or come from scope";
        if (p.fromScope != ScopeDefault::None) {
            if (p.required) return std::string("parameter '") + p.name + "' comes from scope, so it cannot be required";
            bool fits = (p.fromScope == ScopeDefault::Country || p.fromScope == ScopeDefault::RootCountry)
                            ? p.type == ParamType::Country
                            : p.type == ParamType::Int;
            if (!fits) return std::string("scope default of '") + p.name + "' does not match its type";
        }
    }
    return "";
}

// Shared checks for both kinds. Returns "" if fine, else the reason.
std::string CheckSchema(CommandKind kind, const char* name, const char* help, Form form,
                        const std::vector<ParamSpec>& params, bool haveCallback) {
    if (g_finalized) return "registration window is closed (engine registration already ran)";
    if (!ValidIdentifier(name)) return "name must be 1-64 characters of a-z 0-9 _";
    if (!help || !*help) return "help text is required";
    if (!haveCallback) return kind == CommandKind::Effect ? "execute callback is required" : "evaluate callback is required";
    if (form == Form::Value && (params.size() != 1 || !params[0].required))
        return "value form needs exactly one required parameter";
    if (form == Form::Block && params.empty()) return "block form needs at least one parameter";
    std::string why = CheckParams(params);
    if (!why.empty()) return why;
    for (const Command* c : g_commands)
        if (c->kind == kind && c->name == name) return std::string(AKind(kind)) + " with this name is already registered by the loader";
    return "";
}

Command* NewCommand(CommandKind kind, const char* feature, const char* name, const char* help, Form form,
                    const std::vector<ParamSpec>& params, uint32_t scopes) {
    auto* c = new Command{};
    c->kind = kind;
    c->feature = feature ? feature : "";
    c->name = name;
    c->help = help;
    c->form = form;
    c->scopes = scopes;
    c->paramNames.reserve(params.size());
    c->choiceStorage.reserve(params.size());
    for (const ParamSpec& p : params) {
        c->paramNames.push_back(p.name);
        c->choiceStorage.push_back(p.choices ? p.choices : "");
    }
    for (size_t i = 0; i < params.size(); ++i)
        c->params.push_back({c->paramNames[i].c_str(), params[i].type, params[i].required, params[i].fromScope,
                             params[i].choices ? c->choiceStorage[i].c_str() : nullptr, params[i].reserved});
    return c;
}

bool Refuse(CommandKind kind, const char* name, const char* feature, const std::string& why) {
    Log("Native %s '%s' (feature %s): REFUSED: %s", KindName(kind), name ? name : "(null)", feature ? feature : "?",
        why.c_str());
    return false;
}

// Scope mask -> names, as the engine's own table (NOTES.md C1).
std::string ScopeNames(uint32_t mask) {
    static const struct {
        uint32_t bit;
        const char* name;
    } kNames[] = {{0x2, "State"}, {0x4, "Country"}, {0x8, "Character"}, {0x10, "Combatant"}, {0x20, "Ace"},
                  {0x40, "StrategicRegion"}, {0x80, "Operation"}, {0x100, "Unit"},
                  {0x200, "Military Industrial Organisation"}, {0x400, "Purchase Contract"},
                  {0x800, "Raid Instance"}, {0x1000, "Special Project"}, {0x2000, "Faction"}};
    if (mask == 0) return "any";
    std::string s;
    for (const auto& n : kNames) {
        if (!(mask & n.bit)) continue;
        if (!s.empty()) s += ", ";
        s += n.name;
    }
    return s;
}

std::string ParamUsage(const ParamSpec& p) {
    if (p.type == ParamType::Choice) {
        std::string c = p.choices;
        for (char& ch : c)
            if (ch == '|') ch = '/';
        return c;
    }
    if (p.type == ParamType::Country && p.choices) return "<country>/any";
    return std::string("<") + TypeName(p.type) + ">";
}

const char* ChildrenWord(CommandKind k) { return k == CommandKind::Effect ? "<effects>" : "<triggers>"; }

std::string Usage(const Command& c) {
    if (c.iterator) {
        switch (c.iteratorKind) {
            case IteratorKind::Any: return "`" + c.name + " = { count = <int> tooltip = <key> <triggers> }`";
            case IteratorKind::Every:
                return "`" + c.name + " = { limit = { <triggers> } random_select_amount = <int> display_individual_scopes = yes "
                       "tooltip = <key> <effects> }`";
            default: return "`" + c.name + " = { limit = { <triggers> } tooltip = <key> <effects> }`";
        }
    }
    if (c.scopeBlock) {
        std::string s = "`" + c.name + " = {";
        for (const ParamSpec& p : c.params)
            if (!p.reserved) s += std::string(" ") + p.name + " = " + ParamUsage(p);
        return s + " " + ChildrenWord(c.kind) + " }`";
    }
    if (c.effectBlock) {
        std::string s = "`" + c.name + " = {";
        for (const ParamSpec& p : c.params)
            if (!p.reserved) s += std::string(" ") + p.name + " = " + ParamUsage(p);
        for (const std::string& child : c.childNames) s += " " + child + " = { ... }";
        return s + " }`";
    }
    if (c.numeric) {
        const char* rhs = c.numericKind == NumericKind::Ratio ? "<0..1>" : "<int>";
        std::string s;
        if (c.ValueFormAllowed()) s += "`" + c.name + " > " + rhs + "`";
        if (!c.params.empty()) {
            if (!s.empty()) s += " or ";
            s += "`" + c.name + " = {";
            for (const ParamSpec& p : c.params)
                if (!p.reserved) s += std::string(" ") + p.name + " = " + ParamUsage(p);
            s += " " + c.compareKey + " > " + rhs + " }`";
        }
        return s;
    }
    std::string s = "`" + c.name + " = ";
    if (c.form == Form::Value) return s + ParamUsage(c.params[0]) + "`";
    s += "{";
    for (const ParamSpec& p : c.params)
        if (!p.reserved) s += std::string(" ") + p.name + " = " + ParamUsage(p);
    return s + " }`";
}

void LogOnce(volatile long* flag, const char* fmt, const char* a, const char* b) {
    if (InterlockedExchange(flag, 1) == 0) Log(fmt, a, b);
}

// --- SEH wrappers: no objects with destructors in these ----------------------------------------
bool SafeExecute(void (*fn)(const Args&, const Scope&), const Args* a, const Scope* s, DWORD* code) {
    __try {
        fn(*a, *s);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

bool SafeExecuteBlock(void (*fn)(const Args&, const Scope&, const EffectChildren&), const Args* a, const Scope* s,
                      const EffectChildren* children, DWORD* code) {
    __try {
        fn(*a, *s, *children);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

bool SafeEvaluate(bool (*fn)(const Args&, const Scope&), const Args* a, const Scope* s, bool* result, DWORD* code) {
    __try {
        *result = fn(*a, *s);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

bool SafeValue(bool (*fn)(const Args&, const Scope&, int64_t*), const Args* a, const Scope* s, int64_t* out, bool* result,
               DWORD* code) {
    __try {
        *result = fn(*a, *s, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

bool SafeTarget(bool (*fn)(const Args&, const Scope&, ScopeTarget*), const Args* a, const Scope* s, ScopeTarget* out,
                bool* result, DWORD* code) {
    __try {
        *result = fn(*a, *s, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

bool SafeEffectTooltip(void (*fn)(const Args&, const Scope&, TooltipText&), const Args* a, const Scope* s, TooltipText* out,
                       DWORD* code) {
    __try {
        fn(*a, *s, *out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

bool SafeTriggerTooltip(void (*fn)(const Args&, const Scope&, bool, TooltipText&), const Args* a, const Scope* s, bool neg,
                        TooltipText* out, DWORD* code) {
    __try {
        fn(*a, *s, neg, *out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

void LogException(const Command& c, const char* what, DWORD code) {
    if (InterlockedExchange(&const_cast<Command&>(c).failureLogged, 1) == 0)
        Log("Native %s '%s': %s crashed (exception 0x%08lX); %s (logged once)", KindName(c.kind), c.name.c_str(), what,
            code, c.kind == CommandKind::Effect ? "it did nothing" : "it returned false");
}

// --- Runtime argument resolution (hot path: no allocation) ---------------------------------------
Scope RootOrSelf(const Scope& s) {
    Scope r = s.root();
    return r.valid() ? r : s;
}

int CountryFromKeyword(int code, const Scope& s) {
    switch (code) {
        case kCountryThis: return s.countryTag();
        case kCountryRoot: return RootOrSelf(s).countryTag();
        case kCountryFrom: return s.from().countryTag();
        case kCountryPrev: return s.prev().countryTag();
        default: return code > 0 ? code : 0;
    }
}

// Copies the parsed arguments to `out` with scope defaults and country keywords resolved.
// Returns false (and logs once) if a value the command needs is not available in this scope.
// `partial` (tooltips): a scope default the scope does not provide stays absent instead, without
// a log line; *missing (if given) tells whether that happened.
bool ResolveArgs(const ParsedCommand& pc, const Scope& s, ArgValue* out, bool partial = false, bool* missing = nullptr) {
    if (missing) *missing = false;
    const Command& c = *pc.cmd;
    for (size_t i = 0; i < pc.args.size(); ++i) {
        out[i] = pc.args[i];
        const ParamSpec& p = c.params[i];
        if (!out[i].present && p.fromScope != ScopeDefault::None) {
            int v = 0;
            switch (p.fromScope) {
                case ScopeDefault::StrategicRegion: v = s.strategicRegionId(); break;
                case ScopeDefault::State: v = s.stateId(); break;
                case ScopeDefault::Country: v = s.countryTag(); break;
                case ScopeDefault::RootCountry: {
                    v = RootOrSelf(s).countryTag();
                    if (v <= 0) v = s.countryTag();
                    break;
                }
                default: break;
            }
            if (v <= 0 && partial) {
                if (missing) *missing = true;
                continue;
            }
            if (v <= 0) {
                LogOnce(&const_cast<Command&>(c).scopeLogged,
                        "Native command '%s': '%s' not given and not available from the scope; it does nothing / is "
                        "false (logged once)",
                        c.name.c_str(), p.name);
                return false;
            }
            out[i].i = v;
            out[i].present = true;
            out[i].fromScope = true;
            continue;
        }
        if (out[i].present && p.type == ParamType::Country && out[i].i != kCountryAny) {
            out[i].i = CountryFromKeyword(out[i].i, s);
            if (out[i].i <= 0) {
                LogOnce(&const_cast<Command&>(c).scopeLogged,
                        "Native command '%s': '%s' names a scope that is not a country here; it does nothing / is "
                        "false (logged once)",
                        c.name.c_str(), p.name);
                return false;
            }
        }
    }
    return true;
}

std::string FormatFixed(int64_t v, int decimals) {
    bool neg = v < 0;
    uint64_t a = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    uint64_t scale = 1;
    for (int i = 0; i < 5 - decimals; ++i) scale *= 10;
    a = (a + scale / 2) / scale;  // round half up
    uint64_t div = 1;
    for (int i = 0; i < decimals; ++i) div *= 10;
    std::string s = std::to_string(a / div);
    if (decimals > 0) {
        std::string frac = std::to_string(a % div);
        while ((int)frac.size() < decimals) frac = "0" + frac;
        while (!frac.empty() && frac.back() == '0') frac.pop_back();
        if (!frac.empty()) s += "." + frac;
    }
    return neg && s != "0" ? "-" + s : s;
}

}  // namespace

bool Command::ValueFormAllowed() const {
    for (const ParamSpec& p : params)
        if (p.required) return false;
    return true;
}

// ============================================================================================
// Registration
// ============================================================================================
bool register_native_effect(const NativeEffect& d, const char* feature) {
    std::string why = CheckSchema(CommandKind::Effect, d.name, d.help, d.form, d.params, d.execute != nullptr);
    for (const LocText& t : d.texts)
        if (why.empty() && (!t.key || !*t.key || !t.english || !*t.english)) why = "a tooltip text needs a key and an English text";
    if (!why.empty()) return Refuse(CommandKind::Effect, d.name, feature, why);
    Command* c = NewCommand(CommandKind::Effect, feature, d.name, d.help, d.form, d.params, d.scopes);
    c->execute = d.execute;
    c->effectTooltip = d.tooltip;
    c->texts = d.texts;
    g_commands.push_back(c);
    Log("Native effect '%s' (feature %s) queued for engine registration", d.name, feature ? feature : "?");
    return true;
}

bool register_native_trigger(const NativeTrigger& d, const char* feature) {
    std::string why = CheckSchema(CommandKind::Trigger, d.name, d.help, d.form, d.params, d.evaluate != nullptr);
    if (!why.empty()) return Refuse(CommandKind::Trigger, d.name, feature, why);
    Command* c = NewCommand(CommandKind::Trigger, feature, d.name, d.help, d.form, d.params, d.scopes);
    c->evaluate = d.evaluate;
    c->triggerTooltip = d.tooltip;
    g_commands.push_back(c);
    Log("Native trigger '%s' (feature %s) queued for engine registration", d.name, feature ? feature : "?");
    return true;
}

bool register_native_numeric_trigger(const NativeNumericTrigger& d, const char* feature) {
    std::string why;
    if (g_finalized) why = "registration window is closed (engine registration already ran)";
    else if (!ValidIdentifier(d.name)) why = "name must be 1-64 characters of a-z 0-9 _";
    else if (!d.help || !*d.help) why = "help text is required";
    else if (!d.value) why = "value callback is required";
    else if (!d.params.empty() && !ValidIdentifier(d.compareKey)) why = "compare key must be 1-64 characters of a-z 0-9 _";
    else why = CheckParams(d.params);
    for (size_t i = 0; why.empty() && i < d.params.size(); ++i)
        if (d.compareKey && strcmp(d.params[i].name, d.compareKey) == 0) why = "compare key clashes with a parameter";
    for (const Command* c : g_commands)
        if (why.empty() && c->kind == CommandKind::Trigger && c->name == d.name)
            why = "a trigger with this name is already registered by the loader";
    if (!why.empty()) return Refuse(CommandKind::Trigger, d.name, feature, why);
    Command* c = NewCommand(CommandKind::Trigger, feature, d.name, d.help, d.params.empty() ? Form::Value : Form::Block, d.params,
                            d.scopes);
    c->numeric = true;
    c->numericKind = d.kind;
    c->compareKey = d.compareKey ? d.compareKey : "";
    c->value = d.value;
    c->subject = d.subject;
    g_commands.push_back(c);
    Log("Native numeric trigger '%s' (feature %s) queued for engine registration", d.name, feature ? feature : "?");
    return true;
}

bool register_native_scope_block(const NativeScopeBlock& d, const char* feature) {
    std::string why;
    if (g_finalized) why = "registration window is closed (engine registration already ran)";
    else if (!ValidIdentifier(d.name)) why = "name must be 1-64 characters of a-z 0-9 _";
    else if (!d.help || !*d.help) why = "help text is required";
    else if (!d.target) why = "target callback is required";
    else if (d.childScope != ScopeMask::StrategicRegion) why = "only strategic region scope blocks are supported";
    else why = CheckParams(d.params);
    for (const Command* c : g_commands)
        if (why.empty() && c->kind == d.kind && c->name == d.name)
            why = std::string(AKind(d.kind)) + " with this name is already registered by the loader";
    if (!why.empty()) return Refuse(d.kind, d.name, feature, why);
    Command* c = NewCommand(d.kind, feature, d.name, d.help, Form::Block, d.params, d.scopes);
    c->scopeBlock = true;
    c->childScope = d.childScope;
    c->target = d.target;
    g_commands.push_back(c);
    Log("Native scope block %s '%s' (feature %s) queued for engine registration", KindName(d.kind), d.name,
        feature ? feature : "?");
    return true;
}

bool register_native_iterator(const NativeIterator& d, const char* feature) {
    CommandKind kind = d.kind == IteratorKind::Any ? CommandKind::Trigger : CommandKind::Effect;
    std::string why;
    if (g_finalized) why = "registration window is closed (engine registration already ran)";
    else if (!ValidIdentifier(d.name)) why = "name must be 1-64 characters of a-z 0-9 _";
    else if (!d.help || !*d.help) why = "help text is required";
    else if (d.elements != ScopeMask::StrategicRegion) why = "only strategic region iterators are supported";
    else if (d.kind == IteratorKind::Any && (!d.headerText || !*d.headerText)) why = "an any iterator needs a header text";
    for (const Command* c : g_commands)
        if (why.empty() && c->kind == kind && c->name == d.name)
            why = std::string(AKind(kind)) + " with this name is already registered by the loader";
    if (!why.empty()) return Refuse(kind, d.name, feature, why);
    Command* c = NewCommand(kind, feature, d.name, d.help, Form::Block, {}, ScopeMask::Any);
    c->iterator = true;
    c->iteratorKind = d.kind;
    c->childScope = d.elements;
    c->headerKey = d.headerKey ? d.headerKey : "";
    c->headerText = d.headerText ? d.headerText : "";
    if (d.headerKey && *d.headerKey && d.headerText) c->texts.push_back({d.headerKey, d.headerText});
    g_commands.push_back(c);
    Log("Native iterator %s '%s' (feature %s) queued for engine registration", KindName(kind), d.name, feature ? feature : "?");
    return true;
}

bool register_native_effect_block(const NativeEffectBlock& d, const char* feature) {
    std::string why;
    if (g_finalized) why = "registration window is closed (engine registration already ran)";
    else if (!ValidIdentifier(d.name)) why = "name must be 1-64 characters of a-z 0-9 _";
    else if (!d.help || !*d.help) why = "help text is required";
    else if (!d.execute) why = "execute callback is required";
    else if (d.children.empty()) why = "an effect block needs at least one allowed child effect";
    else why = CheckParams(d.params);
    for (size_t i = 0; why.empty() && i < d.children.size(); ++i) {
        const char* child = d.children[i];
        if (!ValidIdentifier(child)) why = "child effect names must be 1-64 characters of a-z 0-9 _";
        else if (strcmp(child, d.name) == 0) why = "an effect block cannot contain itself";
        for (const ParamSpec& p : d.params)
            if (why.empty() && strcmp(p.name, child) == 0) why = std::string("child effect '") + child + "' clashes with a parameter";
        for (size_t j = 0; why.empty() && j < i; ++j)
            if (strcmp(d.children[j], child) == 0) why = std::string("duplicate child effect '") + child + "'";
    }
    for (const LocText& t : d.texts)
        if (why.empty() && (!t.key || !*t.key || !t.english || !*t.english)) why = "a tooltip text needs a key and an English text";
    for (const Command* c : g_commands)
        if (why.empty() && c->kind == CommandKind::Effect && c->name == d.name)
            why = "an effect with this name is already registered by the loader";
    if (!why.empty()) return Refuse(CommandKind::Effect, d.name, feature, why);
    Command* c = NewCommand(CommandKind::Effect, feature, d.name, d.help, Form::Block, d.params, d.scopes);
    c->effectBlock = true;
    for (const char* child : d.children) c->childNames.push_back(child);
    c->blockExecute = d.execute;
    c->effectTooltip = d.tooltip;
    c->texts = d.texts;
    g_commands.push_back(c);
    Log("Native effect block '%s' (feature %s) queued for engine registration", d.name, feature ? feature : "?");
    return true;
}

bool NativeScriptFinalized() { return g_finalized; }

namespace {
struct OnActionDoc {
    std::string name, feature, help;
};
std::vector<OnActionDoc> g_onActionDocs;
}  // namespace

void document_on_action(const char* name, const char* feature, const char* help) {
    g_onActionDocs.push_back({name ? name : "", feature ? feature : "?", help ? help : ""});
}

bool NativeFeatureComplete(const char* feature) {
    bool any = false;
    for (const Command* c : g_commands) {
        if (c->feature != feature) continue;
        any = true;
        if (c->status != CommandStatus::Registered) return false;
    }
    return !any || g_finalized;
}

const std::vector<Command*>& NativeCommands() { return g_commands; }

int NativeScriptFinalize(NativeBackend& backend, const std::wstring& docsPath) {
    g_backend = &backend;
    int registered = 0;
    for (Command* c : g_commands) {
        if (c->status != CommandStatus::Pending) continue;
        c->token = backend.Intern(c->name.c_str());
        std::string reason;
        if (!c->token) {
            reason = "cannot create a script token for the name";
        } else if (backend.EngineHas(c->kind, c->token)) {
            reason = std::string("the game already has ") + AKind(c->kind) + " with this name";
        } else {
            c->paramTokens.clear();
            for (const ParamSpec& p : c->params) c->paramTokens.push_back(backend.Intern(p.name));
            for (size_t i = 0; i < c->paramTokens.size() && reason.empty(); ++i)
                if (!c->paramTokens[i]) reason = std::string("cannot create a token for parameter '") + c->params[i].name + "'";
            if (reason.empty() && c->numeric && !c->compareKey.empty() && !(c->compareToken = backend.Intern(c->compareKey.c_str())))
                reason = "cannot create a token for the compare key";
            c->childTokens.clear();
            for (size_t i = 0; i < c->childNames.size() && reason.empty(); ++i) {
                int t = backend.Intern(c->childNames[i].c_str());
                if (!t || !backend.EngineHas(CommandKind::Effect, t))
                    reason = "the game has no effect '" + c->childNames[i] + "' to allow inside";
                c->childTokens.push_back(t);
            }
            if (reason.empty() && !backend.Bind(*c, &reason) && reason.empty()) reason = "engine binding failed";
        }
        if (reason.empty()) {
            c->status = CommandStatus::Registered;
            ++registered;
            Log("Native %s '%s' (feature %s): registered (token 0x%x)%s", KindName(c->kind), c->name.c_str(),
                c->feature.c_str(), c->token, c->variable ? ", also a game variable" : "");
        } else {
            c->status = CommandStatus::Refused;
            c->reason = reason;
            Refuse(c->kind, c->name.c_str(), c->feature.c_str(), reason);
        }
    }
    g_finalized = true;
    if (!docsPath.empty()) {
        std::string docs = NativeScriptDocs();
        HANDLE f = CreateFileW(docsPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD w = 0;
        bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, docs.data(), (DWORD)docs.size(), &w, nullptr) && w == docs.size();
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        Log(ok ? "Native script docs written to %ls" : "WARNING: cannot write native script docs to %ls", docsPath.c_str());
    }
    return registered;
}

std::string NativeScriptDocs() {
    std::string s =
        "# hoi4_loader native script commands\n\n"
        "Generated by hoi4_loader at engine registration. Commands exist only while their feature is "
        "active and registered; without the loader the game reports them as unknown. Guard native "
        "triggers with `hoi4_loader_feature_<feature> = yes` in the same AND block (see MODDING.md).\n\n"
        "Parameters marked \"from scope\" may be left out; the value then comes from the current scope. "
        "If the scope does not provide it, an effect does nothing and a trigger is false, so a trigger "
        "inside `NOT = { ... }` is then TRUE (logged once in loader.log).\n";
    for (CommandKind kind : {CommandKind::Effect, CommandKind::Trigger}) {
        s += kind == CommandKind::Effect ? "\n## Effects\n" : "\n## Triggers\n";
        bool any = false;
        for (const Command* c : g_commands) {
            if (c->kind != kind) continue;
            any = true;
            s += "\n### " + c->name + "\n\n";
            s += "- Feature: " + c->feature + "\n";
            switch (c->status) {
                case CommandStatus::Registered: s += "- Status: registered\n"; break;
                case CommandStatus::Refused: s += "- Status: REFUSED: " + c->reason + "\n"; break;
                default: s += "- Status: pending\n"; break;
            }
            s += "- Scopes: " + ScopeNames(c->scopes) + "\n";
            s += "- Usage: " + Usage(*c) + "\n";
            if (c->scopeBlock || c->iterator) s += "- Children run in: " + ScopeNames(c->childScope) + " scope\n";
            if (c->iterator && !c->keysDoc.empty()) s += "- Keys (as the game's state iterator): " + c->keysDoc + "\n";
            if (c->effectBlock) {
                s += "- Effects allowed inside (any number, run in the block's scope; any other key is an error):";
                for (size_t i = 0; i < c->childNames.size(); ++i) s += (i ? ", `" : " `") + c->childNames[i] + "`";
                s += "\n";
            }
            if (!c->texts.empty()) {
                s += "- Tooltip texts (localisation keys a mod may define; English otherwise):\n";
                for (const LocText& t : c->texts) {
                    std::string english;  // colour codes as a mod writes them in its .yml (§R ... §!)
                    for (const char* p = t.english; *p; ++p) english += *p == '\x11' ? std::string("\xc2\xa7") : std::string(1, *p);
                    s += std::string("  - `") + t.key + "`: \"" + english + "\"\n";
                }
            }
            if (!c->params.empty()) {
                s += "- Parameters:\n";
                for (const ParamSpec& p : c->params) {
                    s += std::string("  - `") + p.name + "`: " + TypeName(p.type);
                    if (p.type == ParamType::Choice) s += " (" + ParamUsage(p) + ")";
                    if (p.type == ParamType::Country)
                        s += p.choices ? " (tag, THIS, ROOT, FROM, PREV or any)" : " (tag, THIS, ROOT, FROM or PREV)";
                    if (p.reserved) s += ", reserved: not supported yet (a parse error)";
                    else if (p.fromScope != ScopeDefault::None) s += std::string(", optional: from ") + DefaultName(p.fromScope);
                    else s += p.required ? ", required" : ", optional";
                    s += "\n";
                }
            }
            if (c->numeric) {
                if (c->numericKind == NumericKind::Ratio)
                    s += "- Value: a ratio from 0 to 1 (tooltips show percent). Compare with `<` (below) or `>` "
                         "(above); as in vanilla, `=` also means above. A number or a variable.\n";
                else
                    s += "- Value: a whole number. Compare with `>`, `<` or `=` (equal). A number or a variable.\n";
                s += "- If the value cannot be determined (unknown region, missing scope), the trigger is false "
                     "(TRUE under NOT).\n";
                if (c->variable) s += "- Also a game variable: `" + c->name + "` (e.g. `set_variable = { x = " + c->name + " }`).\n";
            }
            s += "\n" + c->help + "\n";
        }
        if (!any) s += "\n(none)\n";
    }
    if (!g_onActionDocs.empty()) {
        s += "\n## On actions\n";
        for (const OnActionDoc& d : g_onActionDocs) {
            s += "\n### " + d.name + "\n\n";
            s += "- Feature: " + d.feature + " (`hoi4_loader_feature_" + d.feature + " = yes` means supported, not fired)\n";
            s += "- Usage: `on_actions = { " + d.name + " = { effect = { ... } } }`\n";
            s += "\n" + d.help + "\n";
        }
    }
    return s;
}

// ============================================================================================
// Parsing
// ============================================================================================
int NativeParamIndex(const Command& cmd, int keyToken) {
    for (size_t i = 0; i < cmd.paramTokens.size(); ++i)
        if (cmd.paramTokens[i] == keyToken) return (int)i;
    return -1;
}

ParsedCommand* NativeNewParsed(const Command& cmd) {
    auto* pc = new ParsedCommand{};
    pc->cmd = &cmd;
    pc->args.resize(cmd.params.size());
    pc->storage.resize(cmd.params.size());
    return pc;
}

bool NativeSetArg(ParsedCommand& pc, size_t index, const char* text, std::string* error) {
    if (index >= pc.args.size()) {
        *error = "no such parameter";
        return false;
    }
    const ParamSpec& spec = pc.cmd->params[index];
    ArgValue& v = pc.args[index];
    text = text ? text : "";
    if (spec.reserved) {
        *error = std::string("'") + spec.name + "' is not supported by " + pc.cmd->name + " yet";
        return false;
    }
    switch (spec.type) {
        case ParamType::Int: {
            char* end = nullptr;
            errno = 0;
            long n = strtol(text, &end, 10);
            if (!*text || *end || errno == ERANGE || n < INT_MIN || n > INT_MAX) {
                *error = std::string("'") + spec.name + "' expects an integer, got '" + text + "'";
                return false;
            }
            v.i = (int)n;
            break;
        }
        case ParamType::Bool:
            if (strcmp(text, "yes") == 0) v.b = true;
            else if (strcmp(text, "no") == 0) v.b = false;
            else {
                *error = std::string("'") + spec.name + "' expects yes or no, got '" + text + "'";
                return false;
            }
            break;
        case ParamType::Key:
        case ParamType::Token: {
            size_t n = strlen(text);
            bool ok = n > 0 && n <= 255;
            for (size_t i = 0; ok && i < n; ++i) ok = (unsigned char)text[i] > ' ' && text[i] != '"' && text[i] != '{' && text[i] != '}';
            if (!ok) {
                *error = std::string("'") + spec.name + "' expects a key (1-255 characters, no spaces, quotes or braces), got '" +
                         text + "'";
                return false;
            }
            pc.storage[index] = text;
            v.s = pc.storage[index].c_str();
            if (spec.type == ParamType::Token) {
                v.i = g_backend ? g_backend->Intern(text) : 0;
                if (!v.i) {
                    *error = std::string("'") + spec.name + "': cannot create a token for '" + text + "'";
                    return false;
                }
            }
            break;
        }
        case ParamType::Text: {
            size_t n = strlen(text);
            bool ok = n > 0 && n <= 255;
            for (size_t i = 0; ok && i < n; ++i) ok = (unsigned char)text[i] >= ' ' && text[i] != '{' && text[i] != '}';
            if (!ok) {
                *error = std::string("'") + spec.name + "' expects a text (1-255 characters, no braces or control characters), got '" +
                         text + "'";
                return false;
            }
            pc.storage[index] = text;
            v.s = pc.storage[index].c_str();
            break;
        }
        case ParamType::Country: {
            static const struct {
                const char* word;
                int code;
            } kWords[] = {{"THIS", kCountryThis}, {"ROOT", kCountryRoot}, {"FROM", kCountryFrom}, {"PREV", kCountryPrev}};
            int code = 0;
            for (const auto& w : kWords)
                if (_stricmp(text, w.word) == 0) code = w.code;
            if (!code && _stricmp(text, "any") == 0) {
                if (!spec.choices) {
                    *error = std::string("'") + spec.name + "' does not accept any in " + pc.cmd->name +
                             "; give a country (tag, THIS, ROOT, FROM or PREV)";
                    return false;
                }
                code = kCountryAny;
            }
            if (!code && g_backend) code = g_backend->CountryTag(text);
            if (!code) {
                *error = std::string("'") + spec.name + "' expects a country tag or THIS/ROOT/FROM/PREV, got '" + text + "'";
                return false;
            }
            v.i = code;
            pc.storage[index] = text;
            v.s = pc.storage[index].c_str();
            break;
        }
        case ParamType::Choice: {
            int idx = ChoiceIndex(spec.choices, text);
            if (idx < 0) {
                *error = std::string("'") + spec.name + "' expects one of " + ParamUsage(spec) + ", got '" + text + "'";
                return false;
            }
            v.i = idx;
            pc.storage[index] = text;
            v.s = pc.storage[index].c_str();
            break;
        }
    }
    v.present = true;
    return true;
}

bool NativeValidate(ParsedCommand& pc) {
    const Command& c = *pc.cmd;
    if (pc.parseError) return false;
    for (size_t i = 0; i < c.params.size(); ++i) {
        if (c.params[i].required && !pc.args[i].present) {
            pc.parseError = true;
            LogOnce(&const_cast<Command&>(c).invalidLogged,
                    "Native command '%s': missing required parameter '%s'; this use is ignored (first occurrence logged)",
                    c.name.c_str(), c.params[i].name);
            return false;
        }
    }
    if (c.numeric) {
        if (pc.valueForm && !c.ValueFormAllowed()) {
            pc.parseError = true;
            LogOnce(&const_cast<Command&>(c).invalidLogged,
                    "Native command '%s': needs the block form %s; this use is ignored (first occurrence logged)",
                    c.name.c_str(), "name = { ... }");
            return false;
        }
        if (!pc.valueForm && !pc.comparePresent) {
            pc.parseError = true;
            LogOnce(&const_cast<Command&>(c).invalidLogged,
                    "Native command '%s': missing the comparison '%s > ...' in the block; this use is ignored (first "
                    "occurrence logged)",
                    c.name.c_str(), c.compareKey.c_str());
            return false;
        }
    }
    return true;
}

// ============================================================================================
// Runtime
// ============================================================================================
void NativeExecute(const ParsedCommand* pc, const void* engineScope) {
    if (!pc || pc->parseError || !pc->cmd->execute) return;
    Scope scope(engineScope);
    ArgValue resolved[kMaxParams];
    if (!ResolveArgs(*pc, scope, resolved)) return;
    Args args(resolved, pc->args.size());
    DWORD code = 0;
    if (!SafeExecute(pc->cmd->execute, &args, &scope, &code)) LogException(*pc->cmd, "execute", code);
}

void NativeExecuteBlock(const ParsedCommand* pc, const void* engineScope, const EffectChildren& children) {
    if (!pc || pc->parseError || !pc->cmd->blockExecute) return;
    Scope scope(engineScope);
    ArgValue resolved[kMaxParams];
    if (!ResolveArgs(*pc, scope, resolved)) return;
    Args args(resolved, pc->args.size());
    DWORD code = 0;
    if (!SafeExecuteBlock(pc->cmd->blockExecute, &args, &scope, &children, &code)) LogException(*pc->cmd, "execute", code);
}

bool NativeIsChild(const Command& cmd, int token) {
    for (int t : cmd.childTokens)
        if (t && t == token) return true;
    return false;
}

bool NativeEvaluate(const ParsedCommand* pc, const void* engineScope) {
    if (!pc || pc->parseError || !pc->cmd->evaluate) return false;
    Scope scope(engineScope);
    ArgValue resolved[kMaxParams];
    if (!ResolveArgs(*pc, scope, resolved)) return false;
    Args args(resolved, pc->args.size());
    bool result = false;
    DWORD code = 0;
    if (!SafeEvaluate(pc->cmd->evaluate, &args, &scope, &result, &code)) {
        LogException(*pc->cmd, "evaluate", code);
        return false;
    }
    return result;
}

bool NativeNumericValue(const ParsedCommand* pc, const void* engineScope, int64_t* out) {
    *out = 0;
    if (!pc || pc->parseError || !pc->cmd->value) return false;
    const Command& c = *pc->cmd;
    LARGE_INTEGER t0{};
    if (g_timing) QueryPerformanceCounter(&t0);
    Scope scope(engineScope);
    ArgValue resolved[kMaxParams];
    bool ok = false;
    if (ResolveArgs(*pc, scope, resolved)) {
        Args args(resolved, pc->args.size());
        DWORD code = 0;
        if (!SafeValue(c.value, &args, &scope, out, &ok, &code)) {
            LogException(c, "value", code);
            ok = false;
        }
    }
    if (!ok) *out = 0;
    if (g_timing) {
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        NativeTimingAdd(c, t1.QuadPart - t0.QuadPart);
    }
    return ok;
}

bool NativeScopeTarget(const ParsedCommand* pc, const void* engineScope, ScopeTarget* out) {
    *out = ScopeTarget{};
    if (!pc || pc->parseError || !pc->cmd->target) return false;
    const Command& c = *pc->cmd;
    Scope scope(engineScope);
    ArgValue resolved[kMaxParams];
    if (!ResolveArgs(*pc, scope, resolved)) return false;
    Args args(resolved, pc->args.size());
    bool ok = false;
    DWORD code = 0;
    if (!SafeTarget(c.target, &args, &scope, out, &ok, &code)) {
        LogException(c, "target", code);
        return false;
    }
    return ok && out->type == c.childScope && out->id > 0;
}

bool NativeCompare(NumericKind kind, CompareOp op, int64_t value, int64_t rhs) {
    if (op == CompareOp::Less) return value < rhs;
    if (op == CompareOp::Equal && kind == NumericKind::Count) return value == rhs;
    return value > rhs;  // Greater; Ratio '=' behaves like '>' (vanilla)
}

std::string NativeFormatNumber(NumericKind kind, int64_t v) {
    if (kind == NumericKind::Ratio) return FormatFixed(v * 100, 1) + "%";
    return FormatFixed(v, 2);
}

std::string NativeNumericTooltip(const ParsedCommand* pc, const void* engineScope, bool negated, CompareOp op, int64_t rhs,
                                 bool rhsKnown) {
    if (!pc) return "";
    const Command& c = *pc->cmd;
    Scope scope(engineScope);
    TooltipText subject;
    ArgValue resolved[kMaxParams];
    // A scope default this scope lacks (e.g. no region under an iterator's header) stays absent:
    // the subject leaves it out and the line shows no current value.
    bool missing = false;
    bool resolvedOk = !pc->parseError && ResolveArgs(*pc, scope, resolved, true, &missing);
    bool argsOk = resolvedOk && !missing;
    if (resolvedOk && c.subject) {
        Args args(resolved, pc->args.size());
        DWORD code = 0;
        if (!SafeEffectTooltip(c.subject, &args, &scope, &subject, &code)) {
            LogException(c, "tooltip", code);
            subject = TooltipText();
        }
    }
    std::string s = negated ? "NOT: " : "";
    s += subject.text().empty() ? c.name : subject.text();
    bool ratio = c.numericKind == NumericKind::Ratio;
    switch (op) {
        case CompareOp::Less: s += ratio ? " below " : " fewer than "; break;
        case CompareOp::Equal: s += ratio ? " above " : " exactly "; break;
        default: s += ratio ? " above " : " more than "; break;
    }
    s += rhsKnown ? NativeFormatNumber(c.numericKind, ratio ? rhs : rhs * 100000) : std::string("?");
    int64_t current = 0;
    if (missing || subject.scopeMissing())
        ;  // the value depends on the scope the line is evaluated in
    else if (argsOk && NativeNumericValue(pc, engineScope, &current))
        s += " (currently " + NativeFormatNumber(c.numericKind, ratio ? current : current * 100000) + ")";
    else
        s += " (currently unavailable)";
    return s;
}

// --- Translatable tooltip texts ---------------------------------------------------------------
namespace {
// The English fallback: $NAME$ and $NAME|<format>$ replaced by the argument. A colour letter in
// the format (H = highlight, shown yellow as by the game) wraps the value in the game's colour
// codes (0x11 = the parsed form of the localisation's colour sign).
std::string FillPlaceholders(const char* text, const std::vector<LocArg>& args) {
    std::string out;
    for (const char* p = text; *p;) {
        const char* end = *p == '$' ? strchr(p + 1, '$') : nullptr;
        if (!end) {
            out += *p++;
            continue;
        }
        std::string spec(p + 1, end);
        std::string name = spec.substr(0, spec.find('|'));
        std::string format = spec.size() > name.size() ? spec.substr(name.size() + 1) : "";
        const LocArg* arg = nullptr;
        for (const LocArg& a : args)
            if (a.name && name == a.name) arg = &a;
        if (!arg) {
            out.append(p, end + 1);
        } else {
            char colour = 0;
            for (char ch : format)
                if (!colour && ch >= 'A' && ch <= 'Z') colour = ch == 'H' ? 'Y' : ch;
            if (colour) out += std::string("\x11") + colour;
            out += arg->value;
            if (colour) out += "\x11!";
        }
        p = end + 1;
    }
    return out;
}
}  // namespace

std::string native_localize(const LocText& text, const std::vector<LocArg>& args) {
    std::string out;
    if (g_backend && text.key && *text.key && g_backend->Localize(text.key, args, &out)) return out;
    return FillPlaceholders(text.english ? text.english : "", args);
}

std::string native_localize_key(const char* key) {
    std::string out;
    if (g_backend && key && *key && g_backend->Localize(key, {}, &out)) return out;
    return key ? key : "";
}

std::string NativeTooltip(const ParsedCommand* pc, const void* engineScope, bool negated, bool* childrenHidden) {
    if (childrenHidden) *childrenHidden = false;
    if (!pc) return "";
    const Command& c = *pc->cmd;
    Scope scope(engineScope);
    TooltipText out;
    DWORD code = 0;
    ArgValue resolved[kMaxParams];
    bool missing = false;
    bool resolvedOk = !pc->parseError && ResolveArgs(*pc, scope, resolved, true, &missing);
    bool argsOk = resolvedOk && !missing;
    Args args(resolvedOk ? resolved : pc->args.data(), pc->args.size());
    if (c.kind == CommandKind::Effect) {
        if (!c.effectTooltip || !argsOk) return "";
        if (!SafeEffectTooltip(c.effectTooltip, &args, &scope, &out, &code)) {
            LogException(c, "tooltip", code);
            return "";
        }
        if (childrenHidden) *childrenHidden = out.childrenHidden();
        return out.text();
    }
    // Trigger tooltips also run when a scope default is missing (the parameter is then absent).
    if (c.triggerTooltip && resolvedOk) {
        if (SafeTriggerTooltip(c.triggerTooltip, &args, &scope, negated, &out, &code)) return out.text();
        LogException(c, "tooltip", code);
    }
    // Generic line: "<name>: <param>=<value> ..." (NOT prefix when negated).
    std::string s = negated ? "NOT " : "";
    s += c.name + ":";
    for (size_t i = 0; i < c.params.size(); ++i) {
        if (!pc->args[i].present) continue;
        s += std::string(" ") + c.params[i].name + "=";
        switch (c.params[i].type) {
            case ParamType::Int: s += std::to_string(pc->args[i].i); break;
            case ParamType::Bool: s += pc->args[i].b ? "yes" : "no"; break;
            default: s += pc->args[i].s; break;
        }
    }
    return s;
}

void NativeSetTiming(bool on) { g_timing = on; }
bool NativeTimingEnabled() { return g_timing; }
void NativeTimingAdd(const Command& c, long long ticks) {
    InterlockedIncrement64(&const_cast<Command&>(c).evalCount);
    InterlockedAdd64(&const_cast<Command&>(c).evalTicks, ticks);
}

void NativeScriptResetForTests() {
    for (Command* c : g_commands) delete c;
    g_commands.clear();
    g_onActionDocs.clear();
    g_finalized = false;
    g_backend = nullptr;
    g_timing = false;
}
