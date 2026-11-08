// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include <boost/test/unit_test.hpp>
#include "fixture/PreparedGraph.h"
#include "fixture/TestNode.h"
#include "engine/ionode.hpp"
#include "nodes/audioprocessor.hpp"
#include "nodes/eqfilter.hpp"
#include <thread>
#include <chrono>
using namespace element;
using namespace juce;
namespace {
struct Probe : EQFilterProcessor {
    std::atomic<bool> inside {false}, exit {false}, releasedInside {false};
    bool stall = false;
    void prepareToPlay (double rate, int block) override {
        EQFilterProcessor::prepareToPlay(rate,block);setLatencySamples(257);
    }
    void processBlock (AudioBuffer<float>&, MidiBuffer&) override {
        inside=true;
        if (stall) while(!exit.load()) std::this_thread::yield();
        else std::this_thread::yield();
        inside=false;
    }
    void releaseResources() override { if(inside.load()) releasedInside=true; }
};
struct Head : AudioPlayHead {
    Optional<PositionInfo> getPosition() const override {
        PositionInfo p;p.setTimeInSamples(-48000);p.setTimeInSeconds(-1);p.setPpqPosition(3);p.setBpm(120);return p;
    }
};
}
BOOST_AUTO_TEST_SUITE(HostAudioSafetyTests)
BOOST_AUTO_TEST_CASE(OversamplingUsesPluginSampleUnitsAndBaseRateLatency)
{
    PreparedGraph f(48000,128);Head head;f.graph.setPlayHead(&head);
    auto* proc=new Probe;auto* node=new AudioProcessorNode(proc);f.graph.addNode(node);
    for(int factor : {1,2,4,1}) {
        node->setOversamplingFactor(factor);
        BOOST_CHECK_EQUAL(proc->getSampleRate(),48000*factor);
        const auto pos=proc->getPlayHead()->getPosition();BOOST_REQUIRE(pos.hasValue());
        BOOST_CHECK_EQUAL(*pos->getTimeInSamples(),-48000*factor);
        BOOST_CHECK_EQUAL(*pos->getTimeInSeconds(),-1);
        BOOST_CHECK_EQUAL(*pos->getPpqPosition(),3);
        // Isolate plugin latency from the host oversampling filter latency.
        const auto before=node->getLatencySamples();proc->setLatencySamples(0);
        node->audioProcessorChanged(proc,AudioProcessorListener::ChangeDetails().withLatencyChanged(true));
        BOOST_CHECK_EQUAL(before-node->getLatencySamples(),(257+factor-1)/factor);
        proc->setLatencySamples(257);
        node->audioProcessorChanged(proc,AudioProcessorListener::ChangeDetails().withLatencyChanged(true));
    }
    f.graph.setPlayHead(nullptr);
}
BOOST_AUTO_TEST_CASE(DisableWaitsForWholeInFlightBlock)
{
    PreparedGraph f(48000,128);auto* proc=new Probe;proc->stall=true;
    auto* node=new AudioProcessorNode(proc);f.graph.addNode(node);f.graph.rebuild();
    std::thread audio([&]{AudioBuffer<float> b(2,128);b.clear();MidiBuffer m;RenderContext rc(b,b,m,128);f.graph.render(rc);});
    while(!proc->inside.load()) std::this_thread::yield();
    std::thread finish([&]{std::this_thread::sleep_for(std::chrono::milliseconds(30));proc->exit=true;});
    node->setEnabled(false);finish.join();audio.join();
    BOOST_CHECK(!proc->releasedInside.load());BOOST_CHECK(!node->isEnabled());
    node->setEnabled(true);BOOST_CHECK(node->isEnabled());
}

BOOST_AUTO_TEST_CASE(RepeatedReconfigurationWhileRendering)
{
    PreparedGraph f(48000,128);auto* proc=new Probe;auto* node=new AudioProcessorNode(proc);
    f.graph.addNode(node);f.graph.rebuild();std::atomic<bool> done{false};std::atomic<int> blocks{0};
    std::thread audio([&]{
        AudioBuffer<float> b(2,128);b.clear();MidiBuffer m;RenderContext rc(b,b,m,128);
        while(!done.load()){f.graph.render(rc);++blocks;}
    });
    while(blocks.load()<10)std::this_thread::yield();
    for(int i=0;i<300;++i){node->setEnabled(false);node->setEnabled(true);}
    done=true;audio.join();
    BOOST_CHECK(!proc->releasedInside.load());BOOST_CHECK(node->isEnabled());BOOST_CHECK_GT(blocks.load(),10);
}
BOOST_AUTO_TEST_CASE(QueuedEnableRequestsKeepLatestDesiredState)
{
    PreparedGraph f;auto* node=new AudioProcessorNode(new Probe);f.graph.addNode(node);
    std::thread control([&]{node->setEnabled(false);node->setEnabled(true);});control.join();
    MessageManager::getInstance()->runDispatchLoopUntil(20);
    BOOST_CHECK(node->isEnabled());
    std::thread os([&]{node->setOversamplingFactor(4);});os.join();
    MessageManager::getInstance()->runDispatchLoopUntil(20);
    BOOST_CHECK_EQUAL(node->getOversamplingFactor(),4);BOOST_CHECK(node->isEnabled());
}
BOOST_AUTO_TEST_CASE(UnchangedDelayArcsKeepTheirHistoryAcrossRebuild)
{
    struct Source : TestNode {
        Source(float v,int delay):TestNode(0,1,0,0),value(v){setLatencySamples(delay);}
        void render(RenderContext& rc) override {for(int i=0;i<rc.audio.getNumSamples();++i)rc.audio.setSample(0,i,value);}
        float value;
    };
    PreparedGraph f(48000,128);auto& g=f.graph;
    auto* a=g.addNode(new Source(.25f,0));auto* b=g.addNode(new Source(.5f,257));
    auto* out=g.addNode(new IONode(IONode::audioOutputNode));
    BOOST_REQUIRE(g.connectChannels(PortType::Audio,a->nodeId,0,out->nodeId,0));
    BOOST_REQUIRE(g.connectChannels(PortType::Audio,b->nodeId,0,out->nodeId,0));g.rebuild();
    AudioBuffer<float> audio(2,128);MidiBuffer midi;RenderContext rc(audio,audio,midi,128);
    for(int n=0;n<5;++n)g.render(rc);
    BOOST_CHECK_SMALL(audio.getSample(0,127)-.75f,1e-6f);
    g.addNode(new Source(0,0));g.rebuild();g.render(rc);
    for(int i=0;i<128;++i)BOOST_CHECK_SMALL(audio.getSample(0,i)-.75f,1e-6f);
}
BOOST_AUTO_TEST_CASE(EQShapeTransitionsRemainContinuousAtDifferentRates)
{
    for(double rate:{44100.,48000.,192000.}) {
        EQFilter filter;filter.reset(rate);filter.setFrequency(1000);filter.setQ(.707f);filter.setGain(1);
        for(int i=0;i<int(rate);++i){float x=1;filter.processBlock(&x,1);}
        float before=1;filter.processBlock(&before,1);filter.setShape(EQFilter::HighPass);
        float after=1;filter.processBlock(&after,1);
        BOOST_CHECK_SMALL(after-before,1e-4f);
        for(int i=0;i<int(rate*.1);++i) {
            if(i%100==0)filter.setShape((i/100)%2 ? EQFilter::LowPass : EQFilter::HighPass);
            float x=.25f;filter.processBlock(&x,1);BOOST_CHECK(std::isfinite(x));BOOST_CHECK_LT(std::abs(x),4.f);
        }
    }
}
BOOST_AUTO_TEST_SUITE_END()
