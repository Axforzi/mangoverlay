# Literal section matching, i386 compile gate, atomic clean-all

## Objective

Close the three items left after the first hardening pass:

1. section names were compared as regular expressions;
2. the compile gate covered only x86_64;
3. `--clean-profiles` truncated `env.conf` in place.

## Problem

- The wrapper matched sections with `grep -q "^\[${SECTION}\]$"`. Section
  names are game executables and commonly contain dots (`Some Game.exe`), so
  `.` acted as a wildcard: `[Some GameXexe]` matched `[Some Game.exe]` and the
  bootstrap skipped creating the real section. The `active_in` check in
  `conf.toml` had the same flaw.
- `test.yml`'s `compile` job built x86_64 only. A 32-bit-only regression still
  reached the release tag, where the i386 build runs.
- `--clean-profiles` did `: > "$ENV_CONF"`; a crash between the truncation and
  the rest of the cleanup left the user with an empty config.

## Scope

- `mangoverlay`: replace every section-header grep with `grep -Fqx "[$name]"`
  and replace the `active_in` regex with a literal awk comparison.
- `.github/workflows/test.yml`: turn `compile` into an `x86_64`/`i386` matrix
  that mirrors `build-release-assets.yml` (same deps, cross file, ELF check).
- `mangoverlay`: write `env.conf` through a temp file and rename it in the
  clean-all path.
- `tests/test_wrapper_paths.sh`: assert the decoy does not match, and that
  clean-all leaves `version`/`[global]` with no profiles.

## Out of scope

- `lsfg-vk` ref pinning, `DLL_PATH` escaping, per-device fp16: separate work.
- Full shellcheck adoption.

## Acceptance criteria

- [x] A dotted section name does not match a decoy with a different character.
- [x] `active_in` is compared literally.
- [x] CI builds both x86_64 and i386 on every push/PR.
- [x] Clean-all renames instead of truncating.
- [x] Existing suites pass; the decoy assertions fail against the old grep.

## Verification

- `tests/run_tests.sh` -> OK, all suites; the wrapper test now has 9 assertions.
- Regression proof: a scratch copy with the old `grep -q "^\[...\]$"` and the
  old `active_in` regex fails exactly the two decoy assertions.
- YAML parse: `compile` is a matrix over `[x86_64, i386]` with the two build
  steps gated by `matrix.arch`; the i386 step mirrors
  `build-release-assets.yml` (cross file, `-Dmangohudctl=false`,
  `-Dwith_xnvctrl=disabled`, `-Dmangoplot=disabled`, ELF 32-bit check).
- Local i386 build: `meson setup --cross-file ... --buildtype=release` +
  `ninja` produced `ELF 32-bit LSB shared object, Intel i386`. The local run
  used a temp copy of the cross file pointing at this host's `/usr/lib32/pkgconfig`;
  the committed cross file keeps the Debian/Ubuntu path CI uses.
- `shellcheck` still unavailable; scripts validated with `bash -n` / `sh -n`.

## Progress

- Work unit commit on branch `fix/section-match-and-ci-i386`:
  `fix: match section names literally and gate i386 builds`. Not pushed;
  delivery is the repository owner's decision.
