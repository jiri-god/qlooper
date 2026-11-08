// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include <boost/test/unit_test.hpp>
#include "fixture/PreparedGraph.h"
#include "engine/ionode.hpp"
#include "nodes/songmanager.hpp"
#include "nodes/songmanagereditor.hpp"
#include "nodes/audioprocessor.hpp"
#include "nodes/looper.hpp"
#include "nodes/loopermanager.hpp"
#include <element/node.h>
#include <element/audioengine.hpp>
#include "engine/rootgraph.hpp"
using namespace element;
using namespace juce;
namespace {
struct MidiSpy:Processor {
    int presses=0,releases=0,zeroVolume=0,notesOff=0;
    MidiSpy():Processor(0) {}
    bool wantsContext() const noexcept override {return true;}
    void refreshPorts() override {if(getNumPorts())return;PortList p;p.add(PortType::Midi,0,0,"in","in",true);setPorts(p);}
    void prepareToRender(double,int) override {}
    void releaseResources() override {}
    void render(RenderContext& rc) override {for(auto m:*rc.midi.getWriteBuffer(0)) {
        auto msg=m.getMessage();if(msg.isController()&&msg.getControllerNumber()==20) {if(msg.getControllerValue()>0)++presses;else ++releases;}
        if(msg.isController()&&msg.getControllerNumber()==7&&msg.getControllerValue()==0)++zeroVolume;
        if(msg.isAllNotesOff())++notesOff;
    }}
    void getPluginDescription(PluginDescription&) const override {}
    void getState(MemoryBlock&) override {}
    void setState(const void*,int) override {}
};
struct SongHead:AudioPlayHead {
    double beat=0; bool running=false;int numerator=4,denominator=4;
    Optional<PositionInfo> getPosition() const override {
        PositionInfo p;p.setBpm(120);p.setPpqPosition(beat);p.setTimeSignature(TimeSignature{numerator,denominator});p.setIsPlaying(running);return p;
    }
};
struct SongRig {
    PreparedGraph fixture;GraphNode& graph=fixture.graph;
    SongManagerNode* song=new SongManagerNode();
    GraphNode *a,*b;Processor *input,*output;SongHead head;
    AudioBuffer<float> audio{2,8};MidiBuffer midi;
    SongRig(double rate=100):fixture(rate,8) {
        input=graph.addNode(new IONode(IONode::audioInputNode));output=graph.addNode(new IONode(IONode::audioOutputNode));
        graph.addNode(song);
        a=addSection("Verse");b=addSection("Chorus");
        graph.setPlayHead(&head);
        song->setOrder({a->nodeId,b->nodeId,a->nodeId});graph.rebuild();
    }
    GraphNode* addSection(const String& name) {
        auto* section=new GraphNode(*element::test::context());(void)name;graph.addNode(section);
        auto* in=section->addNode(new IONode(IONode::audioInputNode));
        auto* out=section->addNode(new IONode(IONode::audioOutputNode));
        for(int ch=0;ch<2;++ch) {
            BOOST_REQUIRE(section->connectChannels(PortType::Audio,in->nodeId,ch,out->nodeId,ch));
            BOOST_REQUIRE(graph.connectChannels(PortType::Audio,input->nodeId,ch,section->nodeId,ch));
            BOOST_REQUIRE(graph.connectChannels(PortType::Audio,section->nodeId,ch,output->nodeId,ch));
        }
        section->rebuild();return section;
    }
    void cc(int value,int at=0) {midi.addEvent(MidiMessage::controllerEvent(1,30,value),at);}
    void run() {for(int ch=0;ch<2;++ch)for(int s=0;s<8;++s)audio.setSample(ch,s,1);RenderContext rc(audio,audio,midi,8);graph.render(rc);midi.clear();
        // Drain display FIFOs just as the live editor does; otherwise a long
        // scripted run observes the first four blocks rather than current state.
        auto observe=[&](auto&& self,GraphNode& g)->void {
            for(int i=0;i<g.getNumNodes();++i) {
                auto* node=g.getNode(i);
                if(auto* looper=dynamic_cast<LooperProcessor*>(node->getAudioProcessor()))looper->view();
                if(auto* child=dynamic_cast<GraphNode*>(node))self(self,*child);
            }
        };
        observe(observe,graph);head.beat+=head.running?16.0/graph.getSampleRate():0;}
};
}
BOOST_AUTO_TEST_SUITE(SongManagerTests)
BOOST_AUTO_TEST_CASE(LiveClockOverviewUsesMeterAndStopsWithoutInventedProgress) {
    SongRig r;r.head.running=true;r.head.numerator=3;r.head.denominator=8;r.head.beat=0.59;
    r.run(); // 0.75 quarter notes = half of a 3/8 bar.
    BOOST_CHECK(r.song->displayClockRunning.load());
    BOOST_CHECK_CLOSE(r.song->displayBarPhase.load(),0.5f,0.01);
    BOOST_CHECK_EQUAL(r.song->displayBeat.load(),2);
    BOOST_CHECK_EQUAL(r.song->displayBeatsPerBar.load(),3);
    r.head.running=false;r.run();
    BOOST_CHECK(!r.song->displayClockRunning.load());
    BOOST_CHECK_EQUAL(r.song->displayBarPhase.load(),0);
    BOOST_CHECK_EQUAL(r.song->displayBeat.load(),0);
}
BOOST_AUTO_TEST_CASE(LiveOverviewWithRecordedSectionCanRender) {
    SongRig r;auto* looper=new LooperProcessor();
    r.a->addNode(new AudioProcessorNode(looper));r.a->rebuild();r.graph.rebuild();
    r.song->next();r.run();looper->request(LooperEngine::Action::record);r.run();
    looper->view();
    looper->request(LooperEngine::Action::play);r.run();
    ValueTree data(types::Node);data.setProperty(tags::object,r.song,nullptr);
    Node node(data,false);SongManagerEditor editor(node);
    BOOST_CHECK_LE(editor.getHeight(),350);
    TextButton* settings=nullptr;ComboBox* sectionPicker=nullptr;
    for(auto* c:editor.getChildren()) {
        if(auto* b=dynamic_cast<TextButton*>(c);b && b->getButtonText()=="Settings")settings=b;
        if(auto* combo=dynamic_cast<ComboBox*>(c);combo && combo->getComponentID()=="song-sections")sectionPicker=combo;
    }
    BOOST_REQUIRE(settings);BOOST_REQUIRE(sectionPicker);BOOST_CHECK(sectionPicker->isVisible());
    settings->onClick();BOOST_CHECK(!sectionPicker->isVisible());
    if(std::getenv("EL_SONG_EDITOR_SNAPSHOT")) {
        auto stream=File("/tmp/jgod-song-settings.png").createOutputStream();BOOST_REQUIRE(stream);
        stream->setPosition(0);stream->truncate();PNGImageFormat().writeImageToStream(editor.createComponentSnapshot(editor.getLocalBounds()),*stream);
    }
    if(std::getenv("EL_SONG_EDITOR_SNAPSHOT")) {
        for(auto* c:editor.getChildren())if(auto* b=dynamic_cast<TextButton*>(c);b && b->getButtonText()=="MIDI")b->onClick();
        auto stream=File("/tmp/jgod-song-midi.png").createOutputStream();BOOST_REQUIRE(stream);
        stream->setPosition(0);stream->truncate();PNGImageFormat().writeImageToStream(editor.createComponentSnapshot(editor.getLocalBounds()),*stream);
    }
    TextButton* helpToggle=nullptr;ComboBox* timing=nullptr;
    for(auto* c:editor.getChildren()){
        if(auto* b=dynamic_cast<TextButton*>(c);b && b->getButtonText()=="?")helpToggle=b;
        if(auto* box=dynamic_cast<ComboBox*>(c);box && box->getNumItems()==3)timing=box;
    }
    BOOST_REQUIRE(helpToggle);BOOST_REQUIRE(timing);
    BOOST_CHECK(timing->getTooltip().isNotEmpty());
    helpToggle->onClick();BOOST_CHECK(timing->getTooltip().isEmpty());
    if(std::getenv("EL_SONG_EDITOR_SNAPSHOT")){
        auto stream=File("/tmp/jgod-song-expanded-help.png").createOutputStream();BOOST_REQUIRE(stream);
        stream->setPosition(0);stream->truncate();PNGImageFormat().writeImageToStream(editor.createComponentSnapshot(editor.getLocalBounds()),*stream);
    }
    helpToggle->onClick();BOOST_CHECK(timing->getTooltip().isNotEmpty());
    settings->onClick();BOOST_CHECK(sectionPicker->isVisible());

    if(std::getenv("EL_SONG_EDITOR_SNAPSHOT")) {
        File target("/tmp/jgod-song-live-overview.png");auto stream=target.createOutputStream();BOOST_REQUIRE(stream);
        stream->setPosition(0);stream->truncate();PNGImageFormat png;
        BOOST_CHECK(png.writeImageToStream(editor.createComponentSnapshot(editor.getLocalBounds()),*stream));
    }
}
BOOST_AUTO_TEST_CASE(ShortReleaseStartsNextBarAndSecondPressCancels) {
    SongRig r;r.head.running=true;r.head.beat=3.50;
    r.cc(127,1);r.run();BOOST_CHECK_EQUAL(r.song->currentIndex.load(),-1);
    r.cc(0,0);r.run();BOOST_CHECK(r.song->pending.load());
    r.song->next();r.run();BOOST_CHECK(!r.song->pending.load());BOOST_CHECK_EQUAL(r.song->currentIndex.load(),-1);
    r.song->next();r.run();for(int i=0;i<30 && r.song->pending.load();++i)r.run();
    BOOST_CHECK_EQUAL(r.song->currentIndex.load(),0);
}
BOOST_AUTO_TEST_CASE(StoppedTransportRepeatedSectionsEndAndRewindKeepLiveSound) {
    SongRig r;r.run();BOOST_CHECK_CLOSE(r.audio.getSample(0,7),1.0f,0.01);
    r.cc(127,0);r.cc(0,1);r.run();BOOST_CHECK_EQUAL(r.song->activeSection.load(),r.a->nodeId);
    r.song->next();r.run();BOOST_CHECK_EQUAL(r.song->activeSection.load(),r.b->nodeId);
    r.song->next();r.run();BOOST_CHECK_EQUAL(r.song->activeSection.load(),r.a->nodeId);
    r.song->next();r.run();BOOST_CHECK(r.song->ended.load());BOOST_CHECK_CLOSE(r.audio.getSample(0,7),1.0f,0.01);
    r.song->next();r.run();BOOST_CHECK_EQUAL(r.song->currentIndex.load(),2);
    r.song->rewind();r.run();BOOST_CHECK_EQUAL(r.song->currentIndex.load(),-1);BOOST_CHECK_EQUAL(r.song->activeSection.load(),r.a->nodeId);BOOST_CHECK_CLOSE(r.audio.getSample(0,7),1.0f,0.01);
}
BOOST_AUTO_TEST_CASE(LongHoldPanicIsSampleExactLatchedAndReleaseDoesNotAdvance) {
    SongRig r;r.song->next();r.run();r.cc(127,3);r.run();
    for(int i=0;i<9;++i)r.run();
    BOOST_CHECK(!r.song->muted.load());r.run();
    BOOST_CHECK(r.song->muted.load());BOOST_CHECK_EQUAL(r.graph.songPanicOffset(),3);
    BOOST_CHECK_CLOSE(r.audio.getSample(0,2),1.0f,0.01);BOOST_CHECK_SMALL(r.audio.getSample(0,3),0.00001f);
    r.cc(0);r.run();BOOST_CHECK_EQUAL(r.song->currentIndex.load(),0);BOOST_CHECK_SMALL(r.audio.getMagnitude(0,8),0.00001f);
    r.song->restoreAudio();r.run();BOOST_CHECK(!r.song->muted.load());BOOST_CHECK_CLOSE(r.audio.getSample(0,7),1.0f,0.01);
    BOOST_CHECK(!r.song->ended.load());
}
BOOST_AUTO_TEST_CASE(PanicMutesSharedRouteWithoutAnyManagerAudioCable) {
    SongRig r;
    for(int ch=0;ch<2;++ch)BOOST_REQUIRE(r.graph.connectChannels(PortType::Audio,r.input->nodeId,ch,r.output->nodeId,ch));
    r.graph.rebuild();r.run();BOOST_CHECK_CLOSE(r.audio.getSample(0,7),2.0f,0.01);
    r.song->panic();r.run();BOOST_CHECK_SMALL(r.audio.getMagnitude(0,8),0.00001f);
}
BOOST_AUTO_TEST_CASE(DrySectionTransitionCrossfadesWithoutHardCut) {
    SongRig r(1000);r.song->next();r.run();
    r.cc(127,1);r.cc(0,4);r.run();
    for(int i=0;i<8;++i)BOOST_CHECK_CLOSE(r.audio.getSample(0,i),1.0f,0.01);
}
BOOST_AUTO_TEST_CASE(UnrelatedRebuildPreservesSongAndGraphOversamplingIsRejected) {
    SongRig r;r.song->next();r.run();const auto selected=r.song->activeSection.load();
    r.graph.rebuild();r.run();BOOST_CHECK_EQUAL(r.song->activeSection.load(),selected);BOOST_CHECK_EQUAL(r.song->currentIndex.load(),0);
    BOOST_CHECK(!r.a->supportsOversampling());r.a->setOversamplingFactor(4);BOOST_CHECK_EQUAL(r.a->getOversamplingFactor(),1);
    auto* second=new SongManagerNode();r.graph.addNode(second);r.graph.rebuild();
    BOOST_CHECK(r.song->effective.load());BOOST_CHECK(!second->effective.load());
}
BOOST_AUTO_TEST_CASE(UnassignedGraphRemainsSharedAndNestedPanicReachesRoot) {
    SongRig r;auto* shared=r.addSection("Shared");auto* nested=new SongManagerNode();shared->addNode(nested);shared->rebuild();r.graph.rebuild();
    r.run();BOOST_CHECK_CLOSE(r.audio.getSample(0,7),2.0f,0.01);
    nested->panic();r.run();BOOST_CHECK_EQUAL(r.graph.songPanicOffset(),0);BOOST_CHECK_SMALL(r.audio.getMagnitude(0,8),0.00001f);
    nested->restoreAudio();r.run();BOOST_CHECK_EQUAL(r.graph.songPanicOffset(),-1);
}
BOOST_AUTO_TEST_CASE(StateRetainsRepeatedOrderAndPedalButRestoresStopped) {
    SongRig r;r.song->pedalCC.store(31);r.song->channel.store(3);r.song->next();r.run();
    MemoryBlock state;r.song->getState(state);SongManagerNode copy;copy.setState(state.getData(),int(state.getSize()));
    BOOST_REQUIRE_EQUAL(copy.orderIDs().size(),3);BOOST_CHECK_EQUAL(copy.orderIDs()[0],copy.orderIDs()[2]);
    BOOST_CHECK_EQUAL(copy.pedalCC.load(),31);BOOST_CHECK_EQUAL(copy.channel.load(),3);BOOST_CHECK_EQUAL(copy.currentIndex.load(),-1);BOOST_CHECK(!copy.muted.load());
}
BOOST_AUTO_TEST_CASE(SectionMidiGateRejectsOrphanPedalReleaseButKeepsEffectZero) {
    SongRig r;auto* source=r.graph.addNode(new IONode(IONode::midiInputNode));
    auto makeSpy=[&](GraphNode* section) {
        auto* in=section->addNode(new IONode(IONode::midiInputNode));auto* spy=new MidiSpy();section->addNode(spy);
        section->addNode(new AudioProcessorNode(new LooperProcessor()));
        BOOST_REQUIRE(section->connectChannels(PortType::Midi,in->nodeId,0,spy->nodeId,0));
        BOOST_REQUIRE(r.graph.connectChannels(PortType::Midi,source->nodeId,0,section->nodeId,0));section->rebuild();return spy;
    };
    auto* a=makeSpy(r.a);auto* b=makeSpy(r.b);r.graph.rebuild();r.song->next();r.run();
    r.midi.addEvent(MidiMessage::controllerEvent(1,20,127),1);r.run();
    BOOST_CHECK_EQUAL(a->presses,1);BOOST_CHECK_EQUAL(b->presses,0);
    r.cc(127,1);r.cc(0,4);
    r.midi.addEvent(MidiMessage::controllerEvent(1,20,0),6);
    r.midi.addEvent(MidiMessage::controllerEvent(1,7,0),7);r.run();
    BOOST_CHECK_EQUAL(a->releases,0);BOOST_CHECK_EQUAL(b->releases,0);BOOST_CHECK_EQUAL(b->zeroVolume,1);
    BOOST_CHECK(a->notesOff>=16);
}
BOOST_AUTO_TEST_CASE(PanicStopsSharedLooperAndEditorShowsExplicitRestore) {
    SongRig r;auto* looper=new LooperProcessor();
    r.graph.addNode(new AudioProcessorNode(looper));r.graph.rebuild();
    looper->setQuantized(false);looper->request(LooperEngine::Action::record);r.run();
    BOOST_CHECK_EQUAL(looper->view().state,int(LooperEngine::State::recording));
    r.song->panic();r.run();BOOST_CHECK_EQUAL(looper->view().state,int(LooperEngine::State::stopped));
    ValueTree data(types::Node);data.setProperty(tags::object,r.song,nullptr);
    Node node(data,false);SongManagerEditor editor(node);bool restore=false;
    for(auto* child:editor.getChildren())if(auto* button=dynamic_cast<TextButton*>(child))restore|=button->getButtonText()=="Restore audio";
    BOOST_CHECK(restore);
    if(std::getenv("EL_SONG_EDITOR_SNAPSHOT")) {
        File target("/tmp/jgod-song-manager.png");auto stream=target.createOutputStream();BOOST_REQUIRE(stream);
        stream->setPosition(0);stream->truncate();PNGImageFormat png;
        BOOST_CHECK(png.writeImageToStream(editor.createComponentSnapshot(editor.getLocalBounds()),*stream));
    }
}
BOOST_AUTO_TEST_CASE(PropagatedPanicCancelsOtherSongTransitionsWithoutAddingMuteOwners) {
    SongRig r;auto* shared=r.addSection("Other song");auto* other=new SongManagerNode();
    shared->addNode(other);auto* part=new GraphNode(*element::test::context());shared->addNode(part);other->setOrder({part->nodeId});shared->rebuild();r.graph.rebuild();r.graph.setPlayHead(&r.head);
    r.head.running=true;r.head.beat=0.5;other->next();r.run();
    BOOST_REQUIRE(other->pending.load());BOOST_CHECK(!other->ended.load());
    r.song->panic();r.run();
    BOOST_CHECK(!other->pending.load());BOOST_CHECK(!other->ended.load());BOOST_CHECK(!other->muted.load());
    r.song->restoreAudio();r.run();r.head.beat=4.0;r.run();
    BOOST_CHECK(!other->pending.load());BOOST_CHECK(!other->ended.load());BOOST_CHECK_EQUAL(other->currentIndex.load(),-1);
    BOOST_CHECK(!other->muted.load());BOOST_CHECK(!r.song->muted.load());
    // A propagated stop also discards requests queued before its callback.
    other->rewind();other->next();shared->scheduleSongStop(0);r.run();
    BOOST_CHECK(!other->ended.load());BOOST_CHECK(!other->pending.load());
}
BOOST_AUTO_TEST_CASE(PanicCoversParallelRootsAndCanRestoreAfterRootSwitch)
{
    Context context(RunMode::Standalone);AudioEngine engine(context);
    RootGraph a(context),b(context);
    DeviceManager::AudioDeviceSetup setup;setup.inputChannels.setRange(0,2,true);setup.outputChannels.setRange(0,2,true);setup.sampleRate=1000;setup.bufferSize=8;
    for(auto* root:{&a,&b}) {
        root->setPlayConfigFor(setup);
        auto* input=root->addNode(new IONode(IONode::audioInputNode));
        auto* output=root->addNode(new IONode(IONode::audioOutputNode));
        for(int ch=0;ch<2;++ch)BOOST_REQUIRE(root->connectChannels(PortType::Audio,input->nodeId,ch,output->nodeId,ch));
        root->rebuild();engine.addGraph(root);
    }
    auto* song=new SongManagerNode();a.addNode(song);a.rebuild();
    engine.setActiveGraphIndex(0);engine.prepareExternalPlayback(1000,8,2,2);
    AudioBuffer<float> audio(2,8);MidiBuffer midi;
    const auto run=[&]{for(int ch=0;ch<2;++ch)for(int i=0;i<8;++i)audio.setSample(ch,i,1);engine.processExternalBuffers(audio,midi);};
    run();run();BOOST_CHECK_GT(audio.getMagnitude(0,8),1.0f);
    song->panic();run();BOOST_CHECK_SMALL(audio.getMagnitude(0,8),.00001f);
    a.setRenderMode(RootGraph::SingleGraph);b.setRenderMode(RootGraph::SingleGraph);engine.setActiveGraphIndex(1);
    run();BOOST_CHECK_SMALL(audio.getMagnitude(0,8),.00001f);
    song->restoreAudio();run();run();BOOST_CHECK_GT(audio.getMagnitude(0,8),.5f);
    engine.removeGraph(&b);engine.removeGraph(&a);a.releaseResources();b.releaseResources();
}
BOOST_AUTO_TEST_CASE(SecondHoldRestoresOnceAndReleaseDoesNotAdvance) {
    SongRig r;r.run();BOOST_CHECK_EQUAL(r.song->activeSection.load(),r.a->nodeId);
    r.cc(127);for(int i=0;i<12;++i)r.run();
    BOOST_REQUIRE(r.song->muted.load());
    // Continuing the same hold must not restore.
    for(int i=0;i<20;++i)r.run();BOOST_CHECK(r.song->muted.load());
    r.cc(0);r.run();r.cc(127);for(int i=0;i<12;++i)r.run();
    BOOST_CHECK(!r.song->muted.load());BOOST_CHECK(!r.song->ended.load());
    BOOST_CHECK_CLOSE(r.audio.getSample(0,7),1.0f,0.01);
    for(int i=0;i<20;++i)r.run();BOOST_CHECK(!r.song->muted.load());
    r.cc(0);r.run();BOOST_CHECK_EQUAL(r.song->currentIndex.load(),-1);
    r.cc(127);r.cc(0,1);r.run();BOOST_CHECK_EQUAL(r.song->currentIndex.load(),0);
}
namespace {
struct SectionLoops {
    LooperManagerNode* manager=nullptr;
    LooperProcessor *first=nullptr,*second=nullptr;
    Processor *firstNode=nullptr,*secondNode=nullptr;
};
SectionLoops addSectionLoops(SongRig& rig,GraphNode& section,Processor& parentMidi) {
    SectionLoops result;
    auto* input=section.addNode(new IONode(IONode::midiInputNode));
    result.manager=new LooperManagerNode();section.addNode(result.manager);
    result.first=new LooperProcessor();result.second=new LooperProcessor();
    result.firstNode=section.addNode(new AudioProcessorNode(result.first));
    result.secondNode=section.addNode(new AudioProcessorNode(result.second));
    BOOST_REQUIRE(section.connectChannels(PortType::Midi,input->nodeId,0,result.manager->nodeId,0));
    for(auto* node:{result.firstNode,result.secondNode})
        BOOST_REQUIRE(section.connectChannels(PortType::Midi,result.manager->nodeId,0,node->nodeId,0));
    BOOST_REQUIRE(rig.graph.connectChannels(PortType::Midi,parentMidi.nodeId,0,section.nodeId,0));
    section.rebuild();return result;
}
}
BOOST_AUTO_TEST_CASE(CompactTransitionsCountInJumpAndFirstLooperProfile) {
    SongRig r;auto* input=r.graph.addNode(new IONode(IONode::midiInputNode));
    auto a=addSectionLoops(r,*r.a,*input);auto b=addSectionLoops(r,*r.b,*input);r.graph.rebuild();r.run();
    r.song->switchTiming=2;r.song->countInBars=1;r.head.running=true;r.head.beat=1;
    r.song->next();r.run();BOOST_CHECK(r.song->pending.load());BOOST_CHECK_GE(r.song->remainingBars.load(),1);
    r.head.beat=7.95;r.run();BOOST_CHECK_EQUAL(r.song->currentIndex.load(),0);BOOST_CHECK(!r.song->pending.load());
    r.song->recordingProfile=1;r.song->jumpTo(1);r.run();BOOST_CHECK_EQUAL(r.song->activeSection.load(),r.b->nodeId);
    BOOST_CHECK_EQUAL(b.first->view().state,int(LooperEngine::State::empty)); // double click has no capture
    r.head.running=false;r.song->rewind();r.run();
    a.manager->move(a.secondNode->nodeId,-1);r.graph.rebuild();r.song->next();r.run();r.run();
    BOOST_CHECK(a.second->view().state==int(LooperEngine::State::recording));
    BOOST_CHECK_EQUAL(a.first->view().state,int(LooperEngine::State::empty));
    r.song->next();r.run();BOOST_CHECK_EQUAL(r.song->activeSection.load(),r.a->nodeId); // capture lock
    r.song->panic();r.run();BOOST_CHECK(r.song->muted.load());BOOST_CHECK(!r.song->pending.load());
}
BOOST_AUTO_TEST_CASE(EmptyOrderKeepsOneLiveSectionAndTransitionSettingsPersist) {
    SongRig r;r.run();r.song->setOrder({});r.run();
    BOOST_CHECK_EQUAL(r.song->activeSection.load(),r.a->nodeId);BOOST_CHECK_CLOSE(r.audio.getSample(0,7),1.f,.01);
    r.song->switchTiming=2;r.song->countInBars=3;r.song->recordingProfile=2;r.song->actionCC[2]=99;
    MemoryBlock state;r.song->getState(state);SongManagerNode copy;copy.setState(state.getData(),int(state.getSize()));
    BOOST_CHECK_EQUAL(copy.switchTiming.load(),2);BOOST_CHECK_EQUAL(copy.countInBars.load(),3);BOOST_CHECK_EQUAL(copy.recordingProfile.load(),2);BOOST_CHECK_EQUAL(copy.actionCC[2].load(),99);
}
BOOST_AUTO_TEST_CASE(DirectMidiButtonsAndNoSectionAudioRoute) {
    SongRig r;for(int i=0;i<4;++i)r.song->actionCC[i]=90+i;r.run();
    auto press=[&](int cc){r.midi.addEvent(MidiMessage::controllerEvent(1,cc,127),0);r.midi.addEvent(MidiMessage::controllerEvent(1,cc,0),1);r.run();};
    press(90);BOOST_CHECK_EQUAL(r.song->currentIndex.load(),0);
    press(90);BOOST_CHECK_EQUAL(r.song->currentIndex.load(),1);
    press(91);r.run();BOOST_CHECK_EQUAL(r.song->currentIndex.load(),-1);
    press(92);BOOST_CHECK(r.song->muted.load());BOOST_CHECK_SMALL(r.audio.getMagnitude(0,8),.00001f);
    press(93);r.run();BOOST_CHECK(!r.song->muted.load());BOOST_CHECK_CLOSE(r.audio.getSample(0,7),1.f,.01);
    PreparedGraph direct(100,8);auto& g=direct.graph;auto* in=g.addNode(new IONode(IONode::audioInputNode));auto* out=g.addNode(new IONode(IONode::audioOutputNode));
    auto* song=new SongManagerNode();g.addNode(song);for(int ch=0;ch<2;++ch)BOOST_REQUIRE(g.connectChannels(PortType::Audio,in->nodeId,ch,out->nodeId,ch));g.rebuild();
    AudioBuffer<float> audio(2,8);for(int ch=0;ch<2;++ch)for(int n=0;n<8;++n)audio.setSample(ch,n,.5f);MidiBuffer midi;RenderContext rc(audio,audio,midi,8);g.render(rc);
    BOOST_CHECK_EQUAL(song->activeSection.load(),0);BOOST_CHECK_CLOSE(audio.getSample(0,7),.5f,.01);
}
BOOST_AUTO_TEST_CASE(OfflineActiveSelectionAcrossSectionsWithoutAudio)
{
    SongRig r;auto* input=r.graph.addNode(new IONode(IONode::midiInputNode));
    auto a=addSectionLoops(r,*r.a,*input);auto b=addSectionLoops(r,*r.b,*input);r.graph.rebuild();
    r.graph.serviceOfflineControls();
    b.second->activeRequested=true;r.graph.serviceOfflineControls();
    BOOST_CHECK_EQUAL(r.song->activeSection.load(),r.b->nodeId);
    BOOST_CHECK_EQUAL(b.manager->selectedID(),b.secondNode->nodeId);
    BOOST_CHECK(b.second->managerSelected.load());BOOST_CHECK(b.second->pedalSectionActive.load());
    BOOST_CHECK(!a.first->pedalSectionActive.load());
    a.second->activeRequested=true;r.graph.serviceOfflineControls();
    BOOST_CHECK_EQUAL(r.song->activeSection.load(),r.a->nodeId);
    BOOST_CHECK_EQUAL(a.manager->selectedID(),a.secondNode->nodeId);
    BOOST_CHECK_EQUAL(a.second->view().state,int(LooperEngine::State::empty));
}
BOOST_AUTO_TEST_CASE(MouseSelectionChangesPedalTargetWithoutStartingPlayback) {
    SongRig r;auto* input=r.graph.addNode(new IONode(IONode::midiInputNode));
    auto a=addSectionLoops(r,*r.a,*input);r.graph.rebuild();r.run();
    BOOST_REQUIRE_EQUAL(r.song->loopTargets().size(),2);
    r.song->selectLooper(a.second);r.run();
    BOOST_CHECK_EQUAL(a.manager->selectedID(),a.secondNode->nodeId);
    BOOST_CHECK_EQUAL(a.second->view().state,int(LooperEngine::State::empty));
    BOOST_CHECK_EQUAL(r.song->currentIndex.load(),-1);
    // Capturing and even a not-yet-consumed capture request lock selection.
    a.second->request(LooperEngine::Action::overdub);
    r.song->selectLooper(a.first);r.run();
    BOOST_CHECK_EQUAL(a.manager->selectedID(),a.secondNode->nodeId);
    r.song->selectLooper(a.first);r.run();
    BOOST_CHECK_EQUAL(a.manager->selectedID(),a.secondNode->nodeId);
}
BOOST_AUTO_TEST_CASE(MouseSectionJumpWaitsForBarAndRoutesSameSamplePedalToChosenLooper) {
    SongRig r;auto* input=r.graph.addNode(new IONode(IONode::midiInputNode));
    auto a=addSectionLoops(r,*r.a,*input);auto b=addSectionLoops(r,*r.b,*input);r.graph.rebuild();r.run();
    r.head.running=true;r.head.beat=3.7;
    r.song->selectLooper(b.second);r.run();
    BOOST_CHECK(r.song->pending.load());BOOST_CHECK_EQUAL(r.song->activeSection.load(),r.a->nodeId);
    BOOST_CHECK(r.song->pendingLooper.load()==b.second);
    BOOST_CHECK_EQUAL(a.manager->selectedID(),a.firstNode->nodeId);
    BOOST_CHECK_EQUAL(b.manager->selectedID(),b.firstNode->nodeId);
    r.head.beat=3.94; // next bar at sample 3
    r.midi.addEvent(MidiMessage::controllerEvent(1,23,127),3);
    r.midi.addEvent(MidiMessage::controllerEvent(1,23,0),4);
    r.run();
    BOOST_CHECK_EQUAL(r.song->activeSection.load(),r.b->nodeId);
    BOOST_CHECK_EQUAL(r.song->currentIndex.load(),1);
    BOOST_CHECK(!r.song->pending.load());BOOST_CHECK(r.song->pendingLooper.load()==nullptr);
    BOOST_CHECK_EQUAL(b.manager->selectedID(),b.secondNode->nodeId);
    BOOST_CHECK_EQUAL(b.first->view().state,int(LooperEngine::State::empty));
    BOOST_CHECK_EQUAL(b.second->view().state,int(LooperEngine::State::recording));
    if(std::getenv("EL_SONG_EDITOR_SNAPSHOT")) {
        ValueTree data(types::Node);data.setProperty(tags::object,r.song,nullptr);
        Node node(data,false);SongManagerEditor editor(node);
        auto stream=File("/tmp/jgod-song-controls.png").createOutputStream();BOOST_REQUIRE(stream);
        stream->setPosition(0);stream->truncate();PNGImageFormat png;
        BOOST_CHECK(png.writeImageToStream(editor.createComponentSnapshot(editor.getLocalBounds()),*stream));
    }
}
BOOST_AUTO_TEST_CASE(MouseJumpCancelsForNewCaptureAndPanicAndUsesExistingOrderOccurrence) {
    SongRig r;auto* input=r.graph.addNode(new IONode(IONode::midiInputNode));
    auto a=addSectionLoops(r,*r.a,*input);auto b=addSectionLoops(r,*r.b,*input);r.graph.rebuild();r.run();
    r.head.running=true;r.head.beat=1;
    r.song->selectLooper(b.second);r.run();BOOST_REQUIRE(r.song->pending.load());
    a.first->request(LooperEngine::Action::overdub);r.run();
    BOOST_CHECK(!r.song->pending.load());BOOST_CHECK_EQUAL(r.song->activeSection.load(),r.a->nodeId);
    a.first->request(LooperEngine::Action::stop);r.run();
    r.song->selectLooper(b.second);r.run();BOOST_REQUIRE(r.song->pending.load());
    r.song->panic();r.run();BOOST_CHECK(!r.song->pending.load());BOOST_CHECK(r.song->pendingLooper.load()==nullptr);
    r.song->restoreAudio();r.run();r.head.running=false;
    r.song->selectLooper(b.second);r.run();BOOST_CHECK_EQUAL(r.song->currentIndex.load(),1);
    r.song->selectLooper(a.second);r.run();BOOST_CHECK_EQUAL(r.song->currentIndex.load(),2);
    BOOST_CHECK_EQUAL(a.manager->selectedID(),a.secondNode->nodeId);
}
BOOST_AUTO_TEST_CASE(CapturePedalBeforeBoundaryCancelsMouseJumpInSameBlock) {
    SongRig r;auto* input=r.graph.addNode(new IONode(IONode::midiInputNode));
    auto a=addSectionLoops(r,*r.a,*input);auto b=addSectionLoops(r,*r.b,*input);r.graph.rebuild();r.run();
    r.head.running=true;r.head.beat=3.7;r.song->selectLooper(b.second);r.run();
    BOOST_REQUIRE(r.song->pending.load());r.head.beat=3.94;
    r.midi.addEvent(MidiMessage::controllerEvent(1,23,127),1);
    r.midi.addEvent(MidiMessage::controllerEvent(1,23,0),2);r.run();
    BOOST_CHECK(!r.song->pending.load());BOOST_CHECK_EQUAL(r.song->activeSection.load(),r.a->nodeId);
    BOOST_CHECK_EQUAL(a.first->view().state,int(LooperEngine::State::recording));
    BOOST_CHECK_EQUAL(b.second->view().state,int(LooperEngine::State::empty));
}
BOOST_AUTO_TEST_CASE(OverviewPlaybackIsActiveSectionOnlyAndDoesNotSelect) {
    SongRig r;auto* input=r.graph.addNode(new IONode(IONode::midiInputNode));
    auto a=addSectionLoops(r,*r.a,*input);auto b=addSectionLoops(r,*r.b,*input);r.graph.rebuild();r.run();
    a.second->request(LooperEngine::Action::overdub);r.run();
    a.second->request(LooperEngine::Action::stop);r.run();
    BOOST_REQUIRE(r.song->toggleLooperPlayback(a.second));r.run();
    BOOST_CHECK_EQUAL(a.second->view().state,int(LooperEngine::State::playing));
    BOOST_CHECK_EQUAL(a.manager->selectedID(),a.firstNode->nodeId);
    BOOST_CHECK(!r.song->toggleLooperPlayback(b.first));
    BOOST_REQUIRE(r.song->toggleLooperPlayback(a.second));r.run();
    BOOST_CHECK_EQUAL(a.second->view().state,int(LooperEngine::State::stopped));
}
BOOST_AUTO_TEST_CASE(ExportDemoOrderWhenRequested)
{
    const auto* path=std::getenv("EL_SONG_DEMO_STATE");if(!path)return;
    SongManagerNode manager;manager.setOrder({20,21,22,20,21});MemoryBlock data;manager.getState(data);
    BOOST_REQUIRE(File(String::fromUTF8(path)).replaceWithText(data.toBase64Encoding()));
}
BOOST_AUTO_TEST_SUITE_END()
