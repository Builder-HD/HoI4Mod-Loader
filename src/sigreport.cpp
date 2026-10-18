// Read-only signature report: on a hoi4.exe build that loader_config.ini does not
// list (or with [diagnostics] signature_scan = 1), count how every SIG and ANCHOR in
// signatures.h matches, run the resolver to check every CODE shape where it is used, and log a
// summary plus what to send when reporting a new game version. Nothing is hooked or patched.
#include "loader.h"

#include "engine.h"

#include <string>
#include <vector>

namespace {

struct Entry {
    const char* name;
    const char* pattern;
    char kind;  // 'S' signature, 'A' anchor, 'C' code shape
};

std::vector<Entry>& Entries() {
    static std::vector<Entry> v;
    return v;
}

struct Add {
    Add(const char* name, const char* pattern, char kind) { Entries().push_back({name, pattern, kind}); }
};

}  // namespace

// Every entry of signatures.h, in file order. Its other constants are not needed here.
#define SIG(name, pattern) static const Add name##_entry(#name, pattern, 'S');
#define ANCHOR(name, text) static const Add name##_entry(#name, text, 'A');
#define CODE(name, pattern) static const Add name##_entry(#name, pattern, 'C');
#include "signatures.h"

namespace {

// The resolver only reads hoi4.exe's code and RTTI, but on an unlisted build an offset decoded
// from changed code can point anywhere: contain a fault instead of crashing the game.
bool ResolveContained(DWORD* code) {
    __try {
        Engine::Resolve();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

}  // namespace

void SigReportBegin(bool unlisted) {
    Log("Signature report (%s): read-only, nothing is hooked or patched", unlisted ? "unlisted hoi4.exe build"
                                                                                 : "[diagnostics] signature_scan = 1");
    int total = 0, ok = 0;
    std::vector<std::string> bad;
    for (const Entry& e : Entries()) {
        if (e.kind == 'C') continue;
        ++total;
        char why[96] = "";
        if (e.kind == 'S') {
            int n = SigCount(e.pattern);
            if (n == 1) ++ok;
            else sprintf_s(why, n < 0 ? "cannot be scanned" : "matches %d times", n);
        } else {
            int strHits = 0;
            int refs = AnchorCount(e.pattern, &strHits);
            if (strHits == 1 && refs == 1) ++ok;
            else if (strHits != 1) sprintf_s(why, "string found %d times", strHits);
            else sprintf_s(why, "%d code references", refs);
        }
        if (*why) bad.push_back(std::string(e.name) + ": " + why);
    }
    Log("Signature report: %d/%d signatures and anchors match exactly once", ok, total);
    for (const std::string& b : bad) Log("Signature report:   FAILS %s", b.c_str());
    SigRecordShapes(true);
}

bool SigReportResolve() {
    DWORD code = 0;
    if (ResolveContained(&code)) return true;
    Log("Signature report: the resolver faulted (exception 0x%08lX); code shapes after that point are not checked", code);
    return false;
}

void SigReportEnd(bool unlisted) {
    SigRecordShapes(false);
    int total = 0, ok = 0;
    std::vector<std::string> failed, unchecked;
    for (const Entry& e : Entries()) {
        if (e.kind != 'C') continue;
        ++total;
        int r = SigShapeResult(e.pattern);
        if (r == 1) ++ok;
        else (r == 0 ? failed : unchecked).push_back(e.name);
    }
    Log("Signature report: %d/%d code shapes match where the resolver checks them", ok, total);
    for (const std::string& n : failed) Log("Signature report:   FAILS %s (checked, different code)", n.c_str());
    for (const std::string& n : unchecked)
        Log("Signature report:   NOT CHECKED %s (the code it belongs to was not found)", n.c_str());
    if (!unlisted) return;
    Log("To report this game version, send these files (nothing else is needed, never hoi4.exe):");
    Log("  1. this loader.log (Documents\\Paradox Interactive\\Hearts of Iron IV\\loader.log)");
    Log("  2. logs\\error.log from the same folder");
    Log("  3. the game version shown in the main menu (bottom left), e.g. v1.19.3");
    Log("The 'my_build = ...' line above is this hoi4.exe's SHA-256; a release lists it once the build is checked.");
}
