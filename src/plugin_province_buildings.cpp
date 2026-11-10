// province_building_removal (engine tier): a cross on the state view's province building entries
// (supply hub, naval base, bunkers, coastal bunkers, ...), as vanilla shows one on the shared slot
// buildings. A click removes one level of that building in that province at once, as
// remove_building does, so the supply system follows for hubs (NOTES.md Part I).
//
// The cross is a `remove` button the loader adds to province_building_entry by patching
// interface/countrystateview.gui when the game reads it (vanilla's copy or a mod's). It shows where
// the local player controls the province and the building has a level, enabled unless vanilla's
// wartime removal cooldown blocks it. Single player only: a click happens on one machine, so in
// multiplayer the cross stays hidden.
#include <atomic>
#include <string>

#include "engine.h"
#include "lifecycle.h"
#include "loader.h"

namespace {

const char* const kFeature = "province_building_removal";
std::atomic<int> g_generation{0};  // a new game or a loaded save: pending clicks no longer apply

bool Allowed() {
    const char* why = nullptr;
    return CurrentSessionSinglePlayer(&why);
}

struct Click {
    int generation;
    void* province;
    void* type;
    void* state;
};

void RemoveClicked(void* ctx) {
    Click* c = (Click*)ctx;
    if (c->generation != g_generation.load()) {
        Log("province_building_removal: click from before the last game start or load ignored");
    } else {
        std::string why;
        int level = Engine::RemoveProvinceBuildingLevel(c->province, c->type, c->state, &why);
        if (level >= 0)
            Log("province_building_removal: one level removed (now %d)", level);
        else
            Log("province_building_removal: nothing removed: %s", why.c_str());
    }
    delete c;
}

// UI thread: the change waits for the next safe point on the main thread.
bool OnClick(void* province, void* type, void* state) {
    if (!Allowed()) return false;
    Click* c = new Click{g_generation.load(), province, type, state};
    if (!RunOnMainThread(&RemoveClicked, c)) {
        delete c;
        return false;
    }
    return true;
}

std::string PatchGui(const std::string& original) { return Engine::PatchCountryStateViewGui(original); }

// The engine switched itself off after a contained fault: clear the feature's flag too.
void OnFault(const char* where) {
    FeatureDeactivate(kFeature, (std::string("fault in the ") + where).c_str());
}

void OnEntered(bool) { ++g_generation; }

}  // namespace

bool ProvinceBuildingRemovalInit() {
    if (!Engine::HaveProvinceBuildingRemoval()) {
        Log("%s: required engine functions or fields missing", kFeature);
        return false;
    }
    if (!MainThreadQueueAvailable()) {
        Log("%s: the main-thread queue is unavailable (clicks could not be deferred)", kFeature);
        return false;
    }
    // The patch first: without the button the hooks have nothing to do; without the hooks a patched
    // button would show and do nothing, so a failed hook leaves the file unpatched (init fails
    // before VfsStart, and features register their files from init only).
    if (!Engine::HookProvinceBuildingRemoval(&Allowed, &OnClick, &OnFault)) {
        Log("%s: hooks could not be queued", kFeature);
        return false;
    }
    if (!register_file_patch(L"interface/countrystateview.gui", &PatchGui)) {
        Log("%s: countrystateview.gui patch could not be registered; the cross will not appear", kFeature);
        return false;
    }
    OnGameEntered(&OnEntered);
    return true;
}
