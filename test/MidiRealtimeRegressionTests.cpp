// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include <boost/test/unit_test.hpp>
#include "fixture/PreparedGraph.h"
#include "fixture/TestNode.h"
#include "engine/ionode.hpp"
#include "engine/boundedmidi.hpp"
#include "engine/boundedmidicollector.hpp"
#include "engine/miditranspose.hpp"
#include "nodes/audioprocessor.hpp"
#include "nodes/loopermanager.hpp"
#include "nodes/songmanager.hpp"
#include "nodes/countingate.hpp"
#if JUCE_MAC
#include <dlfcn.h>
#include <execinfo.h>
#include <cstdio>
#endif
using namespace element;
using namespace juce;
namespace {
#if JUCE_MAC
// Test-only macOS malloc logger: catches C malloc/free (including JUCE's
// inline/heap MidiMessage boundary), not merely C++ operator new.
using MallocLogger=void(*)(uint32_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uint32_t);
thread_local bool watching=false;
thread_local unsigned allocations=0,deallocations=0;
MallocLogger previousLogger=nullptr;
void* traces[32][20];int depths[32];unsigned traceCount=0;
void allocationLog(uint32_t type,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t result,uint32_t skip) {
    if(watching) {
        if(type&2) {
            ++allocations;
            if(traceCount<32) {watching=false;depths[traceCount]=backtrace(traces[traceCount],20);++traceCount;watching=true;}
        }
        if(type&4)++deallocations;
    }
    if(previousLogger)previousLogger(type,a,b,c,result,skip);
}
struct AllocationProbe {
    MallocLogger* slot=reinterpret_cast<MallocLogger*>(dlsym(RTLD_DEFAULT,"malloc_logger"));
    AllocationProbe(){if(slot){previousLogger=*slot;*slot=allocationLog;}}
    ~AllocationProbe(){watching=false;if(slot)*slot=previousLogger;}
    template<class F> std::pair<unsigned,unsigned> measure(F&& f) {
        allocations=deallocations=0;traceCount=0;watching=true;f();watching=false;
        if(std::getenv("EL_TRACE_MIDI_ALLOCS"))for(unsigned i=0;i<traceCount;++i) {std::fprintf(stderr,"Allocation %u\n",i);backtrace_symbols_fd(traces[i],depths[i],2);}
        return {allocations,deallocations};
    }
};
#endif
struct Clock : AudioPlayHead {
    double beat=0;
    Optional<PositionInfo> getPosition() const override {
        PositionInfo p;p.setBpm(120);p.setPpqPosition(beat);p.setIsPlaying(true);
        p.setTimeSignature(TimeSignature{4,4});return p;
    }
};
struct Sink : TestNode {
    bool command=false,heartbeat=false,heartbeatOn=false,external=false,transposed=false;
    Sink():TestNode(2,2,1,0){}
    void render(RenderContext& rc) override {
        for(auto m:*rc.midi.getWriteBuffer(0)) {
            command |= (m.numBytes==16 || m.numBytes==22) && m.data[0]==0xf0;
            heartbeat |= m.numBytes==10 && m.data[0]==0xf0;
            heartbeatOn |= m.numBytes==10 && m.data[0]==0xf0 && m.data[8]==1;
            external |= m.numBytes==40 && m.data[0]==0xf0 && m.data[38]==37;
            transposed |= m.numBytes==3 && m.data[0]==0x90 && m.data[1]==72;
        }
    }
};
}
BOOST_AUTO_TEST_SUITE(MidiRealtimeRegressionTests)
BOOST_AUTO_TEST_CASE(LongPacketsSurviveManagerLooperCountInAndSongFilters)
{
    Clock clock;PreparedGraph fixture(1000,32);auto& root=fixture.graph;root.setPlayHead(&clock);
    auto* rootIn=root.addNode(new IONode(IONode::midiInputNode));
    auto* song=new SongManagerNode();root.addNode(song);
    auto* section=new GraphNode(*element::test::context());root.addNode(section);
    auto* in=section->addNode(new IONode(IONode::midiInputNode));
    auto* manager=new LooperManagerNode();section->addNode(manager);
    auto* looper=new LooperProcessor();looper->setCapacityLimits(2,2,true);looper->countInClick=true;
    auto* wrapper=section->addNode(new AudioProcessorNode(looper));
    auto* gate=section->addNode(new AudioProcessorNode(new CountInGate()));
    auto* sink=new Sink();section->addNode(sink);sink->setTransposeOffset(12);
    const auto wire=[&](Processor* from,Processor* to){BOOST_REQUIRE(section->connectChannels(PortType::Midi,from->nodeId,0,to->nodeId,0));};
    wire(in,manager);wire(manager,wrapper);wire(wrapper,gate);
    wire(manager,sink);wire(wrapper,sink);wire(in,sink);
    section->rebuild();root.rebuild();
    BOOST_REQUIRE(root.connectChannels(PortType::Midi,rootIn->nodeId,0,section->nodeId,0));
    root.rebuild();
    AudioBuffer<float> audio(2,32);MidiBuffer midi;midi.ensureSize(65536);
    RenderContext rc(audio,audio,midi,32);
    const auto render=[&]{audio.clear();root.render(rc);clock.beat+=.064;};
    for(int i=0;i<3;++i){midi.clear();render();looper->view();}
    uint8 packet[40]={0xf0,0x7d};for(int i=2;i<39;++i)packet[i]=uint8(i-1);packet[39]=0xf7;
#if JUCE_MAC
    AllocationProbe probe;BOOST_REQUIRE(probe.slot);
    // Positive control proves the observer sees JUCE's own malloc AND free.
    int size=0;
    const auto positive=probe.measure([&]{MidiMessage owning(packet,40);size=owning.getRawDataSize();});
    BOOST_CHECK_EQUAL(size,40);BOOST_REQUIRE(positive.first>0);BOOST_REQUIRE(positive.second>0);
#endif
    // A normal capture pedal, PLAY/STOP ALL, and external SysEx traverse the
    // actual graph filters. Looper publishes its count-in heartbeat every block.
    for(int cc:{23,23,28,27}) {
        midi.clear();midi.addEvent(packet,40,0);
        midi.addEvent(MidiMessage::noteOn(1,60,uint8(100)),1);
        midi.addEvent(MidiMessage::controllerEvent(1,cc,127),2);
        midi.addEvent(MidiMessage::controllerEvent(1,cc,0),3);
#if JUCE_MAC
        const auto counts=probe.measure(render);
        BOOST_CHECK_EQUAL(counts.first,0);BOOST_CHECK_EQUAL(counts.second,0);
#else
        render();
#endif
        looper->view();
    }
    BOOST_CHECK(sink->command);BOOST_CHECK(sink->heartbeat);BOOST_CHECK(sink->external);BOOST_CHECK(sink->transposed);
    // Run through an audible Sync count-in, not just inactive heartbeats.
    looper->request(LooperEngine::Action::clear);midi.clear();render();looper->view();
    looper->setQuantized(true);looper->recordPreparation=true;looper->countInBars=1;
    midi.clear();midi.addEvent(MidiMessage::controllerEvent(1,20,127),0);
    midi.addEvent(MidiMessage::controllerEvent(1,20,0),1);
    for(int i=0;i<130;++i) {
#if JUCE_MAC
        const auto countInCounts=probe.measure(render);
        BOOST_CHECK_EQUAL(countInCounts.first,0);BOOST_CHECK_EQUAL(countInCounts.second,0);
#else
        render();
#endif
        midi.clear();looper->view();
    }
    BOOST_CHECK(sink->heartbeatOn);
    // Exercise the inactive section filter without permitting SysEx through.
    GraphNode::SongGate closed;closed.enabled=true;closed.before=closed.after=false;section->setSongGate(closed);
    midi.clear();midi.addEvent(packet,40,0);sink->external=false;
#if JUCE_MAC
    const auto closedCounts=probe.measure([&]{section->render(rc);});
    BOOST_CHECK_EQUAL(closedCounts.first,0);BOOST_CHECK_EQUAL(closedCounts.second,0);
#else
    section->render(rc);
#endif
    BOOST_CHECK(!sink->external);
    // Separate transpose-only path (when the host channel/range filter is off).
    MidiTranspose transpose;transpose.setNoteOffset(12);midi.clear();midi.addEvent(packet,40,0);
#if JUCE_MAC
    const auto transposeCounts=probe.measure([&]{transpose.process(midi,32);});
    BOOST_CHECK_EQUAL(transposeCounts.first,0);BOOST_CHECK_EQUAL(transposeCounts.second,0);
#else
    transpose.process(midi,32);
#endif
    BOOST_REQUIRE_EQUAL(midi.getNumEvents(),1);BOOST_CHECK_EQUAL((*midi.begin()).numBytes,40);
}
BOOST_AUTO_TEST_CASE(PreparedAudioImportDoesNotAllocateOrFreeInCallback)
{
#if JUCE_MAC
    LooperProcessor p;p.setMemoryLimits(1,3,4,true);p.prepareToPlay(8000,32);
    AudioBuffer<float> audio(2,32);audio.clear();MidiBuffer midi;p.processBlock(audio,midi);
    auto view=p.view();auto job=std::make_unique<LooperProcessor::ImportJob>();
    job->plan.rate=view.rate;job->plan.capacity=view.capacity;job->plan.target=100;
    job->plan.parts.resize(4);for(auto& part:job->plan.parts)for(auto& ch:part)ch.resize(view.capacity,.2f);
    BOOST_REQUIRE(p.submitImport(std::move(job)));
    AllocationProbe probe;BOOST_REQUIRE(probe.slot);
    auto counts=probe.measure([&]{p.processBlock(audio,midi);});
    BOOST_CHECK_EQUAL(counts.first,0);BOOST_CHECK_EQUAL(counts.second,0);
    BOOST_CHECK_EQUAL(p.collectImport(),1);BOOST_CHECK_EQUAL(p.view().count,4);
#endif
}
BOOST_AUTO_TEST_CASE(CustomControllerCaptureAndWrapDoNotAllocate)
{
#if JUCE_MAC
    LooperProcessor p;p.prepareToPlay(1000,32);p.setCrossfade(0);p.setCaptureTail(0);
    AudioBuffer<float> audio(2,32);MidiBuffer midi;midi.ensureSize(65536);audio.clear();
    p.request(LooperEngine::Action::record);p.processBlock(audio,midi);p.view();
    p.request(LooperEngine::Action::play);p.processBlock(audio,midi);p.view();
    auto c=p.recordingControl(2);c.enabled=true;c.overwrite=true;c.hearPrevious=true;
    BOOST_REQUIRE(p.setRecordingControl(2,c));BOOST_REQUIRE(p.setMidiMapping(6,{70,1}));
    midi.clear();midi.addEvent(MidiMessage::controllerEvent(1,70,127),0);midi.addEvent(MidiMessage::controllerEvent(1,70,0),1);
    AllocationProbe probe;BOOST_REQUIRE(probe.slot);
    const auto capture=probe.measure([&]{p.processBlock(audio,midi);});
    BOOST_CHECK_EQUAL(capture.first,0);BOOST_CHECK_EQUAL(capture.second,0);p.view();
    for(int i=0;i<8;++i){midi.clear();audio.clear();const auto wrap=probe.measure([&]{p.processBlock(audio,midi);});
        BOOST_CHECK_EQUAL(wrap.first,0);BOOST_CHECK_EQUAL(wrap.second,0);p.view();}
#endif
}

BOOST_AUTO_TEST_CASE(HistoryAndHeldAddCallbacksNeverAllocateOrFree)
{
#if JUCE_MAC
    LooperProcessor p;p.prepareToPlay(1000,100);BOOST_REQUIRE(p.setHistoryLimits(2,.1));
    auto c=p.recordingControl(1);c.holdAction=4;c.start=0;BOOST_REQUIRE(p.setRecordingControl(1,c));
    AudioBuffer<float> audio(2,100);MidiBuffer midi;midi.ensureSize(65536);
    auto run=[&]{audio.clear();p.processBlock(audio,midi);};
    p.request(LooperEngine::Action::record);run();midi.clear();p.view();
    p.request(LooperEngine::Action::play);run();midi.clear();p.view();
    AllocationProbe probe;BOOST_REQUIRE(probe.slot);
    midi.addEvent(MidiMessage::controllerEvent(1,23,127),0);
    for(int i=0;i<12;++i){const auto n=probe.measure(run);BOOST_CHECK_EQUAL(n.first,0);BOOST_CHECK_EQUAL(n.second,0);midi.clear();p.view();}
    midi.addEvent(MidiMessage::controllerEvent(1,23,0),0);
    {const auto n=probe.measure(run);BOOST_CHECK_EQUAL(n.first,0);BOOST_CHECK_EQUAL(n.second,0);}midi.clear();p.view();
    BOOST_REQUIRE(p.requestHistory());
    {const auto n=probe.measure(run);BOOST_CHECK_EQUAL(n.first,0);BOOST_CHECK_EQUAL(n.second,0);}
#endif
}
BOOST_AUTO_TEST_CASE(OverloadPreservesReleaseHeadroomWithoutAudioAllocation)
{
    MidiBuffer input,output;output.ensureSize(BoundedMidi::storage);
    std::vector<uint8> packet(16384,1);packet.front()=0xf0;packet.back()=0xf7;
    for(int i=0;i<8;++i)input.addEvent(packet.data(),int(packet.size()),i);
    for(int i=0;i<10000;++i)input.addEvent(MidiMessage::noteOn(1,60,uint8(100)),8);
    input.addEvent(MidiMessage::controllerEvent(1,23,0),9);
    input.addEvent(MidiMessage::allNotesOff(1),10);
    const auto before=BoundedMidi::dropped.load();
#if JUCE_MAC
    AllocationProbe probe;BOOST_REQUIRE(probe.slot);
    const auto allocations=probe.measure([&]{BoundedMidi::append(output,input,0,32,0);});
    BOOST_CHECK_EQUAL(allocations.first,0);BOOST_CHECK_EQUAL(allocations.second,0);
#else
    BoundedMidi::append(output,input,0,32,0);
#endif
    BOOST_CHECK_GT(BoundedMidi::dropped.load(),before);
    BOOST_CHECK_LE(output.data.size(),BoundedMidi::critical);
    bool release=false,panic=false;for(auto m:output)if(m.numBytes==3){release|=m.data[0]==0xb0&&m.data[1]==23&&m.data[2]==0;panic|=m.data[0]==0xb0&&m.data[1]==123;}
    BOOST_CHECK(release);BOOST_CHECK(panic);
    BoundedMidiCollector collector;collector.reset(48000);
    for(auto m:input){auto message=m.getMessage();message.setTimeStamp(Time::getMillisecondCounterHiRes()*.001);collector.addMessageToQueue(message);}
    output.clear();
#if JUCE_MAC
    const auto drain=probe.measure([&]{collector.removeNextBlockOfMessages(output,32);});
    BOOST_CHECK_EQUAL(drain.first,0);BOOST_CHECK_EQUAL(drain.second,0);
#else
    collector.removeNextBlockOfMessages(output,32);
#endif
    release=false;for(auto m:output)if(m.numBytes==3)release|=m.data[0]==0xb0&&m.data[1]==23&&m.data[2]==0;
    BOOST_CHECK(release);
}
BOOST_AUTO_TEST_CASE(OversizedSysExThroughActualSongAndGraphIsBounded)
{
    PreparedGraph f(48000,64);auto& graph=f.graph;
    auto* in=graph.addNode(new IONode(IONode::midiInputNode));
    graph.addNode(new SongManagerNode());
    auto* out=graph.addNode(new IONode(IONode::midiOutputNode));
    BOOST_REQUIRE(graph.connectChannels(PortType::Midi,in->nodeId,0,out->nodeId,0));graph.rebuild();
    AudioBuffer<float> audio(2,64);MidiBuffer midi;midi.ensureSize(200000);RenderContext rc(audio,audio,midi,64);
    audio.clear();graph.render(rc);
    std::vector<uint8> packet(16384,1);packet.front()=0xf0;packet.back()=0xf7;
    midi.clear();for(int i=0;i<8;++i)midi.addEvent(packet.data(),int(packet.size()),i);
    midi.addEvent(MidiMessage::controllerEvent(1,23,0),9);
#if JUCE_MAC
    AllocationProbe probe;BOOST_REQUIRE(probe.slot);
    const auto counts=probe.measure([&]{graph.render(rc);});
    BOOST_CHECK_EQUAL(counts.first,0);BOOST_CHECK_EQUAL(counts.second,0);
#else
    graph.render(rc);
#endif
    BOOST_CHECK_LE(midi.data.size(),BoundedMidi::critical);
    bool release=false;for(auto m:midi)if(m.numBytes==3)release|=m.data[0]==0xb0&&m.data[1]==23&&m.data[2]==0;
    BOOST_CHECK(release);
}

BOOST_AUTO_TEST_SUITE_END()
