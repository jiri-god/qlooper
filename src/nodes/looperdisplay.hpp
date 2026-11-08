// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "looper.hpp"

namespace element {
struct LooperDisplayStatus {
    juce::String main, detail, flags;
    bool countdown=false;
};
inline LooperDisplayStatus looperDisplayStatus(const LooperProcessor::View& v,
    bool online,bool muted,bool bypass,float threshold,const juce::String& notice={})
{
    LooperDisplayStatus d;
    d.flags=muted?"MUTE":"";
    if(bypass)d.flags+=(d.flags.isEmpty()?"":"  ")+juce::String("BYPASS");
    const juce::String mode=v.loopMode==0?"Free":v.loopMode==2?"Fixed":"Sync";
    d.detail=mode;
    if(v.loopMode!=0 && v.barFrames>0 && v.length>0)
        d.detail+=" · "+juce::String(double(v.length)/v.barFrames, v.length%v.barFrames==0?0:1)+" bars";
    const bool recording=v.state==1 || v.state==4 || v.state==5;
    if(!online){d.main="Audio offline";return d;}
    if(recording) {
        d.main="Recording";
        d.detail=v.pending?"Finishing at next bar":v.state==5?"Replace":v.overwrite?"Overwrite":"Add";
        if(v.controller>=2)d.detail="Custom "+juce::String(v.controller-1)+" · "+d.detail;
    } else if(v.armed) {
        if(v.transportWaiting){d.main="Waiting for transport";}
        else if(v.soundWaiting){d.main="Waiting for sound";d.detail=juce::String(threshold,0)+" dB";}
        else if(v.waitingAction==int(LooperEngine::Action::play)+1){d.main="Play in "+juce::String(juce::jmax(1,v.countIn));d.detail="Next bar";}
        else if(v.waitingAction==int(LooperEngine::Action::stop)+1){d.main="Stop in "+juce::String(juce::jmax(1,v.countIn));d.detail="Next bar";}
        else {d.main=juce::String(juce::jmax(1,v.countIn));d.detail="Bars to recording";d.countdown=true;}
    } else {
        d.main=v.state==2?"Playing":v.count>0?"Stopped":"Empty";
        if(v.linkMode>1 && v.linkBase<=0)d.detail="Waiting for master";
        else if(!v.fixedCompatible)d.detail="Check fixed length";
        else if(v.needsClock)d.detail=mode+" · Transport stopped";
        if(notice.isNotEmpty())d.detail=notice;
    }
    return d;
}
// Message-thread summary: pedal selection is deliberately not an input.
struct ManagerDisplayMember {
    juce::String name;
    LooperDisplayStatus status;
    bool online=true, content=false, playing=false, recording=false, waiting=false;
    bool muted=false, bypass=false, silent=false, solo=false;
};
inline LooperDisplayStatus managerDisplayStatus(const std::vector<ManagerDisplayMember>& members,
    bool playPending, const juce::String& switchTarget, int switchBars, bool switchNeedsClock,
    const juce::String& switchAction, float volume)
{
    LooperDisplayStatus d;
    if(members.empty()){d.main="No loopers";d.detail="Connect MIDI to loopers";return d;}
    int playing=0, content=0, muted=0, silent=0;
    bool online=false, solo=false;
    const ManagerDisplayMember* waiting=nullptr;
    const ManagerDisplayMember* recording=nullptr;
    for(const auto& m:members) {
        online|=m.online;solo|=m.solo;content+=m.content;
        if(m.playing){++playing;muted+=m.muted;silent+=m.silent || m.muted || m.bypass;}
        if(m.waiting && !waiting)waiting=&m;
        if(m.recording && !recording)recording=&m;
    }
    if(solo)d.flags="SOLO";
    if(!online){d.main="Audio offline";return d;}
    if(switchNeedsClock) {
        d.main="Waiting for clock";d.detail="Switch needs running transport";
    } else if(switchTarget.isNotEmpty()) {
        d.main=switchNeedsClock?"Waiting for clock":switchBars>0?juce::String(switchBars):"Switch pending";
        d.countdown=!switchNeedsClock && switchBars>0;
        d.detail=switchTarget+" · "+switchAction;
    } else if(playPending) {
        // Group launch always waits for the next bar, never a multi-bar count-in.
        d.main="1";d.countdown=true;d.detail="Bar to Play all";
    } else if(waiting || recording) {
        const auto& m=*(waiting?waiting:recording);
        const auto flags=d.flags;
        d=m.status;
        d.flags=flags;
        d.detail=m.name+" · "+d.detail;
        if(!m.online){d.main="Audio offline";d.countdown=false;}
    } else {
        d.main=playing>0?"Playing":content>0?"Stopped":"No loops";
        d.detail=playing>0?juce::String(playing)+" of "+juce::String(members.size())+" loopers":
            content>0?juce::String(content)+" loopers with recordings":"Ready to record";
    }
    const auto badge=[&](const juce::String& text){d.flags+=(d.flags.isEmpty()?"":"  · ")+text;};
    if(volume<=0)badge("Volume 0");
    else if(playing>0 && muted==playing)badge("All loops muted");
    else if(playing>0 && silent==playing)badge("Playback silenced");
    return d;
}

}
