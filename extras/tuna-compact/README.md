# Compact x42 tuner UI

Cocoa LV2 editor, 420×70 logical pixels, for the existing x42 Instrument Tuner
(`http://gareus.org/oss/lv2/tuna#one`). Displays chromatic note, cents, frequency and
reference pitch. The x42 DSP and audio routing are unchanged. Reference pitch is
editable (220–880 Hz, Enter to apply; double-click restores 440 Hz) and written to
the existing plugin control port, so the host saves it with the session. Other advanced parameters remain outside this compact UI.

Build on this Apple Silicon Mac: `bash extras/tuna-compact/build.sh` from Element.
Requires the already configured JUCE LV2 headers and macOS SDK. Output:
`build/tuna-compact/tunaUI_compact.dylib` (ad-hoc signed).

Installation used on 2026-11-02, with Element closed:
- Copy output into `~/Library/Audio/Plug-Ins/LV2/tuna.lv2/`.
- Preserve `manifest.ttl` as `manifest.ttl.before-compact` once.
- In manifest, change `ui:binary <tunaUI_gl.dylib>` to
  `ui:binary <tunaUI_compact.dylib>`, retaining the original UI URI.
- Restart host. Original binary remains intact. Restore backed-up manifest and
  restart to revert. A vendor reinstall can overwrite this local integration.

The shared UI URI is also referenced by the spectrum variant in the bundle;
this UI targets the Instrument Tuner, not its spectrum analyser.

`check.mm` includes the UI and checks pitch labels (including bass B0/E1), float
port delivery, parent-view attachment/cleanup and renders `/tmp/tuna-compact.png`.
Compile with Cocoa and the same LV2 include path as build.sh, then run locally.
No live audio is emitted by that check. Studio audition remains required.

License: this newly authored UI is GPL-3.0-or-later. Existing x42 tuna.lv2 0.6.10
source revision d6d2341e4a0b5eb71b12943e8692e14dcf0645b1 is GPL-2.0-or-later.
No original GUI artwork or DSP was copied. Redistribution still needs the project's
normal dependency/license review; this local install is not a public package.
