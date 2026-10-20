#!/usr/bin/env bash
# Regression tests for the launch wrapper's Steam library discovery.
#
# libraryfolders.vdf can point at a library whose path contains spaces, e.g.
# "/mnt/My Games/SteamLibrary". The old `for _lib in $(sed ...)` split on every
# whitespace, so the manifest was never found and the game silently fell back to
# a guessed section. This runs the shipped wrapper end to end against a fake
# HOME and checks the section it bootstraps.
set -uo pipefail

ROOT="${1:?usage: test_wrapper_paths.sh <repo-root>}"
WRAPPER="$ROOT/mangoverlay"
[ -f "$WRAPPER" ] || { echo "ERROR: no wrapper at $WRAPPER" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'chmod -R u+w "$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT
fail=0
check() { if [ "$1" = 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; fail=1; fi; }

HOME_DIR="$WORK/home"
CONF_DIR="$HOME_DIR/.config/lsfg-vk"
STEAM_APPS="$HOME_DIR/.local/share/Steam/steamapps"
LIB="$WORK/My Games/SteamLibrary"
LIB_APPS="$LIB/steamapps"
GAME_DIR="$LIB_APPS/common/Some Game"

mkdir -p "$CONF_DIR" "$STEAM_APPS" "$GAME_DIR"
: > "$CONF_DIR/env.conf"
printf 'version = 2\n\n[global]\n' > "$CONF_DIR/conf.toml"

# The library list lives in the DEFAULT Steam install and points elsewhere.
cat > "$STEAM_APPS/libraryfolders.vdf" <<EOF
"libraryfolders"
{
	"0"
	{
		"path"		"$LIB"
	}
}
EOF

cat > "$LIB_APPS/appmanifest_123.acf" <<'EOF'
"AppState"
{
	"installdir"		"Some Game"
}
EOF

: > "$GAME_DIR/Some Game.exe"
chmod +x "$GAME_DIR/Some Game.exe"

run_wrapper() {
   HOME="$HOME_DIR" \
   XDG_CONFIG_HOME="$HOME_DIR/.config" \
   SteamAppId=123 \
   MANGOVERLAY_ENV_CONF="$CONF_DIR/env.conf" \
   PATH="/usr/bin:/bin" \
   "$WRAPPER" /bin/true
}

run_wrapper
grep -q '^\[Some Game\.exe\]$' "$CONF_DIR/env.conf"
check $? "finds a Steam library whose path contains spaces"

grep -q '^LSFGVK_PROFILE="Some Game\.exe"$' "$CONF_DIR/env.conf"
check $? "bootstraps the section with its profile name"

run_wrapper
[ "$(grep -c '^\[Some Game\.exe\]$' "$CONF_DIR/env.conf")" = 1 ]
check $? "a second launch does not duplicate the section"

grep -q '^name = "Some Game\.exe"$' "$CONF_DIR/conf.toml"
check $? "the matching [[profile]] is created in conf.toml"

if command -v flock >/dev/null 2>&1; then
   [ -f "$CONF_DIR/.mangoverlay.lock" ]
   check $? "bootstrap takes the shared config lock"
fi

echo
[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILED"
exit "$fail"
