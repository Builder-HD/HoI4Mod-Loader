// Engine access for map_adjacency (NOTES.md Part R): a border's type between two neighbouring
// provinces and a state's impassable flag. Both live in static map/state data, loaded once at start
// and kept between games, and the game reads them live (movement, attacks, supply, AI), so a change
// is a write of one byte. Main thread only: effects run there in serial code.
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

uint32_t g_nbArray = 0, g_nbCount = 0, g_nbId = 0, g_nbStride = 0;  // map province -> neighbour entries
uint32_t g_nbType = 0;                                                 // entry -> border type byte
uint8_t g_impassableType = 0;                                          // the type of an impassable border (4)
uint32_t g_stateDef = 0, g_stateImpassable = 0;                        // state -> its definition -> flag byte

bool Fail(const char* why) {
    Log("Map adjacency: %s; map_adjacency off", why);
    return false;
}

// The entry of `map`'s neighbour list for province id `b`, null if b is no neighbour. SEH: plain
// reads of the game's data.
uint8_t* EntryOf(const void* map, int b) {
    __try {
        uint8_t* arr = Rd<uint8_t*>(map, g_nbArray);
        int n = Rd<int>(map, g_nbCount);
        if (!arr || n < 0 || n > 4096) return nullptr;
        for (int i = 0; i < n; ++i)
            if (Rd<int>(arr + (size_t)i * g_nbStride, g_nbId) == b) return arr + (size_t)i * g_nbStride;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

uint8_t* StateFlag(int stateId) {
    void* state = StateById(stateId);
    if (!state) return nullptr;
    __try {
        uint8_t* def = Rd<uint8_t*>(state, g_stateDef);
        return def ? def + g_stateImpassable : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* DefOf(const void* state) {
    __try {
        return Rd<void*>(state, g_stateDef);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

}  // namespace

void ResolveAdjacency() {
    // The attack check: the neighbour finder (whose code gives the list layout), then the type
    // compared with the impassable value.
    uint8_t* p = SigFindUnique("adj_attack_check", kSigAdjAttackCheck);
    if (p) {
        uint8_t* find = SigFollowRel32(p + kSigAdjFindCall);
        static const uint8_t kFind[] = {0x48, 0x63, 0x41};  // movsxd rax, [rcx+count]
        if (find && memcmp(find, kFind, sizeof kFind) == 0 && Rd<uint8_t>(find, 0xb) == 0x4C && Rd<uint8_t>(find, 0xc) == 0x8B &&
            Rd<uint8_t>(find, 0xd) == 0x59 && Rd<uint8_t>(find, 0x2a) == 0x48 && Rd<uint8_t>(find, 0x2b) == 0x83 &&
            Rd<uint8_t>(find, 0x2c) == 0xC0) {
            g_nbCount = Rd<uint8_t>(find, 3);
            g_nbArray = Rd<uint8_t>(find, 0xe);
            g_nbId = Rd<uint8_t>(find, 0x18);
            g_nbStride = Rd<uint8_t>(find, 0x2d);
            g_nbType = Rd<uint8_t>(p, kSigAdjTypeDisp);
            g_impassableType = Rd<uint8_t>(p, kSigAdjTypeDisp + 1);
        } else {
            Log("ERROR: map_adjacency: the neighbour finder looks different");
        }
    }
    // The impassable trigger's getter: mov rax, [rcx+def]; movzx eax, byte [rax+flag]; ret.
    if ((p = SigFindUnique("adj_state_impassable", kSigAdjStateImpassable)) != nullptr) {
        uint8_t* get = SigFollowRel32(p + kSigAdjStateGetterCall);
        if (get && Rd<uint8_t>(get, 0) == 0x48 && Rd<uint8_t>(get, 1) == 0x8B && Rd<uint8_t>(get, 2) == 0x41 &&
            Rd<uint8_t>(get, 4) == 0x0F && Rd<uint8_t>(get, 5) == 0xB6 && Rd<uint8_t>(get, 6) == 0x80 && Rd<uint8_t>(get, 0xb) == 0xC3) {
            g_stateDef = Rd<uint8_t>(get, 3);
            g_stateImpassable = Rd<uint32_t>(get, 7);
        } else {
            Log("ERROR: map_adjacency: the state impassable getter looks different");
        }
    }
    Log("Map adjacency: neighbours +0x%x (count +0x%x, stride 0x%x, id +0x%x, type +0x%x, impassable %u), state flag +0x%x/+0x%x",
        g_nbArray, g_nbCount, g_nbStride, g_nbId, g_nbType, g_impassableType, g_stateDef, g_stateImpassable);
}

bool HaveAdjacency() {
    return g_nbArray && g_nbCount && g_nbStride >= 0x10 && g_nbType && g_impassableType && g_stateDef && g_stateImpassable &&
           HaveProvinceData();
}

int ImpassableBorderType() { return g_impassableType; }

int BorderType(int a, int b) {
    if (!HaveAdjacency()) return -1;
    const void* map = MapProvinceById(a);
    uint8_t* e = map ? EntryOf(map, b) : nullptr;
    return e ? e[g_nbType] : -1;
}

bool SetBorderType(int a, int b, int type) {
    if (!HaveAdjacency() || type < 0 || type > 255) return false;
    const void* map = MapProvinceById(a);
    uint8_t* e = map ? EntryOf(map, b) : nullptr;
    if (!e) return false;
    e[g_nbType] = (uint8_t)type;
    return true;
}

int StateImpassable(int stateId) {
    if (!HaveAdjacency()) return -1;
    uint8_t* f = StateFlag(stateId);
    return f ? (*f != 0) : -1;
}

bool SetStateImpassable(int stateId, bool impassable) {
    if (!HaveAdjacency()) return false;
    uint8_t* f = StateFlag(stateId);
    if (!f) return false;
    *f = impassable ? 1 : 0;
    return true;
}

bool AdjacencySelfCheck(std::string* problem) {
    if (!HaveAdjacency()) {
        *problem = "fields unresolved";
        return false;
    }
    // Every state's definition object is its own (the flag is per state, not shared).
    void* seen[64] = {};
    int n = 0;
    for (int id = 1; id < 2000 && n < 64; ++id) {
        void* state = StateById(id);
        if (!state) continue;
        void* def = DefOf(state);
        if (!def) {
            *problem = "state " + std::to_string(id) + " has no definition object";
            return false;
        }
        for (int k = 0; k < n; ++k)
            if (seen[k] == def) {
                *problem = "states share a definition object (the impassable flag would not be per state)";
                return false;
            }
        seen[n++] = def;
    }
    if (n < 2) {
        *problem = "no states found";
        return false;
    }
    return true;
}

}  // namespace Engine
