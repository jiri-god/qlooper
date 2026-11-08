# QLooper 1.0.0-rc1

First public source/distribution candidate of the native Element-based looper.
Feature development is frozen while real-rig testing continues.

- Apple Silicon build; deployment target macOS 14. Current testing is on macOS 26.
- The initial local DMG is ad-hoc signed, **not Developer ID signed or notarized**.
- Tuner (x42), metronome (TICK) and third-party effects are optional external plugins.
- Switching sessions with Looper View open can leave the embedded tuner black;
  restarting the host is the known workaround. No tuner binary is shipped here.
- Large layer limits, long recordings and high sample rates can use substantial
  RAM. Start with defaults at 44.1/48 kHz and 128/256 samples, then test your rig.
- Changing the graph/plugins while playing may interrupt audio. Prepare the graph
  before performance; external plugin behaviour cannot be guaranteed by QLooper.
- Automated tests do not replace hardware/studio listening tests. Use Recorder
  during listening tests and retain any recording containing a click/dropout.
- The app uses separate QLooper preferences. Existing Element sessions can be
  opened explicitly; save a copy before using them in RC1.

No feature compatibility or live-stage certification is claimed for untested
platforms, devices or third-party plugins. Stable release follows user testing.
