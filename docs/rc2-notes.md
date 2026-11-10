# QLooper 1.0.0-rc2

RC2 includes the studio recording fixes and unified preparation/duration settings.

## Prepared start
- Free: Now or Wait for sound. The shared sound threshold is in General.
- Sync/Fixed: Now or Count-in. **1 = Next bar**, 2 = one full bar later, etc.
- The display counts upcoming boundaries, including the unfinished current bar, and shows the current beat.
- The main Count-in checkbox controls the same recording profile as Settings.
- QLooper Manager and Song Manager use the same count numbering for switching. A target recording profile can wait again after switching.
- Old Next bar and count-in settings retain their start timing. The count-in click still covers the full preparation bars after the first boundary; Count-in 1 adds no complete click bar.

## Recording length (per controller)
- **Until stopped**: end manually; memory limits still apply.
- **To loop end**: end at the next boundary of this looper, not each shorter master cycle.
- **Loop lengths: N**: capture N full loop durations from capture start.
- **Finish at loop boundary**: with N lengths, extend to the next cycle boundary, without adding a cycle if already aligned.
- If the initial length is unknown, first establish it manually. Fixed/master constraints supply a known initial length.
- **Stop after recording** independently stops playback after capture ends.
- Held Replace ends on release; held Add also obeys capture limits and ends on release.

Example: start halfway through a loop. To loop end records half a length; N=1 records one full length; N=1 with boundary completion records one and a half lengths.

## RC1 compatibility
- Saved Finish recording becomes To loop end, including existing overdubs. This intentionally changes completion from a full elapsed duration to the nearest loop end in cases where RC1 differed.
- Saved Record for becomes N loop lengths (zero becomes Until stopped). If Finish recording was also enabled, To loop end takes priority.
- Audio and layer data are retained. RC2 writes state version 18; older builds cannot read it. Save a separate session copy before testing RC2.

## Testing and distribution
- Test at 44.1/48 kHz first; enable Recorder during listening tests.
- Hardware TICK phase and preferred Stop fade remain studio-test items.
- Developer ID signing/notarization is still pending. Local builds are ad-hoc signed.
- No new public release is published merely by building RC2.
