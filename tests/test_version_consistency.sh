#!/usr/bin/env bash
# Keeps the documented one-liners and the version they actually install in step.
#
# The one-liner points at master, so the URL no longer decides the version.
# BOOT_BRANCH does: it names the tag the source tarball and the precompiled
# assets are fetched from. That means there is exactly one version reference to
# bump on a release, and the real invariants are:
#
#   1. every documented URL points at the SAME ref (they are copied around, and
#      a mix would send people to different code)
#   2. BOOT_BRANCH is a well-formed version
#   3. that tag actually exists, so the bootstrap download cannot 404
#
# Check 3 is enforced only on a tag ref. On master the release commit lands
# before the tag is pushed, and failing there would turn the test workflow red on
# every single release.
#
# A stale version left in a URL is still worth catching: it would keep pointing
# people at an old release forever while master moves on.
set -uo pipefail

ROOT="${1:?usage: test_version_consistency.sh <project-root>}"
fail=0
check() { if [ "$1" = 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; fail=1; fi; }

BOOT="$(grep -oE 'BOOT_BRANCH="v[0-9]+\.[0-9]+\.[0-9]+"' "$ROOT/install.sh" \
        | head -1 | grep -oE 'v[0-9]+\.[0-9]+\.[0-9]+')"
[ -n "$BOOT" ]
check $? "install.sh pins a BOOT_BRANCH (found: ${BOOT:-none})"
[ "$fail" = 0 ] || { echo; echo "FAILED"; exit 1; }

# 1. every documented URL must name the same ref
refs="$(grep -rhoE 'raw\.githubusercontent\.com/Axforzi/mangoverlay/[A-Za-z0-9._-]+' \
        "$ROOT/README.md" "$ROOT/install.sh" "$ROOT/uninstall.sh" \
        | sed 's|.*/||' | sort -u)"
n_refs="$(printf '%s\n' "$refs" | grep -c .)"
[ "$n_refs" = 1 ]
check $? "every one-liner URL names the same ref ($(echo "$refs" | tr '\n' ' '))"

# the ref has to be something the installer can actually fetch
main_ref="$(printf '%s\n' "$refs" | head -1)"
case "$main_ref" in
    master|main) echo "PASS  the documented ref follows a branch ($main_ref)";;
    v[0-9]*.[0-9]*.[0-9]*) echo "PASS  the documented ref is a release tag ($main_ref)";;
    *) echo "FAIL  the documented ref '$main_ref' is neither a branch nor a tag"; fail=1;;
esac

# 2/3. the tag the install will actually use must exist
if git -C "$ROOT" rev-parse -q --verify "refs/tags/$BOOT" >/dev/null 2>&1; then
   echo "PASS  tag $BOOT exists, so the bootstrap download cannot 404"
else
   REF_TYPE="${GITHUB_REF_TYPE:-branch}"
   REF_NAME="${GITHUB_REF_NAME:-}"
   if [ "$REF_TYPE" = "tag" ]; then
      echo "FAIL  tag $BOOT does not exist; the one-liner would 404"
      fail=1
   else
      echo "SKIP  tag $BOOT does not exist yet (expected on a $REF_TYPE ref)"
   fi
   # on a tag build, the tag being built must be the one installs point at
   if [ "$REF_TYPE" = "tag" ] && [ "$REF_NAME" != "$BOOT" ]; then
      echo "FAIL  the tag being built ($REF_NAME) is not $BOOT"
      fail=1
   fi
fi

# a version left behind in a URL would keep serving an old release
stale="$(grep -rhoE 'raw\.githubusercontent\.com/Axforzi/mangoverlay/v[0-9.]+' \
         "$ROOT/README.md" "$ROOT/install.sh" "$ROOT/uninstall.sh" | sort -u)"
[ -z "$stale" ]
check $? "no one-liner URL is pinned to a version${stale:+ (found: $(echo "$stale" | tr '\n' ' '))}"

echo
[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILED"
exit "$fail"
