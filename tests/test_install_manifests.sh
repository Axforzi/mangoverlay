#!/usr/bin/env bash
# Tests for remove_stale_lsfg_manifests() in install.sh.
#
# The function is extracted from the real script and run against a scratch
# directory, so this tests the shipped logic and not a paraphrase of it.
#
# Why it matters: a duplicate Vulkan layer manifest is invisible. The loader
# keeps one copy and silently discards the other, warning on every process
# start. Nothing fails; the layer order the project depends on is just quietly
# no longer guaranteed.
set -uo pipefail

SRC="${1:?usage: test_install_manifests.sh <path-to-install.sh>}"
[ -f "$SRC" ] || { echo "ERROR: no such file: $SRC" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail=0
check() { if [ "$1" = 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; fail=1; fi; }

# install.sh's info() is not part of the extracted function; stub it so the
# harness does not abort on command-not-found.
info() { printf '%s\n' "$*" >&2; }

sed -n '/^remove_stale_lsfg_manifests() {/,/^}/p' "$SRC" > "$WORK/fn.sh"
if ! grep -q 'rm -f' "$WORK/fn.sh"; then
   echo "ERROR: could not extract remove_stale_lsfg_manifests from $SRC" >&2
   echo "       either it moved or the formatting assumption broke" >&2
   exit 1
fi

VK_DIR="$WORK/vk"
mkdir -p "$VK_DIR"
# shellcheck source=/dev/null
. "$WORK/fn.sh"

out="$(remove_stale_lsfg_manifests 2>&1)"
[ -z "$out" ]; check $? "silent when there is no stale manifest"

touch "$VK_DIR/VkLayer_LSFGVK_frame_generation.json"
out="$(remove_stale_lsfg_manifests 2>&1)"
[ ! -f "$VK_DIR/VkLayer_LSFGVK_frame_generation.json" ]
check $? "removes a stale 64-bit manifest"
printf '%s' "$out" | grep -q 'removed stale duplicate manifest'
check $? "reports what it removed"

touch "$VK_DIR/VkLayer_LSFGVK_frame_generation.x86.json"
remove_stale_lsfg_manifests >/dev/null 2>&1
[ ! -f "$VK_DIR/VkLayer_LSFGVK_frame_generation.x86.json" ]
check $? "removes a stale 32-bit manifest"

# only manifests this project owns may be touched
touch "$VK_DIR/VkLayer_MAKO_render.json" \
      "$VK_DIR/VkLayer_MAKO_spatial_scaling.json" \
      "$VK_DIR/MangoHud_overlay_unified.json" \
      "$VK_DIR/steamoverlay_x86_64.json"
remove_stale_lsfg_manifests >/dev/null 2>&1
all_kept=0
for f in VkLayer_MAKO_render.json VkLayer_MAKO_spatial_scaling.json \
         MangoHud_overlay_unified.json steamoverlay_x86_64.json; do
   [ -f "$VK_DIR/$f" ] || all_kept=1
done
check $all_kept "leaves manifests belonging to other projects alone"

# shellcheck disable=SC1090
grep -q '^remove_stale_lsfg_manifests() {' "$SRC"
check $? "the function still exists in install.sh"

def_line=$(grep -n '^remove_stale_lsfg_manifests() {' "$SRC" | cut -d: -f1)
calls=$(grep -c '^[[:space:]]*remove_stale_lsfg_manifests$' "$SRC")
[ "$calls" -eq 2 ]
check $? "called on both the prebuilt and the local-build path (found $calls)"

first_call=$(grep -n '^[[:space:]]*remove_stale_lsfg_manifests$' "$SRC" | head -1 | cut -d: -f1)
[ "$def_line" -lt "$first_call" ]
check $? "defined before its first call (def:$def_line call:$first_call)"

echo
[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILED"
exit "$fail"
