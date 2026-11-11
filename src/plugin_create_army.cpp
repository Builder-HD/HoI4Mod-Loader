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

// CannotLeave .. BorderWar: the game's own can-change-leader check refuses for the army the
// general leads now; the last three name its reason as vanilla's CANNOT_CHANGE_LEADER_* texts do.
enum class Refusal {
    None,
    UnknownCharacter,
    NotRecruited,
    OtherCountry,
    NotArmyLeader,
    Captured,
    LeadsArmyGroup,
    LeadsArmy,
    CannotLeave,
    Deploying,
    Withdrawing,
    BorderWar,
    NotDivision,        // add_to_army: the scope holds no land division
    DivisionBorderWar,  // add_to_army: the division's own army is in a border war
};

const LocText kReasons[] = {
    {"HOI4_LOADER_CREATE_ARMY_OK", "available"},
    {"HOI4_LOADER_CREATE_ARMY_UNKNOWN", "not a character of this country"},
    {"HOI4_LOADER_CREATE_ARMY_NOT_RECRUITED", "not recruited"},
    {"HOI4_LOADER_CREATE_ARMY_OTHER_COUNTRY", "serves another country"},
    {"HOI4_LOADER_CREATE_ARMY_NOT_ARMY_LEADER", "not a general or field marshal"},
    {"HOI4_LOADER_CREATE_ARMY_CAPTURED", "captured"},
    {"HOI4_LOADER_CREATE_ARMY_LEADS_ARMY_GROUP", "leads an army group"},
    {"HOI4_LOADER_CREATE_ARMY_LEADS_ARMY", "already leads an army"},
    {"HOI4_LOADER_CREATE_ARMY_CANNOT_LEAVE", "their army $ARMY$ cannot change its general now"},
    {"HOI4_LOADER_CREATE_ARMY_DEPLOYING", "their army $ARMY$ is currently being deployed"},
    {"HOI4_LOADER_CREATE_ARMY_WITHDRAWING", "their army $ARMY$ is currently being withdrawn"},
    {"HOI4_LOADER_CREATE_ARMY_BORDER_WAR", "their army $ARMY$ is in a border war"},
    {"HOI4_LOADER_ADD_TO_ARMY_NOT_DIVISION", "this is not a land division"},
    {"HOI4_LOADER_ADD_TO_ARMY_DIVISION_BORDER_WAR", "its army $ARMY$ is in a border war"},
};
const LocText& ReasonText(Refusal r) { return kReasons[(int)r]; }

Refusal LeaveRefusal(Engine::LeaveBlock why) {
    switch (why) {
        case Engine::LeaveBlock::Deploying: return Refusal::Deploying;
        case Engine::LeaveBlock::Withdrawing: return Refusal::Withdrawing;
        case Engine::LeaveBlock::BorderWar: return Refusal::BorderWar;
        default: return Refusal::CannotLeave;
    }
}

// The reason in English for loader.log, with the army the general leads now in place of $ARMY$.
std::string ReasonEnglish(Refusal r, const std::string& army) {
    std::string s = ReasonText(r).english;
    size_t at = s.find("$ARMY$");
    if (at != std::string::npos) s.replace(at, 6, "\"" + army + "\"");
    return s;
}

struct Candidate {
    void* leader = nullptr;
    Engine::LeaderState state;
    std::string name;  // display name (localised if the name is a key)
};

std::string CurrentArmy(const Candidate& c) { return c.state.leads ? Engine::ArmyName(c.state.leads) : std::string(); }

// What to do when the general already leads an army: create_army refuses, or (reassign = yes)
// takes the general along if the game lets that army change its general; add_to_army keeps it.
enum class Leading { Refuse, Leave, Keep };

// Who the general (`key`, `token`) is and whether that general is available to `tag` now. Reads only.
Refusal CheckGeneral(const char* key, int token, int tag, Leading leading, Candidate* c) {
    c->name = native_localize_key(key);
    // The country's own characters first (has_character's lookup, valid after loading a save).
    // Otherwise recruit_character's table, which only tells why: not recruited, or another country.
    void* character = Engine::CountryCharacterByToken(tag, token);
    void* other = character ? nullptr : Engine::CharacterByToken(token);
    void* leader = Engine::ArmyLeaderRole(character ? character : other);
    if (leader) {
        std::string n = Engine::LeaderName(leader);
        if (!n.empty()) c->name = native_localize_key(n.c_str());
    }
    if (!character) {
        if (!other) return Refusal::UnknownCharacter;
        int owner = Engine::CharacterCountry(other);
        if (owner <= 0) return Refusal::NotRecruited;
        if (owner != tag) return Refusal::OtherCountry;
        character = other;
    }
    if (!leader || !Engine::ReadLeader(leader, &c->state) || c->state.roleType < 0 || c->state.roleType >= 2)
        return Refusal::NotArmyLeader;
    c->leader = leader;
    if (c->state.captured) return Refusal::Captured;
    if (c->state.leads && c->state.leadsArmyGroup) return Refusal::LeadsArmyGroup;
    if (c->state.leads && leading == Leading::Refuse) return Refusal::LeadsArmy;
    if (c->state.leads && leading == Leading::Leave && !c->state.canLeave) return LeaveRefusal(c->state.whyNot);
    return Refusal::None;
}

Refusal Check(const Args& a, int tag, Candidate* c) {
    return CheckGeneral(a.key(0), a.i(0), tag, a.b(2) ? Leading::Leave : Leading::Refuse, c);
}

void Execute(const Args& a, const Scope&, const EffectChildren& children) {
    if (!g_ok.load()) return;
    int tag = a.i(3);  // country (ROOT by default), resolved by the core
    std::string country = Engine::CountryTagName(tag);
    Candidate c;
    Refusal r = Check(a, tag, &c);
    if (r != Refusal::None) {
        Log("create_army (%s): %s (%s) cannot lead a new army: %s; nothing created", country.c_str(), c.name.c_str(), a.key(0),
            ReasonEnglish(r, CurrentArmy(c)).c_str());
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
        // Nothing runs, so no create_unit lines either.
        std::string reason = native_localize(ReasonText(r), {{"ARMY", CurrentArmy(c)}});
        out.add(native_localize(kUnavailable, {{"LEADER", c.name}, {"REASON", reason}}).c_str());
        out.hideChildren();
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

// --- add_to_army (NOTES.md G4) ---------------------------------------------------------------------
//
//   every_country_division = {
//       limit = { ... }
//       add_to_army = {
//           leader = GER_erwin_rommel   # character id: an army leader recruited by the division's country
//           name = "1. Panzerarmee"     # optional: only used when a new army is formed
//       }
//   }
//
// The division joins the army the general leads, through the game's own move (as the UI's "assign
// to army" does); its old army is deleted if left without divisions. If the general leads no army,
// a new one is formed from the division under the general (create_army's functions), and the next
// divisions join it. Refused, with a log line and a red tooltip line, if the general is not
// available (as for create_army), the division's army or the general's army is in a border war, or
// the scope holds no land division. Game state writes, main thread, deterministic. No command
// power, and the division gets none of the army's orders.

const char* const kJoinFeature = "add_to_army";
std::atomic<bool> g_joinOk{true};

const LocText kJoin{"HOI4_LOADER_ADD_TO_ARMY", "Joins the army of $LEADER|Y$."};
const LocText kJoinNew{"HOI4_LOADER_ADD_TO_ARMY_NEW", "Forms a new army under $LEADER|Y$."};
const LocText kJoinNewNamed{"HOI4_LOADER_ADD_TO_ARMY_NEW_NAMED", "Forms the army $NAME|Y$ under $LEADER|Y$."};
const LocText kJoinUnavailable{"HOI4_LOADER_ADD_TO_ARMY_UNAVAILABLE", "\x11RCannot join the army of $LEADER$: $REASON$.\x11!"};

// Repeated lines (the same refusal for every division of an every_country_division, all in one
// moment) are logged once, then counted. Main thread only (effects).
std::string g_lastJoinLine;
ULONGLONG g_lastJoinTime = 0;
int g_joinRepeats = 0;
void FlushJoinRepeats() {
    if (g_joinRepeats > 0) Log("add_to_army: (the previous line %d more time(s))", g_joinRepeats);
    g_joinRepeats = 0;
}
void LogJoin(const std::string& line) {
    ULONGLONG now = GetTickCount64();
    bool repeat = line == g_lastJoinLine && now - g_lastJoinTime < 2000;
    g_lastJoinTime = now;
    if (repeat) {
        // The count is written at the next safe point, after the effect (e.g. the whole
        // every_country_division) has run; or before the next different line.
        if (++g_joinRepeats == 1 && MainThreadQueueAvailable()) RunOnMainThread([](void*) { FlushJoinRepeats(); }, nullptr);
        return;
    }
    FlushJoinRepeats();
    g_lastJoinLine = line;
    Log("%s", line.c_str());
}

// Whether the division may join the general's army now (reads only). *target: the general's army
// (null if a new army would be formed); *current: the division's army.
Refusal CheckJoin(const Args& a, void* division, Candidate* c, void** target, void** current) {
    *target = *current = nullptr;
    Refusal r = CheckGeneral(a.key(0), a.i(0), Engine::UnitOwnerTag(division), Leading::Keep, c);
    if (r != Refusal::None) return r;
    *current = Engine::DivisionArmy(division);
    *target = c->state.leads;
    if (*current && *current != *target && Engine::ArmyInBorderWar(*current)) return Refusal::DivisionBorderWar;
    if (*target && *current != *target && Engine::ArmyInBorderWar(*target)) return Refusal::BorderWar;
    return Refusal::None;
}

std::string JoinReason(Refusal r, const Candidate& c, void* current, bool english) {
    std::string army = r == Refusal::DivisionBorderWar ? Engine::ArmyName(current) : CurrentArmy(c);
    return english ? ReasonEnglish(r, army) : native_localize(ReasonText(r), {{"ARMY", army}});
}

void ExecuteJoin(const Args& a, const Scope& s) {
    if (!g_joinOk.load() || !g_ok.load()) return;
    void* division = Engine::ScopeDivision(s.raw());
    if (!division) {
        LogJoin(std::string("add_to_army: ") + a.key(0) + ": " + ReasonText(Refusal::NotDivision).english + "; nothing done");
        return;
    }
    int tag = Engine::UnitOwnerTag(division);
    std::string country = Engine::CountryTagName(tag);
    Candidate c;
    void *target, *current;
    Refusal r = CheckJoin(a, division, &c, &target, &current);
    if (r != Refusal::None) {
        LogJoin("add_to_army (" + country + "): a division cannot join the army of " + c.name + " (" + a.key(0) +
                "): " + JoinReason(r, c, current, true) + "; nothing done");
        return;
    }
    if (target && target == current) return;  // already in that army
    if (target) {
        std::string oldName;
        bool oldDeleted = false;
        bool ok = Engine::JoinArmy(target, division, &oldName, &oldDeleted);
        std::string line = "add_to_army (" + country + "): a division ";
        if (!ok) line += "FAILED to join \"" + Engine::ArmyName(target) + "\" (the game did not move it)";
        else line += "joined \"" + Engine::ArmyName(target) + "\" under " + c.name;
        if (ok && !oldName.empty())
            line += "; it left \"" + oldName + "\"" + (oldDeleted ? ", which had no divisions left and was disbanded" : "");
        LogJoin(line);
        return;
    }
    // The general leads no army: a new one from this division (its old army, if left empty, is
    // deleted by the game's create_army itself).
    std::string oldName = current ? Engine::ArmyName(current) : std::string();
    void* army = Engine::CreateArmy(tag, {division});
    if (!army) {
        LogJoin("add_to_army (" + country + "): the game did not form a new army under " + c.name + "; the division is unchanged");
        return;
    }
    if (a.has(1)) Engine::SetArmyName(army, a.key(1));
    bool led = Engine::SetArmyLeader(army, c.leader);
    LogJoin("add_to_army (" + country + "): a division formed the new army \"" + Engine::ArmyName(army) + "\" under " + c.name +
            (led ? "" : " (FAILED to set the general)") + (oldName.empty() ? "" : "; it left \"" + oldName + "\""));
}

void TooltipJoin(const Args& a, const Scope& s, TooltipText& out) {
    if (!g_joinOk.load() || !g_ok.load()) return;
    Candidate c;
    void* division = Engine::ScopeDivision(s.raw());
    Refusal r = Refusal::None;
    void *target = nullptr, *current = nullptr;
    if (division) {
        r = CheckJoin(a, division, &c, &target, &current);
    } else {
        // An iterator's header (every_country_division) has no division yet: check the general
        // against the scope's country if there is one, else only name the general.
        int tag = s.countryTag() > 0 ? s.countryTag() : s.root().countryTag();
        if (tag > 0) {
            r = CheckGeneral(a.key(0), a.i(0), tag, Leading::Keep, &c);
            target = c.state.leads;
        } else {
            c.name = native_localize_key(a.key(0));
            out.add(native_localize(kJoin, {{"LEADER", c.name}}).c_str());
            return;
        }
    }
    if (r != Refusal::None) {
        out.add(native_localize(kJoinUnavailable, {{"LEADER", c.name}, {"REASON", JoinReason(r, c, current, false)}}).c_str());
        return;
    }
    if (target) out.add(native_localize(kJoin, {{"LEADER", c.name}}).c_str());
    else out.add(native_localize(a.has(1) ? kJoinNewNamed : kJoinNew, {{"LEADER", c.name}, {"NAME", a.has(1) ? a.key(1) : ""}}).c_str());
}

bool SafeJoinSelfCheck(int tag, int* divisions, const char** why, bool* ok) {
    __try {
        *ok = Engine::JoinSelfCheck(tag, divisions, why);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "crashed";
        return false;
    }
}

void OnEnteredJoin(bool) {
    FlushJoinRepeats();
    g_lastJoinLine.clear();
    int tag = Engine::PlayerCountryTag();
    if (tag <= 0) return;  // observer: nothing to check against
    int divisions = 0;
    const char* why = "";
    bool ok = false;
    if (!SafeJoinSelfCheck(tag, &divisions, &why, &ok) || !ok) {
        g_joinOk.store(false);
        Log("add_to_army: self-check failed for %s (%s)", Engine::CountryTagName(tag).c_str(), why);
        FeatureDeactivate(kJoinFeature, "self-check failed: army membership data unreadable");
        return;
    }
    if (!g_joinOk.load()) return;  // switched off earlier this session: stays off
    Log("add_to_army: self-check passed (%s: %d division(s) found in their armies' member lists)", Engine::CountryTagName(tag).c_str(),
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
    for (int r = 1; r <= (int)Refusal::BorderWar; ++r) texts.push_back(kReasons[r]);
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

bool AddToArmyInit() {
    if (!Engine::HaveJoinArmy()) {
        Log("add_to_army: required engine functions missing (join army %d)", Engine::HaveJoinArmy());
        return false;
    }
    std::vector<LocText> texts = {kJoin, kJoinNew, kJoinNewNamed, kJoinUnavailable};
    for (int r = 1; r < (int)(sizeof kReasons / sizeof kReasons[0]); ++r)
        if (r != (int)Refusal::LeadsArmy && r != (int)Refusal::CannotLeave && r != (int)Refusal::Deploying &&
            r != (int)Refusal::Withdrawing)
            texts.push_back(kReasons[r]);
    register_native_effect(
        {"add_to_army",
         "Use it in a division scope (e.g. inside every_country_division): the division joins the army that `leader` (a "
         "character id) leads, as the UI's assignment does; its old army is disbanded if left without divisions. If the "
         "general leads no army, a new army is formed from the division under the general, named `name` if given, and "
         "later divisions join it. The general must be an army leader (general or field marshal) recruited by the "
         "division's country, not captured and not leading an army group. Nothing happens (logged in loader.log; the "
         "tooltip shows the reason in red) if the general is not available, or if the division's army or the general's "
         "army is in a border war. No command power is charged, and the division gets none of the army's orders.",
         Form::Block,
         {{"leader", ParamType::Token, true}, {"name", ParamType::Text, false}},
         ScopeMask::Unit,
         &ExecuteJoin,
         &TooltipJoin,
         texts},
        kJoinFeature);
    OnGameEntered(&OnEnteredJoin);
    return true;
}
