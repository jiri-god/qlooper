# Build QLooper RC1 on macOS

Apple Command Line Tools/Xcode, CMake 3.26+ and Boost 1.74+ are required.
The release uses Apple Silicon (`arm64`), macOS deployment target 14.0 and
RelWithDebInfo. JUCE and sol2 revisions are pinned; CLAP dependencies are vendored.

```sh
brew install cmake boost
git clone https://github.com/jiri-god/qlooper.git
cd qlooper
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DELEMENT_BUILD_TESTS=ON -DELEMENT_BUILD_PLUGINS=OFF \
  -DELEMENT_ENABLE_UPDATER=OFF
cmake --build build --parallel 6
ctest --test-dir build --output-on-failure
open build/element_app_artefacts/RelWithDebInfo/QLooper.app
```

Configure downloads the pinned JUCE/sol2 sources. With the accompanying
`QLooper-1.0.0-rc1-dependency-sources.tar.gz`, unpack it beside this checkout and add:

```sh
-DFETCHCONTENT_SOURCE_DIR_JUCE=/absolute/path/dependencies/JUCE \
-DFETCHCONTENT_SOURCE_DIR_SOL2=/absolute/path/dependencies/sol2 \
-DBoost_INCLUDE_DIR=/absolute/path/dependencies
```

The dependency source archive also contains the exact Boost headers used in the
binary. Apple SDKs/compiler and CMake are build prerequisites, not bundled SDKs.
No proprietary plugin or VST2 SDK is needed. The future QLooper VST/AU is not
built by enabling Element's inherited plugin targets.

`tools/package-macos.sh` packages a built app into an ad-hoc signed test DMG.
A Developer ID/notarized release requires a separate signing pass; do not call
an ad-hoc signature a verified developer signature.
