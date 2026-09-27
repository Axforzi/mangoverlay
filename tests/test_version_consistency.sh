#!/usr/bin/env bash
# Every version reference in the project has to agree, and has to match the tag
# being released.
#
# Why this needs a test: the one-liner is a chain, not a single link.
#
#   README.md          curl .../v0.2.3/install.sh | bash   <- what the user copies
#   install.sh:107     BOOT_BRANCH="v0.2.3"                 <- source tarball to fetch
#   install.sh:131     MANGO_PREBUILT_TAG="$BOOT_BRANCH"   <- prebuilt assets to fetch
#
# Bump the README and forget BOOT_BRANCH and the release is a lie: the user runs
# the v0.2.3 installer, it downloads the v0.2.2 source and the v0.2.2 binaries,
# and v0.2.3 never reaches anyone. Nothing warns about it. The release
# workflow's install-matrix only checks that the wrapper file exists, so a
# mismatched pin passes CI and ships.
set -uo pipefail

ROOT="${1:?usage: test_version_consistency.sh <project-root>}"
fail=0
check() { if [ "$1" = 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; fail=1; fi; }

# the version the installer pins, the one everything else must match
BOOT="$(grep -oE 'BOOT_BRANCH="v[0-9]+\.[0-9]+\.[0-9]+"' "$ROOT/install.sh" \
        | head -1 | grep -oE 'v[0-9]+\.[0-9]+\.[0-9]+')"
[ -n "$BOOT" ]
check $? "install.sh pins a BOOT_BRANCH (found: ${BOOT:-none})"
[ "$fail" = 0 ] || { echo; echo "FAILED"; exit 1; }

# Every reference that actually pins a version has to be that same version.
#
# Only pins are checked, not every vX.Y.Z string: the sources legitimately
# mention old versions in prose ("added v0.2.0", "the v0.2.1 asset bug"), and a
# test that flags those is a test people learn to ignore.
PIN_RE='(BOOT_BRANCH="|mangoverlay/)v[0-9]+\.[0-9]+\.[0-9]+'
off="$(grep -rhoE "$PIN_RE" "$ROOT/README.md" "$ROOT/install.sh" "$ROOT/uninstall.sh" \
       | grep -oE 'v[0-9]+\.[0-9]+\.[0-9]+' | sort -u | grep -v "^$BOOT\$" || true)"
[ -z "$off" ]
check $? "every version pin is $BOOT${off:+ (also found: $(echo "$off" | tr '\n' ' '))}"

# a bump that drops a reference entirely would also pass the check above, so
# assert the expected count: README install + uninstall one-liners, the three
# in-script copies of them, and BOOT_BRANCH itself
n_pins="$(grep -rhoE "$PIN_RE" "$ROOT/README.md" "$ROOT/install.sh" "$ROOT/uninstall.sh" | wc -l)"
[ "$n_pins" -ge 7 ]
check $? "found $n_pins version pins (expected at least 7)"

# the one-liners people actually copy must carry the tag
for f in README.md install.sh uninstall.sh; do
   for url in $(grep -oE 'https://raw\.githubusercontent\.com/Axforzi/mangoverlay/v[0-9]+\.[0-9]+\.[0-9]+' "$ROOT/$f" | sort -u); do
      case "$url" in
         *"/$BOOT") ;;
         *) echo "      $f: $url"; fail=1 ;;
      esac
   done
done
check $fail "every one-liner URL points at $BOOT"

# BOOT_BRANCH must not point at a tag that does not exist: the one-liner would
# 404 on the source download for every new user
git -C "$ROOT" rev-parse -q --verify "refs/tags/$BOOT" >/dev/null 2>&1
if [ $? -eq 0 ]; then
   echo "PASS  tag $BOOT exists"
elif [ "${ALLOW_UNTAGGED:-0}" = "1" ]; then
   echo "SKIP  tag $BOOT does not exist yet (expected before tagging)"
else
   echo "FAIL  tag $BOOT does not exist; the one-liner would 404"
   fail=1
fi

echo
[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILED"
exit "$fail"
