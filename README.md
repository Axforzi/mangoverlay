# mangoverlay

A unified in-game overlay for lossless frame generation on Linux (Vulkan),
built as a fork of [MangoHud](https://github.com/flightlessmango/MangoHud)
that integrates the [lsfg-vk](https://git.lsfg-vk.dev/lsfg-vk) frame
generation layer into one menu.

## What it does

One menu (`Shift_R + F9`) inside the game that controls everything:

- **HUD** — the MangoHud readout: FPS, frame timing, GPU/CPU stats, VRAM/RAM,
  per-stat toggles, HUD position, FPS limit, v-sync and GPU selection.
  The HUD itself toggles with `Shift_R + F12`.
- **Frame generation (lsfg-vk)** — per-game profiles: frame multiplier
  (x2 = 120 fps from 60), pacing, flow scale, performance mode, present-mode
  override and a pre-generation FPS limiter. Settings are written to
  `~/.config/lsfg-vk/conf.toml`.
- **Environment variables per game** — goverlay-style tweaks
  (Proton/DXVK/Wine/AMD options) stored in `~/.config/lsfg-vk/env.conf`.
  Each var is scoped to a single game section; there is **no** `[global]`
  section so a var that breaks one game never leaks into the others.
  Changes apply on the next launch.

The overlay is a full gamepad-driven menu: D-pad to navigate, **A** to
activate, **B** to go back/close, and **Start + Select** to open/close it.

## How it is put together

- `mangoverlay` — a launch wrapper you set as a game's launch option in
  Steam (`mangoverlay %command%`). It:
  1. enables the overlay's own implicit Vulkan layer
     (`VK_LAYER_MANGOHUD_overlay_unified_x86_64`),
  2. disables the system MangoHud so there is no double HUD,
  3. reads `~/.config/lsfg-vk/env.conf` and exports the section matching the
     running game before the game starts.
- A MangoHud fork — the overlay + unified menu that also manages lsfg-vk
  profiles and env vars from inside the game.
- The upstream `lsfg-vk` layer — actually performs the frame generation.

The layer order is `game → LSFGVK → mangoverlay → driver`, so the HUD
measures the frames *after* generation.

## Requirements

- Linux (X11 or Wayland), systemd-free install: everything is user-local.
- A Vulkan-capable GPU.
- [Lossless Scaling](https://store.steampowered.com/app/993090) on Steam —
  its `lsfg-vk.dll` is needed by the runtime at launch.

## Install

### One-liner (recommended)

Downloads the project and installs everything in one shot:

```sh
curl -fsSL https://raw.githubusercontent.com/Axforzi/mangoverlay/v0.1.9/install.sh | bash
```

The script detects it is not running from a local clone, downloads and
extracts the latest project sources to a temp dir and runs the full
installer from there. No `chmod +x` and no sudo needed for the base
install; sudo is only asked for the optional build deps and the
`/usr/local/bin` symlink (both can be skipped).

**Precompiled by default.** Each release ships prebuilt binaries
(`mangoverlay-assets-<tag>.tar.gz`, built by the GitHub Actions workflow):
the installer downloads them and skips the local compilation — no `meson`,
`ninja` or `cmake` required. If the download is unavailable, or your distro's
glibc is older than the CI runner's (the prebuilt binaries would fail to load
with `GLIBC_2.35 not found`), it falls back to a local build automatically and
tells you. Pass `--build` to always compile locally.

### Local clone

```sh
git clone https://github.com/Axforzi/mangoverlay
cd mangoverlay
./install.sh [--skip-overlay] [--skip-lsfg] [--skip-deps]
             [--install-deps] [--dll <path>] [--yes] [--force]
             [--build] [--prefix <dir>] [--help]
```

Without flags, the script (when it is on a release tag, or started from the
one-liner):

1. Detects your distro/package manager and — when compulsory build deps are
   needed — installs them (with sudo, optional).
2. Downloads the precompiled release assets for the current tag and skips
   local compilation.
3. Installs the overlay layer and libraries under
   `~/.local/lib/mangoverlay/` and the layer manifest under
   `~/.local/share/vulkan/implicit_layer.d/`.
4. Installs the `mangoverlay` wrapper to `~/.local/bin/` and — with sudo
   consent — links it into `/usr/local/bin/` so Steam can find it.
5. Installs the upstream `lsfg-vk` layer (patches already applied in CI),
   placing its manifest so it loads *before* the overlay.
6. Locates `lsfg-vk.dll` (auto-search in the Steam install dir, or a file
   picker with zenity/kdialog, or `--dll <path>`) and writes the configs:
   `~/.config/lsfg-vk/conf.toml` and `~/.config/lsfg-vk/env.conf`.

On a git branch without a release tag, or with `--build`, steps 1-2 become
the classic local build: installing the toolchain with sudo, `meson`/`ninja`
for the overlay and `cmake` for lsfg-vk (with the per-profile `frame_limit`
patch).

All binaries and layers go under `~/.local` (or your `--prefix`); no sudo is
needed except for optional build deps and the `/usr/local/bin` symlink.
Existing configs are never overwritten.

> Requires bash. On a release tag the prebuilt assets skip the MangoHud
> sources entirely; when compiling locally the fork sources must be present
> as `MangoHud/` next to `install.sh`.

### Steam

For each game you want the overlay on, set its launch options to:

```
mangoverlay %command%
```

Then in-game:

| Action            | Input             |
|-------------------|-------------------|
| Open/close menu   | `Right Shift + F9` (or gamepad Start + Select) |
| Toggle HUD        | `Right Shift + F12` |
| Navigate          | Arrow keys / D-pad |
| Activate          | Enter / A          |
| Back / close      | Esc / B            |

### Uninstall

`mangoverlay` can uninstall itself — it knows where it was installed:

```sh
mangoverlay uninstall              # asks, then removes
mangoverlay uninstall --yes        # non-interactive
mangoverlay uninstall --purge-configs  # also removes your profiles
```

Or via `curl` when the wrapper is broken or missing:

```sh
curl -fsSL https://raw.githubusercontent.com/Axforzi/mangoverlay/v0.1.9/uninstall.sh | bash
```

By default your per-game configs (`~/.config/lsfg-vk/`) are **kept**; pass
`--purge-configs` to remove them too. Nothing else outside the installed
paths is touched.

## Configuration files

- `~/.config/MangoHud/MangoHud.conf` — HUD options (stats, colors, alpha,
  position, keybinds). Existing configs are preserved by the installer.
- `~/.config/lsfg-vk/conf.toml` — lsfg-vk settings, editable from the menu's
  **Frame generation** tab (multiplier, pacing, flow scale, ...) or with the
  `lsfg-vk-cli` tool.
- `~/.config/lsfg-vk/env.conf` — per-game env vars, editable from the menu's
  **Env** tab. Section names follow the game executable (`[GenshinImpact.exe]`),
  the Proton app name (`[hollow_knight.exe]`), or the numeric `SteamAppId`.

## Layout

```
overlay/
├── install.sh         # integrated installer (prebuilt-first, --build fallback)
├── uninstall.sh       # reversible uninstaller
├── mangoverlay        # per-game launch wrapper (source)
├── .github/workflows/ # CI: builds precompiled release assets per tag
├── MangoHud/          # the MangoHud fork (overlay + unified menu)
├── patches/           # lsfg-vk patches applied by the installer / CI
└── layer-test/        # throwaway layer sanity checks
```

## FAQ / troubleshooting

- **No HUD in game?** Check that `mangoverlay %command%` is the only launch
  option, then press `Right Shift + F12`. The `enable_environment` trigger
  (`MANGOHUD_UNIFIED=1`) is set by the wrapper, so the layer only activates
  in games launched through it.
- **A game has a black screen on launch after enabling a var in Env?** Set
  `disable_all_vars = true` in that game's section of `env.conf` (or clear
  the section) and relaunch — the game starts without any of its vars.
- **Double HUD?** The wrapper sets `DISABLE_MANGOHUD=1` for the game so the
  system MangoHud layer skips it; having both MangoHud forks disabled/enabled
  per game is the expected design.