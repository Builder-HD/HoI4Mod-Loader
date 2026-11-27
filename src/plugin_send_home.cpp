// send_home / send_divisions_home (engine tier): divisions travel home the way the game brings
// volunteers home (NOTES.md Part V).
//
//   every_country_division = {            # division scope: this division
//       limit = { ... }
//       send_home = {
//           days = 20                      # optional: travel days, a number or a variable (default:
//                                          #   the game's VOLUNTEERS_TRANSFER_SPEED, 14)
//           travel_time = yes              # optional: its travel time home instead (division_pathing)
//           province = 6521                # optional: arrival province, or
//           state = 64                     #   arrival state (its province nearest to where they leave)
//       }
//   }
//
//   send_divisions_home = {                # country scope: every division of this country that
//       from = POL                         #   stands in territory POL controls (required)
//       days = 20  province = 6521  state = 64   # optional, as above
//   }
//
// Both call the game's own transfer (as recall_volunteers_from does): the divisions leave the map,
// their army and every list, can't be seen, ordered or attacked, and after `days` arrive at a port
// of their country (the game's choice, or `province` / `state`). The game makes one travelling
// group per army: send_divisions_home brings armies back with their name and general (if free), as
// for volunteers. send_home sends one division at a time, so it arrives without an army (otherwise
// every division of an army would come back as an army of its own). The game keeps 95% of the
// equipment on the way (VOLUNTEERS_RETURN_EQUIPMENT) and shows its "volunteers returned" message.
// Nothing of the loader is saved: a travelling group is a vanilla transfer in the save. Game state
// writes, main thread, deterministic: the same effect gives the same result on every machine.
#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"
#include "native_script.h"

namespace {

const char* const kFeature = "send_home";
std::atomic<bool> g_ok{true};

const LocText kUnitGoes{"HOI4_LOADER_SEND_HOME", "Travels home to $COUNTRY|Y$: out of reach for $DAYS|Y$ days, then arrives without an army."};
const LocText kUnitGoesAt{"HOI4_LOADER_SEND_HOME_AT",
                          "Travels home to $COUNTRY|Y$: out of reach for $DAYS|Y$ days, then arrives in $PLACE|Y$ without an army."};
const LocText kUnitGoesAny{"HOI4_LOADER_SEND_HOME_ANY", "Travels home: out of reach for $DAYS|Y$ days, then arrives without an army."};
const LocText kUnitRefused{"HOI4_LOADER_SEND_HOME_REFUSED", "\x11RCannot travel home: $REASON$.\x11!"};
const LocText kCountryGoes{"HOI4_LOADER_SEND_DIVISIONS_HOME",
                           "$COUNT|Y$ division(s) of $COUNTRY|Y$ in territory controlled by $FROM|Y$ travel home: out of reach for "
                           "$DAYS|Y$ days, then they arrive with their armies."};
const LocText kCountryGoesAt{"HOI4_LOADER_SEND_DIVISIONS_HOME_AT",
                             "$COUNT|Y$ division(s) of $COUNTRY|Y$ in territory controlled by $FROM|Y$ travel home: out of reach "
                             "for $DAYS|Y$ days, then they arrive in $PLACE|Y$ with their armies."};
const LocText kCountryNone{"HOI4_LOADER_SEND_DIVISIONS_HOME_NONE",
                           "\x11RNo division of $COUNTRY$ in territory controlled by $FROM$ can travel home now.\x11!"};
const LocText kCountryStay{"HOI4_LOADER_SEND_DIVISIONS_HOME_STAY",
                           "$COUNT|Y$ more stay there (in battle, volunteers, in a border war, or not taken by the game)."};
const LocText kCountrySelf{"HOI4_LOADER_SEND_DIVISIONS_HOME_SELF", "\x11RDivisions can't travel home from their own country.\x11!"};
const LocText kPlaceIgnored{"HOI4_LOADER_SEND_HOME_PLACE_IGNORED",
                            "$PLACE$ has no province controlled by $COUNTRY$: the game picks the arrival port."};
const LocText kProvince{"HOI4_LOADER_SEND_HOME_PROVINCE", "province $ID$"};

enum class Refusal { None, NotDivision, NotTaken, NotOnMap, AtHome, Volunteer, InBattle, BorderWar };
const LocText kReasons[] = {
    {"HOI4_LOADER_SEND_HOME_OK", "available"},
    {"HOI4_LOADER_SEND_HOME_NOT_DIVISION", "this is not a land division"},
    {"HOI4_LOADER_SEND_HOME_NOT_TAKEN", "the game's transfer does not take this division"},
    {"HOI4_LOADER_SEND_HOME_NOT_ON_MAP", "it is at sea or already travelling"},
    {"HOI4_LOADER_SEND_HOME_AT_HOME", "it already stands in territory its country controls"},
    {"HOI4_LOADER_SEND_HOME_VOLUNTEER", "it is a volunteer (recall_volunteers_from brings volunteers home)"},
    {"HOI4_LOADER_SEND_HOME_IN_BATTLE", "it is in battle"},
    {"HOI4_LOADER_SEND_HOME_BORDER_WAR", "its army $ARMY$ is in a border war"},
};
const LocText& ReasonText(Refusal r) { return kReasons[(int)r]; }

std::string Reason(Refusal r, const void* division, bool english) {
    std::string army = r == Refusal::BorderWar ? Engine::ArmyName(Engine::DivisionArmy(division)) : std::string();
    if (!english) return native_localize(ReasonText(r), {{"ARMY", army}});
    std::string s = ReasonText(r).english;
    size_t at = s.find("$ARMY$");
    if (at != std::string::npos) s.replace(at, 6, "\"" + army + "\"");
    return s;
}

// Whether the division can travel home now (reads only). *from: the controller of its province.
Refusal Check(void* division, int* from) {
    *from = 0;
    if (!division) return Refusal::NotDivision;
    if (!Engine::UnitTransferable(division)) return Refusal::NotTaken;
    void* province = Engine::UnitProvince(division);
    if (!province || !Engine::ProvinceHoldsLandUnit(province, division)) return Refusal::NotOnMap;
    int controller = Engine::ProvinceController(Engine::ProvinceIdOf(province));
    if (controller <= 0) return Refusal::NotOnMap;
    *from = controller;
    if (Engine::IsSameCountry(controller, Engine::UnitOwnerTag(division))) return Refusal::AtHome;
    if (Engine::UnitIsVolunteer(division)) return Refusal::Volunteer;
    if (Engine::UnitInCombat(division)) return Refusal::InBattle;
    void* army = Engine::DivisionArmy(division);
    if (army && Engine::ArmyInBorderWar(army)) return Refusal::BorderWar;
    return Refusal::None;
}

std::string CountryName(int tag) { return native_localize_key(Engine::CountryTagName(tag).c_str()); }

// --- Arrival province ----------------------------------------------------------------------------
struct Arrival {
    int province = 0;   // 0 = the game's own choice
    bool asked = false;  // province or state written
    std::string place;   // what was asked, for tooltips and the log
};

bool Centre(int province, long long* x, long long* y) {
    int r[4];
    if (!Engine::ProvinceRect(province, r)) return false;
    *x = r[0] + r[2] / 2;
    *y = r[1] + r[3] / 2;
    return true;
}

std::string ProvinceText(int id, bool english) {
    std::string key = "VICTORY_POINTS_" + std::to_string(id);
    if (!english) {
        std::string name = native_localize_key(key.c_str());
        if (!name.empty() && name != key) return name;
        return native_localize(kProvince, {{"ID", std::to_string(id)}});
    }
    return "province " + std::to_string(id);
}

std::string StateText(int id, bool english) {
    if (english) return "state " + std::to_string(id);
    std::string key = "STATE_" + std::to_string(id);
    std::string name = native_localize_key(key.c_str());
    return name.empty() || name == key ? "state " + std::to_string(id) : name;
}

// `province` (index ip) if the owner controls it; else `state` (index is): its province the owner
// controls nearest to `nearProvince`. Deterministic: map coordinates, the engine's list order.
Arrival ChooseArrival(const Args& a, size_t ip, size_t is, int owner, int nearProvince, bool english) {
    Arrival out;
    if (a.has(ip) && a.i(ip) > 0) {
        out.asked = true;
        out.place = ProvinceText(a.i(ip), english);
        if (Engine::IsSameCountry(Engine::ProvinceController(a.i(ip)), owner)) out.province = a.i(ip);
        return out;
    }
    if (!a.has(is) || a.i(is) <= 0) return out;
    out.asked = true;
    out.place = StateText(a.i(is), english);
    std::vector<int> ids;
    if (!Engine::StateProvinceIds(a.i(is), &ids)) return out;
    long long nx = 0, ny = 0;
    bool haveNear = nearProvince > 0 && Centre(nearProvince, &nx, &ny);
    long long best = -1;
    for (int id : ids) {
        if (!Engine::IsSameCountry(Engine::ProvinceController(id), owner)) continue;
        long long x = 0, y = 0;
        long long d = haveNear && Centre(id, &x, &y) ? (x - nx) * (x - nx) + (y - ny) * (y - ny) : 0;
        if (best < 0 || d < best) {
            best = d;
            out.province = id;
        }
        if (!haveNear) break;
    }
    return out;
}

// `days` (a number or a variable, read in the effect's scope); 0 = the game's default.
int DaysArg(const Args& a, size_t i, const Scope& s) {
    if (!a.has(i)) return 0;
    int d = a.value(i, s);
    return d < 1 ? 1 : d;
}
// send_home: `travel_time = yes` takes the division's travel time home (division_pathing) when
// known, else `days`.
const size_t kUnitTravelTime = 3;
int UnitDays(const Args& a, const Scope& s, void* division, bool* fromRoute = nullptr) {
    if (fromRoute) *fromRoute = false;
    if (a.b(kUnitTravelTime) && division) {
        int d = DivisionTravelDaysHome(division);
        if (d > 0) {
            if (fromRoute) *fromRoute = true;
            return d;
        }
    }
    return DaysArg(a, 0, s);
}
int DaysShown(int days) { return days > 0 ? days : Engine::SendHomeDefaultDays(); }

// Repeated lines (the same refusal for every division of an every_country_division, all in one
// moment) are logged once, then counted. Main thread only (effects).
std::string g_lastLine;
ULONGLONG g_lastTime = 0;
int g_repeats = 0;
void FlushRepeats() {
    if (g_repeats > 0) Log("send_home: (the previous line %d more time(s))", g_repeats);
    g_repeats = 0;
}
void LogLine(const std::string& line) {
    ULONGLONG now = GetTickCount64();
    bool repeat = line == g_lastLine && now - g_lastTime < 2000;
    g_lastTime = now;
    if (repeat) {
        if (++g_repeats == 1 && MainThreadQueueAvailable()) RunOnMainThread([](void*) { FlushRepeats(); }, nullptr);
        return;
    }
    FlushRepeats();
    g_lastLine = line;
    Log("%s", line.c_str());
}

std::string ArrivalLog(const Arrival& arr) {
    if (!arr.asked) return "the game's arrival port";
    if (arr.province) return "province " + std::to_string(arr.province) + " (" + arr.place + ")";
    return "the game's arrival port (" + arr.place + " has no province the country controls)";
}

// --- send_home (division scope) ------------------------------------------------------------------
// Parameters: 0 days, 1 province, 2 state.

void ExecuteUnit(const Args& a, const Scope& s) {
    if (!g_ok.load()) return;
    void* division = Engine::ScopeDivision(s.raw());
    int from = 0;
    Refusal r = Check(division, &from);
    int owner = division ? Engine::UnitOwnerTag(division) : 0;
    std::string country = owner ? Engine::CountryTagName(owner) : std::string("?");
    if (r != Refusal::None) {
        LogLine("send_home (" + country + "): a division cannot travel home: " + Reason(r, division, true) + "; nothing done");
        return;
    }
    int here = Engine::ProvinceIdOf(Engine::UnitProvince(division));
    Arrival arr = ChooseArrival(a, 1, 2, owner, here, true);
    bool fromRoute = false;
    int days = UnitDays(a, s, division, &fromRoute);
    int made = Engine::SendHome(owner, from, {division}, days, arr.province, false);
    if (made <= 0) {
        LogLine("send_home (" + country + "): the game " +
                (made < 0 ? std::string("transfer list is unreadable") : "found no arrival port (" + Engine::CountryTagName(from) +
                                                                            " or " + country + " has no capital?)") +
                "; the division stays in province " + std::to_string(here));
        return;
    }
    LogLine("send_home (" + country + "): a division leaves province " + std::to_string(here) + " (" +
            Engine::CountryTagName(from) + ") for " + ArrivalLog(arr) + ", " + std::to_string(DaysShown(days)) +
            (fromRoute ? " days (its travel time)" : " days") + ", without an army");
}

void TooltipUnit(const Args& a, const Scope& s, TooltipText& out) {
    if (!g_ok.load()) return;
    void* division = Engine::ScopeDivision(s.raw());
    int days = DaysShown(UnitDays(a, s, division));
    if (!division) {
        // An iterator's header (every_country_division) has no division yet.
        out.add(native_localize(kUnitGoesAny, {{"DAYS", std::to_string(days)}}).c_str());
        return;
    }
    int from = 0;
    Refusal r = Check(division, &from);
    if (r != Refusal::None) {
        out.add(native_localize(kUnitRefused, {{"REASON", Reason(r, division, false)}}).c_str());
        return;
    }
    int owner = Engine::UnitOwnerTag(division);
    Arrival arr = ChooseArrival(a, 1, 2, owner, Engine::ProvinceIdOf(Engine::UnitProvince(division)), false);
    std::string name = CountryName(owner);
    if (arr.province)
        out.add(native_localize(kUnitGoesAt, {{"COUNTRY", name}, {"DAYS", std::to_string(days)}, {"PLACE", arr.place}}).c_str());
    else
        out.add(native_localize(kUnitGoes, {{"COUNTRY", name}, {"DAYS", std::to_string(days)}}).c_str());
    if (arr.asked && !arr.province) out.add(" ").add(native_localize(kPlaceIgnored, {{"PLACE", arr.place}, {"COUNTRY", name}}).c_str());
}

// --- send_divisions_home (country scope) ---------------------------------------------------------
// Parameters: 0 from, 1 days, 2 province, 3 state.

struct Selection {
    std::vector<void*> go;  // the engine's list order, the same on every machine
    int stay = 0;           // in `from`'s territory but refused
    int firstProvince = 0;  // where the first of them stands (for state =)
};

Selection Select(int owner, int from) {
    Selection sel;
    std::vector<void*> units;
    if (!Engine::CountryDivisionList(owner, &units)) return sel;
    for (void* unit : units) {
        int at = 0;
        Refusal r = Check(unit, &at);
        if (r == Refusal::NotOnMap || r == Refusal::AtHome || !Engine::IsSameCountry(at, from)) continue;
        if (r != Refusal::None) {
            ++sel.stay;
            continue;
        }
        if (sel.go.empty()) sel.firstProvince = Engine::ProvinceIdOf(Engine::UnitProvince(unit));
        sel.go.push_back(unit);
    }
    return sel;
}

void ExecuteCountry(const Args& a, const Scope& s) {
    if (!g_ok.load()) return;
    int owner = s.countryTag();
    int from = a.i(0);  // resolved by the core
    std::string country = Engine::CountryTagName(owner), fromName = Engine::CountryTagName(from);
    if (owner <= 0 || from <= 0 || !Engine::CountryByTag(from)) {
        Log("send_divisions_home (%s): no country scope or unknown `from`; nothing done", country.c_str());
        return;
    }
    if (Engine::IsSameCountry(owner, from)) {
        Log("send_divisions_home (%s): `from` is the country itself; nothing done", country.c_str());
        return;
    }
    Selection sel = Select(owner, from);
    if (sel.go.empty()) {
        Log("send_divisions_home (%s): no division in territory of %s can travel home (%d there refused: in battle, volunteers, "
            "in a border war, or not taken by the game)",
            country.c_str(), fromName.c_str(), sel.stay);
        return;
    }
    Arrival arr = ChooseArrival(a, 2, 3, owner, sel.firstProvince, true);
    int days = DaysArg(a, 1, s);
    int made = Engine::SendHome(owner, from, sel.go, days, arr.province, true);
    if (made <= 0) {
        Log("send_divisions_home (%s): the game %s; %zu division(s) stay in territory of %s", country.c_str(),
            made < 0 ? "transfer list is unreadable" : "found no arrival port (no capital?)", sel.go.size(), fromName.c_str());
        return;
    }
    Log("send_divisions_home (%s): %zu division(s) leave territory of %s in %d group(s) for %s, %d days%s", country.c_str(),
        sel.go.size(), fromName.c_str(), made, ArrivalLog(arr).c_str(), DaysShown(days),
        sel.stay ? ("; " + std::to_string(sel.stay) + " refused stay (in battle, volunteers, in a border war, or not taken)").c_str()
                 : "");
}

void TooltipCountry(const Args& a, const Scope& s, TooltipText& out) {
    if (!g_ok.load()) return;
    int owner = s.countryTag();
    int from = a.i(0);
    if (owner <= 0 || from <= 0) return;
    if (Engine::IsSameCountry(owner, from)) {
        out.add(native_localize(kCountrySelf).c_str());
        return;
    }
    std::string name = CountryName(owner), fromName = CountryName(from);
    Selection sel = Select(owner, from);
    if (sel.go.empty()) {
        out.add(native_localize(kCountryNone, {{"COUNTRY", name}, {"FROM", fromName}}).c_str());
        return;
    }
    Arrival arr = ChooseArrival(a, 2, 3, owner, sel.firstProvince, false);
    std::vector<LocArg> args = {{"COUNT", std::to_string(sel.go.size())},
                                {"COUNTRY", name},
                                {"FROM", fromName},
                                {"DAYS", std::to_string(DaysShown(DaysArg(a, 1, s)))},
                                {"PLACE", arr.place}};
    out.add(native_localize(arr.province ? kCountryGoesAt : kCountryGoes, args).c_str());
    if (arr.asked && !arr.province) out.add(" ").add(native_localize(kPlaceIgnored, {{"PLACE", arr.place}, {"COUNTRY", name}}).c_str());
    if (sel.stay) out.add(" ").add(native_localize(kCountryStay, {{"COUNT", std::to_string(sel.stay)}}).c_str());
}

// --- Self-check on every game entry (main thread) ----------------------------------------------
bool SafeSelfCheck(int tag, int* onMap, int* groups, const char** why, bool* ok) {
    __try {
        *ok = Engine::SendHomeSelfCheck(tag, onMap, groups, why);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "crashed";
        return false;
    }
}

void OnEntered(bool) {
    FlushRepeats();
    g_lastLine.clear();
    int tag = Engine::PlayerCountryTag();
    if (tag <= 0) return;  // observer: nothing to check against
    int onMap = 0, groups = 0;
    const char* why = "";
    bool ok = false;
    if (!SafeSelfCheck(tag, &onMap, &groups, &why, &ok) || !ok) {
        g_ok.store(false);
        Log("send_home: self-check failed for %s (%s)", Engine::CountryTagName(tag).c_str(), why);
        FeatureDeactivate(kFeature, "self-check failed: division locations or travelling groups unreadable");
        return;
    }
    if (!g_ok.load()) return;  // switched off earlier this session: stays off
    Log("send_home: self-check passed (%s: %d division(s) found in their province, %d travelling group(s))",
        Engine::CountryTagName(tag).c_str(), onMap, groups);
}

}  // namespace

bool SendHomeInit() {
    if (!Engine::HaveSendHome()) {
        Log("send_home: required engine functions missing (transfer %d)", Engine::HaveSendHome());
        return false;
    }
    std::vector<LocText> unitTexts = {kUnitGoes, kUnitGoesAt, kUnitGoesAny, kUnitRefused, kPlaceIgnored, kProvince};
    for (int r = 1; r < (int)(sizeof kReasons / sizeof kReasons[0]); ++r) unitTexts.push_back(kReasons[r]);
    ParamSpec days{"days", ParamType::Int, false};
    days.variable = true;
    std::vector<ParamSpec> place = {days, {"province", ParamType::Int, false}, {"state", ParamType::Int, false}};
    std::vector<ParamSpec> unitParams = place;
    unitParams.push_back({"travel_time", ParamType::Bool, false});  // kUnitTravelTime
    bool ok = register_native_effect(
        {"send_home",
         "Use it in a division scope (e.g. inside every_country_division): the division travels home the way the game brings "
         "volunteers home. It leaves the map and its army, can't be seen, ordered or attacked, and after `days` (default: the "
         "game's VOLUNTEERS_TRANSFER_SPEED, 14) arrives in its country without an army: at `province`, or at the province of "
         "`state` nearest to where it leaves, if its country controls one, else at the port the game picks. The game keeps "
         "95% of its equipment on the way (VOLUNTEERS_RETURN_EQUIPMENT). Nothing happens (logged in loader.log; the tooltip "
         "shows the reason in red) if the division already stands in territory its country controls, is at sea, in battle, a "
         "volunteer (use recall_volunteers_from) or in an army in a border war. To bring whole armies home with their name and "
         "general, use send_divisions_home. `days` may be a variable (read in the division scope, so use ROOT.my_days or "
         "a global). `travel_time = yes`: away for the division's travel time home in whole days, rounded up, as "
         "division_pathing's travel_time computes it (the route a move order would take); `days` when there is no route or "
         "division_pathing is off.",
         Form::Block,
         unitParams,
         ScopeMask::Unit,
         &ExecuteUnit,
         &TooltipUnit,
         unitTexts},
        kFeature);
    std::vector<LocText> countryTexts = {kCountryGoes, kCountryGoesAt, kCountryNone, kCountryStay, kCountrySelf, kPlaceIgnored, kProvince};
    std::vector<ParamSpec> countryParams = {{"from", ParamType::Country, true}};
    countryParams.insert(countryParams.end(), place.begin(), place.end());
    ok = register_native_effect(
             {"send_divisions_home",
              "Use it in a country scope: every land division of this country that stands in territory controlled by `from` (a "
              "country, or THIS/ROOT/FROM/PREV) travels home the way the game brings volunteers home, one group per army. They "
              "leave the map, can't be seen, ordered or attacked, and after `days` (default 14, the game's "
              "VOLUNTEERS_TRANSFER_SPEED) arrive with their army's name and general (if the general leads nothing else then): "
              "at `province`, or at the province of `state` nearest to where the first of them stands, if this country "
              "controls one, else at the port the game picks. The game keeps 95% of their equipment on the way. Divisions in "
              "battle, volunteers and armies in a border war stay. Meant for troops left in another country after a peace, "
              "e.g. `send_divisions_home = { from = POL }`.",
              Form::Block,
              countryParams,
              ScopeMask::Country,
              &ExecuteCountry,
              &TooltipCountry,
              countryTexts},
             kFeature) &&
         ok;
    OnGameEntered(&OnEntered);
    return ok;
}
