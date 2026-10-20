# Installer and wrapper hardening

## Objective

Stop three failure modes that turn a non-fatal problem into a broken install or
a silently lost setting:

1. a failed `conf.toml` heal aborts an otherwise complete install;
2. the menu and the wrapper write `env.conf`/`conf.toml` with no shared lock;
3. the wrapper splits a Steam library path on spaces.

## Problem

- `install.sh` calls `heal_conf_toml_dll_key` unguarded under `set -euo pipefail`.
  The function returns 1 when `awk` or `mv` fails. By then the libraries and the
  wrapper are installed, but `write_dll_path`, `write_env_conf` and the summary
  are skipped.
- `overlay_menu.cpp` rewrites `env.conf` and `conf.toml` atomically (tmp file +
  rename). The wrapper appends new sections with `>>` at launch. Atomic writes
  prevent torn files but not lost updates: the menu can read, the wrapper can
  append, and the menu's rename then discards the appended line.
- `steam_appid_section` iterates library roots with `for _lib in $(sed ...)`.
  Default IFS splits on spaces, so `"/mnt/My Games/SteamLibrary"` becomes two
  candidates and the manifest is never found.

## Scope

- `install.sh`: guard the heal call; a config problem no longer aborts the
  install.
- `mangoverlay`: word-split library paths on newlines only (and disable globbing
  while doing it); take `flock` on `<config-dir>/.mangoverlay.lock` around the
  bootstrap appends.
- `MangoHud/src/overlay_menu.cpp`: take the same `flock` around every
  read-modify-write of `conf.toml` and `env.conf`.
- `tests/`: update the installer call-site assertion; add an end-to-end wrapper
  test with a library path containing spaces.

## Out of scope

- Rollback of a partially completed install.
- `set -e`/`set -u` in the wrapper.
- Full shellcheck adoption.

## Acceptance criteria

- [x] A failing heal prints its reason and the install continues.
- [x] Both writers serialize on the same lock file; a wrapper append during a
      menu write is not lost.
- [x] A Steam library path with spaces resolves to the right section.
- [x] Existing suites still pass; the new wrapper test fails if the quoting fix
      is reverted.
- [x] The overlay still builds with the release flags.

## Verification

- `tests/run_tests.sh` -> OK, all suites.
- New `tests/test_wrapper_paths.sh` -> all assertions pass. It boots the shipped
  wrapper against a fake HOME whose default Steam install lists a library under
  `My Games/SteamLibrary`, and asserts the bootstrapped `[Some Game.exe]` section
  and its `[[profile]]`.
- Regression proof: reverting only the quoting change in a scratch copy made the
  four section/profile assertions fail.
- Lock wiring: the wrapper test asserts `<config-dir>/.mangoverlay.lock` is
  created. The C++ sides compile and link in both overlay targets.
- Local `meson setup /tmp/mangohud-build --buildtype=release -Dmangohudctl=true`
  + `ninja` produced `src/libMangoHud.so` after the change, with no warning from
  `overlay_menu.cpp`.
- `shellcheck` is not installed on this machine, so the scripts were not linted
  beyond `bash -n` / `sh -n` in the suite. Disclosed, not silently skipped.

## Known limitation

Every writer of `env.conf`/`conf.toml` now takes the same lock: the in-game menu,
the launch wrapper's bootstrap, and the `--clean-profiles` command. There is no
multi-process stress test; proving a race deterministically would need a fault
injection point, which is not worth adding here.

## Progress

- Work unit commit on branch `fix/install-hardening`: `fix: harden installer and
  wrapper config writes`. Not pushed; delivery is the repository owner's
  decision.
