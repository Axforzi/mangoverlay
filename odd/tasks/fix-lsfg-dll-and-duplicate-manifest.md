# Fix: lsfg-vk `dll` key loss + duplicate LSFGVK layer manifest

## Objective

Stop mangoverlay from producing a frame-generation setup that silently freezes the
game, and stop the installer from leaving two manifests for the same Vulkan layer.

## Problem

**Bug 1 - the menu drops the `dll` key.** `MangoHud/src/overlay_menu.cpp`
`lsfg_create_profile_for()` rebuilds `conf.toml` from scratch when the file is
missing or unreadable, and the rebuilt `[global]` block only contains
`allow_fp16`. The `dll = "..."` key that `install.sh` writes (install.sh:976-978)
is not re-emitted, so the lsfg-vk layer loses the path to `lsfg-vk.dll`. With no
`dll` key the layer falls back to searching the Steam tree; when the DLL lives
elsewhere the layer enters frame generation with no shader library and the game
freezes with no error. Confirmed on this machine: DLL at
`/mnt/Backup/Juegos/Lossless/lsfg-vk.dll`, absent from the config, game froze at
`multiplier = 2`. Restoring the key made it work (log: "Shader library loaded
successfully", 26 pipelines built).

**Bug 2 - duplicate layer manifest.** The installer places the LSFGVK manifest in
`$VK_CONFIG_DIR` (`~/.config/vulkan/implicit_layer.d`) so the layer loads before
MangoHud. It never removes a stale manifest left at the cmake/prebuilt source
location `$VK_DIR` (`~/.local/share/vulkan/implicit_layer.d`) by a previous
install. The Vulkan loader then reports on every process start:
`Removing layer VK_LAYER_LSFGVK_frame_generation ... because it is a duplicate of`.
Confirmed present on this machine.

**Bug 3 - `allow_fp16` hardcoded.** The same rebuild path wrote
`allow_fp16 = true` with no capability query anywhere in the menu. Half-precision
compute needs native fp16 units, which arrived with GCN5 (Vega, 2017); GCN4 and
older do not have them. This machine's Radeon RX 550 (POLARIS12) reports
`shaderFloat16 = false`, so the written value asks the layer for something the
hardware cannot do. Not the cause of the freeze, but the same class of bug: a
value written without asking the device.

## Scope

- `MangoHud/src/overlay_menu.cpp` - preserve and heal the `dll` key; warn when
  frame generation is enabled with no resolvable DLL.
- `MangoHud/src/vulkan.cpp` + `overlay.h` - record whether the device reports
  `shaderFloat16` so the menu stops hardcoding `allow_fp16 = true`.
- `install.sh` - record the resolved DLL path for the menu; remove stale
  manifests at the source location on both the prebuilt and local-build paths.
- `uninstall.sh` - remove the recorded DLL path file.

## Tasks

- [x] T1 Read and confirm both root causes against the running system
- [x] T2 Record the resolved DLL path in install.sh so the menu can recover it
- [x] T3 Remove stale duplicate manifests at `$VK_DIR` on both install paths
- [x] T4 Preserve the `dll` key when the menu rewrites `conf.toml`
- [x] T5 Heal a config that has no `dll` key, discovering it in priority order
- [x] T6 Warn in-game when FG is on but no DLL resolves
- [x] T7 Clean the new state file in uninstall.sh
- [x] T8 Verify: build the overlay, review the diff, clean the live duplicate
- [x] T9 Query shaderFloat16 instead of hardcoding allow_fp16

## Constraints

- Never overwrite a user-set `dll` key. The menu may only add one when absent.
- Generated artifacts stay in English regardless of conversation language.
- The menu runs inside a game; it must never block. Discovery is a bounded
  filesystem probe and runs only on config creation.
- No new hard dependency for the prebuilt path.

## Acceptance criteria

1. Creating a fresh `conf.toml` from the menu yields a `[global]` block carrying
   a `dll` key whenever a DLL is discoverable, and never erases an existing one.
2. A profile with `multiplier > 1` and no resolvable DLL produces a clear
   diagnostic instead of a silent freeze.
3. After a full install, exactly one manifest per LSFGVK layer name exists
   across all implicit-layer directories, and the loader emits no duplicate
   warning.
4. `mangoverlay uninstall` leaves no state file behind.

## Verification

A full `meson` build was not possible here: `meson` is not installed, the
`subprojects/*.wrap` deps are remote, and the wayland/dbus headers are missing.
The new logic was therefore verified by extracting the shipped functions from
`overlay_menu.cpp` and `install.sh` and exercising them directly, so what ran is
the real code and not a copy of it.

- `bash -n install.sh`, `bash -n uninstall.sh`, `sh -n mangoverlay` - pass.
- `g++ -std=c++17 -Wall -Wextra` over the 11 extracted functions - compiles;
  only unused-function warnings from the harness.
- 12/12 assertions in the extracted-logic test: nothing is invented when no DLL
  exists, an error is logged instead of failing silently, the key is inserted
  directly after `[global]`, an existing key is preserved verbatim, a
  profile-scoped `dll` is not mistaken for a global one, `[global]` does not
  swallow a later table's keys, `MANGOVERLAY_LSFG_DLL` wins, a non-existent
  override falls through, and a dangling `dll.path` is not trusted.
- 15/15 after the `allow_fp16` change, adding: a device reporting fp16 gets
  `allow_fp16 = true`, one without gets `false`, a user-set value is kept in
  both directions, both keys land inside `[global]`, and a missing `[global]`
  header is reported instead of written past.
- 8/8 assertions in the extracted `install.sh` test, including that manifests
  belonging to other projects are left alone and that the function is defined
  before both of its call sites.
- Live: `VK_LOADER_DEBUG=warn vulkaninfo --summary` reported 74 duplicate LSFGVK
  warnings before and 0 after; exactly one manifest per layer name remains, in
  `~/.config/vulkan/implicit_layer.d/`.
- Build-correctness reviewed by hand rather than by a compiler: the OpenGL target
  compiles `overlay_menu.cpp` but not `vulkan.cpp` (`src/meson.build:229`), and
  resolves the new symbol through `mangohud_static_lib`; the version script's
  `local: *` does not matter because both objects land in the same output. The
  dispatch-table name `GetPhysicalDeviceFeatures` matches the mapping already
  used by `GetPhysicalDeviceProperties` and `GetPhysicalDeviceProperties2` in the
  same function.

## Follow-ups not addressed here

- `device_supports_fp16()` keeps the last device created, so on a multi-GPU setup
  it reflects whichever `vkCreateDevice` ran last. That matches the existing
  `gpu` global, so it is consistent, but it is not per-adapter.
- The `switch` in `draw_overlay_menu` was wrapped in an `else` without
  re-indenting its body, to keep the diff readable.
- No automated test runs in CI. The extraction harness used here is not wired in;
  it is a reasonable candidate for a `tests/` target.
