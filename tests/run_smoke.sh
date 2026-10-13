#!/bin/sh
# Out-of-game smoke test for hoi4_loader. Run from Git Bash after building:
#   cmake --build build --config Release
#   sh tests/run_smoke.sh [build-dir]
#
# Runs build/smoke/{sig,nosig}/hoi4.exe (stand-ins that load version.dll like the game does)
# through every scenario and checks both their results and the loader log lines. The hash
# cache goes to an isolated %LOCALAPPDATA% under the build dir. The real loader.log is
# overwritten unless the game is running (then the loader writes loader.<pid>.log, which
# this script deletes afterwards). Note: scenario 3 touches and scenario 8 appends a byte to
# the stand-in executables; that only affects the test binaries.
BUILD="$(cd "${1:-$(dirname "$0")/../build}" && pwd)"
SIG="$BUILD/smoke/sig"
NOSIG="$BUILD/smoke/nosig"
LA="$BUILD/smoke/localappdata"
CACHE="$LA/hoi4_loader/hash_cache.ini"
DOCS="$(cygpath -u "$(powershell -NoProfile -Command "[Environment]::GetFolderPath('MyDocuments')" | tr -d '\r')")/Paradox Interactive/Hearts of Iron IV"
rc=0

echo "===== 0) native script API core (fake engine backend)"
out=$("$BUILD/tests/native_script_test.exe"); trc=$?
echo "$out" | grep -E 'FAIL|PASSED|FAILED'
[ "$trc" = 0 ] || rc=1

for d in "$SIG" "$NOSIG"; do
    [ -f "$d/hoi4.exe" ] && [ -f "$d/version.dll" ] || { echo "missing $d/hoi4.exe or version.dll; build first"; exit 2; }
done
rm -rf "$LA"; mkdir -p "$LA"

newest_log() { ls -t "$DOCS"/loader.log "$DOCS"/loader.*.log 2>/dev/null | head -1; }
expect_log() {  # expect_log <description> <fixed string>
    if grep -qF -- "$2" "$(newest_log)"; then echo "  ok   log: $1"; else echo "  FAIL log: $1 (missing: $2)"; rc=1; fi
}
run() {  # run <dir> <engine|file> <title>
    echo "===== $3"
    (cd "$1" && LOCALAPPDATA="$(cygpath -w "$LA")" ./hoi4.exe "$2") || rc=1
}
hash_of() { sha256sum "$1/hoi4.exe" | cut -d' ' -f1; }
ini_listed() { printf '[supported_hashes]\n; comment\nother = %064d\nsmoke_build = %s ; test build\n' 0 "$(hash_of "$1" | tr a-f A-F)" > "$1/loader_config.ini"; }
ini_unlisted() { printf '[supported_hashes]\nother = %064d\n' 0 > "$1/loader_config.ini"; }

ini_listed "$SIG"
run "$SIG" engine "1) supported hash + signatures present -> engine tier active (first run, no cache)"
expect_log "label of matching entry" "Supported hoi4.exe build: smoke_build"
expect_log "cache miss, no entry" "cache not used: no entry for this path"
expect_log "engine_probe active" "Feature engine_probe (engine tier): active"
expect_log "signature resolved" "Signature 'physfs_platformOpenRead' -> hoi4.exe+0x"
expect_log "lifecycle fails safe without its signatures" "Feature lifecycle (engine tier): FAILED, left inactive"
expect_log "region_names skipped without lifecycle" "Feature region_names (engine tier): DISABLED, requires lifecycle"
expect_log "game_loaded skipped without lifecycle" "Feature game_loaded (engine tier): DISABLED, requires lifecycle"

run "$SIG" engine "2) same exe again -> hash from cache"
expect_log "cache hit" "(from hash cache,"

touch "$SIG/hoi4.exe"
run "$SIG" engine "3) exe last-write time changed -> rehash"
expect_log "cache invalidated by mtime" "cache not used: last-write time changed"

python -I -c "import sys,re;p=sys.argv[1];s=open(p,encoding='utf-16').read();open(p,'w',encoding='utf-16').write(re.sub(r'sha256=[0-9a-f]+','sha256=nothex',s))" "$CACHE"
run "$SIG" engine "4) corrupt cache entry -> rehash"
expect_log "corrupt entry detected" "cache not used: entry is corrupt"

ini_unlisted "$SIG"
run "$SIG" file "5) unknown hash -> file tier only, engine tier disabled"
expect_log "engine feature disabled" "Feature engine_probe (engine tier): DISABLED, hoi4.exe hash not supported"
expect_log "summary of disabled features" "Engine-tier features disabled: engine_probe"
expect_log "file tier summary" "File-tier features stay active: core detection"
expect_log "ini line printed" "my_build = $(hash_of "$SIG")"

rm -f "$SIG/loader_config.ini"
run "$SIG" file "6) no loader_config.ini -> file tier only"
expect_log "missing ini warned" "loader_config.ini not found"
expect_log "engine feature disabled" "Engine-tier features disabled: engine_probe"

ini_listed "$NOSIG"
run "$NOSIG" file "7) supported hash but signatures missing -> feature fails safe"
expect_log "signature miss logged" "signature 'physfs_doOpen' matched 0 time(s), expected exactly 1"
expect_log "feature left inactive" "Feature engine_probe (engine tier): FAILED, left inactive"

printf 'X' >> "$NOSIG/hoi4.exe"
run "$NOSIG" file "8) exe size changed -> rehash, hash now unknown"
expect_log "cache invalidated by size" "cache not used: size changed"
expect_log "engine feature disabled" "Engine-tier features disabled: engine_probe"

rm -f "$DOCS"/loader.[0-9]*.log
echo "OVERALL: $([ $rc = 0 ] && echo PASSED || echo FAILED)"
exit $rc
