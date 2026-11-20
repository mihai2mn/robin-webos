#!/bin/sh
# Apply the local webos-native-app-kit changes on top of the submodule's upstream commit.
# Run after `git submodule update --init`. Idempotent.
#
# At pivot 8d53a5b the kit ALREADY CONTAINS all five changes: 0001/0003/0004 apply verbatim (so they
# reverse-apply), and 0002/0005 were integrated upstream and then EXTENDED (0002's
# `...GLIBC_RUNTIMES" ${ARGN})` is upstream as `...GLIBC_RUNTIMES;SYSTEM_LIBS"`, and 0005's SYSTEM_LIBS
# arrived with it). Their hunks therefore no longer match context, but their behaviour is present.
#
# This script therefore treats "reverse-applies" OR "all of its added identifiers are present in the
# likely target files" as already-applied, and only applies a patch when it genuinely still applies.
# A patch that neither applies nor has its content is a hard error (the submodule moved).
set -e
here=$(cd "$(dirname "$0")" && pwd)
kit="$here/../webos-native-app-kit"
cmake="$kit/cmake/WebOSPackage.cmake"
chk="$kit/tools/check-package.py"

# Content of 0002 and 0005: every identifier they introduce must be present, in the two files they touch.
content_0002_0005_present() {
    [ -f "$cmake" ] && [ -f "$chk" ] || return 1
    grep -q "GLIBC_RUNTIMES" "$cmake"      || return 1
    grep -q -- "--glibc-runtime" "$cmake"  || return 1
    grep -q -- "--glibc-runtime" "$chk"    || return 1
    grep -q "FIRMWARE_GLIBC" "$chk"        || return 1
    grep -q "SYSTEM_LIBS" "$cmake"         || return 1
    grep -q -- "--system-lib" "$cmake"     || return 1
    grep -q -- "--system-lib" "$chk"       || return 1
    return 0
}

for p in "$here"/webos-native-app-kit/*.patch; do
    base=$(basename "$p")
    if git -C "$kit" apply --check --reverse "$p" >/dev/null 2>&1; then
        echo "already applied: $base"
    elif git -C "$kit" apply --check "$p" >/dev/null 2>&1; then
        echo "applying: $base"
        git -C "$kit" apply "$p"
    elif content_0002_0005_present; then
        echo "already upstream (integrated + extended): $base"
    else
        echo "ERROR: $base neither applies nor is present; the submodule moved" >&2
        exit 1
    fi
done
