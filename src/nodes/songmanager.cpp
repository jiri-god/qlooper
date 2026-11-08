// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/boundedmidi.hpp"
#include "nodes/songmanager.hpp"
#include "nodes/loopermanager.hpp"
#include "engine/graphnode.hpp"
#include <element/node.h>
#include <cmath>

namespace element {
using namespace juce;
SongManagerNode::SongManagerNode():Processor(0) { setName("Song Manager"); }
SongManagerNode::~SongManagerNode() {
    cancelPendingUpdate();
    for(const auto& target:loops)target.looper->pedalSectionActive.store(true);
    for(auto& connection:topologyConnections)connection.disconnect();
}
void SongManagerNode::handleAsyncUpdate() {
    if(auto* graph=getParentGraph())graph->rebuild();
}
void SongManagerNode::refreshPorts() {
    if(getNumPorts()) return;
    PortList ports;
    ports.add(PortType::Audio,0,0,"audio_in_1","Left",true);
    ports.add(PortType::Audio,1,1,"audio_in_2","Right",true);
    ports.add(PortType::Audio,2,0,"audio_out_1","Left",false);
    ports.add(PortType::Audio,3,1,"audio_out_2","Right",false);
    setPorts(ports);
}
void SongManagerNode::getPluginDescription(PluginDescription& d) const {
    d.name=d.descriptiveName="Song Manager"; d.fileOrIdentifier="element.songManager";
    d.uniqueId=0x534d4731; d.pluginFormatName="Element"; d.manufacturerName="Jgod";
    d.category="Utility"; d.version="1.0.0"; d.numInputChannels=d.numOutputChannels=2; d.isInstrument=false;
}
void SongManagerNode::prepareToRender(double r,int) {
    rate=r; filtered.ensureSize(65536); held=fired=false; frame=0;
}
void SongManagerNode::render(RenderContext&) {} // Audio is unchanged; parent owns the final panic gate.
void SongManagerNode::setOrder(const std::vector<uint32>& ids) {
    if(selectionLocked()||pending.load())return;
    order.assign(ids.begin(),ids.begin()+std::min<size_t>(ids.size(),1024));
    if(auto* graph=getParentGraph()) graph->rebuild();
}
void SongManagerNode::cancelSelection() noexcept {
    pendingTarget=pendingIndex=-1;
    pendingSection.store(0);pendingLooper.store(nullptr);
}
bool SongManagerNode::captureLocked(uint32 section) const noexcept {
    for(const auto& target:loops) if(target.section==section &&
        (target.looper->captureBusy.load() || target.looper->captureRequested.load())) return true;
    return false;
}
bool SongManagerNode::selectionLocked() const noexcept {
    return muted.load() || captureLocked(activeSection.load());
}
void SongManagerNode::selectLooper(const LooperProcessor* looper) noexcept {
    if(!effective.load() || selectionLocked()) return;
    for(size_t i=0;i<loops.size();++i) if(loops[i].looper==looper && loops[i].owner) {
        if(captureLocked(loops[i].section)) return;
        selectionRequest.store((uint64_t(targetGeneration)<<32) | uint64_t(i+1));
        return;
    }
}
bool SongManagerNode::toggleLooperPlayback(LooperProcessor* looper) noexcept {
    if(!effective.load() || muted.load()) return false;
    for(const auto& target:loops) if(target.looper==looper && target.section==activeSection.load())
        return looper->request(LooperEngine::Action::playStop);
    return false;
}
void SongManagerNode::rebuildLoopTargets() {
    for(auto& connection:topologyConnections)connection.disconnect();
    topologyConnections.clear();
    for(const auto& target:loops)target.looper->pedalSectionActive.store(true);
    loops.clear();++targetGeneration;selectionRequest.store(0);
    auto collect=[&](auto&& self,GraphNode& graph,uint32 section)->void {
        topologyConnections.push_back(graph.renderingSequenceChanged.connect([this]{triggerAsyncUpdate();}));
        for(int i=0;i<graph.getNumNodes();++i) {
            auto* node=graph.getNode(i);
            if(auto* looper=dynamic_cast<LooperProcessor*>(node->getAudioProcessor())) {
                auto* owner=dynamic_cast<LooperManagerNode*>(graph.getNodeForId(looper->managerOwner.load()));
                loops.push_back({section,node,owner,looper,owner});
            }
            if(auto* nested=dynamic_cast<GraphNode*>(node)) self(self,*nested,section);
        }
    };
    for(const auto& section:roster)
        if(preparedOrder.empty() || std::find(preparedOrder.begin(),preparedOrder.end(),section.node->nodeId)!=preparedOrder.end())
        collect(collect,*section.graph,section.node->nodeId);
}
void SongManagerNode::configure(GraphNode& graph) {
    const auto previousOrder=preparedOrder;
    const auto previousRoster=roster;
    roster.clear(); preparedOrder.clear();
    for(int i=0;i<graph.getNumNodes();++i) if(auto* section=dynamic_cast<GraphNode*>(graph.getNode(i)))
        roster.push_back({section,section});
    for(auto id:order) for(const auto& s:roster) if(s.node->nodeId==id) {preparedOrder.push_back(id);break;}
    // Graph rebuild can invalidate target identities even when the song order is unchanged.
    // Cancel only a pending mouse selection; ordinary Next remains pending.
    if(pendingTarget>=0) pending.store(false);
    cancelSelection();rebuildLoopTargets();
    bool changed=previousOrder!=preparedOrder;
    for(auto id:preparedOrder) {
        const auto old=std::find_if(previousRoster.begin(),previousRoster.end(),[&](const Section& s){return s.node->nodeId==id;});
        const auto now=std::find_if(roster.begin(),roster.end(),[&](const Section& s){return s.node->nodeId==id;});
        changed|=old==previousRoster.end()||now==roster.end()||old->node.get()!=now->node.get();
    }
    // Unrelated cable/mixer rebuilds must not restart the song or lose held MIDI.
    const uint32 oldActive=activeSection.load();
    const bool activeExists=std::any_of(roster.begin(),roster.end(),[&](const Section& section){return section.node->nodeId==oldActive;});
    if(changed || !activeExists) {
        const uint32 fallback=preparedOrder.empty()?(roster.empty()?0:roster.front().node->nodeId):preparedOrder.front();
        const bool retain=activeExists && (preparedOrder.empty() || std::find(preparedOrder.begin(),preparedOrder.end(),oldActive)!=preparedOrder.end());
        activeSection=retain?oldActive:fallback;
        const auto index=std::find(preparedOrder.begin(),preparedOrder.end(),activeSection.load());
        currentIndex=(retain && currentIndex.load()>=0 && index!=preparedOrder.end())?int(index-preparedOrder.begin()):-1;
        pending=false;ended=false;remainingBars=0;
        initial|=!retain;held=fired=false;
    }
    for(const auto& target:loops)target.looper->pedalSectionActive.store(target.section==activeSection.load() && !muted.load());
}
void SongManagerNode::getState(MemoryBlock& data) {
    ValueTree state("JgodSongManager"); state.setProperty("version",2,nullptr);
    state.setProperty("timing",switchTiming.load(),nullptr);state.setProperty("bars",countInBars.load(),nullptr);state.setProperty("recording",recordingProfile.load(),nullptr);
    for(int i=0;i<4;++i)state.setProperty("actionCC"+String(i),actionCC[i].load(),nullptr);
    state.setProperty("cc",pedalCC.load(),nullptr); state.setProperty("channel",channel.load(),nullptr);
    for(auto id:order) { ValueTree item("Section");item.setProperty("id",int64(id),nullptr);state.addChild(item,-1,nullptr); }
    MemoryOutputStream out(data,false);state.writeToStream(out);
}
void SongManagerNode::setState(const void* data,int size) {
    auto state=ValueTree::readFromData(data,size);if(!state.hasType("JgodSongManager")) return;
    switchTiming=jlimit(0,2,int(state.getProperty("timing",1)));countInBars=jlimit(1,32,int(state.getProperty("bars",1)));recordingProfile=jlimit(-1,3,int(state.getProperty("recording",-1)));
    for(int i=0;i<4;++i)actionCC[i]=jlimit(-1,127,int(state.getProperty("actionCC"+String(i),-1)));
    pedalCC.store(jlimit(-1,127,int(state.getProperty("cc",30))));channel.store(jlimit(1,16,int(state.getProperty("channel",1))));
    order.clear();for(int i=0;i<state.getNumChildren()&&i<1024;++i) {auto n=state.getChild(i);if(n.hasType("Section")) order.push_back(uint32(int64(n["id"])));}
    cancelSelection();selectionRequest.store(0);
    currentIndex.store(-1);activeSection.store(0);pending.store(false);ended.store(false);muted.store(false);initial=true;
}
void SongManagerNode::stopForPanic() noexcept {
    cancelSelection();selectionRequest.store(0);
    pending.store(false);
    nextRequest.store(false);rewindRequest.store(false);jumpRequest=-1;remainingBars=0;
    if(!muted.load()) {held=false;fired=false;}
    // Preserve the mute owner's held gesture so a second hold can restore audio.
    // Preserve an explicit Restore request and the current owner's mute latch.
    // Propagating a stop must not manufacture additional mute owners.
}
void SongManagerNode::serviceOfflineSelection()
{
    if(!effective.load()) return;
    for(const auto& t:loops)if(t.looper->activeRequested.exchange(false))selectLooper(t.looper);
    const auto token=selectionRequest.exchange(0);
    const auto index=uint32(token)-1;
    if(!token || uint32(token>>32)!=targetGeneration || index>=loops.size() || selectionLocked())return;
    const auto& target=loops[index];
    if(!target.owner || captureLocked(target.section))return;
    const auto at=std::find(preparedOrder.begin(),preparedOrder.end(),target.section);
    if(at==preparedOrder.end())return;
    cancelSelection();pending=false;ended=false;
    currentIndex=int(at-preparedOrder.begin());activeSection=target.section;
    for(const auto& t:loops)t.looper->pedalSectionActive.store(t.section==target.section && !muted.load());
    target.owner->select(target.node->nodeId);
    target.owner->serviceOfflineSelection();
}

void SongManagerNode::plan(MidiBuffer& midi,int frames) noexcept {
    blockMuted=muted.load(); muteAt=-1;
    int transition=-1, panicAt=-1, stopAll=initial?0:-1;bool manualJump=false,captureSeen=false;
    uint32 before=activeSection.load(), after=before;
    bool clock=false;double beat=0,step=0,bar=4,barOrigin=0;int beatsPerBar=4;
    if(auto* h=getPlayHead()) if(auto p=h->getPosition()) {
        auto bpm=p->getBpm(),ppq=p->getPpqPosition();
        if(auto meter=p->getTimeSignature()) if(meter->numerator>0&&meter->denominator>0) {bar=meter->numerator*4.0/meter->denominator;beatsPerBar=meter->numerator;}
        if(auto origin=p->getPpqPositionOfLastBarStart()) if(std::isfinite(*origin))barOrigin=*origin;
        if(p->getIsPlaying()&&bpm&&ppq&&*bpm>0&&std::isfinite(*bpm)&&std::isfinite(*ppq)&&rate>0) {clock=true;beat=*ppq;step=*bpm/(60*rate);}
    }
    auto doPanic=[&](int at) {
        cancelSelection();selectionRequest.store(0);
        jumpRequest=-1;nextRequest=false;remainingBars=0;
        panicAt=at;muteAt=blockMuted?-1:at;muted.store(true);pending.store(false);ended.store(true);fired=true;
    };
    auto holdAction=[&](int at) {
        if(muted.load()) {restoreRequest.store(true);fired=true;}
        else doPanic(at);
    };
    auto requestNext=[&](int at) {
        if(pending.load()){pending=false;cancelSelection();remainingBars=0;return;}
        if(ended.load()||muted.load()||transition>=0||captureSeen||selectionLocked()||preparedOrder.empty())return;
        pending.store(true);
        pendingImmediate=switchTiming.load()==0;recordingNotice=0;
        pendingBeat=clock?barOrigin+(std::floor((beat+at*step-barOrigin)/bar)+1+(switchTiming.load()==2?countInBars.load():0))*bar:0;
    };
    if(restoreRequest.exchange(false)) {muted.store(false);ended.store(false);blockMuted=false;held=false;fired=true;}
    if(rewindRequest.exchange(false) && !selectionLocked()) {cancelSelection();selectionRequest.store(0);stopAll=0;before=activeSection.load();after=preparedOrder.empty()?(roster.empty()?0:roster.front().node->nodeId):preparedOrder.front();transition=0;currentIndex.store(-1);activeSection.store(after);pending.store(false);ended.store(false);held=fired=false;}
    if(panicRequest.exchange(false)) doPanic(0);
    if(nextRequest.exchange(false)) requestNext(0);
    if(const int jump=jumpRequest.exchange(-1);jump>=0 && jump<int(preparedOrder.size()) && !selectionLocked() && !captureLocked(preparedOrder[size_t(jump)])) {
        cancelSelection();pending=false;remainingBars=0;recordingNotice=0;ended=false;
        after=preparedOrder[size_t(jump)];activeSection=after;currentIndex=jump;transition=0;manualJump=true;
    }
    for(const auto& target:loops)if(target.looper->activeRequested.exchange(false))selectLooper(target.looper);
    if(const auto token=selectionRequest.exchange(0)) {
        const auto index=uint32(token)-1;
        if(uint32(token>>32)==targetGeneration && index<loops.size() && !selectionLocked()) {
            const auto& target=loops[index];
            if(target.owner && target.owner->isEnabled() && !target.owner->isSuspended()
                && !captureLocked(target.section) && target.node->isEnabled() && !target.node->isSuspended()) {
                if(target.section==before) {
                    // Clicking the current section also cancels an armed mouse jump.
                    if(pendingTarget>=0) {pending.store(false);cancelSelection();}
                    target.owner->selectForSection(target.node->nodeId,0);
                } else if(!pending.load()) {
                    const int start=std::max(0,currentIndex.load()+1);
                    auto found=std::find(preparedOrder.begin()+std::min(size_t(start),preparedOrder.size()),preparedOrder.end(),target.section);
                    if(found==preparedOrder.end())found=std::find(preparedOrder.begin(),preparedOrder.end(),target.section);
                    if(found!=preparedOrder.end()) {
                        pendingTarget=int(index);pendingIndex=int(found-preparedOrder.begin());
                        pendingSection.store(target.section);pendingLooper.store(target.looper);
                        pendingImmediate=false;pending.store(true);pendingBeat=clock?barOrigin+(std::floor((beat-barOrigin)/bar)+1)*bar:0;
                        ended.store(false);
                    }
                }
            }
        }
    }
    if(pending.load() && (captureLocked(before) || (pendingTarget>=0 && captureLocked(pendingSection.load())))) {
        pending.store(false);cancelSelection();
    }
    filtered.clear(); auto it=midi.begin();const auto finish=midi.end();
    for(int at=0;at<frames;++at) {
        while(it!=finish&&(*it).samplePosition<=at) {
            const auto metadata=*it;++it;
            if (metadata.numBytes > 3) {
                BoundedMidi::add(filtered,metadata.data,metadata.numBytes,metadata.samplePosition);
                continue;
            }
            const auto msg=metadata.getMessage();
            bool consume=false;
            // The parent plans before child processors render. A capture pedal
            // arriving earlier in this very block is not published as busy yet.
            // Prefer that capture over a queued mouse section jump.
            if(msg.isController() && msg.getControllerValue()>0) {
                for(const auto& target:loops) {
                    if(target.section!=before || !target.owner || target.owner->selectedID()!=target.node->nodeId)continue;
                    for(int action:{1,2,6,7}) {
                        const auto mapping=target.looper->getMidiMapping(action);
                        if(mapping.cc==msg.getControllerNumber() && (mapping.channel==0 || mapping.channel==msg.getChannel())) {
                            captureSeen=true;
                            if(pending.load() && clock && !pendingImmediate && beat+at*step+step*.001<pendingBeat){pending=false;cancelSelection();}
                        }
                    }
                }
            }
            if(msg.isController()) {
                if(learn.load()!=0&&msg.getControllerValue()>0) {if(learn.load()==1)pedalCC.store(msg.getControllerNumber());else actionCC[size_t(jlimit(0,3,learn.load()-2))]=msg.getControllerNumber();channel.store(msg.getChannel());learn.store(0);consume=true;held=false;fired=false;}
                else if(msg.getChannel()==channel.load() && [&]{for(int i=0;i<4;++i)if(actionCC[i].load()==msg.getControllerNumber()) {
                    const bool down=msg.getControllerValue()>0;const bool edge=down&&!actionHeld[i];actionHeld[i]=down;
                    if(edge){if(i==0)requestNext(at);else if(i==1)rewindRequest=true;else if(i==2)doPanic(at);else restoreRequest=true;}return true;
                }return false;}())consume=true;
                else if(msg.getControllerNumber()==pedalCC.load()&&msg.getChannel()==channel.load()) {
                    consume=true;
                    if(msg.getControllerValue()>0) {if(!held) {held=true;fired=false;heldSince=frame+at;}}
                    else if(held) {
                        if(!fired) {
                            if(frame+at-heldSince>=std::max<int64_t>(1,int64_t(std::llround(rate*0.8)))) holdAction(at);
                            else requestNext(at);
                        }
                        held=false;
                    }
                }
            }
            if(!consume)BoundedMidi::add(filtered,msg,metadata.samplePosition);
        }
        if(held&&!fired&&frame+at-heldSince>=std::max<int64_t>(1,int64_t(std::llround(rate*0.8)))) holdAction(at);
        if(pending.load()&&transition<0&&(pendingImmediate||!clock||beat+at*step+step*0.001>=pendingBeat)) {
            const int index=pendingTarget>=0?pendingIndex:currentIndex.load()+1;
            if(pendingTarget>=0) {
                const auto& target=loops[size_t(pendingTarget)];
                target.owner->selectForSection(target.node->nodeId,at);
            }
            const bool recordTransition=pendingTarget<0;
            cancelSelection();pending.store(false);transition=at;
            if(index<int(preparedOrder.size())) {currentIndex.store(index);after=preparedOrder[size_t(index)];activeSection.store(after);
                if(recordTransition && recordingProfile.load()>=0) {
                    LooperManagerNode* owner=nullptr;bool ambiguous=false;
                    for(const auto& t:loops)if(t.section==after && t.owner){if(owner && owner!=t.owner)ambiguous=true;owner=t.owner;}
                    recordingNotice=1;
                    if(owner && !ambiguous && owner->isEnabled() && !owner->isSuspended() && !owner->members().empty()) {
                        const auto& first=owner->members().front();const int profile=recordingProfile.load();
                        if(first.node->isEnabled() && !first.node->isSuspended() && !first.looper->getBypassed() && first.looper->managerCanOverdub.load() && (first.mode==0 || owner->masterLength.load()>0) && first.looper->recordingControl(profile).enabled && !captureLocked(after)) {
                            owner->selectForSection(first.node->nodeId,at,profile);recordingNotice=0;
                        }
                    }
                }
            }
            else {ended.store(true);stopAll=at;after=before;} // Keep last live sound, stop its loops.
        }
    }
    midi.swapWith(filtered);
    for(const auto& section:roster) {
        if(!preparedOrder.empty() && std::find(preparedOrder.begin(),preparedOrder.end(),section.node->nodeId)==preparedOrder.end())continue;
        GraphNode::SongGate gate;gate.enabled=true;
        gate.before=section.node->nodeId==before;gate.after=section.node->nodeId==after;gate.transition=transition;
        gate.stop=stopAll;
        if(transition>=0&&gate.before) gate.stop=transition;
        if(transition>=0&&gate.after&&!manualJump&&!ended.load()&&currentIndex.load()>=0) gate.start=transition;
        gate.panic=panicAt;
        section.graph->setSongGate(gate);
    }
    for(const auto& target:loops)target.looper->pedalSectionActive.store(target.section==after && !muted.load());
    remainingBars=pending.load() && clock && !pendingImmediate?std::max(1,int(std::ceil((pendingBeat-beat-frames*step)/bar))):0;
    displayClockRunning.store(clock);
    displayBpm.store(clock?float(step*60*rate):0);
    displayBeatsPerBar.store(beatsPerBar);
    double phase=clock?std::fmod(beat+frames*step-barOrigin,bar)/bar:0;
    if(phase<0)phase+=1;
    displayBarPhase.store(float(phase));
    displayBeat.store(clock?std::min(beatsPerBar,int(phase*beatsPerBar)+1):0);
    initial=false;frame+=frames;
}
void SongManagerNode::applyOutputMute(AudioBuffer<float>& audio) const noexcept {
    if(blockMuted) audio.clear();
    else if(muteAt>=0&&muteAt<audio.getNumSamples()) for(int ch=0;ch<audio.getNumChannels();++ch) audio.clear(ch,muteAt,audio.getNumSamples()-muteAt);
}
}
