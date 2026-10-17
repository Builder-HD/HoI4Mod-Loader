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

// Fake Scope: the core resolves scope defaults through these; tests pass FakeScope* as the scope.
struct FakeScope {
    int state = 0, country = 0, region = 0;
    const FakeScope* root = nullptr;  // null = ROOT is the scope itself
    const FakeScope* from = nullptr;
};
uint32_t Scope::types() const { return 0; }
int Scope::stateId() const { return p_ ? ((const FakeScope*)p_)->state : 0; }
int Scope::countryTag() const { return p_ ? ((const FakeScope*)p_)->country : 0; }
int Scope::strategicRegionId() const { return p_ ? ((const FakeScope*)p_)->region : 0; }
Scope Scope::link(size_t) const { return Scope(nullptr); }
Scope Scope::root() const { return Scope(p_ ? ((const FakeScope*)p_)->root : nullptr); }
Scope Scope::from() const { return Scope(p_ ? ((const FakeScope*)p_)->from : nullptr); }
Scope Scope::prev() const { return Scope(nullptr); }

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
    int CountryTag(const char* text) override { return strcmp(text, "GER") == 0 ? 7 : 0; }
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

static int g_echo[3] = {};
static bool EvalEcho(const Args& a, const Scope&) {
    g_echo[0] = a.i(0);
    g_echo[1] = a.i(1);
    g_echo[2] = a.has(2) ? a.i(2) : -1;
    return true;
}
static bool ValueRatio(const Args& a, const Scope&, int64_t* out) {
    *out = (int64_t)a.i(0) * 1000;  // region 42 -> 42%
    return a.i(0) != 99;            // region 99: value does not exist
}
static bool ValueCount(const Args& a, const Scope&, int64_t* out) {
    *out = a.i(0) % 10;
    return true;
}
static int g_ownerSeen = 0;
static bool ValueOwner(const Args& a, const Scope&, int64_t* out) {
    g_ownerSeen = a.has(0) ? a.i(0) : 0;
    *out = 1;
    return true;
}
static void SubjectRegion(const Args& a, const Scope&, TooltipText& out) {
    if (a.has(0)) out.add("Region ").add(a.i(0));
    else out.add("Some region");
}
static bool TargetRegion(const Args& a, const Scope& s, ScopeTarget* out) {
    out->type = ScopeMask::StrategicRegion;
    out->id = a.has(0) ? a.i(0) : s.strategicRegionId();
    return out->id > 0;
}
static bool TargetCrash(const Args&, const Scope&, ScopeTarget*) {
    volatile int* p = nullptr;
    return *p == 1;
}
static bool TargetWrongType(const Args&, const Scope&, ScopeTarget* out) {
    out->type = ScopeMask::State;
    out->id = 3;
    return true;
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
    // Scope defaults, country/choice parameters, numeric triggers (checked in 9).
    const char* kOwners = "any|own|enemy";
    CHECK(register_native_trigger(NativeTrigger{"scope_echo", "Echo.", Form::Block,
                                                {{"region", ParamType::Int, false, ScopeDefault::StrategicRegion},
                                                 {"who", ParamType::Country, false, ScopeDefault::RootCountry},
                                                 {"owner", ParamType::Choice, false, ScopeDefault::None, kOwners}},
                                                0, &EvalEcho, nullptr},
                                  "feat_e"),
          "trigger with scope defaults and a choice queued");
    CHECK(register_native_numeric_trigger(NativeNumericTrigger{"num_ratio", "Ratio.", NumericKind::Ratio,
                                                               {{"region", ParamType::Int, false, ScopeDefault::StrategicRegion}},
                                                               ScopeMask::StrategicRegion, "value", &ValueRatio, &SubjectRegion},
                                          "feat_e"),
          "numeric ratio trigger queued");
    CHECK(register_native_numeric_trigger(NativeNumericTrigger{"num_count", "Count.", NumericKind::Count,
                                                               {{"state", ParamType::Int, true}}, ScopeMask::Country, "count",
                                                               &ValueCount, nullptr},
                                          "feat_e"),
          "numeric count trigger queued");
    CHECK(register_native_numeric_trigger(
              NativeNumericTrigger{"num_owner", "Owner.", NumericKind::Count,
                                   {{"owner", ParamType::Country, false, ScopeDefault::None, "any"},
                                    {"foe", ParamType::Country, false, ScopeDefault::None, nullptr, true}},
                                   ScopeMask::Any, "count", &ValueOwner, nullptr},
              "feat_e"),
          "numeric trigger with an any-country and a reserved parameter queued");
    CHECK(!register_native_numeric_trigger(NativeNumericTrigger{"num_badany", "h", NumericKind::Count,
                                                                {{"who", ParamType::Country, false, ScopeDefault::None, "all"}},
                                                                0, "count", &ValueCount, nullptr},
                                           "feat_e"),
          "country parameter with a choice other than any refused");
    CHECK(!register_native_numeric_trigger(NativeNumericTrigger{"num_clash", "h", NumericKind::Count,
                                                                {{"value", ParamType::Int, true}}, 0, "value", &ValueCount, nullptr},
                                           "feat_e"),
          "compare key equal to a parameter refused");
    CHECK(!register_native_trigger(NativeTrigger{"req_scope", "h", Form::Block,
                                                 {{"region", ParamType::Int, true, ScopeDefault::StrategicRegion}}, 0, &EvalEcho, nullptr},
                                   "feat_e"),
          "required parameter with a scope default refused");
    CHECK(!register_native_trigger(NativeTrigger{"bad_default", "h", Form::Block,
                                                 {{"who", ParamType::Int, false, ScopeDefault::RootCountry}}, 0, &EvalEcho, nullptr},
                                   "feat_e"),
          "scope default of the wrong type refused");
    CHECK(!register_native_trigger(NativeTrigger{"no_choices", "h", Form::Block, {{"owner", ParamType::Choice, true}}, 0,
                                                 &EvalEcho, nullptr},
                                   "feat_e"),
          "choice without choices refused");
    // Scope blocks and iterators (checked in 10).
    const std::vector<ParamSpec> kRegionOpt = {{"region", ParamType::Int, false}};
    CHECK(register_native_scope_block(NativeScopeBlock{"in_region", "Block.", CommandKind::Trigger, kRegionOpt, ScopeMask::Any,
                                                       ScopeMask::StrategicRegion, &TargetRegion},
                                      "feat_s"),
          "scope block trigger queued");
    CHECK(register_native_scope_block(NativeScopeBlock{"in_region", "Block.", CommandKind::Effect, kRegionOpt, ScopeMask::Any,
                                                       ScopeMask::StrategicRegion, &TargetRegion},
                                      "feat_s"),
          "scope block effect with the trigger's name queued");
    CHECK(!register_native_scope_block(NativeScopeBlock{"in_region", "Block.", CommandKind::Trigger, kRegionOpt, 0,
                                                        ScopeMask::StrategicRegion, &TargetRegion},
                                       "feat_s"),
          "duplicate scope block refused");
    CHECK(!register_native_scope_block(NativeScopeBlock{"in_state", "Block.", CommandKind::Trigger, kRegionOpt, 0,
                                                        ScopeMask::State, &TargetRegion},
                                       "feat_s"),
          "scope block for another scope type refused");
    CHECK(!register_native_scope_block(NativeScopeBlock{"no_target", "Block.", CommandKind::Effect, kRegionOpt, 0,
                                                        ScopeMask::StrategicRegion, nullptr},
                                       "feat_s"),
          "scope block without target callback refused");
    CHECK(register_native_scope_block(NativeScopeBlock{"crash_region", "Block.", CommandKind::Trigger, {}, 0,
                                                       ScopeMask::StrategicRegion, &TargetCrash},
                                      "feat_s"),
          "scope block without parameters queued");
    CHECK(register_native_scope_block(NativeScopeBlock{"wrong_region", "Block.", CommandKind::Trigger, {}, 0,
                                                       ScopeMask::StrategicRegion, &TargetWrongType},
                                      "feat_s"),
          "scope block (target of the wrong type at runtime) queued");
    CHECK(register_native_iterator(NativeIterator{"any_region", "Any.", IteratorKind::Any, ScopeMask::StrategicRegion, "KEY",
                                                  "Any region"},
                                   "feat_s"),
          "any iterator queued");
    CHECK(register_native_iterator(NativeIterator{"every_region", "Every.", IteratorKind::Every, ScopeMask::StrategicRegion,
                                                  nullptr, nullptr},
                                   "feat_s"),
          "every iterator queued");
    CHECK(register_native_iterator(NativeIterator{"random_region", "Random.", IteratorKind::Random, ScopeMask::StrategicRegion,
                                                  nullptr, nullptr},
                                   "feat_s"),
          "random iterator queued");
    CHECK(!register_native_iterator(NativeIterator{"any_nohead", "Any.", IteratorKind::Any, ScopeMask::StrategicRegion,
                                                   nullptr, nullptr},
                                    "feat_s"),
          "any iterator without header text refused");
    CHECK(!register_native_iterator(NativeIterator{"every_state2", "Every.", IteratorKind::Every, ScopeMask::State, nullptr,
                                                   nullptr},
                                    "feat_s"),
          "iterator over another scope type refused");
    CHECK(!register_native_iterator(NativeIterator{"any_region", "Any.", IteratorKind::Any, ScopeMask::StrategicRegion, "K",
                                                   "Any"},
                                    "feat_s"),
          "duplicate iterator refused");
    NativeEffect texted = Effect("texted_effect", kRegionName);
    texted.texts = {{"TEXTED_KEY", "$OLD|Y$ becomes $NEW$."}};
    CHECK(register_native_effect(texted, "feat_s"), "effect with a tooltip text queued");
    NativeEffect untexted = Effect("untexted_effect", kRegionName);
    untexted.texts = {{"NO_ENGLISH", ""}};
    CHECK(!register_native_effect(untexted, "feat_s"), "tooltip text without English refused");
    CHECK(!NativeFeatureComplete("feat_a"), "feature incomplete before engine registration");
    CHECK(NativeFeatureComplete("no_commands"), "feature without native commands counts as complete");

    // 3. Engine registration.
    FakeBackend be;
    be.failBind.insert("bind_fails");
    document_on_action("on_test_loaded", "feat_d", "Fires in tests.");
    int n = NativeScriptFinalize(be, L"");
    CHECK(n == 16, "16 commands registered (got %d)", n);
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

    // 9. Scope defaults, country/choice parameters, numeric triggers.
    const Command* echo = nullptr;
    const Command* ratio = nullptr;
    const Command* count = nullptr;
    for (const Command* c : NativeCommands()) {
        if (c->name == "scope_echo") echo = c;
        if (c->name == "num_ratio") ratio = c;
        if (c->name == "num_count") count = c;
    }
    CHECK(echo && ratio && count, "scope/numeric commands found");
    if (!echo || !ratio || !count) return g_fail;
    CHECK(ratio->compareToken == be.Intern("value") && count->compareToken == be.Intern("count"), "compare keys interned");

    ParsedCommand* e1 = NativeNewParsed(*echo);
    CHECK(NativeSetArg(*e1, 2, "own", &err) && e1->args[2].i == 1, "choice parsed to its index");
    CHECK(!NativeSetArg(*e1, 2, "foo", &err) && err.find("one of any/own/enemy") != std::string::npos, "unknown choice refused: %s",
          err.c_str());
    CHECK(NativeSetArg(*e1, 1, "ROOT", &err) && e1->args[1].i == kCountryRoot, "ROOT parsed as a keyword");
    CHECK(NativeSetArg(*e1, 1, "GER", &err) && e1->args[1].i == 7, "country tag resolved by the backend");
    CHECK(!NativeSetArg(*e1, 1, "XXX", &err) && err.find("country tag") != std::string::npos, "unknown tag refused");

    FakeScope rootCountry;
    rootCountry.country = 5;
    FakeScope fromCountry;
    fromCountry.country = 9;
    FakeScope regionScope;
    regionScope.region = 42;
    regionScope.root = &rootCountry;
    regionScope.from = &fromCountry;
    ParsedCommand* e2 = NativeNewParsed(*echo);  // everything from scope
    CHECK(NativeValidate(*e2), "use without the scope-default parameters validates");
    CHECK(NativeEvaluate(e2, &regionScope) && g_echo[0] == 42 && g_echo[1] == 5 && g_echo[2] == -1,
          "region from the region scope, country from ROOT, optional choice absent (%d %d %d)", g_echo[0], g_echo[1], g_echo[2]);
    ParsedCommand* e3 = NativeNewParsed(*echo);
    NativeSetArg(*e3, 0, "7", &err);
    NativeSetArg(*e3, 1, "FROM", &err);
    CHECK(NativeEvaluate(e3, &regionScope) && g_echo[0] == 7 && g_echo[1] == 9, "explicit region, FROM resolved per use");
    FakeScope noRegion;
    noRegion.country = 3;
    size_t scopeLogs = LogCount("not available from the scope");
    g_echo[0] = -5;
    CHECK(!NativeEvaluate(e2, &noRegion) && g_echo[0] == -5, "missing scope default: false, callback not called");
    CHECK(!NativeEvaluate(e2, &noRegion), "missing scope default: false again");
    CHECK(LogCount("not available from the scope") == scopeLogs + 1, "missing scope default logged once");
    ParsedCommand* e4 = NativeNewParsed(*echo);
    NativeSetArg(*e4, 0, "7", &err);
    CHECK(NativeEvaluate(e4, &noRegion) && g_echo[1] == 3, "RootCountry falls back to the country scope when ROOT is the scope");

    CHECK(NativeCompare(NumericKind::Ratio, CompareOp::Greater, 60000, 50000) &&
              !NativeCompare(NumericKind::Ratio, CompareOp::Greater, 50000, 50000),
          "ratio >: strictly above");
    CHECK(NativeCompare(NumericKind::Ratio, CompareOp::Equal, 60000, 50000) &&
              !NativeCompare(NumericKind::Ratio, CompareOp::Equal, 50000, 50000),
          "ratio =: behaves like > (vanilla)");
    CHECK(NativeCompare(NumericKind::Count, CompareOp::Equal, 3, 3) && !NativeCompare(NumericKind::Count, CompareOp::Equal, 4, 3),
          "count =: equality");
    CHECK(NativeCompare(NumericKind::Count, CompareOp::Less, 2, 3) && !NativeCompare(NumericKind::Count, CompareOp::Less, 3, 3),
          "count <: strictly below");
    CHECK(NativeFormatNumber(NumericKind::Ratio, 64500) == "64.5%" && NativeFormatNumber(NumericKind::Ratio, 60000) == "60%",
          "ratio formatted as percent");
    CHECK(NativeFormatNumber(NumericKind::Count, 250000) == "2.5" && NativeFormatNumber(NumericKind::Count, 300000) == "3",
          "count formatted");

    ParsedCommand* r1 = NativeNewParsed(*ratio);
    r1->valueForm = true;
    int64_t v = 0;
    CHECK(NativeValidate(*r1), "ratio value form validates (region from scope)");
    CHECK(NativeNumericValue(r1, &regionScope, &v) && v == 42000, "ratio value from the scope region (%lld)", (long long)v);
    ParsedCommand* r2 = NativeNewParsed(*ratio);
    NativeSetArg(*r2, 0, "99", &err);
    CHECK(!NativeNumericValue(r2, &regionScope, &v) && v == 0, "value the callback reports as missing: false");
    CHECK(!NativeValidate(*r2), "block form without the comparison is invalid");
    CHECK(LogCount("missing the comparison 'value > ...'") == 1, "missing comparison logged");
    std::string tip = NativeNumericTooltip(r1, &regionScope, false, CompareOp::Greater, 60000, true);
    CHECK(tip == "Region 42 above 60% (currently 42%)", "ratio tooltip: '%s'", tip.c_str());
    tip = NativeNumericTooltip(r1, &regionScope, true, CompareOp::Less, 0, false);
    CHECK(tip == "NOT: Region 42 below ? (currently 42%)", "negated tooltip, unknown right side: '%s'", tip.c_str());
    tip = NativeNumericTooltip(r1, &noRegion, false, CompareOp::Greater, 60000, true);
    CHECK(tip == "Some region above 60%", "tooltip without the scope value: subject without it, no current value: '%s'",
          tip.c_str());
    ParsedCommand* r3 = NativeNewParsed(*ratio);
    NativeSetArg(*r3, 0, "99", &err);
    r3->comparePresent = true;
    tip = NativeNumericTooltip(r3, &noRegion, false, CompareOp::Greater, 60000, true);
    CHECK(tip == "Region 99 above 60% (currently unavailable)", "tooltip of a value that does not exist: '%s'", tip.c_str());

    ParsedCommand* c2 = NativeNewParsed(*count);
    c2->valueForm = true;
    CHECK(!NativeValidate(*c2), "value form refused when a parameter is required");
    ParsedCommand* c3 = NativeNewParsed(*count);
    NativeSetArg(*c3, 0, "15", &err);
    c3->comparePresent = true;
    CHECK(NativeValidate(*c3), "count block form with comparison validates");
    tip = NativeNumericTooltip(c3, &noRegion, false, CompareOp::Equal, 5, true);
    CHECK(tip == "num_count exactly 5 (currently 5)", "count tooltip: '%s'", tip.c_str());

    NativeSetTiming(true);
    long long evals = count->evalCount;
    NativeNumericValue(c3, &noRegion, &v);
    CHECK(count->evalCount == evals + 1 && v == 5, "timing counts evaluations when enabled");
    NativeSetTiming(false);

    const Command* owner = nullptr;
    for (const Command* c : NativeCommands())
        if (c->name == "num_owner") owner = c;
    CHECK(owner != nullptr, "num_owner found");
    if (!owner) return g_fail;
    ParsedCommand* o1 = NativeNewParsed(*owner);
    CHECK(NativeSetArg(*o1, 0, "any", &err) && o1->args[0].i == kCountryAny, "any parsed where allowed");
    o1->comparePresent = true;
    CHECK(NativeValidate(*o1) && NativeNumericValue(o1, &noRegion, &v) && g_ownerSeen == kCountryAny,
          "any reaches the callback unresolved (%d)", g_ownerSeen);
    ParsedCommand* o2 = NativeNewParsed(*owner);
    CHECK(NativeSetArg(*o2, 0, "ROOT", &err), "ROOT accepted next to any");
    o2->comparePresent = true;
    CHECK(NativeNumericValue(o2, &regionScope, &v) && g_ownerSeen == 5, "ROOT resolved per use (%d)", g_ownerSeen);
    CHECK(!NativeSetArg(*o2, 1, "GER", &err) && err == "'foe' is not supported by num_owner yet", "reserved parameter: %s",
          err.c_str());
    ParsedCommand* e5 = NativeNewParsed(*echo);
    CHECK(!NativeSetArg(*e5, 1, "any", &err) && err.find("does not accept any") != std::string::npos,
          "any refused where not allowed: %s", err.c_str());

    docs = NativeScriptDocs();
    const char* expected9[] = {
        "TRUE under NOT",
        "- Usage: `num_ratio > <0..1>` or `num_ratio = { region = <int> value > <0..1> }`\n",
        "  - `region`: int, optional: from the strategic region scope\n",
        "as in vanilla, `=` also means above",
        "- Usage: `num_count = { state = <int> count > <int> }`\n",
        "Compare with `>`, `<` or `=` (equal)",
        "  - `owner`: choice (any/own/enemy), optional\n",
        "  - `who`: country (tag, THIS, ROOT, FROM or PREV), optional: from ROOT (or the country scope)\n",
        "  - `owner`: country (tag, THIS, ROOT, FROM, PREV or any), optional\n",
        "  - `foe`: country (tag, THIS, ROOT, FROM or PREV), reserved: not supported yet (a parse error)\n",
        "- Usage: `num_owner > <int>` or `num_owner = { owner = <country>/any count > <int> }`\n",
    };
    for (const char* e : expected9)
        CHECK(docs.find(e) != std::string::npos, "docs contain: %.70s", e);

    // 10. Scope blocks and iterators.
    const Command* blockT = nullptr;
    const Command* blockE = nullptr;
    const Command* crashBlock = nullptr;
    const Command* wrongBlock = nullptr;
    const Command* every = nullptr;
    for (const Command* c : NativeCommands()) {
        if (c->name == "in_region" && c->kind == CommandKind::Trigger) blockT = c;
        if (c->name == "in_region" && c->kind == CommandKind::Effect) blockE = c;
        if (c->name == "crash_region") crashBlock = c;
        if (c->name == "wrong_region") wrongBlock = c;
        if (c->name == "every_region") every = c;
    }
    CHECK(blockT && blockE && crashBlock && wrongBlock && every, "scope blocks and iterators found");
    if (!blockT || !blockE || !crashBlock || !wrongBlock || !every) return g_fail;
    CHECK(blockT->scopeBlock && blockT->childScope == ScopeMask::StrategicRegion && blockT->form == Form::Block,
          "scope block flags");
    CHECK(every->iterator && every->kind == CommandKind::Effect && every->iteratorKind == IteratorKind::Every,
          "every iterator is an effect");
    ScopeTarget t;
    ParsedCommand* b1 = NativeNewParsed(*blockT);
    CHECK(NativeSetArg(*b1, 0, "42", &err) && NativeValidate(*b1), "region parsed");
    CHECK(NativeScopeTarget(b1, &noRegion, &t) && t.id == 42 && t.type == ScopeMask::StrategicRegion, "explicit region (%d)", t.id);
    ParsedCommand* b2 = NativeNewParsed(*blockE);
    CHECK(NativeValidate(*b2), "region is optional");
    CHECK(NativeScopeTarget(b2, &regionScope, &t) && t.id == 42, "region from the scope (%d)", t.id);
    CHECK(!NativeScopeTarget(b2, &noRegion, &t), "no region: no target");
    ParsedCommand* b3 = NativeNewParsed(*crashBlock);
    CHECK(!NativeScopeTarget(b3, &regionScope, &t) && LogCount("'crash_region': target crashed") == 1,
          "crashing target contained and logged");
    ParsedCommand* b4 = NativeNewParsed(*wrongBlock);
    CHECK(!NativeScopeTarget(b4, &regionScope, &t), "target of another scope type rejected");
    docs = NativeScriptDocs();
    const char* expected10[] = {
        "- Usage: `in_region = { region = <int> <triggers> }`\n",
        "- Usage: `in_region = { region = <int> <effects> }`\n",
        "- Usage: `crash_region = { <triggers> }`\n",
        "- Children run in: StrategicRegion scope\n",
        "- Usage: `any_region = { count = <int> tooltip = <key> <triggers> }`\n",
        "- Usage: `every_region = { limit = { <triggers> } random_select_amount = <int> display_individual_scopes = yes "
        "tooltip = <key> <effects> }`\n",
        "- Usage: `random_region = { limit = { <triggers> } tooltip = <key> <effects> }`\n",
    };
    for (const char* e : expected10)
        CHECK(docs.find(e) != std::string::npos, "docs contain: %.70s", e);

    // 11. Translatable tooltip texts (the fake backend has no localisation: English fallback).
    const char* expected11[] = {
        "- Tooltip texts (localisation keys a mod may define; English otherwise):\n  - `KEY`: \"Any region\"\n",
        "  - `TEXTED_KEY`: \"$OLD|Y$ becomes $NEW$.\"\n",
    };
    for (const char* e : expected11)
        CHECK(docs.find(e) != std::string::npos, "docs contain: %.70s", e);
    std::string loc = native_localize({"TEXTED_KEY", "$OLD|Y$ becomes $NEW$."}, {{"OLD", "A"}, {"NEW", "B"}});
    CHECK(loc == "\x11YA\x11! becomes B.", "English fallback filled and highlighted");
    loc = native_localize({"K", "$H|H$, $MISSING$ and a lone $ sign"}, {{"H", "x"}});
    CHECK(loc == "\x11Yx\x11!, $MISSING$ and a lone $ sign", "highlight, unknown placeholder and lone $ kept");
    CHECK(native_localize_key("SOME_KEY") == "SOME_KEY", "missing key localises to itself");

    printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail;
}
