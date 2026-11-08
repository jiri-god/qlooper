// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <element/juce.hpp>
#include "boundedmidi.hpp"
namespace element {
/** Admission is bounded before JUCE's timestamp queue, including keyboard input.
    The queue lock already exists in JUCE; no allocation or destruction is added
    to its audio-side drain. */
class BoundedMidiCollector final : public juce::MidiKeyboardState::Listener {
public:
    void reset(double rate) {
        const juce::ScopedLock guard(lock);collector.reset(rate);
        collector.ensureStorageAllocated(BoundedMidi::storage);
        scratch.ensureSize(BoundedMidi::storage);used=0;
    }
    void addMessageToQueue(const juce::MidiMessage& m) {
        const juce::ScopedLock guard(lock);
        if(!BoundedMidi::fits(used,m.getRawData(),m.getRawDataSize()))return;
        used+=m.getRawDataSize()+6;collector.addMessageToQueue(m);
    }
    void removeNextBlockOfMessages(juce::MidiBuffer& out,int frames) {
        const juce::ScopedLock guard(lock);scratch.clear();
        collector.removeNextBlockOfMessages(scratch,frames);used=0;
        BoundedMidi::append(out,scratch,0,frames,0);
    }
    void handleNoteOn(juce::MidiKeyboardState*,int ch,int note,float velocity) override {
        auto m=juce::MidiMessage::noteOn(ch,note,velocity);stamp(m);
    }
    void handleNoteOff(juce::MidiKeyboardState*,int ch,int note,float velocity) override {
        auto m=juce::MidiMessage::noteOff(ch,note,velocity);stamp(m);
    }
private:
    void stamp(juce::MidiMessage& m){m.setTimeStamp(juce::Time::getMillisecondCounterHiRes()*.001);addMessageToQueue(m);}
    juce::CriticalSection lock;juce::MidiMessageCollector collector;juce::MidiBuffer scratch;int used=0;
};
}
