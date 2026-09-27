# tests

Fast checks for the parts of mangoverlay that fail silently.

```sh
tests/run_tests.sh
```

Runs in about a second and needs nothing but a C++17 compiler. No meson, no
ninja, no Vulkan headers, no ImGui. That is deliberate: the point is to catch
regressions on every push, not at release time.

## Why these exist

Every bug found in the lsfg-vk area had the same shape. The code compiled, read
correctly, and passed review. It only misbehaved at runtime, and nothing said
so:

- The `dll` key vanished when the in-game menu rebuilt `conf.toml`. The frame
  generation layer then looked for a shader library it could not find and
  **froze the game with no error**.
- A stale Vulkan layer manifest made the loader discard a copy of the layer on
  every process start. Invisible unless you go looking for loader warnings.
- `allow_fp16` was hardcoded to `true` on devices with no fp16 units, which is
  a perfectly plausible-looking value that is simply wrong on the hardware.

A compiler and a reviewer catch none of that. These tests do.

## How the C++ test works

`overlay_menu.cpp` cannot be compiled on its own: it pulls in ImGui, the Vulkan
dispatch table and the whole overlay state. So `extract_functions.sh` pulls the
functions under test out of the real source with `awk` and the harness compiles
those, against stubs for logging and `device_supports_fp16()`.

**The code under test is the shipped code**, extracted on every run, not a copy
that can drift.

The extraction assumes the project's formatting: the return type sits on its own
line above the signature, and the body ends at the first `}` in column 1. If that
ever stops holding, the script exits non-zero with a clear message rather than
silently testing less than it claims.

## What is covered

`test_lsfg_global_keys.cpp` — the `[global]` block of `conf.toml`:

- a missing `dll` is reported, never invented from thin air
- a `dll` key is restored from the path the installer recorded
- a `dll` or `allow_fp16` value the user set is never overwritten
- `allow_fp16` follows the device: `true` with fp16 units, `false` without
- a missing `[global]` header is reported instead of written past
- `[global]` does not swallow the keys of a later table
- a profile-scoped `dll` is not mistaken for a global one

`test_install_manifests.sh` — `remove_stale_lsfg_manifests()` in `install.sh`:

- stale 64-bit and 32-bit manifests are removed
- manifests belonging to other projects are left alone
- the function is defined before both of its call sites

Plus a parse check on `install.sh`, `uninstall.sh` and the `mangoverlay`
wrapper, so a shell syntax error cannot reach a release.

## Adding a test

1. Add the function name to `FUNCS` in `extract_functions.sh`.
2. Call it from `test_lsfg_global_keys.cpp`.

If a function cannot be extracted, the run fails. Keep it that way.
