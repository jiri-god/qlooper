# Third-party notices — QLooper RC2

The release is an independent modification of Kushview Element. Original
copyrights and licence notices are retained. Sources and complete notices for
fetched dependencies accompany the binary in the dependency-source archive.

| Component | Exact source / version | Terms |
| --- | --- | --- |
| Element | https://github.com/kushview/element, base 618e1ac094839be64849bcadc145363702527a4f plus QLooper modifications | GPL-3.0-or-later; AUTHORS.md and source headers |
| QLooper native nodes, managers, host fixes | this source snapshot | GPL-3.0-or-later, unless otherwise marked |
| JUCE | 8.0.13, 7c9d3783b127263d72bb65fe0a7e2dc8a02a7ac2 | AGPLv3 open-source option; framework LICENSE.md and nested notices |
| sol2 | c1f95a773c6f8f4fde8ca3efe872e7286afe4444 | MIT; LICENSE.txt in dependency sources |
| Boost headers | 1.92.0 | Boost Software License 1.0 |
| clap-juce-extensions | 645ed2fd0949d36639e3d63333f26136df6df769 | MIT; vendored LICENSE.md |
| CLAP | df8f16c69ba1c1a15fb105f0c5a2e5b9ac6be742 | MIT; vendored LICENSE |
| clap-helpers | 7b53a685e11465154b4ccba3065224dbcbf8a893 | MIT; vendored LICENSE |
| Lua | 5.4.0, src/lua/src | MIT, notice in lua.h |
| Roboto Regular | data/fonts/Roboto-Regular.ttf | Apache-2.0 |
| Sample Pad acoustic sounds | Virtuosity Drums, Versilian Studios / Karoryfer Samples | CC0-1.0; data/sample-pad/README.md and manifest.json |
| Sample Pad electronic sounds | Alexander F. Rødseth / xyproto synth v1.4.4 audio pack | CC0-1.0 for audio; same manifest |

JUCE's embedded dependencies retain their own notices, including LV2/Lilv/Serd/
Sord/Sratom (ISC), VST3 SDK (MIT), FLAC/Ogg/Vorbis (BSD-style), JPEG (IJG), PNG/zlib,
HarfBuzz (MIT-style), SheenBidi and AudioUnitSDK (Apache-2.0). The complete JUCE
source tree and its LICENSE.md identify component-specific licences. Unused
framework examples/SDKs in the source archive do not imply that they are linked
into this Mac standalone application. No AAX, ASIO or VST2 binary is distributed.

External AmpliTube, TICK and x42 tuner plugins are not bundled. Their licences
are not permissions to redistribute their binaries. Optional `extras/tuna-compact`
is our GPL-3.0-or-later UI source for an independently installed GPL-2.0-or-later
x42 tuner; existing DSP is not copied into QLooper.

The DMG includes the licence texts/notices. Source releases include all vendored
notices; the separate dependency-source archive supplies JUCE, sol2 and Boost
headers used for the binary. No third-party endorsement is implied.
