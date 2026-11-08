// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/boundedmidi.hpp"
#include "nodes/loopermanager.hpp"
#include "engine/graphnode.hpp"
#include <element/midipipe.hpp>
#include <element/node.h>
#include <limits>

namespace element {
using namespace juce;
LooperManagerNode::LooperManagerNode() : Processor (0) { setName ("Looper Manager"); }
void LooperManagerNode::refreshPorts()
{
    if (getNumPorts()) return;
    PortList ports;
    ports.add (PortType::Midi, 0, 0, "midi_in", "Pedals", true);
    ports.add (PortType::Midi, 1, 0, "midi_out", "Loopers", false);
    setPorts (ports);
}
void LooperManagerNode::cancelPendingForSection(int offset, bool start) noexcept
{
    if(sectionEventCount>=int(sectionEvents.size())) return;
    const SectionEvent event {std::max(0,offset),start};
    int index=sectionEventCount++;
    while(index>0 && sectionEvents[index-1].offset>event.offset) {sectionEvents[index]=sectionEvents[index-1];--index;}
    sectionEvents[index]=event;
}
void LooperManagerNode::selectForSection(uint32 id, int offset,int profile) noexcept
{
    if(sectionEventCount>=int(sectionEvents.size())) return;
    const SectionEvent event {std::max(0,offset),false,id,profile};
    int index=sectionEventCount++;
    while(index>0 && sectionEvents[index-1].offset>event.offset) {sectionEvents[index]=sectionEvents[index-1];--index;}
    sectionEvents[index]=event;
}
void LooperManagerNode::prepareToRender (double rate, int)
{
    for(auto& t:targets){t.control->uiHeld=false;t.control->midiHeld=false;t.control->playingHeld=false;}
    outgoing.ensureSize (65536);
    held = {}; gestureTargets={}; gestureCommands={}; navigation={}; stopHold={}; cancelHandoff();
    if (renderRate > 0 && rate != renderRate) sharedFrame = int64_t(std::llround(sharedFrame * rate / renderRate));
    renderRate = rate; timelineValid = hadClock = false;
    playPending.store (false); playNeedsClock.store (false);
}
void LooperManagerNode::getPluginDescription (PluginDescription& d) const
{
    d.name = d.descriptiveName = "Looper Manager";
    d.fileOrIdentifier = EL_NODE_ID_LOOPER_MANAGER;
    d.uniqueId = 0x4c4d4731;
    d.pluginFormatName = "Element";
    d.manufacturerName = "Jgod";
    d.category = "MIDI";
    d.version = "2.0.0";
    d.numInputChannels = d.numOutputChannels = 0;
    d.isInstrument = false;
}
LooperManagerNode::MemberControl* LooperManagerNode::controlFor(uint32 id) const noexcept
{
    for(const auto& t:targets)if(t.node->nodeId==id)return t.control.get();
    return nullptr;
}
bool LooperManagerNode::requestBox(uint32 id,BoxAction action) noexcept
{
    if(auto* c=controlFor(id)){int empty=-1;return c->request.compare_exchange_strong(empty,int(action));}
    return false;
}
bool LooperManagerNode::playToEnd(uint32 id) const noexcept {auto* c=controlFor(id);return c&&c->playToEnd.load();}
void LooperManagerNode::setPlayToEnd(uint32 id,bool value) noexcept {if(auto* c=controlFor(id))c->playToEnd=value;}
bool LooperManagerNode::momentary(uint32 id) const noexcept {auto* c=controlFor(id);return c&&c->momentary.load();}
void LooperManagerNode::setMomentary(uint32 id,bool value) noexcept {if(isLocked())return;if(auto* c=controlFor(id)){c->uiHeld=false;c->momentary=value;}}
void LooperManagerNode::holdBox(uint32 id,bool down) noexcept {if(auto* c=controlFor(id))c->uiHeld=down;}
int LooperManagerNode::recordingProfile(uint32 id) const noexcept
{if(auto* c=controlFor(id))return c->profile.load();return 1;}
bool LooperManagerNode::setRecordingProfile(uint32 id,int profile) noexcept
{
    if(profile<0 || profile>3 || handoffTarget.load()==id)return false;
    for(const auto& t:targets)if(t.node->nodeId==id && !t.looper->recordingSettingsLocked() && t.control->request.load()!=int(BoxAction::record)){
        t.control->profile=profile;return true;
    }
    return false;
}
bool LooperManagerNode::soloed(uint32 id) const noexcept
{if(auto* c=controlFor(id))return c->solo.load();return false;}
LooperProcessor::MidiMapping LooperManagerNode::getBoxMapping(uint32 id,int action) const noexcept
{
    if(action>=0 && action<boxActions)if(auto* c=controlFor(id)){
        const int packed=c->midi[size_t(action)].load();return {(packed&255)-1,(packed>>8)&31};
    }
    return {};
}
bool LooperManagerNode::setBoxMapping(uint32 id,int action,LooperProcessor::MidiMapping mapping) noexcept
{
    if(action<0 || action>=boxActions || mapping.cc<-1 || mapping.cc>127 || mapping.channel<0 || mapping.channel>16)return false;
    if(mapping.cc>=0) {
        const auto clashes=[&](int cc,int ch){return cc==mapping.cc && (ch==0 || mapping.channel==0 || ch==mapping.channel);};
        for(auto* global:{&previousCC,&nextCC,&stopAllCC,&playAllCC})if(clashes(global->load(),channel.load()))return false;
        for(const auto& t:targets) {
            for(int i=0;i<boxActions;++i)if(t.node->nodeId!=id || i!=action){const int packed=t.control->midi[size_t(i)].load();if(clashes((packed&255)-1,(packed>>8)&31))return false;}
            for(int i=0;i<LooperProcessor::midiActions;++i){const auto m=t.looper->getMidiMapping(i);if(clashes(m.cc,m.channel))return false;}
        }
    }
    if(auto* c=controlFor(id)){c->midi[size_t(action)]=(mapping.cc+1)|(mapping.channel<<8);return true;}
    return false;
}
bool LooperManagerNode::setGlobalMapping(int action,int cc,int midiChannel) noexcept
{
    if(action<1 || action>4 || cc<-1 || cc>127 || midiChannel<0 || midiChannel>16)return false;
    std::array<std::atomic<int>*,4> globals{&previousCC,&nextCC,&stopAllCC,&playAllCC};
    for(int i=0;i<4;++i) {
        const int candidate=i==action-1?cc:globals[size_t(i)]->load();if(candidate<0)continue;
        for(int j=0;j<i;++j)if(candidate==(j==action-1?cc:globals[size_t(j)]->load()))return false;
        for(const auto& t:targets)for(int j=0;j<boxActions;++j){
            const int packed=t.control->midi[size_t(j)].load(),ch=(packed>>8)&31;
            if((packed&255)==candidate+1 && (midiChannel==0 || ch==0 || ch==midiChannel))return false;
        }
    }
    globals[size_t(action-1)]->store(cc);channel.store(midiChannel);return true;
}
void LooperManagerNode::updateSoloMute() noexcept
{
    bool any=false;
    for(const auto& t:targets){if(t.looper->getMuted())t.control->solo=false;any|=t.control->solo.load();}
    for(const auto& t:targets)t.looper->managerSoloMuted=any && !t.control->solo.load();
}
void LooperManagerNode::configure (GraphNode& graph)
{
    cancelHandoff();
    targets.clear();
    for (int i = 0; i < graph.getNumConnections(); ++i)
    {
        const auto* c = graph.getConnection (i);
        if (c->sourceNode != nodeId || c->sourcePort != 1) continue;
        auto* n = graph.getNodeForId (c->destNode);
        auto* p = n ? dynamic_cast<LooperProcessor*> (n->getAudioProcessor()) : nullptr;
        if (!p || p->managerOwner.load() != 0) continue;
        p->managerOwner.store (nodeId);
        p->managerTarget.store (n->nodeId);
        p->managerCaptureGate=captureGate;
        auto& controls=memberControls[n->nodeId];if(!controls)controls=std::make_shared<MemberControl>();
        targets.push_back ({ n, p, false, syncMode(n->nodeId), controls });
    }
    const auto rank = [this] (uint32 id) {
        auto it = std::find (order.begin(), order.end(), id);
        return it == order.end() ? order.size() : size_t(it - order.begin());
    };
    std::stable_sort (targets.begin(), targets.end(), [&] (const Target& a, const Target& b) {
        return rank (a.node->nodeId) < rank (b.node->nodeId);
    });
    order.clear();
    for (const auto& t : targets) order.push_back (t.node->nodeId);
    auto id = selected.load();
    if (std::find (order.begin(), order.end(), id) == order.end()) id = order.empty() ? 0 : order.front();
    choose (id);
    if(master.load()!=0 && std::find(order.begin(),order.end(),master.load())==order.end()){master=0;masterLength=0;}
    if(std::none_of(targets.begin(),targets.end(),[&](const Target& t){return t.node->nodeId==captureGate->load();}))captureGate->store(0);
    updateSoloMute();
}
bool LooperManagerNode::structureLocked() const
{
    if (isLocked() || playPending.load() || playRequested.load() || handoffTarget.load()) return true;
    for (const auto& t:targets)
        if (t.looper->managerState.load() == int(LooperEngine::State::playing)
            || t.looper->managerTransportPending.load()) return true;
    return false;
}
juce::String LooperManagerNode::masterChangeReason(uint32 id) const
{
    if (structureLocked()) return "Stop all loopers and finish or cancel pending actions before choosing a master.";
    int base=0; bool found=id==0;
    for (const auto& t:targets) if (t.node->nodeId==id) {
        found=true; base=std::max(t.looper->managerLength.load(),t.looper->managerFixedLength.load());
    }
    if (!found) return "Looper is no longer available.";
    for (const auto& t:targets) if (t.node->nodeId!=id && syncMode(t.node->nodeId)>0) {
        const int length=std::max(t.looper->managerLength.load(),t.looper->managerFixedLength.load());
        if (length>0 && (base<=0 || (syncMode(t.node->nodeId)==1 ? length!=base : length%base!=0)))
            return "Linked looper " + juce::String(t.node->nodeId) + " has an incompatible length. Existing recordings will not be resized.";
    }
    return {};
}
bool LooperManagerNode::setMaster(uint32 id)
{
    if(id==master.load()) return true;
    if(masterChangeReason(id).isNotEmpty()) return false;
    master.store(id);masterLength.store(0);masterReanchor.store(true);
    if(id!=0)syncModes[id]=0;
    if(auto* g=getParentGraph()) g->rebuild();
    return true;
}
bool LooperManagerNode::setSyncMode(uint32 id, int mode)
{
    if(isLocked() || id==master.load() || mode<0 || mode>2) return false;
    for(const auto& t:targets) if(t.node->nodeId==id) {
        if(t.looper->managerLength.load()>0 || t.looper->managerFixedLength.load()>0) return false;
        syncModes[id]=mode;
        if(auto* g=getParentGraph()) g->rebuild();
        return true;
    }
    return false;
}
bool LooperManagerNode::isLocked() const
{
    for (const auto& t : targets) if (t.looper->captureBusy.load() || t.looper->captureRequested.load()) return true;
    return false;
}
void LooperManagerNode::choose (uint32 id)
{
    selected.store (id);
    for (const auto& t : targets) t.looper->managerSelected.store (t.node->nodeId == id);
}
void LooperManagerNode::advance (int delta)
{
    if (targets.empty()) return;
    int index = 0;
    for (int i = 0; i < int(targets.size()); ++i) if (targets[i].node->nodeId == selected.load()) index = i;
    choose (targets[(index + (delta < 0 ? int(targets.size()) - 1 : 1)) % targets.size()].node->nodeId);
}
void LooperManagerNode::move (uint32 id, int direction)
{
    if (isLocked()) return;
    auto it = std::find (order.begin(), order.end(), id);
    if (it == order.end()) return;
    const auto index = int(it - order.begin()), other = jlimit(0,int(order.size())-1,index + direction);
    if (other < 0 || other >= int(order.size())) return;
    const auto value=order[index];order.erase(order.begin()+index);order.insert(order.begin()+other,value);
    if (auto* g = getParentGraph()) g->rebuild();
}
void LooperManagerNode::serviceOfflineSelection()
{
    // Discard live transport requests when no device is running.
    playRequested=false;stopRequested=false;playPending=false;playNeedsClock=false;cancelHandoff();
    for(const auto& t:targets) {
        const int action=t.control->request.exchange(-1);
        if(action==int(BoxAction::mute)){const bool mute=!t.looper->getMuted();t.looper->setMuted(mute);if(mute)t.control->solo=false;}
        if(action==int(BoxAction::solo)){const bool solo=!t.control->solo.load();t.control->solo=solo;if(solo)t.looper->setMuted(false);}
    }
    updateSoloMute();
    if(isLocked()) return;
    for(const auto& t:targets)
        if(t.looper->pedalSectionActive.load() && t.looper->activeRequested.exchange(false)) choose(t.node->nodeId);
    if(const auto id=requested.exchange(0))
        for(const auto& t:targets)if(t.node->nodeId==id)choose(id);
    if(const auto delta=stepRequested.exchange(0))advance(delta);
}

void LooperManagerNode::render (RenderContext& rc)
{
    auto& midi = *rc.midi.getWriteBuffer (0);
    outgoing.clear();
    const int frames = rc.audio.getNumSamples();
    const bool resetNow = resetRequested.exchange(false);
    if(resetNow) {
        ++stopGeneration;for(auto& t:targets){t.looper->managerMomentaryRelease=true;t.control->uiHeld=false;t.control->midiHeld=false;t.control->playingHeld=false;}
        ++resetEpoch; ++clockEpoch; sharedFrame=0; timelineValid=false; playPending=false; playNeedsClock=false;
        stopRequested=false; playRequested=false; requested=0; stepRequested=0; gestureTargets={}; gestureCommands={}; navigation={}; stopHold={}; cancelHandoff();
        // Keep held presses latched, but do not lose a release in the reset block.
        for (const auto meta : midi)
            if (meta.numBytes == 3 && (meta.data[0] & 0xf0) == 0xb0 && meta.data[2] < 64)
                held[meta.data[0] & 15][meta.data[1] & 127] = false;
        midi.clear();
        for(const auto& t:targets)t.control->request=-1;
    }
    const bool reanchor=masterReanchor.exchange(false);
    int base=0; int64_t origin=0;
    for(const auto& t:targets) if(t.node->nodeId==master.load() && t.looper->managerResetSeen.load()==resetEpoch && !resetNow) {
        base=std::max(t.looper->managerFixedLength.load(),t.looper->managerLength.load()); origin=t.looper->managerOrigin.load();
    }
    if(!resetNow) for(const auto& t:targets) {
        if((t.node->nodeId==master.load() || t.mode>0) && t.node->isEnabled() && !t.node->isSuspended()
            && t.looper->managerResetSeen.load()==resetEpoch && t.looper->managerClockEpoch.load()==clockEpoch) {
            sharedFrame=std::max(sharedFrame,t.looper->managerClockFrame.load());
        }
    }
    if(reanchor) origin=sharedFrame;
    masterLength.store(base);
    for(const auto& t:targets){t.looper->managerPlayToEnd=t.control->playToEnd.load();t.looper->managerMomentaryEnabled=t.control->momentary.load();t.looper->managerVolume=groupVolume.load();t.looper->managerPan=groupPan.load();}
    for(const auto& t:targets) t.looper->setManagerContext(t.node->nodeId==master.load()?1:t.mode==0?0:t.mode+1,
        base,sharedFrame,origin,resetEpoch);
    for(const auto& t:targets) {t.looper->setManagerClockEpoch(clockEpoch); if(reanchor && (t.node->nodeId==master.load() || t.mode>0))t.looper->managerReanchor=true;}
    sharedFrame += frames;
    bool locked = isLocked(), anyActive = false, anyContent = false, anySync = false;
    uint64_t oldest = 0;
    for (auto& t : targets) {
        const auto state = static_cast<LooperEngine::State> (t.looper->managerState.load());
        t.active = state == LooperEngine::State::playing || state == LooperEngine::State::recording || state == LooperEngine::State::overdubbing || state == LooperEngine::State::replacing;
        anyActive |= t.active;
        if (t.active) oldest = std::max (oldest, t.looper->managerRunFrames.load());
        if (t.looper->managerLength.load() > 0) { anyContent = true; anySync |= t.looper->getQuantized(); }
    }
    if (!anyActive) timelineValid = false;
    else if (!timelineValid) { timelineFrames = static_cast<int64_t> (oldest); timelineValid = true; }

    double beat = 0, beatsPerSample = 0, barBeats = 4, barOrigin = 0;
    int numerator=4,denominator=4;
    bool clockValid = false;
    if (auto* head = getPlayHead()) if (auto p = head->getPosition()) {
        const auto bpm = p->getBpm(), ppq = p->getPpqPosition();
        if(auto start=p->getPpqPositionOfLastBarStart()) if(std::isfinite(*start)) barOrigin=*start;
        if (auto meter = p->getTimeSignature())
            if (meter->numerator > 0 && meter->denominator > 0) {numerator=meter->numerator;denominator=meter->denominator;barBeats = numerator * 4.0 / denominator;}
        if (p->getIsPlaying() && bpm && ppq && std::isfinite(*bpm) && *bpm > 0
            && std::isfinite(*ppq) && renderRate > 0) {
            beat = *ppq; beatsPerSample = *bpm / (60.0 * renderRate); clockValid = true;
        }
    }
    transportRunning.store(clockValid);
    for(const auto& t:targets)t.looper->managerDisplayRunning.store(clockValid || anyActive);
    // A transport stop or seek cancels a queued launch, never fires it unexpectedly.
    if (playPending.load() && (!clockValid || locked || anyActive
        || (hadClock && std::abs(beat - expectedBeat) > std::max(0.01, beatsPerSample * 2))))
        playPending.store (false);
    const bool handoffClockChanged = handoffTarget.load() && handoffBeatsPerSample>0 && clockValid
        && (numerator!=handoffNumerator || denominator!=handoffDenominator
            || std::abs(beatsPerSample-handoffBeatsPerSample)>1.0e-12
            || std::abs(barBeats-handoffBarBeats)>1.0e-9
            || std::abs(std::remainder(barOrigin-handoffBarOrigin,barBeats))>1.0e-7);
    if (handoffTarget.load() && (handoffClockChanged || handoffSource != selected.load() ||
        (hadClock && (!clockValid || std::abs(beat-expectedBeat)>std::max(0.01,beatsPerSample*2))))) cancelHandoff();
    hadClock = clockValid;
    expectedBeat = beat + frames * beatsPerSample;
    int events = 0;
    const auto send = [&] (uint32 target, int command, int at, uint64_t age = 0) {

        uint8 packet[22] { 0xf0, 0x7d, 'J', 'L' };
        for (int b = 0; b < 5; ++b) { packet[4+b] = uint8((nodeId >> (b*7)) & 127); packet[9+b] = uint8((target >> (b*7)) & 127); }
        packet[14] = uint8(command);
        const int size = (command == LooperProcessor::managerPlayAllCommand || command == LooperProcessor::managerRestartAllCommand) ? 22 : 16;
        if (size == 22) for (int b = 0; b < 6; ++b) packet[15+b] = uint8((age >> (b*7)) & 127);
        packet[size-1] = 0xf7;
        if (++events > 1024 && BoundedMidi::limit(packet,size)!=BoundedMidi::critical) {
            BoundedMidi::dropped.fetch_add(1,std::memory_order_relaxed);return;
        }
        BoundedMidi::add(outgoing,packet,size,at);
    };
    const int64_t blockFrame = sharedFrame - frames;
    const auto findTarget = [&](uint32 id) -> Target* {
        for (auto& t:targets) if(t.node->nodeId==id) return &t;
        return nullptr;
    };
    uint32 captureOwner=0;
    for(const auto& t:targets)if(t.looper->captureBusy.load() || t.looper->captureRequested.load())captureOwner=t.node->nodeId;
    const auto executeHandoff = [&](int at) {
        auto* to=findTarget(handoffTarget.load());
        if(locked || !to || !to->node->isEnabled() || to->node->isSuspended() || !to->looper->pedalSectionActive.load()) {cancelHandoff();return;}
        if(!pendingSelectionOnly.load()) {
            if(to->looper->getBypassed() || !to->looper->recordingControl(handoffProfile).enabled
                || !to->looper->managerCanOverdub.load() || (to->mode>0 && base<=0)){cancelHandoff();return;}
            send(to->node->nodeId,LooperProcessor::managerProfileFirst+handoffProfile,at);
            locked=true;captureOwner=to->node->nodeId;
        }
        choose(to->node->nodeId);cancelHandoff();
    };
    const auto flushHandoff = [&](int before) {
        if(handoffTarget.load() && !handoffNeedsClock.load()) {
            navigationRemaining=clockValid?std::max(1,int(std::ceil(double(handoffFrame-blockFrame)*beatsPerSample/barBeats))):0;
            if(handoffFrame<blockFrame+before)executeHandoff(jlimit(0,std::max(0,frames-1),int(handoffFrame-blockFrame)));
        }
    };
    const auto queueHandoff = [&](int direction,int at,bool longPress=false,bool forceSelection=false) {
        const int chosenProfile=forceSelection?-1:jlimit(-1,3,longPress?longProfile.load():navigationProfile.load());
        const bool selectionOnly=chosenProfile<0;
        if(handoffTarget.load() && handoffDirection==direction && pendingSelectionOnly.load()==selectionOnly) {cancelHandoff();return;}
        if(locked || targets.size()<2)return;
        int index=0;for(int i=0;i<int(targets.size());++i)if(targets[i].node->nodeId==selected.load())index=i;
        auto& to=targets[(index+int(targets.size())+direction)%targets.size()];
        const int profile=std::max(0,chosenProfile);
        if(!to.node->isEnabled() || to.node->isSuspended() || !to.looper->pedalSectionActive.load()
            || (!selectionOnly && (to.looper->getBypassed() || !to.looper->recordingControl(profile).enabled
                || !to.looper->managerCanOverdub.load() || (to.mode>0 && base<=0))))return;
        cancelHandoff();
        handoffSource=selected.load();handoffTarget=to.node->nodeId;handoffDirection=direction;
        pendingSelectionOnly=selectionOnly;handoffProfile=profile;displayNavigationProfile=chosenProfile;
        const int timing=jlimit(0,2,longPress?longTiming.load():shortTiming.load());
        handoffFrame=blockFrame+at;
        handoffBeatsPerSample=0;
        if(timing!=0) {
            if(!clockValid){cancelHandoff();handoffNeedsClock=true;return;}
            handoffNumerator=numerator;handoffDenominator=denominator;
            handoffBeatsPerSample=beatsPerSample;handoffBarBeats=barBeats;handoffBarOrigin=barOrigin;
            const int bars=timing==2?jlimit(1,32,longPress?longBars.load():shortBars.load()):0;
            const double nextBeat=barOrigin+(std::floor((beat+at*beatsPerSample-barOrigin)/barBeats)+1+bars)*barBeats;
            handoffFrame=blockFrame+int64_t(std::llround((nextBeat-beat)/beatsPerSample));
        } else executeHandoff(at);
    };
    bool sectionAllowsActions=true;
    const auto syncMomentary = [&](Target& t,int at) {
        auto& c=*t.control;
        const bool down=c.momentary.load() && (c.uiHeld.load() || c.midiHeld)
            && t.node->isEnabled() && !t.node->isSuspended() && t.looper->pedalSectionActive.load();
        if(down==c.playingHeld)return;
        if(down && (captureOwner==t.node->nodeId || t.looper->getBypassed() || t.looper->managerLength.load()<=0))return;
        if(!down)t.looper->managerMomentaryRelease=true;
        send(t.node->nodeId,down?LooperProcessor::managerMomentaryStart:LooperProcessor::managerMomentaryStop,at);
        c.playingHeld=down;
    };
    const auto boxAction = [&](Target& t,BoxAction action,int at) {
        if(!sectionAllowsActions || !t.node->isEnabled() || t.node->isSuspended() || !t.looper->pedalSectionActive.load())return;
        auto& c=*t.control;
        if(action==BoxAction::mute){const bool mute=!t.looper->getMuted();t.looper->setMuted(mute);if(mute)c.solo=false;updateSoloMute();return;}
        if(action==BoxAction::solo){const bool solo=!c.solo.load();c.solo=solo;if(solo)t.looper->setMuted(false);updateSoloMute();return;}
        if(action==BoxAction::rewind){if(t.mode==0 && !t.looper->recordingSettingsLocked())send(t.node->nodeId,LooperProcessor::managerRewindCommand,at);return;}
        if(action==BoxAction::record) {
            const int profile=jlimit(0,3,c.profile.load());
            if((locked && captureOwner!=t.node->nodeId) || t.looper->getBypassed() || (captureOwner!=t.node->nodeId && !t.looper->recordingControl(profile).enabled))return;
            cancelHandoff();send(t.node->nodeId,LooperProcessor::managerProfileFirst+profile,at);
            locked=true;captureOwner=t.node->nodeId;return;
        }
        // Transport of the recording target must be ended with its capture action,
        // not an unrelated box command. Other loopers remain independently usable.
        if(captureOwner==t.node->nodeId)return;
        if(t.looper->managerLength.load()>0)send(t.node->nodeId,int(LooperEngine::Action::playStop),at);
    };
    const auto flushStopHold = [&](int through) {
        if (stopHold.channel && gestureFrame + through - stopHold.start >= std::max<int64_t>(1, std::llround(renderRate * 0.8))) {
            resetRequested.store(true); // Apply the existing group reset at the next block boundary.
            stopHold = {};
        }
    };
    const auto flushNavigation = [&](int through) {
        const auto threshold=int64_t(std::llround(renderRate*0.8));
        for(auto& channelGestures:navigation) for(auto& nav:channelGestures)
            if(nav.direction && !nav.fired && gestureFrame+through-nav.start>=threshold) {
                nav.fired=true;
                if(!locked) queueHandoff(nav.direction,through,true);
            }
    };
    const auto launch = [&] (int at) {
        const bool restart = !timelineValid && base > 0;
        if (!timelineValid) {
            timelineFrames = -at; timelineValid = true;
            if (restart) {
                sharedFrame = origin - at + frames; ++clockEpoch;
                for(const auto& t:targets) t.looper->setManagerClockEpoch(clockEpoch);
            }
        }
        send (0, restart ? LooperProcessor::managerRestartAllCommand : LooperProcessor::managerPlayAllCommand,
            at, uint64_t(std::max<int64_t>(0, timelineFrames + at)));
        for (auto& t : targets) if (t.looper->managerLength.load() > 0) t.active = true;
        playPending.store (false); playNeedsClock.store (false);
    };
    const auto flushPending = [&] (int before) {
        if (!playPending.load() || !clockValid || locked) return;
        const double samples = (pendingBeat - beat) / beatsPerSample;
        if (samples > frames) return;
        const int at = std::max(0, int(std::llround(samples)));
        if (at < before) launch (at);
    };
    const auto stopGroup = [&] (int at) {
        ++stopGeneration;for(auto& t:targets){t.looper->managerMomentaryRelease=true;t.control->uiHeld=false;t.control->midiHeld=false;t.control->playingHeld=false;}
        gestureTargets={}; gestureCommands={}; navigation={}; stopHold={}; cancelHandoff();
        send (0, LooperProcessor::managerStopAllCommand, at);
        for (auto& t : targets) t.active = false;
        timelineValid = false; playPending.store(false); playNeedsClock.store(false);
    };
    const auto playGroup = [&] (int at) {
        if (locked || !anyContent || playPending.load()) return;
        bool running = false, stopped = false;
        for (const auto& t : targets) if (t.looper->managerLength.load() > 0) { running |= t.active; stopped |= !t.active; }
        if (!stopped) return;
        if (running || !anySync) { launch (at); return; }
        if (!clockValid) { launch(at); return; }
        pendingBeat = (std::floor((beat + at * beatsPerSample) / barBeats) + 1) * barBeats;
        playPending.store (true); playNeedsClock.store (false);
    };
    for(const auto& t:targets) if(t.looper->pedalSectionActive.load() && t.looper->activeRequested.exchange(false))
        if(!locked){cancelHandoff();choose(t.node->nodeId);}
    if (auto id = requested.exchange (0))
        if (!locked) for (const auto& t : targets) if (t.node->nodeId == id) {cancelHandoff();choose (id);}
    if (auto delta = stepRequested.exchange (0)) { if (!locked) queueHandoff(delta,0,true,true); }
    const bool uiPlay = playRequested.exchange(false);
    if (stopRequested.exchange(false)) stopGroup(0);
    else if (uiPlay) playGroup(0);
    static constexpr LooperEngine::Action actions[] { LooperEngine::Action::playStop, LooperEngine::Action::record,
        LooperEngine::Action::overdub, LooperEngine::Action::stop, LooperEngine::Action::play, LooperEngine::Action::undo };
    int sectionCursor=0;
    const auto flushSectionEvents=[&](int through) {
        while(sectionCursor<sectionEventCount && sectionEvents[sectionCursor].offset<=through) {
            const auto event=sectionEvents[sectionCursor++];
            if(event.selection) {
                if(!locked && findTarget(event.selection)) {cancelHandoff();choose(event.selection);if(event.profile>=0 && event.profile<4)send(event.selection,LooperProcessor::managerProfileFirst+event.profile,event.offset); }
                continue;
            }
            flushPending(event.offset); flushHandoff(event.offset); // Preserve launches strictly before the transition.
            playPending=false; playNeedsClock=false; stopRequested=false; playRequested=false;
            requested=0; stepRequested=0; held={}; gestureTargets={}; gestureCommands={}; navigation={}; stopHold={}; cancelHandoff();
            sectionAllowsActions=event.start;
            for(const auto& target:targets){target.control->request=-1;target.control->uiHeld=false;target.control->midiHeld=false;target.control->playingHeld=false;}
            timelineValid=event.start; timelineFrames=-event.offset; hadClock=false;
            locked=false;
            for(auto& target:targets) target.active=event.start && target.looper->managerLength.load()>0;
            if(event.start) {sharedFrame=origin+frames-event.offset;++clockEpoch;}
        }
    };
    flushSectionEvents(0);
    for(auto& t:targets)syncMomentary(t,0);
    for(auto& t:targets)if(const int action=t.control->request.exchange(-1);action>=0 && action<boxActions)boxAction(t,BoxAction(action),0);
    updateSoloMute();
    for (const auto meta : midi)
    {
        const int at = jlimit(0, std::max(0, frames-1), meta.samplePosition);
        flushSectionEvents(at);
        flushPending(at); flushHandoff(at); flushNavigation(at); flushStopHold(at); // a stop at the exact launch sample takes precedence
        if (!sectionAllowsActions || meta.numBytes != 3 || (meta.data[0] & 0xf0) != 0xb0) continue;
        const int ch = (meta.data[0] & 15) + 1, cc = meta.data[1] & 127;
        const bool press = meta.data[2] >= 64;
        auto& was = held[ch - 1][cc];
        const bool changed=press!=was;
        was = press;
        if (!changed) continue;
        if(!press) {
            if (stopHold.channel == ch && stopHold.cc == cc) stopHold = {};
            auto& nav=navigation[ch-1][cc];
            if(nav.direction) {if(!nav.fired) queueHandoff(nav.direction,at); nav={}; continue;}
            auto& target=gestureTargets[ch-1][cc];
            if(target) {
                if(gestureCommands[ch-1][cc]==LooperProcessor::managerMomentaryStart){if(auto* t=findTarget(target)){t->control->midiHeld=false;syncMomentary(*t,at);}}
                else send(target,gestureCommands[ch-1][cc]+1,at);
                target=0;
            }
            continue;
        }
        const bool channelMatches = channel.load() == 0 || channel.load() == ch;
        if (channelMatches && cc == stopAllCC.load()) { stopGroup(at); stopHold={ch,cc,gestureFrame+at}; continue; }
        if(const auto learning=learningTarget.exchange(0)) {
            const int action=learningAction.exchange(-1);
            lastLearnRejected=!setBoxMapping(learning,action,{cc,ch});continue;
        }
        if (const int learning = learn.exchange (0)) {
            lastLearnRejected=!setGlobalMapping(learning,cc,ch);continue;
        }
        if (channelMatches) {
            if (cc == playAllCC.load()) { playGroup(at); continue; }
            const bool prev = cc == previousCC.load(), next = cc == nextCC.load();
            if (prev || next) { navigation[ch-1][cc]={prev?-1:1,gestureFrame+at,false}; continue; }
        }
        bool direct=false;
        for(auto& t:targets)for(int action=0;action<boxActions;++action){
            const int mapping=t.control->midi[size_t(action)].load();
            if((mapping&255)==cc+1 && (((mapping>>8)&31)==0 || ((mapping>>8)&31)==ch)){
                if(action==0 && t.control->momentary.load()) {
                    t.control->midiHeld=true;gestureTargets[ch-1][cc]=t.node->nodeId;gestureCommands[ch-1][cc]=LooperProcessor::managerMomentaryStart;syncMomentary(t,at);
                } else boxAction(t,BoxAction(action),at);
                direct=true;
            }
        }
        if(direct)continue;
        for (auto& t : targets) {
            if (t.node->nodeId != selected.load()) continue;
            for (int i = 0; i < LooperProcessor::midiActions; ++i) {
                const auto mapping = t.looper->getMidiMapping(i);
                if (mapping.cc != cc || (mapping.channel != 0 && mapping.channel != ch)) continue;
                if((i==1 || i==2 || i>=6) && locked && captureOwner!=0 && captureOwner!=t.node->nodeId)break;
                if(i==8){if(!locked)send(t.node->nodeId,34,at);break;}
                if(i==1 || i==2 || i==6 || i==7)captureOwner=t.node->nodeId;

                cancelHandoff(); playPending.store(false); // individual transport overrides an armed group launch
                if(i==0 || i==1 || i==2 || i==5 || i>=6) {
                    const int command=i==2?20:i==5?22:i==1?28:i>=6?30+(i-6)*2:26;
                    gestureTargets[ch-1][cc]=t.node->nodeId;
                    gestureCommands[ch-1][cc]=uint8(command);
                    send(t.node->nodeId,command,at);
                } else send(t.node->nodeId, int(actions[i]), at);
                if (i == 0 || i == 1 || i == 2 || i == 5 || i>=6) locked = true;
                else {
                    const bool start = i == 4 || (i == 0 && !t.active);
                    if (start && t.looper->managerLength.load() > 0) {
                        if (!timelineValid) { timelineFrames = -at; timelineValid = true; }
                        t.active = true;
                    } else t.active = false;
                    bool active = false; for (const auto& member : targets) active |= member.active;
                    if (!active) timelineValid = false;
                }
                break;
            }
        }
    }
    flushSectionEvents(frames);
    sectionEventCount=0;
    flushPending(frames); flushHandoff(frames); flushNavigation(frames); flushStopHold(frames);
    gestureFrame+=frames;
    if (timelineValid) timelineFrames += frames;
    updateSoloMute();
    blocked.store(locked);
    midi.swapWith(outgoing); outgoing.clear();
}
void LooperManagerNode::getState (MemoryBlock& block)
{
    XmlElement xml ("LooperManager");
    xml.setAttribute ("version", 6);
    xml.setAttribute("groupVolume",double(groupVolume.load()));xml.setAttribute("groupPan",double(groupPan.load()));
    xml.setAttribute("shortTiming",shortTiming.load());xml.setAttribute("longTiming",longTiming.load());
    xml.setAttribute("shortBars",shortBars.load());xml.setAttribute("longBars",longBars.load());
    xml.setAttribute("navigationProfile",navigationProfile.load());xml.setAttribute("longProfile",longProfile.load());
    xml.setAttribute ("master", int(master.load()));
    xml.setAttribute ("selected", int(selected.load()));
    xml.setAttribute ("playAll", playAllCC.load());
    xml.setAttribute ("stopAll", stopAllCC.load());
    xml.setAttribute ("previous", previousCC.load()); xml.setAttribute ("next", nextCC.load()); xml.setAttribute ("channel", channel.load());
    for (auto id : order) {
        auto* child=xml.createNewChildElement("Member"); child->setAttribute("id",int(id)); child->setAttribute("sync",syncMode(id));
        auto found=memberControls.find(id);
        if(auto* c=found==memberControls.end()?nullptr:found->second.get()) {
            child->setAttribute("playToEnd",c->playToEnd.load());child->setAttribute("momentary",c->momentary.load());child->setAttribute("profile",c->profile.load());child->setAttribute("solo",c->solo.load());
            for(int i=0;i<boxActions;++i)child->setAttribute("mapping"+String(i),c->midi[size_t(i)].load());
        }
    }
    const auto text = xml.toString(); block.replaceAll (text.toRawUTF8(), text.getNumBytesAsUTF8());
}
void LooperManagerNode::setState (const void* data, int size)
{
    auto xml = parseXML (String::fromUTF8 (static_cast<const char*> (data), size));
    if (!xml || !xml->hasTagName ("LooperManager") || (xml->getIntAttribute ("version") < 1 || xml->getIntAttribute ("version") > 6)) return;
    groupVolume=float(jlimit(0.0,1.0,xml->getDoubleAttribute("groupVolume",1)));groupPan=float(jlimit(-1.0,1.0,xml->getDoubleAttribute("groupPan",0)));
    selected.store (uint32(xml->getIntAttribute ("selected")));
    previousCC.store (jlimit (-1, 127, xml->getIntAttribute ("previous", 24)));
    nextCC.store (jlimit (-1, 127, xml->getIntAttribute ("next", 25)));
    playAllCC.store (jlimit (-1, 127, xml->getIntAttribute ("playAll", 28)));
    stopAllCC.store (jlimit (-1, 127, xml->getIntAttribute ("stopAll", 27)));
    channel.store (jlimit (0, 16, xml->getIntAttribute ("channel", 1)));
    master.store(uint32(xml->getIntAttribute("master",0))); sharedFrame=0; resetEpoch=0; masterLength=0;
    shortTiming=jlimit(0,2,xml->getIntAttribute("shortTiming",0));longTiming=jlimit(0,2,xml->getIntAttribute("longTiming",0));
    shortBars=jlimit(1,32,xml->getIntAttribute("shortBars",1));longBars=jlimit(1,32,xml->getIntAttribute("longBars",1));
    navigationProfile=jlimit(-1,3,xml->getIntAttribute("navigationProfile",1));
    longProfile=jlimit(-1,3,xml->getIntAttribute("longProfile",-1));
    order.clear(); syncModes.clear();memberControls.clear();
    for (auto* m : xml->getChildIterator()) if (m->hasTagName ("Member")) {
        auto id=uint32(m->getIntAttribute("id")); order.push_back(id); syncModes[id]=jlimit(0,2,m->getIntAttribute("sync",0));
        auto c=std::make_shared<MemberControl>();c->playToEnd=m->getBoolAttribute("playToEnd",false);c->momentary=m->getBoolAttribute("momentary",false);c->profile=jlimit(0,3,m->getIntAttribute("profile",1));c->solo=m->getBoolAttribute("solo",false);
        for(int i=0;i<boxActions;++i){const int packed=m->getIntAttribute("mapping"+String(i),0);const int cc=(packed&255)-1,ch=(packed>>8)&31;
            c->midi[size_t(i)]=(cc>=-1 && cc<=127 && ch<=16)?packed:0;}
        memberControls[id]=std::move(c);
    }
}
} // namespace element
