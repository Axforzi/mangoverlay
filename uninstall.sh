#!/usr/bin/env bash
# ============================================================================
# uninstall.sh -- Removes the unified overlay (MangoHud fork) + mangoverlay
#                 wrapper + lsfg-vk (upstream) layer installed by install.sh.
#
# Usage:
#   One-liner:
#     curl -fsSL https://raw.githubusercontent.com/Axforzi/mangoverlay/v0.1.2/uninstall.sh | bash
#   Or from a local clone:
#     ./uninstall.sh [--prefix <dir>] [--purge-configs] [--yes] [--help]
#
# Flags:
#   --prefix <dir>    Same base used at install time (default: $HOME).
#                     Binaries/libs/layers that live under <prefix>/.local are
#                     removed from there; the /usr/local/bin symlink is only
#                     removed when it points at our own binary.
#   --purge-configs   ALSO remove ~/.config/lsfg-vk/ (conf.toml + env.conf,
#                     i.e. every per-game profile). Off by default: configs
#                     are user data and survive an uninstall.
#   --yes             Ask nothing; remove everything without prompts.
#   --help            Show this help
#
# What is removed:
#   · ~/.local/lib/mangoverlay/          (libMangoHud.so, libMangoHud_opengl.so)
#   · <prefix>/.local/share/vulkan/implicit_layer.d/MangoHud_overlay_unified.json
#   · <prefix>/.local/bin/mangoverlay
#   · /usr/local/bin/mangoverlay          (only the symlink we created)
#   · <prefix>/.local/lib*/liblsfg-vk-layer.so  (lsfg-vk layer)
#   · <prefix>/.local/bin/lsfg-vk-cli     (installed by install.sh)
#   · $XDG_CONFIG_HOME/vulkan/implicit_layer.d/VkLayer_LSFGVK_frame_generation.json
#   · ~/.cache/mangoverlay/               (lsfg-vk source clone)
#   · the PATH block appended to ~/.zshrc / ~/.bashrc by install.sh
#
# What is NOT removed (by design):
#   · ~/.config/lsfg-vk/                  (unless --purge-configs)
#   · the source tree / build dir / git clone you installed from
#   · any lsfg-vk files you installed manually outside install.sh
#     (kept here: lsfg-vk-ui and its .desktop entry from an earlier build)
#
# Safe for friends: only removes files whose paths are derived from
# $PREFIX/$HOME plus the exact names install.sh creates, and only removes
# the /usr/local/bin symlink when it still points at our own binary.
# ============================================================================

set -euo pipefail

# --- Colors / output ---------------------------------------------------------
C_RESET='\033[0m'; C_INFO='\033[1;32m'; C_WARN='\033[1;33m'
C_ERR='\033[1;31m'; C_STEP='\033[1;36m'; C_BOLD='\033[1m'
if [ -n "${NO_COLOR:-}" ] || [ ! -t 1 ]; then
    C_RESET=''; C_INFO=''; C_WARN=''; C_ERR=''; C_STEP=''; C_BOLD=''
fi

info()  { printf "%b" "${C_INFO}[INFO]${C_RESET} " >&2; printf '%b\n' "$*" >&2; }
warn()  { printf "%b" "${C_WARN}[WARN]${C_RESET} " >&2; printf '%b\n' "$*" >&2; }
err()   { printf "%b" "${C_ERR}[ERROR]${C_RESET} " >&2; printf '%b\n' "$*" >&2; }
step()  { printf "%b\n" "${C_STEP}── ${1} ──${C_RESET}"; }
die()   { err "$*"; exit 1; }
have()  { command -v "$1" >/dev/null 2>&1; }

# --- Flags -------------------------------------------------------------------
PURGE_CONFIGS=0; YES=0
PREFIX="$HOME"

usage() {
    if [ -r "$0" ]; then sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'
    else
        info "Usage:"
        printf '   curl -fsSL https://raw.githubusercontent.com/Axforzi/mangoverlay/v0.1.2/uninstall.sh | bash\n'
        printf '   uninstall.sh [--prefix <dir>] [--purge-configs] [--yes] [--help]\n'
    fi
    exit 0
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --purge-configs) PURGE_CONFIGS=1 ;;
        --yes)           YES=1 ;;
        --prefix)        PREFIX="$2"; shift ;;
        -h|--help)       usage ;;
        *) die "Unknown option: $1 (see --help)" ;;
    esac
    shift
done

case "$PREFIX" in
    /*) : ;;
    *)  PREFIX="$PWD/$PREFIX" ;;
esac
if [ -d "$PREFIX" ]; then
    PREFIX=$(CDPATH= cd -- "$PREFIX" && pwd)
fi

# --- Paths (must mirror install.sh exactly) ----------------------------------
BIN_DIR="$PREFIX/.local/bin"
LIB_DIR="$PREFIX/.local/lib/mangoverlay"
LIB_ROOT="$PREFIX/.local/lib"
LIB64_ROOT="$PREFIX/.local/lib64"
VK_DIR="$PREFIX/.local/share/vulkan/implicit_layer.d"
VK_CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/vulkan/implicit_layer.d"
VK_LAYER_JSON="$VK_DIR/MangoHud_overlay_unified.json"
BIN_MANGOVERLAY="$BIN_DIR/mangoverlay"
BIN_LSFG_CLI="$BIN_DIR/lsfg-vk-cli"
CFG_DIR="$HOME/.config/lsfg-vk"
LSFG_JSON="$VK_CONFIG_DIR/VkLayer_LSFGVK_frame_generation.json"
LSFG_CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/mangoverlay"
USR_BIN_LINK="/usr/local/bin/mangoverlay"

# --- Utilities ---------------------------------------------------------------
ask_yes_no() {
    # ask_yes_no "<prompt>" <default: y|n>  → 0 = yes, 1 = no
    local prompt="$1" default="$2" ans
    if [ "$YES" -eq 1 ]; then
        [ "$default" = "y" ]; return $?
    fi
    while :; do
        printf '%s ' "$prompt"
        if ! read -r ans; then
            [ "$default" = "y" ]; return $?
        fi
        [ -z "$ans" ] && ans="$default"
        case "$ans" in
            [yY]*) return 0 ;;
            [nN]*) return 1 ;;
            *) err "Answer 'y' or 'n'." ;;
        esac
    done
}

run_admin() {
    # Runs a command as root (direct if already root)
    if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi
}

rm_file() {
    # rm_file <path> — removes one file, reports what happened. rm -f so a
    # missing path is not an error; guards against -rf on broad directories.
    if [ -f "$1" ] || [ -L "$1" ]; then
        rm -f -- "$1" && info "removed: $1"
    fi
}

rm_dir_if_empty() {
    # rm_dir_if_empty <dir> — removes the directory only when it is empty
    # (never rm -rf anything derived from a prefix dir).
    if [ -d "$1" ]; then
        rmdir -- "$1" 2>/dev/null && info "removed empty dir: $1" || true
    fi
}

# --- 1/5 Confirm ------------------------------------------------------------
step "Uninstall: unified overlay + mangoverlay + lsfg-vk"
info "Install prefix: $PREFIX/.local"
info "Configs: $CFG_DIR  (kept unless --purge-configs)"
echo

if [ "$YES" -eq 0 ]; then
    warn "This removes the overlay layer, the mangoverlay wrapper, lsfg-vk"
    warn "and its source cache. Your per-game configs are NOT removed unless"
    warn "you pass --purge-configs."
    if ! ask_yes_no "Uninstall now? [y/N]" n; then
        info "Cancelled by the user."
        exit 0
    fi
    if ! ask_yes_no "Confirm removing the installed files? (cannot be undone) [y/N]" n; then
        info "Cancelled by the user."
        exit 0
    fi
fi

# --- 2/5 Remove the overlay layer -------------------------------------------
step "Removing the overlay (MangoHud fork) layer"
rm_file "$VK_LAYER_JSON"
rm_file "$LIB_DIR/libMangoHud.so"
rm_file "$LIB_DIR/libMangoHud_opengl.so"
rm_dir_if_empty "$LIB_DIR"
# also strip an old pre-unified MangoHud JSON name the fork used to write
rm_file "$VK_DIR/MangoHud.json"

# --- 3/5 Remove the wrapper ---------------------------------------------------
step "Removing the mangoverlay wrapper"
rm_file "$BIN_MANGOVERLAY"
# Remove only the known symlink we created for Steam; never follow/rchmod
# a real binary or someone else's link.
if [ -L "$USR_BIN_LINK" ]; then
    # Compare against the exact path install.sh created, so a dangling
    # symlink (binary already removed) is still recognized as ours.
    link_target="$(readlink "$USR_BIN_LINK" 2>/dev/null || true)"
    if [ "$link_target" = "$BIN_MANGOVERLAY" ] ||
       { [ "$(basename "$link_target" 2>/dev/null || true)" = "mangoverlay" ] &&
         [ "$(dirname "$link_target" 2>/dev/null || true)" = "$BIN_DIR" ]; }; then
        # The symlink previously pointed at our binary; since we removed it,
        # remove the dangling link (target may be gone).
        if [ -w "$(dirname "$USR_BIN_LINK")" ]; then
            rm -f -- "$USR_BIN_LINK" && info "removed: $USR_BIN_LINK"
        else
            run_admin rm -f -- "$USR_BIN_LINK"
            info "removed (root): $USR_BIN_LINK"
        fi
    else
        warn "Not removing $USR_BIN_LINK: it does not point at our managed binary."
    fi
elif [ -e "$USR_BIN_LINK" ]; then
    warn "Not removing $USR_BIN_LINK: it is a real file, not our symlink."
fi

# --- 4/5 Remove lsfg-vk ------------------------------------------------------
step "Removing lsfg-vk (upstream) layer and tools"
rm_file "$LSFG_JSON"
rm_file "$BIN_LSFG_CLI"
rm_file "$LIB_ROOT/liblsfg-vk-layer.so"
rm_file "$LIB_ROOT/liblsfg-vk-layer.x86.so"
rm_file "$LIB64_ROOT/liblsfg-vk-layer.so"
rm_file "$VK_DIR/VkLayer_LSFGVK_frame_generation.json"
rm_dir_if_empty "$VK_DIR"
rm_dir_if_empty "$VK_CONFIG_DIR"
if [ -d "$LSFG_CACHE_DIR" ]; then
    info "removing lsfg-vk source cache: $LSFG_CACHE_DIR"
    rm -rf -- "$LSFG_CACHE_DIR"
fi

# --- 5/5 Configs + PATH block ------------------------------------------------
step "Configs and shell PATH"
if [ "$PURGE_CONFIGS" -eq 1 ]; then
    if [ -d "$CFG_DIR" ]; then
        info "purging configs: $CFG_DIR"
        rm -rf -- "$CFG_DIR"
    fi
else
    if [ -d "$CFG_DIR" ] || [ -f "$CFG_DIR/conf.toml" ] || [ -f "$CFG_DIR/env.conf" ]; then
        info "Kept configs: $CFG_DIR  (pass --purge-configs to remove them)"
    fi
fi

# Remove the PATH block install.sh appends to the shell rc.
# Matching exactly the two marker lines the installer wrote.
for rc in "$HOME/.zshrc" "$HOME/.bashrc"; do
    [ -f "$rc" ] || continue
    if grep -q '^# >>> mangoverlay installer >>>' "$rc" 2>/dev/null; then
        info "Removing mangoverlay PATH block from $rc"
        if [ -w "$rc" ]; then
            sed -i '/^# >>> mangoverlay installer >>>/,/^# <<< mangoverlay installer <<</d' "$rc"
        else
            warn "Cannot write $rc; remove the mangoverlay PATH block manually."
        fi
    fi
done

rm_dir_if_empty "$BIN_DIR"
rm_dir_if_empty "$LIB_ROOT"
rm_dir_if_empty "$LIB64_ROOT"

# --- Final summary -----------------------------------------------------------
printf "\n%b\n" "${C_STEP}══════════════ Uninstall summary ══════════════${C_RESET}"
info "Removed: overlay layer, mangoverlay wrapper, lsfg-vk layer/tools, source cache."
if [ "$PURGE_CONFIGS" -eq 1 ]; then
    info "Configs purged: $CFG_DIR"
else
    info "Configs kept:   $CFG_DIR (run with --purge-configs to remove them)"
fi
info "Done."