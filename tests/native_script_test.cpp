// Out-of-game tests for the native script API core (src/native_script.cpp), with a fake engine
// backend. Run by tests/run_smoke.sh. Exit code = number of failures.
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../src/native_script.h"

// --- Stubs for what the core links against -------------------------------------------------
static std::vector<std::string> g_log;
void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_log.push_back(buf);
}
std::wstring LoaderOutputPath(const wchar_t* f) { return f; }

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) printf("  ok   " __VA_ARGS__); else { printf("  FAIL " __VA_ARGS__); ++g_fail; } printf("\n"); } while (0)

static size_t LogCount(const char* needle) {
    size_t n = 0;
    for (const std::string& l : g_log) n += l.find(needle) != std::string::npos;
    return n;
}

class FakeBackend : public NativeBackend {
public:
    std::map<std::string, int> tokens;
    std::set<int> vanillaEffects, vanillaTriggers;
    std::set<std::string> failBind;
    std::vector<std::string> bound;
    FakeBackend() {
        vanillaEffects.insert(Intern("set_province_name"));
        vanillaTriggers.insert(Intern("has_global_flag"));
    }
    int Intern(const char* s) override {
        auto it = tokens.find(s);
        if (it != tokens.end()) return it->second;
        int t = 0x5000 + (int)tokens.size();
        tokens[s] = t;
        return t;
    }
    bool EngineHas(CommandKind k, int t) override {
        return k == CommandKind::Effect ? vanillaEffects.count(t) > 0 : vanillaTriggers.count(t) > 0;
    }
    bool Bind(Command& c, std::string* reason) override {
        if (failBind.count(c.name)) {
            *reason = "fake bind failure";
            return false;
        }
        bound.push_back(c.name);
        return true;
    }
};

// --- Callbacks ---------------------------------------------------------------------------------
static int g_lastRegion = 0;
static std::string g_lastKey;
static bool g_lastFlag = false;
static void Exec(const Args& a, const Scope&) {
    g_lastRegion = a.i(0);
    g_lastKey = a.key(1);
    g_lastFlag = a.b(2);
}
static void ExecCrash(const Args&, const Scope&) {
    volatile int* p = nullptr;
    *p = 1;
}
static bool EvalRegionIs42(const Args& a, const Scope&) { return a.i(0) == 42; }
static bool EvalCrash(const Args&, const Scope&) {
    volatile int* p = nullptr;
    return *p == 1;
}
static void TipCustom(const Args& a, const Scope&, bool neg, TooltipText& out) {
    out.add(neg ? "not region " : "region ").add(a.i(0));
}

static NativeEffect Effect(const char* name, std::vector<ParamSpec> params, Form form = Form::Block) {
    return NativeEffect{name, "Help text.", form, params, ScopeMask::Any, &Exec, nullptr};
}

int main() {
    printf("native_script core tests\n");
    const std::vector<ParamSpec> kRegionName = {{"region", ParamType::Int, true}, {"name", ParamType::Key, true},
                                                {"flag", ParamType::Bool, false}};

    // 1. Schema validation.
    CHECK(!register_native_effect(Effect("Bad-Name", kRegionName), "f"), "invalid command name refused");
    CHECK(!register_native_effect(NativeEffect{"no_help", "", Form::Block, kRegionName, 0, &Exec, nullptr}, "f"), "missing help refused");
    CHECK(!register_native_effect(NativeEffect{"no_exec", "h", Form::Block, kRegionName, 0, nullptr, nullptr}, "f"), "missing execute refused");
    CHECK(!register_native_trigger(NativeTrigger{"no_eval", "h", Form::Block, kRegionName, 0, nullptr, nullptr}, "f"), "missing evaluate refused");
    CHECK(!register_native_effect(Effect("value_two", {{"a", ParamType::Int, true}, {"b", ParamType::Int, true}}, Form::Value), "f"),
          "value form with two parameters refused");
    CHECK(!register_native_effect(Effect("value_optional", {{"a", ParamType::Int, false}}, Form::Value), "f"),
          "value form with an optional parameter refused");
    CHECK(!register_native_effect(Effect("block_empty", {}), "f"), "block form without parameters refused");
    CHECK(!register_native_effect(Effect("dup_param", {{"a", ParamType::Int, true}, {"a", ParamType::Key, true}}), "f"),
          "duplicate parameter refused");
    CHECK(!register_native_effect(Effect("bad_param", {{"Region", ParamType::Int, true}}), "f"), "invalid parameter name refused");

    // 2. Valid registrations, loader-side duplicates.
    CHECK(register_native_effect(Effect("set_thing", kRegionName), "feat_a"), "valid block effect queued");
    CHECK(register_native_effect(Effect("value_thing", {{"amount", ParamType::Int, true}}, Form::Value), "feat_a"),
          "valid value effect queued");
    CHECK(!register_native_effect(Effect("set_thing", kRegionName), "feat_b"), "second effect with the same name refused");
    CHECK(register_native_trigger(NativeTrigger{"set_thing", "Trigger with an effect's name.", Form::Block,
                                                {{"region", ParamType::Int, true}}, ScopeMask::State | ScopeMask::Country,
                                                &EvalRegionIs42, nullptr},
                                  "feat_a"),
          "trigger may share a name with an effect");
    CHECK(register_native_trigger(NativeTrigger{"crashy", "Crashes.", Form::Block, {{"region", ParamType::Int, true}}, 0,
                                                &EvalCrash, &TipCustom},
                                  "feat_a"),
          "second trigger queued");
    CHECK(register_native_effect(Effect("set_province_name", {{"id", ParamType::Int, true}}), "feat_b"),
          "vanilla-named effect queued (refused at engine registration)");
    CHECK(register_native_effect(Effect("bind_fails", {{"x", ParamType::Int, true}}), "feat_c"), "effect queued (bind will fail)");
    CHECK(!NativeFeatureComplete("feat_a"), "feature incomplete before engine registration");
    CHECK(NativeFeatureComplete("no_commands"), "feature without native commands counts as complete");

    // 3. Engine registration.
    FakeBackend be;
    be.failBind.insert("bind_fails");
    document_on_action("on_test_loaded", "feat_d", "Fires in tests.");
    int n = NativeScriptFinalize(be, L"");
    CHECK(n == 4, "4 commands registered (got %d)", n);
    CHECK(NativeScriptFinalized(), "window closed after finalize");
    CHECK(NativeFeatureComplete("feat_a"), "feat_a complete (all registered)");
    CHECK(!NativeFeatureComplete("feat_b"), "feat_b incomplete (vanilla name refused)");
    CHECK(!NativeFeatureComplete("feat_c"), "feat_c incomplete (bind failed)");
    CHECK(LogCount("the game already has an effect with this name") == 1, "vanilla conflict logged");
    CHECK(!register_native_effect(Effect("too_late", kRegionName), "feat_a"), "registration after the window refused");
    CHECK(LogCount("registration window is closed") == 1, "late registration logged");

    const Command* setThing = nullptr;
    const Command* valueThing = nullptr;
    const Command* trig = nullptr;
    const Command* crashy = nullptr;
    for (const Command* c : NativeCommands()) {
        if (c->name == "set_thing" && c->kind == CommandKind::Effect) setThing = c;
        if (c->name == "value_thing") valueThing = c;
        if (c->name == "set_thing" && c->kind == CommandKind::Trigger) trig = c;
        if (c->name == "crashy") crashy = c;
    }
    CHECK(setThing && valueThing && trig && crashy, "commands found");
    if (!setThing || !valueThing || !trig || !crashy) return g_fail;
    CHECK(setThing->paramTokens.size() == 3 && NativeParamIndex(*setThing, be.Intern("name")) == 1 &&
              NativeParamIndex(*setThing, be.Intern("nope")) == -1,
          "parameter tokens map to schema indices");

    // 4. Argument parsing.
    std::string err;
    ParsedCommand* pc = NativeNewParsed(*setThing);
    CHECK(NativeSetArg(*pc, 0, "42", &err) && pc->args[0].i == 42, "int parsed");
    CHECK(NativeSetArg(*pc, 0, "-7", &err) && pc->args[0].i == -7, "negative int parsed");
    CHECK(!NativeSetArg(*pc, 0, "4x", &err) && err.find("expects an integer") != std::string::npos, "non-integer refused: %s", err.c_str());
    CHECK(!NativeSetArg(*pc, 0, "", &err), "empty int refused");
    CHECK(!NativeSetArg(*pc, 0, "99999999999", &err), "out-of-range int refused");
    CHECK(NativeSetArg(*pc, 2, "yes", &err) && pc->args[2].b, "bool yes parsed");
    CHECK(NativeSetArg(*pc, 2, "no", &err) && !pc->args[2].b, "bool no parsed");
    CHECK(!NativeSetArg(*pc, 2, "true", &err) && err.find("yes or no") != std::string::npos, "bool 'true' refused");
    CHECK(NativeSetArg(*pc, 1, "MY_LOC-KEY.1", &err) && std::string(pc->args[1].s) == "MY_LOC-KEY.1", "key parsed");
    CHECK(!NativeSetArg(*pc, 1, "has space", &err), "key with space refused");
    CHECK(!NativeSetArg(*pc, 1, std::string(256, 'K').c_str(), &err), "256-character key refused");
    CHECK(NativeSetArg(*pc, 1, std::string(255, 'K').c_str(), &err), "255-character key accepted");

    // 5. Validation (required parameters), logged once per command.
    ParsedCommand* missing = NativeNewParsed(*setThing);
    NativeSetArg(*missing, 0, "42", &err);
    size_t before = LogCount("missing required parameter");
    CHECK(!NativeValidate(*missing), "missing required parameter detected");
    ParsedCommand* missing2 = NativeNewParsed(*setThing);
    CHECK(!NativeValidate(*missing2), "second incomplete use detected");
    CHECK(LogCount("missing required parameter") == before + 1, "missing parameter logged once per command");

    // 6. Execute / evaluate.
    ParsedCommand* ok = NativeNewParsed(*setThing);
    NativeSetArg(*ok, 0, "42", &err);
    NativeSetArg(*ok, 1, "KEY_A", &err);
    CHECK(NativeValidate(*ok), "complete use validates");
    NativeExecute(ok, nullptr);
    CHECK(g_lastRegion == 42 && g_lastKey == "KEY_A" && !g_lastFlag, "execute got typed arguments");
    g_lastRegion = 0;
    NativeExecute(missing, nullptr);
    CHECK(g_lastRegion == 0, "invalid use does not execute");

    ParsedCommand* t42 = NativeNewParsed(*trig);
    NativeSetArg(*t42, 0, "42", &err);
    ParsedCommand* t7 = NativeNewParsed(*trig);
    NativeSetArg(*t7, 0, "7", &err);
    CHECK(NativeEvaluate(t42, nullptr) && !NativeEvaluate(t7, nullptr), "evaluate returns callback result");
    CHECK(!NativeEvaluate(missing2, nullptr), "invalid trigger use evaluates false");

    ParsedCommand* c1 = NativeNewParsed(*crashy);
    NativeSetArg(*c1, 0, "1", &err);
    size_t crashLogs = LogCount("crashed (exception");
    CHECK(!NativeEvaluate(c1, nullptr), "crashing trigger returns false");
    CHECK(!NativeEvaluate(c1, nullptr), "crashing trigger returns false again");
    CHECK(LogCount("crashed (exception") == crashLogs + 1, "crash logged once per command");

    // A crashing effect: own command so its once-flag is fresh.
    Command crashEffect = *setThing;
    crashEffect.execute = &ExecCrash;
    crashEffect.failureLogged = 0;
    ParsedCommand* ce = NativeNewParsed(crashEffect);
    NativeSetArg(*ce, 0, "1", &err);
    NativeSetArg(*ce, 1, "K", &err);
    NativeExecute(ce, nullptr);
    CHECK(LogCount("it did nothing") == 1, "crashing effect contained and logged");

    // 7. Tooltips.
    CHECK(NativeTooltip(t42, nullptr, false) == "set_thing: region=42", "generic trigger tooltip: '%s'",
          NativeTooltip(t42, nullptr, false).c_str());
    CHECK(NativeTooltip(t42, nullptr, true) == "NOT set_thing: region=42", "generic negated tooltip");
    CHECK(NativeTooltip(c1, nullptr, true) == "not region 1", "custom trigger tooltip");
    CHECK(NativeTooltip(ok, nullptr, false).empty(), "effect without tooltip callback: no line");

    // 8. Docs file content.
    std::string docs = NativeScriptDocs();
    const char* expected[] = {
        "## Effects\n",
        "### set_thing\n\n- Feature: feat_a\n- Status: registered\n- Scopes: any\n"
        "- Usage: `set_thing = { region = <int> name = <key> flag = <bool> }`\n- Parameters:\n"
        "  - `region`: int, required\n  - `name`: key, required\n  - `flag`: bool, optional\n\nHelp text.\n",
        "- Usage: `value_thing = <int>`\n",
        "### set_province_name\n\n- Feature: feat_b\n- Status: REFUSED: the game already has an effect with this name\n",
        "### bind_fails\n\n- Feature: feat_c\n- Status: REFUSED: fake bind failure\n",
        "## Triggers\n",
        "### set_thing\n\n- Feature: feat_a\n- Status: registered\n- Scopes: State, Country\n",
    };
    for (const char* e : expected)
        CHECK(docs.find(e) != std::string::npos, "docs contain: %.60s", e);
    CHECK(docs.find("too_late") == std::string::npos && docs.find("Bad-Name") == std::string::npos,
          "refused-at-registration commands are not documented as commands");
    CHECK(docs.find("\n## On actions\n\n### on_test_loaded\n\n"
                    "- Feature: feat_d (`hoi4_loader_feature_feat_d = yes` means supported, not fired)\n"
                    "- Usage: `on_actions = { on_test_loaded = { effect = { ... } } }`\n\nFires in tests.\n") != std::string::npos &&
              docs.find("## On actions") > docs.find("## Triggers"),
          "docs: on_actions section after the triggers");
    CHECK(NativeFeatureComplete("feat_d"), "documenting an on_action adds no native command to its feature");

    printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail;
}
