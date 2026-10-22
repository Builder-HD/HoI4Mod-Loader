// Engine access for create_army (NOTES.md G1): armies (COrdersGroup), their generals and the
// game's own "create army" and "set leader" functions, called the way vanilla's
// start_border_war effect calls them. Resolved once from RTTI and src/signatures.h, with every
// function and offset found in two places where the game uses it twice.
#include <string.h>

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

using Resolve_t = void*(__fastcall*)(const void* handle);  // reference handle -> object (null if gone)
using CreateArmy_t = void*(__fastcall*)(const void* theatre, const void* manager, const void* units, bool keepEmpty);
using SetLeader_t = void(__fastcall*)(void* army, void* leader, bool link);
using CanChange_t = bool(__fastcall*)(void* army, void* reasonOut);
using FindCharacter_t = void*(__fastcall*)(void* characters, uint32_t token);
using RoleOf_t = void*(__fastcall*)(const void* character);
using LeaderVirtual_t = uint64_t(__fastcall*)(void* leader);
using Assign_t = void*(__fastcall*)(EngineString* dst, const char* p, size_t n);
using CountryCharacters_t = void*(__fastcall*)(void* country);
using FindInCountry_t = void*(__fastcall*)(void* characters, uint32_t token, bool flag);

Resolve_t g_resolve = nullptr;
CreateArmy_t g_createArmy = nullptr;
SetLeader_t g_setLeader = nullptr;
CanChange_t g_canChange = nullptr;
FindCharacter_t g_findCharacter = nullptr;
RoleOf_t g_roleOf = nullptr;
Assign_t g_assign = nullptr;
CountryCharacters_t g_countryCharacters = nullptr;
FindInCountry_t g_findInCountry = nullptr;
void** g_gameStateSlot = nullptr;
const uint64_t* g_nullHandle = nullptr;
uint32_t g_manager = 0;         // country -> army manager array (its first element)
uint32_t g_characters = 0;      // gamestate -> character manager
uint32_t g_characterOwner = 0;  // character -> recruiting country's tag
uint32_t g_roleType = 0;        // leader -> 0 field marshal, 1 corps commander, ...
uint32_t g_ledGroup = 0;        // leader -> handle of the army / army group it leads
uint32_t g_leaderName = 0;      // leader -> name (std::string)
uint32_t g_armyLeader = 0;      // army -> leader
uint32_t g_armyName = 0;        // army -> name (std::string)
uint8_t g_armyGroupFlag = 0;    // army -> "is an army group" byte
uint32_t g_capturedSlot = 0;    // leader vtable byte offset: captured
uint32_t g_refreshSlot = 0;     // leader vtable byte offset: refresh after start_border_war's set_leader
// army -> the fields can_change_leader refuses on (0 = not found: the reason is then Unknown)
uint32_t g_borderWar = 0, g_deploying = 0, g_deploying2 = 0, g_withdrawing = 0;

// Fixed by the engine's reference objects: the manager's tag (create_army compares units' tags
// with it) and a division's own handle, which resolves to the division + 0x10. Both are checked
// at runtime before use.
const size_t kManagerTag = 0x48, kHandle = 8, kUnitHandle = 0x18, kUnitRef = 0x10;

bool CallsTarget(const uint8_t* fn, size_t len, const void* target) {
    for (size_t i = 0; i + 5 <= len; ++i)
        if (fn[i] == 0xE8 && fn + i + 5 + Rd<int32_t>(fn, i + 1) == target) return true;
    return false;
}

uint8_t* Slot(const char* rtti, size_t slot) {
    size_t n = 0;
    void** vt = FindVtableByRtti(rtti, &n);
    return vt && n > slot ? (uint8_t*)vt[slot] : nullptr;
}

bool Fail(const char* what) {
    Log("ERROR: create_army: %s; armies unavailable", what);
    return false;
}

// The fields can_change_leader refuses on, from its own code. Only for the refusal's message: if
// they are not found, create_army still works and names no specific reason.
void ResolveLeaveFields() {
    const uint8_t* fn = (const uint8_t*)g_canChange;
    const uint8_t* dep = SigMatchAt(fn, kCodeCanChangeStart) ? SigFindIn(fn + 0x40, 0x100, kCodeCanChangeDeploying) : nullptr;
    const uint8_t* wd = dep ? SigFindIn(dep + 0x11, 0xc0, kCodeCanChangeWithdrawing) : nullptr;
    uint32_t war = wd ? Rd<uint32_t>(fn, kCodeCanChangeWarDisp) : 0, d1 = dep ? Rd<uint32_t>(dep, kCodeCanChangeDeployDisp) : 0,
             d2 = dep ? Rd<uint32_t>(dep, kCodeCanChangeDeploy2Disp) : 0, w = wd ? Rd<uint32_t>(wd, kCodeCanChangeWithdrawDisp) : 0;
    // Cross-checks: the army group flag and the leader field it reads are the ones found elsewhere,
    // and the fields lie inside an army (0x230 bytes) and differ.
    bool ok = wd && Rd<uint8_t>(fn, kCodeCanChangeGroupDisp) == g_armyGroupFlag &&
              Rd<uint32_t>(fn, kCodeCanChangeLeaderDisp) == g_armyLeader && war < 0x230 && d1 < 0x230 && d2 < 0x230 && w < 0x230 &&
              d1 != d2 && w != d1 && w != d2 && war > 0x40;
    if (!ok) {
        Log("create_army: can_change_leader's refusal fields not found; a refused general is reported without the "
            "specific reason");
        return;
    }
    g_borderWar = war;
    g_deploying = d1;
    g_deploying2 = d2;
    g_withdrawing = w;
}

// Mirrors can_change_leader's order: border war, deploying, withdrawing.
LeaveBlock WhyNot(const void* army) {
    if (!g_borderWar) return LeaveBlock::Unknown;
    if (Rd<uint64_t>(army, g_borderWar) != 0 && Rd<void*>(army, g_armyLeader)) return LeaveBlock::BorderWar;
    if (Rd<uint8_t>(army, g_deploying) || Rd<uint8_t>(army, g_deploying2)) return LeaveBlock::Deploying;
    if (Rd<uint8_t>(army, g_withdrawing)) return LeaveBlock::Withdrawing;
    return LeaveBlock::Unknown;
}

bool ResolveAll() {
    uint8_t* p = SigFindUnique("border_war_manager", kSigBorderWarManager);
    if (!p) return Fail("army manager signature not found");
    g_manager = Rd<uint32_t>(p, kSigBorderWarManagerDisp);
    g_nullHandle = (const uint64_t*)SigRipTarget(p + kSigBorderWarNullHandle, 3, 7);
    uint8_t* army = SigFindUnique("border_war_army", kSigBorderWarArmy);
    if (!army) return Fail("create army signature not found");
    g_createArmy = (CreateArmy_t)SigFollowRel32(army + kSigBorderWarCreateCall);
    uint8_t* sl = SigFindIn(army + kSigBorderWarCreateCall, 0x40, kCodeBorderWarSetLeader);
    if (!sl) return Fail("start_border_war's set_leader call has an unexpected shape");
    g_setLeader = (SetLeader_t)SigFollowRel32(sl + kCodeBorderWarSetLeaderCall);
    g_refreshSlot = Rd<uint32_t>(sl, kCodeBorderWarLeaderSlotDisp);

    // The UI's create army must call the same function; its execute also gives the resolver.
    uint8_t* order = Slot(".?AVCOrderGroupCommand@@", 10);
    if (!order || !SigMatchAt(order, kCodeOrderGroupExecute)) return Fail("COrderGroupCommand execute has an unexpected shape");
    g_resolve = (Resolve_t)SigFollowRel32(order + kCodeOrderGroupResolveCall);
    uint8_t* oc = SigFindIn(order, 0x100, kCodeOrderGroupCreate);
    if (!oc || (CreateArmy_t)SigFollowRel32(oc + kCodeOrderGroupCreateCall) != g_createArmy)
        return Fail("the UI's create army and start_border_war call different functions");

    // The UI's set leader: can-change check, and its execute calls the same set_leader.
    uint8_t* check = Slot(".?AVCSetArmyLeaderCommand@@", 9);
    uint8_t* exec = Slot(".?AVCSetArmyLeaderCommand@@", 10);
    if (!check || !exec || !SigMatchAt(check, kCodeSetLeaderCheck) ||
        (Resolve_t)SigFollowRel32(check + kCodeSetLeaderCheckResolveCall) != g_resolve ||
        !CallsTarget(exec, 0x300, (void*)g_setLeader))
        return Fail("CSetArmyLeaderCommand does not match start_border_war's set_leader");
    g_canChange = (CanChange_t)SigFollowRel32(check + kCodeSetLeaderCheckCanChangeCall);
    if (!SigMatchAt((uint8_t*)g_setLeader, kCodeSetLeaderStart) ||
        Rd<uint8_t>(g_setLeader, kCodeSetLeaderNameCapDisp) != Rd<uint8_t>(g_setLeader, kCodeSetLeaderNameDisp) + 0x18)
        return Fail("set_leader has an unexpected shape");
    g_leaderName = Rd<uint8_t>(g_setLeader, kCodeSetLeaderNameDisp);
    g_armyLeader = Rd<uint32_t>(g_setLeader, kCodeSetLeaderArmyLeaderDisp);

    // Leader fields from the vanilla leader triggers.
    uint8_t* isArmy = Slot(".?AVCIsArmyLeader@@", 22);
    uint8_t* leading = Slot(".?AVCIsLeadingArmy@@", 22);
    uint8_t* captured = Slot(".?AVCIsGeneralCapturedTrigger@@", 22);
    if (!isArmy || !leading || !captured || !SigMatchAt(isArmy, kCodeIsArmyLeaderEval) ||
        !SigMatchAt(leading, kCodeIsLeadingArmyEval) || !SigMatchAt(captured, kCodeIsCapturedEval))
        return Fail("a leader trigger has an unexpected shape");
    g_roleOf = (RoleOf_t)SigFollowRel32(isArmy + kCodeIsArmyLeaderRoleCall);
    g_roleType = Rd<uint32_t>(isArmy, kCodeIsArmyLeaderTypeDisp);
    if (Rd<uint32_t>(leading, kCodeLeadingTypeDisp) != g_roleType ||
        (Resolve_t)SigFollowRel32(leading + kCodeLeadingResolveCall) != g_resolve)
        return Fail("is_leading_army disagrees with is_army_leader / the resolver");
    g_ledGroup = Rd<uint32_t>(leading, kCodeLeadingGroupDisp);
    g_armyGroupFlag = Rd<uint8_t>(leading, kCodeLeadingArmyGroupDisp);
    g_capturedSlot = Rd<uint32_t>(captured, kCodeIsCapturedSlotDisp);

    // Characters by token, and their recruiting country, from recruit_character.
    uint8_t* recruit = Slot(".?AVCRecruitCharacterEffect@@", 13);
    uint8_t* look = recruit ? SigFindIn(recruit, 0x200, kCodeCharacterLookup) : nullptr;
    uint8_t* own = recruit ? SigFindIn(recruit, 0x400, kCodeCharacterOwnerCheck) : nullptr;
    uint8_t* getter = own ? SigFollowRel32(own + kCodeCharacterOwnerCall) : nullptr;
    if (!look || !getter || !SigMatchAt(getter, kCodeCharacterOwnerGetter)) return Fail("recruit_character has an unexpected shape");
    g_gameStateSlot = (void**)SigRipTarget(look, kCodeCharacterGameStateRel, 7);
    if (g_gameStateSlot != GameStateSlot()) return Fail("recruit_character reads another gamestate pointer");
    g_characters = Rd<uint32_t>(look, kCodeCharacterManagerDisp);
    g_findCharacter = (FindCharacter_t)SigFollowRel32(look + kCodeCharacterFindCall);
    g_characterOwner = Rd<uint8_t>(getter, 3);

    // A country's own characters by token, as has_character finds them (works after loading a
    // save, unlike recruit_character's table): has_character's target resolver calls a function
    // holding the lookup.
    uint8_t* has = Slot(".?AVCHasCharacterTrigger@@", 22);
    if (!has || !SigMatchAt(has, kCodeHasCharacterEval)) return Fail("has_character has an unexpected shape");
    uint8_t* resolver = SigFollowRel32(has + kCodeHasCharacterResolveCall);
    uint8_t* lookup = nullptr;
    for (size_t i = 0; i + 5 <= 0xa0 && !lookup; ++i)
        if (resolver[i] == 0xE8) lookup = SigFindIn(SigFollowRel32(resolver + i), 0x100, kCodeCountryCharacterLookup);
    if (!lookup) return Fail("has_character's country character lookup not found");
    g_countryCharacters = (CountryCharacters_t)SigFollowRel32(lookup + kCodeCountryCharactersCall);
    g_findInCountry = (FindInCountry_t)SigFollowRel32(lookup + kCodeCountryCharacterFindCall);

    // Army names, from the rename command (which resolves with the same resolver).
    uint8_t* rename = Slot(".?AVCSetOrderGroupNameCommand@@", 10);
    if (!rename || !SigMatchAt(rename, kCodeSetArmyName) || (Resolve_t)SigFollowRel32(rename + kCodeSetArmyNameResolveCall) != g_resolve)
        return Fail("CSetOrderGroupNameCommand has an unexpected shape");
    g_armyName = Rd<uint32_t>(rename, kCodeSetArmyNameDisp);
    g_assign = (Assign_t)SigFollowRel32(rename + kCodeSetArmyNameAssign);
    ResolveLeaveFields();
    return true;
}

// The manager's handle for this country, checked against its tag.
const void* ManagerHandle(int tag) {
    uint8_t* country = (uint8_t*)CountryByTag(tag);
    void** arr = country ? Rd<void**>(country, g_manager) : nullptr;
    uint8_t* mgr = arr ? (uint8_t*)arr[0] : nullptr;
    if (!mgr || Rd<int>(mgr, kManagerTag) != tag) return nullptr;
    return mgr + kHandle;
}

}  // namespace

void ResolveArmies() {
    if (!ResolveAll()) {
        g_createArmy = nullptr;
        return;
    }
    Log("create_army: create %s, set leader %s, can change %s, find character %s, in country %s; manager +0x%x, characters +0x%x, owner "
        "+0x%x, role type +0x%x, led group +0x%x, army group flag +0x%x, leader name +0x%x, army leader +0x%x, army name "
        "+0x%x, captured slot +0x%x, refresh slot +0x%x",
        SigDescribe((void*)g_createArmy).c_str(), SigDescribe((void*)g_setLeader).c_str(), SigDescribe((void*)g_canChange).c_str(),
        SigDescribe((void*)g_findCharacter).c_str(), SigDescribe((void*)g_findInCountry).c_str(), g_manager, g_characters, g_characterOwner, g_roleType, g_ledGroup,
        g_armyGroupFlag, g_leaderName, g_armyLeader, g_armyName, g_capturedSlot, g_refreshSlot);
    if (g_borderWar)
        Log("create_army: leader change refused on: border war +0x%x, deploying +0x%x / +0x%x, withdrawing +0x%x", g_borderWar,
            g_deploying, g_deploying2, g_withdrawing);
}

bool HaveArmies() {
    return g_createArmy && g_setLeader && g_canChange && g_resolve && g_findCharacter && g_roleOf && g_assign && g_nullHandle &&
           g_countryCharacters && g_findInCountry && HaveCountryDivisions();
}

void* CharacterByToken(int token) {
    void* gs = g_gameStateSlot ? *g_gameStateSlot : nullptr;
    void* characters = gs ? Rd<void*>(gs, g_characters) : nullptr;
    return characters && token > 0 ? g_findCharacter(characters, (uint32_t)token) : nullptr;
}

void* CountryCharacterByToken(int tag, int token) {
    void* country = token > 0 ? CountryByTag(tag) : nullptr;
    void* characters = country ? g_countryCharacters(country) : nullptr;
    return characters ? g_findInCountry(characters, (uint32_t)token, true) : nullptr;
}

int CharacterCountry(const void* character) { return character ? Rd<int>(character, g_characterOwner) : 0; }

void* ArmyLeaderRole(const void* character) { return character ? g_roleOf(character) : nullptr; }

bool ReadLeader(void* leader, LeaderState* out) {
    *out = LeaderState{};
    if (!leader) return false;
    out->roleType = Rd<int>(leader, g_roleType);
    void** vt = *(void***)leader;
    out->captured = (((LeaderVirtual_t)vt[g_capturedSlot / 8])(leader) & 0xff) != 0;
    const uint8_t* handle = (const uint8_t*)leader + g_ledGroup;
    if (Rd<uint64_t>(handle, 0) != 0) out->leads = g_resolve(handle);
    if (out->leads) {
        out->leadsArmyGroup = Rd<uint8_t>(out->leads, g_armyGroupFlag) != 0;
        out->canLeave = out->leadsArmyGroup || g_canChange(out->leads, nullptr);
        if (!out->canLeave) out->whyNot = WhyNot(out->leads);
    }
    return true;
}

std::string LeaderName(const void* leader) {
    if (!leader) return "";
    const EngineString* s = (const EngineString*)((const uint8_t*)leader + g_leaderName);
    return s->size < 1024 ? std::string(s->c_str(), (size_t)s->size) : std::string();
}

void* CreateArmy(int tag, const std::vector<void*>& divisions) {
    const void* manager = ManagerHandle(tag);
    if (!manager) {
        Log("create_army: the army manager of %s is unreadable; no army created", CountryTagName(tag).c_str());
        return nullptr;
    }
    std::vector<uint64_t> handles;
    for (void* unit : divisions) {
        const uint8_t* h = (const uint8_t*)unit + kUnitHandle;
        if (UnitOwnerTag(unit) == tag && g_resolve(h) == (uint8_t*)unit + kUnitRef) handles.push_back(Rd<uint64_t>(h, 0));
    }
    if (handles.size() != divisions.size()) {
        Log("create_army: %zu of %zu divisions failed the handle or owner check; no army created", divisions.size() - handles.size(),
            divisions.size());
        return nullptr;
    }
    // The engine's CPdxArray { data, capacity, count, allocator }; create_army only reads it.
    struct {
        uint64_t* data;
        int cap;
        int count;
        void* allocator;
    } units{handles.data(), (int)handles.size(), (int)handles.size(), nullptr};
    uint64_t theatre = *g_nullHandle;
    return g_createArmy(&theatre, manager, &units, false);
}

bool SetArmyLeader(void* army, void* leader) {
    g_setLeader(army, leader, true);
    void** vt = *(void***)leader;
    ((LeaderVirtual_t)vt[g_refreshSlot / 8])(leader);
    return Rd<void*>(army, g_armyLeader) == leader;
}

void SetArmyName(void* army, const char* name) {
    g_assign((EngineString*)((uint8_t*)army + g_armyName), name, strlen(name));
}

std::string ArmyName(const void* army) {
    if (!army) return "";
    const EngineString* s = (const EngineString*)((const uint8_t*)army + g_armyName);
    return s->size < 1024 ? std::string(s->c_str(), (size_t)s->size) : std::string();
}

bool ArmySelfCheck(int tag, int* divisions, const char** why) {
    *divisions = 0;
    if (!ManagerHandle(tag)) {
        *why = "the country's army manager does not carry its tag";
        return false;
    }
    std::vector<void*> units;
    if (!CountryDivisionList(tag, &units)) {
        *why = "the country's division list is unreadable";
        return false;
    }
    for (void* unit : units) {
        if (UnitOwnerTag(unit) != tag || g_resolve((const uint8_t*)unit + kUnitHandle) != (uint8_t*)unit + kUnitRef) {
            *why = "a division's handle does not resolve back to it";
            return false;
        }
        ++*divisions;
    }
    return true;
}

}  // namespace Engine
