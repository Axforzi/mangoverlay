#!/usr/bin/env bash
# Tests for heal_conf_toml_dll_key() in install.sh.
#
# The functions are extracted from the real script and run against scratch
# files, so this exercises the shipped logic.
#
# The invariant that matters: a missing `dll` key gets added, and NOTHING else
# about the user's config changes. conf.toml holds one [[profile]] per game with
# the multiplier, pacing and env vars they tuned, so a rewrite to add one key
# would throw all of that away.
set -uo pipefail

SRC="${1:?usage: test_heal_dll_key.sh <path-to-install.sh>}"
[ -f "$SRC" ] || { echo "ERROR: no such file: $SRC" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail=0
check() { if [ "$1" = 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; fail=1; fi; }

info() { printf '%s\n' "$*" >&2; }
warn() { printf '%s\n' "$*" >&2; }

sed -n '/^conf_toml_has_dll_key() {/,/^}/p' "$SRC" > "$WORK/has.sh"
sed -n '/^heal_conf_toml_dll_key() {/,/^}/p' "$SRC" > "$WORK/heal.sh"
for f in has heal; do
   if [ ! -s "$WORK/$f.sh" ]; then
      echo "ERROR: could not extract the $f function from $SRC" >&2
      exit 1
   fi
done

CONF_TOML="$WORK/conf.toml"
DLL_PATH="$WORK/lsfg-vk.dll"
printf 'x' > "$DLL_PATH"
# shellcheck source=/dev/null
. "$WORK/has.sh"
# shellcheck source=/dev/null
. "$WORK/heal.sh"

# a realistic config: a global block plus two game profiles
write_config() {
   cat > "$CONF_TOML" <<'EOF'
version = 2

[global]
allow_fp16 = false


[[profile]]
active_in = "BatmanAK.exe"
flow_scale = 1.0
multiplier = 2
name = "BatmanAK.exe"
pacing_mode = "vsync"


[[profile]]
active_in = "HollowKnight.exe"
flow_scale = 1.0
multiplier = 3
name = "HollowKnight.exe"
pacing_mode = "vsync"
EOF
}

dll_of() { grep -E '^\s*dll\s*=' "$CONF_TOML" | head -1 | sed 's/^[[:space:]]*//'; }

# 1. missing key is added
write_config
heal_conf_toml_dll_key >/dev/null 2>&1
[ "$(dll_of)" = "dll = \"$DLL_PATH\"" ]
check $? "adds the missing dll key"

# 2. the key lands inside [global], before the first profile
awk '/^\[\[profile\]\]/{exit} /dll[[:space:]]*=/{found=1} END{exit(found?0:1)}' "$CONF_TOML"
check $? "the key is inside [global], not after a profile"

# 3. nothing else changed
write_config
cp "$CONF_TOML" "$WORK/before"
heal_conf_toml_dll_key >/dev/null 2>&1
[ "$(grep -c '^\[\[profile\]\]' "$CONF_TOML")" = 2 ]
check $? "both profiles survive"
grep -q 'multiplier = 2' "$CONF_TOML" && grep -q 'multiplier = 3' "$CONF_TOML"
check $? "per-game multipliers are untouched"
[ "$(grep -vE '^(#|$)' "$WORK/before" | grep -cv '^\s*')" = "$(grep -vE '^(#|$)' "$CONF_TOML" | grep -cv '^\s*')" ]
check $? "the number of settings lines is unchanged apart from the added key"
[ "$(diff <(sed '/^\s*dll\s*=/d' "$CONF_TOML") "$WORK/before" | grep -c '^[<>]')" = 0 ]
check $? "removing the added key reproduces the original file byte for byte"

# 4. an existing key is never overwritten
write_config
sed -i 's/^\[global\]$/[global]\ndll = "\/my\/own\/path.dll"/' "$CONF_TOML"
heal_conf_toml_dll_key >/dev/null 2>&1
[ "$(dll_of)" = 'dll = "/my/own/path.dll"' ]
check $? "a dll the user set is left alone"
[ "$(grep -c '^\s*dll\s*=' "$CONF_TOML")" = 1 ]
check $? "no second dll key is added"

# 5. a dll key inside a profile must not count as the global one
write_config
printf '\n[[profile]]\nactive_in = "Other.exe"\ndll = "profile.dll"\nmultiplier = 1\n' >> "$CONF_TOML"
heal_conf_toml_dll_key >/dev/null 2>&1
[ "$(dll_of)" = "dll = \"$DLL_PATH\"" ]
check $? "a profile-scoped dll does not satisfy the global key"

# 6. no DLL resolved leaves the config byte-identical
write_config
cp "$CONF_TOML" "$WORK/before"
DLL_PATH="" heal_conf_toml_dll_key >/dev/null 2>&1
cmp -s "$WORK/before" "$CONF_TOML"
check $? "an install with no dll resolved changes nothing"

# 7. a dll pointing at a missing file changes nothing
write_config
cp "$CONF_TOML" "$WORK/before"
DLL_PATH="$WORK/nope.dll" heal_conf_toml_dll_key >/dev/null 2>&1
cmp -s "$WORK/before" "$CONF_TOML"
check $? "a dll path that does not exist changes nothing"

# 8. a config with no [global] is reported, not mangled
printf 'version = 2\n\n[[profile]]\nactive_in = "X.exe"\nmultiplier = 2\n' > "$CONF_TOML"
cp "$CONF_TOML" "$WORK/before"
out="$(heal_conf_toml_dll_key 2>&1)"
cmp -s "$WORK/before" "$CONF_TOML"
check $? "a config with no [global] is left untouched"
printf '%s' "$out" | grep -q 'no \[global\] section'
check $? "and says so"

# 9. the healing is wired into the install sequence
grep -q '^    heal_conf_toml_dll_key$' "$SRC"
check $? "install.sh calls heal_conf_toml_dll_key in config_dll_and_configs"

echo
[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILED"
exit "$fail"
