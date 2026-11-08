# QLooper RC1 — your first loop

QLooper includes a modified Element host. You do **not** need a separate Element
installation. Some menus and internal nodes still use the Element name.

## 1. Install and select your audio device

1. Open the DMG and drag **QLooper.app** to **Applications**. Eject the DMG and
   open the copied app. Use this build: stock Element does not contain our nodes.
   Choose the Apple Silicon build. macOS 14 is the build minimum; RC1 has been
   tested locally on macOS 26, not on every older macOS release.
2. This initial RC1 is not notarized. If macOS blocks it, follow Apple's
   [Open Anyway procedure](https://support.apple.com/en-us/102445) in
   **System Settings → Privacy & Security**, after verifying the download source.
   Do not disable Gatekeeper globally.
3. Allow microphone/audio-input access when requested. Double-click the device
   status at the bottom of the main window to open audio settings, then choose
   your audio interface for input and output; enable the channels you use.
4. Start at **44.1 or 48 kHz**, **128 or 256 samples**. Use headphones for microphone
   tests to avoid acoustic feedback. Start with output volume low.

## 2. Add and connect a Looper

1. Create a new session/graph. Show the **Plugins** panel, find **Looper** among
   the built-in Element nodes and drag it into the graph. The Plugins panel is
   in the left sidebar; View → Plugin Manager also lists the available modules.
   The expected names are Looper, Looper Manager and Song Manager. No plugin scan is
   required for our built-in nodes.
2. Connect **Audio In → Looper → Audio Out**. For a mono instrument, connect its
   input channel to both Looper input channels to hear it in both ears. Connect
   Looper left/right outputs to the output pair feeding your headphones/speakers.
3. Double-click the Looper to open its full editor. **Monitor** enables hearing
   the live input; **Mute** silences recorded loops; **Bypass** passes input through.
4. For a first test select **Settings → General → Mode → Free**. In
   **Settings → Recording**, set the Overdub start timing to **Now** for this
   first test; leave **Stop after** off. Close Settings to return to the controls.
5. Click **Overdub** to start the first recording; click again to finish it and
   hear it repeat. Click Overdub again to add another layer, then again to finish.
6. **Play/Stop** toggles playback. **Undo/Redo** works on an entire recording/One
   Take. Record has its own configurable behaviour; inspect Settings → Recording
   before using it on material you want to keep.
7. **File → Save Session As...** saves your session and layers. Keep a backup before tests.

When the looper editor has keyboard focus and you are not editing text:
**A** = Overdub, **S** = Record, **D** = Play/Stop, **Z/X** = Undo/Redo.
The labels in the current editor show the shortcuts. MIDI is optional.

## 3. Work with layers and timing

- Layers have volume/pan, Add/Replace, On, Solo and No FB (ignore feedback).
  The waveform shows the recorded audio, independent of its current volume.
- **Layers…** opens the larger window. Select a waveform for a larger preview.
- **Import** loads prepared audio; files are resampled as needed, not time-stretched.
- **Sync** follows rig tempo; **Fixed bars** sets the initial loop length in bars.
  Configure start/count-in separately for Record, Overdub and the custom profiles.
- Hover a control for a short explanation. In Settings, **?** opens detailed help.

## 4. Add a Looper Manager

1. Add another Looper and a **Looper Manager** from the built-in nodes.
2. Wire the Manager's **MIDI output to each Looper's MIDI input** in that graph.
   Connect the hardware MIDI input to the Manager if you use shared pedals.
3. Open the Manager. Its boxes control individual loopers; **Play All / Stop All**
   control the group. **Active** identifies the looper receiving shared commands.
4. Drag a compatible looper's header into **Master**; assign other loopers to
   **Linked** or leave them **Independent**. Linking has compatibility/recording
   restrictions and never means automatic time-stretching of imported audio.
5. Assign your controller in **Settings → MIDI**. Hold actions need both press
   and release messages (for example MIDI CC values 127 and 0).

## 5. Sections and the combined view

- Prepare section graphs (for example Verse and Chorus), with their audio paths
  connected to the parent graph. Each can contain its own Looper Manager/effects.
- Add **Song Manager** at the parent level. Add the prepared sections to its list
  in the order you need. Next section uses its timing settings; a double-click on
  a section switches immediately. **Panic** silences the song; **Restore audio**
  restores sound without restarting stopped loops.
- Open **Window → Looper View**. It brings Song Manager, Looper Manager, the shown
  Looper and layers together. Missing modules are shown as empty areas.
- The arrows above the looper change the **displayed** looper. Use the Manager
  to change MIDI/pedal control. Split / Loopers / Layers changes the space allocation.

## Optional effects, tuner and metronome

Add your own compatible AU/VST3/LV2 effects in Element's plugin panel and route
**Audio In → effect → Looper → Audio Out** to record that effect, or put effects
after the looper to process playback. Install/scan external plugins separately.

The compact tuner integration expects x42 Instrument Tuner and our optional
compact UI. TICK is an external metronome; neither binary is included in this
first DMG. The top Metronome checkbox controls a matching TICK node when present.
Looping itself needs neither plugin. Keep metronome audio on a separate output
path if you do not want it recorded in the loop.

## If there is no sound

- Check audio device, enabled channels and cables from input through to output.
- Check Monitor, Mute, Bypass, mixer level and Song Manager's active section/Panic.
- Rig transport and loop playback are separate controls; transport Play alone
  does not necessarily play a stopped loop.
- For a useful bug report: include RC1 version, macOS, device, sample rate/buffer,
  exact actions and whether the fault is audible in a Recorder capture.
