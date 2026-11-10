// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include <boost/test/unit_test.hpp>
#include "nodes/looper.hpp"
#include "nodes/looperdisplay.hpp"
#include <element/juce/audio_formats.hpp>
using namespace element;
using namespace juce;
namespace {
using A=LooperEngine::Action;
void tick(LooperEngine& e,float value=0){float r=value;e.process(value,r,1,false);}
void base(LooperEngine& e){e.prepare(1000);e.setTransitionOptions(0,0);e.apply(A::record);for(int i=0;i<100;++i)tick(e,.1f);e.apply(A::play);}
struct Clock:AudioPlayHead {
    double ppq=0;
    Optional<PositionInfo> getPosition() const override {PositionInfo p;p.setBpm(60);p.setTimeSignature(TimeSignature{1,4});p.setPpqPosition(ppq);p.setIsPlaying(true);return p;}
};
}
BOOST_AUTO_TEST_SUITE(LooperRecordingControlTests)
BOOST_AUTO_TEST_CASE(PartialFirstPassSplitsAtTimelineZeroAndUndoIsWholeTake)
{
    LooperEngine e;base(e);for(int i=0;i<70;++i)tick(e);
    e.apply(A::overdub);for(int i=0;i<30;++i)tick(e,2);
    BOOST_REQUIRE_EQUAL(e.getLayerCount(),2); // exact ending has no empty next part
    tick(e,3);BOOST_REQUIRE_EQUAL(e.getLayerCount(),3);
    for(int f=0;f<70;++f)BOOST_CHECK_EQUAL(e.layerSample(1,0,f),0);
    BOOST_CHECK_EQUAL(e.layerSample(1,0,99),2);BOOST_CHECK_EQUAL(e.layerSample(2,0,0),3);
    BOOST_CHECK_EQUAL(e.getLayer(1).take,e.getLayer(2).take);
    e.apply(A::overdub);e.apply(A::undo);BOOST_CHECK_EQUAL(e.getLayerCount(),1);
    e.apply(A::redo);BOOST_CHECK_EQUAL(e.getLayerCount(),3);
}
BOOST_AUTO_TEST_CASE(OverwriteNeverAddsOldAudioAndRetainsUntouchedRemainder)
{
    for(bool hear:{false,true}){
        LooperEngine e;base(e);e.setCaptureBehavior(true,hear);e.apply(A::overdub);
        for(int i=0;i<100;++i)tick(e,2);
        for(int i=0;i<25;++i){float l=3,r=3;e.process(l,r,1,false);if(i>3)BOOST_CHECK_CLOSE(l,hear?2.1f:.1f,.01f);}
        e.apply(A::overdub);BOOST_REQUIRE_EQUAL(e.getLayerCount(),2);
        for(int i=0;i<100;++i)BOOST_CHECK_EQUAL(e.layerSample(1,0,i),i<25?3:2);
        e.apply(A::undo);BOOST_CHECK_EQUAL(e.getLayerCount(),1);e.apply(A::redo);BOOST_CHECK_EQUAL(e.layerSample(1,0,24),3);
    }
}
BOOST_AUTO_TEST_CASE(OverwriteMidCycleKeepsUnwrittenPrefixSilent)
{
    LooperEngine e;base(e);for(int i=0;i<70;++i)tick(e);e.setCaptureBehavior(true,false);e.apply(A::overdub);
    for(int i=0;i<40;++i)tick(e,4);e.apply(A::overdub);
    BOOST_REQUIRE_EQUAL(e.getLayerCount(),2);
    for(int i=0;i<100;++i)BOOST_CHECK_EQUAL(e.layerSample(1,0,i),(i<10||i>=70)?4:0);
}
BOOST_AUTO_TEST_CASE(ControllersPersistIndependentChoicesAndLegacyDefaults)
{
    LooperProcessor p;p.prepareToPlay(100,16);
    for(int i=0;i<4;++i){auto c=p.recordingControl(i);c.enabled=true;c.bars=i+1;c.start=2;c.overwrite=i%2;c.hearPrevious=i==3;c.shortAction=i%2;c.finishFixed=i%2;BOOST_REQUIRE(p.setRecordingControl(i,c));}
    BOOST_REQUIRE(p.setMidiMapping(6,{70,2}));BOOST_REQUIRE(p.setMidiMapping(7,{71,2}));
    MemoryBlock data;p.getStateInformation(data);LooperProcessor q;q.setStateInformation(data.getData(),int(data.getSize()));
    for(int i=0;i<4;++i){auto c=q.recordingControl(i);BOOST_CHECK_EQUAL(c.bars,i+1);BOOST_CHECK_EQUAL(c.overwrite,bool(i%2));BOOST_CHECK_EQUAL(c.hearPrevious,i==3);BOOST_CHECK(c.enabled);}
    BOOST_CHECK_EQUAL(q.getMidiMapping(7).cc,71);
    data.setSize(data.getSize()-40);static_cast<uint8*>(data.getData())[4]=14;
    q.setStateInformation(data.getData(),int(data.getSize()));
    BOOST_CHECK(!q.recordingControl(2).enabled);BOOST_CHECK_EQUAL(q.recordingControl(0).shortAction,1);BOOST_CHECK_EQUAL(q.recordingControl(1).shortAction,0);
    BOOST_CHECK_EQUAL(q.getMidiMapping(7).cc,-1);
}
BOOST_AUTO_TEST_CASE(WaitIgnoresOtherControllerSameCancelsAndSettingsLock)
{
    LooperProcessor p;Clock clock;p.setPlayHead(&clock);p.prepareToPlay(100,10);p.setLoopMode(1);
    auto c=p.recordingControl(1);c.bars=3;c.start=2;BOOST_REQUIRE(p.setRecordingControl(1,c));
    AudioBuffer<float> audio(2,10);MidiBuffer midi;
    auto run=[&]{audio.clear();p.processBlock(audio,midi);midi.clear();clock.ppq+=.1;return p.view();};
    p.request(A::overdub);auto v=run();BOOST_REQUIRE(v.armed);BOOST_CHECK_EQUAL(v.countIn,4);
    BOOST_CHECK(!p.setRecordingControl(1,c));BOOST_CHECK(!p.setMidiMapping(6,{71,1}));
    p.request(A::record);v=run();BOOST_CHECK(v.armed);BOOST_CHECK_EQUAL(v.count,0);
    p.request(A::overdub);v=run();BOOST_CHECK(!v.armed);BOOST_CHECK_EQUAL(v.count,0);
    BOOST_CHECK(p.setRecordingControl(1,c));
}
BOOST_AUTO_TEST_CASE(CustomMidiShortCaptureAndRemoval)
{
    LooperProcessor p;p.prepareToPlay(100,10);p.setCrossfade(0);p.setCaptureTail(0);
    auto c=p.recordingControl(2);c.enabled=true;BOOST_REQUIRE(p.setRecordingControl(2,c));BOOST_REQUIRE(p.setMidiMapping(6,{70,1}));
    AudioBuffer<float> audio(2,10);MidiBuffer midi;
    auto run=[&]{audio.clear();p.processBlock(audio,midi);midi.clear();return p.view();};
    auto press=[&]{midi.addEvent(MidiMessage::controllerEvent(1,70,127),0);run();midi.addEvent(MidiMessage::controllerEvent(1,70,0),0);return run();};
    BOOST_CHECK_EQUAL(press().state,int(LooperEngine::State::recording));BOOST_CHECK_EQUAL(press().state,int(LooperEngine::State::playing));
    p.request(A::stop);run();c.enabled=false;BOOST_REQUIRE(p.setRecordingControl(2,c));BOOST_CHECK_EQUAL(p.getMidiMapping(6).cc,-1);
    BOOST_CHECK_EQUAL(press().state,int(LooperEngine::State::stopped));
}
BOOST_AUTO_TEST_CASE(FixedFinishCanBeChosenIndependentlyOfRecordOrOverdub)
{
    for(bool finish:{false,true})for(auto action:{A::record,A::overdub}){
        LooperEngine e;e.prepare(100);e.setOptions(true,0,0);e.setFixedBars(1);e.setClock(60,1,4,0,true);e.setCaptureBehavior(false,false,finish?1:0);
        e.apply(action);for(int i=0;i<100;++i)tick(e,1);
        BOOST_CHECK(e.getState()==(finish?LooperEngine::State::playing:LooperEngine::State::overdubbing));
        tick(e,2);BOOST_CHECK_EQUAL(e.getLayerCount(),finish?1:2);
    }
}
BOOST_AUTO_TEST_CASE(ResetRetainsAudioAndRemovesCustomMappings)
{
    LooperProcessor p;p.prepareToPlay(100,10);AudioBuffer<float> audio(2,10);MidiBuffer midi;audio.clear();
    p.request(A::record);p.processBlock(audio,midi);p.view();p.request(A::stop);p.processBlock(audio,midi);const auto before=p.view();
    auto c=p.recordingControl(2);c.enabled=true;BOOST_REQUIRE(p.setRecordingControl(2,c));BOOST_REQUIRE(p.setMidiMapping(6,{70,1}));p.setGraceMs(20);p.setPan(.5f);
    BOOST_REQUIRE(p.resetSettings());BOOST_CHECK_EQUAL(p.view().length,before.length);BOOST_CHECK_EQUAL(p.view().count,before.count);
    BOOST_CHECK_EQUAL(p.getGraceMs(),100);BOOST_CHECK_EQUAL(p.getPan(),0);BOOST_CHECK(!p.recordingControl(2).enabled);BOOST_CHECK_EQUAL(p.getMidiMapping(6).cc,-1);
}
BOOST_AUTO_TEST_CASE(EditingFreeControllerPreservesItsSyncNextBarPreference)
{
    LooperProcessor p;p.prepareToPlay(100,16);
    std::unique_ptr<AudioProcessorEditor> editor(p.createEditor());
    auto find=[](auto&& self,Component& component)->ComboBox* {
        if(auto* box=dynamic_cast<ComboBox*>(&component);box && box->getNumItems()==2 && box->getItemText(0)=="New layer")return box;
        for(auto* child:component.getChildren())if(auto* result=self(self,*child))return result;
        return nullptr;
    };
    auto* boundary=find(find,*editor);BOOST_REQUIRE(boundary);
    boundary->setSelectedId(2,sendNotificationSync);
    BOOST_CHECK(p.recordingControl(0).overwrite);BOOST_CHECK_EQUAL(p.recordingControl(0).start,1);
}
BOOST_AUTO_TEST_CASE(IndependentCaptureOptionsRoundtripAndV15Migration)
{
    LooperProcessor p;p.prepareToPlay(100,16);p.countInClick=true;p.setAutoPasses(3);p.setTakeFeedback(false);
    for(int i=0;i<4;++i){auto c=p.recordingControl(i);c.enabled=true;c.loops=i+1;c.metronome=i%2;c.feedbackWithinTake=i<2;BOOST_REQUIRE(p.setRecordingControl(i,c));}
    MemoryBlock data;p.getStateInformation(data);LooperProcessor q;q.setStateInformation(data.getData(),int(data.getSize()));
    for(int i=0;i<4;++i){const auto c=q.recordingControl(i);BOOST_CHECK_EQUAL(c.loops,i+1);BOOST_CHECK_EQUAL(c.metronome,bool(i%2));BOOST_CHECK_EQUAL(c.feedbackWithinTake,i<2);}
    MemoryBlock malformed(data);auto* raw=static_cast<uint8*>(malformed.getData());
    const size_t offset=data.getSize()-40;uint32 packed=ByteOrder::littleEndianInt(raw+offset);
    const auto invalid=ByteOrder::swapIfBigEndian((packed&~(127u<<16))|(65u<<16));std::memcpy(raw+offset,&invalid,4);
    q.setStateInformation(malformed.getData(),int(malformed.getSize()));BOOST_CHECK_EQUAL(q.recordingControl(0).loops,1);
    // v15 stored only the lower14bits; its shared settings must migrate to every controller.
    data.setSize(data.getSize()-16);raw=static_cast<uint8*>(data.getData());raw[4]=15;
    for(int i=0;i<4;++i){const auto old=ByteOrder::swapIfBigEndian(ByteOrder::littleEndianInt(raw+offset+i*4)&16383u);std::memcpy(raw+offset+i*4,&old,4);}
    q.setStateInformation(data.getData(),int(data.getSize()));
    for(int i=0;i<4;++i){const auto c=q.recordingControl(i);BOOST_CHECK_EQUAL(c.loops,3);BOOST_CHECK(c.metronome);BOOST_CHECK(!c.feedbackWithinTake);}
}
BOOST_AUTO_TEST_CASE(EachControllerFinishesAfterItsOwnNumberOfLoopLengths)
{
    LooperProcessor p;p.prepareToPlay(1000,10);p.setCrossfade(0);p.setCaptureTail(0);
    AudioBuffer<float> audio(2,10);MidiBuffer midi;
    auto run=[&]{audio.clear();p.processBlock(audio,midi);midi.clear();return p.view();};
    p.request(A::record);for(int i=0;i<10;++i)run();p.request(A::play);run();
    for(int i=0;i<2;++i){auto c=p.recordingControl(i);c.shortAction=0;c.finishFixed=false;c.loops=i+1;c.overwrite=true;BOOST_REQUIRE(p.setRecordingControl(i,c));}
    for(int control=0;control<2;++control){p.request(control==0?A::record:A::overdub);
        for(int i=0;i<(control+1)*10-1;++i)BOOST_CHECK_EQUAL(run().state,int(LooperEngine::State::overdubbing));
        BOOST_CHECK_EQUAL(run().state,int(LooperEngine::State::playing));}
}
BOOST_AUTO_TEST_CASE(SameTakeFeedbackPolicyComesFromInitiatingController)
{
    for(bool feedback:{false,true}){
        LooperProcessor p;p.prepareToPlay(1000,10);p.setCrossfade(0);p.setCaptureTail(0);p.setFeedback(.5f);
        AudioBuffer<float> audio(2,10);MidiBuffer midi;
        auto run=[&]{for(int c=0;c<2;++c)for(int i=0;i<10;++i)audio.setSample(c,i,.1f);p.processBlock(audio,midi);midi.clear();return p.view();};
        p.request(A::record);for(int i=0;i<10;++i)run();p.request(A::play);run();p.request(A::stop);run();
        auto c=p.recordingControl(1);c.feedbackWithinTake=feedback;BOOST_REQUIRE(p.setRecordingControl(1,c));
        p.request(A::overdub);for(int i=0;i<20;++i)run();
        const auto v=p.view();BOOST_REQUIRE_EQUAL(v.count,3);
        BOOST_CHECK_CLOSE(v.layers[1].gain,feedback?.5f:1.f,.02f);
        BOOST_CHECK_CLOSE(v.layers[0].gain,.25f,.02f);
    }
}
BOOST_AUTO_TEST_CASE(CountInMetronomeIsSelectedPerController)
{
    for(int controller=0;controller<2;++controller){
        LooperProcessor p;Clock clock;p.setPlayHead(&clock);p.prepareToPlay(100,10);p.setLoopMode(1);
        for(int i=0;i<2;++i){auto c=p.recordingControl(i);c.start=2;c.bars=1;c.metronome=i==0;BOOST_REQUIRE(p.setRecordingControl(i,c));}
        AudioBuffer<float> audio(2,10);MidiBuffer midi;bool heard=false,offAfter=false;
        p.request(controller==0?A::record:A::overdub);
        for(int n=0;n<23;++n){audio.clear();midi.clear();p.processBlock(audio,midi);clock.ppq+=.1;p.view();
            for(auto m:midi)if(m.numBytes==10 && m.data[2]=='J' && m.data[3]=='C'){
                heard|=m.data[8]!=0;if(heard && m.data[8]==0)offAfter=true;
            }}
        BOOST_CHECK_EQUAL(heard,controller==0);if(controller==0)BOOST_CHECK(offAfter);
    }
}
BOOST_AUTO_TEST_CASE(HeldRecordAddsAndRecoversPressAudio)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(0,0);
    e.apply(A::record);for(int i=0;i<2000;++i)tick(e,.1f);e.apply(A::play);
    for(int i=0;i<800;++i)tick(e,.7f);
    BOOST_REQUIRE(e.beginHeldRecord(800));
    for(int i=0;i<200;++i)tick(e,.8f);
    e.endHeldRecord();
    BOOST_CHECK(e.getState()==LooperEngine::State::playing);
    BOOST_REQUIRE_EQUAL(e.getLayerCount(),2);
    BOOST_CHECK(!e.getLayer(1).replace);BOOST_CHECK(e.getLayer(0).enabled);
    BOOST_CHECK_CLOSE(e.layerSample(1,0,100),.7f,.01f);
    BOOST_CHECK_CLOSE(e.layerSample(1,0,900),.8f,.01f);
}
BOOST_AUTO_TEST_CASE(HeldFirstCapturePadsSyncAndFixedButFreeHasExactLength)
{
    for(int mode=0;mode<3;++mode){
        LooperEngine e;e.prepare(1000);e.setTransitionOptions(0,0);e.setOptions(mode!=0,100,0);
        if(mode==2)e.setFixedBars(3);
        e.setClock(60,1,4,0,true);
        for(int i=0;i<1200;++i)tick(e,.6f);
        BOOST_REQUIRE(e.beginHeldRecord(800));
        for(int i=0;i<100;++i)tick(e,.8f);e.endHeldRecord();
        BOOST_CHECK_EQUAL(e.getLength(),mode==0?900:mode==1?2000:3000);
        const int prefix=mode==0?0:400;
        for(int i=0;i<prefix;++i)BOOST_CHECK_EQUAL(e.layerSample(0,0,i),0);
        BOOST_CHECK_CLOSE(e.layerSample(0,0,prefix+10),.6f,.01f);
        if(mode)BOOST_CHECK_EQUAL(e.layerSample(0,0,e.getLength()-1),0);
    }
}
BOOST_AUTO_TEST_CASE(StopAfterPersistsPerControllerAndStopsOnlyOnCaptureEnd)
{
    LooperProcessor p;p.prepareToPlay(1000,100);
    auto c=p.recordingControl(1);c.start=0;c.stopAfter=true;BOOST_REQUIRE(p.setRecordingControl(1,c));
    AudioBuffer<float> audio(2,100);MidiBuffer midi;
    auto run=[&]{audio.clear();p.processBlock(audio,midi);midi.clear();return p.view();};
    p.request(A::overdub);BOOST_CHECK_EQUAL(run().state,1);
    p.request(A::overdub);BOOST_CHECK_EQUAL(run().state,3);
    BOOST_CHECK(p.recordingControl(1).stopAfter);BOOST_CHECK(!p.recordingControl(0).stopAfter);
    p.request(A::overdub);BOOST_CHECK_EQUAL(run().state,4);
    p.request(A::overdub);BOOST_CHECK_EQUAL(run().state,3);
}
BOOST_AUTO_TEST_CASE(HeldRecordMidiReleaseEndsWhileHoldRecordContinues)
{
    for(int hold:{1,4}){
        LooperProcessor p;p.prepareToPlay(1000,100);
        auto c=p.recordingControl(1);c.holdAction=hold;c.start=0;BOOST_REQUIRE(p.setRecordingControl(1,c));
        AudioBuffer<float> audio(2,100);MidiBuffer midi;
        auto run=[&]{for(int ch=0;ch<2;++ch)FloatVectorOperations::fill(audio.getWritePointer(ch),.3f,100);p.processBlock(audio,midi);midi.clear();return p.view();};
        midi.addEvent(MidiMessage::controllerEvent(1,23,127),0);
        for(int i=0;i<10;++i)run();
        midi.addEvent(MidiMessage::controllerEvent(1,23,0),0);const auto v=run();
        BOOST_CHECK_EQUAL(v.state,hold==4?2:1);
        if(hold==4)BOOST_CHECK_EQUAL(v.length,1000);
    }
}
BOOST_AUTO_TEST_CASE(HistoryCoherentStereoAndExpiredRangesRejected)
{
    LooperHistory history(100,2);
    for(int i=0;i<250;++i)history.push(float(i),float(-i));
    std::array<std::vector<float>,2> audio;
    BOOST_REQUIRE(history.copy(250,200,audio));
    for(int i=0;i<200;++i){BOOST_CHECK_EQUAL(audio[0][i],i+50);BOOST_CHECK_EQUAL(audio[1][i],-i-50);}
    for(int i=0;i<401;++i)history.push(0,0);
    BOOST_CHECK(!history.copy(250,200,audio));
}
BOOST_AUTO_TEST_CASE(NewSettingsRoundTripAndLegacyDefaults)
{
    LooperProcessor p;p.prepareToPlay(100,10);auto c=p.recordingControl(1);c.holdAction=4;c.stopAfter=true;
    BOOST_REQUIRE(p.setRecordingControl(1,c));BOOST_REQUIRE(p.setHistoryLimits(45,1.5));BOOST_REQUIRE(p.setMidiMapping(8,{75,2}));
    MemoryBlock data;p.getStateInformation(data);LooperProcessor q;q.prepareToPlay(100,10);q.setStateInformation(data.getData(),int(data.getSize()));
    BOOST_CHECK_EQUAL(q.getHistorySeconds(),45);BOOST_CHECK_EQUAL(q.getHistoryMinimum(),1.5);
    BOOST_CHECK_EQUAL(q.recordingControl(1).holdAction,4);BOOST_CHECK(q.recordingControl(1).stopAfter);BOOST_CHECK_EQUAL(q.getMidiMapping(8).cc,75);
}
BOOST_AUTO_TEST_CASE(TransportRequestWaitsWithoutResettingPosition)
{
    struct PausedClock:AudioPlayHead {bool playing=false;Optional<PositionInfo> getPosition()const override{PositionInfo p;p.setBpm(60);p.setTimeSignature(TimeSignature{4,4});p.setPpqPosition(7.5);p.setIsPlaying(playing);return p;}} clock;
    LooperProcessor p;p.prepareToPlay(1000,10);p.setPlayHead(&clock);p.transportStartSupported=true;
    auto c=p.recordingControl(1);c.start=0;BOOST_REQUIRE(p.setRecordingControl(1,c));
    AudioBuffer<float> audio(2,10);MidiBuffer midi;audio.clear();p.request(A::overdub);p.processBlock(audio,midi);
    BOOST_CHECK(p.transportStartRequested.exchange(false));BOOST_CHECK(p.view().armed);
    clock.playing=true;p.processBlock(audio,midi);BOOST_CHECK_EQUAL(p.view().state,1);
    BOOST_CHECK(!p.transportStartRequested.load());
}
BOOST_AUTO_TEST_CASE(HistoryCaptureSnapshotAndSelectionImport)
{
    LooperProcessor p;p.prepareToPlay(100,10);AudioBuffer<float> audio(2,10);MidiBuffer midi;
    auto run=[&]{for(int c=0;c<2;++c)FloatVectorOperations::fill(audio.getWritePointer(c),.2f,10);p.processBlock(audio,midi);midi.clear();p.view();};
    for(int i=0;i<20;++i)run();BOOST_REQUIRE(p.requestHistory(true));run();
    for(int i=0;i<60 && !p.historySelection;++i){Timer::callPendingTimersSynchronously();Thread::sleep(10);}
    BOOST_REQUIRE(p.historySelection);BOOST_CHECK_EQUAL(p.historySelection->audio[0].size(),200);
    BOOST_CHECK(!p.importHistorySelection(0,50));BOOST_REQUIRE(p.importHistorySelection(0,150));
    for(int i=0;i<60 && p.view().count==0;++i){Timer::callPendingTimersSynchronously();run();Thread::sleep(10);}
    BOOST_CHECK_EQUAL(p.view().length,150);BOOST_CHECK_EQUAL(p.view().count,1);
}

BOOST_AUTO_TEST_CASE(SyncHistoryUsesCompletedCycleAndRejectsSeekOrInsufficientHistory)
{
    Clock clock;LooperProcessor p;p.prepareToPlay(100,10);p.setPlayHead(&clock);p.setLoopMode(2);p.setFixedBarCount(2);
    AudioBuffer<float> audio(2,10);MidiBuffer midi;
    auto run=[&](float value){for(int c=0;c<2;++c)FloatVectorOperations::fill(audio.getWritePointer(c),value,10);p.processBlock(audio,midi);clock.ppq+=.1;midi.clear();p.view();};
    for(int i=0;i<35;++i)run(i<20?.25f:.75f);
    BOOST_REQUIRE(p.requestHistory());run(.75f);
    for(int i=0;i<60 && p.view().count==0;++i){Timer::callPendingTimersSynchronously();run(.75f);Thread::sleep(10);}
    BOOST_REQUIRE_EQUAL(p.view().length,200);BOOST_REQUIRE_EQUAL(p.view().count,1);
    const auto file=File::getSpecialLocation(File::tempDirectory).getNonexistentChildFile("capture-cycle",".wav",false);
    BOOST_REQUIRE(p.exportLayer(p.view().layers[0].id,file).wasOk());
    AudioFormatManager formats;formats.registerBasicFormats();std::unique_ptr<AudioFormatReader> reader(formats.createReaderFor(file));BOOST_REQUIRE(reader);
    AudioBuffer<float> captured(2,200);BOOST_REQUIRE(reader->read(&captured,0,200,0,true,true));
    for(int i=0;i<200;++i)BOOST_CHECK_CLOSE(captured.getSample(0,i),.25f,.01f);file.deleteFile();
    const int expected=(int(std::llround(clock.ppq*100))+10)%200;p.request(A::play);run(0);BOOST_CHECK_EQUAL(p.view().position,expected);
    clock.ppq=20;p.requestHistory();run(.3f);Timer::callPendingTimersSynchronously();
    BOOST_CHECK_EQUAL(p.view().count,1);
}
BOOST_AUTO_TEST_CASE(HeldFixedFirstCaptureWritesAfterRecoveredAudioAtCorrectOffset)
{
    LooperEngine e;e.prepare(1000);e.setOptions(true,0,0);e.setFixedBars(3);e.setClock(60,1,4,0,true);e.setTransitionOptions(0,0);
    for(int i=0;i<1200;++i)tick(e,.3f);BOOST_REQUIRE(e.beginHeldRecord(800));
    for(int i=0;i<100;++i)tick(e,.7f);e.endHeldRecord();
    BOOST_CHECK_EQUAL(e.getLength(),3000);
    BOOST_CHECK_CLOSE(e.layerSample(0,0,1205),.7f,.01f);
    BOOST_CHECK_EQUAL(e.layerSample(0,0,1305),0);
}

BOOST_AUTO_TEST_CASE(HeldAddAcrossWrapHasValidOneTakeAndUndoKeepsOriginal)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(0,0);e.apply(A::record);
    for(int i=0;i<1500;++i)tick(e,.1f);e.apply(A::play);
    for(int i=0;i<1400;++i)tick(e,.2f);BOOST_REQUIRE(e.beginHeldRecord(800));
    for(int i=0;i<300;++i)tick(e,.3f);e.endHeldRecord();
    BOOST_REQUIRE_EQUAL(e.getLayerCount(),3);BOOST_CHECK(e.getLayer(1).take!=0);
    BOOST_CHECK_EQUAL(e.getLayer(1).take,e.getLayer(2).take);
    LooperEngine loaded;loaded.prepare(1000);loaded.load(e.snapshot());BOOST_CHECK_EQUAL(loaded.getLayerCount(),3);
    e.apply(A::undo);BOOST_CHECK_EQUAL(e.getLayerCount(),1);BOOST_CHECK_CLOSE(e.layerSample(0,0,0),.1f,.01f);
}

BOOST_AUTO_TEST_CASE(HeldHistoricalPassesSplitAndRespectRollingOrStopLimit)
{
    for(bool roll:{false,true}) {
        LooperEngine e;e.prepare(1000);e.setLimits(10,4,roll);e.setTransitionOptions(0,0);
        e.apply(A::record);for(int i=0;i<200;++i)tick(e,.1f);e.apply(A::play);
        for(int i=0;i<900;++i)tick(e,float(i));
        BOOST_REQUIRE(e.beginHeldRecord(800));
        // Press was at phase 100: partial + three full passes + partial.
        const auto pending=e.snapshot(); // all historical core samples still use fallback
        BOOST_REQUIRE(e.isClonePending());
        e.finalizeOffline();BOOST_REQUIRE_EQUAL(e.getLayerCount(),5);
        const auto copied=e.snapshot();BOOST_REQUIRE_EQUAL(pending.layers.size(),copied.layers.size());
        for(size_t i=0;i<pending.layers.size();++i)BOOST_CHECK(pending.layers[i].audio==copied.layers[i].audio);
        const auto take=e.getLayer(1).take;BOOST_CHECK(take!=0);
        for(int i=1;i<5;++i){BOOST_CHECK_EQUAL(e.getLayer(i).take,take);BOOST_CHECK_EQUAL(e.getLayer(i).part,i-1+(roll?1:0));}
        if(roll) {BOOST_CHECK_EQUAL(e.layerSample(1,0,0),200);BOOST_CHECK_EQUAL(e.layerSample(4,0,99),899);BOOST_CHECK_EQUAL(e.layerSample(4,0,100),0);}
        else {BOOST_CHECK_EQUAL(e.layerSample(1,0,99),0);BOOST_CHECK_EQUAL(e.layerSample(1,0,100),100);BOOST_CHECK_EQUAL(e.layerSample(4,0,199),799);}
        e.apply(A::undo);BOOST_CHECK_EQUAL(e.getLayerCount(),1);
    }
}
BOOST_AUTO_TEST_CASE(FirstHeldFixedCaptureCanCrossBoundaryBeforeRecognition)
{
    LooperEngine e;e.prepare(1000);e.setOptions(true,0,0);e.setFixedBars(1);e.setClock(60,1,4,0,true);e.setTransitionOptions(0,0);
    for(int i=0;i<1500;++i)tick(e,float(i));
    BOOST_REQUIRE(e.beginHeldRecord(800));e.finalizeOffline();
    BOOST_CHECK_EQUAL(e.getLength(),1000);BOOST_REQUIRE_EQUAL(e.getLayerCount(),2);
    BOOST_CHECK_EQUAL(e.layerSample(0,0,699),0);BOOST_CHECK_EQUAL(e.layerSample(0,0,700),700);
    BOOST_CHECK_EQUAL(e.layerSample(1,0,0),1000);BOOST_CHECK_EQUAL(e.layerSample(1,0,499),1499);
    BOOST_CHECK_EQUAL(e.layerSample(1,0,500),0);
}
BOOST_AUTO_TEST_CASE(NoActionHoldNeverStartsTransport)
{
    LooperProcessor p;p.prepareToPlay(1000,100);p.transportStartSupported=true;
    auto c=p.recordingControl(1);c.holdAction=0;BOOST_REQUIRE(p.setRecordingControl(1,c));
    AudioBuffer<float> audio(2,100);MidiBuffer midi;midi.addEvent(MidiMessage::controllerEvent(1,23,127),0);
    for(int i=0;i<10;++i){audio.clear();p.processBlock(audio,midi);midi.clear();}
    BOOST_CHECK(!p.transportStartRequested.load());BOOST_CHECK(!p.view().armed);BOOST_CHECK_EQUAL(p.view().count,0);
}
BOOST_AUTO_TEST_CASE(HeldTransportWaitRetainsOriginalPressAndRoundTrips)
{
    struct StartClock:AudioPlayHead {bool playing=false;double ppq=0;Optional<PositionInfo> getPosition()const override{PositionInfo p;p.setBpm(60);p.setTimeSignature(TimeSignature{1,4});p.setPpqPosition(ppq);p.setIsPlaying(playing);return p;}} clock;
    for(int mode:{0,1}) {
        LooperProcessor p;p.prepareToPlay(1000,100);p.setPlayHead(&clock);p.transportStartSupported=true;p.setLoopMode(mode);
        auto c=p.recordingControl(1);c.holdAction=4;BOOST_REQUIRE(p.setRecordingControl(1,c));
        AudioBuffer<float> audio(2,100);MidiBuffer midi;
        auto run=[&](bool impulse){audio.clear();if(impulse){audio.setSample(0,0,.7f);audio.setSample(1,0,.7f);}p.processBlock(audio,midi);midi.clear();p.view();if(clock.playing)clock.ppq+=.1;};
        clock.playing=false;clock.ppq=0;midi.addEvent(MidiMessage::controllerEvent(1,23,127),0);
        for(int i=0;i<9;++i)run(i==0);
        BOOST_REQUIRE(p.transportStartRequested.exchange(false));clock.playing=true;run(false);
        midi.addEvent(MidiMessage::controllerEvent(1,23,0),0);run(false);
        MemoryBlock data;p.getStateInformation(data);LooperProcessor loaded;loaded.prepareToPlay(1000,100);loaded.setStateInformation(data.getData(),int(data.getSize()));
        BOOST_REQUIRE_EQUAL(loaded.view().count,1);BOOST_CHECK_EQUAL(loaded.view().length,mode?2000:1000);
        const auto file=File::getSpecialLocation(File::tempDirectory).getNonexistentChildFile("held-original-press",".wav",false);
        BOOST_REQUIRE(loaded.exportLayer(loaded.view().layers[0].id,file).wasOk());AudioFormatManager formats;formats.registerBasicFormats();std::unique_ptr<AudioFormatReader> reader(formats.createReaderFor(file));BOOST_REQUIRE(reader);
        AudioBuffer<float> capture(2,int(reader->lengthInSamples));BOOST_REQUIRE(reader->read(&capture,0,capture.getNumSamples(),0,true,true));
        BOOST_CHECK_CLOSE(capture.getSample(0,mode?100:0),.7f,.01f);file.deleteFile();
    }
}
BOOST_AUTO_TEST_CASE(RestoreInvalidatesFrozenHistoryAndPendingImport)
{
    LooperProcessor p;p.prepareToPlay(100,10);MemoryBlock empty;p.getStateInformation(empty);
    AudioBuffer<float> audio(2,10);MidiBuffer midi;
    auto run=[&]{audio.clear();p.processBlock(audio,midi);midi.clear();p.view();};
    for(int i=0;i<20;++i)run();BOOST_REQUIRE(p.requestHistory(true));run();
    for(int i=0;i<60 && !p.historySelection;++i){Timer::callPendingTimersSynchronously();Thread::sleep(10);}
    BOOST_REQUIRE(p.historySelection);BOOST_REQUIRE(p.importHistorySelection(0,150));
    for(int i=0;i<60 && !p.hasHistoryImport();++i){Timer::callPendingTimersSynchronously();Thread::sleep(10);}
    BOOST_REQUIRE(p.hasHistoryImport());p.setStateInformation(empty.getData(),int(empty.getSize()));run();
    for(int i=0;i<10;++i){Timer::callPendingTimersSynchronously();Thread::sleep(10);}
    BOOST_CHECK_EQUAL(p.view().count,0);BOOST_CHECK(!p.historySelection);BOOST_CHECK(!p.hasImport());
    // Also invalidate the ticket before its asynchronous copy has begun.
    for(int i=0;i<20;++i)run();BOOST_REQUIRE(p.requestHistory(true));run();
    p.setStateInformation(empty.getData(),int(empty.getSize()));
    for(int i=0;i<10;++i){Timer::callPendingTimersSynchronously();Thread::sleep(10);}
    BOOST_CHECK(!p.historySelection);BOOST_CHECK_EQUAL(p.view().count,0);
}

BOOST_AUTO_TEST_CASE(HeldHistoricalAudioRespectsLoopLimitAndOverwrite)
{
    for(bool overwrite:{false,true}) {
        LooperEngine e;e.prepare(1000);e.setTransitionOptions(0,0);e.setOptions(false,0,1);e.setCaptureBehavior(overwrite,false);
        e.apply(A::record);for(int i=0;i<200;++i)tick(e,.1f);e.apply(A::play);
        for(int i=0;i<900;++i)tick(e,float(i));BOOST_REQUIRE(e.beginHeldRecord(800));
        BOOST_CHECK(e.getState()==LooperEngine::State::playing);e.finalizeOffline();
        BOOST_REQUIRE_EQUAL(e.getLayerCount(),overwrite?2:3);
        BOOST_CHECK_EQUAL(e.layerSample(1,0,100),100);
        if(overwrite)BOOST_CHECK_EQUAL(e.layerSample(1,0,99),299);
        else{BOOST_CHECK_EQUAL(e.layerSample(1,0,0),0);BOOST_CHECK_EQUAL(e.layerSample(2,0,99),299);}
    }
}

BOOST_AUTO_TEST_CASE(DisplayTracksActualCountInAndKeepsRecordingUntilCaptureEnds)
{
    Clock clock;LooperProcessor p;p.prepareToPlay(100,10);p.setPlayHead(&clock);p.setLoopMode(1);
    auto c=p.recordingControl(1);c.start=2;c.bars=2;c.metronome=false;BOOST_REQUIRE(p.setRecordingControl(1,c));
    AudioBuffer<float> audio(2,10);MidiBuffer midi;
    auto run=[&]{audio.clear();p.processBlock(audio,midi);midi.clear();clock.ppq+=.1;return looperDisplayStatus(p.view(),true,false,false,-40);};
    p.request(A::overdub);auto display=run();BOOST_CHECK_EQUAL(display.main,"3");BOOST_CHECK(display.countdown);
    for(int i=1;i<20;++i)display=run();BOOST_CHECK_EQUAL(display.main,"1");
    for(int i=20;i<32;++i)display=run();BOOST_CHECK_EQUAL(display.main,"Recording");BOOST_CHECK(!display.countdown);
    for(int i=0;i<5;++i)BOOST_CHECK_EQUAL(run().main,"Recording");
    p.request(A::stop);display=run();
    if(p.view().pending){BOOST_CHECK_EQUAL(display.main,"Recording");BOOST_CHECK_EQUAL(display.detail,"Finishing at next bar");}
    for(int i=0;i<15;++i)display=run();BOOST_CHECK_EQUAL(display.main,"Stopped");
}
BOOST_AUTO_TEST_CASE(DisplayPrioritiesAndNonExclusiveMuteBadges)
{
    LooperProcessor::View v;v.loopMode=1;v.count=1;v.length=400;v.barFrames=100;v.state=4;v.armed=true;
    auto d=looperDisplayStatus(v,true,true,true,-40,"Not enough history");
    BOOST_CHECK_EQUAL(d.main,"Recording");BOOST_CHECK_EQUAL(d.detail,"Add");BOOST_CHECK_EQUAL(d.flags,"MUTE  BYPASS");
    v.overwrite=true;BOOST_CHECK_EQUAL(looperDisplayStatus(v,true,false,false,-40).detail,"Overwrite");
    v.state=5;BOOST_CHECK_EQUAL(looperDisplayStatus(v,true,false,false,-40).detail,"Replace");
    v.state=2;v.soundWaiting=true;d=looperDisplayStatus(v,true,false,false,-32);BOOST_CHECK_EQUAL(d.main,"Waiting for sound");BOOST_CHECK_EQUAL(d.detail,"-32 dB");
    v.soundWaiting=false;v.waitingAction=int(A::play)+1;BOOST_CHECK_EQUAL(looperDisplayStatus(v,true,false,false,-40).main,"Play in 1");
    v.waitingAction=int(A::stop)+1;BOOST_CHECK_EQUAL(looperDisplayStatus(v,true,false,false,-40).main,"Stop in 1");
    v.armed=false;BOOST_CHECK_EQUAL(looperDisplayStatus(v,true,false,false,-40).main,"Playing");
    v.state=3;BOOST_CHECK_EQUAL(looperDisplayStatus(v,true,false,false,-40).main,"Stopped");
    v.count=0;v.state=0;v.linkMode=2;v.linkBase=0;d=looperDisplayStatus(v,true,false,false,-40);BOOST_CHECK_EQUAL(d.main,"Empty");BOOST_CHECK_EQUAL(d.detail,"Waiting for master");
    BOOST_CHECK_EQUAL(looperDisplayStatus(v,false,false,false,-40).main,"Audio offline");
}
BOOST_AUTO_TEST_CASE(ManagerDisplaySummarizesGroupAndPrioritizesActions)
{
    std::vector<ManagerDisplayMember> members;
    auto display=[&](bool pending=false,String target={},int bars=0,bool clock=false,float volume=1.f){
        return managerDisplayStatus(members,pending,target,bars,clock,"Overdub",volume);
    };
    BOOST_CHECK_EQUAL(display().main,"No loopers");
    members.resize(2);members[0].name="Rhythm";members[1].name="Bass";
    BOOST_CHECK_EQUAL(display().main,"No loops");
    members[1].content=true;BOOST_CHECK_EQUAL(display().main,"Stopped");
    members[1].playing=true;
    auto d=display();BOOST_CHECK_EQUAL(d.main,"Playing");BOOST_CHECK_EQUAL(d.detail,"1 of 2 loopers");
    // An empty/stopped first (selected) looper must not hide another's playback.
    members[0].status.detail="Waiting for master";
    BOOST_CHECK_EQUAL(display().detail,"1 of 2 loopers");
    members[1].muted=true;d=display();BOOST_CHECK_EQUAL(d.main,"Playing");BOOST_CHECK_EQUAL(d.flags,"All loops muted");
    members[1].solo=true;BOOST_CHECK(display().flags.contains("SOLO"));
    BOOST_CHECK(display(false,{},0,false,0).flags.contains("Volume 0"));
    members[1].muted=false;members[1].solo=false;members[1].bypass=true;
    BOOST_CHECK_EQUAL(display().flags,"Playback silenced");members[1].bypass=false;
    members[1].recording=true;members[1].status.main="Recording";members[1].status.detail="Add";
    d=display();BOOST_CHECK_EQUAL(d.main,"Recording");BOOST_CHECK_EQUAL(d.detail,"Bass · Add");
    members[0].waiting=true;members[0].status.main="3";members[0].status.detail="Bars to recording";members[0].status.countdown=true;
    d=display();BOOST_CHECK_EQUAL(d.main,"3");BOOST_CHECK(d.countdown);BOOST_CHECK_EQUAL(d.detail,"Rhythm · Bars to recording");
    d=display(true);BOOST_CHECK_EQUAL(d.main,"1");BOOST_CHECK(d.countdown);BOOST_CHECK_EQUAL(d.detail,"Bar to Play all");
    d=display(false,"Percussion",2);BOOST_CHECK_EQUAL(d.main,"2");BOOST_CHECK_EQUAL(d.detail,"Percussion · Overdub");
    // Clock rejection clears the pending target in the engine; keep its explanation.
    BOOST_CHECK_EQUAL(display(false,{},0,true).main,"Waiting for clock");
    members[0].online=false;members[1].online=false;
    d=display(true,"Percussion",2);BOOST_CHECK_EQUAL(d.main,"Audio offline");BOOST_CHECK(!d.countdown);
}
BOOST_AUTO_TEST_CASE(OverlappingRecordingPedalReleaseCannotFinishFreshRecord)
{
    for(bool releaseOwnerFirst:{false,true}) {
        LooperProcessor p;p.prepareToPlay(1000,1);p.setCrossfade(0);p.setCaptureTail(0);
        AudioBuffer<float> audio(2,1);MidiBuffer midi;
        auto run=[&](int frames){for(int n=0;n<frames;++n){audio.clear();p.processBlock(audio,midi);midi.clear();p.view();}return p.view();};
        auto edge=[&](int cc,int value){midi.addEvent(MidiMessage::controllerEvent(1,cc,value),0);return run(1);};
        p.request(A::record);run(2000);p.request(A::play);run(1);
        edge(20,127);run(805); // Record hold starts a fresh capture.
        BOOST_REQUIRE_EQUAL(p.view().state,int(LooperEngine::State::recording));
        edge(23,127);run(10);
        if(releaseOwnerFirst){edge(20,0);edge(23,0);}else{edge(23,0);edge(20,0);}
        auto v=run(30);BOOST_CHECK_EQUAL(v.state,int(LooperEngine::State::recording));BOOST_CHECK_EQUAL(v.count,1);
        // A genuinely new gesture may finish recording.
        edge(23,127);v=edge(23,0);BOOST_CHECK_EQUAL(v.state,int(LooperEngine::State::playing));
    }
}
BOOST_AUTO_TEST_CASE(LinkedInitialCaptureUsesLayerPolicyAtMasterBoundary)
{
    for(bool sound:{false,true})for(bool overwrite:{false,true})for(bool finish:{false,true}) {
        LooperEngine e;e.prepare(1000);e.setTransitionOptions(0,0);e.setOptions(false,100,0);
        e.setCaptureBehavior(overwrite,false,finish?1:0);e.setLinkContext(2,1000,500,0);
        e.setPerformanceOptions(true,false,false,false,sound,2,.05f);e.applyPerformance(A::overdub);
        for(int i=0;i<500;++i)tick(e,.2f);
        BOOST_CHECK_EQUAL(e.getLength(),1000);BOOST_CHECK_EQUAL(e.getLayerCount(),1);
        BOOST_CHECK(e.getState()==(finish?LooperEngine::State::playing:LooperEngine::State::overdubbing));
        tick(e,.3f);BOOST_CHECK_EQUAL(e.getLayerCount(),finish||overwrite?1:2);
        if(!finish&&!overwrite){BOOST_CHECK_EQUAL(e.getLayer(0).take,e.getLayer(1).take);BOOST_CHECK_EQUAL(e.layerSample(0,0,100),0);e.apply(A::undo);BOOST_CHECK_EQUAL(e.getLayerCount(),0);}
    }
}
BOOST_AUTO_TEST_CASE(CountInIncludesPartialLeadingBarAndBeat)
{
    LooperEngine e;e.prepare(1000);e.setOptions(true,100,0);e.setClock(60,4,4,2,true);
    e.setPerformanceOptions(true,false,false,false,true,2,.1f);e.applyPerformance(A::overdub);
    BOOST_CHECK_EQUAL(e.getCountInRemaining(),3);BOOST_CHECK_EQUAL(e.getBarPhase(),2000);
    for(int i=0;i<2000;++i)tick(e);BOOST_CHECK_EQUAL(e.getCountInRemaining(),2);BOOST_CHECK_EQUAL(e.getBarPhase(),0);
    LooperProcessor::View v;v.armed=true;v.countIn=3;v.barPhase=2000;v.barFrames=4000;v.numerator=4;
    auto d=looperDisplayStatus(v,true,false,false,-40);BOOST_CHECK_EQUAL(d.main,"3");BOOST_CHECK_EQUAL(d.detail,"Beat 3 / 4");
}

BOOST_AUTO_TEST_CASE(FirstRecordingGestureOwnsAllFourProfilesUntilRelease)
{
    for(int owner=0;owner<4;++owner)for(int intruder=0;intruder<4;++intruder)if(owner!=intruder)
    for(bool ownerReleasedFirst:{false,true}) {
        LooperProcessor p;p.prepareToPlay(1000,100);
        const int cc[]{20,23,70,71};
        for(int i=0;i<4;++i){auto c=p.recordingControl(i);c.enabled=true;c.start=0;c.holdAction=4;c.stopAfter=false;BOOST_REQUIRE(p.setRecordingControl(i,c));}
        BOOST_REQUIRE(p.setMidiMapping(6,{70,1}));BOOST_REQUIRE(p.setMidiMapping(7,{71,1}));
        AudioBuffer<float> audio(2,100);MidiBuffer midi;
        auto run=[&](int n=1){for(int i=0;i<n;++i){audio.clear();p.processBlock(audio,midi);midi.clear();p.view();}return p.view();};
        auto edge=[&](int i,int value){midi.addEvent(MidiMessage::controllerEvent(1,cc[i],value),0);return run();};
        edge(owner,127);edge(intruder,127);run(10);
        BOOST_REQUIRE_EQUAL(p.view().state,int(LooperEngine::State::recording));BOOST_CHECK_EQUAL(p.view().controller,owner);
        if(ownerReleasedFirst){edge(owner,0);run();edge(intruder,0);}else{edge(intruder,0);BOOST_CHECK_EQUAL(p.view().state,int(LooperEngine::State::recording));edge(owner,0);}
        BOOST_CHECK_EQUAL(p.view().state,int(LooperEngine::State::playing));BOOST_CHECK_EQUAL(p.view().count,1);
    }
}

BOOST_AUTO_TEST_CASE(RC2DurationPoliciesUseOwnCycleAndCaptureDuration)
{
    for(int policy=0;policy<4;++policy)for(int offset:{0,40}) {
        LooperEngine e;base(e);for(int n=0;n<offset;++n)tick(e);
        e.setOptions(false,0,policy>=2?1:0);e.setCaptureBehavior(false,false,policy==1?1:0);e.setFinishAtBoundary(policy==3);
        e.apply(A::overdub);
        const int expected=policy==1?100-offset:policy==3&&offset>0?200-offset:100;
        for(int n=0;n<expected-1;++n)tick(e,.2f);
        BOOST_REQUIRE(e.getState()==LooperEngine::State::overdubbing);tick(e,.2f);
        BOOST_CHECK(e.getState()==(policy==0?LooperEngine::State::overdubbing:LooperEngine::State::playing));
    }
}
BOOST_AUTO_TEST_CASE(RC2ZeroExtraBarsMeansNextBoundaryAndRoundPolicyPersists)
{
    LooperEngine e;e.prepare(1000);e.setOptions(true,0,0);e.setClock(60,4,4,2,true);
    e.setPerformanceOptions(true,false,false,true,true,0,.1f);e.applyPerformance(A::overdub);
    BOOST_CHECK_EQUAL(e.getCountInRemaining(),1);for(int n=0;n<2000;++n)tick(e);tick(e);
    BOOST_CHECK(e.getState()==LooperEngine::State::recording);
    LooperProcessor p;auto c=p.recordingControl(1);c.start=2;c.bars=0;c.loops=3;c.finishAtBoundary=true;BOOST_REQUIRE(p.setRecordingControl(1,c));
    MemoryBlock data;p.getStateInformation(data);LooperProcessor restored;restored.setStateInformation(data.getData(),int(data.getSize()));
    auto saved=restored.recordingControl(1);BOOST_CHECK_EQUAL(saved.bars,0);BOOST_CHECK_EQUAL(saved.loops,3);BOOST_CHECK(saved.finishAtBoundary);
}

BOOST_AUTO_TEST_SUITE_END()
