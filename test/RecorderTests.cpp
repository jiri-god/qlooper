// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include <boost/test/unit_test.hpp>
#include "fixture/PreparedGraph.h"
#include "engine/ionode.hpp"
#include "nodes/audioprocessor.hpp"
#include "nodes/looper.hpp"
#include "nodes/recorder.hpp"
#include <element/tags.hpp>
using namespace element;
using namespace juce;
namespace {
struct Rig {
    PreparedGraph fixture {100,8};
    GraphNode& graph=fixture.graph;
    LooperProcessor* looper=new LooperProcessor();
    RecorderNode* recorder=new RecorderNode();
    Processor *input,*loop,*rec,*output;
    File dir=File::getSpecialLocation(File::tempDirectory).getChildFile("jgod-recorder-test-"+Uuid().toString());
    AudioBuffer<float> audio {2,8}; MidiBuffer midi;
    Rig() {
        // Recorder tests assert exact routing/queue samples, independently of loop-edge shaping.
        looper->setCrossfade(0); looper->setCaptureTail(0);
        input=graph.addNode(new IONode(IONode::audioInputNode));
        loop=graph.addNode(new AudioProcessorNode(looper));
        rec=graph.addNode(recorder); output=graph.addNode(new IONode(IONode::audioOutputNode));
        for(int c=0;c<2;++c) {
            BOOST_REQUIRE(graph.connectChannels(PortType::Audio,input->nodeId,c,loop->nodeId,c));
            BOOST_REQUIRE(graph.connectChannels(PortType::Audio,loop->nodeId,c,rec->nodeId,c));
            BOOST_REQUIRE(graph.connectChannels(PortType::Audio,rec->nodeId,c,output->nodeId,c));
        }
        graph.rebuild(); recorder->destination=dir;
        looper->request(LooperEngine::Action::record); render(.25f);
        looper->request(LooperEngine::Action::play); render(0);
    }
    ~Rig(){recorder->releaseResources(); dir.deleteRecursively();}
    void render(float value){for(int c=0;c<2;++c)for(int i=0;i<8;++i)audio.setSample(c,i,value); RenderContext rc(audio,audio,midi,8); graph.render(rc);}
    void finish(){recorder->stopRecording();recorder->releaseResources();BOOST_CHECK(!recorder->isBusy());}
    AudioBuffer<float> read(const String& name,int64_t length){
        AudioFormatManager fm;fm.registerBasicFormats();
        std::unique_ptr<AudioFormatReader> reader(fm.createReaderFor(recorder->lastTake.getChildFile(name)));
        BOOST_REQUIRE(reader!=nullptr);BOOST_CHECK_EQUAL(reader->lengthInSamples,length);
        BOOST_CHECK_EQUAL(reader->sampleRate,100); BOOST_CHECK_EQUAL(reader->numChannels,2);
        AudioBuffer<float> result(2,int(length));reader->read(&result,0,int(length),0,true,true);return result;
    }
    String fileFor(uint32 id){auto json=JSON::parse(recorder->lastTake.getChildFile("take.json"));
        for(auto item:*json["tracks"].getArray())if(int(item["nodeId"])==int(id))return item["file"].toString();
        return{};
    }
};
}
BOOST_AUTO_TEST_SUITE(RecorderTests)
BOOST_AUTO_TEST_CASE (MasterAndStemsShareFramesAndPreserveLiveMonitoring)
{
    Rig r; r.recorder->selected[r.input->nodeId]=false;r.recorder->selected[r.loop->nodeId]=true;
    r.loop->setGain(.5f);r.render(.5f); // finish fader ramp
    BOOST_REQUIRE(r.recorder->startRecording().wasOk());
    for(int i=0;i<3;++i){r.render(.5f);BOOST_CHECK_CLOSE(r.audio.getSample(0,4),.375f,.001f);}
    r.looper->request(LooperEngine::Action::stop); r.render(.5f);
    r.finish();
    auto mix=r.read("00-Live mix.wav",32), stem=r.read(r.fileFor(r.loop->nodeId),32), dry=r.read(r.fileFor(r.input->nodeId),32);
    for(int i=0;i<32;++i){BOOST_CHECK_CLOSE(dry.getSample(0,i),.5f,.001f);
        BOOST_CHECK_SMALL(stem.getSample(0,i)-(i<24?.125f:0),.000001f);
        BOOST_CHECK_CLOSE(mix.getSample(0,i),i<24?.375f:.25f,.001f);}
    BOOST_CHECK(r.looper->getMonitor());
    auto manifest=JSON::parse(r.recorder->lastTake.getChildFile("take.json"));BOOST_CHECK(bool(manifest["complete"]));
}
BOOST_AUTO_TEST_CASE (FullLooperOutputAndSeparateTakesDoNotOverwrite)
{
    Rig r;r.recorder->selected[r.loop->nodeId]=false;
    BOOST_REQUIRE(r.recorder->startRecording().wasOk());r.render(.5f);r.finish();
    const auto first=r.recorder->lastTake;
    auto stem=r.read(r.fileFor(r.loop->nodeId),8);BOOST_CHECK_CLOSE(stem.getSample(0,3),.75f,.001f);
    BOOST_REQUIRE(r.recorder->startRecording().wasOk());r.render(0);r.finish();
    BOOST_CHECK(first!=r.recorder->lastTake);BOOST_CHECK(first.getChildFile("00-Live mix.wav").existsAsFile());
    auto mix=r.read("00-Live mix.wav",8);BOOST_CHECK_CLOSE(mix.getSample(0,3),.25f,.001f);
}
BOOST_AUTO_TEST_CASE (GraphChangesFinishTakeAndMissingSourcesFail)
{
    Rig r;r.recorder->selected[r.loop->nodeId]=true;
    BOOST_REQUIRE(r.recorder->startRecording().wasOk());r.render(0);
    r.graph.rebuild();r.recorder->releaseResources();
    BOOST_CHECK(!r.recorder->isBusy());BOOST_CHECK(r.recorder->statusText().contains("graph changed"));
    r.read("00-Live mix.wav",8);
    r.recorder->selected[99999]=false;BOOST_CHECK(r.recorder->startRecording().failed());
}
BOOST_AUTO_TEST_CASE (FolderFailureAndStateRoundtrip)
{
    Rig r;r.dir.createDirectory();const auto file=r.dir.getChildFile("not-a-directory");file.replaceWithText("preserve me");
    r.recorder->destination=file;BOOST_CHECK(r.recorder->startRecording().failed());BOOST_CHECK_EQUAL(file.loadFileAsString(),"preserve me");
    r.recorder->selected[r.loop->nodeId]=true;MemoryBlock data;r.recorder->getState(data);
    RecorderNode restored;restored.setState(data.getData(),int(data.getSize()));MemoryBlock again;restored.getState(again);
    BOOST_CHECK(data==again);BOOST_CHECK(!restored.isRecording());
}
BOOST_AUTO_TEST_CASE (QueueWrapMaintainsExactSamplesAndStopWithoutAudioCallback)
{
    Rig r;BOOST_REQUIRE(r.recorder->startRecording().wasOk());
    for(int b=0;b<2500;++b){r.render(float(b)/10000);if(b%128==0)Thread::sleep(2);}
    r.finish();auto mix=r.read("00-Live mix.wav",20000);
    for(int b=0;b<2500;++b)BOOST_CHECK_SMALL(mix.getSample(0,b*8)-(.25f+float(b)/10000),.000001f);
}
BOOST_AUTO_TEST_CASE (EditorShowsRecordControls)
{
    Rig r;ValueTree data(types::Node);data.setProperty(tags::object,r.rec,nullptr);Node node(data,false);
    RecorderEditor editor(node);
    if(std::getenv("EL_RECORDER_EDITOR_SNAPSHOT")) {
        auto stream=File("/tmp/jgod-recorder-editor.png").createOutputStream();BOOST_REQUIRE(stream);stream->setPosition(0);stream->truncate();
        PNGImageFormat png;BOOST_CHECK(png.writeImageToStream(editor.createComponentSnapshot(editor.getLocalBounds()),*stream));
    }
    BOOST_CHECK_EQUAL(editor.getWidth(),700);
}
BOOST_AUTO_TEST_SUITE_END()
