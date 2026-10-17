// Engine access for scope blocks and iterators (region_scope, NOTES.md F5): the vanilla state
// iterators the region iterators are cloned from, child scopes built like those iterators build
// them, and freeing engine-owned tooltip strings. Resolved once from RTTI and src/signatures.h.
#include <string.h>

#include "engine.h"
#include "loader.h"
#include "signatures.h"

namespace Engine {

IteratorTemplate anyStateTemplate, everyStateTemplate, randomStateTemplate;
Vtable andTrigger;

namespace {

template <typename T>
T Rd(const void* base, size_t offset) {
    T v;
    memcpy(&v, (const uint8_t*)base + offset, sizeof v);
    return v;
}

using ScopeCopy_t = void*(__fastcall*)(void* scope, const void* parent);
using ScopeSetRegion_t = void(__fastcall*)(void* scope, void* region, bool clear);
using ScopeDtor_t = void(__fastcall*)(void* scope);
using EngineDelete_t = void(__fastcall*)(void* p, size_t size);

ScopeCopy_t g_scopeCopy = nullptr;
ScopeDtor_t g_scopeDtor = nullptr;
ScopeSetRegion_t g_scopeSetRegion = nullptr;
EngineDelete_t g_engineDelete = nullptr;

// CEventScope links (NOTES.md C1): ROOT +0x18, FROM +0x20, PREV +0x28.
const size_t kScopeRoot = 0x18, kScopeFrom = 0x20, kScopePrev = 0x28;
// Candidate list CPdxArray count (kCodeTargetEffectExecute) and the limit's child count
// (kCodeTargetEffectLimit), relative to the fields declared in engine.h.
const size_t kCandidateCount = 0xc, kTriggerChildCount = 0x14;

// Is there a `call target` (E8 rel32) in the first `len` bytes of fn? For cross-checks only.
bool CallsTarget(const uint8_t* fn, size_t len, const void* target) {
    for (size_t i = 0; i + 5 <= len; ++i)
        if (fn[i] == 0xE8 && fn + i + 5 + Rd<int32_t>(fn, i + 1) == target) return true;
    return false;
}

// The vanilla item handler's compare chain:
//   cmp r8d,<key>; je <handler>   (the key's handler is the jump target)
//   cmp r8d,<key>; jne <next>     (the key's handler follows; the chain continues at <next>)
//   ... mov r9d,<scope mask>; [add rsp,n; pop rbx]; jmp <child factory>
// with mov rax,rdx / mov rbx,rdx / push rbx / sub rsp,n in between. Any other instruction fails.
bool DecodeKeyChain(const uint8_t* p, const void* factory, IteratorTemplate* t, const uint8_t** handlers) {
    t->keyCount = 0;
    for (int steps = 0; steps < 48; ++steps) {
        if ((p[0] == 0x48 && p[1] == 0x8B && (p[2] == 0xC2 || p[2] == 0xDA))) { p += 3; continue; }
        if (p[0] == 0x40 && p[1] == 0x53) { p += 2; continue; }
        if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC) { p += 4; continue; }
        if (p[0] == 0x41 && p[1] == 0x81 && p[2] == 0xF8) {
            if (t->keyCount == 8) return false;
            int key = Rd<int>(p, 3);
            p += 7;
            if (p[0] == 0x74) { handlers[t->keyCount] = p + 2 + (int8_t)p[1]; p += 2; }
            else if (p[0] == 0x0F && p[1] == 0x84) { handlers[t->keyCount] = p + 6 + Rd<int32_t>(p, 2); p += 6; }
            else if (p[0] == 0x75) { handlers[t->keyCount] = p + 2; p = p + 2 + (int8_t)p[1]; }
            else if (p[0] == 0x0F && p[1] == 0x85) { handlers[t->keyCount] = p + 6; p = p + 6 + Rd<int32_t>(p, 2); }
            else return false;
            t->keys[t->keyCount++] = key;
            continue;
        }
        if (p[0] == 0x41 && p[1] == 0xB9) {  // mov r9d,<mask>: the fall-through to the child factory
            if (Rd<uint32_t>(p, 2) != 2) return false;  // the state scope (the template's elements)
            p += 6;
            if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xC4) p += 4;
            if (p[0] == 0x5B) p += 1;
            return p[0] == 0xE9 && p + 5 + Rd<int32_t>(p, 1) == factory && t->keyCount > 0;
        }
        return false;
    }
    return false;
}

bool ResolveIterator(const char* cls, const char* entryCls, size_t itemSlot, const void* childFactory, bool hasLimit,
                     IteratorTemplate* t) {
    t->vt.vt = FindVtableByRtti(cls, &t->vt.slots);
    size_t n = 0;
    t->entryVtable = FindVtableByRtti(entryCls, &n);
    if (!t->vt.vt || !t->entryVtable || n < 2 || t->vt.slots < 27) return false;
    const uint8_t* handlers[8] = {};
    if (!DecodeKeyChain((const uint8_t*)t->vt.vt[itemSlot], childFactory, t, handlers)) {
        Log("ERROR: region_scope: %s item handler (slot %zu) has an unexpected shape", cls, itemSlot);
        return false;
    }
    for (int i = 0; i < t->keyCount && hasLimit; ++i)
        if (SigMatchAt(handlers[i], kCodeTargetEffectLimit)) t->limitKey = t->keys[i];
    if (hasLimit && (!t->limitKey || t->vt.slots < 29 || !SigMatchAt((uint8_t*)t->vt.vt[13], kCodeTargetEffectExecute))) {
        Log("ERROR: region_scope: %s: limit handler or execute slot has an unexpected shape", cls);
        return false;
    }
    return true;
}

}  // namespace

void ResolveScopeBlocks() {
    andTrigger.vt = FindVtableByRtti(".?AVCAndTrigger@@", &andTrigger.slots);
    if (!triggerBase.vt || !effectBase.vt || !andTrigger.vt || andTrigger.slots < 23) return;
    bool ok = ResolveIterator(".?AVCAnyStateTrigger@@", ".?AV?$CTriggerEntry@VCAnyStateTrigger@@@@", 6, triggerBase.vt[6], false,
                              &anyStateTemplate) &&
              ResolveIterator(".?AVCEveryStateEffect@@", ".?AV?$CEffectEntry@VCEveryStateEffect@@@@", 4, effectBase.vt[4], true,
                              &everyStateTemplate) &&
              ResolveIterator(".?AVCRandomStateEffect@@", ".?AV?$CEffectEntry@VCRandomStateEffect@@@@", 4, effectBase.vt[4], true,
                              &randomStateTemplate);
    // Child scopes: the copy constructor any_state's evaluate calls; it must also call the
    // CEventScope destructor the loader already uses (player_scope).
    uint8_t* eval = ok ? (uint8_t*)anyStateTemplate.vt.vt[22] : nullptr;
    if (eval && SigMatchAt(eval, kCodeAnyStateEvaluate)) {
        g_scopeCopy = (ScopeCopy_t)SigFollowRel32(eval + kCodeAnyStateScopeCopyCall);
        if (!g_scopeCopy) ok = false;
    } else {
        ok = false;
    }
    g_scopeSetRegion = (ScopeSetRegion_t)SigFindUnique("scope_set_region", kSigScopeSetRegion);
    uint8_t* dtor = ok ? (uint8_t*)everyStateTemplate.vt.vt[0] : nullptr;
    if (dtor && SigMatchAt(dtor, kCodeScalarDeletingDtor)) g_engineDelete = (EngineDelete_t)SigFollowRel32(dtor + kCodeDtorDeleteCall);
    g_scopeDtor = (ScopeDtor_t)EventScopeDestructor();
    if (ok && (!g_scopeDtor || !CallsTarget(eval, 0x400, (void*)g_scopeDtor) || !scriptValueEval ||
               !CallsTarget(eval, 0x400, (void*)scriptValueEval))) {
        Log("ERROR: region_scope: any_state's evaluate does not use the known scope destructor and script value");
        ok = false;
    }
    if (!ok || !g_scopeSetRegion || !g_engineDelete) {
        anyStateTemplate = everyStateTemplate = randomStateTemplate = IteratorTemplate{};
        Log("region_scope: engine pieces missing (templates %d, set region %d, delete %d)", ok, g_scopeSetRegion != nullptr,
            g_engineDelete != nullptr);
        return;
    }
    Log("region_scope: scope copy %s, set region %s, any_state keys %d, every_state keys %d (limit 0x%x), random_state keys %d "
        "(limit 0x%x)",
        SigDescribe((void*)g_scopeCopy).c_str(), SigDescribe((void*)g_scopeSetRegion).c_str(), anyStateTemplate.keyCount,
        everyStateTemplate.keyCount, everyStateTemplate.limitKey, randomStateTemplate.keyCount, randomStateTemplate.limitKey);
}

bool HaveScopeBlocks() {
    return HaveNativeScript() && g_scopeCopy && g_scopeDtor && g_scopeSetRegion && g_engineDelete && anyStateTemplate.keyCount &&
           everyStateTemplate.limitKey && randomStateTemplate.limitKey && HaveRegions();
}

void ScopeBegin(void* buf, const void* parent) {
    g_scopeCopy(buf, parent);
    uint8_t* s = (uint8_t*)buf;
    *(void**)(s + kScopeRoot) = Rd<void*>(parent, kScopeRoot);
    *(void**)(s + kScopeFrom) = Rd<void*>(parent, kScopeFrom);
    *(const void**)(s + kScopePrev) = parent;
}

void ScopeSetRegion(void* scope, void* region) { g_scopeSetRegion(scope, region, true); }

void ScopeEnd(void* buf) { g_scopeDtor(buf); }

void FreeEngineString(EngineString* s) {
    // MSVC basic_string::_Tidy_deallocate with the engine's operator delete: buffers of 4 KB and
    // more are 32-byte aligned with the real block pointer stored just before them.
    if (s->cap > 15 && g_engineDelete) {
        uint8_t* p = (uint8_t*)s->ptr;
        size_t bytes = s->cap + 1;
        if (bytes >= 0x1000) {
            p = *(uint8_t**)(p - 8);
            bytes += 0x27;
        }
        g_engineDelete(p, bytes);
    }
    memset(s, 0, sizeof *s);
    s->cap = 15;
}

}  // namespace Engine
