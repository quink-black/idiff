#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# macos_install.command
#
# Double-click helper that sidesteps macOS Gatekeeper's complaints about
# ad-hoc signed apps. Distributed next to idiff.app (inside the .dmg and
# inside the release .zip). When users double-click this file Finder runs
# it in Terminal; the script then:
#
#   1. locates idiff.app sitting next to it,
#   2. copies it into /Applications (overwriting any previous install),
#   3. strips com.apple.quarantine recursively so Gatekeeper stops
#      whining about "app is damaged" / "cannot verify developer".
#
# No sudo. No Developer ID. No notarization. Just removes the quarantine
# bit the browser set when the dmg/zip was downloaded.
# -----------------------------------------------------------------------------

set -e

# The bundled app ships next to this script regardless of whether the
# user launched us from a mounted dmg or an extracted zip folder.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SRC_APP="$SCRIPT_DIR/idiff.app"
DST_APP="/Applications/idiff.app"

echo "idiff installer"
echo "---------------"

if [[ ! -d "$SRC_APP" ]]; then
    echo "ERROR: idiff.app not found next to this script."
    echo "       Expected at: $SRC_APP"
    echo
    echo "Press any key to close..."
    read -rn 1
    exit 1
fi

echo "Installing $SRC_APP -> $DST_APP ..."
# -R preserves the bundle structure; remove any previous install first so
# stale dylibs from an older version don't linger in Frameworks/.
rm -rf "$DST_APP"
cp -R "$SRC_APP" "$DST_APP"

echo "Clearing com.apple.quarantine attribute..."
# -r: recurse into the bundle (each Mach-O inside can carry the bit).
# -s: don't fail if the attribute is already absent.
xattr -drs com.apple.quarantine "$DST_APP" 2>/dev/null || true

echo
echo "Done. Launch idiff from /Applications or Spotlight (Cmd+Space -> idiff)."
echo
echo "Press any key to close..."
read -rn 1
