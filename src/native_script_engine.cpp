// Native script API, engine binding: turns a registered Command into a real engine effect or
// trigger. Each command gets a clone of a template class's vtable (RTTI locator kept at [-1])
// with the generic slots below; its objects are created by the template's factory and carry a
// ParsedCommand* at +0x58 (the template's private area, used only by slots we replace).
// Slot numbers: NOTES.md C1.
#include <string.h>

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

void ReadItem(void* self, void* parser, int token) {
    ParsedCommand* pc = Rec(self);
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
        bool ok = c.kind == CommandKind::Effect ? BindEffect(c) : BindTrigger(c);
        if (!ok) *reason = "engine registry unavailable";
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
    EngineBackend backend;
    std::wstring docs = LoaderOutputPath(L"loader_script_docs.md");
    int n = NativeScriptFinalize(backend, docs);
    Log("Native script: %d of %zu command(s) registered", n, NativeCommands().size());
}
