// Native script API, engine binding: turns a registered Command into a real engine effect or
// trigger. Each command gets a clone of a template class's vtable (RTTI locator kept at [-1])
// with the generic slots below; its objects are created by the template's factory and carry a
// ParsedCommand* at +0x58 (the template's private area, used only by slots we replace).
// Slot numbers: NOTES.md C1.
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "engine.h"
#include "loader.h"
#include "native_script.h"

namespace {

const size_t kRecordOffset = 0x58;

// Effect slots.
const size_t kE_ReadClause = 3, kE_Item = 4, kE_Validate = 5, kE_Value = 6, kE_Tooltip = 7, kE_Execute = 13, kE_Scopes = 19;
// Trigger slots.
const size_t kT_Value = 4, kT_ReadClause = 5, kT_Item = 6, kT_Validate = 7, kT_Scopes = 14, kT_Targets = 15,
             kT_Tooltip = 21, kT_Evaluate = 22;

struct EffectEntryObj {  // engine reads { vtable, help }
    void** vtable;
    const char* help;
    Command* cmd;
    void** objectVtable;
};
struct TriggerEntryObj {  // engine reads { vtable, help, flag }
    void** vtable;
    const char* help;
    uint8_t flag;
    uint8_t pad[7];
    Command* cmd;
    void** objectVtable;
};

ParsedCommand* Rec(void* self) { return *(ParsedCommand**)((uint8_t*)self + kRecordOffset); }

const char* ValueText(const void* value) {
    // Parser value: text pointer at +8, length at +0x14 (as read by set_state_name's slot 6).
    int n = *(const int*)((const uint8_t*)value + 0x14);
    return n ? *(const char* const*)((const uint8_t*)value + 8) : "";
}

void ReadItemFor(ParsedCommand* pc, void* parser, int token) {
    int idx = pc ? NativeParamIndex(*pc->cmd, token) : -1;
    if (idx < 0) {
        Engine::ParserUnexpected(parser);  // engine error with file/line, skips the value
        if (pc) pc->parseError = true;
        return;
    }
    EngineString text{};
    text.cap = 15;
    Engine::ParserReadString(parser, &text);  // engine-owned if > 15 chars; leaked (parse time)
    std::string error;
    if (!NativeSetArg(*pc, (size_t)idx, text.c_str(), &error)) {
        pc->parseError = true;
        Log("Native command '%s': %s", pc->cmd->name.c_str(), error.c_str());
    }
}

void ReadItem(void* self, void* parser, int token) { ReadItemFor(Rec(self), parser, token); }

bool ReadValue(void* self, const void* value) {
    ParsedCommand* pc = Rec(self);
    if (!pc) return false;
    std::string error;
    if (!NativeSetArg(*pc, 0, ValueText(value), &error)) {
        pc->parseError = true;
        Log("Native command '%s': %s", pc->cmd->name.c_str(), error.c_str());
        return false;
    }
    return true;
}

EngineString* MakeTooltip(EngineString* out, const std::string& text) {
    Engine::ConstructEngineString(out, text.data(), text.size());
    return out;
}

// --- Effect slots --------------------------------------------------------------------------
void __fastcall E_Item(void* self, void* parser, int token, void*) { ReadItem(self, parser, token); }
bool __fastcall E_Validate(void* self) { return Rec(self) && NativeValidate(*Rec(self)); }
bool __fastcall E_Value(void* self, const void* value) { return ReadValue(self, value); }
EngineString* __fastcall E_Tooltip(void* self, EngineString* out, void* ctx) {
    return MakeTooltip(out, NativeTooltip(Rec(self), ctx, false));
}
void __fastcall E_Execute(void* self, void* scope, void*, void*) { NativeExecute(Rec(self), scope); }
uint64_t __fastcall E_Scopes(void* self) { return Rec(self) ? Rec(self)->cmd->scopes : 0; }

// --- Trigger slots -------------------------------------------------------------------------
bool __fastcall T_Value(void* self, const void* value) { return ReadValue(self, value); }
void __fastcall T_Item(void* self, void* parser, int token, void*) { ReadItem(self, parser, token); }
bool __fastcall T_Validate(void* self) { return Rec(self) && NativeValidate(*Rec(self)); }
uint64_t __fastcall T_Scopes(void* self) { return Rec(self) ? Rec(self)->cmd->scopes : 0; }
uint64_t __fastcall T_Targets(void*) { return 0; }
EngineString* __fastcall T_Tooltip(void* self, EngineString* out, void* ctx, char negated) {
    std::string text = NativeTooltip(Rec(self), ctx, negated != 0);
    text += '\n';  // as vanilla trigger tooltips
    return MakeTooltip(out, text);
}
bool __fastcall T_Evaluate(void* self, void* scope) { return NativeEvaluate(Rec(self), scope); }

// --- Entries / factories ---------------------------------------------------------------------
using Create_t = void*(__fastcall*)(void* entry);

void* __fastcall EntryDestroy(void* self, unsigned) { return self; }  // entries live forever

void* __fastcall EffectEntryCreate(EffectEntryObj* e) {
    void* obj = ((Create_t)Engine::effectTemplateEntryVtable[1])(e);
    if (obj) {
        *(void***)obj = e->objectVtable;
        *(ParsedCommand**)((uint8_t*)obj + kRecordOffset) = NativeNewParsed(*e->cmd);
    }
    return obj;
}

void* __fastcall TriggerEntryCreate(TriggerEntryObj* e) {
    void* obj = ((Create_t)Engine::triggerTemplateEntryVtable[1])(e);
    if (obj) {
        *(void***)obj = e->objectVtable;
        *(ParsedCommand**)((uint8_t*)obj + kRecordOffset) = NativeNewParsed(*e->cmd);
    }
    return obj;
}

// Vtable copy with the RTTI locator at [-1]; returns the vtable pointer.
void** Clone(const Engine::Vtable& tmpl) {
    auto** block = new void*[tmpl.slots + 1];
    block[0] = tmpl.vt[-1];
    memcpy(block + 1, tmpl.vt, tmpl.slots * sizeof(void*));
    return block + 1;
}

void** EntryVtable(void** templateEntryVt, void* create) {
    auto** block = new void*[3];
    block[0] = templateEntryVt[-1];
    block[1] = (void*)&EntryDestroy;
    block[2] = create;
    return block + 1;
}

bool BindEffect(Command& c) {
    void** vt = Clone(Engine::effectTemplate);
    vt[kE_ReadClause] = c.form == Form::Block ? Engine::effectBase.vt[3] : Engine::effectValueReader.vt[3];
    vt[kE_Item] = (void*)&E_Item;
    vt[kE_Validate] = (void*)&E_Validate;
    vt[kE_Value] = (void*)&E_Value;
    vt[kE_Tooltip] = (void*)&E_Tooltip;
    vt[kE_Execute] = (void*)&E_Execute;
    vt[kE_Scopes] = (void*)&E_Scopes;
    auto* e = new EffectEntryObj{EntryVtable(Engine::effectTemplateEntryVtable, (void*)&EffectEntryCreate), c.help.c_str(), &c, vt};
    return Engine::RegisterEffect(c.token, (Engine::EffectEntry*)e);
}

bool BindTrigger(Command& c) {
    void** vt = Clone(Engine::triggerTemplate);
    vt[kT_Value] = (void*)&T_Value;
    vt[kT_ReadClause] = c.form == Form::Block ? Engine::triggerBase.vt[5] : Engine::triggerTemplate.vt[5];
    vt[kT_Item] = (void*)&T_Item;
    vt[kT_Validate] = (void*)&T_Validate;
    vt[kT_Scopes] = (void*)&T_Scopes;
    vt[kT_Targets] = (void*)&T_Targets;
    vt[kT_Tooltip] = (void*)&T_Tooltip;
    vt[kT_Evaluate] = (void*)&T_Evaluate;
    auto* e = new TriggerEntryObj{EntryVtable(Engine::triggerTemplateEntryVtable, (void*)&TriggerEntryCreate),
                                  c.help.c_str(), 1, {}, &c, vt};
    return Engine::RegisterTrigger(c.token, e);
}

// --- Numeric triggers (NOTES.md E2) ------------------------------------------------------------
// Objects are the numeric template (CIntTrigger / CValueTrigger) built by its own constructor in
// a block g_numRecord + 16 bytes long; the ParsedCommand* sits right after the larger template.
// Template fields used: operator token +0x58; right side: Count int +0x60, or CScopedVariable
// +0x68 when the byte at +0x70 is set; Ratio CScopedVariable +0x60.
const size_t kOp = 0x58, kCountInt = 0x60, kCountVar = 0x68, kCountVarFlag = 0x70, kRatioVar = 0x60;
const size_t kParserOp = 0x78, kParserValue = 0xc0;
const int kBlockValue = 3;  // parser value type of `{ ... }` (checked: kCodeBlockValueCheck)
size_t g_numRecord = 0;
int g_tokEqual = 0;

ParsedCommand* NumRec(void* self) { return self ? *(ParsedCommand**)((uint8_t*)self + g_numRecord) : nullptr; }
const Engine::NumericTemplate& Tmpl(const Command& c) {
    return c.numericKind == NumericKind::Ratio ? Engine::numericRatio : Engine::numericCount;
}
int& OpAt(void* self) { return *(int*)((uint8_t*)self + kOp); }
bool IsOp(int t) { return t == Engine::tokGreater || t == Engine::tokLess || (g_tokEqual && t == g_tokEqual); }
CompareOp OpOf(int t) {
    return t == Engine::tokGreater ? CompareOp::Greater : t == Engine::tokLess ? CompareOp::Less : CompareOp::Equal;
}

using Clause_t = void(__fastcall*)(void* self, void* parser, void* ctx);
using ReadValue_t = bool(__fastcall*)(void* self, void* value);
using HolderRead_t = void(__fastcall*)(void* holder, void* parser);
using VarRegister_t = void(__fastcall*)(void* self, int token, const char* desc);

// No C++ objects: SEH around the engine's script value evaluation.
bool SafeScriptValue(void* holder, const void* scope, int64_t* out) {
    __try {
        int64_t* r = Engine::scriptValueEval(holder, out, scope, 0, false);
        if (r) *out = *r;
        return r != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The right side in the command's own unit: Count -> whole number (vanilla truncates variables),
// Ratio -> fixed point.
bool RightSide(void* self, const Command& c, const void* scope, int64_t* rhs) {
    uint8_t* o = (uint8_t*)self;
    if (c.numericKind == NumericKind::Ratio) return SafeScriptValue(o + kRatioVar, scope, rhs);
    if (!*(o + kCountVarFlag)) {
        *rhs = *(int*)(o + kCountInt);
        return true;
    }
    int64_t v = 0;
    if (!SafeScriptValue(o + kCountVar, scope, &v)) return false;
    *rhs = v / 100000;
    return true;
}

void BadOperator(ParsedCommand* pc) {
    pc->parseError = true;
    if (InterlockedExchange(&const_cast<Command*>(pc->cmd)->invalidLogged, 1) == 0)
        Log("Native command '%s': the comparison needs >, < or =; this use is ignored (first occurrence logged)",
            pc->cmd->name.c_str());
}

void __fastcall N_ReadClause(void* self, void* parser, void* ctx) {
    ParsedCommand* pc = NumRec(self);
    if (!pc) return;
    if (*(int*)((uint8_t*)parser + kParserValue) == kBlockValue) {  // name = { ... }
        pc->valueForm = false;
        ((Clause_t)Engine::triggerBase.vt[5])(self, parser, ctx);
        return;
    }
    pc->valueForm = true;  // name > value
    int op = *(int*)((uint8_t*)parser + kParserOp);
    if (!IsOp(op)) BadOperator(pc);
    OpAt(self) = op;
    ((ReadValue_t)Tmpl(*pc->cmd).vt.vt[4])(self, (uint8_t*)parser + kParserValue);  // number or variable
}

void __fastcall N_Item(void* self, void* parser, int token, void*) {
    ParsedCommand* pc = NumRec(self);
    if (pc && token == pc->cmd->compareToken) {
        int op = *(int*)((uint8_t*)parser + kParserOp);
        if (!IsOp(op)) BadOperator(pc);
        OpAt(self) = op;
        bool ratio = pc->cmd->numericKind == NumericKind::Ratio;
        void* holder = (uint8_t*)self + (ratio ? kRatioVar : kCountVar);
        ((HolderRead_t)(*(void***)holder)[3])(holder, parser);  // CScopedVariable::Read: number or variable
        if (!ratio) *((uint8_t*)self + kCountVarFlag) = 1;
        pc->comparePresent = true;
        return;
    }
    ReadItemFor(pc, parser, token);
}

bool __fastcall N_Validate(void* self) { return NumRec(self) && NativeValidate(*NumRec(self)); }
uint64_t __fastcall N_Scopes(void* self) { return NumRec(self) ? NumRec(self)->cmd->scopes : 0; }

bool __fastcall N_Evaluate(void* self, void* scope) {
    ParsedCommand* pc = NumRec(self);
    int64_t v = 0, rhs = 0;
    if (!pc || !NativeNumericValue(pc, scope, &v) || !RightSide(self, *pc->cmd, scope, &rhs)) return false;
    return NativeCompare(pc->cmd->numericKind, OpOf(OpAt(self)), v, rhs);
}

// Slot 23 (also what a game variable reads). CIntTrigger: int (this, scope).
int __fastcall N_GetCount(void* self, void* scope) {
    int64_t v = 0;
    NativeNumericValue(NumRec(self), scope, &v);
    return (int)v;
}
// CValueTrigger: CFixedPoint* (this, out, scope).
int64_t* __fastcall N_GetRatio(void* self, int64_t* out, void* scope) {
    NativeNumericValue(NumRec(self), scope, out);
    return out;
}

EngineString* __fastcall N_Tooltip(void* self, EngineString* out, void* scope, char negated) {
    ParsedCommand* pc = NumRec(self);
    std::string text;
    if (pc) {
        int64_t rhs = 0;
        bool known = RightSide(self, *pc->cmd, scope, &rhs);
        text = NativeNumericTooltip(pc, scope, negated != 0, OpOf(OpAt(self)), rhs, known);
    }
    text += '\n';
    return MakeTooltip(out, text);
}

void* __fastcall NumericEntryCreate(TriggerEntryObj* e) {
    const Engine::NumericTemplate& t = Tmpl(*e->cmd);
    size_t bytes = g_numRecord + 0x10;
    void* obj = t.alloc(bytes);
    if (!obj) return nullptr;
    memset(obj, 0, bytes);
    t.ctor(obj);
    *(void***)obj = e->objectVtable;
    *(ParsedCommand**)((uint8_t*)obj + g_numRecord) = NativeNewParsed(*e->cmd);
    return obj;
}

bool SafeRegisterVariable(void* proto, VarRegister_t fn, int token, const char* desc) {
    __try {
        fn(proto, token, desc);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool BindNumeric(Command& c, std::string* reason) {
    if (!Engine::HaveNumeric()) {
        *reason = "numeric trigger engine functions not found on this build";
        return false;
    }
    if (!g_numRecord) {
        // The parser stores the operator as a static lexer token. '>' and '<' come from the numeric
        // template's code; '=' is looked up by name among the static tokens (never interned: that
        // would create a new dynamic token the parser never uses). All three are checked by name.
        for (int t = 1; t < 0x2000 && !g_tokEqual; ++t)
            if (strcmp(Engine::TokenName(t), "=") == 0) g_tokEqual = t;
        if (!g_tokEqual || strcmp(Engine::TokenName(Engine::tokGreater), ">") != 0 ||
            strcmp(Engine::TokenName(Engine::tokLess), "<") != 0) {
            Log("ERROR: numeric triggers: operator tokens not as expected ('>' 0x%x = \"%s\", '<' 0x%x = \"%s\", '=' 0x%x)",
                Engine::tokGreater, Engine::TokenName(Engine::tokGreater), Engine::tokLess, Engine::TokenName(Engine::tokLess),
                g_tokEqual);
            *reason = "comparison operator tokens not found";
            return false;
        }
        g_numRecord = (std::max(Engine::numericCount.size, Engine::numericRatio.size) + 7) & ~(size_t)7;
        Log("Numeric triggers: operator tokens '>' 0x%x, '<' 0x%x, '=' 0x%x; record at +0x%zx", Engine::tokGreater,
            Engine::tokLess, g_tokEqual, g_numRecord);
    }
    const Engine::NumericTemplate& t = Tmpl(c);
    void** vt = Clone(t.vt);
    vt[kT_ReadClause] = (void*)&N_ReadClause;
    vt[kT_Item] = (void*)&N_Item;
    vt[kT_Validate] = (void*)&N_Validate;
    vt[kT_Scopes] = (void*)&N_Scopes;
    vt[kT_Targets] = (void*)&T_Targets;
    vt[kT_Tooltip] = (void*)&N_Tooltip;
    vt[kT_Evaluate] = (void*)&N_Evaluate;
    vt[23] = c.numericKind == NumericKind::Ratio ? (void*)&N_GetRatio : (void*)&N_GetCount;
    auto* e = new TriggerEntryObj{EntryVtable(Engine::triggerTemplateEntryVtable, (void*)&NumericEntryCreate),
                                  c.help.c_str(), 1, {}, &c, vt};
    if (!Engine::RegisterTrigger(c.token, e)) {
        *reason = "engine registry unavailable";
        return false;
    }
    // Game variable of the same name (vanilla's "auto generated using the trigger"): the template's
    // slot 18 adds it to the variable maps of the scopes in slot 14.
    if (c.ValueFormAllowed() && (c.scopes & (ScopeMask::Country | ScopeMask::State))) {
        void* proto = NumericEntryCreate(e);
        if (proto) {
            NumRec(proto)->valueForm = true;
            c.variable = SafeRegisterVariable(proto, (VarRegister_t)vt[18], c.token, c.help.c_str());
            if (!c.variable) Log("Native trigger '%s': registering the game variable crashed; trigger only", c.name.c_str());
        }
    }
    return true;
}

// Binding under SEH: a crash refuses only this command and names the faulting address.
int BindFilter(EXCEPTION_POINTERS* ep, DWORD* code, void** where) {
    *code = ep->ExceptionRecord->ExceptionCode;
    *where = ep->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

bool SafeBind(Command* c, std::string* reason, DWORD* code, void** where) {
    __try {
        if (c->numeric) return BindNumeric(*c, reason);
        return c->kind == CommandKind::Effect ? BindEffect(*c) : BindTrigger(*c);
    } __except (BindFilter(GetExceptionInformation(), code, where)) {
        return false;
    }
}

class EngineBackend : public NativeBackend {
public:
    int Intern(const char* s) override { return Engine::Intern(s); }
    bool EngineHas(CommandKind kind, int token) override {
        return kind == CommandKind::Effect ? Engine::EffectRegistered(token) : Engine::TriggerRegistered(token);
    }
    bool Bind(Command& c, std::string* reason) override {
        if (!Engine::HaveNativeScript()) {
            *reason = "native script engine functions not found on this build";
            return false;
        }
        DWORD code = 0;
        void* where = nullptr;
        bool ok = SafeBind(&c, reason, &code, &where);
        if (code) {
            char buf[160];
            sprintf_s(buf, "binding crashed (exception 0x%08lX at %s)", code, DescribeAddress(where).c_str());
            *reason = buf;
            return false;
        }
        if (!ok && reason->empty()) *reason = "engine registry unavailable";
        return ok;
    }
};

}  // namespace

// --- Scope (read-only view of CEventScope; NOTES.md C1 scope table) ---------------------------
namespace {
template <typename T>
T At(const uint8_t* p, size_t off) {
    T v;
    memcpy(&v, p + off, sizeof v);
    return v;
}
bool IdSet(const uint8_t* p, size_t off) { return At<int>(p, off) != 0 || At<int>(p, off + 4) != 0; }
}  // namespace

uint32_t Scope::types() const {
    if (!p_) return 0;
    uint32_t t = 0;
    if (At<int>(p_, 0xa8) > 0) t |= ScopeMask::State;
    if (At<int>(p_, 0x08) > 0) t |= ScopeMask::Country;
    if (IdSet(p_, 0x50)) t |= ScopeMask::Character;
    if (At<const void*>(p_, 0x60)) t |= ScopeMask::Combatant;
    if (IdSet(p_, 0x68)) t |= ScopeMask::Ace;
    if (At<const void*>(p_, 0x48)) t |= ScopeMask::StrategicRegion;
    if (IdSet(p_, 0x58)) t |= ScopeMask::Operation;
    if (IdSet(p_, 0x70)) t |= ScopeMask::Unit;
    if (IdSet(p_, 0x78)) t |= ScopeMask::IndustrialOrg;
    if (IdSet(p_, 0x80)) t |= ScopeMask::PurchaseContract;
    if (IdSet(p_, 0x88)) t |= ScopeMask::RaidInstance;
    if (IdSet(p_, 0x90)) t |= ScopeMask::SpecialProject;
    if (IdSet(p_, 0x98)) t |= ScopeMask::Faction;
    return t;
}
int Scope::stateId() const { return p_ && At<int>(p_, 0xa8) > 0 ? At<int>(p_, 0xa8) : 0; }
int Scope::countryTag() const { return p_ && At<int>(p_, 0x08) > 0 ? At<int>(p_, 0x08) : 0; }
int Scope::strategicRegionId() const {
    void* region = p_ ? At<void*>(p_, 0x48) : nullptr;
    int id = region ? Engine::RegionTemplateId(region) : 0;
    return id > 0 ? id : 0;
}
Scope Scope::link(size_t offset) const {
    const uint8_t* q = p_ ? At<const uint8_t*>(p_, offset) : nullptr;
    return Scope(q && q != p_ ? q : nullptr);
}
Scope Scope::root() const { return link(0x18); }
Scope Scope::from() const { return link(0x20); }
Scope Scope::prev() const { return link(0x28); }

// Runs at lexer-ready (lifecycle): registers every queued command and writes the docs file.
void NativeScriptRegisterWithEngine() {
    static EngineBackend backend;  // kept: resolves country tags while scripts are parsed
    std::wstring docs = LoaderOutputPath(L"loader_script_docs.md");
    int n = NativeScriptFinalize(backend, docs);
    Log("Native script: %d of %zu command(s) registered", n, NativeCommands().size());
}
