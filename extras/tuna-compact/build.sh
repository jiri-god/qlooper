#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/../.." && pwd)
headers="$repo/build/_deps/juce-src/modules/juce_audio_processors_headless/format_types/LV2_SDK/lv2"
mkdir -p "$repo/build/tuna-compact"
clang++ -std=c++17 -dynamiclib -arch arm64 -fno-objc-arc -framework Cocoa -I "$headers" "$here/compact-ui.mm" -o "$repo/build/tuna-compact/tunaUI_compact.dylib"
codesign --force --sign - "$repo/build/tuna-compact/tunaUI_compact.dylib"
