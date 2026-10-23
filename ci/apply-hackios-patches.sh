#!/bin/sh
# Apply this repository's two HackiOS deltas to a checkout.
#
#   lumia950/ci/apply-hackios-patches.sh [tree]        (default: $HOME/hackios)
#
# The deltas live here for the same reason ci/unixnu-16x.patch does: they belong upstream, but
# this account cannot push to oosexgang's repositories. A tree that already carries a delta makes
# that patch a no-op, so running this twice is safe, and a checkout that has the change from
# upstream needs nothing.
#
#   hackios-kernel-setup.patch    xnu-deps/setup.sh: install the uuid headers, hand dsymutil to xcrun
#   hackios-userland-build.patch  userland/build.sh: copy drawable_test only when it was built
#
# Both paths are in the HackiOS work tree; the second file belongs to the userland repository,
# whose git directory is the tree's .userland.git. git apply does not consult an index, so the
# tree's own repository is enough for either.
set -eu
tree=${1:-$HOME/hackios}
here=$(cd "$(dirname "$0")" && pwd)

[ -d "$tree" ] || { echo "$0: no tree at $tree" >&2; exit 1; }

apply() { # apply WHAT PATCH
	[ -f "$2" ] || { echo "$0: no $2 beside the script" >&2; exit 1; }
	if (cd "$tree" && git apply --reverse --check "$2" >/dev/null 2>&1); then
		echo "$1: already there"
	else
		(cd "$tree" && git apply "$2")
		echo "$1: applied"
	fi
}

apply "xnu-deps/setup.sh" "$here/hackios-kernel-setup.patch"
apply "userland/build.sh" "$here/hackios-userland-build.patch"
