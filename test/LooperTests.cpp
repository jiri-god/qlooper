// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include <boost/test/unit_test.hpp>
#include <limits>
#include <thread>
#include <chrono>
#include <element/juce/audio_formats.hpp>
#include "nodes/looper.hpp"
#include "nodes/countingate.hpp"
#include "nodes/looperresampler.hpp"
#include "nodes/looperkeyboard.hpp"
#include <element/node.h>
#include <element/context.hpp>
#include <element/plugins.hpp>
#include "engine/internalformat.hpp"
#include "testutil.hpp"

using namespace element;
using namespace juce;
using Action = LooperEngine::Action;

namespace {
void frame (LooperEngine& e, float l, float r) { e.process (l, r, 1); }
void record (LooperEngine& e)
{
    e.prepare (10); e.setTransitionOptions(0,0);
    e.apply (Action::record);
    frame (e, 1, 10);
    frame (e, 2, 20);
    frame (e, 3, 30);
    frame (e, 4, 40);
    e.apply (Action::play);
}
void cc (MidiBuffer& midi, int control, int sample, int value = 127)
{
    midi.addEvent (MidiMessage::controllerEvent (1, control, value), sample);
}
} // namespace

BOOST_AUTO_TEST_SUITE (LooperTests)

BOOST_AUTO_TEST_CASE (StereoMonitoringAndLoopBoundaries)
{
    LooperEngine e;
    record (e);
    for (int i = 0; i < 12; ++i) {
        float l = 0.25f, r = -0.5f;
        e.process (l, r, 0.5f);
        BOOST_CHECK_EQUAL (l, 0.25f + 0.5f * (i % 4 + 1));
        BOOST_CHECK_EQUAL (r, -0.5f + 5.0f * (i % 4 + 1));
    }
    e.apply (Action::stop);
    float l = 0.2f, r = 0.4f;
    e.process (l, r, 1);
    BOOST_CHECK_EQUAL (l, 0.2f);
    BOOST_CHECK_EQUAL (r, 0.4f);
}

BOOST_AUTO_TEST_CASE (ManagerMixAffectsPlaybackOnlyAndKeepsSamples)
{
    LooperEngine e;record(e);e.setManagerMix(0,1);
    for(int i=0;i<8;++i){float l=.25f,r=-.5f;e.process(l,r,1);BOOST_CHECK_EQUAL(l,.25f);BOOST_CHECK_EQUAL(r,-.5f);}
    BOOST_CHECK_EQUAL(e.sample(0,0),1);BOOST_CHECK_EQUAL(e.sample(1,0),10);
    e.setManagerMix(.5f,-1);
    for(int i=0;i<4;++i)frame(e,0,0); // allow the smoothed pan to cross from right to left
    float l=.25f,r=-.5f;e.process(l,r,1);
    BOOST_CHECK_EQUAL(r,-.5f);BOOST_CHECK_EQUAL(l,.25f+5.5f);
}
BOOST_AUTO_TEST_CASE (MomentaryBypassesWaitAndPreservesPositionPolicy)
{
    LooperEngine e;record(e);e.apply(Action::stop);e.setOptions(true,0,0);e.setClock(60,4,4,0,true);
    e.setPerformanceOptions(false,true,true,false,false,1,.1f);
    e.applyMomentary(true);BOOST_CHECK(e.getState()==LooperEngine::State::playing);BOOST_CHECK_EQUAL(e.getWaitingAction(),0);
    e.applyMomentary(false);BOOST_CHECK(e.getState()==LooperEngine::State::playing);BOOST_CHECK(e.momentaryMuted());BOOST_CHECK_EQUAL(e.getWaitingAction(),0);
    e.apply(Action::stop);e.applyPerformance(Action::play);BOOST_CHECK(e.getWaitingAction()!=0); // original timing remains
}

BOOST_AUTO_TEST_CASE (MomentaryGatesAudioButAdvancesPhaseAndKeepsLiveInput)
{
    LooperEngine e;record(e);e.setTransitionOptions(0,0);e.apply(Action::stop);
    e.applyMomentary(true);float l=0,r=0;e.process(l,r,1,false);
    BOOST_CHECK_GT(l,0);const int before=e.getPosition();
    e.applyMomentary(false);l=.25f;r=.5f;e.process(l,r,1,true);
    BOOST_CHECK_CLOSE(l,.25f,.001);BOOST_CHECK_CLOSE(r,.5f,.001);
    BOOST_CHECK_EQUAL(e.getPosition(),(before+1)%e.getLength());
    BOOST_CHECK(e.getState()==LooperEngine::State::playing);
    const int reopen=e.getPosition();e.applyMomentary(true);
    BOOST_CHECK_EQUAL(e.getPosition(),reopen);l=0;r=0;e.process(l,r,1,false);BOOST_CHECK_GT(l,0);
    e.applyMomentary(false);e.apply(Action::stop);frame(e,0,0);
    BOOST_CHECK(e.getState()==LooperEngine::State::stopped);
    e.apply(Action::play);BOOST_CHECK(!e.momentaryMuted());
}

BOOST_AUTO_TEST_CASE (PlayToEndUsesMasterBoundaryAndNeedsNewPress)
{
    LooperEngine e;record(e);e.setTransitionOptions(0,0);e.setMixOptions(0,1,0,0);
    e.setLinkContext(0,2,1,0);e.setPlayToEnd(true,2,0); // independent 4-frame loop, master 2 frames
    float l=0,r=0;e.process(l,r,1,false);BOOST_CHECK_GT(l,0);BOOST_CHECK(!e.momentaryMuted());
    l=r=0;e.process(l,r,1,false);BOOST_CHECK(e.momentaryMuted());BOOST_CHECK_EQUAL(l,0);
    BOOST_CHECK(e.getState()==LooperEngine::State::playing);
    for(int i=0;i<5;++i){l=r=0;e.process(l,r,1,false);BOOST_CHECK_EQUAL(l,0);}
    const auto phase=e.getPosition();e.applyPerformance(Action::playStop);
    BOOST_CHECK(!e.momentaryMuted());BOOST_CHECK_EQUAL(e.getPosition(),phase);
    l=r=0;e.process(l,r,1,false);BOOST_CHECK_GT(l,0);
    for(int i=0;i<2;++i){l=r=0;e.process(l,r,1,false);}BOOST_CHECK(e.momentaryMuted());
    // Holding opens once; no repeated opening at subsequent boundaries.
    e.applyMomentary(true);for(int i=0;i<5;++i){l=r=0;e.process(l,r,1,false);}BOOST_CHECK(e.momentaryMuted());
    e.applyMomentary(false);e.applyMomentary(true);BOOST_CHECK(!e.momentaryMuted());
    e.setPlayToEnd(false,2,0);for(int i=0;i<5;++i){l=r=0;e.process(l,r,1,false);}BOOST_CHECK(!e.momentaryMuted());
}
BOOST_AUTO_TEST_CASE (PlayToEndWithoutMasterUsesOwnLoop)
{
    LooperEngine e;record(e);e.setMixOptions(0,1,0,0);e.setPlayToEnd(true,0,0);
    for(int i=0;i<4;++i)frame(e,0,0);
    BOOST_CHECK(!e.momentaryMuted());frame(e,0,0);BOOST_CHECK(e.momentaryMuted());
    BOOST_CHECK(e.getState()==LooperEngine::State::playing);
}

BOOST_AUTO_TEST_CASE (PartialOverdubsUndoAndRepeatedLayers)
{
    LooperEngine e;
    record (e);
    e.apply (Action::overdub);
    frame (e, 10, 100);
    frame (e, 10, 100);
    e.apply (Action::play);
    BOOST_CHECK_EQUAL (e.sample (0, 0), 11);
    BOOST_CHECK_EQUAL (e.sample (0, 2), 3);
    e.apply (Action::undo);
    BOOST_CHECK_EQUAL (e.sample (0, 0), 1);
    e.apply (Action::overdub);
    frame (e, 20, 200);
    frame (e, 20, 200);
    e.apply (Action::undo);
    for (int i = 0; i < 4; ++i)
        BOOST_CHECK_EQUAL (e.sample (0, i), i + 1);
    e.apply (Action::overdub);
    for (int i = 0; i < 8; ++i)
        frame (e, 1, 2);
    e.apply (Action::play);
    e.apply (Action::overdub);
    frame (e, 5, 7);
    e.apply (Action::undo);
    for (int i = 0; i < 4; ++i) {
        BOOST_CHECK_EQUAL (e.sample (0, i), i + 3);
        BOOST_CHECK_EQUAL (e.sample (1, i), 10 * (i + 1) + 4);
    }
    e.apply (Action::undo);
    BOOST_CHECK_EQUAL (e.sample (0, 0), 3);
}

BOOST_AUTO_TEST_CASE (CapacityClearAndReplace)
{
    LooperEngine e;
    e.prepare (1); e.setTransitionOptions(0,0);
    e.apply (Action::record);
    for (int i = 0; i < 65; ++i)
        frame (e, static_cast<float> (i), -static_cast<float> (i));
    BOOST_CHECK_EQUAL (e.getLength(), 60);
    BOOST_CHECK (e.getState() == LooperEngine::State::playing);
    BOOST_CHECK_EQUAL (e.sample (0, 59), 59);
    e.apply (Action::clear);
    BOOST_CHECK_EQUAL (e.getLength(), 0);
    e.apply (Action::record);
    frame (e, 42, 24);
    e.apply (Action::stop);
    BOOST_CHECK_EQUAL (e.sample (0, 0), 42);
    BOOST_CHECK_EQUAL (e.sample (1, 0), 24);
}

BOOST_AUTO_TEST_CASE (MidiSampleOffsetsAndReleaseEdges)
{
    LooperProcessor p;
    p.prepareToPlay (10, 16); p.setCrossfade(0); p.setCaptureTail(0);
    AudioBuffer<float> b (2, 12);
    b.clear();
    for (int i = 0; i < 12; ++i) {
        b.setSample (0, i, static_cast<float> (i));
        b.setSample (1, i, static_cast<float> (-i));
    }
    MidiBuffer m;
    cc (m, 20, 2);
    cc (m, 20, 2); // duplicate high is ignored
    cc (m, 20, 2, 0);
    cc (m, 26, 6);
    p.processBlock (b, m);
    BOOST_CHECK_EQUAL (b.getSample (0, 6), 8); // dry 6 + first recorded sample 2
    BOOST_CHECK_EQUAL (b.getSample (1, 9), -14);
    BOOST_CHECK_EQUAL (b.getSample (0, 10), 12); // wraps after four samples
    BOOST_CHECK (m.isEmpty());
    cc (m, 22, 0);
    b.clear();
    p.processBlock (b, m);
    BOOST_CHECK_EQUAL (b.getMagnitude (0, 12), 0);
}

BOOST_AUTO_TEST_CASE (StateRoundtripIncludesActiveRecordingAndNoDuplicateProgramAudio)
{
    LooperProcessor p;
    p.prepareToPlay (10, 8); p.setCrossfade(0); p.setCaptureTail(0);
    AudioBuffer<float> b (2, 4);
    for (int i = 0; i < 4; ++i) {
        b.setSample (0, i, i + 1.0f);
        b.setSample (1, i, i + 11.0f);
    }
    MidiBuffer m;
    p.request(Action::record);
    p.processBlock (b, m);
    MemoryBlock saved;
    p.getStateInformation (saved);
    BOOST_CHECK_EQUAL (saved.getSize(), 296 + 4 * 8);
    MemoryBlock program;
    p.getCurrentProgramStateInformation (program);
    BOOST_CHECK_EQUAL (program.getSize(), 0);
    LooperProcessor restored;
    restored.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    restored.prepareToPlay (10, 8); restored.setCrossfade(0); restored.setCaptureTail(0);
    b.clear();
    restored.processBlock (b, m);
    BOOST_CHECK_EQUAL (b.getMagnitude (0, 4), 0);
    restored.request (Action::play);
    restored.processBlock (b, m);
    for (int i = 0; i < 4; ++i) {
        BOOST_CHECK_EQUAL (b.getSample (0, i), i + 1);
        BOOST_CHECK_EQUAL (b.getSample (1, i), i + 11);
    }
    restored.prepareToPlay (10, 16); restored.setCrossfade(0); restored.setCaptureTail(0); // graph reprepare must preserve loop
    b.clear();
    restored.processBlock (b, m);
    BOOST_CHECK_EQUAL (b.getSample (0, 0), 1);
    restored.prepareToPlay (20, 16); restored.setCrossfade(0); restored.setCaptureTail(0); // duration preserved; safely stopped
    MemoryBlock resampled;
    restored.getStateInformation (resampled);
    BOOST_CHECK_EQUAL (resampled.getSize(), 296 + 8 * 8);
    b.clear();
    restored.processBlock (b, m);
    BOOST_CHECK_EQUAL (b.getMagnitude (0, 4), 0);
}

BOOST_AUTO_TEST_CASE (LiveRestoreResamplesToPreparedDeviceRate)
{
    LooperProcessor source;
    source.prepareToPlay (44100, 16); source.setCrossfade(0); source.setCaptureTail(0);
    AudioBuffer<float> b (2, 441);
    b.clear();
    b.setSample (0, 0, 1);
    b.setSample (1, 0, 2);
    MidiBuffer m;
    source.request(Action::record);
    source.processBlock (b, m);
    MemoryBlock saved;
    source.getStateInformation (saved);
    LooperProcessor target;
    target.prepareToPlay (48000, 16); target.setCrossfade(0); target.setCaptureTail(0);
    target.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    MemoryBlock result;
    target.getStateInformation (result);
    MemoryInputStream in (result, false);
    in.skipNextBytes (8);
    BOOST_CHECK_EQUAL (in.readDouble(), 48000);
    in.skipNextBytes (4);
    BOOST_CHECK_EQUAL (in.readInt(), 480);
    target.request (Action::play);
    b.clear();
    target.processBlock (b, m);
    // Playback starts with the intentional 2ms de-click; raw converted audio is preserved.
    BOOST_CHECK_EQUAL (b.getSample (0, 0), 0);
    BOOST_CHECK_EQUAL (b.getSample (1, 0), 0);
    MemoryInputStream raw (result, false);
    raw.setPosition (88);
    BOOST_CHECK_CLOSE (raw.readFloat(), 1.0f, 0.001f);
    raw.setPosition (88 + 480 * 4);
    BOOST_CHECK_CLOSE (raw.readFloat(), 2.0f, 0.001f);
}

BOOST_AUTO_TEST_CASE (SnapshotAfterUndoAndMalformedRestore)
{
    LooperProcessor p;
    p.prepareToPlay (10, 4); p.setCrossfade(0); p.setCaptureTail(0);
    AudioBuffer<float> b (2, 4);
    MidiBuffer m;
    for (int i = 0; i < 4; ++i) {
        b.setSample (0, i, 1);
        b.setSample (1, i, 2);
    }
    p.request (Action::record);
    p.processBlock (b, m);
    p.request (Action::play);
    p.request (Action::overdub);
    p.processBlock (b, m);
    p.request (Action::undo);
    b.clear();
    p.processBlock (b, m);
    MemoryBlock saved;
    p.getStateInformation (saved);
    p.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()) - 1);
    MemoryBlock unchanged;
    p.getStateInformation (unchanged);
    BOOST_CHECK (saved == unchanged);
    LooperProcessor q;
    q.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    q.prepareToPlay (10, 4); q.setCrossfade(0); q.setCaptureTail(0);
    q.request (Action::play);
    b.clear();
    q.processBlock (b, m);
    for (int i = 0; i < 4; ++i) {
        BOOST_CHECK_EQUAL (b.getSample (0, i), 1);
        BOOST_CHECK_EQUAL (b.getSample (1, i), 2);
    }
}

BOOST_AUTO_TEST_CASE (BoundedCommandQueueAndEmptyCommands)
{
    LooperProcessor p;
    p.prepareToPlay (10, 8); p.setCrossfade(0); p.setCaptureTail(0);
    for (int i = 0; i < 63; ++i)
        BOOST_CHECK (p.request (Action::clear));
    BOOST_CHECK (! p.request (Action::record));
    AudioBuffer<float> b (2, 8);
    b.clear();
    MidiBuffer m;
    p.processBlock (b, m);
    BOOST_CHECK (p.request (Action::record));
    p.request (Action::play);
    p.request (Action::stop);
    p.request (Action::overdub);
    p.request (Action::undo);
    p.processBlock (b, m);
    BOOST_CHECK_EQUAL (b.getMagnitude (0, 8), 0);
}

BOOST_AUTO_TEST_CASE (RecordToggleRestoresAllPreviousLayersWithUndo)
{
    LooperEngine e;
    record (e);
    e.apply (Action::overdub);
    frame (e, 5, 7);
    e.apply (Action::overdub);
    const auto previous = e.snapshot();
    e.editLayer (e.getLayer (1).id, 2, 0.5f);
    e.apply (Action::record);
    frame (e, 99, 98);
    e.apply (Action::record);
    BOOST_CHECK_EQUAL (e.getLayerCount(), 3);
    BOOST_CHECK_EQUAL (e.getLength(), 4);
    BOOST_CHECK(!e.getLayer(0).enabled); BOOST_CHECK(!e.getLayer(1).enabled);
    e.apply (Action::undo);
    BOOST_CHECK_EQUAL (e.getLayerCount(), 2);
    BOOST_CHECK_EQUAL (e.getLength(), 4);
    BOOST_CHECK_EQUAL (e.getLayer (1).gain, 0.5f);
    BOOST_CHECK_EQUAL (e.layerSample (1, 0, 0), previous.layers[1].audio[0][0]);
    BOOST_CHECK_EQUAL (e.sample (0, 0), 3.5f);
}

BOOST_AUTO_TEST_CASE (OverdubEmptyStoppedAutomaticAndUnlimitedPasses)
{
    LooperEngine e;
    e.configureLimits(10,8,false);e.prepare (10); e.setTransitionOptions(0,0);
    e.apply (Action::overdub);
    frame (e, 1, 2);
    frame (e, 3, 4);
    e.apply (Action::overdub);
    BOOST_CHECK (e.getState() == LooperEngine::State::playing);
    e.apply (Action::stop);
    e.setOptions (false, 0, 2);
    e.apply (Action::overdub);
    for (int i = 0; i < 4; ++i)
        frame (e, 2, 3);
    BOOST_CHECK (e.getState() == LooperEngine::State::playing);
    BOOST_CHECK_EQUAL (e.getLayerCount(), 3);
    BOOST_CHECK_EQUAL (e.layerSample (1, 0, 0), 2);
    BOOST_CHECK_EQUAL (e.layerSample (2, 0, 0), 2);
    e.setOptions (false, 0, 0);
    e.apply (Action::overdub);
    for (int i = 0; i < 10; ++i)
        frame (e, 1, 1);
    BOOST_CHECK (e.getState() == LooperEngine::State::overdubbing);
    BOOST_CHECK_EQUAL (e.getLayerCount(), 8);
    BOOST_CHECK_EQUAL (e.layerSample (7, 0, 0), 1);
    e.apply (Action::stop);
    BOOST_CHECK (e.getState() == LooperEngine::State::stopped);
    BOOST_CHECK_EQUAL (e.layerSample (7, 0, 0), 1);
}

BOOST_AUTO_TEST_CASE (LayerEditsMonitorMuteAndPoolCapacity)
{
    LooperEngine e;e.configureLimits(8,8,false);
    record (e);
    e.apply (Action::overdub);
    for (int i = 0; i < 4; ++i)
        frame (e, 10, 20);
    e.apply (Action::overdub);
    auto id = e.getLayer (1).id;
    e.editLayer (id, 2, 0.5f);
    BOOST_CHECK_EQUAL (e.sample (0, 0), 6);
    e.editLayer (id, 1, 1);
    BOOST_CHECK_EQUAL (e.sample (0, 0), 5);
    e.editLayer (id, 0, 0);
    BOOST_CHECK_EQUAL (e.sample (0, 0), 1);
    float l = 7, r = 8;
    e.process (l, r, 1, false, true);
    BOOST_CHECK_EQUAL (l, 0);
    BOOST_CHECK_EQUAL (e.getPosition(), 1);
    l = 7;
    r = 8;
    e.process (l, r, 1, true, true);
    BOOST_CHECK_EQUAL (l, 7);
    e.editLayer (id, 3, 0);
    BOOST_CHECK_EQUAL (e.getLayerCount(), 1);
    for (int i = 1; i < 8; ++i) {
        e.apply (Action::overdub);
        frame (e, 1, 2);
        e.apply (Action::overdub);
    }
    BOOST_CHECK_EQUAL (e.getLayerCount(), 8);
    e.apply (Action::overdub);
    BOOST_CHECK (e.getState() == LooperEngine::State::playing);
    const auto saved = e.snapshot();
    e.apply (Action::freshRecord);
    frame (e, 90, 80);
    e.apply (Action::record);
    e.apply (Action::undo);
    BOOST_CHECK_EQUAL (e.getLayerCount(), 8);
    for (int i = 0; i < 8; ++i)
        for (int c = 0; c < 2; ++c)
            for (int f = 0; f < 4; ++f)
                BOOST_CHECK_EQUAL (e.layerSample (i, c, f), saved.layers[i].audio[c][f]);
}

BOOST_AUTO_TEST_CASE (QuantizationGraceNextBarHostPhaseAndFrozenTempo)
{
    LooperEngine e;
    e.prepare (100); e.setTransitionOptions(0,0);
    e.setOptions (true, 100, 0);
    e.setClock (120, 4, 4, 0, true);
    e.apply (Action::clear); e.apply (Action::record);
    for (int i = 0; i < 205; ++i)
        frame (e, 1, 2);
    e.apply (Action::record);
    BOOST_CHECK_EQUAL (e.getLength(), 200);
    BOOST_CHECK (e.getState() == LooperEngine::State::playing);
    e.setClock (120, 4, 4, 0, true);
    e.apply (Action::clear); e.apply (Action::record);
    for (int i = 0; i < 220; ++i)
        frame (e, 1, 2);
    e.setClock (240, 4, 4);
    e.apply (Action::record);
    BOOST_CHECK (e.isEndPending());
    for (int i = 220; i < 400; ++i)
        frame (e, 1, 2);
    BOOST_CHECK_EQUAL (e.getLength(), 400);
    BOOST_CHECK (e.getState() == LooperEngine::State::playing);
    e.setClock (120, 4, 4, 1.0, true);
    e.apply (Action::clear); e.apply (Action::record);
    for (int i = 0; i < 10; ++i)
        frame (e, 1, 2);
    e.apply (Action::record);
    for (int i = 10; i < 150; ++i)
        frame (e, 1, 2);
    BOOST_CHECK_EQUAL (e.getLength(), 200);
    BOOST_CHECK (e.getState() == LooperEngine::State::playing);
    e.setClock (std::numeric_limits<double>::quiet_NaN(), 4, 4);
    BOOST_CHECK (! e.hasBarClock());
}

BOOST_AUTO_TEST_CASE (V1CompatibilityAndV2SettingsLayerRoundtrip)
{
    MemoryBlock old;
    MemoryOutputStream out (old, false);
    out.writeInt (0x454c4c50);
    out.writeInt (1);
    out.writeDouble (10);
    out.writeFloat (0.5f);
    out.writeInt (2);
    out.writeFloat (1);
    out.writeFloat (2);
    out.writeFloat (10);
    out.writeFloat (20);
    out.flush();
    LooperProcessor p;
    p.setStateInformation (old.getData(), static_cast<int> (old.getSize()));
    p.prepareToPlay (10, 2); p.setCrossfade(0); p.setCaptureTail(0);
    p.view();
    p.setMonitor (false);
    p.setMuted (true);
    p.setQuantized (true);
    p.setGraceMs (75);
    p.setRecordNextBar (true);
    p.setAutoPasses (3);
    BOOST_CHECK (p.setMidiMapping (1, { 80, 2 }));
    BOOST_CHECK (! p.setMidiMapping (1, { 21, 0 }));
    p.editLayer (p.view().layers[0].id, 2, 0.25f);
    AudioBuffer<float> b (2, 2);
    b.clear();
    MidiBuffer m;
    p.processBlock (b, m);
    MemoryBlock saved;
    p.getStateInformation (saved);
    LooperProcessor q;
    q.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    const auto v = q.view();
    BOOST_REQUIRE_EQUAL (v.count, 1);
    BOOST_CHECK_EQUAL (v.layers[0].gain, 0.25f);
    BOOST_CHECK (! q.getMonitor());
    BOOST_CHECK (q.getMuted());
    BOOST_CHECK (q.getQuantized());
    BOOST_CHECK_EQUAL (q.getGraceMs(), 75);
    BOOST_CHECK (q.getRecordNextBar());
    BOOST_CHECK_EQUAL (q.getAutoPasses(), 3);
    BOOST_CHECK_EQUAL (q.getMidiMapping (1).cc, 80);
    q.setMuted (false);
    q.request (Action::play);
    q.processBlock (b, m);
    BOOST_CHECK_EQUAL (b.getSample (0, 0), 0.125f);
    BOOST_CHECK_EQUAL (b.getSample (1, 1), 2.5f);
}

BOOST_AUTO_TEST_CASE (ConfigurableMidiToggleChannelAndSharedExplicitStop)
{
    LooperProcessor p;
    p.prepareToPlay (10, 4); p.setCrossfade(0); p.setCaptureTail(0);
    p.setMonitor (false);
    BOOST_REQUIRE (p.setMidiMapping (1, { 80, 2 }));
    AudioBuffer<float> b (2, 4);
    b.clear();
    MidiBuffer m;
    m.addEvent (MidiMessage::controllerEvent (1, 80, 127), 0);
    p.processBlock (b, m);
    BOOST_CHECK_EQUAL (p.view().length, 0);
    b.clear();
    b.setSample (0, 1, 0.5f);
    m.addEvent (MidiMessage::controllerEvent (2, 80, 127), 1);
    m.addEvent (MidiMessage::controllerEvent (2, 80, 0), 1);
    m.addEvent (MidiMessage::controllerEvent (2, 80, 127), 3);
    m.addEvent (MidiMessage::controllerEvent (2, 80, 0), 3);
    p.processBlock (b, m);
    BOOST_CHECK_EQUAL (p.view().length, 2);
    BOOST_CHECK_EQUAL (b.getSample (0, 3), 0.5f);
    cc (m, 22, 0);
    b.clear();
    p.processBlock (b, m);
    BOOST_CHECK_EQUAL (p.view().state, static_cast<int> (LooperEngine::State::stopped));
    cc (m, 21, 0);
    cc (m, 21, 1, 0); // Play/Stop now resolves on short release.
    p.processBlock (b, m);
    BOOST_CHECK_EQUAL (p.view().state, static_cast<int> (LooperEngine::State::playing));
}

BOOST_AUTO_TEST_CASE (MultipleLayerStatePersistsPartialAudioAndMixSettings)
{
    LooperProcessor p;
    p.prepareToPlay (10, 4); p.setCrossfade(0); p.setCaptureTail(0);
    p.view();
    p.setMonitor (false);
    AudioBuffer<float> initial (2, 4), partial (2, 2), empty (2, 0);
    MidiBuffer midi;
    for (int i = 0; i < 4; ++i) {
        initial.setSample (0, i, 1);
        initial.setSample (1, i, 2);
    }
    p.request (Action::record);
    p.processBlock (initial, midi);
    p.request (Action::play);
    p.request (Action::overdub);
    for (int i = 0; i < 2; ++i) {
        partial.setSample (0, i, 4);
        partial.setSample (1, i, 6);
    }
    p.processBlock (partial, midi);
    p.request (Action::stop);
    p.processBlock (empty, midi);
    const auto layers = p.view();
    BOOST_REQUIRE_EQUAL (layers.count, 2);
    p.editLayer (layers.layers[0].id, 0, 0);
    p.editLayer (layers.layers[1].id, 1, 1);
    p.editLayer (layers.layers[1].id, 2, 0.5f);
    p.processBlock (empty, midi);
    MemoryBlock saved;
    p.getStateInformation (saved);
    LooperProcessor restored;
    restored.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    const auto loaded = restored.view();
    BOOST_REQUIRE_EQUAL (loaded.count, 2);
    BOOST_CHECK (! loaded.layers[0].enabled);
    BOOST_CHECK (loaded.layers[1].solo);
    BOOST_CHECK_EQUAL (loaded.layers[1].gain, 0.5f);
    restored.request (Action::play);
    initial.clear();
    restored.processBlock (initial, midi);
    BOOST_CHECK_EQUAL (initial.getSample (0, 0), 2);
    BOOST_CHECK_EQUAL (initial.getSample (1, 1), 3);
    BOOST_CHECK_EQUAL (initial.getSample (0, 2), 0);
    BOOST_CHECK_EQUAL (initial.getSample (1, 3), 0);
}

BOOST_AUTO_TEST_CASE (FloatWavExportPreservesLayerAboveUnity)
{
    LooperProcessor p;
    p.prepareToPlay (100, 4); p.setCrossfade(0); p.setCaptureTail(0);
    p.view();
    AudioBuffer<float> b (2, 4);
    MidiBuffer m;
    for (int i = 0; i < 4; ++i) {
        b.setSample (0, i, 2.5f);
        b.setSample (1, i, -3.0f);
    }
    p.request (Action::record);
    p.processBlock (b, m);
    p.request (Action::record);
    b.clear();
    p.processBlock (b, m);
    const auto data = p.view();
    BOOST_REQUIRE_EQUAL (data.count, 1);
    const auto file = File::getSpecialLocation (File::tempDirectory).getNonexistentChildFile ("element-looper-export", ".wav");
    BOOST_REQUIRE (p.exportLayer (data.layers[0].id, file).wasOk());
    WavAudioFormat format;
    std::unique_ptr<AudioFormatReader> reader (format.createReaderFor (file.createInputStream().release(), true));
    BOOST_REQUIRE (reader != nullptr);
    BOOST_CHECK (reader->usesFloatingPointData);
    BOOST_CHECK_EQUAL (reader->numChannels, 2);
    BOOST_CHECK_EQUAL (reader->lengthInSamples, 4);
    AudioBuffer<float> restored (2, 4);
    BOOST_REQUIRE (reader->read (&restored, 0, 4, 0, true, true));
    BOOST_CHECK_EQUAL (restored.getSample (0, 0), 2.5f);
    BOOST_CHECK_EQUAL (restored.getSample (1, 3), -3.0f);
    reader.reset();
    BOOST_CHECK (file.deleteFile());
}

BOOST_AUTO_TEST_CASE (OfflineEditsPersistWithoutRenderingAndDoNotQueueCapture)
{
    LooperProcessor p;p.prepareToPlay(100,8);p.setCrossfade(0);p.setCaptureTail(0);
    AudioBuffer<float> audio(2,8);audio.clear();MidiBuffer midi;
    p.request(Action::record);p.processBlock(audio,midi);p.view();
    p.request(Action::stop);p.processBlock(audio,midi);auto v=p.view();
    BOOST_REQUIRE_EQUAL(v.count,1);const auto id=v.layers[0].id;
    p.serviceOfflineControls();p.view();
    BOOST_CHECK(!p.audioAvailable.load());
    BOOST_REQUIRE(p.editLayer(id,2,.35f));BOOST_REQUIRE(p.editLayer(id,4,-.6f));
    BOOST_REQUIRE(p.editLayer(id,0,0));BOOST_REQUIRE(p.editLayer(id,6,1));
    p.serviceOfflineControls();v=p.view();
    BOOST_CHECK_CLOSE(v.layers[0].gain,.35f,.01f);BOOST_CHECK_CLOSE(v.layers[0].pan,-.6f,.01f);
    BOOST_CHECK(!v.layers[0].enabled);BOOST_CHECK(v.layers[0].ignoreFeedback);
    p.request(Action::record);p.serviceOfflineControls();p.view();
    BOOST_CHECK_EQUAL(p.captureRequested.load(),0);
    p.processBlock(audio,midi);v=p.view();
    BOOST_CHECK(p.audioAvailable.load());BOOST_CHECK_EQUAL(v.state,int(LooperEngine::State::stopped));
    BOOST_CHECK_EQUAL(v.count,1);
    p.serviceOfflineControls();p.view();p.request(Action::clear);p.serviceOfflineControls();
    BOOST_CHECK_EQUAL(p.view().count,0);
}
BOOST_AUTO_TEST_CASE (DeviceLossFinalizesCaptureWithoutWaitingForFutureTail)
{
    LooperProcessor p;p.prepareToPlay(1000,64);p.setCrossfade(10);p.setCaptureTail(200);
    AudioBuffer<float> audio(2,64);audio.clear();MidiBuffer midi;
    p.request(Action::record);p.processBlock(audio,midi);
    p.serviceOfflineControls();auto v=p.view();
    BOOST_REQUIRE_EQUAL(v.count,1);BOOST_CHECK(!v.finishingEdit);
    BOOST_CHECK_EQUAL(v.state,int(LooperEngine::State::stopped));
    BOOST_REQUIRE(p.setMemoryLimits(2,2,2,true));
    BOOST_REQUIRE(p.editLayer(v.layers[0].id,3,0));p.serviceOfflineControls();
    BOOST_CHECK_EQUAL(p.view().count,0);
    MemoryBlock state;p.getStateInformation(state);BOOST_CHECK_GT(state.getSize(),0u);
}
BOOST_AUTO_TEST_CASE (OfflineFinalizationAlsoReleasesRollingTransition)
{
    LooperEngine e;e.configureDuration(1);e.configureLimits(10,1,true);e.prepare(1000);e.setTransitionOptions(10,10);
    const auto frames=[&](int count){for(int i=0;i<count;++i){float l=.1f,r=.2f;e.process(l,r,1);}};
    e.apply(Action::record);frames(100);e.finalizeOffline();e.apply(Action::play);
    e.apply(Action::overdub);frames(201);e.finalizeOffline();
    BOOST_CHECK(!e.isClonePending());BOOST_CHECK(e.resizeMemory(2,10,1,true,1));
}
BOOST_AUTO_TEST_CASE (OfflinePublicationRecoversAfterTheEditorStopsPolling)
{
    LooperProcessor p;p.prepareToPlay(1000,8);AudioBuffer<float> audio(2,8);MidiBuffer midi;
    p.request(Action::record);
    for(int i=0;i<100;++i){audio.clear();p.processBlock(audio,midi);}
    p.serviceOfflineControls();p.view(); // Drain the full UI queue.
    p.serviceOfflineControls();const auto v=p.view();
    BOOST_CHECK_EQUAL(v.state,int(LooperEngine::State::stopped));BOOST_CHECK_EQUAL(v.length,800);
    BOOST_CHECK(!v.finishingEdit);
}
BOOST_AUTO_TEST_CASE (NonfiniteLayerEditsAreRejectedBeforeTheyReachAudio)
{
    LooperEngine e;record(e);const auto id=e.getLayer(0).id;
    for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}) {
        for(int field=0;field<=6;++field)e.editLayer(id,field,bad);
        BOOST_CHECK_EQUAL(e.getLayerCount(),1);BOOST_CHECK_EQUAL(e.getLayer(0).gain,1);
        BOOST_CHECK_EQUAL(e.getLayer(0).pan,0);BOOST_CHECK(e.getLayer(0).enabled);
        float l=0,r=0;e.process(l,r,1);BOOST_CHECK(std::isfinite(l)&&std::isfinite(r));
        LooperProcessor p;BOOST_CHECK(!p.editLayer(id,2,bad));
    }
}
BOOST_AUTO_TEST_CASE (DeviceRateChangeKeepsSettingsAndRejectsStaleLayerIDs)
{
    LooperProcessor p;p.prepareToPlay(1000,32);p.setMemoryLimits(2,2,2,true,2);
    AudioBuffer<float> audio(2,32);audio.clear();MidiBuffer midi;
    p.request(Action::record);p.processBlock(audio,midi);p.serviceOfflineControls();
    const auto before=p.view();BOOST_REQUIRE_EQUAL(before.count,1);
    p.prepareToPlay(2000,32);auto after=p.view();
    BOOST_CHECK_EQUAL(after.length,before.length*2);BOOST_CHECK_EQUAL(p.getUndoDepth(),2);
    BOOST_CHECK_EQUAL(p.getDurationSeconds(),2);BOOST_CHECK_EQUAL(p.getRecordingLimit(),2);
    BOOST_CHECK_NE(after.layers[0].id,before.layers[0].id);
    p.editLayer(before.layers[0].id,3,0);p.serviceOfflineControls();BOOST_CHECK_EQUAL(p.view().count,1);
}
BOOST_AUTO_TEST_CASE (EditKeyboardShortcutsLatchAndClearRequiresShift)
{
    LooperKeyboard k;
    BOOST_CHECK(*k.press('Z',true).action==Action::undo);
    BOOST_CHECK(!k.press('Z',true).action);
    BOOST_CHECK(*k.press('X',true).action==Action::redo);
    BOOST_CHECK(!k.press('C',true).action);
    BOOST_CHECK(!k.press('C',true,true).action); // Shift added while held is not a new press.
    k.releaseUpKeys({});
    BOOST_CHECK(*k.press('C',true,true).action==Action::clear);
    BOOST_CHECK(!k.press('C',true,true).action);
    k.releaseUpKeys({});BOOST_CHECK(!k.press('C',false,true).action);
}
BOOST_AUTO_TEST_CASE (KeyboardPedalsMapLatchAndRequireRelease)
{
    LooperKeyboard keyboard;
    auto first = keyboard.press ('A', true);
    BOOST_REQUIRE (first.action.has_value());
    BOOST_CHECK (*first.action == Action::overdub);
    for (int i = 0; i < 20; ++i) {
        const auto repeated = keyboard.press ('a', true);
        BOOST_CHECK (repeated.consumed);
        BOOST_CHECK (! repeated.action.has_value());
    }
    keyboard.releaseUpKeys ({ true, false, false });
    BOOST_CHECK (! keyboard.press ('A', true).action.has_value());
    keyboard.releaseUpKeys ({ false, false, false });
    BOOST_CHECK (keyboard.press ('a', true).action.has_value());
    BOOST_CHECK (*keyboard.press ('S', true).action == Action::record);
    BOOST_CHECK (*keyboard.press ('D', true).action == Action::playStop);
    BOOST_CHECK (! keyboard.press ('Q', true).consumed);
    LooperKeyboard refocused;
    refocused.suppressHeldKeys ({ true, false, true });
    BOOST_CHECK (! refocused.press ('A', true).action.has_value());
    BOOST_CHECK (! refocused.press ('D', true).action.has_value());
    refocused.releaseUpKeys ({ false, false, false });
    BOOST_CHECK (refocused.press ('D', true).action.has_value());
    LooperKeyboard blocked;
    BOOST_CHECK (! blocked.press ('S', false).consumed);
    BOOST_CHECK (! blocked.press ('S', true).action.has_value());
    blocked.releaseUpKeys ({ false, false, false });
    BOOST_CHECK (blocked.press ('S', true).action.has_value());
}

BOOST_AUTO_TEST_CASE (KeyboardPedalsRespectEditorFocusTextAndModifiers)
{
    Component editor, elsewhere;
    TextButton child;
    TextEditor text;
    Label numericLabel;
    editor.addAndMakeVisible (child);
    editor.addAndMakeVisible (text);
    editor.addAndMakeVisible (numericLabel);
    const KeyPress plain ('S');
    BOOST_CHECK (LooperKeyboard::allows (plain, &editor, editor));
    BOOST_CHECK (LooperKeyboard::allows (plain, &child, editor));
    BOOST_CHECK (! LooperKeyboard::allows (plain, nullptr, editor));
    BOOST_CHECK (! LooperKeyboard::allows (plain, &elsewhere, editor));
    BOOST_CHECK (! LooperKeyboard::allows (plain, &text, editor));
    TextEditor sliderText;
    numericLabel.addAndMakeVisible (sliderText);
    BOOST_CHECK (! LooperKeyboard::allows (plain, &sliderText, editor));
    for (auto modifier : { ModifierKeys::commandModifier, ModifierKeys::ctrlModifier, ModifierKeys::altModifier })
        BOOST_CHECK (! LooperKeyboard::allows (KeyPress ('S', ModifierKeys (modifier), 0), &child, editor));
    BOOST_CHECK (LooperKeyboard::allows (KeyPress ('A', ModifierKeys (ModifierKeys::shiftModifier), 0), &child, editor));
}

BOOST_AUTO_TEST_CASE (KeyboardPedalsUseProcessorTransportQueue)
{
    LooperProcessor processor;
    processor.prepareToPlay (10, 4); processor.setCrossfade(0); processor.setCaptureTail(0);
    processor.view();
    LooperKeyboard keyboard;
    const auto press = [&] (int key) { const auto action = keyboard.press (key, true); if (action.action) BOOST_REQUIRE (processor.request (*action.action)); };
    AudioBuffer<float> audio (2, 4);
    audio.clear();
    MidiBuffer midi;
    press ('S');
    processor.processBlock (audio, midi);
    BOOST_CHECK_EQUAL (processor.view().state, static_cast<int> (LooperEngine::State::recording));
    press ('S');
    processor.processBlock (audio, midi);
    BOOST_CHECK_EQUAL (processor.view().length, 8);
    keyboard.releaseUpKeys ({ false, false, false });
    press ('S');
    processor.processBlock (audio, midi);
    BOOST_CHECK_EQUAL (processor.view().state, static_cast<int> (LooperEngine::State::playing));
    press ('A');
    processor.processBlock (audio, midi);
    BOOST_CHECK_EQUAL (processor.view().state, static_cast<int> (LooperEngine::State::overdubbing));
    press ('D');
    processor.processBlock (audio, midi);
    BOOST_CHECK_EQUAL (processor.view().state, static_cast<int> (LooperEngine::State::stopped));
}

BOOST_AUTO_TEST_CASE (BypassPassesDryAdvancesAndResumesWithMonitorOff)
{
    LooperProcessor p;
    p.prepareToPlay (10, 4); p.setCrossfade(0); p.setCaptureTail(0);
    p.setMonitor (false);
    p.view();
    AudioBuffer<float> take (2, 4), block (2, 1);
    MidiBuffer midi;
    for (int i = 0; i < 4; ++i) {
        take.setSample (0, i, i + 1.0f);
        take.setSample (1, i, 10 * (i + 1.0f));
    }
    p.request (Action::record);
    p.processBlock (take, midi);
    p.request (Action::play);
    block.clear();
    p.processBlock (block, midi);
    BOOST_CHECK_EQUAL (block.getSample (0, 0), 1);
    p.view();
    p.setBypassed (true);
    block.setSample (0, 0, 7);
    block.setSample (1, 0, -9);
    p.processBlock (block, midi);
    BOOST_CHECK_EQUAL (block.getSample (0, 0), 7);
    BOOST_CHECK_EQUAL (block.getSample (1, 0), -9);
    BOOST_CHECK_EQUAL (p.view().position, 2);
    p.setBypassed (false);
    block.clear();
    p.processBlock (block, midi);
    BOOST_CHECK_EQUAL (block.getSample (0, 0), 3);
    BOOST_CHECK_EQUAL (block.getSample (1, 0), 30);
}

BOOST_AUTO_TEST_CASE (BypassImmediatelyFinalizesPendingRecordAndOverdub)
{
    struct Clock : AudioPlayHead {
        Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setIsPlaying (true);
            info.setBpm (120.0);
            info.setTimeSignature (AudioPlayHead::TimeSignature { 4, 4 });
            return info;
        }
    } clock;
    LooperProcessor quantized;
    quantized.prepareToPlay (100, 3); quantized.setCrossfade(0); quantized.setCaptureTail(0);
    quantized.setPlayHead (&clock);
    quantized.setQuantized (true);
    quantized.setGraceMs (0);
    quantized.view();
    AudioBuffer<float> quantizedAudio (2, 3), noAudio (2, 0);
    quantizedAudio.clear();
    MidiBuffer quantizedMidi;
    quantized.request (Action::record);
    quantized.processBlock (quantizedAudio, quantizedMidi);
    quantized.request (Action::record);
    quantized.processBlock (noAudio, quantizedMidi);
    BOOST_REQUIRE (quantized.view().pending);
    quantized.setBypassed (true);
    quantized.processBlock (noAudio, quantizedMidi);
    const auto finalized = quantized.view();
    BOOST_CHECK (! finalized.pending);
    BOOST_CHECK_EQUAL (finalized.length, 3);
    BOOST_CHECK_EQUAL (finalized.state, static_cast<int> (LooperEngine::State::playing));

    LooperEngine engine;
    engine.prepare (100); engine.setTransitionOptions(0,0);
    engine.setOptions (true, 0, 0);
    engine.setClock (120, 4, 4, 0, true);
    engine.apply (Action::record);
    for (int i = 0; i < 15; ++i)
        frame (engine, 1, 2);
    engine.apply (Action::record);
    BOOST_REQUIRE (engine.isEndPending());
    engine.finalizeCapture();
    BOOST_CHECK_EQUAL (engine.getLength(), 15);
    BOOST_CHECK (! engine.isEndPending());
    BOOST_CHECK (engine.getState() == LooperEngine::State::playing);

    LooperProcessor p;
    p.prepareToPlay (10, 2); p.setCrossfade(0); p.setCaptureTail(0);
    p.view();
    AudioBuffer<float> b (2, 2);
    MidiBuffer midi;
    for (int i = 0; i < 2; ++i) {
        b.setSample (0, i, 1);
        b.setSample (1, i, 2);
    }
    p.request (Action::record);
    p.processBlock (b, midi);
    p.setBypassed (true);
    b.clear();
    p.processBlock (b, midi);
    auto data = p.view();
    BOOST_CHECK_EQUAL (data.length, 2);
    BOOST_CHECK_EQUAL (data.state, static_cast<int> (LooperEngine::State::playing));
    p.setBypassed (false);
    p.request (Action::overdub);
    for (int i = 0; i < 2; ++i) {
        b.setSample (0, i, 3);
        b.setSample (1, i, 4);
    }
    p.processBlock (b, midi);
    p.setBypassed (true);
    b.clear();
    p.processBlock (b, midi);
    data = p.view();
    BOOST_CHECK_EQUAL (data.count, 2);
    BOOST_CHECK_EQUAL (data.state, static_cast<int> (LooperEngine::State::playing));
    p.setBypassed (false);
    p.setMonitor (false);
    b.clear();
    p.processBlock (b, midi);
    BOOST_CHECK_EQUAL (b.getSample (0, 0), 4);
    BOOST_CHECK_EQUAL (b.getSample (1, 0), 6);
}

BOOST_AUTO_TEST_CASE (BypassBlocksCaptureControlsButAllowsTransport)
{
    LooperProcessor p;
    p.prepareToPlay (10, 2); p.setCrossfade(0); p.setCaptureTail(0);
    p.view();
    AudioBuffer<float> b (2, 2);
    b.clear();
    MidiBuffer midi;
    p.request (Action::record);
    p.processBlock (b, midi);
    p.setBypassed (true);
    p.request (Action::record);
    p.request (Action::overdub);
    cc (midi, 20, 0);
    cc (midi, 23, 1);
    p.processBlock (b, midi);
    auto data = p.view();
    BOOST_CHECK_EQUAL (data.length, 2);
    BOOST_CHECK_EQUAL (data.count, 1);
    BOOST_CHECK_EQUAL (data.state, static_cast<int> (LooperEngine::State::playing));
    p.request (Action::stop);
    p.processBlock (b, midi);
    BOOST_CHECK_EQUAL (p.view().state, static_cast<int> (LooperEngine::State::stopped));
    p.request (Action::play);
    p.processBlock (b, midi);
    BOOST_CHECK_EQUAL (p.view().state, static_cast<int> (LooperEngine::State::playing));
    p.request (Action::clear);
    p.request (Action::overdub);
    p.request (Action::record);
    p.processBlock (b, midi);
    data = p.view();
    BOOST_CHECK_EQUAL (data.length, 0);
    BOOST_CHECK_EQUAL (data.state, static_cast<int> (LooperEngine::State::empty));
}

BOOST_AUTO_TEST_CASE (BypassPersistsAndVersionTwoRestoresNotBypassed)
{
    LooperProcessor p;
    p.prepareToPlay (10, 2); p.setCrossfade(0); p.setCaptureTail(0);
    p.setBypassed (true);
    p.setMonitor (false);
    MemoryBlock saved;
    p.getStateInformation (saved);
    LooperProcessor restored;
    restored.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    BOOST_CHECK (restored.getBypassed());
    BOOST_CHECK (! restored.getMonitor());
    AudioBuffer<float> b (2, 2);
    b.clear();
    b.setSample (0, 0, 0.5f);
    MidiBuffer midi;
    restored.processBlock (b, midi);
    BOOST_CHECK_EQUAL (b.getSample (0, 0), 0.5f);
    MemoryBlock v2 (saved);
    v2.removeSection(v2.getSize()-160,160); // Empty state: v5 structure + v6 MIDI + v7 mix + v8 transitions
    v2.removeSection (48, 8); // remove v3 bypass and v4 start mode
    auto* bytes = static_cast<unsigned char*> (v2.getData());
    bytes[4] = 2;
    bytes[5] = bytes[6] = bytes[7] = 0;
    restored.setStateInformation (v2.getData(), static_cast<int> (v2.getSize()));
    BOOST_CHECK (! restored.getBypassed());
    BOOST_CHECK (! restored.getMonitor());
    b.setSample (0, 0, 0.5f);
    restored.processBlock (b, midi);
    BOOST_CHECK_EQUAL (b.getSample (0, 0), 0);
}

BOOST_AUTO_TEST_CASE (InternalRegistration)
{
    ElementAudioPluginFormat format (*element::test::context());
    BOOST_CHECK (format.searchPathsForPlugins ({}, false, false).contains (EL_NODE_ID_LOOPER));
    OwnedArray<PluginDescription> descriptions;
    format.findAllTypesForFile (descriptions, EL_NODE_ID_LOOPER);
    BOOST_REQUIRE_EQUAL (descriptions.size(), 1);
    BOOST_CHECK_EQUAL (descriptions[0]->numInputChannels, 2);
    BOOST_CHECK_EQUAL (descriptions[0]->pluginFormatName, "Element");
}

BOOST_AUTO_TEST_CASE (RetrospectiveStartKeepsAudioLengthAndPlaybackPhase)
{
    LooperEngine e;
    e.prepare (100); e.setTransitionOptions(0,0);
    e.setOptions (true, 100, 0);
    e.setClock (120, 4, 4, 0, true);
    for (int i = 0; i < 25; ++i) frame (e, float(i), -float(i));
    e.apply (Action::record);
    BOOST_CHECK_EQUAL (e.getLength(), 25);
    for (int i = 25; i < 205; ++i) frame (e, float(i), -float(i));
    e.apply (Action::overdub);
    BOOST_REQUIRE_EQUAL (e.getLength(), 200);
    BOOST_CHECK_EQUAL (e.getPosition(), 5);
    for (int i = 0; i < 200; ++i) {
        BOOST_CHECK_EQUAL (e.layerSample (0, 0, i), float(i));
        BOOST_CHECK_EQUAL (e.layerSample (0, 1, i), -float(i));
    }
    for (int i = 0; i < 2000; ++i) {
        float l = 0, r = 0;
        e.process (l, r, 1, false);
        BOOST_CHECK_EQUAL (l, float((i + 5) % 200));
    }
    BOOST_CHECK_EQUAL (e.getPosition(), 5);
}

BOOST_AUTO_TEST_CASE (ArmedReplacementCancellationAndEmptyOverdub)
{
    LooperEngine e;
    record (e);
    e.setClock (120, 4, 4, 1, true); // 5 of 20 frames into bar
    e.setOptions (true, 100, 0);
    e.setRecordNextBar (true);
    e.apply (Action::record);
    BOOST_CHECK (e.isStartPending());
    BOOST_CHECK_EQUAL (e.getLength(), 4);
    e.apply (Action::record);
    BOOST_CHECK (!e.isStartPending());
    BOOST_CHECK_EQUAL (e.getLength(), 4);
    e.apply (Action::record);
    for (int i = 0; i < 15; ++i) frame (e, 9, 9);
    BOOST_CHECK (e.isStartPending());
    frame (e, 7, 8);
    BOOST_CHECK (!e.isStartPending());
    BOOST_CHECK_EQUAL (e.getLength(), 4);
    BOOST_CHECK_EQUAL(e.getLayerCount(),2);
    BOOST_CHECK_EQUAL (e.layerSample (1, 0, 3), 7);
    e.apply (Action::undo);
    BOOST_CHECK_EQUAL (e.getLength(), 4);
    e.apply (Action::clear);
    e.apply (Action::overdub);
    BOOST_CHECK (e.isStartPending());
    BOOST_CHECK (e.getState() == LooperEngine::State::empty);
    BOOST_CHECK_EQUAL (e.getLength(), 0);
    e.setOptions (false, 100, 0);
    e.setClock (120, 4, 4, 1, false);
    e.apply (Action::clear);
    e.apply (Action::record);
    BOOST_CHECK (!e.isStartPending());
    BOOST_CHECK_EQUAL (e.getLength(), 0);
}

BOOST_AUTO_TEST_CASE (SyncCancellationRestoresOldLoopAndFreeIgnoresClock)
{
    LooperEngine e;
    record (e);
    e.setOptions (true, 100, 0);
    e.setClock (120, 4, 4, 0, true);
    e.apply (Action::record);
    frame (e, 9, 9); // 100 ms, within same-boundary tolerance
    e.apply (Action::record);
    BOOST_CHECK_EQUAL (e.getLength(), 4);
    BOOST_CHECK_EQUAL (e.layerSample (0, 0, 0), 1);
    e.apply (Action::clear);
    e.apply (Action::overdub);
    e.apply (Action::overdub);
    BOOST_CHECK_EQUAL (e.getLength(), 0);
    BOOST_CHECK_EQUAL (e.getLayerCount(), 0);
    e.setOptions (false, 100, 0);
    e.setRecordNextBar (true);
    e.setClock (120, 4, 4, 2, true);
    e.apply (Action::record);
    BOOST_CHECK (!e.isStartPending());
    BOOST_CHECK_EQUAL (e.getLength(), 0);
    frame (e, 3, 4);
    e.apply (Action::record);
    BOOST_CHECK_EQUAL (e.getLength(), 1);
}

BOOST_AUTO_TEST_CASE (SyncEarlyEndFrozenModeRepeatedPressAndImmediateOverdub)
{
    LooperEngine e;
    e.prepare (100); e.setTransitionOptions(0,0);
    e.setOptions (true, 100, 0);
    e.setClock (120, 4, 4, 0, false);
    e.apply (Action::record);
    BOOST_CHECK (e.needsClock());
    BOOST_CHECK_EQUAL (e.getLayerCount(), 0);
    e.apply (Action::overdub);
    BOOST_CHECK_EQUAL (e.getLayerCount(), 0);
    e.setClock (120, 4, 4, 0, true);
    e.apply (Action::record);
    for (int i = 0; i < 150; ++i) frame (e, 1, 2);
    e.setOptions (false, 0, 0); // only affects the next new take
    e.apply (Action::record);
    BOOST_REQUIRE (e.isEndPending());
    for (int i = 150; i < 195; ++i) frame (e, 0, 0);
    e.apply (Action::record);
    e.apply (Action::overdub);
    BOOST_CHECK (e.isEndPending());
    for (int i = 195; i < 200; ++i) frame (e, 0, 0);
    BOOST_CHECK_EQUAL (e.getLength(), 200);
    BOOST_CHECK (e.getState() == LooperEngine::State::playing);
    e.setOptions (true, 100, 0);
    e.apply (Action::overdub);
    frame (e, 7, 8);
    e.apply (Action::overdub);
    BOOST_CHECK_EQUAL (e.getLayerCount(), 2);
    BOOST_CHECK_EQUAL (e.getLength(), 200);
    BOOST_CHECK (!e.isEndPending());
    e.apply (Action::undo);
    BOOST_CHECK_EQUAL (e.getLayerCount(), 1);
}

BOOST_AUTO_TEST_CASE (SyncHistoryTransferSurvivesWrapReplacementAndUndo)
{
    LooperEngine e;
    e.prepare (10); e.setTransitionOptions(0,0);
    e.setOptions (true, 100, 0);
    e.setClock (60, 4, 4, 0, true);
    for (int i = 0; i < 625; ++i) frame (e, static_cast<float> (i), -static_cast<float> (i));
    // Ring has wrapped; beat phase is 25 samples into a 40-sample bar.
    e.apply (Action::record);
    BOOST_CHECK_EQUAL (e.getLength(), 25);
    for (int i = 625; i < 640; ++i) frame (e, static_cast<float> (i), -static_cast<float> (i));
    e.apply (Action::overdub);
    const auto first = e.snapshot();
    BOOST_REQUIRE_EQUAL (first.length, 40);
    for (int i = 0; i < 40; ++i) BOOST_CHECK_EQUAL (first.layers[0].audio[0][i], 600 + i);
    for (int i = 0; i < 7; ++i) frame (e, 42, 43);
    e.apply (Action::record);
    for (int i = 0; i < 34; ++i) frame (e, 42, 43); // include first sample after armed boundary
    e.apply (Action::record);
    e.apply (Action::undo);
    const auto restored = e.snapshot();
    BOOST_REQUIRE_EQUAL(restored.layers.size(),1);
    BOOST_CHECK (restored.layers[0].audio == first.layers[0].audio);
}

BOOST_AUTO_TEST_CASE (SyncCapacityEndsAtWholeBarRatherThanSixtySeconds)
{
    LooperEngine e;
    e.prepare (100); e.setTransitionOptions(0,0);
    e.setOptions (true, 100, 0);
    e.setClock (86, 4, 4, 0, true);
    e.apply (Action::overdub);
    for (int i = 0; i < 6000; ++i) frame (e, 1, 1);
    const int bar = static_cast<int> (std::llround (100.0 * 60 / 86 * 4));
    BOOST_CHECK_EQUAL (e.getLength(), 6000 / bar * bar);
    BOOST_CHECK (e.getState() == LooperEngine::State::playing);
    BOOST_CHECK_EQUAL (e.getLength() % bar, 0);
}

BOOST_AUTO_TEST_CASE (PlaybackTransitionsAreSmoothedWithoutChangingRawSamples)
{
    LooperEngine e;
    e.prepare (1000); e.setTransitionOptions(0,0);
    e.load (std::array<std::vector<float>, 2> { std::vector<float> (100, 1), std::vector<float> (100, -1) });
    e.apply (Action::play);
    float l = 0, r = 0;
    e.process (l, r, 1, false);
    BOOST_CHECK_EQUAL (l, 0); // Start ramp, no abrupt full-scale edge.
    for (int i = 0; i < 4; ++i) frame (e, 0, 0);
    e.apply (Action::stop);
    l = r = 0;
    e.process (l, r, 1, false);
    BOOST_CHECK_EQUAL (l, 1);
    l = r = 0;
    e.process (l, r, 1, false);
    BOOST_CHECK_CLOSE (l, 0.5f, 0.001f);
    l = r = 0;
    e.process (l, r, 1, false);
    BOOST_CHECK_EQUAL (l, 0);
    const auto raw = e.snapshot();
    BOOST_CHECK_EQUAL (raw.length, 100);
    for (const auto sample : raw.layers[0].audio[0]) BOOST_CHECK_EQUAL (sample, 1);
    e.apply (Action::play);
    for (int i = 0; i < 4; ++i) frame (e, 0, 0);
    e.editLayer (e.getLayer (0).id, 2, 0);
    for(int i=1;i<=10;++i) {
        l=r=0;e.process(l,r,1,false);
        BOOST_CHECK_SMALL(l-float(10-i)/10,0.00001f);
    }
}

BOOST_AUTO_TEST_CASE (LoopSeamDeclickKeepsExactLengthAndRecordedWaveform)
{
    LooperEngine e;
    e.prepare (1000); e.setTransitionOptions(0,0);
    std::array<std::vector<float>, 2> wave { std::vector<float> (100, -1), std::vector<float> (100, -1) };
    for (auto& channel : wave) std::fill_n (channel.begin(), 50, 1);
    e.load (wave);
    e.apply (Action::play);
    for (int i = 0; i < 100; ++i) frame (e, 0, 0);
    BOOST_CHECK_EQUAL (e.getPosition(), 0);
    float l = 0, r = 0;
    e.process (l, r, 1, false);
    BOOST_CHECK_EQUAL (l, -1); // Seam starts at preceding output, not +1.
    l = r = 0;
    e.process (l, r, 1, false);
    BOOST_CHECK_SMALL (l, 0.00001f);
    l = r = 0;
    e.process (l, r, 1, false);
    BOOST_CHECK_EQUAL (l, 1);
    BOOST_CHECK_EQUAL (e.getLength(), 100);
    BOOST_CHECK (e.snapshot().layers[0].audio == wave);
}

BOOST_AUTO_TEST_CASE (OfflineResamplerRejectsAliasesAndPreservesPeriodicPassband)
{
    LooperEngine::Snapshot snapshot;
    snapshot.rate = 192000;
    snapshot.length = 1920;
    snapshot.layers.resize (1);
    constexpr double pi = 3.14159265358979323846;
    for (auto& channel : snapshot.layers[0].audio) channel.resize (snapshot.length);
    for (int i = 0; i < snapshot.length; ++i)
    {
        snapshot.layers[0].audio[0][i] = static_cast<float> (std::cos (2 * pi * 1000 * i / snapshot.rate));
        snapshot.layers[0].audio[1][i] = static_cast<float> (0.25 + std::sin (2 * pi * 50000 * i / snapshot.rate));
    }
    resampleLooperSnapshot (snapshot, 48000);
    BOOST_REQUIRE_EQUAL (snapshot.length, 480);
    double passError = 0, aliasEnergy = 0;
    for (int i = 0; i < snapshot.length; ++i)
    {
        const double expected = std::cos (2 * pi * 1000 * i / 48000);
        passError = std::max (passError, std::abs (snapshot.layers[0].audio[0][i] - expected));
        const double alias = snapshot.layers[0].audio[1][i] - 0.25;
        aliasEnergy += alias * alias;
    }
    BOOST_CHECK_LT (passError, 0.001); // Includes both seam edges, no padding loss.
    BOOST_CHECK_LT (std::sqrt (aliasEnergy / snapshot.length), 0.0001); // > 76 dB rejection.
    const auto unchanged = snapshot.layers[0].audio;
    resampleLooperSnapshot (snapshot, 48000);
    BOOST_CHECK (snapshot.layers[0].audio == unchanged);
}

BOOST_AUTO_TEST_CASE (OfflineResamplerRoundsDurationAndHandlesLargeRateRatios)
{
    LooperEngine::Snapshot snapshot;
    snapshot.rate = 44100;
    snapshot.length = 7;
    snapshot.layers.push_back ({ {}, { std::vector<float> (7, 0.25f), std::vector<float> (7, -0.5f) } });
    resampleLooperSnapshot (snapshot, 48000);
    BOOST_REQUIRE_EQUAL (snapshot.length, 8);
    for (const auto value : snapshot.layers[0].audio[0]) BOOST_CHECK_CLOSE (value, 0.25f, 0.001f);
    for (const auto value : snapshot.layers[0].audio[1]) BOOST_CHECK_CLOSE (value, -0.5f, 0.001f);
    // Multistage decimation bounds coefficient memory even for synthetic rates.
    snapshot.rate = 384000;
    resampleLooperSnapshot (snapshot, 1);
    BOOST_REQUIRE_EQUAL (snapshot.length, 1);
    BOOST_CHECK_CLOSE (snapshot.layers[0].audio[0][0], 0.25f, 0.001f);
    BOOST_CHECK_CLOSE (snapshot.layers[0].audio[1][0], -0.5f, 0.001f);
}

BOOST_AUTO_TEST_CASE (SaveAndExportDuringPlaybackDoNotSkipSamplesOrTransport)
{
    LooperProcessor p;
    p.prepareToPlay (1000, 128); p.setCrossfade(0); p.setCaptureTail(0); p.setMonitor(false);
    AudioBuffer<float> seed(2, 16000); MidiBuffer midi;
    for (int c=0;c<2;++c) for(int i=0;i<seed.getNumSamples();++i) seed.setSample(c,i,0.25f);
    p.request(Action::record); p.processBlock(seed,midi);
    p.request(Action::play); seed.clear(); p.processBlock(seed,midi);
    const auto layerID=p.view().layers[0].id;
    std::atomic<bool> done {false}; std::atomic<int> bad {0}, blocks {0};
    std::thread audio([&] {
        AudioBuffer<float> block(2,128); MidiBuffer events;
        while(!done.load()) {
            block.clear(); p.processBlock(block,events);
            for(int c=0;c<2;++c) for(int i=0;i<128;++i)
                if(std::abs(block.getSample(c,i)-0.25f)>1.0e-6f) ++bad;
            ++blocks; std::this_thread::yield();
        }
    });
    while(blocks.load()<2) std::this_thread::yield();
    const int before=blocks.load();
    for(int attempt=0;attempt<8;++attempt) {
        MemoryBlock saved; p.getStateInformation(saved);
        BOOST_CHECK_EQUAL(saved.getSize(),296+16000*8);
        MemoryInputStream in(saved,false); in.setPosition(88);
        for(int i=0;i<32000;++i) if(in.readFloat()!=0.25f) { ++bad; break; }
    }
    const auto file=File::getSpecialLocation(File::tempDirectory).getNonexistentChildFile("jgod-live-export", ".wav");
    const auto result=p.exportLayer(layerID,file);
    done.store(true); audio.join();
    BOOST_CHECK(result.wasOk()); BOOST_CHECK_EQUAL(bad.load(),0); BOOST_CHECK_GT(blocks.load(),before);
    file.deleteFile();
    // Last publication remains usable after callbacks cease; no handshake required.
    MemoryBlock stoppedDevice; p.getStateInformation(stoppedDevice);
    BOOST_CHECK_EQUAL(stoppedDevice.getSize(),296+16000*8);
}

BOOST_AUTO_TEST_CASE (DenseLayerSnapshotsStayFiniteDuringConcurrentPlayback)
{
    LooperProcessor p;p.setMemoryLimits(1,20,8,false,1);p.prepareToPlay(48000,128);
    p.setCrossfade(0);p.setCaptureTail(0);p.setMonitor(false);p.setFeedback(1);
    MidiBuffer midi;AudioBuffer<float> seed(2,512);
    const auto fill=[&]{for(int c=0;c<2;++c)for(int i=0;i<512;++i)seed.setSample(c,i,.001f);};
    fill();p.request(Action::record);p.processBlock(seed,midi);
    p.request(Action::play);seed.clear();p.processBlock(seed,midi);
    for(int take=0;take<19;++take) {
        p.request(Action::overdub);
        for(int pass=0;pass<8;++pass){fill();p.processBlock(seed,midi);p.view();}
        seed.clear();p.request(Action::play);p.processBlock(seed,midi);
    }
    BOOST_REQUIRE_GE(p.view().count,150);
    std::atomic<bool> done{false};std::atomic<int> bad{0};std::vector<double> timings;
    timings.reserve(4096);
    std::thread audio([&]{AudioBuffer<float> block(2,128);MidiBuffer events;
        while(!done.load()) {
            block.clear();const auto start=std::chrono::steady_clock::now();p.processBlock(block,events);
            if(timings.size()<4096)timings.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count());
            for(int c=0;c<2;++c)for(int i=0;i<128;++i)if(!std::isfinite(block.getSample(c,i)))++bad;
        }
    });
    for(int i=0;i<4;++i){MemoryBlock saved;p.getStateInformation(saved);BOOST_CHECK_GT(saved.getSize(),500000u);}
    done=true;audio.join();BOOST_CHECK_EQUAL(bad.load(),0);BOOST_REQUIRE(!timings.empty());
    std::sort(timings.begin(),timings.end());
    BOOST_TEST_MESSAGE("153 layers / 48kHz / 128 frames, concurrent saves: median "<<timings[timings.size()/2]
        <<" us, p99 "<<timings[std::min(timings.size()-1,timings.size()*99/100)]<<" us, max "<<timings.back()<<" us (ordinary worker, not CoreAudio)");
}

BOOST_AUTO_TEST_CASE (LiveOverdubSnapshotsAreCoherentAcrossStereoAndLoopWraps)
{
    LooperProcessor p; p.prepareToPlay(1000,128); p.setCrossfade(0); p.setCaptureTail(0);
    AudioBuffer<float> seed(2,16000); seed.clear(); MidiBuffer midi;
    p.request(Action::record); p.processBlock(seed,midi);
    p.request(Action::play); p.processBlock(seed,midi);
    p.request(Action::overdub);
    std::atomic<bool> done {false}; std::atomic<int> blocks {0};
    std::thread audio([&] {
        AudioBuffer<float> block(2,128); MidiBuffer events;
        while(!done.load()) {
            for(int c=0;c<2;++c) for(int i=0;i<128;++i) block.setSample(c,i,0.125f);
            p.processBlock(block,events); ++blocks; std::this_thread::yield();
        }
    });
    while(blocks.load()<4) std::this_thread::yield();
    for(int attempt=0;attempt<12;++attempt) {
        MemoryBlock saved; p.getStateInformation(saved);
        MemoryInputStream in(saved,false); in.setPosition(24);
        const int count=in.readInt(); BOOST_CHECK_GE(count,2); BOOST_CHECK_LE(count,LooperEngine::maxLayers);
        in.setPosition(76+12+16000*8+12);
        std::vector<float> left(16000);
        float low=std::numeric_limits<float>::max(), high=0;
        for(auto& v:left) {v=in.readFloat();low=std::min(low,v);high=std::max(high,v);}
        int transitions=0;
        for(int i=0;i<16000;++i) {
            BOOST_CHECK_EQUAL(in.readFloat(),left[i]);
            if(i && left[i]!=left[i-1]) ++transitions;
        }
        BOOST_CHECK_LE(high-low,0.125f); BOOST_CHECK_LE(transitions,1);
    }
    done.store(true);audio.join();
}

BOOST_AUTO_TEST_CASE (SnapshotsSurviveReplacementAndCircularHistoryReuse)
{
    LooperProcessor p; p.prepareToPlay(1000,64); p.setCrossfade(0); p.setCaptureTail(0);
    struct Clock : AudioPlayHead {
        double beat=0;
        Optional<PositionInfo> getPosition() const override {
            PositionInfo p; p.setBpm(120); p.setPpqPosition(beat); p.setIsPlaying(true);
            p.setTimeSignature(TimeSignature{4,4}); return p;
        }
    } clock;
    p.setPlayHead(&clock); p.setQuantized(true);
    std::atomic<bool> done {false}; std::atomic<int> blocks {0};
    std::thread audio([&] {
        AudioBuffer<float> block(2,64); MidiBuffer events;
        int n=0;
        while(!done.load()) {
            const float value=float((n/80)%8+1)*0.0625f;
            for(int i=0;i<64;++i) {block.setSample(0,i,value);block.setSample(1,i,-value);}
            if(n%80==3) p.request(Action::freshRecord);
            if(n%80==30) p.request(Action::play);
            p.processBlock(block,events); clock.beat+=64.0/500; ++n; ++blocks;
            std::this_thread::yield();
        }
    });
    while(blocks.load()<100) std::this_thread::yield();
    for(int attempt=0;attempt<20;++attempt) {
        MemoryBlock saved;p.getStateInformation(saved);
        MemoryInputStream in(saved,false); in.setPosition(20);
        const int length=in.readInt(),count=in.readInt();
        BOOST_CHECK_LE(count,1);in.setPosition(76);
        if(count) {
            in.skipNextBytes(12);std::vector<float> l(length);
            for(auto& v:l) v=in.readFloat();
            for(int i=0;i<length;++i) BOOST_CHECK_EQUAL(in.readFloat(),-l[i]);
        }
    }
    done.store(true);audio.join();
}

BOOST_AUTO_TEST_CASE (FourLoopers192kRealtimeStress)
{
    if(std::getenv("EL_LOOPER_STRESS")==nullptr) return;
    constexpr int blockSize=128, sampleRate=192000;
    const int seconds=std::getenv("EL_LOOPER_STRESS_SECONDS") ? std::clamp(std::atoi(std::getenv("EL_LOOPER_STRESS_SECONDS")),1,60) : 1;
    std::array<std::unique_ptr<LooperProcessor>,4> loopers;
    AudioBuffer<float> block(2,blockSize);MidiBuffer events;
    const auto feed=[&](LooperProcessor& p,float value) {
        for(int c=0;c<2;++c) for(int i=0;i<blockSize;++i) block.setSample(c,i,value);
        p.processBlock(block,events);
    };
    for(auto& p:loopers) {
        p=std::make_unique<LooperProcessor>();
        if(std::getenv("EL_LOOPER_STRESS_ENHANCED")) { p->setUndoDepth(9);p->setFeedback(0.9f);p->setPan((&p-&loopers[0])%2?0.5f:-0.5f);p->setPlayFade(20);p->setStopFade(20); }
        p->prepareToPlay(sampleRate,blockSize);
        p->request(Action::record);
        for(int i=0;i<seconds*sampleRate/blockSize;++i) feed(*p,0.0625f);
        p->request(Action::play);feed(*p,0);
        for(int layer=1;layer<4;++layer) {
            p->request(Action::overdub);
            for(int i=0;i<seconds*sampleRate/blockSize;++i) feed(*p,0.0625f);
            p->request(Action::overdub);feed(*p,0);
        }
        p->setMonitor(false);
    }
    std::atomic<bool> done {false}; std::atomic<int> blocks {0}, bad {0};
    std::vector<double> durations;durations.reserve(200000);
    std::vector<double> cloneWindow;cloneWindow.reserve(5000);
    std::atomic<int> replaceWindowStart {-1};
    std::thread audio([&] {
        while(!done.load()) {
            const auto start=std::chrono::steady_clock::now();
            for(auto& p:loopers) feed(*p,0);
            const auto elapsed=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
            if(durations.size()<200000) durations.push_back(elapsed);
            const auto first=replaceWindowStart.load();
            if(first>=0 && blocks.load()-first<5000) cloneWindow.push_back(elapsed);
            if(!std::isfinite(block.getSample(0,127))) ++bad;
            ++blocks;
        }
    });
    while(blocks.load()<1000) std::this_thread::yield();
    // Keep capture active while saving: stresses the copy-before-write path too.
    if(std::getenv("EL_LOOPER_STRESS_REPLACE")) {
        replaceWindowStart.store(blocks.load());
        for(auto& p:loopers) p->request(Action::replace);
    } else loopers[0]->request(Action::overdub);
    for(int pass=0;pass<2;++pass) for(auto& p:loopers) { MemoryBlock saved;p->getStateInformation(saved); }
    done.store(true);audio.join();
    if(!cloneWindow.empty()) {
        std::sort(cloneWindow.begin(),cloneWindow.end());
        BOOST_TEST_MESSAGE("Replace initial clone window: blocks="<<cloneWindow.size()<<" median_us="<<cloneWindow[cloneWindow.size()/2]
            <<" p99_us="<<cloneWindow[cloneWindow.size()*99/100]<<" max_us="<<cloneWindow.back());
    }
    std::sort(durations.begin(),durations.end());
    BOOST_REQUIRE(!durations.empty());BOOST_CHECK_EQUAL(bad.load(),0);
    BOOST_TEST_MESSAGE("4 loopers / 4 layers / 192kHz / 128 samples / loop_seconds="<<seconds<<" with concurrent save: timed_blocks="<<durations.size()<<" total_blocks="<<blocks.load()
        <<" median_us="<<durations[durations.size()/2]<<" p99_us="<<durations[durations.size()*99/100]
        <<" max_us="<<durations.back()<<" budget_us="<<1e6*blockSize/sampleRate);
}


BOOST_AUTO_TEST_CASE(MasterLinkedSameCircularAndMultiplesSilentPrefix)
{
    LooperEngine master;master.prepare(100); master.setTransitionOptions(0,0);master.setLinkContext(1,0,0,0);
    master.apply(Action::record);for(int i=0;i<4;++i)frame(master,.1f,.2f);master.apply(Action::record);
    BOOST_REQUIRE_EQUAL(master.getFixedLength(),4);BOOST_CHECK_EQUAL(master.getOrigin(),0);
    LooperEngine same;same.prepare(100); same.setTransitionOptions(0,0);same.setLinkContext(2,4,2,0);same.setCaptureBehavior(false,false,0);same.apply(Action::record);
    for(int i=1;i<=4;++i)frame(same,float(i),float(i));same.apply(Action::play);
    BOOST_CHECK_EQUAL(same.getFixedLength(),4);
    BOOST_REQUIRE_EQUAL(same.getLayerCount(),2);
    BOOST_CHECK_EQUAL(same.layerSample(0,0,0),0);BOOST_CHECK_EQUAL(same.layerSample(0,0,1),0);
    BOOST_CHECK_EQUAL(same.layerSample(1,0,0),3);BOOST_CHECK_EQUAL(same.layerSample(1,0,1),4);
    BOOST_CHECK_EQUAL(same.layerSample(0,0,2),1);BOOST_CHECK_EQUAL(same.layerSample(0,0,3),2);
    same.apply(Action::record);frame(same,7,7);same.apply(Action::record);
    BOOST_CHECK_EQUAL(same.getLength(),4);BOOST_CHECK_EQUAL(same.getFixedLength(),4);
    LooperEngine longer;longer.prepare(100); longer.setTransitionOptions(0,0);longer.setOptions(false,0,0);longer.setLinkContext(3,4,2,0);
    longer.apply(Action::overdub);for(int i=1;i<=6;++i)frame(longer,float(i),float(i));longer.apply(Action::overdub);
    BOOST_REQUIRE_EQUAL(longer.getFixedLength(),8);BOOST_CHECK_EQUAL(longer.getMultiple(),2);
    BOOST_CHECK_EQUAL(longer.layerSample(0,0,0),0);BOOST_CHECK_EQUAL(longer.layerSample(0,0,1),0);
    for(int i=2;i<8;++i)BOOST_CHECK_EQUAL(longer.layerSample(0,0,i),i-1);
    longer.apply(Action::overdub);frame(longer,.5f,.5f);frame(longer,.6f,.6f);longer.apply(Action::overdub);
    BOOST_CHECK_CLOSE(longer.sample(0,0),.5f,.001);BOOST_CHECK_CLOSE(longer.sample(0,1),.6f,.001);
    longer.apply(Action::record);frame(longer,9,9);longer.apply(Action::record);
    BOOST_CHECK_EQUAL(longer.getFixedLength(),8);BOOST_CHECK_EQUAL(longer.getLength(),8);
}
BOOST_AUTO_TEST_CASE(LinkedWaitsForMasterAndNextBarResetClearsEverything)
{
    LooperEngine e;e.prepare(100); e.setTransitionOptions(0,0);e.setLinkContext(2,0,0,0);e.apply(Action::record);frame(e,1,1);
    BOOST_CHECK_EQUAL(e.getLayerCount(),0);
    e.setLinkContext(2,8,5,0);e.setOptions(true,0,0);e.setClock(6000,4,4,1,true);
    e.apply(Action::overdub);BOOST_CHECK(e.isStartPending());
    for(int i=0;i<3;++i)frame(e,1,1);
    BOOST_CHECK_EQUAL(e.getLayerCount(),0);frame(e,2,2);BOOST_CHECK_EQUAL(e.getLayerCount(),1);
    BOOST_CHECK_EQUAL(e.layerSample(0,0,0),2);
    e.resetAll();BOOST_CHECK_EQUAL(e.getFixedLength(),0);BOOST_CHECK_EQUAL(e.getLength(),0);
    BOOST_CHECK(!e.canUndo());BOOST_CHECK(!e.isStartPending());BOOST_CHECK(!e.isEndPending());
    e.apply(Action::undo);BOOST_CHECK_EQUAL(e.getLength(),0);
}
BOOST_AUTO_TEST_CASE(LinkedMultiplesEndsAtWholeCycleAndKeepsStructureThroughLoad)
{
    LooperEngine e;e.prepare(100); e.setTransitionOptions(0,0);e.setOptions(false,0,0);e.setLinkContext(3,4,2,0);
    e.apply(Action::record);frame(e,1,1);e.apply(Action::record);BOOST_CHECK(e.isEndPending());
    frame(e,2,2);BOOST_REQUIRE_EQUAL(e.getFixedLength(),4);
    const auto saved=e.snapshot();LooperEngine restored;restored.prepare(100); restored.setTransitionOptions(0,0);restored.load(saved);
    restored.setLinkContext(3,4,0,0);BOOST_CHECK_EQUAL(restored.getFixedLength(),4);
    restored.apply(Action::clear);BOOST_CHECK_EQUAL(restored.getFixedLength(),4);BOOST_CHECK_EQUAL(restored.getLength(),0);
    restored.apply(Action::record);frame(restored,3,3);restored.apply(Action::record);BOOST_CHECK_EQUAL(restored.getLength(),4);
}

BOOST_AUTO_TEST_CASE(LinkedZeroSampleCaptureCancelsWithoutPhantomLayer)
{
    for(int mode:{2,3}) {
        LooperEngine e;e.prepare(100); e.setTransitionOptions(0,0);e.setLinkContext(mode,4,2,0);e.apply(Action::record);e.apply(Action::record);
        BOOST_CHECK_EQUAL(e.getLength(),0);BOOST_CHECK_EQUAL(e.getLayerCount(),0);BOOST_CHECK_EQUAL(e.getFixedLength(),0);
        BOOST_CHECK(!e.isEndPending());
    }
}

BOOST_AUTO_TEST_CASE(LinkedGroupRestartDoesNotRebaseSamplesBeforeMidiEvent)
{
    LooperProcessor p;p.prepareToPlay(100,16); p.setCrossfade(0); p.setCaptureTail(0);p.setMonitor(false);p.managerOwner=99;p.managerTarget=1;
    p.setManagerContext(1,0,0,0,0);p.request(Action::record);
    AudioBuffer<float> input(2,10);for(int c=0;c<2;++c)for(int i=0;i<10;++i)input.setSample(c,i,float(i+1));
    MidiBuffer midi;p.processBlock(input,midi);
    p.setManagerContext(1,0,10,0,0);p.request(Action::record);AudioBuffer<float> one(2,1);one.clear();p.processBlock(one,midi);
    BOOST_REQUIRE_EQUAL(p.managerFixedLength.load(),10);
    p.setManagerContext(1,10,7,0,0);AudioBuffer<float> audio(2,8);audio.clear();
    uint8 bytes[22]{0xf0,0x7d,'J','L'};bytes[4]=99;bytes[14]=LooperProcessor::managerRestartAllCommand;bytes[21]=0xf7;
    midi.addEvent(bytes,22,4);p.processBlock(audio,midi);
    const float expected[]{8,9,10,1,1,2,3,4};
    for(int i=0;i<8;++i)BOOST_CHECK_EQUAL(audio.getSample(0,i),expected[i]);
}
BOOST_AUTO_TEST_CASE(MasterReassignmentDoesNotResizeLinkedRecording)
{
    LooperEngine e;e.prepare(100);e.setTransitionOptions(0,0);e.setOptions(false,0,0);e.setLinkContext(3,4,0,0);
    e.apply(Action::record);for(int i=0;i<8;++i)frame(e,float(i+1)/10,float(i+1)/10);e.apply(Action::record);e.apply(Action::stop);
    const auto before=e.snapshot();BOOST_REQUIRE_EQUAL(before.fixedLength,8);
    e.prepareMasterChange(3,8,100);e.setLinkContext(3,8,100,100);
    const auto after=e.snapshot();
    BOOST_CHECK_EQUAL(after.length,before.length);BOOST_CHECK_EQUAL(after.fixedLength,8);BOOST_CHECK_EQUAL(after.multiple,1);
    BOOST_CHECK(e.getState()==LooperEngine::State::stopped);
    BOOST_REQUIRE_EQUAL(after.layers.size(),before.layers.size());
    for(size_t i=0;i<after.layers.size();++i) {
        BOOST_CHECK(after.layers[i].audio[0]==before.layers[i].audio[0]);
        BOOST_CHECK(after.layers[i].audio[1]==before.layers[i].audio[1]);
    }
}

BOOST_AUTO_TEST_CASE(LinkedMultiplePhaseSurvivesSaveAndRateConversion)
{
    LooperEngine e;e.prepare(100); e.setTransitionOptions(0,0);e.setOptions(false,0,0);e.setLinkContext(3,4,6,0);
    e.apply(Action::record);for(int i=0;i<6;++i)frame(e,.25f,.25f);e.apply(Action::record);
    auto saved=e.snapshot();BOOST_REQUIRE_EQUAL(saved.phaseOffset,4);BOOST_REQUIRE_EQUAL(saved.fixedLength,8);
    resampleLooperSnapshot(saved,200);LooperEngine restored;restored.prepare(200); restored.setTransitionOptions(0,0);restored.load(saved);
    restored.setLinkContext(3,8,0,0);auto state=restored.snapshot();
    BOOST_CHECK_EQUAL(state.fixedLength,16);BOOST_CHECK_EQUAL(state.multiple,2);BOOST_CHECK_EQUAL(state.phaseOffset,8);
}

BOOST_AUTO_TEST_CASE(MidiUndoRedoShortReleaseAndLongAcrossBlocks)
{
    LooperProcessor p; p.prepareToPlay(100,8); p.setCrossfade(0); p.setCaptureTail(0); p.setMonitor(false);
    AudioBuffer<float> audio(2,8); MidiBuffer midi;
    const auto render=[&] { audio.clear(); p.processBlock(audio,midi); return p.view(); };
    p.request(Action::record); render(); p.request(Action::play); render();
    p.request(Action::overdub); render(); p.request(Action::overdub); render();
    BOOST_REQUIRE_EQUAL(p.view().count,2);
    cc(midi,29,3); BOOST_CHECK_EQUAL(render().count,2);
    cc(midi,29,2,0); BOOST_CHECK_EQUAL(render().count,1);
    BOOST_CHECK(p.view().redo);
    cc(midi,29,0); render();
    for(int i=0;i<9;++i) BOOST_CHECK_EQUAL(render().count,1);
    // Exactly 80 samples held, release at next block boundary must be Redo.
    cc(midi,29,0,0); BOOST_CHECK_EQUAL(render().count,2);
    BOOST_CHECK(!p.view().redo);
}
BOOST_AUTO_TEST_CASE(MidiOverdubTapAndReplaceHoldPreserveOtherLayer)
{
    LooperProcessor p; p.prepareToPlay(100,8); p.setCrossfade(0); p.setCaptureTail(0); p.setMonitor(false);
    p.setCrossfade(0); p.setCaptureTail(0);
    AudioBuffer<float> audio(2,8); MidiBuffer midi;
    const auto render=[&](float value=1) { for(int c=0;c<2;++c) for(int f=0;f<8;++f) audio.setSample(c,f,value); p.processBlock(audio,midi); return p.view(); };
    p.request(Action::record); render(); p.request(Action::play); render();
    cc(midi,23,0); BOOST_CHECK_EQUAL(render().count,1);
    cc(midi,23,0,0); BOOST_CHECK_EQUAL(render(2).count,2);
    cc(midi,23,0); render(2); cc(midi,23,0,0); render();
    BOOST_REQUIRE_EQUAL(p.view().state,int(LooperEngine::State::playing));
    cc(midi,23,0); render(); for(int i=0;i<9;++i) render();
    BOOST_CHECK_EQUAL(render(9).state,int(LooperEngine::State::replacing));
    cc(midi,23,0,0); BOOST_CHECK_EQUAL(render().state,int(LooperEngine::State::playing));
    BOOST_REQUIRE_EQUAL(p.view().count,4);
    BOOST_CHECK(p.view().layers[3].replace);
    MemoryBlock saved; p.getStateInformation(saved);
    MemoryInputStream in(saved,false); in.setPosition(88);
    for(int i=0;i<16;++i) BOOST_CHECK_EQUAL(in.readFloat(),1); // original layer intact
    in.setPosition(88+3*(8*8+12));
    for(int i=0;i<16;++i) BOOST_CHECK_EQUAL(in.readFloat(),9); // Replace is a new layer
    p.editLayer(p.view().layers[3].id,0,0); render(0);
    BOOST_CHECK_GT(audio.getMagnitude(0,8),0); // muted Replace reveals previous audio
}


BOOST_AUTO_TEST_CASE(StopAndBypassCancelHeldGesturesWithoutReleaseAction)
{
    LooperProcessor p; p.prepareToPlay(100,8); p.setCrossfade(0); p.setCaptureTail(0); AudioBuffer<float> audio(2,8); MidiBuffer midi;
    const auto render=[&] {audio.clear();p.processBlock(audio,midi);return p.view();};
    p.request(Action::record);render();p.request(Action::play);render();
    cc(midi,23,0);render();p.request(Action::stop);render();
    cc(midi,23,0,0);BOOST_CHECK_EQUAL(render().state,int(LooperEngine::State::stopped));
    p.request(Action::play);render();cc(midi,23,0);render();p.setBypassed(true);render();
    p.setBypassed(false);cc(midi,23,0,0);BOOST_CHECK_EQUAL(render().count,1);
    cc(midi,29,0);render();p.scheduleSectionEvent(3,false);render();
    cc(midi,29,0,0);BOOST_CHECK_EQUAL(render().count,1);
    BOOST_CHECK_EQUAL(p.view().state,int(LooperEngine::State::stopped));
}
BOOST_AUTO_TEST_CASE(UndoMappingV6RoundtripAndV5Migration)
{
    LooperProcessor p;p.prepareToPlay(100,8); p.setCrossfade(0); p.setCaptureTail(0);BOOST_REQUIRE(p.setMidiMapping(5,{77,3}));
    MemoryBlock saved;p.getStateInformation(saved);LooperProcessor q;
    q.setStateInformation(saved.getData(),int(saved.getSize()));BOOST_CHECK_EQUAL(q.getMidiMapping(5).cc,77);BOOST_CHECK_EQUAL(q.getMidiMapping(5).channel,3);
    saved.removeSection(saved.getSize()-140,140);static_cast<unsigned char*>(saved.getData())[4]=6; // Empty v8 settings plus v7 extension.
    LooperProcessor v6;v6.setStateInformation(saved.getData(),int(saved.getSize()));
    BOOST_CHECK_EQUAL(v6.getMidiMapping(5).cc,77);BOOST_CHECK_EQUAL(v6.getFeedback(),1);BOOST_CHECK_EQUAL(v6.getPan(),0);BOOST_CHECK_EQUAL(v6.getUndoDepth(),1);
    saved.removeSection(saved.getSize()-4,4);static_cast<unsigned char*>(saved.getData())[4]=5;
    q.setStateInformation(saved.getData(),int(saved.getSize()));BOOST_CHECK_EQUAL(q.getMidiMapping(5).cc,29);
}


BOOST_AUTO_TEST_CASE(ReplaceSnapshotPreservesPriorAudioAndUndoRedo)
{
    LooperProcessor p;p.prepareToPlay(1000,10000); p.setCrossfade(0); p.setCaptureTail(0);p.setMonitor(false);
    p.setCrossfade(0);p.setCaptureTail(0);
    AudioBuffer<float> audio(2,10000);MidiBuffer midi;
    for(int c=0;c<2;++c) for(int f=0;f<10000;++f) audio.setSample(c,f,1);
    p.request(Action::record);p.processBlock(audio,midi);p.view();
    audio.setSize(2,8);audio.clear();p.request(Action::play);p.processBlock(audio,midi);p.view();
    p.request(Action::replace);
    for(int c=0;c<2;++c) for(int f=0;f<8;++f) audio.setSample(c,f,9);
    p.processBlock(audio,midi);BOOST_REQUIRE_EQUAL(p.view().count,2);
    const auto inspect=[&](bool replaced) {
        MemoryBlock saved;p.getStateInformation(saved);MemoryInputStream in(saved,false);in.setPosition(24);
        BOOST_CHECK_EQUAL(in.readInt(),replaced?2:1);in.setPosition(88);
        for(int c=0;c<2;++c) for(int f=0;f<10000;++f) BOOST_CHECK_EQUAL(in.readFloat(),1);
        if(replaced) { in.skipNextBytes(12);
            for(int c=0;c<2;++c) for(int f=0;f<10000;++f)
                BOOST_CHECK_EQUAL(in.readFloat(),f>=8 && f<16?9:0);
        }
    };
    inspect(true);
    p.request(Action::endReplace);audio.clear();p.processBlock(audio,midi);p.view();
    p.request(Action::undo);p.processBlock(audio,midi);p.view();inspect(false);
    p.request(Action::redo);p.processBlock(audio,midi);p.view();inspect(true);
}


BOOST_AUTO_TEST_CASE(SectionEventsSortByOffsetBeforeSameBlockPanic)
{
    LooperProcessor p;p.prepareToPlay(100,8); p.setCrossfade(0); p.setCaptureTail(0);p.setMonitor(false);
    AudioBuffer<float> audio(2,8);MidiBuffer midi;
    for(int c=0;c<2;++c) for(int f=0;f<8;++f) audio.setSample(c,f,1);
    p.request(Action::record);p.processBlock(audio,midi);p.view();
    p.request(Action::stop);audio.clear();p.processBlock(audio,midi);p.view();
    p.scheduleSectionEvent(6,false);p.scheduleSectionEvent(2,true);p.scheduleSectionEvent(6,false);
    audio.clear();p.processBlock(audio,midi);
    for(int f=0;f<8;++f) BOOST_CHECK_EQUAL(audio.getSample(0,f),f>=2 && f<6?1:0);
    BOOST_CHECK_EQUAL(p.view().state,int(LooperEngine::State::stopped));
}


BOOST_AUTO_TEST_CASE(PanPreservesStereoAndFoldsAtExtremes)
{
    LooperEngine e; record(e);
    float l=0,r=0; e.process(l,r,1,false); BOOST_CHECK_EQUAL(l,1); BOOST_CHECK_EQUAL(r,10);
    e.editLayer(e.getLayer(0).id,4,-1); l=r=0; e.process(l,r,1,false);
    BOOST_CHECK_EQUAL(l,22); BOOST_CHECK_EQUAL(r,0);
    e.editLayer(e.getLayer(0).id,4,1); l=r=0; e.process(l,r,1,false);
    BOOST_CHECK_EQUAL(l,3); BOOST_CHECK_EQUAL(r,30); // Smoothed travel crosses center first.
    l=r=0;e.process(l,r,1,false);BOOST_CHECK_EQUAL(l,0);BOOST_CHECK_EQUAL(r,44);
    e.editLayer(e.getLayer(0).id,4,0); e.setMixOptions(-1,1,0,0); l=r=0;e.process(l,r,1,false);
    BOOST_CHECK_EQUAL(l,11); BOOST_CHECK_EQUAL(r,0);
    BOOST_CHECK_EQUAL(e.layerSample(0,0,0),1); BOOST_CHECK_EQUAL(e.layerSample(0,1,0),10);
}

BOOST_AUTO_TEST_CASE(FeedbackPartialCycleUndoRedoAndStablePlayback)
{
    LooperEngine e; record(e); e.setMixOptions(0,0.25f,0,0);
    e.editLayer(e.getLayer(0).id,0,0); // Muting must not exempt a layer from feedback.
    e.apply(Action::overdub); frame(e,2,2); frame(e,2,2); e.apply(Action::overdub);
    BOOST_CHECK_CLOSE(e.getLayer(0).feedbackGain,0.5f,0.001);
    for(int i=0;i<20;++i) frame(e,0,0);
    BOOST_CHECK_CLOSE(e.getLayer(0).feedbackGain,0.5f,0.001);
    e.apply(Action::undo); BOOST_CHECK_EQUAL(e.getLayerCount(),1); BOOST_CHECK_EQUAL(e.getLayer(0).feedbackGain,1);
    e.apply(Action::redo); BOOST_CHECK_EQUAL(e.getLayerCount(),2); BOOST_CHECK_CLOSE(e.getLayer(0).feedbackGain,0.5f,0.001);
    BOOST_CHECK_EQUAL(e.getLayer(0).gain,1);
}

BOOST_AUTO_TEST_CASE(FeedbackEarlierOverdubPassesAndZero)
{
    LooperEngine e; record(e); e.setMixOptions(0,0.5f,0,0); e.apply(Action::overdub);
    for(int i=0;i<4;++i) frame(e,2,2);
    for(int i=0;i<4;++i) frame(e,0,0);
    BOOST_CHECK_CLOSE(e.getLayer(0).feedbackGain,0.25f,0.001);
    BOOST_CHECK_EQUAL(e.getLayerCount(),3);
    for(int i=0;i<4;++i) { BOOST_CHECK_EQUAL(e.layerSample(1,0,i),2);
        BOOST_CHECK_CLOSE(e.getLayer(1).feedbackGain,0.5f,0.001f); }
    e.apply(Action::overdub); e.setMixOptions(0,0,0,0); e.apply(Action::overdub);
    for(int i=0;i<4;++i) frame(e,3,3);
    BOOST_CHECK_EQUAL(e.getLayer(0).feedbackGain,0); BOOST_CHECK_EQUAL(e.getLayer(1).feedbackGain,0);
    e.apply(Action::overdub); e.apply(Action::undo);
    BOOST_CHECK_CLOSE(e.getLayer(0).feedbackGain,0.25f,0.001);
    BOOST_CHECK_CLOSE(e.getLayer(1).feedbackGain,0.5f,0.001f);
    BOOST_CHECK_EQUAL(e.getLayerCount(),3);
}

BOOST_AUTO_TEST_CASE(NineUndoRedoStepsAndBranchAfterUndo)
{
    LooperEngine e; e.prepare(10); e.setTransitionOptions(0,0); e.setUndoDepth(9);
    for(int take=1;take<=12;++take) {
        e.apply(Action::freshRecord); for(int f=0;f<4;++f) frame(e,float(take),float(take)); e.apply(Action::play);
    }
    for(int take=11;take>=3;--take) { BOOST_REQUIRE(e.canUndo()); e.apply(Action::undo); BOOST_CHECK_EQUAL(e.sample(0,0),take); }
    BOOST_CHECK(!e.canUndo());
    for(int take=4;take<=12;++take) { BOOST_REQUIRE(e.canRedo()); e.apply(Action::redo); BOOST_CHECK_EQUAL(e.sample(0,0),take); }
    BOOST_CHECK(!e.canRedo());
    e.apply(Action::undo); e.apply(Action::undo); e.apply(Action::freshRecord); frame(e,99,99); e.apply(Action::play);
    BOOST_CHECK(!e.canRedo()); e.apply(Action::undo); BOOST_CHECK_EQUAL(e.sample(0,0),10);
    e.setUndoDepth(1); BOOST_CHECK(e.canRedo()); e.apply(Action::redo); BOOST_CHECK_EQUAL(e.sample(0,0),99);
}

BOOST_AUTO_TEST_CASE(DeepHistoryAcrossFullTakesDoesNotExhaustOrCorrupt)
{
    LooperEngine e; e.prepare(10); e.setTransitionOptions(0,0); e.setUndoDepth(9);
    for(int take=0;take<4;++take) {
        e.apply(Action::freshRecord); for(int f=0;f<4;++f) frame(e,1,1); e.apply(Action::play);
        for(int layer=1;layer<8;++layer) { e.apply(Action::overdub); for(int f=0;f<4;++f) frame(e,1,1); e.apply(Action::overdub); }
        BOOST_CHECK_EQUAL(e.getLayerCount(),8); BOOST_CHECK_EQUAL(e.sample(0,0),8);
    }
    for(int i=0;i<9;++i) { BOOST_REQUIRE(e.canUndo()); e.apply(Action::undo); BOOST_CHECK_GT(e.getLength(),0); }
}

BOOST_AUTO_TEST_CASE(PlayAndStopFadesRenderActualContinuingAudio)
{
    LooperEngine e; record(e); e.apply(Action::stop); frame(e,0,0);
    e.setMixOptions(0,1,400,400); e.apply(Action::play);
    for(int i=0;i<4;++i) { float l=0,r=0;e.process(l,r,1,false);BOOST_CHECK_CLOSE(l,float((i+1)*(i+1))/4,0.001); }
    e.apply(Action::stop);
    const float expected[]={0.75f,1.0f,0.75f,0};
    for(float value:expected) { float l=0,r=0;e.process(l,r,1,false);BOOST_CHECK_SMALL(l-value,0.00001f); }
    float l=0,r=0;e.process(l,r,1,false);BOOST_CHECK_EQUAL(l,0);BOOST_CHECK(e.getState()==LooperEngine::State::stopped);
}

BOOST_AUTO_TEST_CASE(NewMixSettingsRoundTripAndUndoDepthLiveGuard)
{
    LooperProcessor p; p.prepareToPlay(100,4); p.setCrossfade(0); p.setCaptureTail(0);p.setPan(-0.4f);p.setFeedback(0.75f);p.setPlayFade(123);p.setStopFade(456);p.setUndoDepth(3);
    AudioBuffer<float> audio(2,4);audio.clear();MidiBuffer midi;
    p.request(Action::record);p.processBlock(audio,midi);p.request(Action::play);p.processBlock(audio,midi);
    p.editLayer(p.view().layers[0].id,4,0.8f);p.processBlock(audio,midi);p.setUndoDepth(8);BOOST_CHECK_EQUAL(p.getUndoDepth(),3);
    MemoryBlock saved;p.getStateInformation(saved);LooperProcessor restored;restored.setStateInformation(saved.getData(),int(saved.getSize()));
    BOOST_CHECK_CLOSE(restored.getPan(),-0.4f,0.001);BOOST_CHECK_CLOSE(restored.getFeedback(),0.75f,0.001);
    BOOST_CHECK_EQUAL(restored.getPlayFade(),123);BOOST_CHECK_EQUAL(restored.getStopFade(),456);BOOST_CHECK_EQUAL(restored.getUndoDepth(),3);
    BOOST_CHECK_CLOSE(restored.view().layers[0].pan,0.8f,0.001);
}

BOOST_AUTO_TEST_CASE(ReplaceOfFadedLayerMakesNewInputAudibleAndPreservesUndo)
{
    LooperEngine e; e.prepare(1000); e.setTransitionOptions(0,0);
    e.apply(Action::record);for(int i=0;i<10000;++i) frame(e,1,1);e.apply(Action::play);
    e.setMixOptions(0,0,0,0);e.apply(Action::overdub);for(int i=0;i<10000;++i) frame(e,0,0);e.apply(Action::overdub);
    const auto silence=e.getLayer(1).id;e.editLayer(silence,3,0); // Keep only faded original.
    BOOST_CHECK_EQUAL(e.getLayer(0).feedbackGain,0);
    e.apply(Action::replace);frame(e,3,3);e.apply(Action::endReplace);
    BOOST_REQUIRE_EQUAL(e.getLayerCount(),2);
    BOOST_CHECK_EQUAL(e.getLayer(0).feedbackGain,0);BOOST_CHECK_EQUAL(e.layerSample(1,0,0),3);
    BOOST_CHECK_EQUAL(e.sample(0,0),3);BOOST_CHECK_EQUAL(e.sample(0,9000),0);
    const auto saved=e.snapshot();BOOST_CHECK_EQUAL(saved.layers[0].audio[0][9000],1); // raw original is preserved
    e.apply(Action::undo);BOOST_CHECK_EQUAL(e.getLayerCount(),1);BOOST_CHECK_EQUAL(e.getLayer(0).feedbackGain,0);
    e.apply(Action::redo);BOOST_CHECK_EQUAL(e.getLayerCount(),2);BOOST_CHECK_EQUAL(e.sample(0,0),3);
}

BOOST_AUTO_TEST_CASE(MidiPlayStopReleaseAndHoldClear)
{
    LooperProcessor p; p.prepareToPlay(100,8); p.setCrossfade(0); p.setCaptureTail(0); p.setMonitor(false);
    AudioBuffer<float> audio(2,8); MidiBuffer midi;
    const auto render=[&] { audio.clear(); p.processBlock(audio,midi); return p.view(); };
    p.request(Action::record); render(); p.request(Action::play); render();
    cc(midi,21,0); BOOST_CHECK_EQUAL(render().state,int(LooperEngine::State::playing));
    cc(midi,21,0,0); BOOST_CHECK_EQUAL(render().state,int(LooperEngine::State::stopped));
    cc(midi,21,0); render();
    for(int i=0;i<9;++i) BOOST_CHECK_EQUAL(render().count,1);
    cc(midi,21,0,0); BOOST_CHECK_EQUAL(render().count,0); // exact threshold, no Play
    BOOST_CHECK_EQUAL(p.view().state,int(LooperEngine::State::empty));
    p.request(Action::record); render();
    cc(midi,21,0); render(); for(int i=0;i<11;++i) render();
    BOOST_CHECK_EQUAL(p.view().count,0); // clear while recording, before release
    cc(midi,21,0,0); BOOST_CHECK_EQUAL(render().state,int(LooperEngine::State::empty));
}


BOOST_AUTO_TEST_CASE(MidiRecordTapWaitsForReleaseAndHoldStartsFreshTake)
{
    LooperProcessor p; p.prepareToPlay(100,8); p.setCrossfade(0); p.setCaptureTail(0); p.setMonitor(false);
    AudioBuffer<float> audio(2,8); MidiBuffer midi;
    const auto render=[&] { audio.clear(); p.processBlock(audio,midi); return p.view(); };
    cc(midi,20,0); BOOST_CHECK_EQUAL(render().count,0);
    cc(midi,20,0); BOOST_CHECK_EQUAL(render().count,0); // repeated high is not a tap
    cc(midi,20,0,0); BOOST_CHECK_EQUAL(render().state,int(LooperEngine::State::recording));
    render(); render();
    cc(midi,20,0); render();
    cc(midi,20,0,0); auto first=render();
    BOOST_REQUIRE_EQUAL(first.state,int(LooperEngine::State::playing));
    const auto firstLength=first.length;
    cc(midi,20,0); render(); cc(midi,20,0,0); auto next=render();
    BOOST_CHECK_EQUAL(next.count,2); BOOST_CHECK_EQUAL(next.length,firstLength);
    BOOST_CHECK(!next.layers[0].enabled);
    cc(midi,20,0); render(); cc(midi,20,0,0); render();
    cc(midi,20,0); render();
    for(int i=0;i<9;++i) render();
    // At exactly 0.8 s the release fires only the hold, not Record/end.
    cc(midi,20,0,0); auto fresh=render();
    BOOST_CHECK_EQUAL(fresh.count,1);
    BOOST_CHECK_EQUAL(fresh.state,int(LooperEngine::State::recording));
    BOOST_CHECK_LT(fresh.length,firstLength);
}

BOOST_AUTO_TEST_CASE(MidiRecordHoldIsLatchedAndCancelledByStopOrBypass)
{
    LooperProcessor p; p.prepareToPlay(100,8); p.setCrossfade(0); p.setCaptureTail(0);
    AudioBuffer<float> audio(2,8); MidiBuffer midi;
    const auto render=[&] { audio.clear(); p.processBlock(audio,midi); return p.view(); };
    cc(midi,20,0); render();
    for(int i=0;i<10;++i) render();
    auto fresh=p.view(); BOOST_REQUIRE_EQUAL(fresh.state,int(LooperEngine::State::recording));
    cc(midi,20,0); auto repeated=render();
    BOOST_CHECK_GT(repeated.length,fresh.length); // no repeated reset
    cc(midi,20,0,0); BOOST_CHECK_EQUAL(render().state,int(LooperEngine::State::recording));
    p.request(Action::stop); render();
    cc(midi,20,0); render(); p.request(Action::stop); render();
    cc(midi,20,0,0); BOOST_CHECK_EQUAL(render().state,int(LooperEngine::State::stopped));
    const auto count=p.view().count;
    p.setBypassed(true); render(); cc(midi,20,0); render();
    for(int i=0;i<11;++i) render();
    p.setBypassed(false); cc(midi,20,0,0); render();
    BOOST_CHECK_EQUAL(p.view().count,count);
    BOOST_CHECK_EQUAL(p.view().state,int(LooperEngine::State::stopped));
}


BOOST_AUTO_TEST_CASE(CaptureHandlesAndReplaceModeRoundtripV8AndRejectInvalidMetadata)
{
    LooperProcessor p; p.prepareToPlay(1000,32); p.setMonitor(false);
    p.setCrossfade(7); p.setCaptureTail(20);
    AudioBuffer<float> audio(2,32); MidiBuffer midi;
    const auto render=[&](float value,int frames=32) {
        audio.setSize(2,frames);
        for(int c=0;c<2;++c) for(int i=0;i<frames;++i) audio.setSample(c,i,value);
        p.processBlock(audio,midi);return p.view();
    };
    render(0.25f); // known prehistory
    p.request(Action::record);render(1);p.request(Action::play);render(0.5f);
    p.request(Action::replace);render(2,8);
    p.request(Action::endReplace);render(0.75f,5); // snapshot halfway through tail capture
    MemoryBlock partial;p.getStateInformation(partial);
    LooperProcessor partialRestore;partialRestore.setStateInformation(partial.getData(),int(partial.getSize()));
    BOOST_CHECK_EQUAL(partialRestore.getCrossfade(),7);
    BOOST_CHECK_EQUAL(partialRestore.getCaptureTail(),20);
    BOOST_REQUIRE_EQUAL(partialRestore.view().count,2);
    BOOST_CHECK(partialRestore.view().layers[1].replace);
    render(0.5f,32);
    MemoryBlock saved;p.getStateInformation(saved);
    LooperProcessor restored;restored.setStateInformation(saved.getData(),int(saved.getSize()));
    MemoryBlock roundtrip;restored.getStateInformation(roundtrip);
    BOOST_CHECK(saved==roundtrip); // coverage, raw stereo handles, mode and settings survive
    MemoryInputStream in(saved,false);in.setPosition(20);
    const int length=in.readInt(),count=in.readInt();
    const int metadata=76+count*(12+length*8)+16+4+20+count*8;
    const auto reject=[&](int offset,int value) {
        MemoryBlock malformed(saved);
        const auto little=ByteOrder::swapIfBigEndian(uint32(value));
        std::memcpy(static_cast<char*>(malformed.getData())+offset,&little,sizeof(little));
        restored.setStateInformation(malformed.getData(),int(malformed.getSize()));
        MemoryBlock unchanged;restored.getStateInformation(unchanged);BOOST_CHECK(unchanged==roundtrip);
    };
    reject(metadata,-1); // negative crossfade
    reject(metadata+4,2001); // tail beyond bounded allocation
    reject(metadata+8,2); // non-boolean Replace
    reject(metadata+8+8,length+1); // coverage beyond period
    reject(metadata+8+16,-1); // invalid prehistory length
    restored.setStateInformation(saved.getData(),int(saved.getSize())-1);
    MemoryBlock unchanged;restored.getStateInformation(unchanged);BOOST_CHECK(unchanged==roundtrip);
}

BOOST_AUTO_TEST_CASE(LegacyV7LayersMigrateToAddWithoutCaptureHandles)
{
    LooperProcessor p;p.prepareToPlay(100,8);p.setCrossfade(0);p.setCaptureTail(0);
    AudioBuffer<float> audio(2,8);MidiBuffer midi;audio.clear();
    p.request(Action::record);p.processBlock(audio,midi);p.request(Action::play);p.processBlock(audio,midi);
    MemoryBlock saved;p.getStateInformation(saved);
    MemoryInputStream in(saved,false);in.setPosition(20);const int length=in.readInt(),count=in.readInt();
    const int legacySize=76+count*(12+length*8)+16+4+20+count*8;
    saved.setSize(legacySize);static_cast<unsigned char*>(saved.getData())[4]=7;
    LooperProcessor migrated;migrated.setStateInformation(saved.getData(),int(saved.getSize()));
    BOOST_REQUIRE_EQUAL(migrated.view().count,1);BOOST_CHECK(!migrated.view().layers[0].replace);
    BOOST_CHECK_EQUAL(migrated.getCrossfade(),10);BOOST_CHECK_EQUAL(migrated.getCaptureTail(),10);
    BOOST_CHECK_EQUAL(migrated.view().length,8);
}


BOOST_AUTO_TEST_CASE(PartialReplaceCoverageAndCaptureHandlesResample441To48AndBack)
{
    LooperEngine::Snapshot saved;
    saved.rate=44100; saved.length=4410; saved.fixedLength=4410; saved.multiple=1; saved.phaseOffset=147;
    LooperEngine::SavedLayer layer; layer.info.replace=true;
    layer.coverageStart=2940; layer.coverageFrames=2205; layer.captureEnd=735; // wraps across the loop seam
    for(int c=0;c<2;++c) {
        layer.audio[c].resize(saved.length);
        layer.pre[c].resize(441); layer.post[c].resize(882);
        const float sign=c==0?1.0f:-1.0f;
        for(int i=0;i<saved.length;++i) layer.audio[c][i]=sign*std::sin(float(i)*0.02f);
        for(int i=0;i<441;++i) layer.pre[c][i]=sign*0.25f;
        for(int i=0;i<882;++i) layer.post[c][i]=sign*0.5f*(1.0f-float(i)/882);
    }
    saved.layers.push_back(layer);
    resampleLooperSnapshot(saved,48000);
    BOOST_CHECK_EQUAL(saved.length,4800);BOOST_CHECK_EQUAL(saved.fixedLength,4800);BOOST_CHECK_EQUAL(saved.phaseOffset,160);
    auto& converted=saved.layers[0];BOOST_CHECK(converted.info.replace);
    BOOST_CHECK_EQUAL(converted.coverageStart,3200);BOOST_CHECK_EQUAL(converted.coverageFrames,2400);
    BOOST_CHECK_EQUAL(converted.captureEnd,800);BOOST_REQUIRE_EQUAL(converted.pre[0].size(),480);BOOST_REQUIRE_EQUAL(converted.post[0].size(),960);
    for(const auto* audio : {&converted.audio,&converted.pre,&converted.post})
        for(size_t i=0;i<(*audio)[0].size();++i) {
            BOOST_CHECK(std::isfinite((*audio)[0][i]));BOOST_CHECK(std::isfinite((*audio)[1][i]));
            BOOST_CHECK_SMALL((*audio)[0][i]+(*audio)[1][i],1.0e-6f);
        }
    resampleLooperSnapshot(saved,44100);
    BOOST_CHECK_EQUAL(saved.length,4410);BOOST_CHECK_EQUAL(saved.fixedLength,4410);BOOST_CHECK_EQUAL(saved.phaseOffset,147);
    BOOST_CHECK_EQUAL(converted.coverageStart,2940);BOOST_CHECK_EQUAL(converted.coverageFrames,2205);
    BOOST_CHECK_EQUAL(converted.captureEnd,735);BOOST_CHECK_EQUAL(converted.pre[0].size(),441);BOOST_CHECK_EQUAL(converted.post[0].size(),882);
    for(const auto* audio : {&converted.audio,&converted.pre,&converted.post})
        for(const auto& channel:*audio) for(float sample:channel) BOOST_CHECK(std::isfinite(sample));
}

BOOST_AUTO_TEST_CASE(HeldReplaceCapturesAttackAtPhysicalPressRatherThanRecognition)
{
    LooperProcessor p;p.prepareToPlay(1000,2000);p.setCrossfade(0);p.setCaptureTail(0);p.setMonitor(false);
    AudioBuffer<float> base(2,2000);base.clear();MidiBuffer midi;
    p.request(Action::record);p.processBlock(base,midi);p.view();
    AudioBuffer<float> one(2,1);one.clear();p.request(Action::play);p.processBlock(one,midi);p.view();
    AudioBuffer<float> audio(2,1000);audio.clear();
    audio.setSample(0,5,.8f);audio.setSample(1,5,-.6f);
    midi.addEvent(MidiMessage::controllerEvent(1,23,127),5);
    midi.addEvent(MidiMessage::controllerEvent(1,23,0),900);
    p.processBlock(audio,midi);const auto view=p.view();
    BOOST_REQUIRE_EQUAL(view.count,2);BOOST_CHECK(view.layers[1].replace);
    BOOST_CHECK_EQUAL(view.state,int(LooperEngine::State::playing));
    MemoryBlock saved;p.getStateInformation(saved);MemoryInputStream in(saved,false);
    // Raw second-layer PCM, before any crossfade or playback gain.
    const int64_t secondAudio=76+(12+2000*8)+12;
    in.setPosition(secondAudio+6*4);BOOST_CHECK_EQUAL(in.readFloat(),.8f);
    in.setPosition(secondAudio+2000*4+6*4);BOOST_CHECK_EQUAL(in.readFloat(),-.6f);
    in.setPosition(secondAudio+806*4);BOOST_CHECK_EQUAL(in.readFloat(),0);
}

BOOST_AUTO_TEST_CASE(HeldReplaceSnapshotMaterializesUncopiedHistory)
{
    LooperProcessor p;p.prepareToPlay(1000,2000);p.setCrossfade(0);p.setCaptureTail(0);p.setMonitor(false);
    AudioBuffer<float> base(2,2000);base.clear();MidiBuffer midi;
    p.request(Action::record);p.processBlock(base,midi);p.view();
    AudioBuffer<float> one(2,1);one.clear();p.request(Action::play);p.processBlock(one,midi);p.view();
    AudioBuffer<float> audio(2,810);audio.clear();
    audio.setSample(0,5,.8f);audio.setSample(1,700,-.4f);
    midi.addEvent(MidiMessage::controllerEvent(1,23,127),5);
    p.processBlock(audio,midi);const auto view=p.view();
    BOOST_REQUIRE_EQUAL(view.count,2);BOOST_CHECK_EQUAL(view.state,int(LooperEngine::State::replacing));
    MemoryBlock saved;p.getStateInformation(saved);MemoryInputStream in(saved,false);
    const int64_t secondAudio=76+(12+2000*8)+12;
    in.setPosition(secondAudio+6*4);BOOST_CHECK_EQUAL(in.readFloat(),.8f);
    in.setPosition(secondAudio+2000*4+701*4);BOOST_CHECK_EQUAL(in.readFloat(),-.4f);
    LooperProcessor restored;restored.setStateInformation(saved.getData(),int(saved.getSize()));
    BOOST_REQUIRE_EQUAL(restored.view().count,2);
}

BOOST_AUTO_TEST_CASE(FeedbackEffectiveVolumeViewRestoreAndCaptureLock)
{
    LooperProcessor p;p.prepareToPlay(1000,100);p.setCrossfade(0);p.setCaptureTail(0);
    p.setQuantized(false);p.setFeedback(0.25f);
    AudioBuffer<float> audio(2,100);MidiBuffer midi;
    auto tick=[&]{audio.clear();p.processBlock(audio,midi);p.view();};
    p.request(Action::record);for(int i=0;i<10;++i)tick();
    p.request(Action::play);tick();auto id=p.view().layers[0].id;
    p.request(Action::overdub);for(int i=0;i<5;++i)tick();
    BOOST_CHECK_CLOSE(p.view().layers[0].gain,0.5f,0.001f);
    p.editLayer(id,2,1.5f);tick();
    BOOST_CHECK_LT(p.view().layers[0].gain,0.5f);
    p.request(Action::overdub);tick();
    MemoryBlock saved;p.getStateInformation(saved);
    LooperProcessor q;q.setStateInformation(saved.getData(),int(saved.getSize()));
    BOOST_CHECK_CLOSE(q.view().layers[0].gain,p.view().layers[0].gain,0.001f);
    p.editLayer(id,2,1);tick();BOOST_CHECK_EQUAL(p.view().layers[0].gain,1);
    BOOST_CHECK_EQUAL(p.view().layers[0].feedbackGain,1);
}
BOOST_AUTO_TEST_CASE(OneTakeV9RoundtripIncludesGroupsIgnoreFeedbackAndOptions)
{
    LooperProcessor p;p.prepareToPlay(1000,100);p.setQuantized(false);p.setCrossfade(0);p.setCaptureTail(0);
    p.setTakeFades(false);p.setTakeFeedback(false);
    AudioBuffer<float> a(2,100);MidiBuffer m;auto tick=[&]{a.clear();p.processBlock(a,m);p.view();};
    p.request(Action::record);tick();p.request(Action::play);tick();
    p.editLayer(p.view().layers[0].id,6,1);p.request(Action::overdub);tick();tick();
    p.request(Action::overdub);tick();BOOST_REQUIRE_EQUAL(p.view().count,3);
    MemoryBlock b;p.getStateInformation(b);LooperProcessor q;q.setStateInformation(b.getData(),int(b.getSize()));
    auto v=q.view();BOOST_REQUIRE_EQUAL(v.count,3);BOOST_CHECK(v.layers[0].ignoreFeedback);
    BOOST_CHECK_NE(v.layers[1].take,0);BOOST_CHECK_EQUAL(v.layers[1].take,v.layers[2].take);
    BOOST_CHECK_EQUAL(v.layers[2].part,1);BOOST_CHECK(v.layers[1].continued);
    BOOST_CHECK(!q.getTakeFades());BOOST_CHECK(!q.getTakeFeedback());
    // The old v8 payload remains readable after removing only the v9 trailer.
    b.removeSection(b.getSize()-(112+3*16),112+3*16);static_cast<unsigned char*>(b.getData())[4]=8;
    LooperProcessor old;old.setStateInformation(b.getData(),int(b.getSize()));
    BOOST_REQUIRE_EQUAL(old.view().count,3);BOOST_CHECK_EQUAL(old.view().layers[1].take,0);
    BOOST_CHECK(old.getTakeFades());BOOST_CHECK(old.getTakeFeedback());
}
BOOST_AUTO_TEST_CASE(CapacityV10DefaultsRoundtripAndCaptureGuard)
{
    LooperProcessor p;BOOST_CHECK_EQUAL(p.getRecordingLimit(),10);BOOST_CHECK_EQUAL(p.getTakeLimit(),4);BOOST_CHECK(p.getRolling());
    BOOST_REQUIRE(p.setCapacityLimits(3,2,true));p.prepareToPlay(100,10);p.setCrossfade(0);p.setCaptureTail(0);
    AudioBuffer<float> a(2,10);MidiBuffer m;auto tick=[&]{a.clear();p.processBlock(a,m);p.view();};
    p.request(Action::record);tick();BOOST_CHECK(!p.setCapacityLimits(5,5,false));
    p.request(Action::play);tick();p.request(Action::overdub);
    for(int i=0;i<180;++i)tick();p.request(Action::stop);tick();
    BOOST_REQUIRE_EQUAL(p.view().count,3);BOOST_CHECK_GT(p.view().layers[1].part,160);
    MemoryBlock b;p.getStateInformation(b);LooperProcessor q;q.setStateInformation(b.getData(),int(b.getSize()));
    BOOST_REQUIRE_EQUAL(q.view().count,3);BOOST_CHECK_EQUAL(q.getRecordingLimit(),3);BOOST_CHECK_EQUAL(q.getTakeLimit(),2);BOOST_CHECK(q.getRolling());
    BOOST_CHECK_EQUAL(q.view().layers[1].part,p.view().layers[1].part);
    b.removeSection(b.getSize()-104,104);static_cast<unsigned char*>(b.getData())[4]=9;
    LooperProcessor old;old.setStateInformation(b.getData(),int(b.getSize()));
    BOOST_REQUIRE_EQUAL(old.view().count,3);BOOST_CHECK_EQUAL(old.getRecordingLimit(),10);BOOST_CHECK_EQUAL(old.getTakeLimit(),4);
}

BOOST_AUTO_TEST_CASE(MemoryDurationV11LongLoopsRoundtripRateChangeAndV10Defaults)
{
    LooperProcessor p;BOOST_REQUIRE(p.setMemoryLimits(90,1,1,false));p.prepareToPlay(10,10);
    p.setCrossfade(0);p.setCaptureTail(0);
    AudioBuffer<float> a(2,10);MidiBuffer m;auto tick=[&]{a.clear();p.processBlock(a,m);p.view();};
    p.request(Action::record);tick();BOOST_CHECK(!p.setMemoryLimits(100,1,1,false));
    for(int i=1;i<70;++i)tick();p.request(Action::stop);tick();
    BOOST_CHECK_EQUAL(p.view().length,700);BOOST_CHECK(!p.setMemoryLimits(60,1,1,false));
    MemoryBlock b;p.getStateInformation(b);LooperProcessor q;q.prepareToPlay(20,10);q.setStateInformation(b.getData(),int(b.getSize()));
    BOOST_CHECK_EQUAL(q.getDurationSeconds(),90);BOOST_CHECK_EQUAL(q.view().length,1400);
    q.prepareToPlay(30,10);BOOST_CHECK_EQUAL(q.view().length,2100);
    // Empty old v10 sessions retain the historical 60s default.
    LooperProcessor empty;empty.prepareToPlay(10,10);empty.getStateInformation(b);
    b.removeSection(b.getSize()-92,92);static_cast<unsigned char*>(b.getData())[4]=10;
    q.setStateInformation(b.getData(),int(b.getSize()));BOOST_CHECK_EQUAL(q.getDurationSeconds(),60);
    BOOST_CHECK_EQUAL(q.view().count,0);
}

BOOST_AUTO_TEST_CASE(MemoryResizeKeepsLiveMonitoringDuringConfiguration)
{
    LooperProcessor p;p.prepareToPlay(1000,64);p.setMonitor(true);
    std::atomic<bool> run{true},ready{false};std::atomic<int> bad{0},blocks{0};
    std::thread audioThread([&]{AudioBuffer<float> a(2,64);MidiBuffer m;ready.store(true);
        while(run.load()){
            for(int c=0;c<2;++c)for(int i=0;i<64;++i)a.setSample(c,i,0.125f);
            p.processBlock(a,m);++blocks;
            for(int c=0;c<2;++c)for(int i=0;i<64;++i)if(a.getSample(c,i)!=0.125f)++bad;
        }});
    while(!ready.load())std::this_thread::yield();
    const auto before=p.getMemoryBytes();
    const bool resized=p.setMemoryLimits(2,2,2,true);
    run.store(false);audioThread.join();
    BOOST_CHECK(resized);BOOST_CHECK_LT(p.getMemoryBytes(),before/5);
    BOOST_CHECK_GT(blocks.load(),0);BOOST_CHECK_EQUAL(bad.load(),0);
}

BOOST_AUTO_TEST_CASE(LoopModesAndIndependentStartChoicesRoundtripV12)
{
    LooperProcessor p;p.prepareToPlay(100,10);p.setLoopMode(2);p.setFixedBarCount(7);
    p.setRecordNextBar(true);p.setOverdubNextBar(false);
    MemoryBlock b;p.getStateInformation(b);LooperProcessor q;q.setStateInformation(b.getData(),int(b.getSize()));
    BOOST_CHECK_EQUAL(q.getLoopMode(),2);BOOST_CHECK_EQUAL(q.getFixedBarCount(),7);
    BOOST_CHECK(q.getRecordNextBar());BOOST_CHECK(!q.getOverdubNextBar());
    b.removeSection(b.getSize()-88,88);static_cast<unsigned char*>(b.getData())[4]=11;
    q.setStateInformation(b.getData(),int(b.getSize()));BOOST_CHECK_EQUAL(q.getLoopMode(),1);
    BOOST_CHECK(q.getOverdubNextBar());BOOST_CHECK_EQUAL(q.getFixedBarCount(),4);
}
BOOST_AUTO_TEST_CASE(PerformanceTransportPreservesPhaseAndCancelsPending)
{
    LooperEngine e;e.prepare(100);e.setTransitionOptions(0,0);
    e.setOptions(true,100,0);e.setClock(60,4,4,0,true);e.setFixedBars(1);
    e.setPerformanceOptions(true,false,false,false,false,2,.1f);
    e.apply(Action::record);for(int i=0;i<400;++i)frame(e,.2f,.2f);
    BOOST_REQUIRE_EQUAL(e.getLength(),400);BOOST_REQUIRE(e.getState()==LooperEngine::State::playing);
    for(int i=0;i<73;++i)frame(e,0,0);
    e.applyPerformance(Action::stop);for(int i=0;i<51;++i)frame(e,0,0);
    BOOST_CHECK_EQUAL(e.getSyncPosition(),124);
    e.applyPerformance(Action::play);BOOST_CHECK_EQUAL(e.getPosition(),124);
    e.setPerformanceOptions(true,true,true,false,false,2,.1f);
    e.applyPerformance(Action::playStop);BOOST_REQUIRE(e.isStartPending());
    e.applyPerformance(Action::playStop);BOOST_CHECK(!e.isStartPending());BOOST_CHECK(e.getState()==LooperEngine::State::playing);
    e.applyPerformance(Action::stop);
    for(int i=0;i<276;++i)frame(e,0,0);
    BOOST_CHECK(e.getState()==LooperEngine::State::playing);
    frame(e,0,0);BOOST_CHECK(e.getState()==LooperEngine::State::stopped);
    e.applyPerformance(Action::play);e.applyPerformance(Action::playStop);
    BOOST_CHECK(!e.isStartPending());BOOST_CHECK(e.getState()==LooperEngine::State::stopped);
    e.setPerformanceOptions(false,false,false,false,false,2,.1f);
    e.applyPerformance(Action::play);BOOST_CHECK_EQUAL(e.getPosition(),0);
}
BOOST_AUTO_TEST_CASE(PerformanceCountInFullBarsCancelAndClockLoss)
{
    LooperEngine e;e.prepare(100);e.setOptions(true,100,0);e.setClock(60,3,4,1,true);
    e.setPerformanceOptions(true,false,false,true,true,2,.1f);
    e.applyPerformance(Action::record);BOOST_REQUIRE(e.isStartPending());
    for(int i=0;i<200;++i){BOOST_CHECK(!e.countInAudible());frame(e,0,0);}
    BOOST_CHECK(e.countInAudible());
    for(int i=0;i<600;++i){BOOST_CHECK(e.countInAudible());frame(e,.3f,.3f);}
    BOOST_CHECK(!e.countInAudible());BOOST_CHECK(e.getState()==LooperEngine::State::empty);
    frame(e,.7f,.7f);BOOST_REQUIRE(e.getState()==LooperEngine::State::recording);
    BOOST_CHECK_EQUAL(e.getLength(),1);BOOST_CHECK_CLOSE(e.snapshot().layers[0].audio[0][0],.7f,.001);
    e.apply(Action::clear);e.applyPerformance(Action::overdub);e.applyPerformance(Action::overdub);
    BOOST_CHECK(!e.isStartPending());BOOST_CHECK_EQUAL(e.getLength(),0);
    e.applyPerformance(Action::record);e.setClock(60,3,4,0,false);
    BOOST_CHECK(!e.isStartPending());
}
BOOST_AUTO_TEST_CASE(PerformanceFreeSoundTriggerPreservesAttackAndBacking)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(0,0);
    e.setPerformanceOptions(true,true,true,true,true,2,.5f);
    e.applyPerformance(Action::record);
    for(int i=1;i<=5;++i)frame(e,.05f*i,.05f*i);
    BOOST_REQUIRE(e.waitingForSound());frame(e,.8f,.8f);
    BOOST_REQUIRE(e.getState()==LooperEngine::State::recording);BOOST_CHECK_EQUAL(e.getLength(),6);
    auto snap=e.snapshot();BOOST_CHECK_CLOSE(snap.layers[0].audio[0][0],.05f,.001);
    BOOST_CHECK_CLOSE(snap.layers[0].audio[0][5],.8f,.001);
    for(int i=0;i<100;++i)frame(e,.2f,.2f);
    e.apply(Action::play);const int length=e.getLength();
    e.applyPerformance(Action::overdub);for(int i=0;i<20;++i)frame(e,0,0);
    BOOST_CHECK(e.waitingForSound());BOOST_CHECK(e.getState()==LooperEngine::State::playing);
    e.applyPerformance(Action::overdub);BOOST_CHECK(!e.isStartPending());BOOST_CHECK_EQUAL(e.getLayerCount(),1);
    e.applyPerformance(Action::overdub);frame(e,.8f,.8f);
    BOOST_CHECK(e.getState()==LooperEngine::State::overdubbing);BOOST_CHECK_EQUAL(e.getLength(),length);BOOST_CHECK_EQUAL(e.getLayerCount(),2);
}
BOOST_AUTO_TEST_CASE(PerformanceSettingsRoundtripAndInvalidPayload)
{
    LooperProcessor p;p.prepareToPlay(100,16);p.setLoopMode(1);p.preserveTimeline=false;p.playOnBar=true;p.stopOnBar=true;
    p.recordPreparation=true;p.overdubPreparation=true;p.countInClick=true;p.countInBars=3;p.triggerDb=-27;
    MemoryBlock b;p.getStateInformation(b);LooperProcessor q;q.setStateInformation(b.getData(),int(b.getSize()));
    BOOST_CHECK(!q.preserveTimeline.load());BOOST_CHECK(q.playOnBar.load());BOOST_CHECK(q.stopOnBar.load());
    BOOST_CHECK(q.recordPreparation.load());BOOST_CHECK(q.overdubPreparation.load());BOOST_CHECK(q.countInClick.load());
    BOOST_CHECK_EQUAL(q.countInBars.load(),3);BOOST_CHECK_EQUAL(q.triggerDb.load(),-27);
    q.setStateInformation(b.getData(),int(b.getSize())-1);BOOST_CHECK_EQUAL(q.countInBars.load(),3);
    b.removeSection(b.getSize()-76,76);static_cast<unsigned char*>(b.getData())[4]=12;
    q.setStateInformation(b.getData(),int(b.getSize()));BOOST_CHECK(q.preserveTimeline.load());BOOST_CHECK(!q.recordPreparation.load());
}
BOOST_AUTO_TEST_CASE(CountInGateMultipleSourcesEdgesAndDisconnect)
{
    CountInGate gate;gate.prepareToPlay(1000,16);AudioBuffer<float> audio(2,16);MidiBuffer midi;
    auto signal=[&](int id,int at,bool on){uint8_t b[10]{0xf0,0x7d,'J','C',uint8_t(id),0,0,0,uint8_t(on),0xf7};midi.addEvent(b,10,at);};
    for(int c=0;c<2;++c)for(int i=0;i<16;++i)audio.setSample(c,i,1);
    signal(1,0,false);signal(2,0,false);signal(1,2,true);signal(2,4,true);signal(1,6,false);signal(2,12,false);
    gate.processBlock(audio,midi);BOOST_CHECK_EQUAL(audio.getSample(0,0),0);BOOST_CHECK_EQUAL(audio.getSample(0,3),1);
    BOOST_CHECK_EQUAL(audio.getSample(0,7),1);BOOST_CHECK_EQUAL(audio.getSample(0,13),0);BOOST_CHECK(midi.isEmpty());
    signal(1,0,true);for(int c=0;c<2;++c)for(int i=0;i<16;++i)audio.setSample(c,i,1);gate.processBlock(audio,midi);
    BOOST_CHECK_EQUAL(audio.getSample(0,15),1);
    for(int c=0;c<2;++c)for(int i=0;i<16;++i)audio.setSample(c,i,1);gate.processBlock(audio,midi);
    BOOST_CHECK_EQUAL(audio.getSample(0,1),0); // no heartbeat = removed/suspended source
}
BOOST_AUTO_TEST_CASE(CountInMidiOutputIsSampleTimedAndNeverRecorded)
{
    struct Clock:AudioPlayHead {
        double ppq=.5;
        Optional<PositionInfo> getPosition() const override {
            PositionInfo p;p.setBpm(60);p.setPpqPosition(ppq);p.setIsPlaying(true);
            p.setTimeSignature(TimeSignature{4,4});return p;
        }
    } clock;
    LooperProcessor p;p.prepareToPlay(100,64);p.setPlayHead(&clock);p.setLoopMode(1);
    p.recordPreparation=true;p.countInClick=true;p.countInBars=2;p.setCrossfade(0);p.setCaptureTail(0);
    CountInGate gate;gate.prepareToPlay(100,64);
    BOOST_CHECK(p.producesMidi());BOOST_REQUIRE(p.request(Action::record));
    AudioBuffer<float> guitar(2,64),click(2,64);MidiBuffer control;
    for(int block=0;block<20;++block){
        clock.ppq=(50+block*64)/100.0;
        for(int c=0;c<2;++c)for(int i=0;i<64;++i){guitar.setSample(c,i,.25f);click.setSample(c,i,1);}
        p.processBlock(guitar,control);p.view();
        BOOST_CHECK(!control.isEmpty());gate.processBlock(click,control);
        for(int i=0;i<64;++i){const int time=block*64+i;
            BOOST_CHECK_EQUAL(click.getSample(0,i),time>=350 && time<1150?1.0f:0.0f);
        }
    }
    BOOST_CHECK_EQUAL(p.view().state,int(LooperEngine::State::recording));
    MemoryBlock saved;p.getStateInformation(saved);LooperProcessor restored;restored.setStateInformation(saved.getData(),int(saved.getSize()));
    BOOST_CHECK_EQUAL(restored.view().length,130);
    // TICK audio exists only downstream of the Gate, never in the recording input.
    restored.setLoopMode(0);restored.setMonitor(false);restored.request(Action::play);
    guitar.clear();restored.processBlock(guitar,control);
    BOOST_CHECK_CLOSE(guitar.getSample(0,10),.25f,.001);
}
BOOST_AUTO_TEST_SUITE_END()
