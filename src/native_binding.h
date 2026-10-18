// Native script API, engine binding internals shared by native_script_engine.cpp (generic and
// numeric commands) and native_scope_engine.cpp (scope blocks and iterators). Slot numbers:
// NOTES.md C1 and F5.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>

#include "engine.h"
#include "native_script.h"

namespace NativeBinding {

// Generic commands keep their ParsedCommand* in the template's private area.
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

using Create_t = void*(__fastcall*)(void* entry);

ParsedCommand* Rec(void* self);
// A block key: the command's parameter, else "Unexpected token" (engine error, value skipped).
void ReadItemFor(ParsedCommand* pc, void* parser, int token);
EngineString* MakeTooltip(EngineString* out, const std::string& text);
// Vtable copy with the RTTI locator at [-1]; `tail` is stored at vt[tmpl.slots].
void** Clone(const Engine::Vtable& tmpl, void* tail = nullptr);
void** EntryVtable(void** templateEntryVt, void* create);

// Generic slots (native_script_engine.cpp).
bool __fastcall E_Validate(void* self);
bool __fastcall E_Value(void* self, const void* value);
uint64_t __fastcall E_Scopes(void* self);
bool __fastcall T_Value(void* self, const void* value);
bool __fastcall T_Validate(void* self);
uint64_t __fastcall T_Scopes(void* self);
uint64_t __fastcall T_Targets(void*);
void* __fastcall EffectEntryCreate(EffectEntryObj* e);
void* __fastcall TriggerEntryCreate(TriggerEntryObj* e);

// native_scope_engine.cpp.
bool BindScopeBlock(Command& c, std::string* reason);
bool BindIterator(Command& c, std::string* reason);
bool BindEffectBlock(Command& c, std::string* reason);

}  // namespace NativeBinding
