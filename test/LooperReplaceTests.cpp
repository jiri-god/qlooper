// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include <boost/test/unit_test.hpp>
#include "nodes/looperengine.hpp"
using namespace element;
namespace {
using Action=LooperEngine::Action;
void process(LooperEngine& e,float value=0){float right=value;e.process(value,right,1,false);}
void loadPattern(LooperEngine& e,int length,int layers=1){
    e.prepare(1000);e.setTransitionOptions(0,0);
    LooperEngine::Snapshot saved;saved.length=length;
    for(int i=0;i<layers;++i){LooperEngine::SavedLayer layer;
        for(auto& ch:layer.audio){ch.resize(length);for(int f=0;f<length;++f)ch[f]=float((i+1)*10+f);}
        saved.layers.push_back(std::move(layer));}
    e.load(saved);e.apply(Action::play);
}
}
BOOST_AUTO_TEST_SUITE(LooperReplaceTests)
BOOST_AUTO_TEST_CASE(ReplaceIsNonDestructiveCoverageMasksAllEarlierLayersAndSilence)
{
    LooperEngine e;loadPattern(e,8,2);process(e);process(e);
    e.apply(Action::replace);process(e,99);process(e,0);e.apply(Action::endReplace);
    BOOST_REQUIRE_EQUAL(e.getLayerCount(),3);BOOST_CHECK(e.getLayer(2).replace);
    for(int f=0;f<8;++f){BOOST_CHECK_EQUAL(e.layerSample(0,0,f),10+f);BOOST_CHECK_EQUAL(e.layerSample(1,0,f),20+f);
        BOOST_CHECK_EQUAL(e.sample(0,f),f==2?99:f==3?0:30+2*f);}
    e.editLayer(e.getLayer(2).id,0,0);BOOST_CHECK_EQUAL(e.sample(0,2),34);
    e.editLayer(e.getLayer(2).id,0,1);e.editLayer(e.getLayer(2).id,5,0);BOOST_CHECK_EQUAL(e.sample(0,2),133);
    e.apply(Action::undo);BOOST_REQUIRE_EQUAL(e.getLayerCount(),2);
    e.apply(Action::redo);BOOST_REQUIRE_EQUAL(e.getLayerCount(),3);BOOST_CHECK_EQUAL(e.layerSample(2,0,2),99);
}
BOOST_AUTO_TEST_CASE(WrappedCoverageAndLaterAddRemainAudible)
{
    LooperEngine e;loadPattern(e,8);for(int i=0;i<7;++i)process(e);
    e.apply(Action::replace);process(e,77);process(e,88);e.apply(Action::endReplace);
    BOOST_CHECK_EQUAL(e.sample(0,7),77);BOOST_CHECK_EQUAL(e.sample(0,0),88);BOOST_CHECK_EQUAL(e.sample(0,1),11);
    e.apply(Action::overdub);process(e,5);e.apply(Action::overdub);
    BOOST_CHECK_EQUAL(e.sample(0,1),16);
    e.editLayer(e.getLayer(1).id,1,1);BOOST_CHECK_EQUAL(e.sample(0,1),0);
}
BOOST_AUTO_TEST_CASE(ShortRecordAddsFixedLengthTakeAndUndoRestoresMuteState)
{
    LooperEngine e;loadPattern(e,8,2);e.editLayer(e.getLayer(0).id,0,0);
    e.apply(Action::record);BOOST_REQUIRE_EQUAL(e.getLayerCount(),3);
    BOOST_CHECK(!e.getLayer(0).enabled);BOOST_CHECK(!e.getLayer(1).enabled);BOOST_CHECK(!e.startedByOverdub());
    process(e,4);process(e,5);e.apply(Action::record);
    BOOST_CHECK(e.getState()==LooperEngine::State::playing);BOOST_CHECK_EQUAL(e.getLength(),8);
    e.apply(Action::undo);BOOST_REQUIRE_EQUAL(e.getLayerCount(),2);
    BOOST_CHECK(!e.getLayer(0).enabled);BOOST_CHECK(e.getLayer(1).enabled);
    e.apply(Action::redo);BOOST_REQUIRE_EQUAL(e.getLayerCount(),3);BOOST_CHECK(!e.getLayer(1).enabled);
}
BOOST_AUTO_TEST_CASE(FreshRecordReleasesIndependentLengthButPreservesMasterConstraint)
{
    LooperEngine e;loadPattern(e,8);e.apply(Action::freshRecord);process(e,1);process(e,2);e.apply(Action::record);
    BOOST_CHECK_EQUAL(e.getLayerCount(),1);BOOST_CHECK_EQUAL(e.getLength(),2);
    auto saved=e.snapshot();saved.fixedLength=2;saved.multiple=1;e.load(saved);e.setLinkContext(1,2,0,0);
    e.apply(Action::freshRecord);process(e,3);e.apply(Action::record);
    BOOST_CHECK_EQUAL(e.getFixedLength(),2);BOOST_CHECK_EQUAL(e.getLength(),2);
}
BOOST_AUTO_TEST_CASE(FirstOverdubSyncArmsNextBarAndDoesNotCaptureBeforeBoundary)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(0,0);e.setOptions(true,20,0);
    e.setClock(60,1,4,0.5,true);e.apply(Action::overdub);
    BOOST_CHECK(e.isStartPending());BOOST_CHECK_EQUAL(e.getLayerCount(),0);
    for(int i=0;i<500;++i)process(e,9);BOOST_CHECK_EQUAL(e.getLayerCount(),0);
    process(e,7);BOOST_CHECK_EQUAL(e.getLayerCount(),1);BOOST_CHECK_EQUAL(e.layerSample(0,0,0),7);
}
BOOST_AUTO_TEST_CASE(RecordArmingDoesNotMuteOldLayersUntilActualStart)
{
    LooperEngine e;loadPattern(e,100);e.setOptions(true,20,0);e.setRecordNextBar(true);e.setClock(600,1,4,0.5,true);
    e.apply(Action::record);BOOST_CHECK(e.isStartPending());BOOST_CHECK(e.getLayer(0).enabled);
    for(int i=0;i<50;++i)process(e);BOOST_CHECK(e.getLayer(0).enabled);
    process(e);BOOST_CHECK(!e.getLayer(0).enabled);BOOST_CHECK_EQUAL(e.getLength(),100);
}
BOOST_AUTO_TEST_CASE(TailWrapsWithoutChangingLengthAndPersistsRawHandles)
{
    LooperEngine e;loadPattern(e,20);e.setTransitionOptions(0,4);
    for(int i=0;i<18;++i)process(e);e.apply(Action::overdub);process(e,3);e.apply(Action::overdub);
    BOOST_CHECK(e.isTailPending());for(int i=0;i<4;++i)process(e,4);
    BOOST_CHECK(!e.isTailPending());BOOST_CHECK_EQUAL(e.getLength(),20);
    BOOST_CHECK_EQUAL(e.sample(0,18),31); // old28 + recorded3
    BOOST_CHECK_EQUAL(e.sample(0,19),33); // old29 + first tail4
    BOOST_CHECK_EQUAL(e.sample(0,0),13);  // old10 + tail3
    BOOST_CHECK_EQUAL(e.sample(0,1),13);  // old11 + tail2
    BOOST_CHECK_EQUAL(e.sample(0,2),13);  // old12 + tail1
    auto saved=e.snapshot();BOOST_REQUIRE_EQUAL(saved.layers[1].post[0].size(),4);
    BOOST_CHECK_EQUAL(saved.layers[1].captureEnd,19);BOOST_CHECK_EQUAL(saved.layers[1].coverageFrames,1);
    LooperEngine restored;restored.prepare(1000);restored.setTransitionOptions(0,4);restored.load(saved);
    for(int f=0;f<20;++f)BOOST_CHECK_EQUAL(restored.sample(0,f),e.sample(0,f));
}
BOOST_AUTO_TEST_CASE(ReplaceTailReturnsToUnderlyingMixInMiddleOfCycle)
{
    LooperEngine e;loadPattern(e,20);e.setTransitionOptions(0,4);
    e.apply(Action::replace);process(e,100);e.apply(Action::endReplace);
    for(int i=0;i<4;++i)process(e,100);
    BOOST_CHECK_EQUAL(e.sample(0,0),100);BOOST_CHECK_EQUAL(e.sample(0,1),100);
    BOOST_CHECK_EQUAL(e.sample(0,2),78); // .25*12+.75*100
    BOOST_CHECK_EQUAL(e.sample(0,5),15);
    e.editLayer(e.getLayer(1).id,0,0);BOOST_CHECK_EQUAL(e.sample(0,1),11);
}
BOOST_AUTO_TEST_CASE(CrossfadeKeepsConstantSignalAtSeamAndRawSamplesUnchanged)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(4,4);
    for(int i=0;i<10;++i)process(e,1);e.apply(Action::record);
    for(int i=0;i<20;++i)process(e,1);e.apply(Action::record);
    for(int i=0;i<4;++i)process(e,1);
    BOOST_CHECK_EQUAL(e.getLength(),20);
    for(int f=0;f<20;++f){BOOST_CHECK_EQUAL(e.sample(0,f),1);BOOST_CHECK_EQUAL(e.layerSample(0,0,f),1);}
    auto saved=e.snapshot();BOOST_CHECK_EQUAL(saved.layers[0].pre[0].size(),4);BOOST_CHECK_EQUAL(saved.layers[0].post[0].size(),4);
}
BOOST_AUTO_TEST_CASE(MaxLayersRejectsReplaceAndResetDropsFinishingCapture)
{
    LooperEngine e;e.configureLimits(8,8,false);loadPattern(e,100,8);e.apply(Action::replace);BOOST_CHECK(!e.isReplacing());BOOST_CHECK_EQUAL(e.getLayerCount(),8);
    e.resetAll();e.setTransitionOptions(5,5);e.apply(Action::record);process(e,1);e.apply(Action::record);
    BOOST_CHECK(e.isTailPending());e.resetAll();BOOST_CHECK(!e.isClonePending());BOOST_CHECK_EQUAL(e.getLayerCount(),0);
}
BOOST_AUTO_TEST_CASE(HandleCaptureHasFixedWriteBudget)
{
    LooperEngine e;loadPattern(e,6000);e.setTransitionOptions(250,2000);
    for(int i=0;i<500;++i)process(e,1);
    int writes=0;e.setWriteObserver(&writes,[](void* context,float* p,float value)noexcept{++*static_cast<int*>(context);std::atomic_ref<float>(*p).store(value);});
    e.apply(Action::replace);
    for(int i=0;i<100;++i){writes=0;process(e,2);BOOST_CHECK_LE(writes,70);}
    e.apply(Action::endReplace);for(int i=0;i<100;++i){writes=0;process(e,3);BOOST_CHECK_LE(writes,70);}
    e.setWriteObserver(nullptr,nullptr);
}
BOOST_AUTO_TEST_CASE(RetrospectiveRecordPreHandleBelongsBeforeLogicalBarStart)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(4,4);e.setOptions(true,10,0);
    e.setClock(600,1,4,0,true);e.setRecordNextBar(false);
    for(int i=0;i<120;++i)process(e,float(i));
    e.apply(Action::record);process(e,120);
    auto saved=e.snapshot();BOOST_REQUIRE_EQUAL(saved.layers[0].pre[0].size(),4);
    for(int i=0;i<4;++i)BOOST_CHECK_EQUAL(saved.layers[0].pre[0][i],96+i);
    BOOST_CHECK_EQUAL(saved.layers[0].audio[0][0],100);
}
BOOST_AUTO_TEST_CASE(LateQuantizedEndRetainsAlreadyRecordedTailAtCorrectOffset)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(0,5);e.setOptions(true,10,0);
    e.setClock(600,1,4,0,true);e.apply(Action::record);
    for(int i=0;i<100;++i)process(e,2);for(int i=0;i<3;++i)process(e,9);
    e.apply(Action::record);BOOST_CHECK_EQUAL(e.getLength(),100);
    process(e,8);process(e,8);
    auto saved=e.snapshot();BOOST_REQUIRE_EQUAL(saved.layers[0].post[0].size(),5);
    for(int i=0;i<5;++i)BOOST_CHECK_EQUAL(saved.layers[0].post[0][i],i<3?9:8);
    BOOST_CHECK(!e.isTailPending());
}
BOOST_AUTO_TEST_CASE(FinishingTailRejectsStaleLayerDeletionAndModeCommands)
{
    LooperEngine e;loadPattern(e,20);e.setTransitionOptions(0,5);
    e.apply(Action::overdub);process(e,2);e.apply(Action::overdub);
    const auto id=e.getLayer(1).id;e.editLayer(id,3,1);e.editLayer(id,5,1);
    BOOST_CHECK_EQUAL(e.getLayerCount(),2);BOOST_CHECK(!e.getLayer(1).replace);
    for(int i=0;i<5;++i)process(e);
    e.editLayer(id,5,1);BOOST_CHECK(e.getLayer(1).replace);
}
BOOST_AUTO_TEST_CASE(LiveTailIsNotMonitoredTwice)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(0,5);e.apply(Action::record);
    for(int i=0;i<20;++i)process(e,1);e.apply(Action::record);
    float left=10,right=10;
    for(int i=0;i<3;++i){left=right=10;e.process(left,right,1,true);}
    BOOST_CHECK_CLOSE(left,11,0.001f);
}
BOOST_AUTO_TEST_CASE(LiveReplaceTailRestoresUnderlyingMixGraduallyWithMonitoring)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(0,100);
    std::array<std::vector<float>,2> audio;for(auto& ch:audio)ch.assign(1000,1);e.load(audio);e.apply(Action::play);
    for(int i=0;i<10;++i)process(e);
    e.apply(Action::replace);for(int i=0;i<10;++i)process(e,0);e.apply(Action::endReplace);
    float left=0,right=0;for(int i=0;i<11;++i){left=right=0;e.process(left,right,1,true);}
    BOOST_CHECK_CLOSE(left,0.1f,0.01f);
}
BOOST_AUTO_TEST_CASE(MusicalTailLongerThanCrossfadeIsAdditiveAndIndependentlyAdjustable)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(2,8);e.apply(Action::record);
    for(int i=0;i<20;++i)process(e,1);e.apply(Action::record);for(int i=0;i<8;++i)process(e,1);
    BOOST_CHECK_CLOSE(e.sample(0,2),1.75f,0.001f);
    e.setTransitionOptions(8,2);BOOST_CHECK_CLOSE(e.sample(0,2),1.0f,0.001f);
    BOOST_CHECK_EQUAL(e.getLength(),20);
}
BOOST_AUTO_TEST_CASE(IncreasingTailAfterCaptureCannotCreateAbruptUnrecordedSuffix)
{
    LooperEngine e;loadPattern(e,20);e.setTransitionOptions(0,4);
    e.apply(Action::overdub);process(e,1);e.apply(Action::overdub);for(int i=0;i<4;++i)process(e,4);
    e.setTransitionOptions(0,100);
    BOOST_CHECK_EQUAL(e.sample(0,4),15); // base14 + last available tail1, not nearly-full4
    BOOST_CHECK_EQUAL(e.sample(0,5),15);
}
BOOST_AUTO_TEST_CASE(LinkedSilentPrefixCoverageSurvivesSaveAndLoad)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(0,0);e.setLinkContext(3,10,3,0);
    e.apply(Action::record);for(int i=0;i<7;++i)process(e,1);e.apply(Action::record);
    e.editLayer(e.getLayer(0).id,5,1);
    auto saved=e.snapshot();BOOST_REQUIRE_EQUAL(saved.layers.size(),1);
    BOOST_CHECK_EQUAL(saved.layers[0].coverageStart,3);BOOST_CHECK_EQUAL(saved.layers[0].coverageFrames,7);
    auto view=e.snapshotView();BOOST_CHECK_EQUAL(view.layers[0].coverageStart,3);BOOST_CHECK_EQUAL(view.layers[0].coverageFrames,7);
    LooperEngine restored;restored.prepare(1000);restored.setTransitionOptions(0,0);restored.load(saved);
    auto again=restored.snapshot();BOOST_CHECK_EQUAL(again.layers[0].coverageStart,3);BOOST_CHECK_EQUAL(again.layers[0].coverageFrames,7);
    for(int f=0;f<10;++f)BOOST_CHECK_EQUAL(restored.sample(0,f),e.sample(0,f));
}
BOOST_AUTO_TEST_CASE(LiveReplaceOnsetUsesConfiguredCrossfade)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(100,0);
    std::array<std::vector<float>,2> audio;for(auto& ch:audio)ch.assign(1000,1);e.load(audio);e.apply(Action::play);
    for(int i=0;i<10;++i)process(e);
    e.apply(Action::replace);
    float left=0,right=0;
    for(int i=0;i<50;++i){left=right=0;e.process(left,right,1,true);}
    BOOST_CHECK_CLOSE(left,0.5f,0.01f);
    for(int i=0;i<50;++i){left=right=0;e.process(left,right,1,true);}
    BOOST_CHECK_SMALL(left,0.00001f);
    e.apply(Action::endReplace);e.resetAll();
    e.setTransitionOptions(0,0);e.load(audio);e.apply(Action::play);for(int i=0;i<10;++i)process(e);
    e.apply(Action::replace);for(int i=0;i<3;++i){left=right=0;e.process(left,right,1,true);}
    BOOST_CHECK_SMALL(left,0.00001f);
}
BOOST_AUTO_TEST_CASE(NewRecordTakeFadesOldLayersOverConfiguredCrossfade)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(100,0);
    std::array<std::vector<float>,2> audio;for(auto& ch:audio)ch.assign(1000,1);e.load(audio);e.apply(Action::play);
    for(int i=0;i<10;++i)process(e);
    e.apply(Action::record);
    float left=0,right=0;
    for(int i=0;i<50;++i){left=right=0;e.process(left,right,1,true);}
    BOOST_CHECK_CLOSE(left,0.5f,0.01f);
    for(int i=0;i<50;++i){left=right=0;e.process(left,right,1,true);}
    BOOST_CHECK_SMALL(left,0.00001f);
}
BOOST_AUTO_TEST_CASE(NewRecordFadePreservesPriorReplaceCompositeIncludingSilentMasks)
{
    for(bool silentReplace:{false,true}) {
        LooperEngine e;e.prepare(1000);e.setTransitionOptions(100,0);
        LooperEngine::Snapshot saved;saved.length=1000;
        for(int i=0;i<2;++i) {
            LooperEngine::SavedLayer layer;
            for(auto& ch:layer.audio)ch.assign(1000,i==0?1.0f:2.0f);
            layer.info.replace=i==1;
            if(i==1 && silentReplace)layer.info.gain=0;
            saved.layers.push_back(std::move(layer));
        }
        e.load(saved);e.apply(Action::play);for(int i=0;i<10;++i)process(e);
        e.apply(Action::record);float left=0,right=0;
        for(int i=0;i<50;++i){left=right=0;e.process(left,right,1,true);}
        if(silentReplace)BOOST_CHECK_SMALL(left,0.00001f);
        else BOOST_CHECK_CLOSE(left,1.0f,0.01f); // old composite2 times50%, never base1+Replace2
        for(int i=0;i<51;++i){left=right=0;e.process(left,right,1,true);}
        BOOST_CHECK_SMALL(left,0.00001f);
    }
}
BOOST_AUTO_TEST_CASE(NewRecordFadePreservesPriorSoloExclusions)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(100,0);
    LooperEngine::Snapshot saved;saved.length=1000;
    for(int i=0;i<2;++i){LooperEngine::SavedLayer layer;for(auto& ch:layer.audio)ch.assign(1000,i==0?1.0f:9.0f);
        layer.info.solo=i==0;layer.info.replace=i==1;saved.layers.push_back(std::move(layer));}
    e.load(saved);e.apply(Action::play);for(int i=0;i<10;++i)process(e);
    e.apply(Action::record);float left=0,right=0;
    for(int i=0;i<50;++i){left=right=0;e.process(left,right,1,true);}
    BOOST_CHECK_CLOSE(left,0.5f,0.01f);
}
BOOST_AUTO_TEST_CASE(HeldReplacePreservesPhysicalPressAttackAt48kWithoutBulkCopy)
{
    LooperEngine e;e.prepare(48000);e.setTransitionOptions(0,0);
    std::array<std::vector<float>,2> audio;for(auto& ch:audio)ch.assign(96000,0.25f);e.load(audio);e.apply(Action::play);
    for(int i=0;i<100;++i)process(e);
    for(int i=0;i<38400;++i)process(e,i==0?0.9f:0.1f);
    int writes=0;e.setWriteObserver(&writes,[](void* ctx,float* p,float v)noexcept{++*static_cast<int*>(ctx);std::atomic_ref<float>(*p).store(v);});
    BOOST_REQUIRE(e.beginHeldReplace(38400));BOOST_CHECK_EQUAL(writes,0);
    BOOST_CHECK_EQUAL(e.layerSample(1,0,100),0.9f); // virtual source is coherent before materialization
    auto view=e.snapshotView();BOOST_REQUIRE(view.layers[1].fallbackChannels[0]!=nullptr);
    BOOST_CHECK_EQUAL(view.layers[1].cloneLength,38400);BOOST_CHECK_GT(view.layers[1].fallbackCapacity,38400);
    for(int i=0;i<1201;++i){writes=0;process(e,0.2f);BOOST_CHECK_LE(writes,70);}
    BOOST_CHECK(!e.isClonePending());BOOST_CHECK_EQUAL(e.layerSample(1,0,100),0.9f);
    BOOST_CHECK_EQUAL(e.layerSample(1,0,38499),0.1f);BOOST_CHECK_EQUAL(e.layerSample(1,0,38500),0.2f);
    e.setWriteObserver(nullptr,nullptr);
}
BOOST_AUTO_TEST_CASE(OrdinaryReplaceDoesNotCaptureEarlierAudio)
{
    LooperEngine e;loadPattern(e,2000);for(int i=0;i<800;++i)process(e,i==0?9.0f:1.0f);
    e.apply(Action::replace);process(e,2);e.apply(Action::endReplace);
    BOOST_CHECK_EQUAL(e.layerSample(1,0,0),0);BOOST_CHECK_EQUAL(e.layerSample(1,0,800),2);
}
BOOST_AUTO_TEST_CASE(HeldReplaceShortLoopRetainsLatestPassAndLiveWritesWin)
{
    LooperEngine e;loadPattern(e,100);
    for(int i=0;i<800;++i)process(e,float(i));
    BOOST_REQUIRE(e.beginHeldReplace(800));
    for(int f=0;f<100;++f)BOOST_CHECK_EQUAL(e.layerSample(1,0,f),700+f);
    // Backfill and live recording overlap the same short circular buffer.
    for(int i=0;i<20;++i)process(e,float(1000+i));
    for(int f=0;f<100;++f)BOOST_CHECK_EQUAL(e.layerSample(1,0,f),f<20?1000+f:700+f);
    BOOST_CHECK_EQUAL(e.getLength(),100);
}
BOOST_AUTO_TEST_CASE(HeldReplaceMissingHistoryDoesNotMaskUnrecordedPrefix)
{
    LooperEngine e;loadPattern(e,2000);
    for(int i=0;i<5;++i)process(e,float(i+1));
    BOOST_REQUIRE(e.beginHeldReplace(800));
    auto saved=e.snapshot();BOOST_CHECK_EQUAL(saved.layers[1].coverageFrames,5);
    for(int i=0;i<5;++i)BOOST_CHECK_EQUAL(saved.layers[1].audio[0][i],i+1);
    BOOST_CHECK_EQUAL(e.sample(0,1900),1910);
}
BOOST_AUTO_TEST_CASE(HeldReplaceUndoAndClearDoNotCorruptPendingRedoStorage)
{
    for(bool clear:{false,true}) {
        LooperEngine e;loadPattern(e,2000);e.setUndoDepth(3);
        for(int i=0;i<800;++i)process(e,float(i));
        BOOST_REQUIRE(e.beginHeldReplace(800));process(e,1000);
        if(clear)e.apply(Action::clear);else e.apply(Action::undo);
        for(int i=0;i<30;++i)process(e);
        BOOST_CHECK(!e.isClonePending());
        if(clear)e.apply(Action::undo);else e.apply(Action::redo);
        BOOST_REQUIRE_EQUAL(e.getLayerCount(),2);
        for(int i=0;i<800;++i)BOOST_CHECK_EQUAL(e.layerSample(1,0,i),i);
        BOOST_CHECK_EQUAL(e.layerSample(1,0,800),1000);
    }
}
BOOST_AUTO_TEST_CASE(HeldReplaceStoppedStartsAtPhysicalPressPhaseZero)
{
    LooperEngine e;loadPattern(e,2000);e.apply(Action::stop);
    for(int i=0;i<800;++i)process(e,i==0?9.0f:1.0f);
    BOOST_REQUIRE(e.beginHeldReplace(800));
    BOOST_CHECK_EQUAL(e.getPosition(),800);BOOST_CHECK_EQUAL(e.layerSample(1,0,0),9);
    process(e,2);BOOST_CHECK_EQUAL(e.layerSample(1,0,800),2);
}
BOOST_AUTO_TEST_CASE(HeldReplaceLinkedUsesMasterTimelineAndPreHandlePhysicalPress)
{
    LooperEngine e;loadPattern(e,2000);e.setTransitionOptions(4,0);
    e.setLinkContext(2,2000,50,0);
    for(int i=0;i<804;++i)process(e,float(i));
    BOOST_REQUIRE(e.beginHeldReplace(800));
    process(e,804);
    auto saved=e.snapshot();BOOST_REQUIRE_EQUAL(saved.layers[1].pre[0].size(),4);
    for(int i=0;i<4;++i)BOOST_CHECK_EQUAL(saved.layers[1].pre[0][i],i);
    BOOST_CHECK_EQUAL(saved.layers[1].coverageStart,54);
    BOOST_CHECK_EQUAL(saved.layers[1].audio[0][54],4);
    BOOST_CHECK_EQUAL(saved.layers[1].audio[0][854],804);
}
BOOST_AUTO_TEST_CASE(HeldHistorySurvivesStoppedHostClockUpdatesAndResetCancelsCopy)
{
    LooperEngine e;loadPattern(e,2000);
    for(int i=0;i<800;++i){e.setClock(120,4,4,0,false);process(e,float(i));}
    BOOST_REQUIRE(e.beginHeldReplace(800));BOOST_CHECK_EQUAL(e.layerSample(1,0,0),0);
    BOOST_CHECK_EQUAL(e.layerSample(1,0,799),799);
    e.resetAll();for(int i=0;i<30;++i)process(e);
    BOOST_CHECK_EQUAL(e.getLayerCount(),0);BOOST_CHECK(!e.canRedo());BOOST_CHECK(!e.isClonePending());
}
BOOST_AUTO_TEST_CASE(HeldReplaceImmediateRestartRendersUncopiedWrappedHistory)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(0,0);
    std::array<std::vector<float>,2> audio;for(auto& ch:audio)ch.assign(2000,0.25f);e.load(audio);e.apply(Action::play);
    for(int i=0;i<1600;++i)process(e,0);
    for(int i=0;i<800;++i)process(e,0.75f);
    BOOST_REQUIRE(e.beginHeldReplace(800));
    e.apply(Action::endReplace);e.apply(Action::stop);e.apply(Action::play);
    float left=0,right=0;for(int i=0;i<3;++i){left=right=0;e.process(left,right,1,false);}
    BOOST_REQUIRE(e.isClonePending()); // zero lies beyond the materialized prefix
    BOOST_CHECK_CLOSE(left,0.75f,0.001f);
}
BOOST_AUTO_TEST_CASE(FullOverdubEndBlendsAccumulatedPassInsteadOfDroppingIt)
{
    LooperEngine e; e.prepare(1000); e.setTransitionOptions(10,0);
    LooperEngine::Snapshot saved; saved.length=1000;
    LooperEngine::SavedLayer base; for(auto& c:base.audio)c.assign(1000,0);
    saved.layers.push_back(base);e.load(saved);e.apply(Action::play);
    e.apply(Action::overdub);for(int i=0;i<1200;++i)process(e,0.1f);
    e.apply(Action::overdub);for(int i=0;i<10;++i)process(e,0.1f);
    BOOST_CHECK_CLOSE(e.sample(0,199),0.2f,0.001f);
    BOOST_CHECK_CLOSE(e.sample(0,200),0.2f,0.001f);
    for(int f=201;f<=210;++f)BOOST_CHECK_LT(std::abs(e.sample(0,f)-e.sample(0,f-1)),0.02f);
    BOOST_CHECK_CLOSE(e.sample(0,210),0.1f,0.001f);
    BOOST_CHECK_CLOSE(e.layerSample(1,0,199),0.1f,0.001f);
    BOOST_CHECK_CLOSE(e.layerSample(2,0,199),0.1f,0.001f);
    BOOST_CHECK_CLOSE(e.layerSample(1,0,200),0.1f,0.001f);
}
BOOST_AUTO_TEST_CASE(SavedFullLayerEndDoesNotDuckIntoSilentPostForAddOrReplace)
{
    for(bool replace:{false,true}) for(int end:{0,200,995}) {
        LooperEngine e;e.prepare(1000);e.setTransitionOptions(10,0);
        LooperEngine::Snapshot saved;saved.length=1000;
        LooperEngine::SavedLayer layer;layer.info.replace=replace;
        layer.coverageFrames=1000;layer.captureEnd=end;
        for(auto& c:layer.audio)c.assign(1000,0.25f);
        for(auto& c:layer.post)c.assign(10,0);
        saved.layers.push_back(layer);e.load(saved);
        BOOST_CHECK_CLOSE(e.sample(0,end),0.25f,0.001f);
        for(int i=1;i<=10;++i)BOOST_CHECK_LT(std::abs(e.sample(0,(end+i)%1000)-e.sample(0,(end+i-1)%1000)),0.03f);
        BOOST_CHECK_CLOSE(e.sample(0,(end+10)%1000),0.25f,0.001f);
        auto again=e.snapshot();BOOST_CHECK_EQUAL(again.layers[0].post[0][0],0);
    }
}
BOOST_AUTO_TEST_CASE(FeedbackVolumeCanBeRestoredAndIsLockedDuringOverdub)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(0,0);
    LooperEngine::Snapshot saved;saved.length=1000;
    LooperEngine::SavedLayer layer;for(auto& c:layer.audio)c.assign(1000,0.25f);
    saved.layers.push_back(layer);e.load(saved);e.apply(Action::play);
    e.setMixOptions(0,0.25f,0,0);e.apply(Action::overdub);
    for(int i=0;i<500;++i)process(e);
    auto id=e.getLayer(0).id;auto before=e.getLayer(0);
    e.editLayer(id,2,1.5f);
    BOOST_CHECK_EQUAL(e.getLayer(0).gain,before.gain);
    BOOST_CHECK_EQUAL(e.getLayer(0).feedbackGain,before.feedbackGain);
    e.apply(Action::overdub);
    BOOST_CHECK_CLOSE(e.sample(0,0),0.125f,0.001f);
    e.editLayer(id,2,1);BOOST_CHECK_EQUAL(e.getLayer(0).feedbackGain,1);
    BOOST_CHECK_CLOSE(e.sample(0,0),0.25f,0.001f);
    e.setMixOptions(0,0,0,0);e.apply(Action::overdub);
    for(int i=0;i<1000;++i)process(e);e.apply(Action::overdub);
    BOOST_CHECK_EQUAL(e.getLayer(0).feedbackGain,0);
    e.editLayer(id,2,1);BOOST_CHECK_CLOSE(e.sample(0,0),0.25f,0.001f);
    BOOST_CHECK_EQUAL(e.layerSample(0,0,0),0.25f);
    LooperEngine restored;restored.prepare(1000);restored.load(e.snapshot());
    BOOST_CHECK_CLOSE(restored.sample(0,0),0.25f,0.001f);
}
BOOST_AUTO_TEST_CASE(LiveOverdubEndKeepsEarlierPassWhileMonitoringContinuation)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(100,0);
    LooperEngine::Snapshot saved;saved.length=1000;
    LooperEngine::SavedLayer base;for(auto& c:base.audio)c.assign(1000,0);
    saved.layers.push_back(base);e.load(saved);e.apply(Action::play);e.apply(Action::overdub);
    for(int i=0;i<1200;++i){float l=0.1f,r=l;e.process(l,r,1,true);}
    e.apply(Action::overdub);
    for(int i=0;i<10;++i){float l=0.1f,r=l;e.process(l,r,1,true);
        if(i>2)BOOST_CHECK_CLOSE(l,0.2f,0.001f);}
}
BOOST_AUTO_TEST_CASE(OneTakeSplitsWithoutEmptyPartAndUndoRestoresWholeTake)
{
    LooperEngine e;loadPattern(e,100);for(int i=0;i<30;++i)process(e);
    e.apply(Action::overdub);
    for(int i=0;i<250;++i)process(e,float(i/100+1));e.apply(Action::overdub);
    BOOST_REQUIRE_EQUAL(e.getLayerCount(),4);
    const auto take=e.getLayer(1).take;BOOST_CHECK_NE(take,0);
    for(int i=1;i<4;++i){BOOST_CHECK_EQUAL(e.getLayer(i).take,take);BOOST_CHECK_EQUAL(e.getLayer(i).part,i-1);}
    auto saved=e.snapshot();BOOST_CHECK_EQUAL(saved.layers[1].coverageStart,30);
    BOOST_CHECK_EQUAL(saved.layers[3].coverageFrames,80);
    BOOST_CHECK_EQUAL(e.layerSample(1,0,30),1);BOOST_CHECK_EQUAL(e.layerSample(2,0,30),2);
    BOOST_CHECK_EQUAL(e.layerSample(3,0,30),3);BOOST_CHECK_EQUAL(e.getLength(),100);
    e.apply(Action::undo);BOOST_CHECK_EQUAL(e.getLayerCount(),1);
    e.apply(Action::redo);BOOST_CHECK_EQUAL(e.getLayerCount(),4);
    e.apply(Action::overdub);for(int i=0;i<100;++i)process(e,4);e.apply(Action::overdub);
    BOOST_CHECK_EQUAL(e.getLayerCount(),6); // 80->0 and 0->80 are separate timeline parts
}
BOOST_AUTO_TEST_CASE(OneTakeFeedbackOptionAndIgnoreFeedbackPreserveRawAudio)
{
    for(bool sameTake:{false,true}) {
        LooperEngine e;loadPattern(e,100);e.setTakeOptions(true,sameTake);
        auto id=e.getLayer(0).id;e.editLayer(id,6,1);
        e.setMixOptions(0,0.25f,0,0);e.apply(Action::overdub);
        for(int i=0;i<200;++i)process(e,1);e.apply(Action::overdub);
        BOOST_REQUIRE_EQUAL(e.getLayerCount(),3);
        BOOST_CHECK_EQUAL(e.getLayer(0).feedbackGain,1);
        BOOST_CHECK_CLOSE(e.getLayer(1).feedbackGain,sameTake?0.25f:1.0f,0.001f);
        BOOST_CHECK_EQUAL(e.layerSample(1,0,0),1);
        auto saved=e.snapshot();LooperEngine q;q.prepare(1000);q.load(saved);
        BOOST_CHECK(q.getLayer(0).ignoreFeedback);BOOST_CHECK_EQUAL(q.getLayer(1).take,e.getLayer(1).take);
        e.editLayer(id,6,0);e.apply(Action::overdub);for(int i=0;i<100;++i)process(e);
        BOOST_CHECK_CLOSE(e.getLayer(0).feedbackGain,0.25f,0.001f);
    }
}
BOOST_AUTO_TEST_CASE(OneTakeBrokenJoinsCanFadeOrRemainRawAndPartsDeleteIndependently)
{
    LooperEngine e;loadPattern(e,100);e.setTransitionOptions(10,0);
    e.apply(Action::overdub);for(int i=0;i<250;++i)process(e,float(1+i/100));e.apply(Action::overdub);
    for(int i=0;i<20;++i)process(e,1);
    BOOST_REQUIRE_EQUAL(e.getLayerCount(),4);
    // Intact internal post is suppressed. Muting its successor enables that tail.
    e.editLayer(e.getLayer(0).id,0,0);
    const float connected=e.sample(0,5);
    e.editLayer(e.getLayer(2).id,0,0);
    const float broken=e.sample(0,5);
    e.setTakeOptions(false,true);const float noFade=e.sample(0,5);
    BOOST_CHECK_NE(broken,noFade);BOOST_CHECK_GT(connected,0);
    e.editLayer(e.getLayer(2).id,3,0);BOOST_CHECK_EQUAL(e.getLayerCount(),3);
    BOOST_CHECK_EQUAL(e.getLayer(2).part,2); // don't renumber across the missing pass
}
BOOST_AUTO_TEST_CASE(ContinuousCaptureAcrossZeroIsNotDeclickedAtTimelineWrap)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(10,0);
    LooperEngine::Snapshot s;s.length=100;LooperEngine::SavedLayer l;
    l.coverageStart=25;l.coverageFrames=100;l.captureEnd=20;
    for(auto& c:l.audio){for(int i=0;i<100;++i)c.push_back(std::sin(float(i)*0.062831853f));}
    for(auto& c:l.post)c.assign(10,0);s.layers.push_back(l);e.load(s);e.apply(Action::play);
    for(int i=0;i<100;++i)process(e);
    float left=0,right=0;e.process(left,right,1,false);
    BOOST_CHECK_SMALL(left,0.000001f); // original first sample, not repeated final sample
}
BOOST_AUTO_TEST_CASE(OneTakeGainJoinInterpolatesOverTenMilliseconds)
{
    LooperEngine e;e.prepare(1000);e.setTransitionOptions(0,0);
    LooperEngine::Snapshot s;s.length=100;
    for(int part=0;part<2;++part) {
        LooperEngine::SavedLayer l;l.info.take=7;l.info.part=part;l.info.continued=part==0;
        l.info.gain=part==0?0.25f:1.0f;l.coverageFrames=part==0?100:50;l.captureEnd=part==0?0:50;
        for(auto& c:l.audio)c.assign(100,1);s.layers.push_back(l);
    }
    e.load(s);
    for(int i=0;i<=10;++i)BOOST_CHECK_CLOSE(e.sample(0,i),0.5f+0.75f*i/10,0.001f);
    e.editLayer(e.getLayer(0).id,6,1); // immunity does not alter current gain
    BOOST_CHECK_CLOSE(e.sample(0,0),0.5f,0.001f);
}
BOOST_AUTO_TEST_CASE(OneTakeSplitUsesBoundedWritesAndStopsAtCapacity)
{
    LooperEngine e;e.configureLimits(10,7,false);loadPattern(e,100);e.setTransitionOptions(10,0);
    int writes=0;e.setWriteObserver(&writes,[](void* p,float* dst,float value)noexcept{
        ++*static_cast<int*>(p);std::atomic_ref<float>(*dst).store(value);});
    e.apply(Action::overdub);
    for(int i=0;i<900;++i){writes=0;process(e,1);BOOST_CHECK_LE(writes,70);}
    e.setWriteObserver(nullptr,nullptr);
    BOOST_CHECK_EQUAL(e.getLayerCount(),8);
    BOOST_CHECK(e.getState()==LooperEngine::State::playing);
    e.apply(Action::undo);BOOST_CHECK_EQUAL(e.getLayerCount(),1);
    e.apply(Action::redo);BOOST_CHECK_EQUAL(e.getLayerCount(),8);
}
BOOST_AUTO_TEST_CASE(UndoRedoPreserveStoppedTransportIncludingEmptyRestore)
{
    LooperEngine e;loadPattern(e,100);e.apply(Action::overdub);
    for(int i=0;i<150;++i)process(e,1);e.apply(Action::overdub);
    e.apply(Action::stop);for(int i=0;i<4;++i)process(e);e.apply(Action::undo);
    BOOST_CHECK_EQUAL(e.getLayerCount(),1);BOOST_CHECK(e.getState()==LooperEngine::State::stopped);
    float l=0,r=0;e.process(l,r,1,false);BOOST_CHECK_EQUAL(l,0);
    e.apply(Action::redo);BOOST_CHECK_EQUAL(e.getLayerCount(),3);
    BOOST_CHECK(e.getState()==LooperEngine::State::stopped);
    e.apply(Action::play);e.apply(Action::undo);BOOST_CHECK(e.getState()==LooperEngine::State::playing);
    e.apply(Action::redo);BOOST_CHECK(e.getState()==LooperEngine::State::playing);
    e.apply(Action::stop);e.apply(Action::clear);e.apply(Action::undo);
    BOOST_CHECK(e.getState()==LooperEngine::State::stopped);
    e.apply(Action::redo);BOOST_CHECK(e.getState()==LooperEngine::State::empty);
    e.apply(Action::undo);BOOST_CHECK(e.getState()==LooperEngine::State::stopped);
}
BOOST_AUTO_TEST_CASE(CapacityCountsOneTakeAsOneRecordingAndRollingForgetsPermanently)
{
    LooperEngine e;e.configureLimits(2,4,true);loadPattern(e,100);
    e.apply(Action::overdub);
    for(int i=0;i<700;++i)process(e,float(i/100+1));e.apply(Action::overdub);
    BOOST_REQUIRE_EQUAL(e.getLayerCount(),5);BOOST_CHECK_EQUAL(e.recordingCount(),2);
    for(int i=1;i<=4;++i) {
        BOOST_CHECK_EQUAL(e.getLayer(i).part,i+2);
        BOOST_CHECK_EQUAL(e.layerSample(i,0,0),float(i+3));
    }
    const auto take=e.getLayer(1).take;e.apply(Action::overdub);
    BOOST_CHECK(e.getState()==LooperEngine::State::playing); // recording limit reached
    e.apply(Action::undo);BOOST_CHECK_EQUAL(e.getLayerCount(),1);
    e.apply(Action::redo);BOOST_REQUIRE_EQUAL(e.getLayerCount(),5);
    BOOST_CHECK_EQUAL(e.getLayer(1).take,take);BOOST_CHECK_EQUAL(e.getLayer(1).part,3);
    BOOST_CHECK_EQUAL(e.layerSample(0,0,0),10); // unrelated recording retained
}
BOOST_AUTO_TEST_CASE(CapacityStopAndLoweringLimitsNeverDeleteExistingAudio)
{
    LooperEngine e;e.configureLimits(10,4,false);loadPattern(e,100);
    e.apply(Action::overdub);for(int i=0;i<400;++i)process(e,1);
    BOOST_CHECK(e.getState()==LooperEngine::State::playing);
    BOOST_CHECK_EQUAL(e.getLayerCount(),5);BOOST_CHECK_EQUAL(e.recordingCount(),2);
    e.apply(Action::stop);e.setLimits(1,2,false);
    BOOST_CHECK_EQUAL(e.getLayerCount(),5);e.apply(Action::overdub);
    BOOST_CHECK(e.getState()==LooperEngine::State::stopped);
    e.setLimits(3,2,false);e.apply(Action::overdub);
    for(int i=0;i<200;++i)process(e,2);
    BOOST_CHECK_EQUAL(e.recordingCount(),3);BOOST_CHECK_EQUAL(e.getLayerCount(),7);
}
BOOST_AUTO_TEST_CASE(RollingDoesNotAddForgottenAudioOrAllocateInCallback)
{
    LooperEngine e;e.configureLimits(10,4,true);e.prepare(1000);e.setTransitionOptions(0,0);
    LooperEngine::Snapshot s;s.length=100;LooperEngine::SavedLayer l;
    for(auto& c:l.audio)c.assign(100,0);s.layers.push_back(l);e.load(s);e.apply(Action::play);e.apply(Action::overdub);
    int writes=0;e.setWriteObserver(&writes,[](void* p,float* dst,float v)noexcept{++*static_cast<int*>(p);std::atomic_ref<float>(*dst).store(v);});
    for(int i=0;i<700;++i) {
        writes=0;float a=1,b=1;e.process(a,b,1,true);
        BOOST_CHECK_LE(writes,70);
        if(i>=400)BOOST_CHECK_CLOSE(a,4,0.001f);
    }
    e.setWriteObserver(nullptr,nullptr);
    BOOST_CHECK_EQUAL(e.getLayerCount(),5);
}
BOOST_AUTO_TEST_CASE(RollingSinglePassAndIgnoreFeedbackStillForgetsOldest)
{
    LooperEngine e;e.configureLimits(10,1,true);loadPattern(e,100);
    e.apply(Action::overdub);for(int i=0;i<50;++i)process(e,1);
    const auto forgotten=e.getLayer(1).id;e.editLayer(forgotten,6,1);
    for(int i=0;i<250;++i)process(e,2);e.apply(Action::overdub);
    BOOST_REQUIRE_EQUAL(e.getLayerCount(),2);BOOST_CHECK_NE(e.getLayer(1).id,forgotten);
    BOOST_CHECK_EQUAL(e.getLayer(1).part,2);
    e.apply(Action::stop);e.apply(Action::undo);e.apply(Action::redo);
    BOOST_CHECK_EQUAL(e.getLayer(1).part,2);
}

BOOST_AUTO_TEST_CASE(FoldedFullTakeUsesOneSeamBlendAndPreservesConnectedOutgoingAudio)
{
    LooperEngine::Snapshot saved;saved.rate=1000;saved.length=1000;
    const int seam=700;
    for(int part=0;part<4;++part){
        LooperEngine::SavedLayer l;l.info.take=5;l.info.part=57+part;l.info.continued=part<3;
        l.info.gain=0.3f+0.1f*part;l.coverageStart=seam;l.coverageFrames=part<3?1000:400;
        l.captureEnd=(seam+l.coverageFrames)%1000;
        for(int c=0;c<2;++c){l.audio[c].resize(1000);
            for(int f=0;f<l.coverageFrames;++f)l.audio[c][(seam+f)%1000]=(part+1)*0.1f+f*0.0001f;
            for(int f=0;f<10;++f){l.pre[c].push_back((part+1)*0.1f+(f-10)*0.0001f);
                l.post[c].push_back((part+1)*0.1f+(l.coverageFrames+f)*0.0001f);}}
        saved.layers.push_back(std::move(l));
    }
    for(int mask:{1,2,4,15,7,5}){
        for(int i=0;i<4;++i)saved.layers[i].info.enabled=(mask&(1<<i))!=0;
        LooperEngine e;e.prepare(1000);e.setTransitionOptions(10,0);e.load(saved);
        const float jump=std::abs(e.sample(0,seam)-e.sample(0,seam-1));
        BOOST_CHECK_MESSAGE(jump<0.001f,"mask "<<mask<<" seam jump "<<jump);
    }
}
BOOST_AUTO_TEST_CASE(MemoryResizeReleasesStorageAndPreservesRolledTakeUndoRedo)
{
    LooperEngine e;e.configureLimits(10,4,true);loadPattern(e,100);
    e.apply(Action::overdub);for(int i=0;i<700;++i)process(e,float(i/100+1));
    e.apply(Action::stop);process(e);
    auto before=e.snapshot();const auto bytes=e.memoryBytes();
    BOOST_REQUIRE(e.resizeMemory(1,2,2,true,1));
    BOOST_CHECK_LT(e.memoryBytes(),bytes/5);BOOST_CHECK_EQUAL(e.getLayerCount(),5);
    auto after=e.snapshot();
    for(size_t i=0;i<before.layers.size();++i){BOOST_CHECK(before.layers[i].audio==after.layers[i].audio);
        BOOST_CHECK_EQUAL(before.layers[i].info.id,after.layers[i].info.id);}
    e.apply(Action::undo);BOOST_CHECK_EQUAL(e.getLayerCount(),1);
    e.apply(Action::redo);BOOST_CHECK_EQUAL(e.getLayerCount(),5);
    after=e.snapshot();for(size_t i=0;i<before.layers.size();++i)BOOST_CHECK(before.layers[i].audio==after.layers[i].audio);
    BOOST_CHECK(e.getState()==LooperEngine::State::stopped);
}
BOOST_AUTO_TEST_CASE(MemoryResizeRejectsTruncatingUndoOrLinkedLengthWithoutMutation)
{
    LooperEngine e;e.configureLimits(1,1,false);e.prepare(100);e.setTransitionOptions(0,0);
    e.apply(Action::record);for(int i=0;i<400;++i)process(e,0.1f);e.apply(Action::stop);
    e.apply(Action::freshRecord);for(int i=0;i<100;++i)process(e,0.2f);e.apply(Action::stop);
    const auto bytes=e.memoryBytes();BOOST_CHECK(!e.resizeMemory(2,1,1,false,1));
    BOOST_CHECK_EQUAL(e.memoryBytes(),bytes);BOOST_CHECK_EQUAL(e.getLength(),100);
    BOOST_REQUIRE(e.resizeMemory(4,1,1,false,1));e.apply(Action::undo);
    BOOST_CHECK_EQUAL(e.getLength(),400);BOOST_CHECK_CLOSE(e.sample(0,10),0.1f,0.001f);
    e.apply(Action::redo);BOOST_CHECK_EQUAL(e.getLength(),100);BOOST_CHECK_CLOSE(e.sample(0,10),0.2f,0.001f);
}

BOOST_AUTO_TEST_CASE(FixedBarsRecordStartsOnBarAndStopsAtExactLength)
{
    LooperEngine e;e.prepare(100);e.setOptions(true,100,0);e.setFixedBars(2);e.setTransitionOptions(0,0);
    e.setClock(60,3,4,1.0,true);e.apply(Action::record);BOOST_CHECK(e.isStartPending());
    for(int i=0;i<200;++i)process(e,0.25f);BOOST_CHECK_EQUAL(e.getLength(),0);
    for(int i=0;i<599;++i)process(e,0.25f);BOOST_CHECK(e.getState()==LooperEngine::State::recording);
    process(e,0.25f);BOOST_CHECK_EQUAL(e.getLength(),600);BOOST_CHECK(e.getState()==LooperEngine::State::playing);
    BOOST_CHECK_EQUAL(e.getLayerCount(),1);BOOST_CHECK_CLOSE(e.sample(0,599),0.25f,0.001f);
}
BOOST_AUTO_TEST_CASE(FixedBarsOverdubContinuesOneTakeAndRespectsPassLimit)
{
    for(bool rolling:{false,true}) {
        LooperEngine e;e.configureLimits(1,2,rolling);e.prepare(100);e.setOptions(true,100,0);
        e.setFixedBars(1);e.setTransitionOptions(0,0);e.setClock(60,1,4,0,true);
        e.apply(Action::overdub);for(int i=0;i<200;++i)process(e,1);
        BOOST_CHECK_EQUAL(e.getLength(),100);BOOST_CHECK_EQUAL(e.getLayerCount(),2);
        BOOST_CHECK_EQUAL(e.recordingCount(),1);BOOST_CHECK_EQUAL(e.getLayer(0).take,e.getLayer(1).take);
        BOOST_CHECK(e.getState()==(rolling?LooperEngine::State::overdubbing:LooperEngine::State::playing));
        if(rolling){for(int i=0;i<100;++i)process(e,2);e.apply(Action::stop);BOOST_CHECK_EQUAL(e.getLayer(0).part,1);}
        else e.apply(Action::stop);
        e.apply(Action::undo);BOOST_CHECK_EQUAL(e.getLayerCount(),0);
        e.apply(Action::redo);BOOST_CHECK_EQUAL(e.getLayerCount(),2);
    }
}
BOOST_AUTO_TEST_CASE(FixedBarsEarlyEndLeavesSilenceAndExistingLengthIsPreserved)
{
    LooperEngine e;e.prepare(100);e.setOptions(true,100,0);e.setFixedBars(2);e.setTransitionOptions(0,0);
    e.setClock(60,1,4,0,true);e.apply(Action::record);for(int i=0;i<30;++i)process(e,1);
    e.apply(Action::record);BOOST_CHECK_EQUAL(e.getLength(),200);BOOST_CHECK_EQUAL(e.getPosition(),30);
    BOOST_CHECK_EQUAL(e.layerSample(0,0,29),1);BOOST_CHECK_EQUAL(e.layerSample(0,0,30),0);
    e.apply(Action::stop);e.setFixedBars(5);e.setRecordNextBar(false);e.apply(Action::record);
    for(int i=0;i<200;++i)process(e,2);
    BOOST_CHECK_EQUAL(e.getLength(),200);BOOST_CHECK(e.getState()==LooperEngine::State::playing);
    BOOST_CHECK_EQUAL(e.getLayerCount(),3);
}
BOOST_AUTO_TEST_CASE(FixedBarsRejectsMemoryAndMasterConflictsBeforeCapture)
{
    LooperEngine e;e.configureDuration(1);e.prepare(100);e.setOptions(true,100,0);
    e.setFixedBars(2);e.setClock(60,1,4,0,true);e.apply(Action::record);
    BOOST_CHECK(!e.isStartPending());BOOST_CHECK_EQUAL(e.getLayerCount(),0);
    e.configureDuration(60);e.prepare(100);e.setOptions(true,100,0);e.setFixedBars(3);
    e.setClock(60,1,4,0,true);e.setLinkContext(2,200,0,0);e.apply(Action::record);
    BOOST_CHECK(!e.isStartPending());
    e.setLinkContext(3,200,0,0);e.apply(Action::record);BOOST_CHECK(!e.isStartPending());
    e.setFixedBars(4);e.apply(Action::record);BOOST_CHECK(e.isStartPending());
    for(int i=0;i<400;++i)process(e,1);BOOST_CHECK_EQUAL(e.getLength(),400);BOOST_CHECK_EQUAL(e.getFixedLength(),400);
}
BOOST_AUTO_TEST_CASE(OverdubStartChoiceIsIndependentAndWaitingCanCancel)
{
    LooperEngine e;loadPattern(e,100);e.setOptions(true,100,0);e.setRecordNextBar(false);
    e.setOverdubNextBar(true);e.setClock(60,1,4,0.5,true);
    e.apply(Action::overdub);BOOST_CHECK(e.isStartPending());BOOST_CHECK(e.getState()==LooperEngine::State::playing);
    e.apply(Action::overdub);BOOST_CHECK(!e.isStartPending());BOOST_CHECK_EQUAL(e.getLayerCount(),1);
    e.apply(Action::overdub);for(int i=0;i<500;++i)process(e);BOOST_CHECK_EQUAL(e.getLayerCount(),1);
    process(e,2);BOOST_CHECK(e.getState()==LooperEngine::State::overdubbing);BOOST_CHECK_EQUAL(e.getLayerCount(),2);
    e.apply(Action::stop);e.setOverdubNextBar(false);e.setRecordNextBar(true);e.apply(Action::overdub);
    BOOST_CHECK(!e.isStartPending());BOOST_CHECK(e.getState()==LooperEngine::State::overdubbing);
}

BOOST_AUTO_TEST_CASE(FixedBarsLinkedCaptureKeepsMasterPhaseAndContinuesAtThatPhase)
{
    LooperEngine e;e.prepare(100);e.setOptions(true,100,0);e.setFixedBars(4);e.setTransitionOptions(0,0);
    e.setClock(60,1,4,0.5,true);e.setLinkContext(2,400,50,0);e.apply(Action::overdub);
    for(int i=0;i<50;++i)process(e,0);
    for(int i=0;i<400;++i)process(e,float(i+1));
    BOOST_CHECK_EQUAL(e.getLength(),400);BOOST_CHECK_EQUAL(e.getFixedLength(),400);
    BOOST_CHECK_EQUAL(e.layerSample(0,0,100),1);BOOST_CHECK_EQUAL(e.layerSample(0,0,99),400);
    BOOST_CHECK_EQUAL(e.getPosition(),100);BOOST_CHECK(e.getState()==LooperEngine::State::overdubbing);
    process(e,5);BOOST_REQUIRE_EQUAL(e.getLayerCount(),2);
    BOOST_CHECK_EQUAL(e.snapshot().layers[1].coverageStart,100);
}
BOOST_AUTO_TEST_CASE(EmptySyncOverdubImmediateRecoversCurrentBarAndClockStopCancelsWait)
{
    LooperEngine e;e.prepare(100);e.setOptions(true,100,0);e.setTransitionOptions(0,0);
    e.setClock(60,1,4,0,true);for(int i=0;i<50;++i)process(e,0.3f);
    e.setOverdubNextBar(false);e.apply(Action::overdub);BOOST_CHECK(!e.isStartPending());
    BOOST_CHECK_EQUAL(e.getLength(),50);process(e,0.7f);BOOST_CHECK_CLOSE(e.layerSample(0,0,50),0.7f,0.001f);
    e.apply(Action::clear);e.setOverdubNextBar(true);e.apply(Action::overdub);BOOST_CHECK(e.isStartPending());
    e.setClock(60,1,4,0,false);BOOST_CHECK(!e.isStartPending());BOOST_CHECK_EQUAL(e.getLayerCount(),0);
}
BOOST_AUTO_TEST_CASE(FixedCaptureSnapshotRetainsPlannedLengthAndSilentRemainder)
{
    LooperEngine e;e.prepare(100);e.setOptions(true,100,0);e.setFixedBars(4);e.setTransitionOptions(0,0);
    e.setClock(60,1,4,0,true);e.apply(Action::record);for(int i=0;i<30;++i)process(e,1);
    auto saved=e.snapshot();BOOST_CHECK_EQUAL(saved.length,400);BOOST_CHECK_EQUAL(saved.layers[0].audio[0].size(),400);
    BOOST_CHECK_EQUAL(saved.layers[0].coverageFrames,30);BOOST_CHECK_EQUAL(saved.layers[0].audio[0][31],0);
    BOOST_CHECK_EQUAL(e.snapshotView().length,400);BOOST_CHECK_EQUAL(e.getLength(),30);
}
BOOST_AUTO_TEST_SUITE_END()
