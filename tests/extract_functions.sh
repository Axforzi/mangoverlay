#!/usr/bin/env bash
# Extract the functions under test from overlay_menu.cpp.
#
# The tests compile the real functions rather than a copy of them, so a change
# to the shipped code is what gets tested. The functions are pulled out with
# awk because overlay_menu.cpp cannot be compiled standalone: it pulls in ImGui,
# the Vulkan dispatch table and the whole overlay state.
#
# This relies on the project's formatting: the return type sits on its own line
# above the signature, and the body ends at the first `}` in column 1. If that
# ever stops holding, the extraction fails loudly below rather than silently
# testing nothing.
set -euo pipefail

SRC="${1:?usage: extract_functions.sh <path-to-overlay_menu.cpp> <output.inc>}"
OUT="${2:?usage: extract_functions.sh <path-to-overlay_menu.cpp> <output.inc>}"

[ -f "$SRC" ] || { echo "ERROR: no such source file: $SRC" >&2; exit 1; }

# The set the tests exercise. Keep in sync with the harness.
FUNCS="menu_trim menu_toml_key menu_toml_value
       lsfg_config_path lsfg_config_dir lsfg_file_exists
       lsfg_read_first_line lsfg_find_dll
       find_global_key_line find_global_header_line
       lsfg_ensure_dll_key lsfg_ensure_fp16_key"

extract() {
   awk -v fn="$1" '
      # signature line: the name at column 0, immediately after its return type
      $0 ~ ("^" fn "\\(") {
         grab = 1
         print ""
         print "/* --- " fn " --- */"
         if (prev != "") print prev          # the return type
         print
         next
      }
      grab && /^}[[:space:]]*$/ { print; grab = 0; prev = ""; next }
      grab { print; next }
      { prev = $0 }
   ' "$SRC"
}

: > "$OUT"
for f in $FUNCS; do extract "$f" >> "$OUT"; done

# A missing function means the tests would silently cover less than they claim.
for f in $FUNCS; do
   if ! grep -q "^${f}(" "$OUT"; then
      echo "ERROR: could not extract '$f' from $SRC" >&2
      echo "       either it moved or the formatting assumption broke" >&2
      exit 1
   fi
done

echo "extracted $(grep -c '^/\* --- ' "$OUT") functions from $(basename "$SRC")"
