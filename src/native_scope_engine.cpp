// Native script API, engine binding of scope blocks and iterators (NOTES.md F5).
//
// Scope blocks (strategic_region = { region = 42 <children> }) are the generic native commands of
// native_script_engine.cpp (clones of CAlwaysTrigger / CRenameProvinceEffect with the parsed
// arguments at +0x58) that also hold children: keys that are not parameters go to the engine's
// child factory (CTrigger slot 6 / CEffect slot 4) with the children's scope mask, into the
// object's own child list, which CAndTrigger::Evaluate / CEffect's children loop then run in a
// child scope set to the target region.
//
// Iterators are clones of the vanilla state iterators, built by the vanilla factories. Only the
// slots that touch the elements or pass the scope mask on are replaced: any_ 6 (item), 22
// (evaluate), 23 (header); every_ / random_ 4 (item), 25 (collect), 26 (set scope), every_ 27 /
// random_ 28 (element name), every_ 10 (tooltip per element). Each clone keeps its IteratorInfo
// one slot past the template's last.
//
// Effect blocks (create_army = { leader = ... create_unit = { ... } }) are the same generic
// effects with children, but the children stay in the current scope, only the allowed child
// effects are read, and the plugin's execute decides when CEffect's children loop runs.
#include <windows.h>
#include <string.h>

#include <algorithm>

#include "engine.h"
#include "loader.h"
#include "native_binding.h"

namespace NativeBinding {
namespace {

using Factory_t = void(__fastcall*)(void* self, void* parser, int token, uint32_t mask);  // CTrigger 6 / CEffect 4
using Check_t = bool(__fastcall*)(void* trigger, void* scope);                           // CTrigger 3 / 22
using ReadClause_t = void(__fastcall*)(void* self, void* parser, uint32_t mask);         // CTrigger 5
using Run_t = void(__fastcall*)(void* self, void* scope);                                // CEffect 13, collect
using Formatter_t = EngineString*(__fastcall*)(EngineString* out, bool fulfilled, int indent, EngineString* text);
using TriggerTooltip11_t = EngineString*(__fastcall*)(void* self, EngineString* out, void* scope, int p4, char negated,
                                                      int indent, Formatter_t fmt);
using TriggerTooltip12_t = EngineString*(__fastcall*)(void* self, EngineString* out, void* scope, char negated, int indent,
                                                      int mode, int flags);
using EffectTooltip8_t = EngineString*(__fastcall*)(void* self, EngineString* out, void* scope, int indent, int flag);
using EffectTooltip10_t = EngineString*(__fastcall*)(void* self, EngineString* out, void* scope, int indent);
using Alloc_t = void*(__fastcall*)(void* allocator, size_t bytes, size_t align);
using Free_t = void(__fastcall*)(void* allocator, void* p);

const uint32_t kRegionMask = ScopeMask::StrategicRegion;
const size_t kChildCount = 0x14;  // CTrigger / CEffect child list count (CAndTrigger::Evaluate)

void** Vt(void* self) { return *(void***)self; }

// --- Shared ---------------------------------------------------------------------------------
std::string Text(const EngineString& s) { return std::string(s.c_str(), (size_t)s.size); }

std::string Indent(int indent) { return std::string((size_t)(indent > 0 ? indent : 0) * 3, ' '); }

void* RegionOf(const ScopeTarget& t) { return t.type == kRegionMask ? Engine::RegionById(t.id) : nullptr; }

// The element's display name (custom names included).
std::string ElementName(void* region) {
    int id = region ? Engine::RegionTemplateId(region) : -1;
    return id > 0 ? Engine::RegionName(id) : std::string();
}

// "£trigger_yes£" / "£trigger_no£" as the vanilla requirement lists show them (looked up once).
const char* Prefix(bool fulfilled) {
    static const EngineString* yes = nullptr;
    static const EngineString* no = nullptr;
    static volatile long looked = 0;
    if (InterlockedCompareExchange(&looked, 1, 0) == 0) {
        yes = Engine::LocalizeKey("TRIGGER_FULLFILLED_PREFIX");
        no = Engine::LocalizeKey("TRIGGER_UNFULLFILLED_PREFIX");
    }
    const EngineString* s = fulfilled ? yes : no;
    return s ? s->c_str() : "";
}

// Evaluating children under SEH: a fault reads as false and turns the command off (logged once).
volatile long g_faultLogged = 0;
bool g_broken = false;

void LogFault(const Command& c, const char* what, DWORD code) {
    g_broken = true;
    if (InterlockedExchange(&g_faultLogged, 1) == 0)
        Log("Native %s '%s': %s crashed (exception 0x%08lX); region scope blocks and iterators are off from now on",
            c.kind == CommandKind::Effect ? "effect" : "trigger", c.name.c_str(), what, code);
}

bool SafeInRegion(Check_t check, void* self, void* parent, void* region, bool* result, DWORD* code) {
    alignas(16) uint8_t child[Engine::kScopeBuffer];
    __try {
        Engine::ScopeBegin(child, parent);
        Engine::ScopeSetRegion(child, region);
        *result = check(self, child);
        Engine::ScopeEnd(child);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

bool SafeRunInRegion(Run_t run, void* self, void* parent, void* region, DWORD* code) {
    alignas(16) uint8_t child[Engine::kScopeBuffer];
    __try {
        Engine::ScopeBegin(child, parent);
        Engine::ScopeSetRegion(child, region);
        run(self, child);
        Engine::ScopeEnd(child);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

// --- Scope blocks ---------------------------------------------------------------------------
void* Target(void* self, void* scope) {
    ScopeTarget t;
    return !g_broken && NativeScopeTarget(Rec(self), scope, &t) ? RegionOf(t) : nullptr;
}

std::string Header(void* region) { return region ? ElementName(region) : std::string("(no strategic region here)"); }

void __fastcall SB_T_Item(void* self, void* parser, int token, uint32_t mask) {
    ParsedCommand* pc = Rec(self);
    if (pc && NativeParamIndex(*pc->cmd, token) >= 0) return ReadItemFor(pc, parser, token);
    ((Factory_t)Engine::triggerBase.vt[kT_Item])(self, parser, token, pc ? pc->cmd->childScope : mask);
}

void __fastcall SB_E_Item(void* self, void* parser, int token, uint32_t mask) {
    ParsedCommand* pc = Rec(self);
    if (pc && NativeParamIndex(*pc->cmd, token) >= 0) return ReadItemFor(pc, parser, token);
    ((Factory_t)Engine::effectBase.vt[kE_Item])(self, parser, token, pc ? pc->cmd->childScope : mask);
}

bool __fastcall SB_T_Evaluate(void* self, void* scope) {
    ParsedCommand* pc = Rec(self);
    if (!pc) return false;
    LARGE_INTEGER t0{};
    bool timing = NativeTimingEnabled();
    if (timing) QueryPerformanceCounter(&t0);
    bool result = false;
    void* region = Target(self, scope);
    DWORD code = 0;
    if (region && !SafeInRegion((Check_t)Engine::andTrigger.vt[kT_Evaluate], self, scope, region, &result, &code)) {
        LogFault(*pc->cmd, "evaluate", code);
        result = false;
    }
    if (timing) {
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        NativeTimingAdd(*pc->cmd, t1.QuadPart - t0.QuadPart);
    }
    return result;
}

void __fastcall SB_E_Execute(void* self, void* scope, void*, void*) {
    ParsedCommand* pc = Rec(self);
    void* region = pc ? Target(self, scope) : nullptr;
    DWORD code = 0;
    if (region && !SafeRunInRegion((Run_t)Engine::effectBase.vt[kE_Execute], self, scope, region, &code))
        LogFault(*pc->cmd, "execute", code);
}

// The target scope for the children's tooltip. Without a target the children are not shown:
// evaluated in the outer scope they would only log scope errors.
struct ChildScope {
    alignas(16) uint8_t buf[Engine::kScopeBuffer];
    bool own = false;
    ChildScope(void* parent, void* region) {
        if (region && !g_broken) {
            Engine::ScopeBegin(buf, parent);
            Engine::ScopeSetRegion(buf, region);
            own = true;
        }
    }
    ~ChildScope() {
        if (own) Engine::ScopeEnd(buf);
    }
};

// Slot 11: "<region>:" as a requirement line (fulfilled mark from the whole block), then the
// children one level deeper, each marked in the region scope. As CContextTrigger (GER = { }).
EngineString* __fastcall SB_T_Tooltip11(void* self, EngineString* out, void* scope, int, char negated, int indent,
                                        Formatter_t fmt) {
    void* region = Target(self, scope);
    std::string kids;
    ChildScope child(scope, region);
    if (child.own) {
        EngineString tmp{};
        ((TriggerTooltip11_t)Engine::triggerBase.vt[11])(self, &tmp, child.buf, 0, negated, indent + 1, fmt);
        kids = Text(tmp);
        Engine::FreeEngineString(&tmp);
    }
    bool ok = ((Check_t)Vt(self)[3])(self, scope);
    std::string head = Header(region) + ":\n";
    EngineString header{}, line{};
    Engine::ConstructEngineString(&header, head.data(), head.size());
    EngineString* formatted = fmt(&line, ok != (negated != 0), indent, &header);
    std::string text = (formatted ? Text(*formatted) : head) + kids;
    Engine::FreeEngineString(&header);
    Engine::FreeEngineString(&line);
    return MakeTooltip(out, text);
}

// Slot 12: the text form (indent spaces, yes/no prefix, "<region>:", children one level deeper).
EngineString* __fastcall SB_T_Tooltip12(void* self, EngineString* out, void* scope, char negated, int indent, int mode,
                                        int flags) {
    void* region = Target(self, scope);
    std::string text = Indent(indent);
    if ((indent == 0 || mode == 1) && mode != 2) text += Prefix(((Check_t)Vt(self)[3])(self, scope) != (negated != 0));
    text += Header(region) + ":\n";
    ChildScope child(scope, region);
    if (child.own) {
        EngineString tmp{};
        ((TriggerTooltip12_t)Engine::triggerBase.vt[12])(self, &tmp, child.buf, negated, indent + 1, 0, flags);
        text += Text(tmp);
        Engine::FreeEngineString(&tmp);
    }
    return MakeTooltip(out, text);
}

// Effect slot 8: "<region>:" then the children's effect lines one level deeper, in the region.
EngineString* __fastcall SB_E_Tooltip8(void* self, EngineString* out, void* scope, int indent, int flag) {
    void* region = Target(self, scope);
    std::string text = Indent(indent) + Header(region) + ":";
    ChildScope child(scope, region);
    if (child.own) {
        EngineString tmp{};
        ((EffectTooltip8_t)Engine::effectBase.vt[8])(self, &tmp, child.buf, indent + 1, flag);
        if (tmp.size) text += "\n" + Text(tmp);
        Engine::FreeEngineString(&tmp);
    }
    return MakeTooltip(out, text);
}

// Effect slot 10 (the variant the engine uses for previews): the same with the base slot 10.
EngineString* __fastcall SB_E_Tooltip10(void* self, EngineString* out, void* scope, int indent) {
    void* region = Target(self, scope);
    std::string text = Indent(indent) + Header(region) + ":";
    ChildScope child(scope, region);
    if (child.own) {
        EngineString tmp{};
        ((EffectTooltip10_t)Engine::effectBase.vt[10])(self, &tmp, child.buf, indent + 1);
        if (tmp.size) text += "\n" + Text(tmp);
        Engine::FreeEngineString(&tmp);
    }
    return MakeTooltip(out, text);
}

// --- Effect blocks --------------------------------------------------------------------------
// Slot 4: the block's own keys, then the allowed child effects (checked against the block's
// scope mask); anything else is the engine's "Unexpected token" and that use does nothing.
void __fastcall EB_Item(void* self, void* parser, int token, uint32_t mask) {
    ParsedCommand* pc = Rec(self);
    if (pc && NativeParamIndex(*pc->cmd, token) >= 0) return ReadItemFor(pc, parser, token);
    if (pc && NativeIsChild(*pc->cmd, token)) return ((Factory_t)Engine::effectBase.vt[kE_Item])(self, parser, token, mask);
    Engine::ParserUnexpected(parser);  // engine error with file and line, skips the value
    if (!pc) return;
    pc->parseError = true;
    if (InterlockedExchange(&const_cast<Command*>(pc->cmd)->invalidLogged, 1) == 0)
        Log("Native effect '%s': unexpected key '%s' (only its parameters and %s are allowed inside); this use does nothing "
            "(first occurrence logged)",
            pc->cmd->name.c_str(), Engine::TokenName(token), pc->cmd->childNames.empty() ? "-" : pc->cmd->childNames[0].c_str());
}

volatile long g_childFaultLogged = 0;

bool SafeRunChildren(void* self, const void* scope, DWORD* code) {
    __try {
        ((Run_t)Engine::effectBase.vt[kE_Execute])(self, (void*)scope);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

bool RunChildren(void* self, const void* scope) {
    DWORD code = 0;
    if (SafeRunChildren(self, scope, &code)) return true;
    ParsedCommand* pc = Rec(self);
    if (InterlockedExchange(&g_childFaultLogged, 1) == 0)
        Log("Native effect '%s': its child effects crashed (exception 0x%08lX) (logged once)", pc ? pc->cmd->name.c_str() : "?",
            code);
    return false;
}

void __fastcall EB_Execute(void* self, void* scope, void*, void*) {
    NativeExecuteBlock(Rec(self), scope, EffectChildren(&RunChildren, self, scope));
}

// Slots 8 / 10: the block's own line, then the children's lines one level deeper (left out when
// the callback hides them). An invalid use (parse error) runs nothing, so it shows nothing, its
// children included.
bool Invalid(void* self) { return !Rec(self) || Rec(self)->parseError; }

std::string BlockLine(void* self, void* scope, int indent, bool* childrenHidden) {
    std::string own = NativeTooltip(Rec(self), scope, false, childrenHidden);
    return own.empty() ? std::string() : Indent(indent) + own;
}

EngineString* __fastcall EB_Tooltip8(void* self, EngineString* out, void* scope, int indent, int flag) {
    if (Invalid(self)) return MakeTooltip(out, "");
    bool hidden = false;
    std::string text = BlockLine(self, scope, indent, &hidden);
    if (hidden) return MakeTooltip(out, text);
    EngineString tmp{};
    ((EffectTooltip8_t)Engine::effectBase.vt[8])(self, &tmp, scope, text.empty() ? indent : indent + 1, flag);
    if (tmp.size) text += (text.empty() ? "" : "\n") + Text(tmp);
    Engine::FreeEngineString(&tmp);
    return MakeTooltip(out, text);
}

EngineString* __fastcall EB_Tooltip10(void* self, EngineString* out, void* scope, int indent) {
    if (Invalid(self)) return MakeTooltip(out, "");
    bool hidden = false;
    std::string text = BlockLine(self, scope, indent, &hidden);
    if (hidden) return MakeTooltip(out, text);
    EngineString tmp{};
    ((EffectTooltip10_t)Engine::effectBase.vt[10])(self, &tmp, scope, text.empty() ? indent : indent + 1);
    if (tmp.size) text += (text.empty() ? "" : "\n") + Text(tmp);
    Engine::FreeEngineString(&tmp);
    return MakeTooltip(out, text);
}

// --- Iterators ------------------------------------------------------------------------------
struct IteratorInfo {
    Command* cmd;
    const Engine::IteratorTemplate* tmpl;
    int prioritizeKey = 0;       // random_: the vanilla `prioritize` key (rejected)
    volatile long headerState = 0;  // any_: 0 not looked up, 1 looking, 2 done
    std::string header;
};

const Engine::IteratorTemplate& Template(IteratorKind k) {
    return k == IteratorKind::Any ? Engine::anyStateTemplate
           : k == IteratorKind::Every ? Engine::everyStateTemplate
                                      : Engine::randomStateTemplate;
}

IteratorInfo* Info(void* self, IteratorKind k) { return (IteratorInfo*)Vt(self)[Template(k).vt.slots]; }

bool VanillaKey(const Engine::IteratorTemplate& t, int token) {
    for (int i = 0; i < t.keyCount; ++i)
        if (t.keys[i] == token) return true;
    return false;
}

// any_: the vanilla keys (tooltip, count) to the vanilla handler; children checked as region scope.
void __fastcall Any_Item(void* self, void* parser, int token, uint32_t mask) {
    const Engine::IteratorTemplate& t = Engine::anyStateTemplate;
    if (VanillaKey(t, token)) return ((Factory_t)t.vt.vt[kT_Item])(self, parser, token, mask);
    ((Factory_t)Engine::triggerBase.vt[kT_Item])(self, parser, token, kRegionMask);
}

bool SafeAnyLoop(void* self, void* scope, int64_t count, bool* result, DWORD* code) {
    alignas(16) uint8_t child[Engine::kScopeBuffer];
    Check_t all = (Check_t)Engine::andTrigger.vt[kT_Evaluate];
    __try {
        *result = false;
        int regions = Engine::RegionCount();
        Engine::ScopeBegin(child, scope);
        for (int id = 1; id < regions; ++id) {
            void* region = Engine::RegionById(id);
            if (!region) continue;
            Engine::ScopeSetRegion(child, region);
            if (all(self, child) && --count < 1) {
                *result = true;
                break;
            }
        }
        Engine::ScopeEnd(child);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

// The `count` script value (number or variable), fixed point.
bool SafeCount(void* holder, void* scope, int64_t* out) {
    __try {
        int64_t* r = Engine::scriptValueEval(holder, out, scope, 0, false);
        if (r) *out = *r;
        return r != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// any_ slot 22: true once `count` regions (default 1) meet all children, in region id order.
bool __fastcall Any_Evaluate(void* self, void* scope) {
    IteratorInfo* info = Info(self, IteratorKind::Any);
    LARGE_INTEGER t0{};
    bool timing = NativeTimingEnabled();
    if (timing) QueryPerformanceCounter(&t0);
    bool result = false;
    int64_t count = 0;
    if (!g_broken && SafeCount((uint8_t*)self + Engine::kIterCount, scope, &count)) {
        DWORD code = 0;
        if (!SafeAnyLoop(self, scope, count / 100000, &result, &code)) {
            LogFault(*info->cmd, "evaluate", code);
            result = false;
        }
    }
    if (timing) {
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        NativeTimingAdd(*info->cmd, t1.QuadPart - t0.QuadPart);
    }
    return result;
}

// any_ slot 23: the header ("Any strategic region"): the localisation key if a mod defines it.
EngineString* __fastcall Any_Header(void* self, EngineString* out, void*) {
    IteratorInfo* info = Info(self, IteratorKind::Any);
    if (InterlockedCompareExchange(&info->headerState, 1, 0) == 0) {
        const EngineString* s = info->cmd->headerKey.empty() ? nullptr : Engine::LocalizeKey(info->cmd->headerKey.c_str());
        info->header = s ? s->c_str() : info->cmd->headerText;
        InterlockedExchange(&info->headerState, 2);
    }
    return MakeTooltip(out, info->headerState == 2 ? info->header : info->cmd->headerText);
}

// every_ / random_ slot 4: limit and children checked as region scope; prioritize rejected.
void IterItem(IteratorKind kind, void* self, void* parser, int token, uint32_t mask) {
    const Engine::IteratorTemplate& t = Template(kind);
    IteratorInfo* info = Info(self, kind);
    if (token == t.limitKey) {
        uint8_t* limit = (uint8_t*)self + Engine::kIterLimit;
        if (*(int*)(limit + kChildCount) != 0)  // a second limit: the vanilla error
            return ((Factory_t)t.vt.vt[kE_Item])(self, parser, token, mask);
        return ((ReadClause_t)Vt(limit)[kT_ReadClause])(limit, parser, kRegionMask);
    }
    if (token && token == info->prioritizeKey) {
        if (InterlockedExchange(&info->cmd->invalidLogged, 1) == 0)
            Log("Native effect '%s': prioritize (a list of state ids) is not supported; ignored (first occurrence logged)",
                info->cmd->name.c_str());
        Engine::ParserUnexpected(parser);  // engine error with file and line, skips the value
        return;
    }
    if (VanillaKey(t, token)) return ((Factory_t)t.vt.vt[kE_Item])(self, parser, token, mask);
    ((Factory_t)Engine::effectBase.vt[kE_Item])(self, parser, token, kRegionMask);
}
void __fastcall Every_Item(void* self, void* parser, int token, uint32_t mask) { IterItem(IteratorKind::Every, self, parser, token, mask); }
void __fastcall Random_Item(void* self, void* parser, int token, uint32_t mask) { IterItem(IteratorKind::Random, self, parser, token, mask); }

// The candidate list (CPdxArray at kIterCandidates), grown with its own allocator as the vanilla
// collect does.
struct Candidates {
    void** data;
    int cap;
    int count;
    void* allocator;
};

void Push(void* self, void* region) {
    auto* a = (Candidates*)((uint8_t*)self + Engine::kIterCandidates);
    if (a->count >= a->cap) {
        int cap = std::max(std::max(a->cap * 2, 16), Engine::RegionCount());
        void** data = (void**)((Alloc_t)Vt(a->allocator)[1])(a->allocator, (size_t)cap * sizeof(void*), 8);
        if (a->count) memcpy(data, a->data, (size_t)a->count * sizeof(void*));
        if (a->data) ((Free_t)Vt(a->allocator)[2])(a->allocator, a->data);
        a->data = data;
        a->cap = cap;
    }
    a->data[a->count++] = region;
}

bool SafeCollect(void* self, void* scope, DWORD* code) {
    alignas(16) uint8_t child[Engine::kScopeBuffer];
    uint8_t* limit = (uint8_t*)self + Engine::kIterLimit;
    Check_t check = (Check_t)Vt(limit)[3];
    __try {
        int regions = Engine::RegionCount();
        Engine::ScopeBegin(child, scope);
        for (int id = 1; id < regions; ++id) {
            void* region = Engine::RegionById(id);
            if (!region) continue;
            Engine::ScopeSetRegion(child, region);
            if (check(limit, child)) Push(self, region);
        }
        Engine::ScopeEnd(child);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

// Slot 25: the regions that meet `limit`, in id order (the caller cleared the list).
void CollectFor(IteratorKind kind, void* self, void* scope) {
    DWORD code = 0;
    if (!g_broken && !SafeCollect(self, scope, &code)) {
        ((Candidates*)((uint8_t*)self + Engine::kIterCandidates))->count = 0;
        LogFault(*Info(self, kind)->cmd, "limit", code);
    }
}
void __fastcall Every_Collect(void* self, void* scope) { CollectFor(IteratorKind::Every, self, scope); }
void __fastcall Random_Collect(void* self, void* scope) { CollectFor(IteratorKind::Random, self, scope); }

// Slot 26: the element becomes the scope.
void __fastcall Iter_SetScope(void*, void* region, void* scope) { Engine::ScopeSetRegion(scope, region); }

// every_ slot 27: the element's name, highlighted as vanilla does (\x11Y ... \x11!).
EngineString* __fastcall Every_Name(void*, EngineString* out, void* region, void*) {
    return MakeTooltip(out, "\x11Y" + ElementName(region) + "\x11!");
}
// random_ slot 28: the element's name (the caller highlights it).
EngineString* __fastcall Random_Name(void*, EngineString* out, void* region) { return MakeTooltip(out, ElementName(region)); }

// every_ slot 10: the children's preview tooltip once per matching region, each in its scope.
EngineString* __fastcall Every_Tooltip10(void* self, EngineString* out, void* scope, int indent) {
    auto* a = (Candidates*)((uint8_t*)self + Engine::kIterCandidates);
    a->count = 0;
    ((Run_t)Vt(self)[25])(self, scope);
    std::string text;
    for (int i = 0; i < a->count; ++i) {
        ChildScope child(scope, a->data[i]);
        if (!child.own) break;
        if (!text.empty() && text.back() != '\n') text += '\n';
        EngineString tmp{};
        ((EffectTooltip10_t)Engine::effectBase.vt[10])(self, &tmp, child.buf, indent);
        text += Text(tmp);
        Engine::FreeEngineString(&tmp);
    }
    return MakeTooltip(out, text);
}

void* __fastcall IterTriggerCreate(TriggerEntryObj* e) {
    void* obj = ((Create_t)Template(e->cmd->iteratorKind).entryVtable[1])(e);
    if (obj) *(void***)obj = e->objectVtable;
    return obj;
}
void* __fastcall IterEffectCreate(EffectEntryObj* e) {
    void* obj = ((Create_t)Template(e->cmd->iteratorKind).entryVtable[1])(e);
    if (obj) *(void***)obj = e->objectVtable;
    return obj;
}

}  // namespace

bool BindScopeBlock(Command& c, std::string* reason) {
    if (!Engine::HaveScopeBlocks()) {
        *reason = "scope block engine functions not found on this build";
        return false;
    }
    if (c.kind == CommandKind::Trigger) {
        void** vt = Clone(Engine::triggerTemplate);
        vt[kT_Value] = (void*)&T_Value;
        vt[kT_ReadClause] = Engine::triggerBase.vt[kT_ReadClause];
        vt[kT_Item] = (void*)&SB_T_Item;
        vt[kT_Validate] = (void*)&T_Validate;
        vt[11] = (void*)&SB_T_Tooltip11;
        vt[12] = (void*)&SB_T_Tooltip12;
        vt[kT_Scopes] = (void*)&T_Scopes;
        vt[kT_Targets] = (void*)&T_Targets;
        vt[kT_Tooltip] = Engine::andTrigger.vt[kT_Tooltip];  // no own line: slot 11 writes it
        vt[kT_Evaluate] = (void*)&SB_T_Evaluate;
        auto* e = new TriggerEntryObj{EntryVtable(Engine::triggerTemplateEntryVtable, (void*)&TriggerEntryCreate),
                                      c.help.c_str(), 1, {}, &c, vt};
        return Engine::RegisterTrigger(c.token, e);
    }
    void** vt = Clone(Engine::effectTemplate);
    vt[kE_ReadClause] = Engine::effectBase.vt[kE_ReadClause];
    vt[kE_Item] = (void*)&SB_E_Item;
    vt[kE_Validate] = (void*)&E_Validate;
    vt[kE_Value] = (void*)&E_Value;
    vt[kE_Tooltip] = Engine::effectBase.vt[kE_Tooltip];  // no own line: slot 8 writes it
    vt[8] = (void*)&SB_E_Tooltip8;
    vt[10] = (void*)&SB_E_Tooltip10;
    vt[kE_Execute] = (void*)&SB_E_Execute;
    vt[kE_Scopes] = (void*)&E_Scopes;
    auto* e = new EffectEntryObj{EntryVtable(Engine::effectTemplateEntryVtable, (void*)&EffectEntryCreate), c.help.c_str(), &c, vt};
    return Engine::RegisterEffect(c.token, (Engine::EffectEntry*)e);
}

bool BindEffectBlock(Command& c, std::string* reason) {
    if (!Engine::HaveScopeBlocks()) {  // FreeEngineString (tooltips) comes with the scope block pieces
        *reason = "effect block engine functions not found on this build";
        return false;
    }
    void** vt = Clone(Engine::effectTemplate);
    vt[kE_ReadClause] = Engine::effectBase.vt[kE_ReadClause];
    vt[kE_Item] = (void*)&EB_Item;
    vt[kE_Validate] = (void*)&E_Validate;
    vt[kE_Value] = (void*)&E_Value;
    vt[kE_Tooltip] = Engine::effectBase.vt[kE_Tooltip];  // no own line here: slots 8 / 10 write it
    vt[8] = (void*)&EB_Tooltip8;
    vt[10] = (void*)&EB_Tooltip10;
    vt[kE_Execute] = (void*)&EB_Execute;
    vt[kE_Scopes] = (void*)&E_Scopes;
    auto* e = new EffectEntryObj{EntryVtable(Engine::effectTemplateEntryVtable, (void*)&EffectEntryCreate), c.help.c_str(), &c, vt};
    return Engine::RegisterEffect(c.token, (Engine::EffectEntry*)e);
}

bool BindIterator(Command& c, std::string* reason) {
    if (!Engine::HaveScopeBlocks()) {
        *reason = "iterator engine functions not found on this build";
        return false;
    }
    const Engine::IteratorTemplate& t = Template(c.iteratorKind);
    auto* info = new IteratorInfo{&c, &t};
    // Document the vanilla keys by name; random_'s prioritize is the one rejected.
    c.keysDoc.clear();
    for (int i = 0; i < t.keyCount; ++i) {
        const char* name = Engine::TokenName(t.keys[i]);
        if (!*name) continue;
        if (c.iteratorKind == IteratorKind::Random && strcmp(name, "prioritize") == 0) {
            info->prioritizeKey = t.keys[i];
            continue;
        }
        if (!c.keysDoc.empty()) c.keysDoc += ", ";
        c.keysDoc += name;
    }
    if (info->prioritizeKey) c.keysDoc += "; prioritize is not supported (a parse error)";
    void** vt = Clone(t.vt, info);
    switch (c.iteratorKind) {
        case IteratorKind::Any: {
            vt[kT_Item] = (void*)&Any_Item;
            vt[kT_Evaluate] = (void*)&Any_Evaluate;
            vt[23] = (void*)&Any_Header;
            auto* e = new TriggerEntryObj{EntryVtable(t.entryVtable, (void*)&IterTriggerCreate), c.help.c_str(), 1, {}, &c, vt};
            return Engine::RegisterTrigger(c.token, e);
        }
        case IteratorKind::Every:
            vt[kE_Item] = (void*)&Every_Item;
            vt[10] = (void*)&Every_Tooltip10;
            vt[25] = (void*)&Every_Collect;
            vt[26] = (void*)&Iter_SetScope;
            vt[27] = (void*)&Every_Name;
            break;
        default:
            vt[kE_Item] = (void*)&Random_Item;
            vt[25] = (void*)&Random_Collect;
            vt[26] = (void*)&Iter_SetScope;
            vt[28] = (void*)&Random_Name;
            break;
    }
    auto* e = new EffectEntryObj{EntryVtable(t.entryVtable, (void*)&IterEffectCreate), c.help.c_str(), &c, vt};
    return Engine::RegisterEffect(c.token, (Engine::EffectEntry*)e);
}

}  // namespace NativeBinding
