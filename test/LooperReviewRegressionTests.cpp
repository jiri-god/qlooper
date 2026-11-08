// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include <boost/test/unit_test.hpp>
#include "nodes/looperengine.hpp"
using E = element::LooperEngine;
using A = E::Action;
namespace {
void tick(E& e, int n, float l=.25f, float r=.5f) {
    while(n--) { float a=l,b=r;e.process(a,b,1); }
}
void setup(E& e) {
    e.configureDuration(2);e.configureLimits(3,4,true);e.prepare(1000);
    e.setTransitionOptions(0,0);e.setOptions(false,0,0);
}
void same(E& a,E& b) {
    BOOST_CHECK(a.getState()==b.getState());
    BOOST_CHECK_EQUAL(a.getLength(),b.getLength());
    BOOST_CHECK_EQUAL(a.getCapturedProgress(),b.getCapturedProgress());
    const auto x=a.snapshot(),y=b.snapshot();
    BOOST_REQUIRE_EQUAL(x.layers.size(),y.layers.size());
    for(size_t i=0;i<x.layers.size();++i) {
        BOOST_CHECK(x.layers[i].audio==y.layers[i].audio);
        BOOST_CHECK_EQUAL(x.layers[i].info.take,y.layers[i].info.take);
        BOOST_CHECK_EQUAL(x.layers[i].info.part,y.layers[i].info.part);
        BOOST_CHECK_EQUAL(x.layers[i].info.continued,y.layers[i].info.continued);
    }
}
}
BOOST_AUTO_TEST_SUITE(LooperReviewRegressionTests)
BOOST_AUTO_TEST_CASE(UnavailableRedoIsNoOpForEveryCapturePath)
{
    for(int mode=0;mode<7;++mode) {
        E actual,control; setup(actual);setup(control);
        for(auto* e:{&actual,&control}) {
            if(mode>=4 && mode<=5) e->setLinkContext(2,mode==4?1000:2000,0,0);
            if(mode==6) {e->setOptions(true,0,0);e->setClock(600,4,4,0,true);e->setFixedBars(1);}
            if(mode>=1 && mode<=3) {
                e->apply(A::record);tick(*e,100);e->apply(A::play);
            }
            e->apply(mode==1?A::overdub:mode==3?A::replace:A::record);
            tick(*e,10);
        }
        BOOST_REQUIRE(!actual.canRedo());
        actual.applyPerformance(A::redo);same(actual,control);
        actual.apply(A::redo);same(actual,control);
        tick(actual,101);tick(control,101);same(actual,control);
        actual.apply(A::record);control.apply(A::record);
        tick(actual,2100);tick(control,2100);same(actual,control);
        BOOST_CHECK_LE(actual.getLength(),2000);
    }
}
BOOST_AUTO_TEST_CASE(UnavailableHistoryDoesNotCancelSoundPreparation)
{
    for(bool sync:{false,true}) for(auto action:{A::undo,A::redo}) {
        E e;setup(e);e.setOptions(sync,0,0);e.setClock(600,4,4,0,true);e.setPerformanceOptions(true,false,false,true,true,2,.1f);
        e.applyPerformance(A::record);BOOST_REQUIRE(e.isStartPending());
        e.applyPerformance(action);BOOST_CHECK(e.isStartPending());
        e.apply(action);BOOST_CHECK(e.isStartPending());
        tick(e,sync?1201:1,.5f,.7f);BOOST_CHECK(e.getState()==E::State::recording);
    }
}
BOOST_AUTO_TEST_CASE(LinkedSoundAttackPreservesLengthPhaseAndStereo)
{
    for(int mode:{2,3}) for(int history:{0,2,20}) for(int phase:{0,2,400,998}) for(auto action:{A::record,A::overdub}) {
        E e;setup(e);e.setClock(120,4,4,0,true);
        tick(e,history,.01f,.02f);
        e.setLinkContext(mode,1000,phase,0);
        e.setPerformanceOptions(true,false,false,true,true,2,.1f);
        e.applyPerformance(action);BOOST_REQUIRE(e.waitingForSound());
        tick(e,1,.5f,.7f);
        const int n=std::min(history,5);
        if(mode==2) BOOST_CHECK_EQUAL(e.getLength(),1000);
        BOOST_CHECK_EQUAL(e.getCapturedProgress(),n+1);
        const auto snapshot=e.snapshot();BOOST_REQUIRE_EQUAL(snapshot.layers.size(),1);
        const int triggerIndex=mode==3 && phase<n ? phase+1000 : phase;
        BOOST_CHECK_EQUAL(snapshot.layers[0].audio[0][triggerIndex],.5f);
        BOOST_CHECK_EQUAL(snapshot.layers[0].audio[1][triggerIndex],.7f);
        for(int i=1;i<=n;++i) {
            const int index=mode==2?(phase-i+1000)%1000:triggerIndex-i;
            BOOST_CHECK_EQUAL(snapshot.layers[0].audio[0][index],.01f);
            BOOST_CHECK_EQUAL(snapshot.layers[0].audio[1][index],.02f);
        }
        e.applyPerformance(action);
        tick(e,2000);
        BOOST_CHECK(e.getLength()>0);BOOST_CHECK_EQUAL(e.getLength()%1000,0);
        BOOST_CHECK_LE(e.getLength(),2000);
    }
}
BOOST_AUTO_TEST_CASE(LinkedAttackAtCapacityAndLockedMaster)
{
    for(int mode:{1,2,3}) {
        E e;setup(e);
        if(mode==1) { // Establish and retain the master's fixed length.
            e.setLinkContext(1,0,0,0);e.apply(A::record);tick(e,2000);e.apply(A::play);
            BOOST_REQUIRE_EQUAL(e.getFixedLength(),2000);
            e.apply(A::clear);
        }
        tick(e,20,.01f,.02f);e.setLinkContext(mode,2000,2,0);
        e.setPerformanceOptions(true,false,false,true,true,2,.1f);e.applyPerformance(A::record);
        BOOST_REQUIRE(e.waitingForSound());
        tick(e,1,.5f,.7f);
        if(mode!=3)BOOST_CHECK_EQUAL(e.getLength(),2000);
        tick(e,2099,.5f,.7f);e.finalizeOffline();
        BOOST_CHECK(e.getLength()>0);BOOST_CHECK_LE(e.getLength(),2000);
    }
}
BOOST_AUTO_TEST_CASE(FirstFreeMasterSoundRecoveryAnchorsCapturedSampleZero)
{
    for(int rate:{1000,44100,48000,192000}) for(int history:{0,2,1200}) for(bool automatic:{false,true}) for(int mode:{0,1}) {
        E e;e.configureDuration(1);e.configureLimits(1,1,true);e.prepare(rate);
        e.setTransitionOptions(0,0);e.setOptions(false,0,0);e.setClock(120,4,4,0,true);e.setLinkContext(mode,0,0,0);
        e.setPerformanceOptions(true,false,false,true,false,1,.5f);
        e.applyPerformance(A::record);tick(e,history,.1f,.1f);
        tick(e,1,1,1);
        const int prefix=std::min(history,int(rate*.005));
        if(mode==1)BOOST_CHECK_EQUAL(e.getOrigin(),history-prefix);
        if(automatic)tick(e,rate-prefix-1,1,1);
        else {tick(e,100,1,1);e.apply(A::play);}
        BOOST_REQUIRE(e.getState()==E::State::playing);
        BOOST_CHECK_EQUAL(e.getSyncPosition(),0);
        tick(e,1,0,0);BOOST_CHECK_EQUAL(e.getPosition(),1);
        if(mode==1) {
        E linked;linked.configureDuration(1);linked.configureLimits(1,1,true);linked.prepare(rate);
        linked.setOptions(false,0,0);linked.setLinkContext(2,e.getLength(),e.getLinkFrame(),e.getOrigin());
        linked.apply(A::record);
        BOOST_CHECK_EQUAL(linked.getSyncPosition(),e.getSyncPosition());
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
