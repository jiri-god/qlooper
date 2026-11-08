// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include <boost/test/unit_test.hpp>
#include "fixture/PreparedGraph.h"
#include "engine/ionode.hpp"
#include "nodes/audioprocessor.hpp"
#include "nodes/loopermanager.hpp"
#include <element/midipipe.hpp>
#include <element/node.h>
using namespace element;
using namespace juce;
namespace {
struct Rig {
    PreparedGraph fixture {100, 8};
    GraphNode& g = fixture.graph;
    LooperManagerNode* manager = new LooperManagerNode();
    LooperProcessor *a = new LooperProcessor(), *b = new LooperProcessor();
    Processor *input, *m, *an, *bn;
    AudioBuffer<float> audio {2,8}; MidiBuffer midi;
    Rig() {
        input = g.addNode (new IONode (IONode::midiInputNode));
        m = g.addNode (manager); an = g.addNode (new AudioProcessorNode(a)); bn = g.addNode (new AudioProcessorNode(b));
        BOOST_REQUIRE (g.connectChannels (PortType::Midi,input->nodeId,0,m->nodeId,0));
        BOOST_REQUIRE (g.connectChannels (PortType::Midi,m->nodeId,0,an->nodeId,0));
        BOOST_REQUIRE (g.connectChannels (PortType::Midi,m->nodeId,0,bn->nodeId,0));
        g.rebuild(); a->view(); b->view();
    }
    void press (int cc, int at=0) { midi.addEvent (MidiMessage::controllerEvent (1,cc,127),at); midi.addEvent (MidiMessage::controllerEvent(1,cc,0),at+1); }
    void render() { audio.clear(); RenderContext rc(audio,audio,midi,8); g.render(rc); a->view(); b->view(); midi.clear(); }
};
}
BOOST_AUTO_TEST_SUITE (LooperManagerTests)
BOOST_AUTO_TEST_CASE (RecordedMasterSelectionPreservesAudioAndRejectsIncompatibleLinks)
{
    Rig r;
    r.a->request(LooperEngine::Action::record);r.render();r.a->request(LooperEngine::Action::record);r.render();
    BOOST_CHECK(!r.manager->setMaster(r.an->nodeId)); // playing
    r.a->request(LooperEngine::Action::stop);r.render();
    const auto length=r.a->view().length; const auto count=r.a->view().count;
    BOOST_REQUIRE(r.manager->setMaster(r.an->nodeId));r.render();r.render();
    BOOST_CHECK_EQUAL(r.manager->masterLength.load(),length);
    BOOST_CHECK_EQUAL(r.a->view().length,length);BOOST_CHECK_EQUAL(r.a->view().count,count);
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::stopped));
    for(int i=0;i<50;++i)r.render();
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::stopped));
    BOOST_CHECK(!r.manager->transportRunning.load());
    BOOST_CHECK(!r.a->transportStartRequested.load());
    BOOST_REQUIRE(r.manager->setSyncMode(r.bn->nodeId,1));
    r.b->request(LooperEngine::Action::record);r.render();r.b->request(LooperEngine::Action::record);r.render();
    r.b->request(LooperEngine::Action::stop);r.render();
    BOOST_REQUIRE(r.manager->setMaster(r.bn->nodeId));r.render();r.render();
    BOOST_CHECK_EQUAL(r.b->view().length,length);BOOST_CHECK_EQUAL(r.a->view().length,length);
    BOOST_CHECK_EQUAL(r.manager->syncMode(r.bn->nodeId),0);
}
BOOST_AUTO_TEST_CASE (MasterAssignmentDoesNotAnimateIdleLooperDisplay)
{
    Rig r;
    r.a->request(LooperEngine::Action::record);r.render();r.render();r.render();
    r.a->request(LooperEngine::Action::record);r.render();
    r.manager->stopAll();r.render();r.render();
    BOOST_REQUIRE(r.manager->setMaster(r.an->nodeId));r.render();r.render();
    const auto idle=r.a->view();bool internalAdvanced=false;
    for(int i=0;i<7;++i){r.render();auto v=r.a->view();
        BOOST_CHECK_EQUAL(v.displaySyncPosition,idle.displaySyncPosition);
        BOOST_CHECK_EQUAL(v.displayMasterPosition,idle.displayMasterPosition);
        BOOST_CHECK_EQUAL(v.state,int(LooperEngine::State::stopped));
        internalAdvanced|=v.masterPosition!=idle.masterPosition;
    }
    BOOST_CHECK(internalAdvanced); // presentation gating must not freeze the sync engine
    r.a->request(LooperEngine::Action::play);r.render();r.render();
    auto moving=r.a->view();r.render();
    BOOST_CHECK_NE(r.a->view().displayMasterPosition,moving.displayMasterPosition);
    r.manager->stopAll();r.render();r.render();const auto stopped=r.a->view();
    for(int i=0;i<4;++i)r.render();
    BOOST_CHECK_EQUAL(r.a->view().displayMasterPosition,stopped.displayMasterPosition);
}
BOOST_AUTO_TEST_CASE (PlayToEndBoxReopensMutedPlaybackWithoutStopping)
{
    Rig r;r.a->request(LooperEngine::Action::record);r.render();r.render();r.render();
    r.a->request(LooperEngine::Action::record);r.render();r.manager->stopAll();r.render();r.render();
    BOOST_REQUIRE(r.manager->setMaster(r.an->nodeId));r.render();
    r.manager->setPlayToEnd(r.an->nodeId,true);r.render();
    BOOST_REQUIRE(r.manager->requestBox(r.an->nodeId,LooperManagerNode::BoxAction::playStop));
    for(int i=0;i<5;++i)r.render();
    BOOST_CHECK(r.a->view().momentaryMuted);BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::playing));
    BOOST_REQUIRE(r.manager->requestBox(r.an->nodeId,LooperManagerNode::BoxAction::playStop));r.render();
    BOOST_CHECK(!r.a->view().momentaryMuted);BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::playing));
    for(int i=0;i<4;++i)r.render();BOOST_CHECK(r.a->view().momentaryMuted);
    BOOST_CHECK(r.manager->playToEnd(r.an->nodeId));
}
BOOST_AUTO_TEST_CASE (MasterCompatibilityUsesLinkedLengthsOnly)
{
    Rig r;BOOST_REQUIRE(r.manager->setSyncMode(r.bn->nodeId,2));
    r.a->managerLength=8;r.b->managerLength=24;
    BOOST_CHECK(r.manager->masterChangeReason(r.an->nodeId).isEmpty());
    r.a->managerLength=16;
    BOOST_CHECK(r.manager->masterChangeReason(r.an->nodeId).isNotEmpty());
    r.a->managerLength=0;
    BOOST_CHECK(r.manager->masterChangeReason(r.an->nodeId).isNotEmpty());
    BOOST_CHECK(r.manager->masterChangeReason(0).isNotEmpty());
    r.b->managerLength=0;BOOST_REQUIRE(r.manager->setSyncMode(r.bn->nodeId,1));
    r.a->managerLength=8;r.b->managerLength=16;
    BOOST_CHECK(r.manager->masterChangeReason(r.an->nodeId).isNotEmpty());
    r.b->managerLength=8;
    BOOST_CHECK(r.manager->masterChangeReason(r.an->nodeId).isEmpty());
    r.b->managerLength=0;BOOST_REQUIRE(r.manager->setSyncMode(r.bn->nodeId,0));r.b->managerLength=13;
    BOOST_CHECK(r.manager->masterChangeReason(r.an->nodeId).isEmpty());
    r.b->captureRequested=1;BOOST_CHECK(r.manager->masterChangeReason(r.an->nodeId).isNotEmpty());
    r.b->captureRequested=0;r.b->managerTransportPending=true;
    BOOST_CHECK(r.manager->masterChangeReason(r.an->nodeId).isNotEmpty());
}
BOOST_AUTO_TEST_CASE (MomentaryMidiAndUiReleaseAndGroupSettingsPersist)
{
    Rig r;r.a->request(LooperEngine::Action::record);r.render();r.a->request(LooperEngine::Action::record);r.render();r.manager->stopAll();r.render();
    r.manager->setMomentary(r.an->nodeId,true);r.render();BOOST_CHECK(r.a->view().momentaryMuted);BOOST_REQUIRE(r.manager->setBoxMapping(r.an->nodeId,0,{80,1}));
    r.a->setQuantized(true);r.a->playOnBar=true;r.a->stopOnBar=true;
    r.midi.addEvent(MidiMessage::controllerEvent(1,80,127),2);r.render();
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::playing));
    r.midi.addEvent(MidiMessage::controllerEvent(1,80,0),3);r.render();BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::playing));
    r.manager->holdBox(r.an->nodeId,true);r.render();BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::playing));
    r.manager->holdBox(r.an->nodeId,false);r.render();BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::playing));
    r.manager->holdBox(r.an->nodeId,true);r.render();auto generation=r.manager->stopGeneration.load();r.manager->stopAll();r.render();r.render();
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::stopped));BOOST_CHECK(r.manager->stopGeneration.load()>generation);
    r.manager->groupVolume=.4f;r.manager->groupPan=-.5f;r.render();BOOST_CHECK_CLOSE(r.a->managerVolume.load(),.4f,.001);BOOST_CHECK_EQUAL(r.a->managerPan.load(),-.5f);
    r.manager->setPlayToEnd(r.an->nodeId,true);r.render();BOOST_CHECK(r.a->managerPlayToEnd.load());
    MemoryBlock data;r.manager->getState(data);LooperManagerNode loaded;loaded.setState(data.getData(),int(data.getSize()));
    MemoryBlock roundtrip;loaded.getState(roundtrip);BOOST_CHECK(roundtrip.toString().contains("playToEnd=\"1\""));
    BOOST_CHECK_CLOSE(loaded.groupVolume.load(),.4f,.001);BOOST_CHECK_EQUAL(loaded.groupPan.load(),-.5f);
}
BOOST_AUTO_TEST_CASE (RoutesOnlySelectedAndLocksSameBlockAndCapture)
{
    Rig r;
    BOOST_REQUIRE_EQUAL (r.manager->members().size(),2);
    BOOST_CHECK_EQUAL (r.manager->selectedID(),r.an->nodeId);
    r.press(20,2); r.render(); // subsequent selection is locked during capture
    BOOST_CHECK_EQUAL (r.manager->selectedID(),r.an->nodeId);
    BOOST_CHECK_EQUAL (r.a->view().length,5);
    BOOST_CHECK_EQUAL (r.b->view().length,0);
    r.manager->step(1); r.render(); BOOST_CHECK_EQUAL(r.manager->selectedID(),r.an->nodeId);
    r.press(20); r.render(); // finish
    r.manager->step(1); r.render(); BOOST_CHECK_EQUAL(r.manager->selectedID(),r.bn->nodeId);
    r.press(23); r.render(); BOOST_CHECK_EQUAL(r.b->view().state,1);
    BOOST_CHECK_EQUAL(r.a->view().state,2);
    r.press(23); r.render(); r.manager->step(-1); r.render();
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.an->nodeId);
    r.press(23); r.render(); r.manager->step(1); r.render();
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.an->nodeId); // overdub lock
    r.press(23); r.render(); r.manager->step(-1); r.render();
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.bn->nodeId); // wraps backwards
}
BOOST_AUTO_TEST_CASE (NavigationShortReleaseRecordsAdjacentWithoutErase)
{
    Rig r;
    r.press(20);r.render();r.press(20);r.render();
    const auto aLength=r.a->view().length;
    r.press(25,2);r.render();
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.bn->nodeId);
    BOOST_CHECK_EQUAL(r.a->view().length,aLength);
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::playing));
    BOOST_CHECK_EQUAL(r.b->view().state,int(LooperEngine::State::recording));
    r.render();r.press(24,2);r.render();
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.bn->nodeId); // no capture takeover
    r.press(23);r.render();r.press(24,2);r.render();
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.an->nodeId);
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::overdubbing));
    BOOST_CHECK_EQUAL(r.a->view().length,aLength);
    BOOST_CHECK_EQUAL(r.b->view().state,int(LooperEngine::State::playing));
}
BOOST_AUTO_TEST_CASE (NavigationIntoStoppedContentOverdubsWithoutErasing)
{
    Rig r;r.manager->step(1);r.render();r.press(20);r.render();r.press(20);r.render();
    const auto length=r.b->view().length;
    r.press(27);r.render();r.manager->step(-1);r.render();
    r.press(25);r.render();
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.bn->nodeId);
    BOOST_CHECK_EQUAL(r.b->view().length,length);
    BOOST_CHECK_EQUAL(r.b->view().count,2);
    BOOST_CHECK_EQUAL(r.b->view().state,int(LooperEngine::State::overdubbing));
}
BOOST_AUTO_TEST_CASE (NavigationRejectsBypassedTargetWithoutFinishingSource)
{
    Rig r;r.b->setBypassed(true);r.press(20);r.render();
    r.press(25);r.render();
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.an->nodeId);
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::recording));
    BOOST_CHECK_EQUAL(r.b->view().count,0);
    BOOST_CHECK_EQUAL(r.manager->handoffTarget.load(),0);
}
BOOST_AUTO_TEST_CASE (NavigationActionsAreIndependentAndPersist)
{
    Rig r;r.manager->navigationProfile=-1;r.manager->longProfile=1;
    r.press(25);r.render();BOOST_CHECK_EQUAL(r.manager->selectedID(),r.bn->nodeId);BOOST_CHECK_EQUAL(r.b->view().count,0);
    r.midi.addEvent(MidiMessage::controllerEvent(1,24,127),0);r.render();for(int i=0;i<11;++i)r.render();
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.an->nodeId);BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::recording));
    MemoryBlock data;r.manager->getState(data);LooperManagerNode restored;restored.setState(data.getData(),int(data.getSize()));
    BOOST_CHECK_EQUAL(restored.navigationProfile.load(),-1);BOOST_CHECK_EQUAL(restored.longProfile.load(),1);
}
BOOST_AUTO_TEST_CASE (NavigationHoldSelectsWithoutRecording)
{
    Rig r;
    r.midi.addEvent(MidiMessage::controllerEvent(1,25,127),0);r.render();
    for(int i=0;i<10;++i) r.render();
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.bn->nodeId);
    r.midi.addEvent(MidiMessage::controllerEvent(1,25,0),0);r.render();
    BOOST_CHECK_EQUAL(r.b->view().count,0);
    BOOST_CHECK_EQUAL(r.manager->handoffTarget.load(),0);
}
BOOST_AUTO_TEST_CASE (NavigationSyncSamePedalCancelsAndStopCancels)
{
    struct Clock:AudioPlayHead {
        double ppq=0;
        Optional<PositionInfo> getPosition() const override {PositionInfo p;p.setBpm(120);p.setPpqPosition(ppq);p.setTimeSignature(TimeSignature{4,4});p.setIsPlaying(true);return p;}
    } clock;
    Rig r;r.manager->shortTiming=1;r.b->setOverdubNextBar(false);r.manager->setPlayHead(&clock);r.a->setPlayHead(&clock);r.b->setPlayHead(&clock);r.a->setQuantized(true);r.b->setQuantized(true);
    r.press(25);r.render();
    BOOST_CHECK_EQUAL(r.manager->handoffTarget.load(),r.bn->nodeId);
    clock.ppq+=.16;r.press(25);r.render();BOOST_CHECK_EQUAL(r.manager->handoffTarget.load(),0);
    clock.ppq+=.16;r.press(25);r.render();BOOST_CHECK_EQUAL(r.manager->handoffTarget.load(),r.bn->nodeId);
    clock.ppq+=.16;r.press(27);r.render();BOOST_CHECK_EQUAL(r.manager->handoffTarget.load(),0);
    BOOST_CHECK_EQUAL(r.b->view().count,0);
}
BOOST_AUTO_TEST_CASE (NavigationSyncExecutesAtNextBarAndHoldIsBlockedDuringCapture)
{
    struct Clock:AudioPlayHead {
        double ppq=0;
        Optional<PositionInfo> getPosition() const override {PositionInfo p;p.setBpm(120);p.setPpqPosition(ppq);p.setTimeSignature(TimeSignature{4,4});p.setIsPlaying(true);return p;}
    } clock;
    Rig r;r.manager->shortTiming=1;r.b->setOverdubNextBar(false);r.manager->setPlayHead(&clock);r.a->setPlayHead(&clock);r.b->setPlayHead(&clock);
    r.a->setQuantized(true);r.b->setQuantized(true);
    r.press(25);r.render();
    BOOST_CHECK_EQUAL(r.b->view().count,0);
    for(int i=1;i<=25;++i) {clock.ppq=i*.16;r.render();}
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.bn->nodeId);
    BOOST_CHECK_EQUAL(r.b->view().state,int(LooperEngine::State::recording));
    r.midi.addEvent(MidiMessage::controllerEvent(1,24,127),0);
    for(int i=0;i<12;++i) {clock.ppq+=.16;r.render();}
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.bn->nodeId);
    r.midi.addEvent(MidiMessage::controllerEvent(1,24,0),0);clock.ppq+=.16;r.render();
    BOOST_CHECK_EQUAL(r.manager->handoffTarget.load(),0);
}
BOOST_AUTO_TEST_CASE (FirstMasterCaptureBlocksNavigationAndKeepsLinkedTargetEmpty)
{
    Rig r;BOOST_REQUIRE(r.manager->setMaster(r.an->nodeId));BOOST_REQUIRE(r.manager->setSyncMode(r.bn->nodeId,1));
    r.press(20);r.render();r.press(25);r.render();
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.an->nodeId);
    BOOST_CHECK_EQUAL(r.manager->handoffTarget.load(),0);
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::recording));
    BOOST_CHECK_EQUAL(r.b->view().count,0);
}
BOOST_AUTO_TEST_CASE (SectionDepartureCancelsQueuedHandoffAndHeldNavigation)
{
    struct Clock:AudioPlayHead {
        Optional<PositionInfo> getPosition() const override {PositionInfo p;p.setBpm(120);p.setPpqPosition(0);p.setTimeSignature(TimeSignature{4,4});p.setIsPlaying(true);return p;}
    } clock;
    Rig r;r.manager->shortTiming=1;r.b->setOverdubNextBar(false);r.manager->setPlayHead(&clock);r.a->setPlayHead(&clock);r.b->setPlayHead(&clock);r.a->setQuantized(true);r.press(25);r.render();
    BOOST_REQUIRE(r.manager->handoffTarget.load()!=0);
    r.manager->cancelPendingForSection(0,false);r.render();
    BOOST_CHECK_EQUAL(r.manager->handoffTarget.load(),0);
    BOOST_CHECK_EQUAL(r.b->view().count,0);
}
BOOST_AUTO_TEST_CASE (QueuedHandoffCancelsWhenTempoOrMeterChanges)
{
    struct Clock:AudioPlayHead {
        double ppq=0,bpm=120,origin=0; int beats=4;
        Optional<PositionInfo> getPosition() const override {PositionInfo p;p.setBpm(bpm);p.setPpqPosition(ppq);p.setPpqPositionOfLastBarStart(origin);p.setTimeSignature(TimeSignature{beats,4});p.setIsPlaying(true);return p;}
    } clock;
    Rig r;r.manager->shortTiming=1;r.b->setOverdubNextBar(false);r.manager->setPlayHead(&clock);r.a->setPlayHead(&clock);r.b->setPlayHead(&clock);r.a->setQuantized(true);
    r.press(25);r.render();BOOST_REQUIRE(r.manager->handoffTarget.load()!=0);
    clock.ppq=.16;clock.bpm=90;r.render();BOOST_CHECK_EQUAL(r.manager->handoffTarget.load(),0);
    clock.ppq+=.12;r.press(25);r.render();BOOST_REQUIRE(r.manager->handoffTarget.load()!=0);
    clock.ppq+=.12;clock.beats=3;r.render();BOOST_CHECK_EQUAL(r.manager->handoffTarget.load(),0);
    clock.ppq+=.12;r.press(25);r.render();BOOST_REQUIRE(r.manager->handoffTarget.load()!=0);
    clock.ppq+=.12;clock.origin=.5;r.render();BOOST_CHECK_EQUAL(r.manager->handoffTarget.load(),0);
    BOOST_CHECK_EQUAL(r.b->view().count,0);
}
BOOST_AUTO_TEST_CASE (DirectMidiSuppressedRemovalRestoresAndStateOrderRoundtrip)
{
    Rig r;
    BOOST_REQUIRE(r.g.connectChannels(PortType::Midi,r.input->nodeId,0,r.bn->nodeId,0)); r.g.rebuild();
    r.press(20); r.render(); BOOST_CHECK_EQUAL(r.b->view().length,0);
    r.press(20); r.render();
    r.manager->move(r.bn->nodeId,-1);
    BOOST_CHECK_EQUAL(r.manager->members()[0].node->nodeId,r.bn->nodeId);
    r.manager->playAllCC.store(74); r.manager->stopAllCC.store(73); r.manager->previousCC.store(70); r.manager->nextCC.store(71); r.manager->channel.store(2);
    MemoryBlock saved; r.manager->getState(saved);
    LooperManagerNode restored; restored.setState(saved.getData(),int(saved.getSize()));
    MemoryBlock savedAgain; restored.getState(savedAgain); BOOST_CHECK(saved == savedAgain);
    BOOST_REQUIRE(r.g.removeNode(r.m->nodeId));
    BOOST_CHECK_EQUAL(r.b->managerOwner.load(),0);
    r.press(20); r.render(); BOOST_CHECK_EQUAL(r.b->view().state,1);
}
BOOST_AUTO_TEST_CASE (LearnConsumesPressAndQueuedUICaptureBlocksSelection)
{
    Rig r;
    r.manager->learn.store(2); r.press(72); r.render();
    BOOST_CHECK_EQUAL(r.manager->nextCC.load(),72);
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.an->nodeId);
    r.manager->step(1); r.render(); BOOST_CHECK_EQUAL(r.manager->selectedID(),r.bn->nodeId);
    r.b->request(LooperEngine::Action::overdub);
    r.manager->step(-1); r.render(); BOOST_CHECK_EQUAL(r.manager->selectedID(),r.bn->nodeId);
    r.b->request(LooperEngine::Action::overdub); r.render();
    r.manager->step(-1); r.render(); BOOST_CHECK_EQUAL(r.manager->selectedID(),r.an->nodeId);
}
BOOST_AUTO_TEST_CASE (SyncArmedAndPendingEndPreventSwitchUntilFinished)
{
    struct Clock : AudioPlayHead {
        double ppq = 1;
        Optional<PositionInfo> getPosition() const override {
            PositionInfo p; p.setBpm(120); p.setPpqPosition(ppq); p.setTimeSignature(TimeSignature{4,4}); p.setIsPlaying(true); return p;
        }
    } clock;
    Rig r;
    r.a->setPlayHead(&clock); r.a->setQuantized(true); r.a->setRecordNextBar(true); r.a->setGraceMs(0);
    r.press(20); r.render(); BOOST_CHECK(r.a->view().armed);
    r.manager->step(1); r.render(); BOOST_CHECK_EQUAL(r.manager->selectedID(),r.an->nodeId);
    clock.ppq = 4; r.render(); BOOST_CHECK_EQUAL(r.a->view().state,1);
    clock.ppq = 4.16; r.press(20); r.render(); BOOST_CHECK(r.a->view().pending);
    r.manager->step(1); r.render(); BOOST_CHECK_EQUAL(r.manager->selectedID(),r.an->nodeId);
    for(int i=0;i<26;++i) {clock.ppq += .16; r.render();}
    BOOST_CHECK(!r.a->captureBusy.load());
    r.manager->step(1); r.render(); BOOST_CHECK_EQUAL(r.manager->selectedID(),r.bn->nodeId);
}

BOOST_AUTO_TEST_CASE (StopAllStopsPlayingAndCapturePreservingLayers)
{
    Rig r;
    r.press(20); r.render(); r.press(20); r.render();
    r.manager->step(1); r.render(); r.press(23); r.render();
    BOOST_REQUIRE(r.manager->isLocked());
    r.press(27,3); r.render();
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::stopped));
    BOOST_CHECK_EQUAL(r.b->view().state,int(LooperEngine::State::stopped));
    BOOST_CHECK_EQUAL(r.a->view().length,8);
    BOOST_CHECK_EQUAL(r.b->view().length,10); // Overdub starts on pedal release (one sample after press).
    r.press(27); r.render(); // never resumes
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::stopped));
    r.b->request(LooperEngine::Action::overdub); r.render();
    r.manager->stopAll(); r.render();
    BOOST_CHECK_EQUAL(r.b->view().state,int(LooperEngine::State::stopped));
    BOOST_CHECK(!r.manager->isLocked());
}
BOOST_AUTO_TEST_CASE (PanicCancelsSyncArmingAndPendingEnd)
{
    struct Clock : AudioPlayHead {
        double ppq = 1;
        Optional<PositionInfo> getPosition() const override {
            PositionInfo p; p.setBpm(120); p.setPpqPosition(ppq); p.setTimeSignature(TimeSignature{4,4}); p.setIsPlaying(true); return p;
        }
    } clock;
    Rig r;
    r.a->setPlayHead(&clock); r.a->setQuantized(true); r.a->setRecordNextBar(true); r.a->setGraceMs(0);
    r.press(20); r.render(); BOOST_REQUIRE(r.a->view().armed);
    r.manager->stopAll(); r.render(); BOOST_CHECK(!r.a->view().armed);
    r.press(20); r.render(); clock.ppq=4; r.render();
    clock.ppq=4.16; r.press(20); r.render(); BOOST_REQUIRE(r.a->view().pending);
    r.press(27); r.render();
    BOOST_CHECK(!r.a->view().pending); BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::stopped));
    BOOST_CHECK_GT(r.a->view().length,0);
}
namespace {
void seedLoops(Rig& r) {
    r.a->request(LooperEngine::Action::record);r.render();r.render();
    r.a->request(LooperEngine::Action::record);r.render();
    r.b->request(LooperEngine::Action::record);r.render();r.render();r.render();
    r.b->request(LooperEngine::Action::record);r.render();
    r.manager->stopAll(); r.render();
    BOOST_REQUIRE_EQUAL(r.a->managerLength.load(),16);
    BOOST_REQUIRE_EQUAL(r.b->managerLength.load(),24);
}
struct GroupClock : AudioPlayHead {
    double ppq = 0; bool running = true; int numerator = 4, denominator = 4;
    Optional<PositionInfo> getPosition() const override {
        PositionInfo p; p.setBpm(120); p.setPpqPosition(ppq); p.setTimeSignature(TimeSignature{numerator,denominator}); p.setIsPlaying(running); return p;
    }
};
}
BOOST_AUTO_TEST_CASE (PlayAllFreeFromZeroAndJoinAfterManyShortCycles)
{
    Rig r; seedLoops(r);
    r.press(28,3); r.render();
    BOOST_CHECK_EQUAL(r.a->view().position,5); BOOST_CHECK_EQUAL(r.b->view().position,5);
    for(int i=0;i<7;++i) r.render(); // elapsed61, not position13
    r.b->request(LooperEngine::Action::stop); r.render(); // elapsed69
    r.press(28,2); r.render(); // join at71; end77
    BOOST_CHECK_EQUAL(r.a->view().position,77%16);
    BOOST_CHECK_EQUAL(r.b->view().position,77%24);
    r.press(28); r.render(); // both playing: no reset
    BOOST_CHECK_EQUAL(r.a->view().position,85%16);
    BOOST_CHECK_EQUAL(r.b->view().position,85%24);
    r.press(27); r.render(); r.manager->playAll(); r.render();
    BOOST_CHECK_EQUAL(r.a->view().position,8); BOOST_CHECK_EQUAL(r.b->view().position,8);
}
BOOST_AUTO_TEST_CASE (IndividualStartEstablishesTimelineAndGroupIgnoresOverdub)
{
    Rig r; seedLoops(r);
    r.a->request(LooperEngine::Action::play); r.render();
    for(int i=0;i<8;++i) r.render(); //72 frames elapsed
    r.manager->playAll(); r.render();
    BOOST_CHECK_EQUAL(r.b->view().position,80%24);
    r.b->request(LooperEngine::Action::stop); r.render();
    r.a->request(LooperEngine::Action::overdub); r.manager->playAll(); r.render();
    BOOST_CHECK_EQUAL(r.b->view().state,int(LooperEngine::State::stopped));
    r.press(28); r.render(); BOOST_CHECK_EQUAL(r.b->view().state,int(LooperEngine::State::stopped));
    r.a->request(LooperEngine::Action::overdub); r.render();
    r.manager->playAll(); r.render();
    BOOST_CHECK_EQUAL(r.b->view().state,int(LooperEngine::State::playing));
}
BOOST_AUTO_TEST_CASE (MixedSyncNextBarSampleAccurateAndPanicCancelsLaunch)
{
    Rig r; seedLoops(r); GroupClock clock;
    r.manager->setPlayHead(&clock); r.a->setQuantized(true);
    clock.ppq=3.9; r.press(28,1); r.render(); // boundary5 samples into block
    BOOST_CHECK_EQUAL(r.a->view().position,3); BOOST_CHECK_EQUAL(r.b->view().position,3);
    BOOST_CHECK(!r.manager->playPending.load());
    r.manager->stopAll(); clock.ppq=4.06; r.render();
    clock.ppq=4.22; r.press(28); r.render(); BOOST_CHECK(r.manager->playPending.load());
    clock.ppq=4.38; r.press(27); r.render(); BOOST_CHECK(!r.manager->playPending.load());
    clock.ppq=7.9; r.render(); BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::stopped));
    // Stop on the scheduled sample wins over the queued start.
    clock.ppq=8.06; r.press(28); r.render();
    for(int i=0;i<24;++i) {clock.ppq+=.16; r.render();}
    r.manager->stopAll(); r.render();
    clock.ppq=11.9; r.press(28); r.press(27,5); r.render();
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::stopped));
}
BOOST_AUTO_TEST_CASE (PendingClockStopSeekCaptureAndThreeFour)
{
    Rig r; seedLoops(r); GroupClock clock;
    r.manager->setPlayHead(&clock); r.a->setQuantized(true);
    clock.running=false; r.manager->playAll(); r.render();
    BOOST_CHECK(!r.manager->playNeedsClock.load()); BOOST_CHECK(!r.manager->playPending.load());
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::playing));
    BOOST_CHECK(!r.a->transportStartRequested.load());r.manager->stopAll();r.render();
    clock.running=true; clock.ppq=1; r.manager->playAll(); r.render(); BOOST_REQUIRE(r.manager->playPending.load());
    clock.running=false; r.render(); BOOST_CHECK(!r.manager->playPending.load());
    clock.running=true; clock.ppq=1; r.manager->playAll(); r.render();
    clock.ppq=9; r.render(); BOOST_CHECK(!r.manager->playPending.load());
    clock.ppq=9.16; r.manager->playAll(); r.render();
    r.b->request(LooperEngine::Action::record); clock.ppq=9.32; r.render();
    BOOST_CHECK(!r.manager->playPending.load());
    r.manager->stopAll(); clock.ppq=9.48; r.render();
    clock.numerator=3; clock.ppq=11.9; r.manager->playAll(); r.render();
    BOOST_CHECK_EQUAL(r.a->view().position,3); BOOST_CHECK_EQUAL(r.b->view().position,3);
}
BOOST_AUTO_TEST_CASE (SameBlockRecordThenPlayIgnoredAndStopThenPlayRestarts)
{
    Rig r; seedLoops(r);
    r.press(23,1); r.press(28,3); r.render();
    BOOST_CHECK_EQUAL(r.b->view().state,int(LooperEngine::State::stopped));
    r.manager->stopAll(); r.render();
    r.press(28); r.render();
    r.press(27,1); r.press(28,4); r.render();
    BOOST_CHECK_EQUAL(r.a->view().position,4); BOOST_CHECK_EQUAL(r.b->view().position,4);
}
BOOST_AUTO_TEST_CASE (QueuedStartAcrossBlocksRepeatedPressAndRecordedOrigin)
{
    Rig r; seedLoops(r); GroupClock clock;
    r.manager->setPlayHead(&clock); r.a->setQuantized(true);
    clock.ppq=3.7; r.press(28); r.render(); BOOST_REQUIRE(r.manager->playPending.load());
    clock.ppq=3.86; r.press(28); r.render(); // original boundary at sample7, no re-arm
    BOOST_CHECK_EQUAL(r.a->view().position,1); BOOST_CHECK_EQUAL(r.b->view().position,1);
    r.manager->stopAll(); clock.ppq=4.02; r.render();
    r.a->setQuantized(false); r.a->request(LooperEngine::Action::record); r.render(); r.render();
    r.a->request(LooperEngine::Action::record); r.render();
    for(int i=0;i<9;++i) r.render();
    const auto elapsed=r.a->managerRunFrames.load();
    BOOST_REQUIRE_GT(elapsed, uint64_t(r.a->managerLength.load()));
    r.manager->playAll(); r.render();
    BOOST_CHECK_EQUAL(r.b->view().position,int((elapsed+8)%24));
}
BOOST_AUTO_TEST_CASE (OldStateDefaultsAndPlayLearn)
{
    Rig r; seedLoops(r);
    const String old("<LooperManager version=\"1\" previous=\"24\" next=\"25\" channel=\"1\"/>");
    LooperManagerNode restored; restored.setState(old.toRawUTF8(), old.getNumBytesAsUTF8());
    BOOST_CHECK_EQUAL(restored.playAllCC.load(),28); BOOST_CHECK_EQUAL(restored.stopAllCC.load(),27);
    r.manager->learn.store(4); r.press(75); r.render();
    BOOST_CHECK_EQUAL(r.manager->playAllCC.load(),75);
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::stopped));
    r.press(75); r.render(); BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::playing));
}
BOOST_AUTO_TEST_CASE (NativeLoopTransportRejectsOversampling)
{
    Rig r;
    for (auto* node : { r.an, r.bn, r.m })
    {
        BOOST_CHECK (! node->supportsOversampling());
        node->setOversamplingFactor (8);
        BOOST_CHECK_EQUAL (node->getOversamplingFactor(), 1);
    }
    // A rejected change must not reprepare/erase existing captured audio.
    r.press (20); r.render(); r.press (20); r.render();
    const auto length = r.a->view().length;
    r.an->setOversamplingFactor (2);
    r.render();
    BOOST_CHECK_EQUAL (r.a->view().length, length);
    BOOST_CHECK_EQUAL (r.a->view().state, int (LooperEngine::State::playing));
}

BOOST_AUTO_TEST_CASE(MasterRulesAndResetAreAppliedToEveryMember)
{
    Rig r;BOOST_REQUIRE(r.manager->setMaster(r.an->nodeId));BOOST_REQUIRE(r.manager->setSyncMode(r.bn->nodeId,1));
    r.b->request(LooperEngine::Action::record);r.render();BOOST_CHECK_EQUAL(r.b->managerLength.load(),0);
    r.a->request(LooperEngine::Action::record);r.render();r.a->request(LooperEngine::Action::record);r.render();r.render();
    BOOST_REQUIRE_EQUAL(r.manager->masterLength.load(),8);BOOST_CHECK(!r.manager->setMaster(r.bn->nodeId));
    r.a->request(LooperEngine::Action::stop);r.render();
    r.b->request(LooperEngine::Action::record);r.render();r.b->request(LooperEngine::Action::record);r.render();
    BOOST_CHECK_EQUAL(r.b->managerFixedLength.load(),8);
    MemoryBlock managerState;r.manager->getState(managerState);LooperManagerNode settings;settings.setState(managerState.getData(),int(managerState.getSize()));
    BOOST_CHECK_EQUAL(settings.masterID(),r.an->nodeId);BOOST_CHECK_EQUAL(settings.syncMode(r.bn->nodeId),1);
    MemoryBlock saved;r.b->getStateInformation(saved);LooperProcessor loaded;loaded.setStateInformation(saved.getData(),int(saved.getSize()));
    BOOST_CHECK_EQUAL(loaded.managerFixedLength.load(),8);
    r.b->request(LooperEngine::Action::overdub);r.manager->resetAll();r.render();r.render();
    BOOST_CHECK_EQUAL(r.a->managerLength.load(),0);BOOST_CHECK_EQUAL(r.b->managerLength.load(),0);
    BOOST_CHECK_EQUAL(r.a->managerFixedLength.load(),0);BOOST_CHECK_EQUAL(r.b->managerFixedLength.load(),0);
    BOOST_CHECK_EQUAL(r.manager->masterLength.load(),0);BOOST_CHECK_EQUAL(r.manager->masterID(),r.an->nodeId);
    BOOST_CHECK_EQUAL(r.manager->syncMode(r.bn->nodeId),1);
    r.a->request(LooperEngine::Action::undo);r.b->request(LooperEngine::Action::record);r.render();
    BOOST_CHECK_EQUAL(r.a->managerLength.load(),0);BOOST_CHECK_EQUAL(r.b->managerLength.load(),0);
}

BOOST_AUTO_TEST_CASE(LinkedClockContinuesWhenManagerIsDisabledAndResumes)
{
    Rig r;BOOST_REQUIRE(r.manager->setMaster(r.an->nodeId));
    r.press(20,2);r.render();r.press(20,4);r.render();r.render();
    BOOST_REQUIRE_EQUAL(r.a->managerFixedLength.load(),10);
    const auto before=r.a->managerClockFrame.load();r.m->setEnabled(false);
    r.render();r.render();r.render();BOOST_CHECK_EQUAL(r.a->managerClockFrame.load(),before+24);
    r.m->setEnabled(true);r.render();BOOST_CHECK_EQUAL(r.a->managerClockFrame.load(),before+32);
}
BOOST_AUTO_TEST_CASE(GroupRestartChangesClockOnlyAtItsEvent)
{
    Rig r;BOOST_REQUIRE(r.manager->setMaster(r.an->nodeId));
    r.press(20,2);r.render();r.press(20,4);r.render();r.render();
    const auto origin=r.a->managerOrigin.load();
    r.press(27,1);r.press(28,4);r.render();
    BOOST_CHECK_EQUAL(r.a->managerClockFrame.load(),origin+4);
    r.render();BOOST_CHECK_EQUAL(r.a->managerClockFrame.load(),origin+12);
}
BOOST_AUTO_TEST_CASE(StateReplacementAfterResetPreservesLoadedAudio)
{
    Rig r; BOOST_REQUIRE(r.manager->setMaster(r.an->nodeId));
    r.press(20);r.render();r.press(20);r.render();
    MemoryBlock saved;r.a->getStateInformation(saved);
    r.manager->resetAll();r.render();r.render();
    r.a->setStateInformation(saved.getData(),int(saved.getSize()));r.render();
    BOOST_CHECK_EQUAL(r.a->managerLength.load(),8);
}
BOOST_AUTO_TEST_CASE(DisabledFollowerCannotRestorePreRestartClock)
{
    Rig r; BOOST_REQUIRE(r.manager->setMaster(r.an->nodeId));
    BOOST_REQUIRE(r.manager->setSyncMode(r.bn->nodeId,1));
    r.press(20,2);r.render();r.press(20,4);r.render();r.render();
    for(int i=0;i<12;++i)r.render();
    r.bn->setEnabled(false);
    r.press(27,1);r.press(28,4);r.render();
    const auto frame=r.a->managerClockFrame.load();
    r.bn->setEnabled(true);r.render();
    BOOST_CHECK_EQUAL(r.a->managerClockFrame.load(),frame+8);
    BOOST_CHECK_EQUAL(r.b->managerClockFrame.load(),frame+8);
}
BOOST_AUTO_TEST_CASE(ExplicitLoadSupersedesResetWhileDisabled)
{
    Rig r; BOOST_REQUIRE(r.manager->setMaster(r.an->nodeId));
    r.press(20);r.render();r.press(20);r.render();
    MemoryBlock saved;r.a->getStateInformation(saved);
    r.an->setEnabled(false);r.manager->resetAll();r.render();
    r.a->setStateInformation(saved.getData(),int(saved.getSize()));
    r.an->setEnabled(true);r.render();BOOST_CHECK_EQUAL(r.a->managerLength.load(),8);
    r.manager->resetAll();r.render();BOOST_CHECK_EQUAL(r.a->managerLength.load(),0);
}
BOOST_AUTO_TEST_CASE(ManagerEditorShowsMasterModesAndReset)
{
    Rig r; ValueTree data(types::Node);data.setProperty(tags::object,r.m,nullptr);
    Node node(data,false);LooperManagerEditor editor(node);
    TextButton* settings=nullptr;TextButton* reset=nullptr;ComboBox* order=nullptr;
    std::function<void(Component&)> scan=[&](Component& c){
        if(auto* b=dynamic_cast<TextButton*>(&c)){if(b->getButtonText()=="Settings")settings=b;if(b->getButtonText()=="RESET ALL LOOPS...")reset=b;}
        if(auto* box=dynamic_cast<ComboBox*>(&c);box && box->getNumItems()==2)order=box;
        for(auto* child:c.getChildren())scan(*child);
    };
    scan(editor);BOOST_REQUIRE(settings);
    settings->onClick();scan(editor);BOOST_REQUIRE(reset);BOOST_CHECK(reset->findParentComponentOfClass<Viewport>()!=nullptr);
    settings->onClick();BOOST_CHECK(reset->findParentComponentOfClass<Viewport>()==nullptr);

}

BOOST_AUTO_TEST_CASE(GestureReleaseRoutesToSelectedAndBlocksChangingTarget)
{
    Rig r;r.press(20);r.render();r.press(20);r.render();
    r.press(23);r.render();r.press(23);r.render();
    BOOST_REQUIRE_EQUAL(r.a->view().count,2);
    r.midi.addEvent(MidiMessage::controllerEvent(1,29,127),0);r.render();
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.an->nodeId);
    BOOST_CHECK_EQUAL(r.a->view().count,2);
    r.manager->step(1);r.render();BOOST_CHECK_EQUAL(r.manager->selectedID(),r.an->nodeId);
    r.midi.addEvent(MidiMessage::controllerEvent(1,29,0),0);r.render();
    BOOST_CHECK_EQUAL(r.a->view().count,1);BOOST_CHECK_EQUAL(r.b->view().count,0);
    r.manager->step(1);r.render();BOOST_CHECK_EQUAL(r.manager->selectedID(),r.bn->nodeId);
}
BOOST_AUTO_TEST_CASE(StopAllCancelsPendingPedalRelease)
{
    Rig r;r.press(20);r.render();r.press(20);r.render();
    r.midi.addEvent(MidiMessage::controllerEvent(1,23,127),0);r.render();
    r.press(27);r.render();
    r.midi.addEvent(MidiMessage::controllerEvent(1,23,0),0);r.render();
    BOOST_CHECK_EQUAL(r.a->view().count,1);BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::stopped));
}


BOOST_AUTO_TEST_CASE(SectionTransitionPreservesEarlierPedalRelease)
{
    Rig r;r.press(20);r.render();r.press(20);r.render();
    r.midi.addEvent(MidiMessage::controllerEvent(1,23,127),0);r.render();
    BOOST_REQUIRE_EQUAL(r.a->view().count,1);
    r.manager->cancelPendingForSection(6,false);r.a->scheduleSectionEvent(6,false);r.b->scheduleSectionEvent(6,false);
    r.midi.addEvent(MidiMessage::controllerEvent(1,23,0),2);r.render();
    BOOST_CHECK_EQUAL(r.a->view().count,2); // release before boundary began the overdub
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::stopped));
}

BOOST_AUTO_TEST_CASE(PlayStopHoldClearsOnlySelectedLooper)
{
    Rig r;
    r.a->request(LooperEngine::Action::record);r.render();r.a->request(LooperEngine::Action::play);r.render();
    r.b->request(LooperEngine::Action::record);r.render();r.b->request(LooperEngine::Action::play);r.render();
    r.midi.addEvent(MidiMessage::controllerEvent(1,21,127),0);r.render();
    r.manager->step(1);r.render();
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.an->nodeId);
    for(int i=0;i<10;++i)r.render();
    BOOST_CHECK_EQUAL(r.a->view().count,0); BOOST_CHECK_EQUAL(r.b->view().count,1);
    r.midi.addEvent(MidiMessage::controllerEvent(1,21,0),0);r.render();
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::empty));
}
BOOST_AUTO_TEST_CASE(StopAllHoldResetsGroupButTapPreservesContent)
{
    Rig r;
    r.a->request(LooperEngine::Action::record);r.render();r.a->request(LooperEngine::Action::play);r.render();
    r.b->request(LooperEngine::Action::record);r.render();r.b->request(LooperEngine::Action::play);r.render();
    r.press(27);r.render();
    BOOST_CHECK_EQUAL(r.a->view().count,1);BOOST_CHECK_EQUAL(r.b->view().count,1);
    r.manager->playAll();r.render();
    r.midi.addEvent(MidiMessage::controllerEvent(1,27,127),0);r.render();
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::stopped));
    BOOST_CHECK_EQUAL(r.b->view().state,int(LooperEngine::State::stopped));
    for(int i=0;i<9;++i)r.render();
    r.render(); // reset at next block boundary
    BOOST_CHECK_EQUAL(r.a->view().count,0);BOOST_CHECK_EQUAL(r.b->view().count,0);
    r.a->request(LooperEngine::Action::record);r.render();
    r.midi.addEvent(MidiMessage::controllerEvent(1,27,127),0);r.render();
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::recording)); // repeated high is not a new press
    r.midi.addEvent(MidiMessage::controllerEvent(1,27,0),0);r.render();
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::recording));
}

BOOST_AUTO_TEST_CASE(StopHoldReleaseInResetBlockAllowsNextPress)
{
    Rig r;r.press(20);r.render();r.press(20);r.render();
    r.midi.addEvent(MidiMessage::controllerEvent(1,27,127),0);r.render();
    for(int i=0;i<9;++i)r.render();
    r.midi.addEvent(MidiMessage::controllerEvent(1,27,0),0);r.render();
    BOOST_CHECK_EQUAL(r.a->view().count,0);
    r.press(20);r.render();r.press(20);r.render();
    r.press(27);r.render();
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::stopped));
    BOOST_CHECK_EQUAL(r.a->view().count,1);
}
BOOST_AUTO_TEST_CASE(SectionChangeCancelsStopHold)
{
    Rig r;r.press(20);r.render();r.press(20);r.render();
    r.midi.addEvent(MidiMessage::controllerEvent(1,27,127),0);r.render();
    r.manager->cancelPendingForSection(0,false);r.render();
    for(int i=0;i<12;++i)r.render();
    BOOST_CHECK_EQUAL(r.a->view().count,1);
}


BOOST_AUTO_TEST_CASE(RecordTapAndHoldRouteToCapturedManagerTarget)
{
    Rig r;
    r.midi.addEvent(MidiMessage::controllerEvent(1,20,127),0); r.render();
    BOOST_CHECK_EQUAL(r.a->view().count,0);
    r.manager->step(1); r.render();
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.an->nodeId);
    r.midi.addEvent(MidiMessage::controllerEvent(1,20,0),0); r.render();
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::recording));
    BOOST_CHECK_EQUAL(r.b->view().count,0);
    r.press(20); r.render();
    r.midi.addEvent(MidiMessage::controllerEvent(1,20,127),0); r.render();
    for(int i=0;i<10;++i) r.render();
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::recording));
    BOOST_CHECK_EQUAL(r.a->view().count,1);
    r.midi.addEvent(MidiMessage::controllerEvent(1,20,0),0); r.render();
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::recording));
    BOOST_CHECK_EQUAL(r.b->view().count,0);
}

BOOST_AUTO_TEST_CASE(CustomRecordingControllersRouteOnlyToSelectedLooper)
{
    Rig r;
    for(auto* p:{r.a,r.b}) {
        auto c=p->recordingControl(2);c.enabled=true;BOOST_REQUIRE(p->setRecordingControl(2,c));
        BOOST_REQUIRE(p->setMidiMapping(6,{70,1}));
    }
    r.press(70);r.render();BOOST_CHECK_EQUAL(r.a->view().state,1);BOOST_CHECK_EQUAL(r.b->view().count,0);
    r.press(70);r.render();BOOST_CHECK_EQUAL(r.a->view().state,2);
    r.manager->step(1);r.render();r.press(70);r.render();
    BOOST_CHECK_EQUAL(r.b->view().state,1);BOOST_CHECK_EQUAL(r.a->view().state,2);
    r.press(70);r.render();
    // The custom hold uses the same targeted press/release protocol as built-ins.
    r.midi.addEvent(MidiMessage::controllerEvent(1,70,127),0);r.render();
    for(int i=0;i<11;++i)r.render();BOOST_CHECK_EQUAL(r.b->view().state,5);
    r.midi.addEvent(MidiMessage::controllerEvent(1,70,0),0);r.render();BOOST_CHECK_EQUAL(r.b->view().state,2);
}
BOOST_AUTO_TEST_CASE (BoxActionsDoNotSelectAndSoloRestoresUnderlyingMutes)
{
    Rig r;
    r.a->setMuted(true);r.manager->requestBox(r.bn->nodeId,LooperManagerNode::BoxAction::solo);r.render();
    BOOST_CHECK(r.manager->soloed(r.bn->nodeId));BOOST_CHECK(r.a->managerSoloMuted.load());BOOST_CHECK(r.a->getMuted());
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.an->nodeId);
    r.manager->requestBox(r.an->nodeId,LooperManagerNode::BoxAction::solo);r.render();
    BOOST_CHECK(!r.a->getMuted());BOOST_CHECK(!r.a->managerSoloMuted.load());BOOST_CHECK(!r.b->managerSoloMuted.load());
    r.manager->requestBox(r.bn->nodeId,LooperManagerNode::BoxAction::mute);r.render();
    BOOST_CHECK(!r.manager->soloed(r.bn->nodeId));BOOST_CHECK(r.b->getMuted());BOOST_CHECK(r.b->managerSoloMuted.load());
    r.manager->requestBox(r.an->nodeId,LooperManagerNode::BoxAction::solo);r.render();
    BOOST_CHECK(!r.b->managerSoloMuted.load());BOOST_CHECK(r.b->getMuted());
}
BOOST_AUTO_TEST_CASE (DirectRecordingProfileAndSingleRecorderGuard)
{
    Rig r;
    auto custom=r.b->recordingControl(2);custom.enabled=true;custom.shortAction=0;custom.start=0;
    BOOST_REQUIRE(r.b->setRecordingControl(2,custom));BOOST_REQUIRE(r.manager->setRecordingProfile(r.bn->nodeId,2));
    r.manager->requestBox(r.bn->nodeId,LooperManagerNode::BoxAction::record);r.render();
    BOOST_CHECK_EQUAL(r.b->view().state,int(LooperEngine::State::recording));BOOST_CHECK_EQUAL(r.b->view().controller,2);
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.an->nodeId);
    r.manager->requestBox(r.an->nodeId,LooperManagerNode::BoxAction::record);r.render();BOOST_CHECK_EQUAL(r.a->view().count,0);
    r.a->request(LooperEngine::Action::record);r.render();BOOST_CHECK_EQUAL(r.a->view().count,0); // own editor cannot bypass guard
    r.manager->requestBox(r.bn->nodeId,LooperManagerNode::BoxAction::record);r.render();
    BOOST_CHECK_EQUAL(r.b->view().state,int(LooperEngine::State::playing));
    r.manager->requestBox(r.an->nodeId,LooperManagerNode::BoxAction::record);r.render();BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::recording));
}
BOOST_AUTO_TEST_CASE (DirectMidiAndSettingsRoundTrip)
{
    Rig r;BOOST_REQUIRE(r.manager->setBoxMapping(r.bn->nodeId,0,{90,1}));
    r.b->request(LooperEngine::Action::record);r.render();r.b->request(LooperEngine::Action::record);r.render();
    r.press(90);r.render();BOOST_CHECK_EQUAL(r.b->view().state,int(LooperEngine::State::stopped));
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.an->nodeId);
    r.manager->learnBox(r.an->nodeId,3);r.press(91);r.render();
    BOOST_CHECK_EQUAL(r.manager->getBoxMapping(r.an->nodeId,3).cc,91);BOOST_CHECK(!r.a->getMuted());
    r.press(91);r.render();BOOST_CHECK(r.a->getMuted());
    r.manager->shortTiming=2;r.manager->shortBars=3;r.manager->longTiming=1;r.manager->navigationProfile=2;
    MemoryBlock saved;r.manager->getState(saved);LooperManagerNode restored;restored.setState(saved.getData(),int(saved.getSize()));
    BOOST_CHECK_EQUAL(restored.shortTiming.load(),2);BOOST_CHECK_EQUAL(restored.shortBars.load(),3);BOOST_CHECK_EQUAL(restored.longTiming.load(),1);
    MemoryBlock again;restored.getState(again);auto xml=parseXML(String::fromUTF8(static_cast<const char*>(again.getData()),int(again.getSize())));
    BOOST_REQUIRE(xml);BOOST_CHECK_EQUAL(xml->getChildElement(0)->getIntAttribute("mapping3"),92+(1<<8));
}
BOOST_AUTO_TEST_CASE (PendingBoxRecordingCancelsWithOneClick)
{
    struct Clock:AudioPlayHead {Optional<PositionInfo> getPosition()const override {PositionInfo p;p.setBpm(120);p.setPpqPosition(1);p.setTimeSignature(TimeSignature{4,4});p.setIsPlaying(true);return p;}} clock;
    Rig r;r.a->setQuantized(true);r.a->setPlayHead(&clock);
    r.manager->requestBox(r.an->nodeId,LooperManagerNode::BoxAction::record);r.render();BOOST_REQUIRE(r.a->view().armed);
    r.manager->requestBox(r.an->nodeId,LooperManagerNode::BoxAction::record);r.render();
    BOOST_CHECK(!r.a->view().armed);BOOST_CHECK_EQUAL(r.a->view().count,0);
}
BOOST_AUTO_TEST_CASE (NavigationCountInThenTargetProfileWaitAndCancel)
{
    struct Clock:AudioPlayHead {double beat=0;Optional<PositionInfo> getPosition()const override {PositionInfo p;p.setBpm(120);p.setPpqPosition(beat);p.setTimeSignature(TimeSignature{4,4});p.setIsPlaying(true);return p;}} clock;
    Rig r;r.manager->setPlayHead(&clock);r.b->setPlayHead(&clock);r.b->setQuantized(true);
    auto control=r.b->recordingControl(1);control.start=2;control.bars=2;BOOST_REQUIRE(r.b->setRecordingControl(1,control));
    r.manager->shortTiming=2;r.manager->shortBars=1;
    r.press(25);r.render();BOOST_REQUIRE_EQUAL(r.manager->handoffTarget.load(),r.bn->nodeId);
    for(int i=1;i<=50;++i){clock.beat=i*.16;r.render();}
    BOOST_CHECK_EQUAL(r.manager->selectedID(),r.bn->nodeId);BOOST_CHECK_GT(r.b->view().waitingAction,0);BOOST_CHECK_EQUAL(r.b->view().count,0);
    r.manager->requestBox(r.bn->nodeId,LooperManagerNode::BoxAction::record);clock.beat+=.16;r.render();BOOST_CHECK_EQUAL(r.b->view().waitingAction,0);
}
BOOST_AUTO_TEST_CASE (BoxCancelUsesRunningProfileRatherThanFutureProfile)
{
    struct Clock:AudioPlayHead {Optional<PositionInfo> getPosition()const override {PositionInfo p;p.setBpm(120);p.setPpqPosition(1);p.setTimeSignature(TimeSignature{4,4});p.setIsPlaying(true);return p;}} clock;
    Rig r;r.b->setQuantized(true);r.b->setPlayHead(&clock);r.manager->navigationProfile=0;
    r.press(25);r.render();BOOST_REQUIRE(r.b->view().armed);BOOST_REQUIRE_EQUAL(r.b->view().controller,0);
    BOOST_CHECK_EQUAL(r.manager->recordingProfile(r.bn->nodeId),1);
    r.manager->requestBox(r.bn->nodeId,LooperManagerNode::BoxAction::record);r.render();
    BOOST_CHECK(!r.b->view().armed);BOOST_CHECK_EQUAL(r.b->view().count,0);
}
BOOST_AUTO_TEST_CASE (DirectMappingCannotStealGlobalOrLooperPedals)
{
    Rig r;
    BOOST_CHECK(!r.manager->setBoxMapping(r.an->nodeId,0,{27,1}));
    BOOST_CHECK(!r.manager->setBoxMapping(r.an->nodeId,0,{23,1}));
    BOOST_REQUIRE(r.manager->setBoxMapping(r.an->nodeId,0,{90,1}));
    BOOST_CHECK(!r.manager->setBoxMapping(r.bn->nodeId,0,{90,0}));
    BOOST_CHECK(r.manager->setBoxMapping(r.bn->nodeId,0,{90,2}));
}
BOOST_AUTO_TEST_CASE (BoxCancelUsesTransportWaitController)
{
    Rig r;r.b->transportStartSupported=true;r.manager->navigationProfile=0;
    r.press(25);r.render();BOOST_REQUIRE(r.b->view().transportWaiting);BOOST_REQUIRE(r.b->transportStartRequested.load());
    r.manager->requestBox(r.bn->nodeId,LooperManagerNode::BoxAction::record);r.render();
    BOOST_CHECK(!r.b->view().transportWaiting);BOOST_CHECK(!r.b->transportStartRequested.load());BOOST_CHECK_EQUAL(r.b->view().count,0);
}
BOOST_AUTO_TEST_CASE (PromotingLinkedLooperToMasterClearsItsOldLinkMode)
{
    Rig r;BOOST_REQUIRE(r.manager->setSyncMode(r.bn->nodeId,1));BOOST_REQUIRE(r.manager->setMaster(r.bn->nodeId));
    BOOST_CHECK_EQUAL(r.manager->syncMode(r.bn->nodeId),0);
    r.press(25);r.render();BOOST_CHECK_EQUAL(r.b->view().state,int(LooperEngine::State::recording));
}
BOOST_AUTO_TEST_CASE (BoxPendingPlayAndStopCancelWithoutCaptureLock)
{
    struct Clock:AudioPlayHead {double beat=1;Optional<PositionInfo> getPosition()const override {PositionInfo p;p.setBpm(120);p.setPpqPosition(beat);p.setTimeSignature(TimeSignature{4,4});p.setIsPlaying(true);return p;}} clock;
    Rig r;r.a->request(LooperEngine::Action::record);r.render();r.a->request(LooperEngine::Action::record);r.render();
    r.a->setQuantized(true);r.a->setPlayHead(&clock);r.a->stopOnBar=true;r.a->playOnBar=true;
    r.manager->requestBox(r.an->nodeId,LooperManagerNode::BoxAction::playStop);r.render();clock.beat+=.16;
    BOOST_REQUIRE_GT(r.a->view().waitingAction,0);BOOST_CHECK(!r.a->captureBusy.load());
    r.manager->requestBox(r.an->nodeId,LooperManagerNode::BoxAction::playStop);r.render();clock.beat+=.16;
    BOOST_CHECK_EQUAL(r.a->view().waitingAction,0);BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::playing));
    r.manager->stopAll();r.render();clock.beat+=.16;
    r.manager->requestBox(r.an->nodeId,LooperManagerNode::BoxAction::playStop);r.render();clock.beat+=.16;BOOST_REQUIRE_GT(r.a->view().waitingAction,0);
    r.manager->requestBox(r.an->nodeId,LooperManagerNode::BoxAction::playStop);r.render();clock.beat+=.16;
    BOOST_CHECK_EQUAL(r.a->view().waitingAction,0);BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::stopped));
}
BOOST_AUTO_TEST_CASE (RewindPreservesStoppedStateAndLinkedPosition)
{
    Rig r;r.a->request(LooperEngine::Action::record);r.render();r.render();r.render();r.a->request(LooperEngine::Action::record);r.render();
    r.manager->requestBox(r.an->nodeId,LooperManagerNode::BoxAction::rewind);r.render();
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::playing));BOOST_CHECK_EQUAL(r.a->view().position,8);
    r.manager->stopAll();r.render();r.manager->requestBox(r.an->nodeId,LooperManagerNode::BoxAction::rewind);r.render();
    BOOST_CHECK_EQUAL(r.a->view().state,int(LooperEngine::State::stopped));BOOST_CHECK_EQUAL(r.a->view().position,0);
    // Empty structural setup uses a second rig; linked Rewind must never issue a restart.
    Rig linked;BOOST_REQUIRE(linked.manager->setMaster(linked.an->nodeId));BOOST_REQUIRE(linked.manager->setSyncMode(linked.bn->nodeId,1));
    linked.a->request(LooperEngine::Action::record);linked.render();linked.render();linked.render();linked.a->request(LooperEngine::Action::record);linked.render();
    linked.b->request(LooperEngine::Action::record);linked.render();linked.render();linked.render();linked.b->request(LooperEngine::Action::record);linked.render();
    const auto before=linked.b->view().position,length=linked.b->view().length;BOOST_REQUIRE_GT(length,0);
    linked.manager->requestBox(linked.bn->nodeId,LooperManagerNode::BoxAction::rewind);linked.render();
    BOOST_CHECK_EQUAL(linked.b->view().position,(before+8)%length);
}
BOOST_AUTO_TEST_CASE (GlobalLearningRejectsDirectMappingAndDisconnectedCaptureReleasesGate)
{
    Rig r;BOOST_REQUIRE(r.manager->setBoxMapping(r.bn->nodeId,3,{90,1}));
    r.manager->learn=1;r.press(90);r.render();BOOST_CHECK(r.manager->lastLearnRejected.load());BOOST_CHECK_EQUAL(r.manager->previousCC.load(),24);
    BOOST_CHECK(!r.b->getMuted());
    r.a->request(LooperEngine::Action::record);r.render();BOOST_REQUIRE(r.a->captureBusy.load());
    BOOST_REQUIRE(r.g.removeConnection(r.m->nodeId,r.m->getPortForChannel(PortType::Midi,0,false),r.an->nodeId,r.an->getPortForChannel(PortType::Midi,0,true)));r.g.rebuild();
    r.b->request(LooperEngine::Action::record);r.render();BOOST_CHECK_EQUAL(r.b->view().state,int(LooperEngine::State::recording));
    BOOST_CHECK_EQUAL(r.a->managerOwner.load(),0);BOOST_CHECK(!r.a->managerCaptureGate);BOOST_CHECK(!r.a->managerSoloMuted.load());
}
BOOST_AUTO_TEST_SUITE_END()
