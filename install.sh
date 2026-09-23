#!/usr/bin/env bash
# ============================================================================
# install.sh -- Integrated installer: unified overlay (MangoHud fork) +
#               mangoverlay wrapper + lsfg-vk (upstream) + dll detection
#
# Usage:
#   One-liner (downloads the whole project and installs):
#     curl -fsSL https://raw.githubusercontent.com/Axforzi/mangoverlay/v0.1.6/install.sh | bash
#   Or from a local clone:
#     ./install.sh [--skip-overlay] [--skip-lsfg] [--skip-deps]
#                  [--install-deps] [--dll <path>] [--yes] [--force]
#                  [--build] [--prefix <dir>] [--help]
#
# Flags:
#   --skip-overlay   Do not build or install the overlay (MangoHud fork)
#   --skip-lsfg      Do not clone/build/install lsfg-vk (upstream)
#   --skip-deps      Do not suggest or ask about build deps
#   --install-deps   Install build deps WITHOUT asking (automatic mode,
#                    needs sudo) using the detected package manager
#   --dll <path>     Use that lsfg-vk.dll path (validates it exists), no prompt
#   --yes            Ask nothing: use defaults and skip GUI prompts
#   --force          Rebuild the overlay even if a build already exists
#   --build          Compile locally instead of downloading the precompiled
#                    release assets (default is to try the assets first and
#                    fall back to a local build if they are unavailable or
#                    your glibc is too old for the prebuilt binaries)
#   --prefix <dir>   Install base (default: $HOME); binaries, libs and
#                    layers go to <prefix>/.local, configs to ~/.config
#   --help           Show this help
#
# Requires Linux with bash. 100% user install without sudo except for the
# optional build-deps installation. Does not hardcode user paths: everything
# is relative to $HOME / $PREFIX.
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
step()  { printf "%b\n" "${C_STEP}── Step ${1} of 6: ${2} ──${C_RESET}"; }
die()   { err "$*"; exit 1; }
have()  { command -v "$1" >/dev/null 2>&1; }

# --- Default flags -----------------------------------------------------------
SKIP_OVERLAY=0; SKIP_LSFG=0; SKIP_DEPS=0; INSTALL_DEPS=0
FORCE=0; YES=0; FORCE_BUILD=0
PREFIX="$HOME"; DLL_OVERRIDE=""
# lsfg-vk patch status (set in install_lsfg); "skipped" is the default for
# the final summary when --skip-lsfg skips their application. Patches are
# applied in lexicographic order: 01-frame-limit (independent) MUST come
# before 02-config-hardening, which is generated against the 01 state.
PATCH_STATUS="skipped"

usage() {
    if [ -r "$0" ]; then sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
    else
        info "Usage:"
        printf '   curl -fsSL https://raw.githubusercontent.com/Axforzi/mangoverlay/v0.1.6/install.sh | bash\n'
        printf '   install.sh [--skip-overlay] [--skip-lsfg] [--skip-deps]\n'
        printf '             [--install-deps] [--dll <path>] [--yes] [--force]\n'
        printf '             [--build] [--prefix <dir>] [--help]\n'
    fi
    exit 0
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --skip-overlay)  SKIP_OVERLAY=1 ;;
        --skip-lsfg)     SKIP_LSFG=1 ;;
        --skip-deps)     SKIP_DEPS=1 ;;
        --install-deps)  INSTALL_DEPS=1 ;;
        --force)         FORCE=1 ;;
        --force-build|--build) FORCE_BUILD=1 ;;
        --yes)           YES=1 ;;
        --prefix)        PREFIX="$2"; shift ;;
        --dll)           DLL_OVERRIDE="$2"; shift ;;
        -h|--help)       usage ;;
        *) die "Unknown option: $1 (see --help)" ;;
    esac
    shift
done

# --- Bootstrap: standalone run (curl | bash) downloads the sources -----------
# Detects that we are not inside the project tree (the MangoHud/ source and
# patches/ are missing next to the script), downloads the repo tarball from
# GitHub, extracts it to a temp dir and re-runs this same script from there.
# Everything after the bootstrap therefore sees a complete source tree, exactly
# like a local ./install.sh run.
if [ ! -d "$(dirname -- "$0")/MangoHud" ]; then
    info "Standalone run detected (curl | bash) — downloading mangoverlay sources..."
    BOOT_REPO="Axforzi/mangoverlay"
    # Pinned to a release tag so the installer is reproducible: whoever runs
    # the one-liner (which points at this tag) also downloads the same tag.
    BOOT_BRANCH="v0.1.6"
    BOOT_URL="https://github.com/$BOOT_REPO/archive/refs/tags/$BOOT_BRANCH.tar.gz"
    if ! have curl && ! have wget; then
        die "Neither curl nor wget is available; cannot download the sources."
    fi
    BOOT_DIR=$(mktemp -d "${TMPDIR:-/tmp}/mangoverlay-boot.XXXXXX") \
        || die "mktemp failed: cannot create a temp directory."
    info "Downloading $BOOT_URL"
    if have curl; then
        curl -fsSL "$BOOT_URL" -o "$BOOT_DIR/src.tar.gz" \
            || { rm -rf "$BOOT_DIR"; die "Download failed (curl)."; }
    else
        wget -q "$BOOT_URL" -O "$BOOT_DIR/src.tar.gz" \
            || { rm -rf "$BOOT_DIR"; die "Download failed (wget)."; }
    fi
    tar -xzf "$BOOT_DIR/src.tar.gz" -C "$BOOT_DIR" \
        || { rm -rf "$BOOT_DIR"; die "Extraction of the source tarball failed."; }
    # GitHub tarballs extract into <repo>-<branch>/ (e.g. mangoverlay-master/)
    BOOT_ROOT=$(find "$BOOT_DIR" -mindepth 1 -maxdepth 1 -type d -name 'mangoverlay-*' | head -n1)
    [ -n "$BOOT_ROOT" ] && [ -f "$BOOT_ROOT/install.sh" ] \
        || { rm -rf "$BOOT_DIR"; die "The source tarball did not contain the project."; }
    info "Running the installer from $BOOT_ROOT"
    # Carry the pinned tag into the re-run: it lets the inner script fetch
    # the precompiled release assets for exactly this version.
    MANGO_PREBUILT_TAG="$BOOT_BRANCH" bash "$BOOT_ROOT/install.sh" "$@"
    rc=$?
    rm -rf "$BOOT_DIR"
    exit $rc
fi

# --- Paths (all relative to $HOME / $PREFIX) ---------------------------------
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MANGO_SRC="$SCRIPT_DIR/MangoHud"
MANGO_WRAPPER_SRC="$SCRIPT_DIR/mangoverlay"

case "$PREFIX" in
    /*) : ;;
    *)  PREFIX="$PWD/$PREFIX" ;;
esac
PREFIX=$(CDPATH= cd -- "$PREFIX" && pwd)

BIN_DIR="$PREFIX/.local/bin"
LIB_DIR="$PREFIX/.local/lib/mangoverlay"
VK_DIR="$PREFIX/.local/share/vulkan/implicit_layer.d"
# The LSFGVK manifest must live in the first directory the loader scans
# ($XDG_CONFIG_HOME/vulkan/implicit_layer.d) to load BEFORE MangoHud.
# That way MangoHud's FPS counter measures the post-generation frames.
VK_CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/vulkan/implicit_layer.d"
VK_LAYER_JSON="$VK_DIR/MangoHud_overlay_unified.json"
BIN_MANGOVERLAY="$BIN_DIR/mangoverlay"
CFG_DIR="$HOME/.config/lsfg-vk"
CONF_TOML="$CFG_DIR/conf.toml"
ENV_CONF="$CFG_DIR/env.conf"
LSFG_SRC="${XDG_CACHE_HOME:-$HOME/.cache}/mangoverlay/lsfg-vk"

# --- Prebuilt assets (optional; the default is to use them) ------------------
# Each release ships mangoverlay-assets-<tag>.tar.gz built by the
# .github/workflows/build-release-assets.yml workflow. install.sh downloads
# these first and only compiles locally when they do not exist or --build is
# passed. The tag is carried from the one-liner bootstrap; a local clone
# sitting exactly on a tag uses that tag, a branch without a tag builds from
# source (the release assets would not match its tree).
PREBUILT_TAG="${MANGO_PREBUILT_TAG:-}"
if [ -z "$PREBUILT_TAG" ] && [ -d "$SCRIPT_DIR/.git" ]; then
    PREBUILT_TAG="$(git -C "$SCRIPT_DIR" describe --tags --exact-match 2>/dev/null || true)"
fi
PREBUILT_DIR=""                       # set by fetch_prebuilt on success
PREBUILT_BASE_URL="https://github.com/Axforzi/mangoverlay/releases/download"

# --- Prebuilt glibc gate ------------------------------------------------------
# The release assets are built on the CI runner (ubuntu-22.04, glibc 2.35).
# glibc symbol versioning means those binaries cannot load on systems with an
# older glibc (they die with "GLIBC_2.35 not found" at game launch), so when
# the local glibc is older than the build's we MUST compile locally instead of
# shipping an install that breaks at runtime.
MIN_GLIBC="2.35"
glibc_version() {
    # "2.35" -> 235 ; empty on non-glibc libcs (e.g. musl).
    # getconf prints a prefix ("glibc 2.43"), so take the last field first.
    getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $NF}' | awk -F. '{print $1*100+$2}'
}
prebuilt_supported_by_glibc() {
    local gv min_ver
    gv="$(glibc_version)"
    [ -n "$gv" ] || return 1
    min_ver="$(printf '%s\n' "$MIN_GLIBC" | awk -F. '{print $1*100+$2}')"
    if [ "$gv" -ge "$min_ver" ] 2>/dev/null; then
        return 0
    fi
    return 1
}

# fetch_prebuilt: download the release assets for $PREBUILT_TAG into a cache
# dir and verify the expected files are really there. On success sets
# PREBUILT_DIR and returns 0; on any failure returns 1 (the caller falls back
# to a local build, which keeps the installer resilient).
fetch_prebuilt() {
    [ -n "$PREBUILT_TAG" ] || return 1
    [ "$FORCE_BUILD" -eq 1 ] && return 1
    if ! prebuilt_supported_by_glibc; then
        local gv
        gv="$(getconf GNU_LIBC_VERSION 2>/dev/null || true)"
        if [ -n "$gv" ]; then
            warn "Your glibc is $gv; the precompiled assets need glibc >= $MIN_GLIBC."
            warn "Those binaries would fail at runtime (GLIBC_2.35 not found)."
        else
            warn "Your libc is not glibc (or could not be detected); the precompiled"
            warn "assets need glibc >= $MIN_GLIBC and would not load here."
        fi
        warn "Compiling locally instead (equivalent to --build)."
        return 1
    fi

    local cachedir="${XDG_CACHE_HOME:-$HOME/.cache}/mangoverlay/prebuilt/$PREBUILT_TAG"
    if [ -z "$(ls -A "$cachedir" 2>/dev/null)" ]; then
        info "Downloading precompiled assets for $PREBUILT_TAG..."
        mkdir -p "$cachedir"
        local url="$PREBUILT_BASE_URL/$PREBUILT_TAG/mangoverlay-assets-$PREBUILT_TAG.tar.gz"
        if have curl; then
            curl -fsSL "$url" -o "$cachedir/assets.tar.gz" \
                || { rm -rf "$cachedir"; warn "Prebuilt download failed (curl): $url"; return 1; }
        elif have wget; then
            wget -q "$url" -O "$cachedir/assets.tar.gz" \
                || { rm -rf "$cachedir"; warn "Prebuilt download failed (wget): $url"; return 1; }
        else
            warn "Neither curl nor wget is available; skipping precompiled assets."
            return 1
        fi
        tar -xzf "$cachedir/assets.tar.gz" -C "$cachedir" \
            || { rm -rf "$cachedir"; warn "Prebuilt archive corrupt: $url"; return 1; }
        rm -f "$cachedir/assets.tar.gz"
    else
        info "Using cached precompiled assets for $PREBUILT_TAG."
    fi

    local missing=""
    [ -s "$cachedir/overlay/libMangoHud.so" ]            || missing="$missing libMangoHud.so"
    [ -s "$cachedir/overlay/libMangoHud_opengl.so" ]     || missing="$missing libMangoHud_opengl.so"
    [ -s "$cachedir/lsfg/liblsfg-vk-layer.so" ]          || missing="$missing liblsfg-vk-layer.so"
    [ -s "$cachedir/lsfg/lsfg-vk-cli" ]                  || missing="$missing lsfg-vk-cli"
    if [ -n "$missing" ]; then
        rm -rf "$cachedir"
        warn "Prebuilt assets incomplete (missing:$missing); compiling locally."
        return 1
    fi
    PREBUILT_DIR="$cachedir"
    info "Precompiled assets ready in $PREBUILT_DIR"
    return 0
}

CURRENT_STEP="pre-flight"
trap 'err "Installation aborted (failed at: $CURRENT_STEP)"; exit 1' ERR

# --- Utilities ---------------------------------------------------------------
# Reads a line from the user. Falls back to /dev/tty so interactive prompts
# still work when the script runs through a pipe (curl ... | bash), where
# stdin is already exhausted. Returns 1 only when no tty exists at all.
read_user_input() {
    if [ -t 0 ]; then
        read -r "$1"
        return $?
    fi
    # stdin is a pipe (curl ... | bash): ask on the controlling terminal.
    # The {} group silences the shell's open error when there is no terminal.
    if { exec 3< /dev/tty; } 2>/dev/null; then
        read -r "$1" <&3
        exec 3<&-
        return $?
    fi
    return 1
}

ask_yes_no() {
    # ask_yes_no "<prompt>" <default: y|n>  → 0 = yes, 1 = no
    local prompt="$1" default="$2" ans
    if [ "$YES" -eq 1 ]; then
        [ "$default" = "y" ]; return $?
    fi
    while :; do
        printf '%s ' "$prompt"
        if ! read_user_input ans; then     # EOF: use default
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

detect_pkgmgr() {
    if have pacman;        then PKG_MGR="pacman"
    elif have dnf;         then PKG_MGR="dnf"
    elif have apt-get;     then PKG_MGR="apt"
    elif have zypper;      then PKG_MGR="zypper"
    elif have emerge;      then PKG_MGR="emerge"
    elif have apk;         then PKG_MGR="apk"
    else PKG_MGR=""
    fi
}

# --- 1/6 Detect distro and build deps ----------------------------------------
install_build_deps() {
    # Sets per manager (common names; overlay MangoHud fork + lsfg-vk)
    local pkgs=""
    case "$PKG_MGR" in
        apt)
            pkgs="meson ninja-build cmake g++ git pkg-config glslang-tools \
python3-mako libx11-dev libdbus-1-dev libxnvctrl-dev libwayland-dev \
libxkbcommon-dev libdrm-dev libpciaccess-dev libvulkan-dev"
            ;;
        dnf)
            pkgs="meson ninja-build cmake gcc-c++ git pkgconf-pkg-config \
glslang python3-mako libX11-devel dbus-devel libXNVCtrl-devel wayland-devel \
libxkbcommon-devel libdrm-devel libpciaccess-devel vulkan-headers"
            ;;
        pacman)
            pkgs="meson ninja cmake gcc git pkgconf glslang python-mako libx11 dbus \
libxnvctrl wayland libxkbcommon libdrm libpciaccess vulkan-headers"
            ;;
        zypper)
            pkgs="meson ninja cmake gcc-c++ git pkg-config glslang python3-mako \
libX11-devel dbus-1-devel libxnvctrl-devel wayland-devel libxkbcommon-devel \
libdrm-devel libpciaccess-devel vulkan-headers"
            ;;
        emerge)
            pkgs="dev-util/meson dev-util/ninja dev-util/cmake sys-devel/gcc \
dev-vcs/git dev-util/pkgconf dev-util/glslang dev-python/mako x11-libs/libX11 \
sys-libs/dbus x11-libs/libXNVCtrl dev-libs/wayland x11-libs/libxkbcommon \
x11-libs/libdrm x11-libs/libpciaccess dev-util/vulkan-headers"
            ;;
        *)
            die "Unknown package manager. Install manually: \
meson, ninja, cmake, glslang, X11/Wayland/DBus/Vulkan headers."
            ;;
    esac

    info "Installing build deps (${PKG_MGR}):"
    printf '  %s\n' "$pkgs"
    case "$PKG_MGR" in
        apt)    run_admin apt-get update -y
                run_admin apt-get install -y $pkgs ;;
        dnf)    run_admin dnf install -y $pkgs ;;
        pacman) run_admin pacman -S --needed --noconfirm $pkgs ;;
        zypper) run_admin zypper --non-interactive install $pkgs ;;
        emerge) run_admin emerge --ask=n --quiet $pkgs ;;
    esac
}

check_toolchain() {
    # Returns 0 if the toolchain is complete; 1 if tools are missing
    local missing=""
    for t in git cmake; do
        have "$t" || missing="$missing $t"
    done
    if [ "$SKIP_OVERLAY" -eq 0 ]; then
        for t in meson ninja; do
            have "$t" || missing="$missing $t"
        done
    fi
    if [ -n "$missing" ]; then
        warn "Missing build tools:$missing"
        return 1
    fi
    return 0
}

step_deps() {
    CURRENT_STEP="1/6 detect distro and deps"
    step "1" "Detect distro and deps"
    detect_pkgmgr
    if [ -n "$PKG_MGR" ]; then
        info "Detected package manager: ${C_BOLD}${PKG_MGR}${C_RESET}"
    else
        warn "No known package manager detected."
    fi

    if [ -n "$PREBUILT_DIR" ]; then
        info "Using precompiled assets: build toolchain not needed."
        return 0
    fi

    if [ "$SKIP_DEPS" -eq 1 ]; then
        info "Skipping deps (--skip-deps)."
        return 0
    fi
    if check_toolchain; then
        info "Build toolchain complete (meson/ninja/cmake/git)."
        return 0
    fi

    if [ "$INSTALL_DEPS" -eq 1 ]; then
        install_build_deps
    elif [ "$YES" -eq 1 ]; then
        warn "Deps not installed (--yes mode without --install-deps)."
    else
        if ask_yes_no "Install build deps using your package manager (needs sudo)? [y/N]" n; then
            install_build_deps
        else
            warn "You can install them manually later; the build will fail if they are missing."
        fi
    fi
}

# --- 2/6 Build the overlay (MangoHud fork) -----------------------------------
setup_mango_build() {
    # Reconfigures if the build dir already exists (preserves options);
    # if reconfiguration fails (build from another machine), recreates clean.
    ( cd "$MANGO_SRC"
      if [ -f build/build.ninja ]; then
          meson setup --reconfigure build \
              || { warn "Reconfiguration failed; recreating build directory."
                   rm -rf build
                   meson setup build --buildtype=release -Dmangohudctl=true; }
      else
          meson setup build --buildtype=release -Dmangohudctl=true
      fi
      ninja -C build )
}

build_overlay() {
    CURRENT_STEP="2/6 build overlay"
    step "2" "Build the overlay (MangoHud fork)"
    if [ -n "$PREBUILT_DIR" ]; then
        info "Using precompiled overlay from $PREBUILT_TAG release assets."
        return 0
    fi
    [ -d "$MANGO_SRC" ] || die "Overlay source not found: $MANGO_SRC"

    if [ -f "$MANGO_SRC/build/src/libMangoHud.so" ] && [ "$FORCE" -eq 0 ]; then
        if ask_yes_no "A build already exists. Reuse it without rebuilding? [Y/n]" y; then
            info "Reusing the existing build."
            return 0
        fi
    fi
    if ! have meson || ! have ninja; then
        die "meson/ninja missing. Run with --install-deps or install them manually."
    fi
    info "Building overlay (meson/ninja)..."
    setup_mango_build
    [ -f "$MANGO_SRC/build/src/libMangoHud.so" ] \
        || die "The build did not produce build/src/libMangoHud.so"
    info "Overlay built OK."
}

# --- 3/6 Install overlay layer ------------------------------------------------
install_overlay_layer() {
    CURRENT_STEP="3/6 install overlay layer"
    step "3" "Install overlay layer"
    mkdir -p "$LIB_DIR" "$VK_DIR"
    if [ -n "$PREBUILT_DIR" ]; then
        # Precompiled assets: the CI workflow already applied every patch and
        # built the libraries; we only place them and generate the manifest.
        install -m 755 "$PREBUILT_DIR/overlay/libMangoHud.so" "$LIB_DIR/"
        install -m 755 "$PREBUILT_DIR/overlay/libMangoHud_opengl.so" "$LIB_DIR/"
    else
        install -m 755 "$MANGO_SRC/build/src/libMangoHud.so" "$LIB_DIR/"
        if [ -f "$MANGO_SRC/build/src/libMangoHud_opengl.so" ]; then
            install -m 755 "$MANGO_SRC/build/src/libMangoHud_opengl.so" "$LIB_DIR/"
        fi
    fi

    local json_lib_path="$LIB_DIR/libMangoHud.so"
    cat > "$VK_LAYER_JSON" <<EOF
{
    "file_format_version" : "1.0.0",
    "layer" : {
      "name": "VK_LAYER_MANGOHUD_overlay_unified_x86_64",
      "type": "GLOBAL",
      "api_version": "1.3.0",
      "library_path": "$json_lib_path",
      "implementation_version": "1",
      "description": "MangoHud fork unified overlay (menu Shift_R+F9) - per-game layer",
      "functions": {
         "vkGetInstanceProcAddr": "overlay_GetInstanceProcAddr",
         "vkGetDeviceProcAddr": "overlay_GetDeviceProcAddr"
      },
      "enable_environment": {
        "MANGOHUD_UNIFIED": "1"
      },
      "disable_environment": {
        "DISABLE_MANGOHUD_UNIFIED": "1"
      }
    }
}
EOF
    info "Overlay layer installed at $VK_LAYER_JSON"
}

# --- 4/6 Install mangoverlay wrapper -----------------------------------------
ensure_bin_in_path() {
    local rc
    case "${SHELL##*/}" in
        zsh) rc="$HOME/.zshrc" ;;
        *)   rc="$HOME/.bashrc" ;;
    esac
    case ":$PATH:" in *":$BIN_DIR:"*) return 0 ;; esac
    if [ -f "$rc" ] && grep -q '^# >>> mangoverlay installer >>>' "$rc" 2>/dev/null; then
        return 0
    fi
    {
        printf '\n# >>> mangoverlay installer >>>\n'
        printf 'export PATH="%s:$PATH"\n' "$BIN_DIR"
        printf '# <<< mangoverlay installer <<<\n'
    } >> "$rc"
    warn "Added $BIN_DIR to PATH in $rc. Run 'source $rc' or open a new terminal."
}

install_wrapper() {
    CURRENT_STEP="4/6 install mangoverlay wrapper"
    step "4" "Install mangoverlay wrapper"
    [ -f "$MANGO_WRAPPER_SRC" ] \
        || die "Wrapper not found: $MANGO_WRAPPER_SRC"
    mkdir -p "$BIN_DIR"
    install -m 755 "$MANGO_WRAPPER_SRC" "$BIN_MANGOVERLAY"
    ensure_bin_in_path
    info "Wrapper installed at $BIN_MANGOVERLAY"

    # Steam does not inherit ~/.local/bin from bashrc/zshrc; its PATH
    # includes /usr/local/bin. A symlink there makes 'mangoverlay
    # %command%' work without a full path.
    if [ -w /usr/local/bin ]; then
        ln -sf "$BIN_MANGOVERLAY" /usr/local/bin/mangoverlay
        info "Symlink created at /usr/local/bin/mangoverlay (visible to Steam)."
    elif [ "$YES" -eq 1 ]; then
        run_admin ln -sf "$BIN_MANGOVERLAY" /usr/local/bin/mangoverlay
        info "Symlink created at /usr/local/bin/mangoverlay (visible to Steam)."
    else
        if ask_yes_no "Create a symlink in /usr/local/bin so Steam sees 'mangoverlay'? [y/N]" n; then
            run_admin ln -sf "$BIN_MANGOVERLAY" /usr/local/bin/mangoverlay
            info "Symlink created at /usr/local/bin/mangoverlay (visible to Steam)."
        else
            warn "No symlink: use the absolute path in Steam: $BIN_MANGOVERLAY %command%"
        fi
    fi
}

# --- 5/6 Install lsfg-vk (always from upstream) -------------------------------
install_lsfg() {
    CURRENT_STEP="5/6 install lsfg-vk upstream"
    step "5" "Install lsfg-vk (upstream)"

    if [ -n "$PREBUILT_DIR" ]; then
        # Precompiled assets: the CI workflow cloned upstream and applied the
        # patches, so we only place the library + cli and write the manifest
        # that cmake would have generated (with the absolute library_path the
        # Vulkan loader needs; see the local-build branch below for the
        # rationale on the config dir).
        local prebuilt_lib=""
        if [ -d "$PREFIX/.local/lib64" ]; then
            prebuilt_lib="$PREFIX/.local/lib64/liblsfg-vk-layer.so"
        else
            prebuilt_lib="$PREFIX/.local/lib/liblsfg-vk-layer.so"
        fi
        mkdir -p "$(dirname "$prebuilt_lib")" "$BIN_DIR" "$VK_CONFIG_DIR"
        install -m 755 "$PREBUILT_DIR/lsfg/liblsfg-vk-layer.so" "$prebuilt_lib"
        install -m 755 "$PREBUILT_DIR/lsfg/lsfg-vk-cli" "$BIN_DIR/lsfg-vk-cli"

        cat > "$VK_CONFIG_DIR/VkLayer_LSFGVK_frame_generation.json" <<EOF
{
  "file_format_version": "1.1.0",
  "layer": {
    "name": "VK_LAYER_LSFGVK_frame_generation",
    "description": "Lossless Scaling frame generation layer",
    "implementation_version": "2",
    "library_path": "$prebuilt_lib",
    "type": "GLOBAL",
    "api_version": "1.4.350",
    "disable_environment": {
      "DISABLE_LSFGVK": "1"
    }
  }
}
EOF
        PATCH_STATUS="prebuilt"
        info "lsfg-vk installed from precompiled assets: $prebuilt_lib"
        info "lsfg-vk JSON at $VK_CONFIG_DIR/VkLayer_LSFGVK_frame_generation.json"
        return 0
    fi

    for t in git cmake; do
        have "$t" || die "Missing '$t'. Run with --install-deps or install it manually."
    done

    if [ -d "$LSFG_SRC/.git" ]; then
        info "Updating existing clone at $LSFG_SRC"
        git -C "$LSFG_SRC" pull --ff-only --quiet \
            || warn "git pull failed; using the existing clone."
    else
        info "Cloning https://git.lsfg-vk.dev/lsfg-vk.git (master branch)..."
        mkdir -p "$(dirname "$LSFG_SRC")"
        git clone --depth 1 https://git.lsfg-vk.dev/lsfg-vk.git "$LSFG_SRC"
    fi

    # Durable patches for the lsfg-vk source, applied in lexicographic order
    # (the filenames are numbered so 01-frame-limit lands before
    # 02-config-hardening, which is generated on top of it). Each patch is
    # skipped when already applied (reverse-check OK). When the reverse-check
    # ALSO fails, the tree still builds as-is: that happens on upgrade paths
    # where 02 has already changed the context around 01 (a fully patched
    # tree), or when upstream moved on (we build without that patch).
    PATCHES_DIR="$(dirname "$(readlink -f "$0")")/patches"
    PATCH_STATUS="none"
    for patch in "$PATCHES_DIR"/*.patch; do
        [ -f "$patch" ] || continue
        name="$(basename "$patch")"
        if git -C "$LSFG_SRC" apply --check "$patch" 2>/dev/null; then
            git -C "$LSFG_SRC" apply "$patch"
            info "Patch applied: $name"
            if [ "$PATCH_STATUS" = "none" ] || [ "$PATCH_STATUS" = "partial" ]; then
                PATCH_STATUS="applied"
            fi
        elif git -C "$LSFG_SRC" apply --reverse --check "$patch" 2>/dev/null; then
            info "Patch already applied, skipping: $name"
            if [ "$PATCH_STATUS" = "none" ]; then
                PATCH_STATUS="applied (already present)"
            elif [ "$PATCH_STATUS" = "applied" ]; then
                PATCH_STATUS="applied"
            fi
        else
            # Either another patch already changed this patch's context (the
            # tree is already fully patched) or upstream moved; either way the
            # current tree state is the best we have and the build proceeds.
            info "$name not cleanly applicable; keeping the current tree state (already patched, or upstream changed)."
            if [ "$PATCH_STATUS" = "none" ]; then
                PATCH_STATUS="partial"
            fi
        fi
    done
    # A failed reverse-check for an EARLIER patch usually means a LATER patch
    # already changed its context: if the last patch is verifiably present,
    # the whole ordered set is already applied (02 cannot land without 01).
    if [ "$PATCH_STATUS" = "partial" ]; then
        last_patch="$(ls "$PATCHES_DIR"/*.patch 2>/dev/null | sort | tail -n1)"
        if [ -n "$last_patch" ] && git -C "$LSFG_SRC" apply --reverse --check "$last_patch" 2>/dev/null; then
            PATCH_STATUS="applied (already present)"
        fi
    fi

    cmake -S "$LSFG_SRC" -B "$LSFG_SRC/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX/.local" \
        -DLSFGVK_BUILD_UI=OFF \
        -DLSFGVK_BUILD_CLI=ON \
        -DLSFGVK_BUILD_LAYER=ON
    cmake --build "$LSFG_SRC/build" -j"$(nproc)"
    cmake --install "$LSFG_SRC/build"

    # The JSON generated by cmake uses a relative library_path
    # (liblsfg-vk-layer.so), which the Vulkan loader resolves against the
    # JSON dir and does not find. It also lands in .local/share/vulkan,
    # where it is discovered AFTER MangoHud: the FPS counter would measure
    # PRE-generation frames. We move it to the first directory the loader
    # scans ($XDG_CONFIG_HOME/vulkan/implicit_layer.d) and set the absolute
    # library_path post-install (lib64 on Fedora, lib on Debian).
    local lsfg_json_src="$PREFIX/.local/share/vulkan/implicit_layer.d/VkLayer_LSFGVK_frame_generation.json"
    local lsfg_json="$VK_CONFIG_DIR/VkLayer_LSFGVK_frame_generation.json"
    local lsfg_lib=""
    for cand in "$PREFIX/.local/lib64/liblsfg-vk-layer.so" "$PREFIX/.local/lib/liblsfg-vk-layer.so"; do
        if [ -f "$cand" ]; then lsfg_lib="$cand"; break; fi
    done
    if [ -n "$lsfg_lib" ]; then
        mkdir -p "$VK_CONFIG_DIR"
        # mv -f overwrites a previous manifest at the destination; by moving
        # (not copying) the source disappears from $VK_DIR, so two manifests
        # with the same layer name never coexist. (lsfg_json_src was just
        # regenerated by cmake --install.)
        if [ -f "$lsfg_json_src" ]; then
            mv -f "$lsfg_json_src" "$lsfg_json"
        fi
        if [ -f "$lsfg_json" ]; then
            # Replace only the library_path line of the JSON (no python dep).
            sed -i "s|\"library_path\": \".*liblsfg-vk-layer\.so\"|\"library_path\": \"$lsfg_lib\"|" "$lsfg_json"
            info "lsfg-vk JSON at $lsfg_json pointing to: $lsfg_lib"
        else
            warn "lsfg-vk JSON not found after install; create it manually."
        fi
    else
        warn "Could not locate liblsfg-vk-layer.so after install."
    fi

    info "lsfg-vk installed at $PREFIX/.local (layer + cli)."
}

# --- 6/6 Detect dll + configs ----------------------------------------------
find_dll_in_steam() {
    # Same paths as upstream utils::findDll() (Steam + XDG + cwd)
    local prefix rel cand
    local steam_paths=".local/share/Steam/steamapps/common
.steam/steam/steamapps/common
.steam/debian-installation/steamapps/common
.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common
snap/steam/common/.local/share/Steam/steamapps/common"
    for prefix in "${XDG_DATA_HOME:-}" "$HOME"; do
        [ -n "$prefix" ] || continue
        for rel in $steam_paths; do
            cand="$prefix/$rel/Lossless Scaling/lsfg-vk.dll"
            if [ -f "$cand" ]; then
                printf '%s\n' "$cand"
                return 0
            fi
        done
    done
    if [ -f "./lsfg-vk.dll" ]; then
        printf '%s\n' "$(pwd)/lsfg-vk.dll"
        return 0
    fi
    return 1
}

prompt_dll_manual() {
    # GTK/KDE GUI if available; otherwise a terminal prompt with validation.
    local picked=""
    if [ "$YES" -eq 1 ]; then
        return 1
    fi
    if have zenity || have kdialog; then
        info ""
        info "A file selector is about to open to choose your lsfg-vk.dll."
        info "The window appears right after this line."
        info "If you do not have the file at hand, cancel the selector and we continue anyway."
        printf '%s' "Press Enter to continue... " >&2
        read_user_input _ || return 1
        info ""
    fi
    if have zenity; then
        picked=$(zenity --file-selection --title="Select lsfg-vk.dll" \
                        --file-filter="DLL | *.dll") \
            && [ -n "$picked" ] && [ -f "$picked" ] || picked=""
    fi
    if [ -z "$picked" ] && have kdialog; then
        picked=$(kdialog --getopenfilename "$HOME" "*.dll") \
            && [ -n "$picked" ] && [ -f "$picked" ] || picked=""
    fi
    if [ -z "$picked" ]; then
        info "dll not found. Enter the path manually (Enter to skip):"
        while :; do
            printf 'Path to lsfg-vk.dll: ' >&2
            if ! read_user_input picked; then return 1; fi
            if [ -z "$picked" ]; then return 1; fi
            if [ -f "$picked" ]; then break; fi
            err "The file does not exist: $picked"
        done
    fi
    printf '%s\n' "$picked"
    return 0
}

write_conf_toml() {
    # Only created if it does not exist; never overwrites user config.
    local dll_line=""
    if [ -n "${DLL_PATH:-}" ]; then
        dll_line="dll = \"$DLL_PATH\""
    fi
    if [ -f "$CONF_TOML" ]; then
        warn "conf.toml already exists; left untouched."
        return 0
    fi
    mkdir -p "$CFG_DIR"
    cat > "$CONF_TOML" <<TOML
# lsfg-vk -- configuration (version 2)
version = 2

[global]
# Allow fp16 (half precision) compute in the frame generation pipeline
allow_fp16 = true
$dll_line
TOML
    info "Created $CONF_TOML"
}

write_env_conf() {
    if [ -f "$ENV_CONF" ]; then
        warn "env.conf already exists; left untouched."
        return 0
    fi
    mkdir -p "$CFG_DIR"
    cat > "$ENV_CONF" <<'CONF'
# mangoverlay -- per-game environment variables (goverlay-style INI)
#
# One section [GameName.exe] (or [GameName] / [SteamAppId]) applies to that
# game only. There is NO [global] section by design: every env var is
# per-game so a variable that breaks one game never leaks into the others.
# Each KEY = value line is exported as an environment variable before the
# game launches.

# --- Special options (not env vars) ---
# disable_all_vars = true            # disables ALL vars in this section
# gamemode = 1                       # wraps the command with gamemoderun
# game-performance = 1               # wraps the command with game-performance

# --- Useful variables (MangoHud / DXVK / Wine) ---
# MANGOHUD = 1
# MANGOHUD_FPS = 1
# MANGOHUD_CPU_STATS = 1
# MANGOHUD_GPU_STATS = 1
# MANGOHUD_RAM = 1
# MANGOHUD_VRAM = 1
# MANGOHUD_FPS_LIMIT = 0
# DXVK_HUD = frametimes
# WINE_FULLSCREEN_FSR = 1

# [GenshinImpact.exe]
# disable_all_vars = false
# PROTON_ENABLE_WAYLAND = 1
CONF
    info "Created $ENV_CONF"
}

config_dll_and_configs() {
    CURRENT_STEP="6/6 configure dll and configs"
    step "6" "Configure dll and configs"
    DLL_PATH=""

    if [ -n "$DLL_OVERRIDE" ]; then
        if [ -f "$DLL_OVERRIDE" ]; then
            DLL_PATH="$DLL_OVERRIDE"
            info "Using dll from --dll: $DLL_PATH"
        else
            err "dll from --dll does not exist: $DLL_OVERRIDE"
        fi
    elif DLL_FOUND=$(find_dll_in_steam); then
        DLL_PATH="$DLL_FOUND"
        info "dll auto-found in Steam: $DLL_PATH"
    elif DLL_PICKED=$(prompt_dll_manual); then
        DLL_PATH="$DLL_PICKED"
        info "dll selected manually: $DLL_PATH"
    else
        warn "dll not configured: install Lossless Scaling in Steam and download its files,"
        warn "or rerun the installer. The runtime also looks for it in Steam."
    fi

    write_conf_toml
    write_env_conf
}

# --- Pre-flight: check sources and previous installation ----------------------
# Try the precompiled release assets first; on any failure PREBUILT_DIR stays
# empty and the installer falls back to a full local build.
fetch_prebuilt || true

if [ "$SKIP_OVERLAY" -eq 0 ] && [ -z "$PREBUILT_DIR" ]; then
    [ -d "$MANGO_SRC" ] || die "Overlay source not found: $MANGO_SRC. If you run this script in a directory without the project sources, rerun via 'curl -fsSL https://raw.githubusercontent.com/Axforzi/mangoverlay/v0.1.6/install.sh | bash'."
fi

HAS_PREVIOUS=0
[ -f "$VK_LAYER_JSON" ]        && HAS_PREVIOUS=1
[ -f "$BIN_MANGOVERLAY" ]      && HAS_PREVIOUS=1
[ -f "$LIB_DIR/libMangoHud.so" ] && HAS_PREVIOUS=1
[ -f "$CONF_TOML" ]            && HAS_PREVIOUS=1
[ -f "$ENV_CONF" ]             && HAS_PREVIOUS=1

printf "%b\n" "${C_STEP}==========================================${C_RESET}"
printf "%b\n" "${C_BOLD}  Unified overlay + mangoverlay + lsfg-vk${C_RESET}"
printf "%b\n" "${C_BOLD}  Integrated installer${C_RESET}"
printf "%b\n" "${C_STEP}==========================================${C_RESET}"
info "Install prefix: $PREFIX/.local"
info "Configs: $CFG_DIR"

if [ "$HAS_PREVIOUS" -eq 1 ] && [ "$YES" -eq 0 ]; then
    warn "A previous installation was detected (layers, wrapper or configs present)."
    if ! ask_yes_no "Reinstall and overwrite binaries and layers? (existing configs are NOT touched) [y/N]" n; then
        info "Cancelled by the user."
        exit 0
    fi
    if ! ask_yes_no "Confirm replacing the current installation? (cannot be undone) [y/N]" n; then
        info "Cancelled by the user."
        exit 0
    fi
fi

# --- Main sequence -----------------------------------------------------------
step_deps

if [ "$SKIP_OVERLAY" -eq 1 ]; then
    info "Skipping overlay (--skip-overlay)."
else
    build_overlay
    install_overlay_layer
fi

install_wrapper

if [ "$SKIP_LSFG" -eq 1 ]; then
    info "Skipping lsfg-vk (--skip-lsfg)."
else
    install_lsfg
fi

config_dll_and_configs

# --- Final summary ------------------------------------------------------------
if [ -f "$PREFIX/.local/lib64/liblsfg-vk-layer.so" ]; then
    LSFG_LIB_SHOW="$PREFIX/.local/lib64/liblsfg-vk-layer.so"
else
    LSFG_LIB_SHOW="$PREFIX/.local/lib/liblsfg-vk-layer.so"
fi
printf "\n%b\n" "${C_STEP}══════════════ Installation summary ══════════════${C_RESET}"
info "Unified overlay:"
printf '  · Vulkan layer: %s\n' "$VK_LAYER_JSON"
printf '  · Libraries:    %s\n' "$LIB_DIR/"
info "Wrapper:"
printf '  · %s\n' "$BIN_MANGOVERLAY"
info "lsfg-vk:"
printf '  · Layer:         %s\n' "$VK_CONFIG_DIR/VkLayer_LSFGVK_frame_generation.json"
printf '  · Library:       %s\n' "$LSFG_LIB_SHOW"
if [ -n "$PREBUILT_DIR" ]; then
    printf '  · Origin:        precompiled assets for %s\n' "$PREBUILT_TAG"
else
    printf '  · Source:        %s\n' "$LSFG_SRC"
fi
if [ "$PATCH_STATUS" != "skipped" ] && { [ -n "$PREBUILT_DIR" ] || [ -d "$(dirname "$(readlink -f "$0")")/patches" ]; }; then
    printf '  · lsfg-vk patches: %s\n' "$PATCH_STATUS"
fi
info "Configs:"
printf '  · %s\n' "$CONF_TOML"
printf '  · %s\n' "$ENV_CONF"
if [ -n "${DLL_PATH:-}" ]; then
    info "lsfg-vk.dll: $DLL_PATH"
else
    warn "lsfg-vk.dll: NOT configured (the runtime looks for it in Steam anyway)."
fi

printf "\n%b\n" "${C_STEP}══════════════ Manual steps ══════════════${C_RESET}"
info "1) In Steam, set the game's launch options (or game + friends):"
printf '     mangoverlay %%command%%\n'
info "2) The overlay activates in that game with ${C_BOLD}Right Shift + F9${C_RESET}."
if [ -z "${DLL_PATH:-}" ]; then
    info "3) dll missing: install 'Lossless Scaling' in Steam and download its"
    printf '     files, or rerun this installer with --dll <path>.\n'
fi
info "Done. Have fun!"