#!/usr/bin/env bash
# Run the project's tests.
#
# Deliberately independent of the meson build: these tests compile the shipped
# functions on their own, so they run in a couple of seconds with nothing but a
# C++17 compiler. That keeps them useful as a fast gate on every push instead
# of something that only runs when a release is cut.
#
# Usage:
#   tests/run_tests.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
MENU_SRC="$ROOT/MangoHud/src/overlay_menu.cpp"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

CXX="${CXX:-g++}"
command -v "$CXX" >/dev/null 2>&1 || { echo "ERROR: no C++ compiler ($CXX)" >&2; exit 1; }

rc=0

echo "== lsfg-vk [global] handling =="
bash "$HERE/extract_functions.sh" "$MENU_SRC" "$WORK/extracted.inc" || exit 1
"$CXX" -std=c++17 -Wall -Wextra -I"$WORK" -o "$WORK/test" \
   "$HERE/test_lsfg_global_keys.cpp" 2>"$WORK/build.log"
if [ $? -ne 0 ] || grep -q ' error:' "$WORK/build.log"; then
   echo "ERROR: the test harness failed to compile:" >&2
   cat "$WORK/build.log" >&2
   exit 1
fi
# the harness writes its scratch dir relative to cwd
( cd "$WORK" && ./test ) || rc=1

echo
echo "== install.sh conf.toml healing =="
bash "$HERE/test_heal_dll_key.sh" "$ROOT/install.sh" || rc=1

echo
echo "== install.sh layer manifests =="
bash "$HERE/test_install_manifests.sh" "$ROOT/install.sh" || rc=1

echo
echo "== version references =="
bash "$HERE/test_version_consistency.sh" "$ROOT" || rc=1

echo
echo "== shell syntax =="
for f in "$ROOT/install.sh" "$ROOT/uninstall.sh"; do
   if bash -n "$f"; then echo "PASS  $(basename "$f") parses"; else
      echo "FAIL  $(basename "$f") does not parse"; rc=1
   fi
done
if sh -n "$ROOT/mangoverlay"; then echo "PASS  mangoverlay parses (POSIX sh)"; else
   echo "FAIL  mangoverlay does not parse"; rc=1
fi

echo
[ "$rc" = 0 ] && echo "OK" || echo "FAILURES"
exit "$rc"
