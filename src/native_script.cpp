// Native script API, engine-free core: schema validation, the registration window, conflict
// checks, typed argument parsing, SEH-guarded callbacks, tooltips and the generated docs.
// The engine side lives in native_script_engine.cpp; the smoke test links this file with a fake
// NativeBackend.
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
        default: return "key";
    }
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
    for (size_t i = 0; i < params.size(); ++i) {
        if (!ValidIdentifier(params[i].name)) return "parameter names must be 1-64 characters of a-z 0-9 _";
        for (size_t j = 0; j < i; ++j)
            if (strcmp(params[i].name, params[j].name) == 0) return std::string("duplicate parameter '") + params[i].name + "'";
    }
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
    for (const ParamSpec& p : params) c->paramNames.push_back(p.name);
    for (size_t i = 0; i < params.size(); ++i) c->params.push_back({c->paramNames[i].c_str(), params[i].type, params[i].required});
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

std::string Usage(const Command& c) {
    std::string s = "`" + c.name + " = ";
    if (c.form == Form::Value) return s + "<" + TypeName(c.params[0].type) + ">`";
    s += "{";
    for (const ParamSpec& p : c.params) s += std::string(" ") + p.name + " = <" + TypeName(p.type) + ">";
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

bool SafeEvaluate(bool (*fn)(const Args&, const Scope&), const Args* a, const Scope* s, bool* result, DWORD* code) {
    __try {
        *result = fn(*a, *s);
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

}  // namespace

// ============================================================================================
// Registration
// ============================================================================================
bool register_native_effect(const NativeEffect& d, const char* feature) {
    std::string why = CheckSchema(CommandKind::Effect, d.name, d.help, d.form, d.params, d.execute != nullptr);
    if (!why.empty()) return Refuse(CommandKind::Effect, d.name, feature, why);
    Command* c = NewCommand(CommandKind::Effect, feature, d.name, d.help, d.form, d.params, d.scopes);
    c->execute = d.execute;
    c->effectTooltip = d.tooltip;
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
            if (reason.empty() && !backend.Bind(*c, &reason) && reason.empty()) reason = "engine binding failed";
        }
        if (reason.empty()) {
            c->status = CommandStatus::Registered;
            ++registered;
            Log("Native %s '%s' (feature %s): registered (token 0x%x)", KindName(c->kind), c->name.c_str(), c->feature.c_str(),
                c->token);
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
        "triggers with `hoi4_loader_feature_<feature> = yes` in the same AND block (see README).\n";
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
            s += "- Parameters:\n";
            for (const ParamSpec& p : c->params)
                s += std::string("  - `") + p.name + "`: " + TypeName(p.type) + (p.required ? ", required" : ", optional") + "\n";
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
        case ParamType::Key: {
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
    return true;
}

// ============================================================================================
// Runtime
// ============================================================================================
void NativeExecute(const ParsedCommand* pc, const void* engineScope) {
    if (!pc || pc->parseError || !pc->cmd->execute) return;
    Args args(pc->args.data(), pc->args.size());
    Scope scope(engineScope);
    DWORD code = 0;
    if (!SafeExecute(pc->cmd->execute, &args, &scope, &code)) LogException(*pc->cmd, "execute", code);
}

bool NativeEvaluate(const ParsedCommand* pc, const void* engineScope) {
    if (!pc || pc->parseError || !pc->cmd->evaluate) return false;
    Args args(pc->args.data(), pc->args.size());
    Scope scope(engineScope);
    bool result = false;
    DWORD code = 0;
    if (!SafeEvaluate(pc->cmd->evaluate, &args, &scope, &result, &code)) {
        LogException(*pc->cmd, "evaluate", code);
        return false;
    }
    return result;
}

std::string NativeTooltip(const ParsedCommand* pc, const void* engineScope, bool negated) {
    if (!pc) return "";
    const Command& c = *pc->cmd;
    Args args(pc->args.data(), pc->args.size());
    Scope scope(engineScope);
    TooltipText out;
    DWORD code = 0;
    if (c.kind == CommandKind::Effect) {
        if (!c.effectTooltip) return "";
        if (!SafeEffectTooltip(c.effectTooltip, &args, &scope, &out, &code)) {
            LogException(c, "tooltip", code);
            return "";
        }
        return out.text();
    }
    if (c.triggerTooltip && !pc->parseError) {
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

void NativeScriptResetForTests() {
    for (Command* c : g_commands) delete c;
    g_commands.clear();
    g_onActionDocs.clear();
    g_finalized = false;
}
