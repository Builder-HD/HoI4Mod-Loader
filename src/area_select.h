// Area selection (NOTES.md M4, M14): the `area = { ... }` block a native command declares to pick
// provinces, and the selection itself. attention_area uses it to mark provinces; a general area
// selector feature (an iterator over selected provinces) is meant to share it.
//
//   area = { province = <id> state = <id>|this strategic_region = <id>   (any number of each, added
//                                                                        up; none: every land province)
//            owner = <country> controller = <country> coastal = yes/no    (filters, all must match)
//            random_provinces = <n> }                                     (keep n of them at random)
//
// Several area blocks add up. Selection reads game state only, in province id order, and the random
// picks come from a counter of Select calls (every machine runs the same effects in the same order),
// not from the clock: the same on every machine of a multiplayer game (safe in effects).
#pragma once
#include <stddef.h>

#include <vector>

#include "native_script.h"

namespace AreaSelect {

// The keys of one area block, in their Args order.
const std::vector<ParamSpec>& Keys();
// The block parameter itself (repeatable): put it into a command's parameter list.
ParamSpec BlockParam(const char* name = "area", bool required = true);
// The engine data selection needs (province data, state and region province lists, countries).
bool Available();
// The provinces every block of parameter `index` selects together, ascending ids, no duplicates.
// Main thread (effects): it walks every state and province once. Each call advances the random
// counter, also when no block uses random_provinces.
std::vector<int> Select(const Args& args, size_t index, const Scope& scope);
// A new game session: the random counter starts again (call when a game is entered).
void ResetSession();

}  // namespace AreaSelect
