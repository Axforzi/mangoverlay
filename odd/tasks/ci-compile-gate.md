# CI compile gate + reachable test diagnostics

## Objective

Catch compile errors in the MangoHud fork on every push and pull request, instead
of only when a release tag is cut, and make a harness compile failure print its
diagnostics instead of aborting silently.

## Problem

- `.github/workflows/test.yml` runs only `tests/run_tests.sh`. That script compiles
  awk-extracted functions from `overlay_menu.cpp`; it never invokes `meson` or
  `ninja`. The full fork is compiled only by `build-release-assets.yml`, which
  triggers on tags `v*`. A compile error in `vulkan.cpp`, `overlay.cpp`,
  `gamepad.cpp` or any other fork source reaches master and is discovered only
  when a release is attempted.
- `tests/run_tests.sh` invokes `g++` under `set -euo pipefail`. A compile failure
  aborts the script before the `if [ $? -ne 0 ]` block that prints
  `$WORK/build.log`, so CI goes red with an empty log.

## Scope

- `.github/workflows/test.yml`: add a `compile` job that mirrors the release
  workflow's x86_64 build (same runner, same dependencies, `fetch-depth: 0` for
  `vcs_tag`, `meson setup` + `ninja`, assert `libMangoHud.so`).
- `tests/run_tests.sh`: capture the compiler exit status without tripping
  `set -e`, then print `build.log` on failure.

## Out of scope

- i386 compile in the fast gate: it stays on release tags only.
- The installer and wrapper fixes tracked separately.

## Acceptance criteria

- [x] A compile error in any `MangoHud/src/*.cpp` fails the `compile` job on push
      and pull request.
- [x] A harness compile failure prints `build.log` and exits non-zero.
- [x] `tests/run_tests.sh` still passes on a clean tree.
- [x] `test.yml` remains valid YAML.

## Verification

- `tests/run_tests.sh` -> exit 0, all suites OK.
- Simulated compiler failure (`CXX` pointed at a stub that exits 1) -> the script
  printed `ERROR: the test harness failed to compile:` plus `build.log` and
  exited 1. Before the fix it aborted silently.
- `test.yml` parsed with PyYAML: jobs `['tests', 'compile']`.
- The compile job's command was reproduced locally out-of-tree with meson 1.12.1:
  `meson setup /tmp/mangohud-build --buildtype=release -Dmangohudctl=true` +
  `ninja` produced `src/libMangoHud.so` (113 targets). Local config added
  `-Dwith_xnvctrl=disabled` only because the host lacks those headers; CI
  installs `libxnvctrl-dev` and mirrors the release flags exactly.

## Progress

- Work unit commit on branch `ci/compile-gate`: `ci: compile the overlay on every
  push and pull request`. Not pushed; delivery is the repository owner's
  decision.
