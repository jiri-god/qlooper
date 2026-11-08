# QLooper RC1

**Looping comes first.** Layers become loops. Loops become music.

QLooper — a live layered looper, QLooper Manager, Song Manager and combined
QLooper View, built into a modified [Kushview Element](https://github.com/kushview/element).
An independent release by **princ.biz**, not an official Kushview product.

- Website: https://qlooper.princ.biz
- [Quick start](docs/quick-start.md) — installation, your first loop, managers and MIDI.
- [Downloads](https://github.com/jiri-god/qlooper/releases) — choose RC1 for Apple Silicon.
- [Report a problem](https://github.com/jiri-god/qlooper/issues) — include your Mac,
  macOS, audio device, sample rate, buffer size and steps to reproduce.
- [Build from source](docs/building.md) · [Licensing](LICENSING.md) · [Known limitations](docs/rc1-notes.md)

## What is included

Native QLooper with separate editable layers and One Takes, Record/Overdub/custom
recording profiles, Free/Sync/Fixed bars, retrospective capture, audio-file import,
Undo/Redo, feedback, fades and MIDI control. QLooper Manager coordinates several
loopers; Song Manager switches prepared sections. Window → QLooper View brings
these editors together. Recorder and Sample Pad are also included.

External effects, x42 tuner and TICK metronome are **not bundled**. Looping works
without them. Optional external-plugin setup is described in the quick start.

## RC1 means testing

This is the first public release candidate, not a claim of stage-certified
reliability. Keep backups and test your complete rig before a performance.
The distributed build targets Apple Silicon and macOS 14 or later; current
validation is on one Apple Silicon Mac running macOS 26. Other Macs and OS versions
have not been verified. Linux/Windows code is inherited but not supported by this release.

## Open source and future plugins

The Element-based macOS edition is free and open source. A paid VST/AU edition
is a future plan, **not included in RC1**. GPL-covered source retains its notices;
the combined JUCE 8 build also has AGPLv3 obligations. See LICENSING.md for precise
scope, dependency notices, source availability and the future licensing boundary.

Original Element contributors retain their copyrights; see AUTHORS.md. This
repository starts from a curated source snapshot, excluding private recordings,
credentials, local settings and development history.

Printable guide: [Quick start PDF](docs/QLooper-Quick-Start.pdf). An offline HTML version is included in the DMG.
