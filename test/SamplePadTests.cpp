// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include <boost/test/unit_test.hpp>
#include "nodes/samplepad.hpp"
#include "nodes/samplepadbank.hpp"
#include "binarydata.hpp"
#include <cmath>
#include <thread>
using namespace element;
using namespace juce;
namespace {
struct Pad {
    SamplePadNode p;
    AudioBuffer<float> out { 2, 16 };
    MidiBuffer midi;
    Pad()
    {
        p.prepareToRender (48000, 16);
        p.mapping.store (29);
    }
    void sound (int frames = 32, double rate = 48000)
    {
        AudioBuffer<float> a (1, frames);
        for (int i = 0; i < frames; ++i)
            a.setSample (0, i, 1);
        BOOST_REQUIRE (p.loadAudio (a, rate, "Test kick").wasOk());
    }
    void run() { p.process (out, midi); }
};
} // namespace
BOOST_AUTO_TEST_SUITE (SamplePadTests)
BOOST_AUTO_TEST_CASE (PressTimingReleaseAndRepeatedCC)
{
    Pad t;
    t.sound();
    t.midi.addEvent (MidiMessage::controllerEvent (1, 29, 127), 3);
    t.midi.addEvent (MidiMessage::controllerEvent (1, 29, 127), 5);
    t.midi.addEvent (MidiMessage::controllerEvent (1, 29, 0), 7);
    t.run();
    BOOST_CHECK_EQUAL (t.out.getSample (0, 2), 0);
    BOOST_CHECK_EQUAL (t.out.getSample (0, 3), 1);
    BOOST_CHECK_EQUAL (t.out.getSample (0, 8), 1);
    BOOST_CHECK_EQUAL (t.out.getSample (1, 8), 1);
    t.midi.addEvent (MidiMessage::controllerEvent (1, 29, 127), 0);
    t.run();
    BOOST_CHECK_EQUAL (t.out.getSample (0, 0), 2);
}
BOOST_AUTO_TEST_CASE (SourceRateAndOverlap)
{
    Pad t;
    t.sound (8, 24000);
    t.p.trigger();
    t.run();
    BOOST_CHECK_GT (t.out.getSample (0, 15), .3f);
    t.run();
    BOOST_CHECK_EQUAL (t.out.getSample (0, 0), 0);
}
BOOST_AUTO_TEST_CASE (NoteVelocityAndLearnDoesNotTrigger)
{
    Pad t;
    t.sound();
    t.p.learn.store (true);
    t.midi.addEvent (MidiMessage::noteOn (2, 36, uint8 (64)), 0);
    t.run();
    BOOST_CHECK_EQUAL (t.out.getMagnitude (0, 16), 0);
    BOOST_CHECK_EQUAL (t.p.kind.load(), 1);
    BOOST_CHECK_EQUAL (t.p.mapping.load(), 36);
    BOOST_CHECK_EQUAL (t.p.channel.load(), 2);
    t.midi.addEvent (MidiMessage::noteOn (2, 36, uint8 (64)), 2);
    t.midi.addEvent (MidiMessage::noteOff (2, 36), 4);
    t.run();
    BOOST_CHECK_CLOSE (t.out.getSample (0, 5), 64.f / 127.f, .01);
}
BOOST_AUTO_TEST_CASE (PersistenceFailureAndBypass)
{
    Pad t;
    t.sound();
    t.p.gain.store (.5);
    MemoryBlock state;
    t.p.getState (state);
    SamplePadNode restored;
    restored.prepareToRender (48000, 16);
    restored.setState (state.getData(), int (state.getSize()));
    restored.trigger();
    restored.process (t.out, t.midi);
    BOOST_CHECK_EQUAL (t.out.getSample (0, 0), .5);
    BOOST_CHECK_EQUAL (restored.sampleName(), "Test kick");
    BOOST_CHECK (restored.loadFile (File ("/does-not-exist.wav")).failed());
    restored.process (t.out, t.midi, true);
    BOOST_CHECK_EQUAL (t.out.getMagnitude (0, 16), 0);
    restored.process (t.out, t.midi);
    BOOST_CHECK_EQUAL (t.out.getMagnitude (0, 16), 0);
}
BOOST_AUTO_TEST_CASE (VoiceBoundAndSoundOff)
{
    Pad t;
    t.sound();
    for (int i = 0; i < 20; ++i) {
        t.midi.addEvent (MidiMessage::controllerEvent (1, 29, 127), 0);
        t.midi.addEvent (MidiMessage::controllerEvent (1, 29, 0), 0);
    }
    t.run();
    BOOST_CHECK_EQUAL (t.out.getSample (0, 0), 16);
    t.midi.addEvent (MidiMessage::allSoundOff (1), 5);
    t.run();
    BOOST_CHECK_EQUAL (t.out.getSample (0, 4), 16);
    BOOST_CHECK_EQUAL (t.out.getSample (0, 5), 0);
}
BOOST_AUTO_TEST_CASE (SampleReplacementAcknowledgment)
{
    Pad t;
    t.sound();
    AudioBuffer<float> a (1, 32);
    a.clear();
    BOOST_CHECK (t.p.loadAudio (a, 48000, "New").wasOk());
    t.run();
    BOOST_CHECK (t.p.loadAudio (a, 48000, "New").wasOk());
    t.p.trigger();
    t.run();
    BOOST_CHECK_EQUAL (t.out.getMagnitude (0, 16), 0);
    BOOST_CHECK_EQUAL (t.p.sampleName(), "New");
}
BOOST_AUTO_TEST_CASE (FactoryBankAndOfflineSelection)
{
    SamplePadNode p;
    for (const auto& entry : samplePadBank) {
        int bytes = 0;
        const auto* data = res::getNamedResource (entry.resource, bytes);
        BOOST_REQUIRE (data);
        BOOST_REQUIRE (p.loadSampleData (data, size_t (bytes), entry.name).wasOk());
        BOOST_CHECK_EQUAL (p.sampleName(), entry.name);
    }
}
BOOST_AUTO_TEST_CASE (EmptyStateClearsSoundAndMalformedPreservesIt)
{
    Pad t;
    t.sound();
    SamplePadNode empty;
    MemoryBlock state;
    empty.getState (state);
    t.p.setState (state.getData(), int (state.getSize()));
    t.p.trigger();
    t.run();
    BOOST_CHECK_EQUAL (t.out.getMagnitude (0, 16), 0);
    BOOST_CHECK_EQUAL (t.p.sampleName(), "No sound loaded");
    t.sound();
    const char invalid[4] = { 0, 0, 0, 0 };
    t.p.setState (invalid, 4);
    BOOST_CHECK_EQUAL (t.p.sampleName(), "Test kick");
}
BOOST_AUTO_TEST_CASE (RateChangeAndAntiAlias)
{
    SamplePadNode p;
    p.prepareToRender (48000, 480);
    AudioBuffer<float> source (1, 9600);
    for (int i = 0; i < 9600; ++i)
        source.setSample (0, i, float (std::sin (2 * 3.141592653589793 * 30000 * i / 96000)));
    BOOST_REQUIRE (p.loadAudio (source, 96000, "Ultrasonic").wasOk());
    AudioBuffer<float> out (2, 480);
    MidiBuffer midi;
    p.trigger();
    p.process (out, midi);
    p.process (out, midi);
    BOOST_CHECK_LT (out.getRMSLevel (0, 0, 480), .005f);
    p.prepareToRender (192000, 480);
    p.trigger();
    int blocks = 0;
    for (; blocks < 45; ++blocks) {
        p.process (out, midi);
        if (blocks == 39)
            BOOST_CHECK_GT (out.getMagnitude (0, 480), .5f);
        if (blocks >= 40)
            BOOST_CHECK_EQUAL (out.getMagnitude (0, 480), 0);
    }
}
BOOST_AUTO_TEST_CASE (SectionAllNotesOffIsSampleAccurate)
{
    Pad t;
    t.sound();
    t.p.trigger();
    t.midi.addEvent (MidiMessage::allNotesOff (1), 7);
    t.run();
    BOOST_CHECK_EQUAL (t.out.getSample (0, 6), 1);
    BOOST_CHECK_EQUAL (t.out.getSample (0, 7), 0);
    t.run();
    BOOST_CHECK_EQUAL (t.out.getMagnitude (0, 16), 0);
}
BOOST_AUTO_TEST_CASE (ConcurrentSoundReplacementAndEmptyRestore)
{
    SamplePadNode pad;
    pad.prepareToRender (48000, 64);
    AudioBuffer<float> source (1, 256);
    source.clear();
    for (int i = 0; i < 256; ++i)
        source.setSample (0, i, .2f);
    SamplePadNode empty;
    MemoryBlock emptyState;
    empty.getState (emptyState);
    std::atomic<bool> done { false }, failed { false };
    std::thread writer ([&] {for(int i=0;i<1000;++i){if(pad.loadAudio(source,48000,"Concurrent").failed())failed.store(true);if(i%7==0)pad.setState(emptyState.getData(),int(emptyState.getSize()));}done.store(true); });
    AudioBuffer<float> out (2, 64);
    MidiBuffer midi;
    while (! done.load()) {
        pad.trigger();
        pad.process (out, midi);
        for (int i = 0; i < 64; ++i)
            if (! std::isfinite (out.getSample (0, i)))
                failed.store (true);
    }
    writer.join();
    BOOST_CHECK (! failed.load());
}
BOOST_AUTO_TEST_CASE (TruncatedStateCannotChangeSettings)
{
    Pad t;
    t.sound();
    t.p.gain.store (.7f);
    MemoryBlock state;
    t.p.getState (state);
    for (int bytes = 0; bytes < 41; ++bytes) {
        t.p.setState (state.getData(), bytes);
        BOOST_CHECK_EQUAL (t.p.gain.load(), .7f);
        BOOST_CHECK_EQUAL (t.p.mapping.load(), 29);
        BOOST_CHECK_EQUAL (t.p.sampleName(), "Test kick");
    }
}
BOOST_AUTO_TEST_SUITE_END()
