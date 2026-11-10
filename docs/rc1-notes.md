# QLooper 1.0.0-rc1

First public source/distribution candidate of the native Element-based looper.
Feature development is frozen while real-rig testing continues.

- Apple Silicon build; deployment target macOS 14. Current testing is on macOS 26.
- The initial local DMG is ad-hoc signed, **not Developer ID signed or notarized**.
- Tuner (x42), metronome (TICK) and third-party effects are optional external plugins.
- Switching sessions with QLooper View open can leave the embedded tuner black;
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

## Studio fixes — 9 November 2026

- Overlapping Record, Overdub and Custom pedal gestures: the first held control owns the gesture. A second overlapping press and its release are ignored; release it before trying again.
- A linked first capture with a predetermined length now follows **At loop boundary**. New layer leaves the uncaptured prefix silent and continues in a new part of the same One Take at the cycle boundary. Overwrite keeps reusing the layer. A first multiples-of-master capture still establishes its length before normal layering starts.
- **Finish recording** also applies to a predetermined master-linked length. **Stop after recording** remains a separate playback choice.
- Count-in displays the full remaining wait, including a partial leading bar, plus the current beat. Two full count-in bars requested mid-bar therefore begin with 3 on the display. Recording timing and the TICK route are unchanged.
- Count-in bars/metronome require Count-in in Sync/Fixed; Previous pass requires Overwrite; Finish recording requires a predetermined Fixed/master length. Unavailable settings retain their stored values. Mode, length and memory edits are locked during recording gestures and pending capture work.

Validation: all 74 automated test suites passed. Hardware TICK phase and preferred Stop fade remain studio-test items; enable Recorder for listening tests.
