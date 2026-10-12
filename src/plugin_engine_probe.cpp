// engine_probe (engine tier): a no-op diagnostic feature. It resolves two PhysFS signatures
// inside hoi4.exe and cross-checks them (openRead must call doOpen), but hooks and patches
// nothing. Its flag, hoi4_loader_plugin_engine_probe, tells script that engine-tier features
// are enabled on this hoi4.exe.
#include "loader.h"
#include "signatures.h"

bool EngineProbeInit() {
    uint8_t* doOpen = SigFindUnique("physfs_doOpen", kSigPhysfsDoOpen);
    uint8_t* openRead = SigFindUnique("physfs_platformOpenRead", kSigPhysfsOpenRead);
    if (!doOpen || !openRead) return false;
    uint8_t* callee = SigFollowRel32(openRead + kSigPhysfsOpenReadCallOffset);
    if (callee != doOpen) {
        Log("ERROR: engine_probe: openRead calls %s, expected doOpen at %s", SigDescribe(callee).c_str(),
            SigDescribe(doOpen).c_str());
        return false;
    }
    return true;
}
