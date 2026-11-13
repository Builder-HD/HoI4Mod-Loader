// Engine access for air_scenarios (NOTES.md Part L): the game's air scenario templates, its own
// template loader, and the start of a scenario, where the loader may play another template.
// Graphics only: the game never writes game state from here.
#include <windows.h>
#include <string.h>

#include <string>

#include "engine.h"
#include "loader.h"
#include "signatures.h"

namespace Engine {

namespace {

template <typename T>
T Rd(const void* base, size_t offset) {
    T v;
    memcpy(&v, (const uint8_t*)base + offset, sizeof v);
    return v;
}

using Alloc_t = void*(__fastcall*)(size_t size);
using Loader_t = void*(__fastcall*)(void* tmpl, int slot);
using Database_t = void*(__fastcall*)();
using Lookup_t = void*(__fastcall*)(void* database, const char* name);
using Start_t = void(__fastcall*)(void* instance, void* a2, void* a3, void* attackers, void* defenders, void* a6);
using Creator_t = const int*(__fastcall*)(const void* design);

const int kMaxSlots = 64;

Alloc_t g_alloc = nullptr;
Loader_t g_loader = nullptr, g_loaderOrig = nullptr;
Database_t g_database = nullptr;
Lookup_t g_lookup = nullptr;
uint8_t* g_startFn = nullptr;
Start_t g_startOrig = nullptr;
Creator_t g_creator = nullptr;
const char** g_names = nullptr;  // the game's name table (in .data)
int g_slots = 0;
uint32_t g_templatesDisp = 0;  // manager -> its template array
uint32_t g_templateSize = 0;
uint32_t g_entity = 0, g_attackers = 0, g_defenders = 0;  // template fields
uint32_t g_instanceTemplate = 0;                            // instance -> its template
uint32_t g_designType = 0, g_typeName = 0;                  // design -> type, type -> name

void* g_vanilla[kMaxSlots] = {};
AirBuilt_t g_builtFn = nullptr;
AirPick_t g_pickFn = nullptr;
volatile LONG g_off = 0;           // a fault: the game's templates from now on
volatile LONG g_faultLogged = 0, g_threadLogged = 0;
bool g_building = false;           // inside AirBuildTemplate (main thread)

bool InSection(const char* name, const void* p, size_t len) {
    uint8_t* base = nullptr;
    size_t size = 0;
    return ImageSection(name, &base, &size) && (const uint8_t*)p >= base && (const uint8_t*)p + len <= base + size;
}

// The name table: every entry a short printable string in .rdata.
bool NameTableOk(const char** table, int count) {
    if (!InSection(".data", table, sizeof(char*) * (size_t)count)) return false;
    for (int i = 0; i < count; ++i) {
        const char* s = table[i];
        if (!InSection(".rdata", s, 1)) return false;
        size_t n = 0;
        while (n < 64 && InSection(".rdata", s + n, 1) && s[n] >= 0x21 && s[n] < 0x7f) ++n;
        if (n == 0 || n >= 64 || s[n] != 0) return false;
    }
    return true;
}

void LogFaultOnce(const char* where) {
    if (!InterlockedExchange(&g_faultLogged, 1))
        Log("ERROR: air_scenarios: fault in the %s; the game's own air scenarios from now on", where);
}

// --- Hooks ----------------------------------------------------------------------------------------

// The game's template loader: record the game's templates; after the last one, the plugin builds
// its own (the game builds all slots in order, 0 .. count-1, in the manager's constructor).
bool LoaderAfter(void* tmpl, int slot) {
    __try {
        if (slot >= 0 && slot < g_slots) g_vanilla[slot] = tmpl;
        return slot == g_slots - 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_off, 1);
        LogFaultOnce("template loader hook");
        return false;
    }
}
void CallBuiltSafe() {
    __try {
        g_builtFn();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_off, 1);
        LogFaultOnce("building the air scenario templates");
    }
}
void* __fastcall LoaderDetour(void* tmpl, int slot) {
    void* r = g_loaderOrig(tmpl, slot);
    if (!g_building && !g_off && LoaderAfter(tmpl, slot) && g_builtFn) CallBuiltSafe();
    return r;
}

void Pick(void* instance, void* attackers, void* defenders) {
    void* tmpl = Rd<void*>(instance, g_instanceTemplate);
    if (!tmpl) return;
    int slot = Rd<int>(tmpl, 0);
    // Only the game's own template for its slot is replaced (never one the loader put in).
    if (slot < 0 || slot >= g_slots || tmpl != g_vanilla[slot]) return;
    void* other = g_pickFn(slot, attackers, defenders);
    if (other && Rd<int>(other, 0) == slot) memcpy((uint8_t*)instance + g_instanceTemplate, &other, sizeof other);
}
void PickSafe(void* instance, void* attackers, void* defenders) {
    __try {
        Pick(instance, attackers, defenders);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_off, 1);
        LogFaultOnce("scenario start hook");
    }
}
void __fastcall StartDetour(void* instance, void* a2, void* a3, void* attackers, void* defenders, void* a6) {
    if (!g_off && g_pickFn && instance) {
        if (!InterlockedExchange(&g_threadLogged, 1))
            Log("air_scenarios: a scenario starts on thread %lu", GetCurrentThreadId());
        PickSafe(instance, attackers, defenders);
    }
    g_startOrig(instance, a2, a3, attackers, defenders, a6);
}

// The game's loader on our entity: the slot's name table entry points at our name (without
// "_entity", which the loader appends) for this one call, then it is restored.
void* BuildRaw(int slot, const char* baseName) {
    void* tmpl = g_alloc(g_templateSize);
    if (!tmpl) return nullptr;
    const char* saved = g_names[slot];
    g_building = true;
    __try {
        g_names[slot] = baseName;
        g_loaderOrig(tmpl, slot);
    } __finally {
        g_names[slot] = saved;
        g_building = false;
    }
    return tmpl;
}
void* BuildSafe(int slot, const char* baseName) {
    __try {
        return BuildRaw(slot, baseName);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool Fail(const char* what) {
    Log("ERROR: air_scenarios: %s; air scenarios unavailable", what);
    return false;
}

bool ResolveAll() {
    uint8_t* t = SigFindUnique("air_templates", kSigAirTemplates);
    uint8_t* s = SigFindUnique("air_spawn", kSigAirSpawn);
    if (!t || !s) return Fail("a signature is missing");
    uint8_t templates = Rd<uint8_t>(t, kSigAirTemplatesDisp);
    g_templatesDisp = templates;
    g_templateSize = Rd<uint32_t>(t, kSigAirTemplateSizeImm);
    g_alloc = (Alloc_t)SigFollowRel32(t + kSigAirAllocCall);
    g_loader = (Loader_t)SigFollowRel32(t + kSigAirLoaderCall);
    g_slots = Rd<uint8_t>(t, kSigAirSlotCountImm);
    if (g_slots < 1 || g_slots > kMaxSlots || g_templateSize < 0x30 || g_templateSize > 0x200)
        return Fail("the template loop has unexpected numbers");

    // The loader: template fields, the entity database, the name table, the entity lookup.
    uint8_t* head = SigFindIn((uint8_t*)g_loader, 0x100, kCodeAirLoaderHead);
    uint8_t* look = SigFindIn((uint8_t*)g_loader, 0x200, kCodeAirLoaderLookup);
    if (!head || !look) return Fail("the template loader has an unexpected shape");
    g_entity = Rd<uint8_t>(head, kCodeAirEntityDisp);
    g_attackers = Rd<uint8_t>(head, kCodeAirAttackersDisp);
    g_defenders = Rd<uint8_t>(head, kCodeAirDefendersDisp);
    g_database = (Database_t)SigFollowRel32(head + kCodeAirDatabaseCall);
    g_lookup = (Lookup_t)SigFollowRel32(look + kCodeAirLookupCall);
    uint8_t* imageBase = SigRipTarget(head + kCodeAirImageBaseLea, 3, 7);
    if (imageBase != (uint8_t*)GetModuleHandleW(nullptr)) return Fail("the name table is not image-relative");
    g_names = (const char**)(imageBase + Rd<uint32_t>(head, kCodeAirNameTableDisp));
    if (Rd<uint8_t>(look, kCodeAirLookupEntityDisp) != g_entity || g_attackers + 0x18 != g_defenders ||
        g_defenders + 0x10 > g_templateSize || !NameTableOk(g_names, g_slots))
        return Fail("the template loader's fields disagree");

    // A scenario: its instance holds the slot's template (from the manager's array), and the start
    // instantiates that template's entity.
    uint8_t* inst = SigFollowRel32(s + kSigAirSpawnInstanceCall);
    g_startFn = SigFollowRel32(s + kSigAirSpawnStartCall);
    uint8_t* ic = SigFindIn(inst, 0x60, kCodeAirInstance);
    if (!ic || !SigMatchAt(g_startFn, kCodeAirStart)) return Fail("the scenario instance or start has an unexpected shape");
    g_instanceTemplate = Rd<uint8_t>(ic, kCodeAirInstanceTemplateDisp);
    if (Rd<uint8_t>(ic, kCodeAirInstanceTemplatesDisp) != templates || Rd<uint8_t>(g_startFn, kCodeAirStartTemplateDisp) != g_instanceTemplate ||
        Rd<uint8_t>(g_startFn, kCodeAirStartEntityDisp) != g_entity)
        return Fail("the scenario instance, start and templates disagree");

    // The plane model per node: among the start's calls, the function that reads a design's
    // creator and equipment type.
    uint8_t* attach = nullptr;
    for (size_t i = 0; i + 5 <= 0x600 && !attach; ++i)
        if (g_startFn[i] == 0xE8 && SigMatchAt(SigFollowRel32(g_startFn + i), kCodeAirAttach)) attach = SigFollowRel32(g_startFn + i);
    if (!attach) return Fail("the plane model function was not found");
    uint8_t* creator = SigFollowRel32(attach + kCodeAirAttachCreatorCall);
    uint8_t* model = SigFollowRel32(attach + kCodeAirAttachModelCall);
    uint8_t* mt = SigFindIn(model, 0x400, kCodeAirModelType);
    if (!SigMatchAt(creator, kCodeAirCreator) || Rd<uint8_t>(creator, kCodeAirCreatorTagDisp) != Rd<uint8_t>(creator, kCodeAirCreatorTagDisp2) || !mt)
        return Fail("the plane's creator or equipment type has an unexpected shape");
    g_creator = (Creator_t)creator;
    g_designType = Rd<uint32_t>(mt, kCodeAirDesignTypeDisp);
    g_typeName = Rd<uint8_t>(mt, kCodeAirTypeNameDisp);
    if (EquipmentTypeOffset() && EquipmentTypeOffset() != g_designType)
        return Fail("the design's equipment type disagrees with production_data's");
    return true;
}

const char* TypeNameOf(const void* type) {
    const EngineString* s = (const EngineString*)((const uint8_t*)type + g_typeName);
    return s->size < 256 ? s->c_str() : "";
}

}  // namespace

void ResolveAirScenarios() {
    if (!ResolveAll()) {
        g_loader = nullptr;
        return;
    }
    Log("air_scenarios: %d slots (names %s), templates manager+0x%x (0x%x bytes; entity +0x%x, attackers +0x%x, defenders "
        "+0x%x), loader %s, start %s, instance template +0x%x, design type +0x%x, type name +0x%x",
        g_slots, SigDescribe(g_names).c_str(), g_templatesDisp, g_templateSize, g_entity, g_attackers, g_defenders, SigDescribe((void*)g_loader).c_str(),
        SigDescribe(g_startFn).c_str(), g_instanceTemplate, g_designType, g_typeName);
}

bool HaveAirScenarios() {
    return g_loader && g_alloc && g_database && g_lookup && g_startFn && g_creator && g_names && g_slots && g_entity &&
           g_attackers && g_defenders && g_instanceTemplate && g_designType && g_typeName;
}

int AirSlotCount() { return g_slots; }
const char* AirSlotName(int slot) { return slot >= 0 && slot < g_slots ? g_names[slot] : ""; }

bool AirEntityExists(const char* name) {
    void* db = g_database ? g_database() : nullptr;
    return db && name && *name && g_lookup(db, name) != nullptr;
}

void* AirVanillaTemplate(int slot) { return slot >= 0 && slot < g_slots ? g_vanilla[slot] : nullptr; }

int AirTemplateNodes(const void* tmpl, bool defenders) {
    return tmpl ? Rd<int>(tmpl, (defenders ? g_defenders : g_attackers) + 0xc) : 0;
}
const void* AirTemplateEntity(const void* tmpl) { return tmpl ? Rd<const void*>(tmpl, g_entity) : nullptr; }

void* AirBuildTemplate(int slot, const std::string& entity) {
    static const char kSuffix[] = "_entity";
    if (slot < 0 || slot >= g_slots || entity.size() <= sizeof kSuffix - 1) return nullptr;
    // The name table points at it only during the build; the loader copies the name.
    std::string base = entity.substr(0, entity.size() - (sizeof kSuffix - 1));
    void* tmpl = BuildSafe(slot, base.c_str());
    if (!tmpl) Log("ERROR: air_scenarios: building a template for %s faulted", entity.c_str());
    return tmpl;
}

int AirListCount(const void* list) {
    if (!list) return 0;
    int n = Rd<int>(list, 0xc);
    return n > 0 && n < 4096 && Rd<const void*>(list, 0) ? n : 0;
}
const void* AirListDesign(const void* list, int i) { return Rd<const void* const*>(list, 0)[i]; }

bool AirReadDesign(const void* design, std::string* equipment, std::string* archetype, int* creatorTag) {
    equipment->clear();
    archetype->clear();
    *creatorTag = 0;
    const void* type = design ? Rd<const void*>(design, g_designType) : nullptr;
    if (!type) return false;
    *equipment = TypeNameOf(type);
    if (uint32_t a = TypeArchetypeOffset()) {
        const void* arch = Rd<const void*>(type, a);
        if (arch && arch != type) *archetype = TypeNameOf(arch);
    }
    const int* tag = g_creator(design);
    if (tag && *tag > 0) *creatorTag = *tag;
    return !equipment->empty();
}

bool HookAirScenarios(AirBuilt_t built, AirPick_t pick) {
    g_builtFn = built;
    g_pickFn = pick;
    if (!HookQueue((void*)g_loader, (void*)&LoaderDetour, (void**)&g_loaderOrig, "air_template_loader")) return false;
    if (!HookQueue(g_startFn, (void*)&StartDetour, (void**)&g_startOrig, "air_scenario_start")) {
        HookUnqueue((void*)g_loader);
        return false;
    }
    return true;
}

void DisableAirScenarios() { InterlockedExchange(&g_off, 1); }

}  // namespace Engine
