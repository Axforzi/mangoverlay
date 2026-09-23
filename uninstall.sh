#!/usr/bin/env bash
# ============================================================================
# uninstall.sh -- Thin entry point for removing the unified overlay.
#
# The real uninstall logic lives in `mangoverlay uninstall` (the installed
# wrapper self-locates its own prefix). This script just finds that binary
# and delegates; if the wrapper is missing or broken it prints the exact
# paths left behind so you can remove them manually.
#
# Usage:
#   One-liner:
#     curl -fsSL https://raw.githubusercontent.com/Axforzi/mangoverlay/v0.1.8/uninstall.sh | bash
#   Or from a local clone:
#     ./uninstall.sh [--prefix <dir>] [--purge-configs] [--yes] [--help]
#
# Flags are forwarded to `mangoverlay uninstall`, except:
#   --prefix <dir>   Where the install lives (default: $HOME). Used only to
#                    locate the wrapper; the wrapper self-locates from there.
#   --help           Show this help.
#
# Prefer `mangoverlay uninstall` directly when the wrapper is working:
# it is discoverable from `mangoverlay --help` and needs no URL.
# ============================================================================

set -euo pipefail

C_ERR='\033[1;31m'; C_INFO='\033[1;32m'; C_RESET='\033[0m'
if [ -n "${NO_COLOR:-}" ] || [ ! -t 1 ]; then
    C_ERR=''; C_INFO=''; C_RESET=''
fi
err() { printf "%b" "${C_ERR}[ERROR]${C_RESET} " >&2; printf '%b\n' "$*" >&2; }
info(){ printf "%b" "${C_INFO}[INFO]${C_RESET} " >&2; printf '%b\n' "$*" >&2; }
die() { err "$*"; exit 1; }

PREFIX="$HOME"
FORWARD=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="${2:-}"; [ -n "$PREFIX" ] || die "--prefix needs a value"; shift ;;
        -h|--help)
            sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) FORWARD+=("$1") ;;
    esac
    shift
done

case "$PREFIX" in
    /*) : ;;
    *) PREFIX="$PWD/$PREFIX" ;;
esac
[ -d "$PREFIX" ] && PREFIX=$(CDPATH= cd -- "$PREFIX" && pwd)

# --- locate the installed wrapper -------------------------------------------
BIN_MANGOVERLAY="$PREFIX/.local/bin/mangoverlay"

if [ ! -x "$BIN_MANGOVERLAY" ]; then
    # /usr/local/bin may hold our symlink even with a custom prefix
    if [ -x /usr/local/bin/mangoverlay ]; then
        BIN_MANGOVERLAY=/usr/local/bin/mangoverlay
    elif [ -x "$HOME/.local/bin/mangoverlay" ]; then
        BIN_MANGOVERLAY="$HOME/.local/bin/mangoverlay"
    fi
fi

# --- wrapper missing/broken: fallback instructions ---------------------------
warn_block() {
    cat >&2 <<EOF
${C_ERR}[ERROR]${C_RESET} No working mangoverlay wrapper found under $PREFIX
or $HOME/.local/bin.

Remove the installed files manually (paths assume prefix $PREFIX):
  rm -f "$PREFIX/.local/share/vulkan/implicit_layer.d/MangoHud_overlay_unified.json"
  rm -rf "$PREFIX/.local/lib/mangoverlay"
  rm -f "$PREFIX/.local/bin/mangoverlay" "$PREFIX/.local/bin/lsfg-vk-cli"
  rm -f "$PREFIX/.local/lib/liblsfg-vk-layer.so" "$PREFIX/.local/lib/liblsfg-vk-layer.x86.so"
  rm -f "$PREFIX/.local/lib64/liblsfg-vk-layer.so"
  rm -f "${XDG_CONFIG_HOME:-$HOME/.config}/vulkan/implicit_layer.d/VkLayer_LSFGVK_frame_generation.json"
  rm -rf "${XDG_CACHE_HOME:-$HOME/.cache}/mangoverlay"
  sudo rm -f /usr/local/bin/mangoverlay
  # remove the mangoverlay installer PATH block from ~/.zshrc / ~/.bashrc
  # configs: ${XDG_CONFIG_HOME:-$HOME/.config}/lsfg-vk  (kept by default)
EOF
}

if [ -x "$BIN_MANGOVERLAY" ]; then
    info "Found wrapper: $BIN_MANGOVERLAY"
    if "$BIN_MANGOVERLAY" uninstall --help >/dev/null 2>&1; then
        exec "$BIN_MANGOVERLAY" uninstall "${FORWARD[@]}"
    fi
    # wrapper exists but is an old build without the uninstall subcommand
    if [ "${FORWARD[*]}" != "--yes" ]; then
        printf 'The installed wrapper is too old to uninstall itself.\n'
        printf 'Show manual removal instructions? [y/N] '
        read -r ans
        case "$ans" in
            y|Y) : ;;
            *) die "Update it (curl | bash install.sh) and try again." ;;
        esac
    fi
    warn_block
    exit 1
fi

if [ "${FORWARD[*]}" != "--yes" ]; then
    printf 'No mangoverlay wrapper found. Show manual removal instructions? [y/N] '
    read -r ans
    case "$ans" in
        y|Y) : ;;
        *) die "Nothing changed." ;;
    esac
fi
warn_block
exit 1