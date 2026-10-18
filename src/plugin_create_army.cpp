// create_army (engine tier): spawn divisions as a ready army under a chosen general (NOTES.md G1).
//
//   64 = {                            # a state scope, as vanilla create_unit needs one
//       create_army = {
//           leader = GER_erwin_rommel   # character id: an army leader recruited by the country
//           name = "1. Panzerarmee"     # optional army name
//           reassign = yes              # optional: take the general even from the army they lead now
//           country = GER               # optional: the army's country (default ROOT)
//           create_unit = { ... }       # vanilla create_unit, any number; nothing else is allowed inside
//       }
//   }
//
// The general is checked first; if not available, nothing spawns. Then the create_unit
// children run in the state scope (where they place the divisions), the country's division list
// before and after gives the new divisions, and the game's own "create army" and "set leader"
// functions (as vanilla start_border_war calls them) make them an army under that general. Game
// state writes, main thread, deterministic: the same effect gives the same army on every machine.
// No command power is charged.
#include <windows.h>

#include <atomic>
#include <string>
#include <unordered_set>
#include <vector>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "native_script.h"

namespace {

const char* const kFeature = "create_army";
std::atomic<bool> g_ok{true};

// The vanilla soft cap (NMilitary CORPS_COMMANDER_DIVISIONS_CAP / FIELD_MARSHAL_DIVISIONS_CAP):
// more divisions are allowed but reduce the general's bonuses. Only used for the log line.
const size_t kVanillaDivisionCap = 24;

const LocText kCreate{"HOI4_LOADER_CREATE_ARMY", "Creates a new army led by $LEADER|Y$."};
const LocText kCreateNamed{"HOI4_LOADER_CREATE_ARMY_NAMED", "Creates the army $NAME|Y$ led by $LEADER|Y$."};
const LocText kLeaves{"HOI4_LOADER_CREATE_ARMY_LEAVES", "$LEADER|Y$ leaves $ARMY|Y$, which keeps its divisions without a general."};
const LocText kUnavailable{"HOI4_LOADER_CREATE_ARMY_UNAVAILABLE",
                           "\x11R$LEADER$ cannot lead a new army: $REASON$. Nothing is created.\x11!"};

enum class Refusal { None, UnknownCharacter, NotRecruited, OtherCountry, NotArmyLeader, Captured, LeadsArmyGroup, LeadsArmy, CannotLeave };

const LocText kReasons[] = {
    {"HOI4_LOADER_CREATE_ARMY_OK", "available"},
    {"HOI4_LOADER_CREATE_ARMY_UNKNOWN", "there is no such character"},
    {"HOI4_LOADER_CREATE_ARMY_NOT_RECRUITED", "not recruited"},
    {"HOI4_LOADER_CREATE_ARMY_OTHER_COUNTRY", "serves another country"},
    {"HOI4_LOADER_CREATE_ARMY_NOT_ARMY_LEADER", "not a general or field marshal"},
    {"HOI4_LOADER_CREATE_ARMY_CAPTURED", "captured"},
    {"HOI4_LOADER_CREATE_ARMY_LEADS_ARMY_GROUP", "leads an army group"},
    {"HOI4_LOADER_CREATE_ARMY_LEADS_ARMY", "already leads an army"},
    {"HOI4_LOADER_CREATE_ARMY_CANNOT_LEAVE", "the headquarters of their current army is deploying or withdrawing"},
};
const LocText& ReasonText(Refusal r) { return kReasons[(int)r]; }

struct Candidate {
    void* leader = nullptr;
    Engine::LeaderState state;
    std::string name;  // display name (localised if the name is a key)
};

// Who `leader` is and whether that general may lead a new army of `tag` now. Reads only.
Refusal Check(const Args& a, int tag, Candidate* c) {
    void* character = Engine::CharacterByToken(a.i(0));
    c->name = native_localize_key(a.key(0));
    if (!character) return Refusal::UnknownCharacter;
    void* leader = Engine::ArmyLeaderRole(character);
    if (leader) {
        std::string n = Engine::LeaderName(leader);
        if (!n.empty()) c->name = native_localize_key(n.c_str());
    }
    int owner = Engine::CharacterCountry(character);
    if (owner <= 0) return Refusal::NotRecruited;
    if (owner != tag) return Refusal::OtherCountry;
    if (!leader || !Engine::ReadLeader(leader, &c->state) || c->state.roleType < 0 || c->state.roleType >= 2)
        return Refusal::NotArmyLeader;
    c->leader = leader;
    if (c->state.captured) return Refusal::Captured;
    if (c->state.leads && c->state.leadsArmyGroup) return Refusal::LeadsArmyGroup;
    if (c->state.leads && !a.b(2)) return Refusal::LeadsArmy;
    if (c->state.leads && !c->state.canLeave) return Refusal::CannotLeave;
    return Refusal::None;
}

void Execute(const Args& a, const Scope&, const EffectChildren& children) {
    if (!g_ok.load()) return;
    int tag = a.i(3);  // country (ROOT by default), resolved by the core
    std::string country = Engine::CountryTagName(tag);
    Candidate c;
    Refusal r = Check(a, tag, &c);
    if (r != Refusal::None) {
        Log("create_army (%s): %s (%s) cannot lead a new army: %s; nothing created", country.c_str(), c.name.c_str(), a.key(0),
            ReasonText(r).english);
        return;
    }
    std::vector<void*> before, after;
    if (!Engine::CountryDivisionList(tag, &before)) {
        Log("create_army (%s): the country's divisions are unreadable; nothing created", country.c_str());
        return;
    }
    std::string previous = c.state.leads ? Engine::ArmyName(c.state.leads) : std::string();
    bool childrenOk = children.run();
    Engine::CountryDivisionList(tag, &after);
    std::unordered_set<void*> known(before.begin(), before.end());
    std::vector<void*> spawned;
    for (void* unit : after)  // the engine's list order, the same on every machine
        if (!known.count(unit)) spawned.push_back(unit);
    if (!childrenOk) {
        Log("create_army (%s): the create_unit blocks crashed; %zu division(s) created stay without an army", country.c_str(),
            spawned.size());
        return;
    }
    if (spawned.empty()) {
        Log("create_army (%s): create_unit created no division (see error.log for its reason); no army, %s unchanged",
            country.c_str(), c.name.c_str());
        return;
    }
    void* army = Engine::CreateArmy(tag, spawned);
    if (!army) {
        Log("create_army (%s): the game did not create the army; %zu division(s) stay without an army", country.c_str(),
            spawned.size());
        return;
    }
    if (a.has(1)) Engine::SetArmyName(army, a.key(1));
    bool led = Engine::SetArmyLeader(army, c.leader);
    Log("create_army (%s): army \"%s\" with %zu division(s) under %s%s%s%s", country.c_str(), Engine::ArmyName(army).c_str(),
        spawned.size(), c.name.c_str(), led ? "" : " (FAILED to set the general)",
        previous.empty() ? "" : ("; the general left \"" + previous + "\", which keeps its divisions").c_str(),
        spawned.size() > kVanillaDivisionCap ? "; more than the vanilla 24-division cap: the general's bonuses are reduced" : "");
}

void Tooltip(const Args& a, const Scope&, TooltipText& out) {
    if (!g_ok.load()) return;
    Candidate c;
    Refusal r = Check(a, a.i(3), &c);
    if (r != Refusal::None) {
        out.add(native_localize(kUnavailable, {{"LEADER", c.name}, {"REASON", native_localize(ReasonText(r))}}).c_str());
        return;
    }
    out.add(native_localize(a.has(1) ? kCreateNamed : kCreate, {{"LEADER", c.name}, {"NAME", a.has(1) ? a.key(1) : ""}}).c_str());
    if (c.state.leads)
        out.add(" ").add(native_localize(kLeaves, {{"LEADER", c.name}, {"ARMY", Engine::ArmyName(c.state.leads)}}).c_str());
}

// --- Self-check on every game entry (main thread) ----------------------------------------------
bool SafeSelfCheck(int tag, int* divisions, const char** why, bool* ok) {
    __try {
        *ok = Engine::ArmySelfCheck(tag, divisions, why);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "crashed";
        return false;
    }
}

void OnEntered(bool) {
    int tag = Engine::PlayerCountryTag();
    if (tag <= 0) return;  // observer: nothing to check against
    int divisions = 0;
    const char* why = "";
    bool ok = false;
    if (!SafeSelfCheck(tag, &divisions, &why, &ok) || !ok) {
        g_ok.store(false);
        Log("create_army: self-check failed for %s (%s)", Engine::CountryTagName(tag).c_str(), why);
        FeatureDeactivate(kFeature, "self-check failed: army data unreadable");
        return;
    }
    g_ok.store(true);
    Log("create_army: self-check passed (%s: army manager and %d division handle(s))", Engine::CountryTagName(tag).c_str(),
        divisions);
}

}  // namespace

bool CreateArmyInit() {
    if (!Engine::HaveArmies() || !Engine::HaveScopeBlocks()) {
        Log("create_army: required engine functions missing (armies %d, effect blocks %d)", Engine::HaveArmies(),
            Engine::HaveScopeBlocks());
        return false;
    }
    std::vector<LocText> texts = {kCreate, kCreateNamed, kLeaves, kUnavailable};
    for (const LocText& t : kReasons)
        if (&t != &kReasons[0]) texts.push_back(t);
    register_native_effect_block(
        {"create_army",
         "Use it in a state scope, as vanilla create_unit (e.g. `64 = { create_army = { ... } }`): the create_unit "
         "blocks inside spawn their divisions in that state, and the divisions created for `country` (default ROOT) "
         "become a new army of that country led by `leader` (a character id), named `name` if given. The general must "
         "be an army leader (general or field marshal) recruited by that country, not captured and not leading an "
         "army group. If the general already leads an army, nothing happens unless `reassign = yes`: then the "
         "general leaves it and it keeps its divisions without a general. If the general is not available, nothing "
         "spawns (logged in loader.log; the tooltip shows the reason in red). No command power is charged; add vanilla "
         "add_command_power in the same effect block for a cost.",
         {{"leader", ParamType::Token, true},
          {"name", ParamType::Text, false},
          {"reassign", ParamType::Bool, false},
          {"country", ParamType::Country, false, ScopeDefault::RootCountry}},
         ScopeMask::State,
         {"create_unit"},
         &Execute,
         &Tooltip,
         texts},
        kFeature);
    OnGameEntered(&OnEntered);
    return true;
}
