// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include <boost/test/unit_test.hpp>
#include "nodes/looperfileimport.hpp"
using namespace element;
using namespace juce;
namespace {
using E=LooperEngine;
LooperProcessor::View info(E& e) {
    LooperProcessor::View v;v.rate=e.getRate();v.capacity=e.getCapacity();v.length=e.getLength();v.importLength=e.importTarget();
    v.count=e.getLayerCount();v.recordings=e.recordingCount();v.linkMode=e.getLinkMode();v.linkBase=e.getLinkBase();v.requestedBars=e.getRequestedBars();
    for(int i=0;i<v.count;++i)v.layers[i]=e.getLayer(i);return v;
}
void setup(E& e) {e.configureDuration(1);e.configureLimits(3,4,true);e.prepare(8000);e.setOptions(false,0,0);e.setTransitionOptions(10,10);}
struct AudioFile {
    File file=File::getSpecialLocation(File::tempDirectory).getNonexistentChildFile("looper-import-test",".wav");
    AudioFile(int frames,double rate=8000,int channels=2) {
        std::unique_ptr<OutputStream> stream=file.createOutputStream();WavAudioFormat format;
        auto writer=format.createWriterFor(stream,AudioFormatWriterOptions().withSampleRate(rate).withNumChannels(channels).withBitsPerSample(32).withSampleFormat(AudioFormatWriterOptions::SampleFormat::floatingPoint));
        BOOST_REQUIRE(writer);AudioBuffer<float> audio(channels,frames);
        for(int c=0;c<channels;++c)for(int i=0;i<frames;++i)audio.setSample(c,i,float((i%97)+1)/200*(c==0?1:-1));
        BOOST_REQUIRE(writer->writeFromAudioSampleBuffer(audio,0,frames));
    }
    ~AudioFile(){file.deleteFile();}
};
}
BOOST_AUTO_TEST_SUITE(LooperImportTests)
BOOST_AUTO_TEST_CASE(FirstImportPaddingSplitAndAtomicHistory) {
    E e;setup(e);AudioFile first(100),shorter(60),longer(250);
    auto a=prepareLooperFile(first.file,info(e),3,4);BOOST_REQUIRE(a.job);BOOST_REQUIRE(e.commitImport(a.job->plan));
    BOOST_CHECK_EQUAL(e.getLength(),100);BOOST_CHECK(e.getState()==E::State::stopped);
    auto b=prepareLooperFile(shorter.file,info(e),3,4);BOOST_REQUIRE(b.job);BOOST_REQUIRE(e.commitImport(b.job->plan));
    auto c=prepareLooperFile(longer.file,info(e),3,4);BOOST_REQUIRE(c.job);BOOST_REQUIRE(e.commitImport(c.job->plan));
    const auto saved=e.snapshot();BOOST_REQUIRE_EQUAL(saved.layers.size(),5);
    BOOST_CHECK_EQUAL(saved.layers[1].audio[0][60],0);
    BOOST_CHECK_EQUAL(saved.layers[4].audio[0][50],0);
    BOOST_CHECK_EQUAL(saved.layers[2].info.take,saved.layers[4].info.take);
    BOOST_CHECK_EQUAL(saved.layers[4].info.part,2);
    BOOST_CHECK_EQUAL(saved.layers[2].captureEnd,-1);BOOST_CHECK(saved.layers[2].pre[0].empty());
    e.apply(E::Action::undo);BOOST_CHECK_EQUAL(e.getLayerCount(),2);BOOST_CHECK(e.getState()==E::State::stopped);
    e.apply(E::Action::redo);BOOST_CHECK_EQUAL(e.getLayerCount(),5);BOOST_CHECK(e.getState()==E::State::stopped);
    auto over=prepareLooperFile(first.file,info(e),3,4);BOOST_CHECK(!over.job);
}
BOOST_AUTO_TEST_CASE(LimitsStalePlansAndMasterConstraints) {
    E e;setup(e);AudioFile file(100),longFile(401);
    e.setLinkContext(2,100,0,0);
    auto tooLong=prepareLooperFile(longFile.file,info(e),3,4);BOOST_CHECK(!tooLong.job);BOOST_CHECK_EQUAL(e.getLayerCount(),0);
    auto a=prepareLooperFile(file.file,info(e),3,4);BOOST_REQUIRE(a.job);
    e.setLinkContext(2,200,0,0);BOOST_CHECK(!e.commitImport(a.job->plan));BOOST_CHECK_EQUAL(e.getLayerCount(),0);
    auto b=prepareLooperFile(file.file,info(e),3,4);BOOST_REQUIRE(b.job);BOOST_REQUIRE(e.commitImport(b.job->plan));
    BOOST_CHECK_EQUAL(e.getLength(),200);BOOST_CHECK_EQUAL(e.getFixedLength(),200);
    auto stale=prepareLooperFile(file.file,info(e),3,4);BOOST_REQUIRE(stale.job);
    e.resetAll();BOOST_CHECK(!e.commitImport(stale.job->plan));
    e.setLinkContext(2,0,0,0);BOOST_CHECK(!prepareLooperFile(file.file,info(e),3,4).job);
    e.clearLinkContext();e.setClock(120,4,4,0,true);e.setFixedBars(1);
    BOOST_CHECK(!prepareLooperFile(file.file,info(e),3,4).job); // two seconds > 1 sec capacity
}
BOOST_AUTO_TEST_CASE(MonoAndFiniteResamplingPreserveDuration) {
    E e;setup(e);AudioFile file(4410,44100,1);
    auto a=prepareLooperFile(file.file,info(e),3,4);BOOST_REQUIRE(a.job);BOOST_CHECK_EQUAL(a.job->plan.target,800);
    BOOST_CHECK(a.job->plan.parts[0][0]==a.job->plan.parts[0][1]);
    BOOST_REQUIRE(e.commitImport(a.job->plan));BOOST_CHECK_EQUAL(e.getLength(),800);
    AudioBuffer<float> signal(2,1000);signal.clear();signal.setSample(0,999,1);
    auto converted=resampleImportedAudio(signal,8000,16000);
    BOOST_CHECK_EQUAL(converted[0].size(),2000);BOOST_CHECK_SMALL(converted[0][0],1e-8f);
}
BOOST_AUTO_TEST_CASE(ProcessorImportSurvivesSessionAndOfflineUndo) {
    LooperProcessor p;p.setMemoryLimits(1,3,4,true);p.prepareToPlay(8000,32);AudioFile file(100);
    auto prepared=prepareLooperFile(file.file,p.view(),3,4);BOOST_REQUIRE(prepared.job);
    BOOST_REQUIRE(p.submitImport(std::move(prepared.job)));p.serviceOfflineControls();BOOST_CHECK_EQUAL(p.collectImport(),1);
    auto v=p.view();BOOST_CHECK_EQUAL(v.count,1);BOOST_CHECK_EQUAL(v.length,100);
    p.layerPreviewVisible=false;MemoryBlock state;p.getStateInformation(state);
    LooperProcessor restored;restored.setStateInformation(state.getData(),int(state.getSize()));
    BOOST_CHECK(!restored.layerPreviewVisible.load());
    BOOST_CHECK_EQUAL(restored.view().count,1);BOOST_CHECK_EQUAL(restored.view().length,100);
    p.request(E::Action::undo);p.serviceOfflineControls();BOOST_CHECK_EQUAL(p.view().count,0);
    p.request(E::Action::redo);p.serviceOfflineControls();BOOST_CHECK_EQUAL(p.view().count,1);BOOST_CHECK_EQUAL(p.view().state,3);
}
BOOST_AUTO_TEST_CASE(WaveformIsRawAndIndependentOfLayerGain)
{
    LooperProcessor p;p.setMemoryLimits(1,3,4,true);p.prepareToPlay(8000,32);AudioFile file(100);
    auto prepared=prepareLooperFile(file.file,p.view(),3,4);BOOST_REQUIRE(prepared.job);
    BOOST_REQUIRE(p.submitImport(std::move(prepared.job)));p.serviceOfflineControls();BOOST_REQUIRE_EQUAL(p.collectImport(),1);
    auto waves=p.waveforms();BOOST_REQUIRE_EQUAL(waves.size(),1);
    BOOST_CHECK_GT(*std::max_element(waves[0]->high.begin(),waves[0]->high.end()),.4f);
    BOOST_CHECK_LT(*std::min_element(waves[0]->low.begin(),waves[0]->low.end()),-.4f);
    const auto id=p.view().layers[0].id;BOOST_REQUIRE(p.editLayer(id,2,.1f));p.serviceOfflineControls();
    auto quiet=p.waveforms();BOOST_REQUIRE_EQUAL(quiet.size(),1);
    BOOST_CHECK(waves[0]->low==quiet[0]->low);BOOST_CHECK(waves[0]->high==quiet[0]->high);
}
BOOST_AUTO_TEST_CASE(AdvertisedFormatsAndOptionalDecodeFixture)
{
    AudioFormatManager formats;formats.registerBasicFormats();
    for(const char* extension:{"wav","aiff","flac"})BOOST_REQUIRE(formats.findFormatForFileExtension(extension));
#if JUCE_MAC
    for(const char* extension:{"mp3","m4a","aac"})BOOST_REQUIRE(formats.findFormatForFileExtension(extension));
#endif
    if(const auto* path=std::getenv("EL_LOOPER_IMPORT_FIXTURE")) {
        E e;setup(e);auto imported=prepareLooperFile(File(path),info(e),3,4);
        BOOST_REQUIRE_MESSAGE(imported.job,imported.error.toStdString());BOOST_CHECK(e.commitImport(imported.job->plan));
    }
}
BOOST_AUTO_TEST_SUITE_END()
