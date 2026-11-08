#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-"$root/build"}
out=${2:-"$root/dist"}
app="$build/element_app_artefacts/RelWithDebInfo/QLooper.app"
[[ -d "$app" ]] || { echo "Missing app: $app" >&2; exit 1; }
mkdir -p "$out"
stage=$(mktemp -d "${TMPDIR:-/tmp}/qlooper-dmg.XXXXXX")
trap 'rm -rf "$stage"' EXIT
ditto "$app" "$stage/QLooper.app"
mkdir -p "$stage/Documentation" "$stage/QLooper.app/Contents/Resources/Licenses"
cp "$root/docs/quick-start.md" "$root/docs/rc1-notes.md" "$stage/Documentation/"
cp "$root/LICENSE" "$root/LICENSING.md" "$root/THIRD-PARTY-NOTICES.md" "$stage/Documentation/"
ditto "$root/LICENSES" "$stage/QLooper.app/Contents/Resources/Licenses"
printf '[InternetShortcut]\nURL=https://github.com/jiri-god/qlooper\n' > "$stage/Source code.url"
ln -s /Applications "$stage/Applications"
codesign --force --sign "${SIGN_IDENTITY:--}" "$stage/QLooper.app"
codesign --verify --deep --strict "$stage/QLooper.app"
name=QLooper-1.0.0-rc1-macOS-arm64.dmg
hdiutil create -ov -volname 'QLooper RC1' -srcfolder "$stage" -format UDZO "$out/$name"
hdiutil verify "$out/$name"
(cd "$out" && shasum -a 256 "$name" > "$name.sha256")
echo "Created $out/$name (not notarized by this script)"
