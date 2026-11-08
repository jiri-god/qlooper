// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nodes/recorder.hpp"
#include "nodes/looper.hpp"
#include "engine/graphnode.hpp"
#include <element/node.h>

namespace element {
using namespace juce;
RecorderNode::RecorderNode() : Processor(0), Thread("Live session recorder")
{
    setName("Recorder");
    destination = File::getSpecialLocation(File::userMusicDirectory).getChildFile("Jgod Recordings");
}
RecorderNode::~RecorderNode() { releaseResources(); }
void RecorderNode::refreshPorts()
{
    if (getNumPorts()) return;
    PortList ports;
    for (int c=0; c<2; ++c) ports.add(PortType::Audio,c,c,"input_"+String(c+1),"Mix "+String(c+1),true);
    for (int c=0; c<2; ++c) ports.add(PortType::Audio,c+2,c,"output_"+String(c+1),"Out "+String(c+1),false);
    setPorts(ports);
}
void RecorderNode::prepareToRender(double r, int b)
{
    releaseResources(); rate=r; maximumBlock=jmax(4096,b);
}
void RecorderNode::releaseResources()
{
    stopRecording();
    // Graph excludes rendering before release/destruction. Flush and close on writer thread.
    if (isThreadRunning()) waitForThreadToExit(-1);
}
void RecorderNode::getPluginDescription(PluginDescription& d) const
{
    d.name=d.descriptiveName="Recorder"; d.fileOrIdentifier=EL_NODE_ID_RECORDER;
    d.uniqueId=0x4a524543; d.pluginFormatName="Element"; d.manufacturerName="Jgod";
    d.category="Audio"; d.version="1.0.0"; d.numInputChannels=d.numOutputChannels=2;
    d.isInstrument=false;
}
std::vector<RecorderNode::Source> RecorderNode::sources() const
{
    std::vector<Source> result;
    if (auto* g=getParentGraph()) for(int i=0;i<g->getNumNodes();++i) {
        auto* p=g->getNode(i);
        const int channels=p->getNumPorts(PortType::Audio,false);
        if (p==this || dynamic_cast<RecorderNode*>(p) || channels==0) continue;
        result.push_back({p->nodeId,sourceLabels.count(p->nodeId)?sourceLabels.at(p->nodeId):p->getName(),channels,dynamic_cast<LooperProcessor*>(p->getAudioProcessor())!=nullptr});
    }
    return result;
}
Result RecorderNode::startRecording()
{
    if (busy.load() || isThreadRunning()) return Result::fail("The previous take is still finishing.");
    if (!getParentGraph() || rate<=0 || maximumBlock<=0) return Result::fail("Recorder needs an active audio graph.");
    if (getOversamplingFactor()!=1) return Result::fail("Disable oversampling on Recorder before recording.");
    auto available=sources();
    int channels=2;
    for(const auto& choice:selected) {
        auto it=std::find_if(available.begin(),available.end(),[&](const Source& s){return s.id==choice.first;});
        if(it==available.end()) return Result::fail("A selected source no longer exists. Select the tracks again.");
        if(choice.second && it->looper && getParentGraph()->getNodeForId(it->id)->getOversamplingFactor()!=1)
            return Result::fail("Loops-only capture requires looper oversampling to be off.");
        channels+=it->channels;
    }
    if(channels>128) return Result::fail("Select at most 128 audio channels per take.");
    if(auto result=destination.createDirectory(); result.failed()) return result;
    const auto takeName="Take-"+Time::getCurrentTime().formatted("%Y%m%d-%H%M%S")+"-"+Uuid().toString().substring(0,8);
    lastTake=destination.getChildFile(takeName);
    if(auto result=lastTake.createDirectory(); result.failed()) return result;
    tracks.clear();
    tracks.push_back({nodeId,"Live mix","00-Live mix.wav",2,0,false,{}});
    int offset=2, index=1;
    for(const auto& s:available) if(auto it=selected.find(s.id);it!=selected.end()) {
        const bool loopsOnly=s.looper && it->second;
        auto name=File::createLegalFileName(s.name).substring(0,80);
        auto filename=String(index++).paddedLeft('0',2)+"-"+name+"-"+String(s.id)+(loopsOnly?"-loops-only":"")+".wav";
        tracks.push_back({s.id,s.name,filename,s.channels,offset,loopsOnly,{}}); offset+=s.channels;
    }
    error.store(0); captured.store(0); written.store(0);
    WavAudioFormat format;
    for(auto& t:tracks) {
        std::unique_ptr<OutputStream> stream=lastTake.getChildFile(t.filename).createOutputStream();
        if(stream) t.writer=format.createWriterFor(stream, AudioFormatWriterOptions().withSampleRate(rate)
            .withNumChannels(t.channels).withBitsPerSample(32).withSampleFormat(AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if(!t.writer) {
            for(auto& other:tracks) other.writer.reset();
            error.store(2); writeManifest(false); return Result::fail("Cannot create audio files in the selected folder.");
        }
    }
    const int capacity=jmax(maximumBlock*4,int(rate*2));
    staging.setSize(channels,maximumBlock); queued.setSize(channels,capacity);
    fifo.setTotalSize(capacity); fifo.reset();
    writeManifest(false);
    busy.store(true); starting.store(true);
    if(!startThread()) { starting.store(false); busy.store(false); for(auto& t:tracks)t.writer.reset(); return Result::fail("Cannot start the disk writer."); }
    accepting.store(true); starting.store(false);
    return Result::ok();
}
void RecorderNode::graphChanged() noexcept { if(accepting.load()) fail(3); }
void RecorderNode::beginBlock(int frames) noexcept
{
    // Publish in-flight before observing accepting. Stop/worker can then safely drain.
    audioBusy.store(true);
    blockActive=accepting.load(); blockFrames=frames;
    if(!blockActive) { audioBusy.store(false); return; }
    if(frames>maximumBlock || frames<=0) { fail(4); blockActive=false; audioBusy.store(false); return; }
    staging.clear(0,frames);
}
void RecorderNode::captureNode(Processor& node,const AudioBuffer<float>& audio,int channels,
                               float gainStart,float gainEnd,bool processed) noexcept
{
    if(!blockActive) return;
    for(const auto& t:tracks) if(t.id==node.nodeId) {
        const AudioBuffer<float>* source=&audio;
        if(t.loopsOnly) {
            auto* looper=dynamic_cast<LooperProcessor*>(node.getAudioProcessor());
            if(!processed) continue; // disabled/suspended loopers have no loop playback
            source=looper ? looper->recorderLoopAudio(blockFrames) : nullptr;
            if(!source || node.getOversamplingFactor()!=1) { fail(4); return; }
        }
        for(int c=0;c<jmin(t.channels,jmin(channels,source->getNumChannels()));++c) {
            staging.copyFrom(t.offset+c,0,*source,c,0,blockFrames);
            if(t.loopsOnly) staging.applyGainRamp(t.offset+c,0,blockFrames,gainStart,gainEnd);
        }
    }
}
void RecorderNode::endBlock() noexcept
{
    if(blockActive) {
        if(fifo.getFreeSpace()<blockFrames) fail(1);
        else {
            const auto scope=fifo.write(blockFrames);
            for(int c=0;c<queued.getNumChannels();++c) {
                queued.copyFrom(c,scope.startIndex1,staging,c,0,scope.blockSize1);
                queued.copyFrom(c,scope.startIndex2,staging,c,scope.blockSize1,scope.blockSize2);
            }
            captured.fetch_add(blockFrames);
        }
    }
    blockActive=false; audioBusy.store(false);
}
void RecorderNode::run()
{
    int64_t lastFlush=0;
    for(;;) {
        const int ready=jmin(4096,fifo.getNumReady());
        if(ready>0) {
            const auto scope=fifo.read(ready);
            for(auto& t:tracks) {
                const float* pointers[128] {};
                const auto writePart=[&](int start,int size) {
                    if(size==0) return;
                    for(int c=0;c<t.channels;++c) pointers[c]=queued.getReadPointer(t.offset+c,start);
                    if(!t.writer->writeFromFloatArrays(pointers,t.channels,size)) fail(2);
                };
                writePart(scope.startIndex1,scope.blockSize1); writePart(scope.startIndex2,scope.blockSize2);
            }
            written.fetch_add(ready);
            if(written.load()-lastFlush>=int64_t(rate)) {
                for(auto& t:tracks) if(!t.writer->flush()) fail(2);
                lastFlush=written.load();
                if(lastTake.getBytesFreeOnVolume()<64*1024*1024) fail(5);
            }
        } else {
            if(!starting.load() && !accepting.load() && !audioBusy.load() && fifo.getNumReady()==0) break;
            wait(5);
        }
    }
    for(auto& t:tracks) { if(!t.writer->flush()) error.store(2); t.writer.reset(); }
    writeManifest(error.load()==0);
    busy.store(false);
}
void RecorderNode::writeManifest(bool complete)
{
    auto* object=new DynamicObject(); var json(object);
    object->setProperty("sampleRate",rate); object->setProperty("frames",static_cast<juce::int64>(written.load()));
    object->setProperty("complete",complete); object->setProperty("status",complete?String("Saved"):statusText());
    object->setProperty("note","00-Live mix is the performance mix at Recorder output. Other files are optional stems, not an additive mix. All files share frame zero. No plugin-latency alignment is added between taps.");
    Array<var> entries;
    for(const auto& t:tracks) { auto* item=new DynamicObject();
        item->setProperty("nodeId",int(t.id)); item->setProperty("name",t.name); item->setProperty("file",t.filename);
        item->setProperty("channels",t.channels); item->setProperty("loopsOnly",t.loopsOnly); entries.add(var(item)); }
    object->setProperty("tracks",entries);
    lastTake.getChildFile("take.json").replaceWithText(JSON::toString(json,true));
}
String RecorderNode::statusText() const
{
    switch(error.load()) {
        case 1:return "Stopped: disk buffer overflow. Take may be incomplete.";
        case 2:return "Stopped: disk write failed. Check the take files.";
        case 3:return "Stopped: audio graph changed. Take preserved.";
        case 4:return "Stopped: audio configuration changed. Take preserved.";
        case 5:return "Stopped: less than 64 MB free on disk.";
        default:break;
    }
    if(accepting.load()) return "Recording live session";
    if(busy.load()) return "Finishing files...";
    return lastTake.isDirectory()?"Ready — previous take saved":"Ready";
}
void RecorderNode::getState(MemoryBlock& block)
{
    XmlElement xml("Recorder"); xml.setAttribute("version",1); xml.setAttribute("folder",destination.getFullPathName());
    for(auto choice:selected) { auto* s=xml.createNewChildElement("Source"); s->setAttribute("id",int(choice.first)); s->setAttribute("loopsOnly",choice.second); }
    const auto text=xml.toString(); block.replaceAll(text.toRawUTF8(),text.getNumBytesAsUTF8());
}
void RecorderNode::setState(const void* data,int size)
{
    if(isBusy()) return;
    auto xml=parseXML(String::fromUTF8(static_cast<const char*>(data),size));
    if(!xml || !xml->hasTagName("Recorder") || xml->getIntAttribute("version")!=1) return;
    const auto path=xml->getStringAttribute("folder"); if(File::isAbsolutePath(path)) destination=File(path);
    selected.clear(); for(auto* s:xml->getChildIterator()) if(s->hasTagName("Source")) selected[uint32(s->getIntAttribute("id"))]=s->getBoolAttribute("loopsOnly");
}
RecorderEditor::RecorderEditor(const Node& n):NodeEditor(n),recorder(dynamic_cast<RecorderNode*>(n.getObject()))
{
    for(Component* c:std::initializer_list<Component*>{&heading,&status,&path,&help,&record,&stop,&folder,&show,&viewport}) addAndMakeVisible(c);
    heading.setText("LIVE SESSION RECORDER",dontSendNotification);
    help.setText("Route the finished stereo mix through Recorder -> Audio Out.\nLive mix is always recorded. Optional stems below share its start and duration.\nLoops only excludes live input from the file, without changing monitoring.\nSelect tracks before recording; graph edits stop the take. WAV 32-bit float.",dontSendNotification);
    help.setJustificationType(Justification::topLeft);
    viewport.setViewedComponent(&list,false);
    record.setColour(TextButton::buttonColourId,Colours::darkred);
    record.onClick=[this]{ auto result=recorder->startRecording(); lastError=result.failed()?result.getErrorMessage():String(); timerCallback(); };
    stop.onClick=[this]{recorder->stopRecording();};
    folder.onClick=[this]{
        chooser=std::make_unique<FileChooser>("Choose recording folder",recorder->destination);
        Component::SafePointer<RecorderEditor> safe(this);
        chooser->launchAsync(FileBrowserComponent::openMode|FileBrowserComponent::canSelectDirectories,[safe](const FileChooser& f){
            if(safe && !safe->recorder->isBusy() && f.getResult().isDirectory()) safe->recorder->destination=f.getResult();
        });
    };
    show.onClick=[this]{if(recorder->lastTake.isDirectory())recorder->lastTake.revealToUser();};
    setSize(700,530); refreshSources(); timerCallback(); startTimerHz(5);
}
void RecorderEditor::refreshSources()
{
    if(recorder->isBusy()) return;
    if (auto* graph=recorder->getParentGraph()) for(int i=0;i<graph->getNumNodes();++i) {
        const auto id=graph->getNode(i)->nodeId;
        auto model=getNode().getParentGraph().getNodeById(id);
        if(model.isValid() && model.getName().isNotEmpty())recorder->sourceLabels[id]=model.getName();
        else recorder->sourceLabels.erase(id);
    }
    const auto sources=recorder->sources(); String next;
    for(auto it=recorder->selected.begin();it!=recorder->selected.end();) {
        if(std::none_of(sources.begin(),sources.end(),[&](const RecorderNode::Source& s){return s.id==it->first;})) it=recorder->selected.erase(it);
        else ++it;
    }
    for(const auto& s:sources) next+=String(s.id)+s.name+String(s.channels)+String(recorder->selected.count(s.id))+String(int(recorder->selected.count(s.id)?recorder->selected.at(s.id):false));
    if(next==signature) return;
    signature=next; rows.clear();
    for(const auto& s:sources) {
        Row row; row.id=s.id;
        row.selected=std::make_unique<ToggleButton>(s.name+"  ("+String(s.channels)+" ch)");
        row.selected->setToggleState(recorder->selected.count(s.id)>0,dontSendNotification);
        row.selected->onClick=[this,id=s.id]{auto it=std::find_if(rows.begin(),rows.end(),[id](const Row& r){return r.id==id;});
            if(recorder->isBusy()||it==rows.end())return;
            if(it->selected->getToggleState())recorder->selected[id]=it->loopsOnly && it->loopsOnly->getToggleState(); else recorder->selected.erase(id);};
        list.addAndMakeVisible(*row.selected);
        if(s.looper) {row.loopsOnly=std::make_unique<ToggleButton>("Loops only");
            row.loopsOnly->setToggleState(recorder->selected.count(s.id)?recorder->selected.at(s.id):true,dontSendNotification);
            row.loopsOnly->onClick=[this,id=s.id]{for(auto& r:rows)if(r.id==id && !recorder->isBusy() && recorder->selected.count(id))recorder->selected[id]=r.loopsOnly->getToggleState();};
            list.addAndMakeVisible(*row.loopsOnly); }
        rows.push_back(std::move(row));
    }
    resized();
}
void RecorderEditor::resized()
{
    auto r=getLocalBounds().reduced(12); heading.setBounds(r.removeFromTop(30)); status.setBounds(r.removeFromTop(30));
    auto row=r.removeFromTop(34); record.setBounds(row.removeFromLeft(140).reduced(2)); stop.setBounds(row.removeFromLeft(100).reduced(2)); folder.setBounds(row.removeFromLeft(170).reduced(2)); show.setBounds(row.reduced(2));
    path.setBounds(r.removeFromTop(30)); help.setBounds(r.removeFromTop(100)); viewport.setBounds(r);
    list.setSize(jmax(200,r.getWidth()-16),int(rows.size())*32);
    int y=0;for(auto& entry:rows){entry.selected->setBounds(0,y,list.getWidth()-150,30); if(entry.loopsOnly)entry.loopsOnly->setBounds(list.getWidth()-145,y,145,30);y+=32;}
}
void RecorderEditor::paint(Graphics& g){g.fillAll(Colours::darkgrey.darker());}
void RecorderEditor::timerCallback()
{
    if(!recorder)return;
    refreshSources(); const bool busy=recorder->isBusy();
    status.setText((lastError.isNotEmpty()?lastError:recorder->statusText())+"  |  "+String(recorder->seconds(),1)+" s",dontSendNotification);
    path.setText(recorder->destination.getFullPathName(),dontSendNotification);
    record.setEnabled(!busy); stop.setEnabled(recorder->isRecording());folder.setEnabled(!busy);show.setEnabled(!busy && recorder->lastTake.isDirectory());list.setEnabled(!busy);
}
} // namespace element
