// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nodes/looper.hpp"
#include "looperdisplay.hpp"
#include "nodes/looperwidgets.hpp"
#include "nodes/contexthelp.hpp"
#include "nodes/looperfileimport.hpp"
#include "nodes/looperresampler.hpp"
#include "nodes/looperkeyboard.hpp"
#include "ui/icons.hpp"
#include <element/node.h>
#include <element/juce/gui_basics.hpp>
#include <element/juce/audio_formats.hpp>
#include <thread>
#include <future>
#include <cmath>
#include <bit>

namespace element {
using namespace juce;
namespace {
constexpr LooperEngine::Action midiActionCommands[] { LooperEngine::Action::playStop, LooperEngine::Action::record, LooperEngine::Action::overdub, LooperEngine::Action::stop, LooperEngine::Action::play, LooperEngine::Action::undo };
void resample (LooperEngine::Snapshot& snapshot, double targetRate)
{
    resampleLooperSnapshot (snapshot, targetRate);
}
} // namespace

// Exclusive lifecycle changes (prepare/restore) may wait off audio.
// Saving/export uses the separate snapshot publication and never owns this gate.
class LooperProcessor::Exclusive
{
public:
    explicit Exclusive (LooperProcessor& p) : owner (p)
    {
        while (owner.occupied.exchange (true, std::memory_order_acquire))
            std::this_thread::yield();
    }
    ~Exclusive() { owner.occupied.store (false, std::memory_order_release); }

private:
    LooperProcessor& owner;
};
struct LooperProcessor::SnapshotRequest
{
    LooperEngine::SnapshotView view;
    std::unique_ptr<std::atomic<uint64_t>[]> cells;
    bool previewOnly=false;
    size_t cellCount = 0;
    int cellStride = 0;
    struct Range { uintptr_t begin=0,end=0; int layer=0,channel=0,kind=0; };
    std::array<Range,LooperEngine::maxLayers*8> ranges {};
    int rangeCount = 0;
    // Prepared off audio before pinning. Core buffers have unique owners;
    // held Add parts can share the same fallback range in recent history.
    void setView(const LooperEngine::SnapshotView& v) noexcept {
        view=v; cellStride=stride(v); rangeCount=0;
        for(int i=0;i<v.count;++i) for(int c=0;c<2;++c) {
            const auto& l=v.layers[i];
            const auto add=[&](const float* data,int frames,int kind) {
                if(!data || frames<=0)return;
                const auto base=reinterpret_cast<uintptr_t>(data);
                ranges[rangeCount++]={base,base+size_t(frames)*sizeof(float),i,c,kind};
            };
            add(l.channels[c],v.capacity,0);
            add(l.fallbackChannels[c],l.fallbackCapacity,1);
            add(l.pre[c],l.preFrames,2);add(l.post[c],l.postFrames,3);
        }
        std::sort(ranges.begin(),ranges.begin()+rangeCount,[](const Range& a,const Range& b){return a.begin<b.begin;});
    }
    static int stride(const LooperEngine::SnapshotView& v) noexcept {
        int handles=0;
        for(int i=0;i<v.count;++i) handles=std::max(handles,v.layers[i].preFrames+v.layers[i].postFrames);
        return v.length+handles;
    }
    explicit SnapshotRequest (LooperEngine::SnapshotView v,bool preview=false) : view (v),previewOnly(preview),
        cellCount (preview?0:size_t(v.count) * 2 * size_t(stride(v) + std::max(1, int(v.rate))))
    {
        cells = std::make_unique<std::atomic<uint64_t>[]> (cellCount);
        for (size_t i = 0; i < cellCount; ++i) cells[i].store (0, std::memory_order_seq_cst);
    }
    static uint64_t encode (float value) noexcept { return uint64_t(std::bit_cast<uint32_t>(value)) | (uint64_t(1) << 32); }
    static float decode (uint64_t value) noexcept { return std::bit_cast<float>(uint32_t(value)); }
    bool fallback(const LooperEngine::SnapshotLayerView& layer,int frame) const noexcept
    {
        if(!layer.fallbackChannels[0] || layer.fallbackCapacity<=0 || view.length<=0) return false;
        const int relative=(frame-layer.cloneStart+view.length)%view.length;
        return relative>=layer.cloneWritten && relative<layer.cloneLength;
    }
    bool valid (const LooperEngine::SnapshotLayerView& layer, int frame) const noexcept
    {
        if(fallback(layer,frame)) return true;
        return frame >= layer.missingPrefix && (frame - layer.start + view.length) % view.length < layer.written;
    }
    float* address(const LooperEngine::SnapshotLayerView& layer,int channel,int frame) const noexcept
    {
        if(fallback(layer,frame))
            return layer.fallbackChannels[channel]+(layer.fallbackOffset+(frame-layer.cloneStart+view.length)%view.length)%layer.fallbackCapacity;
        return layer.channels[channel]+(layer.offset+frame)%view.capacity;
    }
    std::atomic<uint64_t>& cell (int layer, int channel, int frame) noexcept
    { return cells[(size_t(layer) * 2 + size_t(channel)) * size_t(cellStride) + size_t(frame)]; }
};
static_assert (std::atomic<uint64_t>::is_always_lock_free);
static_assert (std::atomic_ref<float>::is_always_lock_free);
void LooperProcessor::snapshotWrite (void* context, float* address, float value) noexcept
{
    auto& request = *static_cast<SnapshotRequest*>(context);
    if(request.previewOnly){std::atomic_ref<float>(*address).store(value,std::memory_order_seq_cst);return;}
    const auto pointer = reinterpret_cast<uintptr_t>(address);
    const auto end=request.ranges.begin()+request.rangeCount;
    auto it=std::upper_bound(request.ranges.begin(),end,pointer,
        [](uintptr_t p,const SnapshotRequest::Range& r){return p<r.begin;});
    while(it!=request.ranges.begin()) {
        --it;
        if(pointer>=it->end)break;
        {
            const auto& layer=request.view.layers[it->layer];
            const int physical=int((pointer-it->begin)/sizeof(float));
            int frame=-1;
            if(it->kind>=2) frame=request.view.length+(it->kind==3?layer.preFrames:0)+physical;
            else {
                const int capacity=it->kind==1?layer.fallbackCapacity:request.view.capacity;
                const int offset=it->kind==1?layer.fallbackOffset:layer.offset;
                const int relative=(physical-offset+capacity)%capacity;
                const int candidate=it->kind==1?(layer.cloneStart+relative)%request.view.length:relative;
                if(candidate<request.view.length && request.valid(layer,candidate)
                    && request.address(layer,it->channel,candidate)==address) frame=candidate;
            }
            if(frame>=0) {
                uint64_t empty=0;
                request.cell(it->layer,it->channel,frame).compare_exchange_strong(empty,
                    SnapshotRequest::encode(std::atomic_ref<float>(*address).load(std::memory_order_seq_cst)),std::memory_order_seq_cst);
            }
        }
    }
    // Reader also loads atomically; the winner of the cell CAS preserves the
    // sample before this write, regardless of reader/writer interleaving.
    std::atomic_ref<float>(*address).store(value, std::memory_order_seq_cst);
}
void LooperProcessor::publishSnapshotView() noexcept
{
    auto description = engine.snapshotView();
    const auto gate = snapshotGate.load();
    description.epoch = (gate & ~uint64_t(3)) + ((gate & 1) ? 4 : 0);
    snapshotViews[snapshotBack] = description;
    snapshotBack = snapshotMiddle.exchange(snapshotBack);
}
LooperProcessor::Waveforms LooperProcessor::waveforms()
{
    std::lock_guard<std::recursive_mutex> lock(snapshotMutex);
    SnapshotRequest request({},true);
    for(;;) {
        snapshotFront=snapshotMiddle.exchange(snapshotFront);
        const auto description=snapshotViews[snapshotFront];
        auto expected=snapshotGate.load();
        if(expected!=description.epoch){std::this_thread::yield();continue;}
        request.setView(description);snapshotRequest.store(&request);
        if(snapshotGate.compare_exchange_strong(expected,description.epoch|2))break;
        snapshotRequest.store(nullptr);
    }
    struct Release {
        LooperProcessor& owner;
        ~Release(){owner.snapshotGate.fetch_and(~uint64_t(2));while(owner.snapshotReaders.load())std::this_thread::yield();owner.snapshotRequest.store(nullptr);}
    } release{*this};
    Waveforms result;
    const int n=request.view.length;
    for(int i=0;i<request.view.count;++i) {
        auto wave=std::make_shared<Waveform>();const auto& layer=request.view.layers[i];wave->id=layer.info.id;
        for(int f=0;f<n;++f) {
            if(!request.valid(layer,f))continue;
            const int first=std::min(1023,int(int64_t(f)*1024/std::max(1,n)));
            const int last=std::min(1024,std::max(first+1,int(int64_t(f+1)*1024/std::max(1,n))));
            for(int c=0;c<2;++c) {
                const float value=std::atomic_ref<float>(*request.address(layer,c,f)).load(std::memory_order_seq_cst);
                for(int bin=first;bin<last;++bin){wave->low[bin]=std::min(wave->low[bin],value);wave->high[bin]=std::max(wave->high[bin],value);}
            }
        }
        result.push_back(std::move(wave));
    }
    return result;
}
LooperEngine::Snapshot LooperProcessor::takeSnapshot()
{
    std::lock_guard<std::recursive_mutex> lock(snapshotMutex);
    std::unique_ptr<SnapshotRequest> request;
    for (;;) {
        snapshotFront = snapshotMiddle.exchange(snapshotFront);
        const auto description = snapshotViews[snapshotFront];
        const auto gate = snapshotGate.load();
        if (gate != description.epoch) { std::this_thread::yield(); continue; }
        // Allocation/zeroing is outside the callback; retry if a block advances.
        // Reuse the allocation when only position/metadata have changed.
        const size_t needed = size_t(description.count) * 2 * size_t(SnapshotRequest::stride(description));
        if (!request || request->cellCount < needed) {
            request = std::make_unique<SnapshotRequest>(description);
            continue;
        }
        request->setView(description);
        snapshotRequest.store(request.get());
        auto expected = description.epoch;
        if (snapshotGate.compare_exchange_strong(expected, description.epoch | 2)) break;
        snapshotRequest.store(nullptr);
    }
    LooperEngine::Snapshot result;
    result.rate = request->view.rate; result.length = request->view.length;
    result.fixedLength = request->view.fixedLength; result.multiple = request->view.multiple; result.phaseOffset = request->view.phaseOffset;
    try {
        for (int i = 0; i < request->view.count; ++i) {
            LooperEngine::SavedLayer layer;
            const auto& source = request->view.layers[i];
            layer.info = source.info;
            layer.coverageStart=source.coverageStart; layer.coverageFrames=source.coverageFrames; layer.captureEnd=source.captureEnd;
            for (int c = 0; c < 2; ++c) {
                layer.audio[c].resize(result.length);
                for (int f = 0; f < result.length; ++f) {
                    if (!request->valid(source, f)) { layer.audio[c][f] = 0; continue; }
                    auto& cell = request->cell(i, c, f);
                    uint64_t value = cell.load(std::memory_order_seq_cst);
                    if (!value) {
                        const auto sample = std::atomic_ref<float>(*request->address(source,c,f)).load(std::memory_order_seq_cst);
                        const auto encoded = SnapshotRequest::encode(sample);
                        if (cell.compare_exchange_strong(value, encoded, std::memory_order_seq_cst)) value = encoded;
                    }
                    layer.audio[c][f] = SnapshotRequest::decode(value);
                }
            }
            for(int part=0;part<2;++part) {
                auto& handle=part?layer.post:layer.pre;
                const int frames=part?source.postFrames:source.preFrames;
                for(int c=0;c<2;++c) {
                    handle[c].resize(frames);
                    const auto* data=part?source.post[c]:source.pre[c];
                    for(int f=0;f<frames;++f) {
                        auto& cell=request->cell(i,c,result.length+(part?source.preFrames:0)+f);
                        uint64_t value=cell.load(std::memory_order_seq_cst);
                        if(!value) {
                            const auto encoded=SnapshotRequest::encode(std::atomic_ref<float>(const_cast<float&>(data[f])).load(std::memory_order_seq_cst));
                            if(cell.compare_exchange_strong(value,encoded,std::memory_order_seq_cst)) value=encoded;
                        }
                        handle[c][f]=SnapshotRequest::decode(value);
                    }
                }
            }
            result.layers.push_back(std::move(layer));
        }
    } catch (...) {
        snapshotGate.fetch_and(~uint64_t(2));
        while (snapshotReaders.load()) std::this_thread::yield();
        snapshotRequest.store(nullptr);
        throw;
    }
    snapshotGate.fetch_and(~uint64_t(2));
    while (snapshotReaders.load()) std::this_thread::yield();
    snapshotRequest.store(nullptr);
    return result;
}

LooperProcessor::LooperProcessor()
    : AudioPluginInstance (BusesProperties().withInput ("Effect Return / Record", AudioChannelSet::stereo(), true).withOutput ("Playback + Monitor", AudioChannelSet::stereo(), true))
{
    static std::atomic<uint32_t> nextCountInSource{1};countInSource=nextCountInSource.fetch_add(1)&0x0fffffff;
    countInMidi.ensureSize(65536);
    auto parameter = std::make_unique<AudioParameterFloat> (ParameterID ("loopVolume", 1), "Loop Volume", 0.0f, 1.0f, 1.0f);
    volume = parameter.get();
    addHostedParameter (std::move (parameter));
    gain.setCurrentAndTargetValue (1.0f);
    const int defaults[] { 21, 20, 23, 22, 26, 29, -1, -1, -1 };
    recordingControls[0]=1u|(3u<<2)|(1u<<4)|(2u<<6)|(1u<<10)|(1u<<13);
    recordingControls[1]=(2u<<2)|(1u<<4)|(2u<<6)|(1u<<13);
    recordingControls[2]=recordingControls[3]=(2u<<2)|(1u<<4)|(2u<<6);
    for (int i = 0; i < midiActions; ++i)
        mappings[i].store (defaults[i]);
    startTimerHz(20);
}
LooperProcessor::~LooperProcessor() { stopTimer();if(historyFuture.valid())historyFuture.wait(); }
void LooperProcessor::fillInPluginDescription (PluginDescription& d) const
{
    d.name = getName();
    d.descriptiveName = "QLooper - layered stereo looper";
    d.fileOrIdentifier = EL_NODE_ID_LOOPER;
    d.pluginFormatName = "Element";
    d.manufacturerName = EL_NODE_FORMAT_AUTHOR;
    d.version = "1.9.0";
    d.numInputChannels = d.numOutputChannels = 2;
    d.isInstrument = false;
    d.hasSharedContainer = false;
}
void LooperProcessor::prepareToPlay (double newRate, int maximumBlock)
{
    if (! std::isfinite (newRate) || newRate < 1 || newRate > 384000)
        return;
    std::lock_guard<std::recursive_mutex> snapshotLock(snapshotMutex);
    Exclusive access (*this);

    if(managerMomentaryActive){engine.applyMomentary(false);managerMomentaryActive=false;}
    loopCapture.setSize(2, jmax(4096, maximumBlock));
    loopCaptureValid = false;
    if (engine.getRate() != newRate)
    {
        const auto origin = engine.getRate()>0 ? int64_t(std::llround(engine.getOrigin()*newRate/engine.getRate())) : 0;
        const auto frame = engine.getRate()>0 ? int64_t(std::llround(engine.getLinkFrame()*newRate/engine.getRate())) : 0;
        auto snapshot = engine.snapshot();
        resample (snapshot, newRate);
        engine.restorePrepared(snapshot,newRate,getDurationSeconds(),getRecordingLimit(),getTakeLimit(),getRolling(),getUndoDepth());
        engine.restoreRuntimeOrigin(origin,frame);
    }
    snapshotGate.fetch_add(4);
    {std::lock_guard<std::mutex> lock(historyMutex);
        if(!history || history->rate!=newRate){history=std::make_shared<LooperHistory>(newRate,historySeconds.load());invalidateHistory();}historyAllocatedBytes=history->memoryBytes();historyClockValid=false;}
    preparedRate = newRate;
    allocatedBytes.store(engine.memoryBytes());
    gain.reset (newRate, 0.01);
    gain.setCurrentAndTargetValue (volume->get());
    cancelGestures();
    publish();
}
void LooperProcessor::serviceOfflineControls()
{
    const bool first=audioAvailable.exchange(false);
    if (!first && commands.getNumReady()==0 && !viewPublishPending.load() && importPending.load()==nullptr) return;
    std::lock_guard<std::recursive_mutex> snapshotLock(snapshotMutex);
    Exclusive access(*this);
    snapshotGate.fetch_add(4);
    if(first) { cancelGestures();engine.finalizeOffline(); }
    engine.setFixedBars(fixedMode.load()?fixedBarCount.load():0);
    consumeImport();
    const auto scope=commands.read(commands.getNumReady());
    const auto apply=[this](const Command& c) {
        if(c.kind==3)return; // Capture cannot run without incoming audio.
        if(c.kind==1) engine.editLayer(c.id,c.field,c.value);
        else if(c.kind==2) cancelGestures();
        else {
            const auto a=c.action;
            if(a==LooperEngine::Action::record || a==LooperEngine::Action::freshRecord || a==LooperEngine::Action::overdub || a==LooperEngine::Action::replace) --captureRequested;
            if(a==LooperEngine::Action::undo || a==LooperEngine::Action::redo || a==LooperEngine::Action::clear || a==LooperEngine::Action::stop) engine.apply(a);
        }
    };
    for(int i=0;i<scope.blockSize1;++i)apply(actions[scope.startIndex1+i]);
    for(int i=0;i<scope.blockSize2;++i)apply(actions[scope.startIndex2+i]);
    publish();
}

bool LooperProcessor::enqueue (Command command) noexcept
{
    const auto scope = commands.write (1);
    if (scope.blockSize1 == 0)
        return false;
    actions[scope.startIndex1] = command;
    return true;
}
bool LooperProcessor::request (LooperEngine::Action action) noexcept
{
    Command command;
    command.action = action;
    const bool capture = action == LooperEngine::Action::record || action == LooperEngine::Action::freshRecord || action == LooperEngine::Action::overdub || action == LooperEngine::Action::replace;
    if (capture) ++captureRequested;
    const bool accepted = enqueue (command);
    if (capture && !accepted) --captureRequested;
    return accepted;
}
bool LooperProcessor::editLayer (uint32_t id, int field, float value) noexcept
{
    if (!std::isfinite(value)) return false;
    Command command;
    command.kind = 1;
    command.id = id;
    command.field = field;
    command.value = value;
    return enqueue (command);
}
bool LooperProcessor::setCapacityLimits(int recordings,int parts,bool rolling)
{ return setMemoryLimits(getDurationSeconds(),recordings,parts,rolling); }
bool LooperProcessor::setMemoryLimits(int seconds,int recordings,int parts,bool rolling,int depth)
{
    seconds=jlimit(1,LooperEngine::maxDurationSeconds,seconds);
    recordings=jlimit(1,LooperEngine::maxRecordings,recordings);parts=jlimit(1,LooperEngine::maxTakePasses,parts);
    depth=depth<0?getUndoDepth():jlimit(1,9,depth);
    if(seconds==getDurationSeconds() && recordings==getRecordingLimit() && parts==getTakeLimit()
        && rolling==getRolling() && depth==getUndoDepth())return true;
    std::lock_guard<std::recursive_mutex> snapshotLock(snapshotMutex);
    Exclusive access(*this);
    if(captureRequested.load()>0 || captureBusy.load()) return false;
    try {
        if(!engine.resizeMemory(seconds,recordings,parts,rolling,depth))return false;
        snapshotGate.fetch_add(4);
        durationSeconds.store(seconds);recordingLimit.store(recordings);takeLimit.store(parts);
        rollOldest.store(rolling);undoDepth.store(depth);allocatedBytes.store(engine.memoryBytes());publish();
    } catch(const std::bad_alloc&) { return false; }
    return true;
}
void LooperProcessor::setUndoDepth(int depth)
{ setMemoryLimits(getDurationSeconds(),getRecordingLimit(),getTakeLimit(),getRolling(),depth); }
namespace {
uint32_t packControl(LooperProcessor::RecordingControl c) {
    return uint32_t(c.shortAction)|(uint32_t(c.holdAction&3)<<2)|(uint32_t(c.start)<<4)
        |(uint32_t(c.bars)<<6)|(uint32_t(c.finishFixed)<<10)|(uint32_t(c.overwrite)<<11)
        |(uint32_t(c.hearPrevious)<<12)|(uint32_t(c.enabled)<<13)
        |(uint32_t(c.metronome)<<14)|(uint32_t(c.feedbackWithinTake)<<15)
        |(uint32_t(c.loops)<<16)|(1u<<23)|(uint32_t(c.stopAfter)<<24)|(uint32_t(c.holdAction==4)<<25)|(uint32_t(c.finishAtBoundary)<<26); // bit23: independent settings, not legacy defaults
}
LooperProcessor::RecordingControl unpackControl(uint32_t n) {
    return {int(n&3),(n&(1u<<25))?4:int((n>>2)&3),int((n>>4)&3),int((n>>6)&15),
        bool(n&1024),bool(n&2048),bool(n&4096),bool(n&8192),
        int((n>>16)&127),bool(n&(1u<<14)),bool(n&(1u<<15)),bool(n&(1u<<24)),bool(n&(1u<<26))};
}
}
LooperProcessor::RecordingControl LooperProcessor::recordingControl(int i) const noexcept {
    if(i<0 || i>=4)return {};
    const auto packed=recordingControls[i].load();
    auto c=unpackControl(packed);
    // Retain legacy API defaults until this controller is explicitly configured.
    if(!(packed&(1u<<23))){c.loops=autoPasses.load();c.metronome=countInClick.load();c.feedbackWithinTake=takeFeedback.load();}
    if(i<2)c.start=(i==0?recordPreparation.load():overdubPreparation.load())?2:
        (i==0?getRecordNextBar():getOverdubNextBar())?1:0;
    if(i==0)c.bars=countInBars.load();
    return c;
}
bool LooperProcessor::setRecordingControl(int i,RecordingControl c) {
    if(i<0 || i>=4 || recordingSettingsLocked() || c.shortAction<0 || c.shortAction>1
        || c.holdAction<0 || c.holdAction>4 || c.start<0 || c.start>2 || c.bars<0 || c.bars>8
        || c.loops<0 || c.loops>64 || (i<2 && !c.enabled))return false;
    recordingControls[i]=packControl(c);
    if(i==0){recordPreparation=c.start==2;recordNextBar=c.start==1;countInBars=c.bars;}
    if(i==1){overdubPreparation=c.start==2;overdubNextBar=c.start==1;}
    if(!c.enabled)mappings[controlMidi(i)]=-1;
    return true;
}
void LooperProcessor::configureCapture(int i) noexcept {
    const auto c=activeControl;
    juce::ignoreUnused(i);
    engine.setOptions(quantized.load(),graceMs.load(),c.loops);
    engine.setTakeOptions(takeFades.load(),c.feedbackWithinTake);
    engine.setCaptureBehavior(c.overwrite,c.hearPrevious,c.finishFixed?1:0);
    engine.setFinishAtBoundary(c.finishAtBoundary);
    const bool nextBar=c.start==1 || (quantized.load() && c.start==2 && c.bars==0);
    engine.setRecordNextBar(nextBar);engine.setOverdubNextBar(nextBar);
    engine.setPerformanceOptions(preserveTimeline.load(),playOnBar.load(),stopOnBar.load(),
        c.start==2&&!nextBar,c.start==2&&!nextBar,c.bars,Decibels::decibelsToGain(triggerDb.load()));
}
bool LooperProcessor::claimManagerCapture() noexcept {
    if(managerOwner.load()==0 || !managerCaptureGate)return true;
    uint32_t empty=0;const auto target=managerTarget.load();
    return managerCaptureGate->compare_exchange_strong(empty,target) || empty==target;
}
void LooperProcessor::controlAction(int i,bool hold,int lookback) noexcept {
    if(activeBypass || i<0 || i>=4)return;
    const auto c=recordingControl(i);if(!c.enabled || (hold && c.holdAction==0))return;
    if(!claimManagerCapture())return;
    if(transportWaitController>=0) {
        if(transportWaitController==i){transportWaitController=-1;transportStartRequested=false;}
        return;
    }
    if(engine.isStartPending()) {
        if(i==currentController){engine.cancelCaptureStart();}
        return;
    }
    if(engine.capturingNow() && !hold) {engine.apply(LooperEngine::Action::play);return;}
    if(engine.capturingNow() && hold && c.holdAction!=3 && (i!=currentController || c.holdAction!=2))return;
    if(!hostPlaying && transportStartSupported.load() && !engine.capturingNow()) {
        transportWaitController=i;transportWaitHold=hold;transportWaitLookback=lookback;
        transportStartRequested.store(true);return;
    }
    currentController=i;activeControl=c;configureCapture(i);
    if(hold) {
        if(c.holdAction==4){heldRecordActive=engine.beginHeldRecord(lookback);if(heldRecordActive && !engine.capturingNow() && c.stopAfter)engine.apply(LooperEngine::Action::stop);}
        else if(c.holdAction==2)engine.beginHeldReplace(lookback);
        else if(c.holdAction==3)engine.applyPerformance(LooperEngine::Action::freshRecord);
        else if(c.holdAction==1)engine.applyPerformance(LooperEngine::Action::record);
    } else engine.applyPerformance(c.shortAction==0?LooperEngine::Action::overdub:LooperEngine::Action::record);
}
bool LooperProcessor::resetSettings() {
    if(recordingSettingsLocked())return false;
    // Memory defaults are applied first and may reject retained audio/history
    // that would exceed them. Never discard recordings to make a reset fit.
    if(!setMemoryLimits(60,10,4,true,1))return false;
    for(int i=0;i<4;++i){RecordingControl c;c.enabled=i<2;if(i==0){c.shortAction=1;c.holdAction=3;c.finishFixed=true;}setRecordingControl(i,c);}
    const int defaults[]{21,20,23,22,26,29,-1,-1,-1};for(int i=0;i<midiActions;++i)mappings[i]=defaults[i];
    setLoopMode(0);setFixedBarCount(4);setGraceMs(100);setAutoPasses(0);setPan(0);setFeedback(1);
    setPlayFade(0);setStopFade(0);setCrossfade(10);setCaptureTail(10);setTakeFades(true);setTakeFeedback(true);
    preserveTimeline=true;playOnBar=false;stopOnBar=false;countInClick=false;triggerDb=-40;
    setHistoryLimits(30,1.0);
    setMonitor(true);setMuted(false);setBypassed(false);setVolume(1);return true;
}
bool LooperProcessor::setMidiMapping (int action, MidiMapping mapping)
{
    if (recordingSettingsLocked() || (action>=6 && action<8 && !recordingControl(action-4).enabled)) return false;
    if (action < 0 || action >= midiActions || mapping.cc < -1 || mapping.cc > 127 || mapping.channel < 0 || mapping.channel > 16)
        return false;
    for (int i = 0; i < midiActions; ++i)
    {
        const auto other = getMidiMapping (i);
        if (i != action && mapping.cc >= 0 && other.cc == mapping.cc && (other.channel == mapping.channel || other.channel == 0 || mapping.channel == 0))
            return false;
    }
    Command cancel; cancel.kind = 2; if (!enqueue(cancel)) return false;
    mappings[action].store (mapping.cc < 0 ? -1 : mapping.cc | (mapping.channel << 8));
    return true;
}
LooperProcessor::MidiMapping LooperProcessor::getMidiMapping (int action) const
{
    if (action < 0 || action >= midiActions)
        return {};
    const int value = mappings[action].load();
    return { value < 0 ? -1 : value & 127, value < 0 ? 0 : value >> 8 };
}
void LooperProcessor::publish() noexcept
{
    publishSnapshotView();
    managerCanOverdub.store(engine.canBeginOverdub());
    managerTransportPending.store(engine.getWaitingAction()!=0);
    managerState.store (int(engine.getState()));
    managerLength.store (engine.getLength());
    managerFixedLength.store(engine.getFixedLength()); managerOrigin.store(engine.getOrigin()); managerClockFrame.store(engine.getLinkFrame());
    managerResetSeen.store(resetGeneration); managerClockEpoch.store(consumedClockEpoch);
    managerRunFrames.store (engine.getRunFrames());
    captureBusy.store (engine.getState() == LooperEngine::State::recording
        || engine.getState() == LooperEngine::State::overdubbing || engine.isReplacing() || engine.isClonePending() || gestures[0].down || gestures[1].down || gestures[2].down || gestures[3].down || gestures[4].down || gestures[5].down || pedalClearPending || engine.isCaptureStartPending() || engine.isEndPending() || transportWaitController>=0);
    if(!captureBusy.load() && managerCaptureGate) {
        auto target=managerTarget.load();managerCaptureGate->compare_exchange_strong(target,0);
    }
    const auto scope = views.write (1);
    viewPublishPending.store(scope.blockSize1 == 0);
    if (scope.blockSize1 == 0)
        return;
    auto& data = viewData[scope.startIndex1];
    data.state = static_cast<int> (engine.getState());
    data.momentaryMuted=engine.momentaryMuted();
    data.loopMode=getLoopMode();data.controller=currentController;data.overwrite=activeControl.overwrite;data.transportWaiting=transportWaitController>=0;
    data.capacity=engine.getCapacity();data.importLength=engine.importTarget();
    data.linkMode=engine.getLinkMode();data.linkBase=engine.getLinkBase();data.requestedBars=engine.getRequestedBars();
    data.masterLength=engine.masterLength();data.masterPosition=engine.masterPosition();
    data.length = engine.getLength();
    data.position = engine.getPosition();
    data.waitingAction=engine.getWaitingAction();data.countIn=engine.getCountInRemaining();data.barPhase=engine.getBarPhase();
    data.soundWaiting=engine.waitingForSound();data.syncPosition=engine.getSyncPosition();
    // Presentation only: the shared sample clock keeps advancing for synchronization.
    // Assigning a master must not make an idle rig appear to start playing.
    if(displayLinkMode!=data.linkMode || data.length==0) {
        displaySyncPosition=0;displayMasterPosition=0;displayLinkMode=data.linkMode;
    }
    const bool displayRunning=hostPlaying || data.state==1 || data.state==2 || data.state==4 || data.state==5
        || (managerOwner.load()!=0 && managerDisplayRunning.load());
    if(displayRunning){displaySyncPosition=data.syncPosition;displayMasterPosition=data.masterPosition;}
    data.displaySyncPosition=data.length>0?displaySyncPosition%data.length:0;
    data.displayMasterPosition=data.masterLength>0?displayMasterPosition%data.masterLength:0;

    data.barFrames=engine.getBarFrames();data.captureTarget=engine.getCaptureTarget();data.capturedProgress=engine.getCapturedProgress();data.fixedCompatible=engine.fixedBarsCompatible();
    data.rate = engine.getRate();
    data.count = engine.getLayerCount();data.recordings=engine.recordingCount();
    data.pending = engine.isEndPending();
    data.armed = engine.isStartPending() || transportWaitController>=0;
    data.needsClock = engine.needsClock();
    data.captureOverdub = engine.startedByOverdub();
    data.undo = engine.canUndo();
    data.redo = engine.canRedo();
    data.finishingEdit = engine.isClonePending();
    data.bpm = hostBpm;
    data.numerator = hostNumerator;
    data.denominator = hostDenominator;
    for (int i = 0; i < LooperEngine::maxLayers; ++i) {
        data.layers[i] = engine.getLayer (i);
        data.layers[i].gain *= data.layers[i].feedbackGain;
    }
}
LooperProcessor::View LooperProcessor::view() const
{
    const auto scope = views.read (views.getNumReady());
    for (int i = 0; i < scope.blockSize1; ++i)
        cachedView = viewData[scope.startIndex1 + i];
    for (int i = 0; i < scope.blockSize2; ++i)
        cachedView = viewData[scope.startIndex2 + i];
    return cachedView;
}
void LooperProcessor::applyAction (LooperEngine::Action action) noexcept
{
    if((action==LooperEngine::Action::freshRecord || action==LooperEngine::Action::replace) && !claimManagerCapture())return;
    if (action == LooperEngine::Action::stop || action == LooperEngine::Action::clear || action == LooperEngine::Action::freshRecord) cancelGestures();
    if (activeBypass && (action == LooperEngine::Action::record || action == LooperEngine::Action::freshRecord || action == LooperEngine::Action::overdub || action == LooperEngine::Action::replace))
        return;
    if(action==LooperEngine::Action::record){controlAction(0);return;}
    if(action==LooperEngine::Action::overdub){controlAction(1);return;}
    engine.applyPerformance (action);
}
void LooperProcessor::cancelGestures() noexcept
{
    if (engine.getState()==LooperEngine::State::replacing) engine.apply(LooperEngine::Action::endReplace);
    if(heldRecordActive)engine.endHeldRecord();
    heldRecordActive=false;transportWaitController=-1;transportStartRequested=false;
    for(int g : {0,3,4,5}) if(gestures[g].down) suppressedRecordingPress[g]=true;
    gestures = {}; held = {}; pedalClearPending = false;
}
void LooperProcessor::fireHold(int index) noexcept
{
    if(index==0 || index>=3) {
        const int controller=index==0?1:index==3?0:index-2;
        controlAction(controller,true,int(gestures[index].elapsed));
    } else if(index==2) {
        pedalClearPending=engine.isClonePending();
        if(!pedalClearPending)engine.apply(LooperEngine::Action::clear);
    } else applyAction(LooperEngine::Action::redo);
}
void LooperProcessor::pedalEdge(int action,bool pressed) noexcept
{
    if(action==8){if(pressed)triggerHistory(false);return;}
    if(action==3 || action==4){if(pressed)applyAction(midiActionCommands[action]);return;}
    const int index=action==2?0:action==5?1:action==1?3:action>=6?action-2:2;
    auto& gesture=gestures[index];
    const bool recordingPedal=index==0 || index>=3;
    if(recordingPedal) {
        if(suppressedRecordingPress[index]) {if(!pressed)suppressedRecordingPress[index]=false;return;}
        if(pressed && !gesture.down) {
            for(int g : {0,3,4,5}) if(g!=index && gestures[g].down) {
                suppressedRecordingPress[index]=true;return;
            }
        }
    }
    if(pressed) {if(!gesture.down)gesture={true,false,0};}
    else if(gesture.down) {
        if(!gesture.fired && gesture.elapsed>=std::max<int64_t>(1,std::llround(preparedRate*.8))){gesture.fired=true;fireHold(index);}
        const bool fired=gesture.fired;gesture={};
        const int controller=action==1?0:action==2?1:action>=6?action-4:-1;
        if(controller>=0) {
            if(fired && transportWaitController==controller && transportWaitHold
                && (recordingControl(controller).holdAction==2 || recordingControl(controller).holdAction==4)){transportWaitController=-1;transportStartRequested=false;}
            if(fired && recordingControl(controller).holdAction==4) {if(heldRecordActive)engine.endHeldRecord();heldRecordActive=false;}
            else if(fired && recordingControl(controller).holdAction==2)engine.apply(LooperEngine::Action::endReplace);
            else if(!fired)controlAction(controller);
        } else if(!fired)applyAction(action==5?LooperEngine::Action::undo:LooperEngine::Action::playStop);
    }
}
void LooperProcessor::scheduleSectionEvent(int offset, bool start) noexcept
{
    if (sectionEventCount >= int(sectionEvents.size())) return;
    const SectionEvent event {std::max(0,offset),start};
    int index=sectionEventCount++;
    // Graph panic events may be offered before earlier section transitions.
    // Stable ordering preserves the caller's stop/start sequence at equal offsets.
    while(index>0 && sectionEvents[index-1].offset>event.offset) {
        sectionEvents[index]=sectionEvents[index-1]; --index;
    }
    sectionEvents[index]=event;
}
void LooperProcessor::applySectionEvent(bool start) noexcept
{
    managerMomentaryActive=false;
    cancelGestures();
    engine.finalizeCapture();
    engine.apply(LooperEngine::Action::stop);
    if (start) {
        engine.alignLinkedTimeline(0);
        engine.playFromTimeline(0);
    }
}
void LooperProcessor::publishCountIn(int offset,bool force) noexcept
{
    const bool enabled=currentController>=0?activeControl.metronome:countInClick.load();
    const bool on=!activeBypass && enabled && engine.countInAudible();
    if((force && enabled) || on!=lastCountIn){
        uint8_t data[10]{0xf0,0x7d,'J','C',0,0,0,0,uint8_t(on),0xf7};
        for(int n=0;n<4;++n)data[4+n]=uint8_t((countInSource>>(n*7))&127);
        countInMidi.addEvent(data,10,offset);lastCountIn=on;
    }
}
void LooperProcessor::stopAfterCapture() noexcept
{
    const bool now=engine.capturingNow();
    if(captureWasRunning && !now && !engine.isStartPending()) {
        heldRecordActive=false;
        if(currentController>=0 && activeControl.stopAfter)engine.apply(LooperEngine::Action::stop);
    }
    captureWasRunning=now;
}
void LooperProcessor::processRange (AudioBuffer<float>& audio, int start, int end) noexcept
{
    if (audio.getNumChannels() < 2)
        return;
    auto* l = audio.getWritePointer (0);
    auto* r = audio.getWritePointer (1);
    const bool live = activeBypass || monitor.load(), mute = activeBypass || muted.load() || (managerOwner.load()!=0 && managerSoloMuted.load());
    const int64_t holdSamples=std::max<int64_t>(1,std::llround(preparedRate*0.8));
    for (int i = start; i < end; ++i) {
        while(sectionEventCursor<sectionEventCount && sectionEvents[sectionEventCursor].offset<=i)
            applySectionEvent(sectionEvents[sectionEventCursor++].start);
        for (int g=0;g<int(gestures.size());++g) {
            auto& gesture=gestures[g];
            if (gesture.down && !gesture.fired && gesture.elapsed >= holdSamples) {
                gesture.fired=true;
                fireHold(g);
            }
            if (gesture.down) ++gesture.elapsed;
        }
        if (pedalClearPending && !engine.isClonePending()) {
            pedalClearPending = false; engine.apply(LooperEngine::Action::clear);
        }
        if(transportWaitController>=0 && transportWaitHold && transportWaitLookback<INT32_MAX)++transportWaitLookback;
        if(history)history->push(l[i],r[i]);
        if(hostPlaying)++historySyncAvailable;
        stopAfterCapture();
        publishCountIn(i,i==0);
        engine.process (l[i], r[i], gain.getNextValue(), live, mute,
                        loopCaptureValid ? loopCapture.getWritePointer(0)+i : nullptr,
                        loopCaptureValid ? loopCapture.getWritePointer(1)+i : nullptr);
        stopAfterCapture();
    }
}
void LooperProcessor::processBlock (AudioBuffer<float>& audio, MidiBuffer& midi)
{
    audioAvailable.store(true);
    countInMidi.clear();
    loopCaptureValid = false;
    if (occupied.exchange (true, std::memory_order_acquire))
    {
        resetHeldAfterGap = true;
        sectionEventCount=sectionEventCursor=0;
        if (! bypassed.load() && ! monitor.load())
            audio.clear();
        midi.clear();
        return;
    }
    snapshotReaders.fetch_add(1);
    const auto snapshotState = snapshotGate.fetch_or(1);
    struct SnapshotCallbackGuard {
        LooperProcessor& owner;
        ~SnapshotCallbackGuard() { owner.snapshotGate.fetch_add(3); owner.snapshotReaders.fetch_sub(1); owner.occupied.store(false, std::memory_order_release); }
    } snapshotGuard { *this };

    auto* snapshot = (snapshotState & 2) ? snapshotRequest.load() : nullptr;
    engine.setWriteObserver(snapshot, snapshot ? &LooperProcessor::snapshotWrite : nullptr);
    loopCaptureValid = loopCapture.getNumSamples() >= audio.getNumSamples();
    if (loopCaptureValid) loopCapture.clear(0, audio.getNumSamples());
    if (resetHeldAfterGap)
    {
        cancelGestures();
        resetHeldAfterGap = false;
    }
    const bool nextBypass = bypassed.load();
    if (nextBypass && ! activeBypass) { cancelGestures(); engine.finalizeCapture(); }
    activeBypass = nextBypass;
    gain.setTargetValue (volume->get());
    engine.setOptions (quantized.load(), graceMs.load(), autoPasses.load());
    engine.setRecordNextBar (recordNextBar.load());
    engine.setOverdubNextBar(overdubNextBar.load());
    engine.setFixedBars(fixedMode.load()?fixedBarCount.load():0);
    engine.setPerformanceOptions(preserveTimeline.load(),playOnBar.load(),stopOnBar.load(),
        recordPreparation.load(),overdubPreparation.load(),countInBars.load(),Decibels::decibelsToGain(triggerDb.load()));
    const bool momentaryEnabled=managerOwner.load()!=0 && managerMomentaryEnabled.load();
    if(momentaryEnabled!=lastMomentaryEnabled){engine.setMomentaryGate(!momentaryEnabled);lastMomentaryEnabled=momentaryEnabled;}
    if(!managerOwner.load())engine.clearMomentaryGate();
    engine.setManagerMix(managerOwner.load()?managerVolume.load():1.0f,managerOwner.load()?managerPan.load():0.0f);
    engine.setMixOptions(pan.load(),overdubFeedback.load(),playFadeMs.load(),stopFadeMs.load());
    engine.setTransitionOptions(crossfadeMs.load(), captureTailMs.load());
    engine.setTakeOptions(takeFades.load(),takeFeedback.load());
    if(currentController>=0 && (engine.isStartPending() || engine.capturingNow()))configureCapture(currentController);
    hostBpm = 0;
    hostNumerator = hostDenominator = 4;
    double ppq = 0;
    bool running = false;
    if (auto* playhead = getPlayHead())
        if (const auto position = playhead->getPosition())
        {
            if (const auto bpm = position->getBpm())
                hostBpm = *bpm;
            if (const auto signature = position->getTimeSignature())
            {
                hostNumerator = signature->numerator;
                hostDenominator = signature->denominator;
            }
            if (const auto beats = position->getPpqPosition())
                ppq = *beats;
            running = position->getIsPlaying();
        }
    engine.setClock (hostBpm, hostNumerator, hostDenominator, ppq, running);
    hostPlaying=running;
    const auto musicalFrame=engine.musicalFrame();
    if(!running || !historyClockValid || std::abs(musicalFrame-expectedHostFrame)>1 || historyClockBar!=engine.getBarFrames())historySyncAvailable=0;
    historyClockValid=running;historyClockBar=engine.getBarFrames();expectedHostFrame=musicalFrame+(running?audio.getNumSamples():0);
    const bool managed = managerOwner.load() != 0;
    const bool fresh = consumedContext != linkContext.serial;
    if (lastManagerOwner != managerOwner.load()) {
        if(managerMomentaryActive){engine.applyMomentary(false);managerMomentaryActive=false;}
        cancelGestures();
        if(managed) resetGeneration=linkContext.generation;
        lastManagerOwner=managerOwner.load();
    }
    if(managed && !fresh) {if(managerMomentaryActive){engine.applyMomentary(false);managerMomentaryActive=false;}cancelGestures();} // A disabled/disconnected Manager cannot complete a held pedal.
    const bool resetting = managed && fresh && resetGeneration != linkContext.generation;
    if (resetting) {
        engine.resetAll(); resetGeneration = linkContext.generation; cancelGestures(); midi.clear();
    }
    if (managed && fresh) { if(managerReanchor.exchange(false)) engine.prepareMasterChange(linkContext.mode,linkContext.base,linkContext.origin); engine.setLinkContext(linkContext.mode,linkContext.base,linkContext.frame,linkContext.origin); consumedContext=linkContext.serial; consumedClockEpoch=linkClockEpoch; }
    else if (!managed) engine.clearLinkContext();
    engine.setPlayToEnd(managed && managerPlayToEnd.load(),managed?linkContext.base:0,managed?linkContext.origin:0);
    if(running && transportWaitController>=0) {
        const int i=transportWaitController;const bool h=transportWaitHold;const int back=transportWaitLookback;
        transportWaitController=-1;controlAction(i,h,back);
    }

    {
        const auto scope = commands.read (commands.getNumReady());
        const auto apply = [this, resetting] (const Command& command) { if(command.kind==3) {if(!resetting)triggerHistory(command.field!=0);}
        else if (command.kind == 0) {
            if (!resetting) applyAction (command.action);
            if (command.action == LooperEngine::Action::record || command.action == LooperEngine::Action::freshRecord || command.action == LooperEngine::Action::overdub || command.action == LooperEngine::Action::replace) --captureRequested;
        } else if (command.kind == 2) cancelGestures(); else if (!resetting) engine.editLayer (command.id, command.field, command.value); };
        for (int i = 0; i < scope.blockSize1; ++i)
            apply (actions[scope.startIndex1 + i]);
        for (int i = 0; i < scope.blockSize2; ++i)
            apply (actions[scope.startIndex2 + i]);
    }
    consumeImport();
    int cursor = 0;
    for (const auto metadata : midi)
    {
        const auto* bytes = metadata.data;
        if ((metadata.numBytes == 16 || metadata.numBytes == 22) && bytes[0] == 0xf0 && bytes[1] == 0x7d && bytes[2] == 'J' && bytes[3] == 'L' && bytes[metadata.numBytes - 1] == 0xf7)
        {
            uint32_t owner = 0, target = 0;
            for (int b = 0; b < 5; ++b) { owner |= uint32_t(bytes[4+b] & 127) << (b*7); target |= uint32_t(bytes[9+b] & 127) << (b*7); }
            const bool panic = metadata.numBytes == 16 && target == 0 && bytes[14] == managerStopAllCommand;
            const bool groupPlay = metadata.numBytes == 22 && target == 0 && (bytes[14] == managerPlayAllCommand || bytes[14] == managerRestartAllCommand);
            if (owner != 0 && owner == managerOwner.load() && (panic || groupPlay || (metadata.numBytes == 16 && target == managerTarget.load() && (bytes[14] <= int(LooperEngine::Action::playStop) || (bytes[14]>=managerGestureFirst && bytes[14]<=managerMomentaryStop))))) {
                const int at = jlimit (cursor, audio.getNumSamples(), metadata.samplePosition);
                processRange (audio, cursor, at); cursor = at;
                while(sectionEventCursor<sectionEventCount && sectionEvents[sectionEventCursor].offset<=at)
                    applySectionEvent(sectionEvents[sectionEventCursor++].start);
                if (panic) { managerMomentaryActive=false;cancelGestures(); engine.finalizeCapture(); engine.apply (LooperEngine::Action::stop); }
                else if (groupPlay) {
                    uint64_t frames = 0;
                    for (int b = 0; b < 6; ++b) frames |= uint64_t(bytes[15+b] & 127) << (b*7);
                    if(bytes[14]==managerRestartAllCommand) engine.alignLinkedTimeline(frames);
                    engine.clearMomentaryGate();engine.playFromTimeline (frames);
                }
                else if(bytes[14]==managerFinishCaptureCommand) { cancelGestures(); engine.finalizeCapture(); }
                else if(bytes[14]==managerBeginOverdubCommand) { if(!activeBypass) { cancelGestures(); currentController=1;activeControl=recordingControl(1);configureCapture(1);engine.managerBeginOverdub(); } }
                else if(bytes[14]>=managerProfileFirst && bytes[14]<managerProfileFirst+4) {
                    const bool existing=engine.isStartPending() || engine.capturingNow() || transportWaitController>=0;
                    const int controller=transportWaitController>=0?transportWaitController:currentController;
                    controlAction(existing && controller>=0 ? controller : bytes[14]-managerProfileFirst);
                }
                else if(bytes[14]==managerMomentaryStart) {
                    managerMomentaryRelease=false;
                    if(!engine.capturingNow() && !engine.isCaptureStartPending() && !activeBypass) {engine.applyMomentary(true);managerMomentaryActive=engine.getState()==LooperEngine::State::playing;}
                }
                else if(bytes[14]==managerMomentaryStop) {if(managerMomentaryActive)engine.applyMomentary(false);managerMomentaryActive=false;}
                else if(bytes[14]==managerRewindCommand) engine.rewind();
                else if(bytes[14]==34)triggerHistory(false);
                else if (bytes[14]>=managerGestureFirst) pedalEdge(bytes[14]>=30?6+(bytes[14]-30)/2:bytes[14]<22?2:bytes[14]<24?5:bytes[14]<28?0:1,(bytes[14]&1)==0);
                else applyAction (static_cast<LooperEngine::Action> (bytes[14]));
            }
            continue;
        }
        if (managerOwner.load() != 0) continue;
        if (metadata.numBytes != 3 || (bytes[0] & 0xf0) != 0xb0)
            continue;
        const int at = jlimit (cursor, audio.getNumSamples(), metadata.samplePosition);
        processRange (audio, cursor, at);
        cursor = at;
        while(sectionEventCursor<sectionEventCount && sectionEvents[sectionEventCursor].offset<=at)
            applySectionEvent(sectionEvents[sectionEventCursor++].start);
        auto& down = held[bytes[0] & 15][bytes[1] & 127];
        const bool pressed = bytes[2] >= 64;
        if (pressed != down)
        {
            for (int i = 0; i < midiActions; ++i)
            {
                const auto mapping = getMidiMapping (i);
                if (mapping.cc == bytes[1] && (mapping.channel == 0 || mapping.channel == (bytes[0] & 15) + 1))
                    if(i<6 || i==8 || recordingControl(i-4).enabled)pedalEdge (i, pressed);
            }
        }
        down = pressed;
    }
    processRange (audio, cursor, audio.getNumSamples());
    while(sectionEventCursor<sectionEventCount && sectionEvents[sectionEventCursor].offset<=audio.getNumSamples()) applySectionEvent(sectionEvents[sectionEventCursor++].start);
    sectionEventCount=sectionEventCursor=0;
    midi.swapWith(countInMidi);
    engine.setWriteObserver(nullptr, nullptr);
    if(managerMomentaryRelease.exchange(false) && managerMomentaryActive){engine.applyMomentary(false);managerMomentaryActive=false;}
    publish();
}
void LooperProcessor::getStateInformation (MemoryBlock& data)
{
    const auto saved = takeSnapshot();
    MemoryOutputStream out (data, false);
    out.writeInt (0x454c4c50);
    out.writeInt (18);
    out.writeDouble (saved.rate > 0 ? saved.rate : 44100);
    out.writeFloat (volume->get());
    out.writeInt (saved.length);
    out.writeInt (saved.layers.size());
    out.writeInt (monitor.load());
    out.writeInt (muted.load());
    out.writeInt (quantized.load());
    out.writeInt (graceMs.load());
    out.writeInt (autoPasses.load());
    out.writeInt (bypassed.load());
    out.writeInt (recordNextBar.load());
    for (int i=0;i<5;++i) out.writeInt(mappings[i].load());
    for (int i = 0; i < saved.layers.size(); ++i)
    {
        const auto info = saved.layers[i].info;
        out.writeInt (info.enabled);
        out.writeInt (info.solo);
        out.writeFloat (info.gain);
        for (int c = 0; c < 2; ++c)
            for (int f = 0; f < saved.length; ++f)
                out.writeFloat (saved.layers[i].audio[c][f]);
    }
    out.writeInt(saved.fixedLength); out.writeInt(saved.multiple); out.writeInt64(saved.phaseOffset);
    out.writeInt(mappings[5].load());
    out.writeFloat(pan.load()); out.writeFloat(overdubFeedback.load());
    out.writeInt(playFadeMs.load()); out.writeInt(stopFadeMs.load()); out.writeInt(undoDepth.load());
    for(const auto& layer : saved.layers) { out.writeFloat(layer.info.pan); out.writeFloat(layer.info.feedbackGain); }
    out.writeInt(crossfadeMs.load()); out.writeInt(captureTailMs.load());
    for(const auto& layer : saved.layers) {
        out.writeInt(layer.info.replace); out.writeInt(layer.coverageStart); out.writeInt(layer.coverageFrames); out.writeInt(layer.captureEnd);
        out.writeInt(int(layer.pre[0].size())); out.writeInt(int(layer.post[0].size()));
        for(const auto* handle : {&layer.pre, &layer.post})
            for(const auto& channel : *handle) for(float sample : channel) out.writeFloat(sample);
    }
    out.writeInt(takeFades.load());out.writeInt(takeFeedback.load());
    for(const auto& layer:saved.layers) {
        out.writeInt(layer.info.ignoreFeedback);out.writeInt(int(layer.info.take));
        out.writeInt(layer.info.part);out.writeInt(layer.info.continued);
    }
    out.writeInt(getRecordingLimit());out.writeInt(getTakeLimit());out.writeInt(getRolling());
    out.writeInt(getDurationSeconds());
    out.writeInt(fixedMode.load());out.writeInt(getFixedBarCount());out.writeInt(getOverdubNextBar());
    for(bool v:{preserveTimeline.load(),playOnBar.load(),stopOnBar.load(),recordPreparation.load(),overdubPreparation.load(),countInClick.load()})out.writeInt(v);
    out.writeInt(countInBars.load());out.writeFloat(triggerDb.load());
    out.writeInt(layerPreviewVisible.load());
    for(int i=0;i<4;++i)out.writeInt(int(packControl(recordingControl(i))));
    for(int i=6;i<8;++i)out.writeInt(mappings[i].load());
    out.writeInt(historySeconds.load());out.writeDouble(historyMinimum.load());out.writeInt(mappings[8].load());
}
void LooperProcessor::setStateInformation (const void* data, int size)
{
    if (size < 24)
        return;
    MemoryInputStream in (data, static_cast<size_t> (size), false);
    if (in.readInt() != 0x454c4c50)
        return;
    const int version = in.readInt();
    if (version < 1 || version > 18)
        return;
    LooperEngine::Snapshot snapshot;
    snapshot.rate = in.readDouble();
    const float savedVolume = in.readFloat();
    snapshot.length = in.readInt();
    if (! std::isfinite (snapshot.rate) || snapshot.rate < 1 || snapshot.rate > 384000 || ! std::isfinite (savedVolume)
        || snapshot.length < 0 || snapshot.length > snapshot.rate * (version>=11?LooperEngine::maxDurationSeconds:60))
        return;
    const int count = version == 1 ? (snapshot.length ? 1 : 0) : in.readInt();
    int live = 1, mute = 0, quantize = 0, grace = 100, passes = 0, bypass = 0, nextBar = 0;
    float savedPan=0,savedFeedback=1; int savedPlayFade=0,savedStopFade=0,savedUndoDepth=1;
    int savedCrossfade=10, savedCaptureTail=10;
    bool savedTakeFades=true,savedTakeFeedback=true;
    int savedRecordings=10,savedParts=4,savedRolling=1,savedDuration=60;
    std::array<int, midiActions> mappingValues { 21, 20, 23, 22, 26, 29, -1, -1, -1 };
    if (version >= 2)
    {
        if (size < (version >= 4 ? 76 : version == 3 ? 72 : 68))
            return;
        live = in.readInt();
        mute = in.readInt();
        quantize = in.readInt();
        grace = in.readInt();
        passes = in.readInt();
        if (version >= 3)
            bypass = in.readInt();
        if (version >= 4) nextBar = in.readInt();
        for (int i=0;i<5;++i) mappingValues[i]=in.readInt();
        // Old sessions may already use CC29: leave the new action unassigned in that case.
        if (version<6) for(int i=0;i<5;++i) if(mappingValues[i]>=0 && (mappingValues[i]&127)==29) mappingValues[5]=-1;
        if (live < 0 || live > 1 || mute < 0 || mute > 1 || quantize < 0 || quantize > 1 || grace < 0 || grace > 500 || passes < 0 || passes > 64 || bypass < 0 || bypass > 1 || nextBar < 0 || nextBar > 1)
            return;
        for (const auto mapping : mappingValues)
            if (mapping != -1 && (mapping < 0 || (mapping & 255) > 127 || (mapping >> 8) > 16))
                return;
        for (int i = 0; i < 5; ++i)
            for (int j = i + 1; j < 5; ++j)
            {
                const int a = mappingValues[i], b = mappingValues[j];
                if (a >= 0 && b >= 0 && (a & 127) == (b & 127) && ((a >> 8) == (b >> 8) || (a >> 8) == 0 || (b >> 8) == 0))
                    return;
            }
    }
    if (count < 0 || count > LooperEngine::maxLayers || (count == 0 && snapshot.length != 0))
        return;
    const int64_t expected = version == 1 ? 24 + static_cast<int64_t> (snapshot.length) * 8 : (version >= 4 ? 76 : version == 3 ? 72 : 68) + count * (12 + static_cast<int64_t> (snapshot.length) * 8);
    const auto baseSize = expected + (version >= 5 ? 16 : 0) + (version>=6 ? 4 : 0) + (version>=7 ? 20+count*8 : 0);
    if (version < 8 ? size != baseSize : size < baseSize + 8 + count * 24)
        return;
    for (int i = 0; i < count; ++i)
    {
        LooperEngine::SavedLayer layer;
        if (version >= 2)
        {
            const int enabled = in.readInt(), solo = in.readInt();
            layer.info.gain = in.readFloat();
            if (enabled < 0 || enabled > 1 || solo < 0 || solo > 1 || ! std::isfinite (layer.info.gain) || layer.info.gain < 0 || layer.info.gain > 2)
                return;
            layer.info.enabled = enabled != 0;
            layer.info.solo = solo != 0;
        }
        for (auto& channel : layer.audio)
        {
            channel.resize (snapshot.length);
            for (auto& sample : channel)
            {
                sample = in.readFloat();
                if (! std::isfinite (sample))
                    return;
            }
        }
        snapshot.layers.push_back (std::move (layer));
    }
    if (version >= 5) {
        snapshot.fixedLength = in.readInt(); snapshot.multiple = in.readInt(); snapshot.phaseOffset = in.readInt64();
        if (snapshot.fixedLength < 0 || snapshot.fixedLength > snapshot.rate * (version>=11?LooperEngine::maxDurationSeconds:60) || snapshot.multiple < 0
            || snapshot.multiple > snapshot.rate * (version>=11?LooperEngine::maxDurationSeconds:60) || ((snapshot.fixedLength == 0) != (snapshot.multiple == 0)) || snapshot.phaseOffset < 0
            || (snapshot.fixedLength == 0 ? snapshot.phaseOffset != 0 : snapshot.phaseOffset >= snapshot.fixedLength)) return;
    }
    if(version>=6) {
        const int value=in.readInt();
        if(value!=-1 && (value<0 || (value&255)>127 || (value>>8)>16)) return;
        for(int i=0;i<5;++i) { const int other=mappingValues[i];
            if(value>=0 && other>=0 && (value&127)==(other&127)
                && ((value>>8)==0 || (other>>8)==0 || (value>>8)==(other>>8))) return;
        }
        mappingValues[5]=value;
    }
    if(version>=7) {
        savedPan=in.readFloat(); savedFeedback=in.readFloat(); savedPlayFade=in.readInt(); savedStopFade=in.readInt(); savedUndoDepth=in.readInt();
        if(!std::isfinite(savedPan) || std::abs(savedPan)>1 || !std::isfinite(savedFeedback) || savedFeedback<0 || savedFeedback>1
            || savedPlayFade<0 || savedPlayFade>10000 || savedStopFade<0 || savedStopFade>10000 || savedUndoDepth<1 || savedUndoDepth>9) return;
        for(auto& layer : snapshot.layers) { layer.info.pan=in.readFloat(); layer.info.feedbackGain=in.readFloat();
            if(!std::isfinite(layer.info.pan) || std::abs(layer.info.pan)>1 || !std::isfinite(layer.info.feedbackGain) || layer.info.feedbackGain<0 || layer.info.feedbackGain>1) return;
        }
    }
    if(version>=8) {
        savedCrossfade=in.readInt(); savedCaptureTail=in.readInt();
        if(savedCrossfade<0 || savedCrossfade>250 || savedCaptureTail<0 || savedCaptureTail>2000) return;
        for(auto& layer : snapshot.layers) {
            if(in.getNumBytesRemaining()<24) return;
            const int replace=in.readInt(); layer.coverageStart=in.readInt(); layer.coverageFrames=in.readInt(); layer.captureEnd=in.readInt();
            const int pre=in.readInt(), post=in.readInt();
            if(replace<0 || replace>1 || layer.coverageStart<0 || layer.coverageStart>=snapshot.length
                || layer.coverageFrames<0 || layer.coverageFrames>snapshot.length
                || layer.captureEnd < -1 || layer.captureEnd>=snapshot.length
                || pre<0 || post<0 || pre>std::ceil(snapshot.rate*0.25) || post>std::ceil(snapshot.rate*2.0)
                || in.getNumBytesRemaining()<int64_t(pre+post)*8) return;
            layer.info.replace=replace!=0;
            for(auto* handle : {&layer.pre,&layer.post}) {
                const int frames=handle==&layer.pre?pre:post;
                for(auto& channel : *handle) {
                    channel.resize(frames);
                    for(auto& sample : channel) { sample=in.readFloat(); if(!std::isfinite(sample)) return; }
                }
            }
        }
        if(version==8 && in.getNumBytesRemaining()!=0) return;
    }
    if(version>=9) {
        if(in.getNumBytesRemaining()!=8+count*16+(version>=10?12:0)+(version>=11?4:0)+(version>=12?12:0)+(version>=13?32:0)+(version>=14?4:0)+(version>=15?24:0)+(version>=17?16:0)) return;
        const int fades=in.readInt(),feedback=in.readInt();
        if(fades<0 || fades>1 || feedback<0 || feedback>1)return;
        savedTakeFades=fades!=0;savedTakeFeedback=feedback!=0;
        for(auto& layer:snapshot.layers) {
            const int ignore=in.readInt(),take=in.readInt(),part=in.readInt(),continued=in.readInt();
            if(ignore<0 || ignore>1 || take<0 || take==INT_MAX || part<0 || part==INT_MAX
                || continued<0 || continued>1 || (take==0 && (part!=0 || continued!=0)))return;
            layer.info.ignoreFeedback=ignore!=0;layer.info.take=uint32_t(take);
            layer.info.part=part;layer.info.continued=continued!=0;
        }
        for(int i=0;i<count;++i)for(int j=0;j<i;++j)
            if(snapshot.layers[i].info.take!=0 && snapshot.layers[i].info.take==snapshot.layers[j].info.take
                && snapshot.layers[i].info.part==snapshot.layers[j].info.part)return;
    }
    if(version>=10) {
        savedRecordings=in.readInt();savedParts=in.readInt();savedRolling=in.readInt();
        if(savedRecordings<1||savedRecordings>LooperEngine::maxRecordings||savedParts<1||savedParts>LooperEngine::maxTakePasses
            ||savedRolling<0||savedRolling>1)return;
    }
    if(version>=11) {
        savedDuration=in.readInt();
        if(savedDuration<1 || savedDuration>LooperEngine::maxDurationSeconds
            || snapshot.length>snapshot.rate*savedDuration || snapshot.fixedLength>snapshot.rate*savedDuration)return;
    }
    int savedFixed=0,savedBars=4,savedODNext=1;
    if(version>=12) {
        savedFixed=in.readInt();savedBars=in.readInt();savedODNext=in.readInt();
        if(savedFixed<0||savedFixed>1||savedBars<1||savedBars>256||savedODNext<0||savedODNext>1
            ||(savedFixed && !quantize))return;
    }
    std::array<int,6> performance {1,0,0,0,0,0};int savedCountIn=2;float savedThreshold=-40;
    if(version>=13) {
        if(in.getNumBytesRemaining()<32)return;
        for(auto& v:performance){v=in.readInt();if(v<0||v>1)return;}
        savedCountIn=in.readInt();savedThreshold=in.readFloat();
        if(savedCountIn<(version>=18?0:1)||savedCountIn>8||!std::isfinite(savedThreshold)||savedThreshold< -80||savedThreshold>0)return;
    }
    int previewVisible=1;
    if(version>=14){previewVisible=in.readInt();if(previewVisible<0||previewVisible>1)return;}
    std::array<RecordingControl,4> controls;
    for(int i=0;i<4;++i){controls[i].enabled=i<2;controls[i].bars=savedCountIn;}
    controls[0].shortAction=1;controls[0].holdAction=3;controls[0].finishFixed=true;
    if(version>=15) {
        for(auto& c:controls){const auto packed=uint32_t(in.readInt());c=unpackControl(packed);
            if(packed>(version>=18?0x7ffffffu:version>=17?0x3ffffffu:version>=16?0xffffffu:16383u) || c.shortAction>1 || c.start>2 || c.bars<0 || c.bars>8 || c.loops>64)return;}
        if(!controls[0].enabled || !controls[1].enabled)return;
        for(int i=6;i<8;++i){mappingValues[i]=in.readInt();const int n=mappingValues[i];
            if(n< -1 || (n>=0 && ((n&255)>127 || (n>>8)>16)))return;
            if(!controls[i-4].enabled && n!=-1)return;
            for(int j=0;j<i;++j)if(n>=0 && mappingValues[j]>=0 && (n&127)==(mappingValues[j]&127)
                && ((n>>8)==0 || (mappingValues[j]>>8)==0 || (n>>8)==(mappingValues[j]>>8)))return;
        }
    }
    if(version<16)for(auto& c:controls){c.loops=passes;c.metronome=performance[5]!=0;c.feedbackWithinTake=savedTakeFeedback;}
    int savedHistory=30,historyMapping=-1;double savedMinimum=1;
    if(version>=17){savedHistory=in.readInt();savedMinimum=in.readDouble();historyMapping=in.readInt();
        if(savedHistory<1 || savedHistory>300 || !std::isfinite(savedMinimum) || savedMinimum<.1 || savedMinimum>10 || savedMinimum>savedHistory
            || historyMapping< -1 || (historyMapping>=0 && ((historyMapping&255)>127 || (historyMapping>>8)>16)))return;
        for(int j=0;j<8;++j)if(historyMapping>=0 && mappingValues[j]>=0 && (historyMapping&127)==(mappingValues[j]&127)
            && ((historyMapping>>8)==0 || (mappingValues[j]>>8)==0 || (historyMapping>>8)==(mappingValues[j]>>8)))return;
    }
    mappingValues[8]=historyMapping;
    std::lock_guard<std::recursive_mutex> snapshotLock(snapshotMutex);
    Exclusive access (*this);
    const double targetRate = preparedRate > 0 ? preparedRate : snapshot.rate;
    resample (snapshot, targetRate);
    engine.restorePrepared(snapshot,targetRate,savedDuration,savedRecordings,savedParts,savedRolling!=0,savedUndoDepth);
    snapshotGate.fetch_add(4);
    recordingLimit.store(savedRecordings);takeLimit.store(savedParts);rollOldest.store(savedRolling!=0);
    undoDepth.store(savedUndoDepth);
    durationSeconds.store(savedDuration);allocatedBytes.store(engine.memoryBytes());
    crossfadeMs.store(savedCrossfade); captureTailMs.store(savedCaptureTail);
    takeFades.store(savedTakeFades);takeFeedback.store(savedTakeFeedback);engine.setTakeOptions(savedTakeFades,savedTakeFeedback);
    pan.store(savedPan); overdubFeedback.store(savedFeedback); playFadeMs.store(savedPlayFade); stopFadeMs.store(savedStopFade);
    *volume = jlimit (0.0f, 1.0f, savedVolume);
    gain.setCurrentAndTargetValue (volume->get());
    bypassed.store (bypass != 0);
    recordNextBar.store (nextBar != 0);
    activeBypass = false;
    monitor.store (live != 0);
    muted.store (mute != 0);
    quantized.store (quantize != 0);
    fixedMode.store(savedFixed!=0);fixedBarCount.store(savedBars);overdubNextBar.store(savedODNext!=0);
    preserveTimeline=performance[0]!=0;playOnBar=performance[1]!=0;stopOnBar=performance[2]!=0;
    recordPreparation=performance[3]!=0;overdubPreparation=performance[4]!=0;countInClick=performance[5]!=0;
    for(int i=0;i<4;++i)recordingControls[i]=packControl(controls[i]);
    currentController=-1;
    {std::lock_guard<std::mutex> lock(historyMutex);history=std::make_shared<LooperHistory>(targetRate,savedHistory);invalidateHistory();historyAllocatedBytes=history->memoryBytes();historyClockValid=false;}
    historySeconds=savedHistory;historyMinimum=savedMinimum;
    countInBars=savedCountIn;triggerDb=savedThreshold;layerPreviewVisible=previewVisible!=0;
    graceMs.store (grace);
    autoPasses.store (passes);
    for (int i = 0; i < midiActions; ++i)
        mappings[i].store (mappingValues[i]);
    cancelGestures();
    commands.reset(); captureRequested.store(0);
    resetGeneration = offeredResetGeneration.load(); // Explicit load supersedes already delivered resets.
    publish();
}
bool LooperProcessor::submitImport(std::unique_ptr<ImportJob> job)
{
    if(importOwned)return false;
    importOwned=std::move(job);importPending.store(importOwned.get(),std::memory_order_release);return true;
}
int LooperProcessor::collectImport()
{
    if(!importOwned)return 0;
    const int result=importOwned->result.load(std::memory_order_acquire);
    if(result!=0)importOwned.reset();return result;
}
void LooperProcessor::consumeImport() noexcept
{
    // A snapshot may still be copying a free slot referenced by old history.
    if(snapshotGate.load() & 2)return;
    if(auto* job=importPending.exchange(nullptr,std::memory_order_acquire)) {
        const bool ok=(job->historyGeneration==0 || job->historyGeneration==historyGeneration.load()) && (job->expectedMode<0 || getLoopMode()==job->expectedMode) && captureRequested.load()==0 && !captureBusy.load() && engine.commitImport(job->plan);
        if(ok && job->switchToFree)setLoopMode(0);
        job->result.store(ok?1:-1,std::memory_order_release);
    }
}
Result LooperProcessor::exportLayer (uint32_t id, const File& destination)
{
    const auto saved = takeSnapshot();
    const auto found = std::find_if(saved.layers.begin(), saved.layers.end(), [id](const auto& layer) { return layer.info.id == id; });
    if (found == saved.layers.end() || saved.length == 0)
        return Result::fail ("Layer no longer exists or has no audio");
    AudioBuffer<float> audio(2, saved.length);
    const double sampleRate = saved.rate;
    for (int c = 0; c < 2; ++c)
        audio.copyFrom(c, 0, found->audio[c].data(), saved.length);
    TemporaryFile temporary (destination);
    std::unique_ptr<OutputStream> stream = temporary.getFile().createOutputStream();
    if (! stream)
        return Result::fail ("Cannot create WAV file");
    WavAudioFormat format;
    auto writer = format.createWriterFor (stream, AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (32).withSampleFormat (AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (! writer)
        return Result::fail ("Cannot create WAV writer");
    if (! writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()))
        return Result::fail ("Cannot write WAV audio");
    writer.reset();
    return temporary.overwriteTargetFileWithTemporary() ? Result::ok() : Result::fail ("Cannot replace destination WAV");
}
namespace {
std::unique_ptr<LooperProcessor::ImportJob> historyPlan(const LooperProcessor::HistorySelection& audio,
    const LooperProcessor::View& v,int first,int last,int recordingLimit,int partLimit)
{
    const int frames=last-first,target=v.importLength>0?v.importLength:frames;
    if(audio.rate!=v.rate || first<0 || last>int(audio.audio[0].size()) || frames<=0 || target<=0
        || target>v.capacity || v.importLength<0 || v.recordings>=recordingLimit)return {};
    const int parts=(frames+target-1)/target;if(parts>partLimit || parts+v.count>LooperEngine::maxLayers)return {};
    auto job=std::make_unique<LooperProcessor::ImportJob>();job->expectedMode=v.loopMode;
    auto& plan=job->plan;plan.rate=v.rate;plan.capacity=v.capacity;plan.previousLength=v.length;plan.target=target;
    plan.mode=v.linkMode;plan.base=v.linkBase;plan.bars=v.requestedBars;
    for(int i=0;i<v.count;++i)plan.ids.push_back(v.layers[i].id);
    plan.parts.resize(size_t(parts));
    for(int i=0;i<parts;++i)for(int c=0;c<2;++c) {
        auto& dst=plan.parts[size_t(i)][c];dst.resize(size_t(v.capacity),0);
        const int n=std::min(target,frames-i*target);
        std::copy_n(audio.audio[c].data()+first+i*target,n,dst.data());
    }
    return job;
}
}
bool LooperProcessor::setHistoryLimits(int seconds,double minimum)
{
    if(seconds<1 || seconds>300 || !std::isfinite(minimum) || minimum<.1 || minimum>10 || minimum>seconds
        || recordingSettingsLocked() || historyRequest.load()!=0 || historyFuture.valid())return false;
    if(seconds==historySeconds.load()){historyMinimum=minimum;return true;}
    try {
        auto replacement=std::make_shared<LooperHistory>(preparedRate>0?preparedRate:44100,seconds);
        Exclusive access(*this);std::lock_guard<std::mutex> lock(historyMutex);
        history=std::move(replacement);invalidateHistory();historyAllocatedBytes=history->memoryBytes();historySyncAvailable=0;historyClockValid=false;historySeconds=seconds;historyMinimum=minimum;
        historyMessage="History cleared after changing its capacity.";return true;
    } catch(...) {return false;}
}
bool LooperProcessor::requestHistory(bool select) noexcept
{
    if(!audioAvailable.load())return false;
    Command c;c.kind=3;c.field=select?1:0;return enqueue(c);
}
void LooperProcessor::triggerHistory(bool select) noexcept
{
    if(!history || activeBypass || engine.capturingNow() || engine.isStartPending() || engine.isEndPending())return;
    int idle=0;if(!historyRequest.compare_exchange_strong(idle,1))return;
    auto& t=historyTicket;t={};t.generation=historyGeneration.load();auto& v=t.view;
    v.rate=engine.getRate();v.capacity=engine.getCapacity();v.loopMode=getLoopMode();
    v.importLength=engine.importTarget();v.length=engine.getLength();v.count=engine.getLayerCount();
    v.recordings=engine.recordingCount();v.linkMode=engine.getLinkMode();v.linkBase=engine.getLinkBase();v.requestedBars=engine.getRequestedBars();
    for(int i=0;i<v.count;++i)v.layers[i]=engine.getLayer(i);
    t.end=history->written.load();t.select=select || v.importLength==0;
    if(v.importLength<0){historyRequest.store(-1);return;}
    if(t.select)t.frames=int(std::min<uint64_t>(t.end,history->window));
    else {
        t.frames=v.importLength;
        // Last complete cycle relative to the looper's shared timeline.
        const int phase=quantized.load()?engine.historyCyclePhase(t.frames):0;
        if(quantized.load() && (!hostPlaying || historySyncAvailable<int64_t(phase)+t.frames)){historyRequest.store(-1);return;}
        if(uint64_t(phase)>t.end){historyRequest.store(-1);return;}
        t.end-=phase;t.alignTimeline=quantized.load();t.timelineOrigin=engine.musicalFrame()-phase-t.frames;
    }
    if(t.frames<int(std::ceil(v.rate*historyMinimum.load())) || t.frames>history->window || uint64_t(t.frames)>t.end) {
        historyRequest.store(-1);return;
    }
    historyRequest.store(2,std::memory_order_release);
}
void LooperProcessor::invalidateHistory() noexcept
{
    ++historyGeneration;
    // A worker owns state 3 until it is collected. Never permit a new ticket
    // to overwrite that request while an old lifecycle is still finishing.
    if(historyRequest.load()!=3)historyRequest=0;
}
void LooperProcessor::timerCallback()
{
    const auto generation=historyGeneration.load();
    if(historyDisplayGeneration!=generation) {
        historyDisplayGeneration=generation;historySelection.reset();++historySelectionVersion;
        historyMessage="History cleared after loading or changing audio settings.";
    }
    if(historyRequest.load()==-1){historyMessage="Capture ignored: not enough history, too short, or invalid loop length.";historyRequest=0;}
    if(historyRequest.load(std::memory_order_acquire)==2 && !historyFuture.valid()) {
        HistoryTicket ticket;std::shared_ptr<LooperHistory> source;
        {std::lock_guard<std::mutex> lock(historyMutex);
            int ready=2;if(!historyRequest.compare_exchange_strong(ready,3))return;
            ticket=historyTicket;source=history;
        }
        const int recordings=getRecordingLimit(),parts=getTakeLimit();
        historyFuture=std::async(std::launch::async,[source,ticket,recordings,parts]{
            HistoryResult result;result.generation=ticket.generation;
            try {
                result.selection=std::make_shared<HistorySelection>();result.selection->generation=ticket.generation;result.selection->rate=source->rate;
                if(!source->copy(ticket.end,ticket.frames,result.selection->audio)) {
                    result.selection.reset();result.error="History expired while copying. Please capture again.";return result;
                }
                if(ticket.select) {
                    auto& wave=*result.selection;
                    for(size_t i=0;i<wave.audio[0].size();++i) {
                        const int bin=int(i*1024/wave.audio[0].size());
                        for(int c=0;c<2;++c){wave.low[bin]=std::min(wave.low[bin],wave.audio[c][i]);wave.high[bin]=std::max(wave.high[bin],wave.audio[c][i]);}
                    }
                } else {
                    result.job=historyPlan(*result.selection,ticket.view,0,ticket.frames,recordings,parts);
                    result.selection.reset();
                    if(result.job){result.job->historyGeneration=ticket.generation;result.job->plan.alignTimeline=ticket.alignTimeline;result.job->plan.timelineOrigin=ticket.timelineOrigin;}
                    if(!result.job)result.error="Capture exceeds recording or layer limits. Nothing changed.";
                }
            }catch(...) {result.selection.reset();result.error="Not enough memory to capture history.";}
            return result;
        });
    }
    if(historyFuture.valid() && historyFuture.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        auto result=historyFuture.get();historyRequest=0;
        if(result.generation!=historyGeneration.load())return;
        if(result.selection){historySelection=result.selection;++historySelectionVersion;historyMessage="Select a range, then Capture selection.";}
        else if(result.job) {
            if(submitImport(std::move(result.job))){historyWaitingImport=true;historyMessage="Applying captured One Take...";}
            else historyMessage="Another import is pending. Please retry Capture.";
        } else historyMessage=result.error;
    }
    if(historyWaitingImport && importOwned && importOwned->result.load()!=0) {
        const int result=collectImport();historyWaitingImport=false;
        historyMessage=result>0?"History captured as a One Take.":"Looper changed or is busy; nothing captured.";
    }
}
bool LooperProcessor::importHistorySelection(int first,int last)
{
    if(!historySelection || historySelection->generation!=historyGeneration.load() || historyFuture.valid() || historyRequest.load()!=0 || hasImport()
        || last-first<int(historySelection->rate*historyMinimum.load()))return false;
    const auto source=historySelection;const auto v=view();const int recordings=getRecordingLimit(),parts=getTakeLimit();
    historyRequest=3;
    historyFuture=std::async(std::launch::async,[source,v,first,last,recordings,parts]{
        HistoryResult result;result.generation=source->generation;
        try{result.job=historyPlan(*source,v,first,last,recordings,parts);if(result.job)result.job->historyGeneration=source->generation;}catch(...){}
        if(!result.job)result.error="Selection exceeds loop or layer limits. Nothing changed.";
        return result;
    });return true;
}
std::shared_ptr<const LooperProcessor::Waveform> LooperProcessor::liveHistoryWaveform()
{
    std::shared_ptr<LooperHistory> source;{std::lock_guard<std::mutex> lock(historyMutex);source=history;}
    if(!source)return {};
    auto wave=std::make_shared<Waveform>();const auto end=source->written.load();const auto frames=std::min<uint64_t>(end,source->window);
    for(int bin=0;bin<1024;++bin)for(int k=0;k<8;++k){
        const auto frame=end-frames+(frames*(bin*8+k))/8192;float l=0,r=0;
        if(frame<end && source->sample(frame,l,r)){wave->low[bin]=std::min({wave->low[bin],l,r});wave->high[bin]=std::max({wave->high[bin],l,r});}
    }
    return wave;
}

String LooperProcessor::status() const
{
    const auto data = view();
    const char* names[] { "Empty", "Recording", "Playing", "Stopped", "Overdub", "Replace" };
    String result = String (names[jlimit (0, 5, data.state)]) + " | " + String (data.rate > 0 ? data.length / data.rate : 0, 2) + " / "+String(getDurationSeconds())+" s | " + String(data.recordings)+"/"+String(getRecordingLimit())+" recordings | "+String(data.count)+" layers";
    if (managerOwner.load() != 0) result = (managerSelected.load() ? "SELECTED | " : "Managed | ") + result;
    if(getLoopMode()==2 && !data.fixedCompatible)result+=" | Fixed bars: check transport, memory limit or master length";
    if (getBypassed())
        result = "BYPASS | " + result;
    if (data.finishingEdit) result += " | Finishing edit...";
    if (data.armed) result += " | Waiting for next bar...";
    if (data.pending)
        result += " | Finishing bar...";
    if (data.needsClock && data.state != 1) result += " | Sync: press Play in Element, or choose Free";
    if (data.recordings >= getRecordingLimit())
        result += " | layer limit: delete before overdub";
    return result;
}
void LooperProcessor::setVolume (float value)
{
    volume->beginChangeGesture();
    volume->setValueNotifyingHost (value);
    volume->endChangeGesture();
}

namespace {
class LooperLook final : public LookAndFeel_V4 {
public:
    LooperLook(){
        setColour(Slider::textBoxBackgroundColourId,Colour(0xff111820));
        setColour(Slider::textBoxOutlineColourId,Colour(0xff485664));
        setColour(Slider::textBoxTextColourId,Colour(0xffdce3e9));
        setColour(Slider::trackColourId,Colour(0xff99b9c7));
        setColour(Slider::thumbColourId,Colour(0xffd4dde5));
        setColour(ToggleButton::tickColourId,Colours::skyblue);
        setColour(ComboBox::backgroundColourId,Colour(0xff202933));
        setColour(ComboBox::outlineColourId,Colour(0xff495c6b));
    }
    void drawButtonText(Graphics& g,TextButton& b,bool over,bool down) override {
        if(!bool(b.getProperties()["recordingCard"])) {LookAndFeel_V4::drawButtonText(g,b,over,down);return;}
        g.setFont(FontOptions(15));
        g.setColour(b.findColour(b.getToggleState()?TextButton::textColourOnId:TextButton::textColourOffId).withMultipliedAlpha(b.isEnabled()?1.0f:.5f));
        g.drawText(b.getButtonText(),b.getLocalBounds().withHeight(44).reduced(3,0),Justification::centred,false);
    }
    void drawToggleButton(Graphics& g,ToggleButton& b,bool over,bool down) override {
        if(!bool(b.getProperties()["compactLayer"])) {LookAndFeel_V4::drawToggleButton(g,b,over,down);return;}
        drawTickBox(g,b,1,5,13,13,b.getToggleState(),b.isEnabled(),over,down);
        g.setColour(Colours::white.withAlpha(b.isEnabled()?1.0f:.4f));g.setFont(FontOptions(11));
        g.drawText(b.getButtonText(),16,0,b.getWidth()-16,b.getHeight(),Justification::centredLeft,false);
    }
    Font getComboBoxFont(ComboBox& b) override {return bool(b.getProperties()["compactLayer"])?Font(FontOptions(11)):LookAndFeel_V4::getComboBoxFont(b);}
    void positionComboBoxText(ComboBox& b,Label& l) override {
        if(!bool(b.getProperties()["compactLayer"])) {LookAndFeel_V4::positionComboBoxText(b,l);return;}
        l.setBounds(2,1,b.getWidth()-14,b.getHeight()-2);l.setFont(getComboBoxFont(b));
    }
    void drawComboBox(Graphics& g,int w,int h,bool down,int x,int y,int bw,int bh,ComboBox& b) override {
        if(!bool(b.getProperties()["compactLayer"])) {LookAndFeel_V4::drawComboBox(g,w,h,down,x,y,bw,bh,b);return;}
        g.setColour(findColour(ComboBox::backgroundColourId));g.fillRoundedRectangle(0,0,float(w),float(h),3);
        g.setColour(findColour(ComboBox::outlineColourId));g.drawRoundedRectangle(.5f,.5f,w-1.f,h-1.f,3,1);
        Path arrow;arrow.startNewSubPath(w-11.f,h*.43f);arrow.lineTo(w-7.f,h*.6f);arrow.lineTo(w-3.f,h*.43f);
        g.setColour(Colours::white);g.strokePath(arrow,PathStrokeType(1));
    }
    Slider::SliderLayout getSliderLayout(Slider& slider) override {
        if(bool(slider.getProperties().getWithDefault("numberOnly",false))) {
            Slider::SliderLayout layout;layout.textBoxBounds=slider.getLocalBounds();return layout;
        }
        return LookAndFeel_V4::getSliderLayout(slider);
    }
    void drawButtonBackground(Graphics& g,Button& b,const Colour& colour,bool over,bool down) override {
        auto area=b.getLocalBounds().toFloat().reduced(1);
        auto fill=Colour(0xff2b3440);
        if(colour==Colours::darkorange)fill=Colour(0xff3c3227);
        if(over)fill=fill.brighter(.12f);if(down)fill=fill.darker(.15f);
        g.setColour(fill);g.fillRoundedRectangle(area,6);
        g.setColour(colour==Colours::darkorange?Colours::orange:Colour(0xff526170));g.drawRoundedRectangle(area,6,1);
    }
    void drawLinearSlider(Graphics& g,int x,int y,int w,int h,float pos,float,float,
                          const Slider::SliderStyle style,Slider& slider) override {
        const bool vertical=style==Slider::LinearVertical;
        if(bool(slider.getProperties().getWithDefault("numberOnly",false)))return;
        const float cx=x+w*.5f,cy=y+h*.5f;
        g.setColour(Colour(0xff11171e));
        if(vertical)g.fillRoundedRectangle(cx-4,float(y),8,float(h),3);
        else g.fillRoundedRectangle(float(x),cy-2.5f,float(w),5,2);
        g.setColour(slider.isEnabled()?Colour(0xff99b9c7):Colour(0xff526170));
        if(vertical)g.fillRoundedRectangle(cx-4,pos,8,std::max(0.0f,y+h-pos),3);
        else g.fillRoundedRectangle(float(x),cy-2.5f,std::max(0.0f,pos-x),5,2);
        g.setColour(slider.isEnabled()?Colour(0xffd4dde5):Colour(0xff758594));
        if(vertical)g.fillRoundedRectangle(cx-16,pos-6,32,12,2);
        else g.fillRoundedRectangle(pos-4,cy-7,8,14,2);
    }
};
class LooperLayerPanel final : public Component {
public:
    std::vector<uint32_t> takes;
    std::vector<Rectangle<int>> cells;
    void paint(Graphics& g) override {
        const int count=std::min(takes.size(),cells.size());
        for(int first=0;first<count;) {
            int last=first;
            while(last+1<count && takes[first]!=0 && takes[last+1]==takes[first]
                && cells[last+1].getX()==cells[first].getX())++last;
            const auto block=cells[first].getUnion(cells[last]);
            const int x=block.getX(),y=block.getY(),height=block.getHeight()-7;
            g.setColour(Colour(0xff28333c));
            g.fillRoundedRectangle(float(x+14),float(y),float(block.getWidth()-18),float(height),4);
            if(takes[first]!=0) {
                g.setColour(Colour(0xff7fa1b5));
                g.fillRect(x+4,y+2,3,height-4);
                // Only close the actual endpoints of a take, not column breaks.
                if(first==0 || takes[first]!=takes[first-1])g.fillRect(x+4,y+2,9,2);
                if(last+1==count || takes[last]!=takes[last+1])g.fillRect(x+4,y+height-4,9,2);
            }
            first=last+1;
        }
    }

};
class LooperActiveButton final : public Button {
public:
    LooperActiveButton():Button("Active"){}
    bool active=false;
    void paintButton(Graphics& g,bool,bool) override {
        g.setFont(FontOptions(15.0f));g.setColour(active?Colours::skyblue:Colours::grey);
        g.drawText("Active",getLocalBounds().withTrimmedRight(22),Justification::centredRight);
        g.fillEllipse(float(getWidth()-13),float(getHeight()/2-5),10,10);
    }
};
using LayerWaveData=LooperProcessor::Waveform;
class LayerWaveform final : public Component, public SettableTooltipClient {
public:
    std::shared_ptr<const LayerWaveData> data;
    double position=0;bool selected=false,audible=true;std::function<void()> onSelect;
    bool rangeEditing=false,dragStart=false;double rangeStart=0,rangeEnd=1;
    std::function<void(double,double)> onRange;
    void mouseDown(const MouseEvent& e) override {
        if(!rangeEditing){if(onSelect)onSelect();return;}
        const double x=double(e.x)/std::max(1,getWidth());dragStart=std::abs(x-rangeStart)<std::abs(x-rangeEnd);mouseDrag(e);
    }
    void mouseDrag(const MouseEvent& e) override {
        if(!rangeEditing)return;const double x=std::clamp(double(e.x)/std::max(1,getWidth()),0.0,1.0);
        if(dragStart)rangeStart=std::min(x,rangeEnd);else rangeEnd=std::max(x,rangeStart);
        if(onRange)onRange(rangeStart,rangeEnd);repaint();
    }
    void paint(Graphics& g) override {
        g.fillAll(Colour(0xff141c25));
        if(data) {
            g.setColour(Colour(0xff93b9c8).withAlpha(audible?.9f:.35f));
            const float mid=getHeight()*.5f,scale=mid-2;
            for(int x=0;x<getWidth();++x) {
                const int first=x*1024/std::max(1,getWidth()),last=std::max(first+1,(x+1)*1024/std::max(1,getWidth()));
                float lo=0,hi=0;for(int i=first;i<std::min(last,1024);++i){lo=std::min(lo,data->low[i]);hi=std::max(hi,data->high[i]);}
                g.drawVerticalLine(x,mid-std::clamp(hi,0.f,1.f)*scale,mid-std::clamp(lo,-1.f,0.f)*scale+.5f);
            }
            g.setColour(Colours::white.withAlpha(audible?.65f:.25f));
            g.drawVerticalLine(int(std::clamp(position,0.0,1.0)*std::max(0,getWidth()-1)),0,float(getHeight()));
        } else {g.setColour(Colours::grey);g.drawText("Select a layer waveform",getLocalBounds(),Justification::centred);}
        if(rangeEditing){g.setColour(Colours::skyblue.withAlpha(.18f));g.fillRect(float(rangeStart*getWidth()),0.f,float((rangeEnd-rangeStart)*getWidth()),float(getHeight()));
            g.setColour(Colours::orange);g.drawVerticalLine(int(rangeStart*(getWidth()-1)),0,float(getHeight()));g.drawVerticalLine(int(rangeEnd*(getWidth()-1)),0,float(getHeight()));}
        if(selected){g.setColour(Colours::skyblue);g.drawRect(getLocalBounds(),1);}
    }
};
using LayerWaves=LooperProcessor::Waveforms;
class RecordingSettings final : public Component, private Timer
{
    struct Card : Component {
        LooperProcessor& owner;int index;
        GroupComponent group;
        std::array<Label,11> labels;
        Label loopUnits,barUnits;
        ComboBox shortAction,holdAction,start,boundary,finish;
        Slider bars,loops;
        ToggleButton roundEnd,hear,metronome,withinTake,stopAfter;
        TextButton toggle;
        Card(LooperProcessor& p,int i):owner(p),index(i) {
            group.setText(i==0?"Record":i==1?"Overdub":"Custom "+String(i-1)+" (no live button)");
            addAndMakeVisible(group);group.setInterceptsMouseClicks(false,false);
            const char* names[]{"Short press:","Hold:","Start:","Count-in:","Count-in metronome:","Recording length:","At loop boundary:","Previous pass:","Loop lengths:","Feedback within One Take:","Stop after recording:"};
            for(int n=0;n<11;++n){labels[n].setText(names[n],dontSendNotification);addAndMakeVisible(labels[n]);}
            shortAction.addItem("Add layers",1);shortAction.addItem("Mute old layers + record",2);
            holdAction.addItem("No action",1);holdAction.addItem("Record",2);holdAction.addItem("Replace while held",3);holdAction.addItem("Clear + fresh recording",4);holdAction.addItem("Record while held",5);
            start.addItem("Now",1);start.addItem("Count-in / Wait for sound",3);
            boundary.addItem("New layer",1);boundary.addItem("Overwrite current layer",2);
            finish.addItem("Until stopped",1);finish.addItem("To loop end",2);finish.addItem("Loop lengths",3);roundEnd.setButtonText("Finish at loop boundary");addAndMakeVisible(roundEnd);roundEnd.setTooltip("After the selected number of full loop lengths, continue to the next end of this looper cycle. If already at the boundary, stop there.");hear.setButtonText("Play while overwriting");
            for(auto* c:std::initializer_list<Component*>{&shortAction,&holdAction,&start,&boundary,&bars,&loops,&finish,&hear,&metronome,&withinTake,&stopAfter,&toggle})addAndMakeVisible(c);
            bars.setRange(1,9,1);loops.setRange(1,64,1);barUnits.setText("1 = Next bar",dontSendNotification);barUnits.setFont(FontOptions(12));addAndMakeVisible(barUnits);
            for(auto* slider:{&bars,&loops}){slider->getProperties().set("numberOnly",true);slider->setTextBoxStyle(Slider::TextBoxLeft,false,64,24);slider->setScrollWheelEnabled(false);}
            loopUnits.setText("loops",dontSendNotification);loopUnits.setFont(FontOptions(12));addAndMakeVisible(loopUnits);
            loops.setTooltip("Record N full loop durations from capture start, not N layers. Finish at loop boundary optionally extends to the next cycle end. Memory limits still apply. Replace while held ends on release.");
            labels[8].setTooltip(loops.getTooltip());
            metronome.setTooltip("Requests the count-in metronome for this controller only. Connect Looper MIDI Out to Count-in Gate; TICK follows the separate headphones audio route.");
            labels[4].setTooltip(metronome.getTooltip());
            stopAfter.setTooltip("When this recording finishes, stop the entire looper, including older layers. Other loopers and live monitoring are unchanged. This setting stays enabled until you change it.");
            labels[10].setTooltip(stopAfter.getTooltip());
            shortAction.setTooltip("Add layers keeps older layers audible. Mute old layers + record disables them but keeps them for Undo. Both use this controller's timing and capture settings; another short press finishes recording.");
            holdAction.setTooltip("No action: ignore holds. Record: start this controller's Record action; release does not stop it. Replace while held: replace older audio until release. Clear + fresh recording: clear and establish a new loop. Record while held: add audio until release. Both held modes recover input from the original press and bypass Next bar/count-in.");
            withinTake.setTooltip("Allows feedback to fade earlier layers from this same One Take. When off, only earlier recordings fade. Ignore feedback on a layer always takes precedence.");
            labels[9].setTooltip(withinTake.getTooltip());
            boundary.setTooltip("New layer starts a new One Take part at each loop wrap. Overwrite progressively replaces only this recording's layer, without adding old and new audio.");
            hear.setTooltip("Monitor the previous pass while replacing it. This never mixes it into the new recording. Other layers keep playing normally.");
            start.setTooltip("Count-in is used in Sync/Fixed bars. Free mode waits for the shared sound threshold. The first Fixed bars capture always starts on a bar boundary.");
            finish.setTooltip("At the fixed loop length, finish recording. When off, continue capturing according to At loop boundary.");
            bars.setTooltip("Count upcoming bar boundaries: 1 = Next bar, 2 = one full bar later. Only Sync/Fixed. The display includes the unfinished current bar; timing is unchanged for saved sessions.");
            start.setTooltip("Now starts without preparation; the first Fixed capture still aligns to a bar. Count-in counts upcoming boundaries (1 = Next bar). In Free, Wait for sound uses the shared threshold. Held Add/Replace bypass preparation; short presses use it.");
            finish.setTooltip("Until stopped: finish manually. To loop end: finish at the next end of this looper cycle. Loop lengths: record N full durations from capture start, optionally rounded to the cycle boundary. Unknown initial length must first be established manually. Stop after recording separately controls playback.");
            toggle.setTooltip("Enable or remove this Custom recording profile. Enabling starts with Overdub-like defaults. Removing the profile does not delete recorded audio. Assign MIDI in the MIDI tab, or select the profile in a Manager.");
            Component* fields[]{&shortAction,&holdAction,&start,&bars,&metronome,&finish,&boundary,&hear,&loops,&withinTake,&stopAfter};
            for(int n=0;n<11;++n)if(auto* client=dynamic_cast<TooltipClient*>(fields[n]))labels[n].setTooltip(client->getTooltip());
            const char* brief[]{"Action for a short press of this recording controller.","Action after holding this MIDI controller for 0.8 seconds.","When this controller begins recording.","Count upcoming boundaries: 1 = Next bar.","Request a click on the separate count-in audio route.","Choose manual completion, the next loop end, or N loop lengths.","Add another layer or overwrite at each loop wrap.","Hear the previous pass while overwriting it.","Record this many full loop lengths from capture start.","Apply feedback to earlier parts of this same One Take.","Stop this looper after the recording finishes."};
            for(int n=0;n<11;++n){fields[n]->getProperties().set("jgodHelpBrief",brief[n]);labels[n].getProperties().set("jgodHelpBrief",brief[n]);}
            auto changed=[this]{auto c=owner.recordingControl(index);c.shortAction=shortAction.getSelectedId()-1;c.holdAction=holdAction.getSelectedId()-1;
                if(owner.getQuantized() || c.start!=1 || start.getSelectedId()!=1)c.start=start.getSelectedId()==3?2:0;
                if(owner.getQuantized())c.bars=int(bars.getValue())-1;c.loops=finish.getSelectedId()==3?int(loops.getValue()):0;c.finishAtBoundary=roundEnd.getToggleState();c.metronome=metronome.getToggleState();c.feedbackWithinTake=withinTake.getToggleState();c.stopAfter=stopAfter.getToggleState();c.finishFixed=finish.getSelectedId()==2;c.overwrite=boundary.getSelectedId()==2;c.hearPrevious=hear.getToggleState();owner.setRecordingControl(index,c);};
            for(auto* box:{&shortAction,&holdAction,&start,&boundary,&finish})box->onChange=changed;
            bars.onValueChange=changed;loops.onValueChange=changed;roundEnd.onClick=changed;hear.onClick=changed;metronome.onClick=changed;withinTake.onClick=changed;stopAfter.onClick=changed;
            toggle.onClick=[this]{auto c=owner.recordingControl(index);if(c.enabled)c.enabled=false;else {c={};c.enabled=true;}owner.setRecordingControl(index,c);};
        }
        void refresh(bool locked){
            auto c=owner.recordingControl(index);
            const char* holdTips[]{
                "A long press does nothing. Release after a long press does not trigger the short action.",
                "Starts Record using this controller's timing and capture settings. Existing layers are disabled but retained; release does not finish recording.",
                "Replaces older audio in the recorded region until release. Recovers audio from the original press and ignores Next bar/count-in. Existing loop length is retained.",
                "Clears all layers and starts a fresh recording using this controller's timing. Release does not finish recording; a new loop length can be established unless constrained by Fixed/master.",
                "Adds a new recording while held, leaving older layers enabled. Recovers audio from the original press and ignores Next bar/count-in; release finishes with normal fade handles. Fixed/sync lengths are padded with silence when needed."};
            holdAction.setTooltip(holdTips[jlimit(0,4,c.holdAction)]);labels[1].setTooltip(holdAction.getTooltip());
            shortAction.setTooltip(c.shortAction==0?"Adds layers while older layers keep playing. Uses this controller's timing and limits; another short press finishes recording.":"Records a new layer with the existing loop length and disables older layers without deleting them. Uses this controller's timing and limits; another short press finishes recording.");labels[0].setTooltip(shortAction.getTooltip());
            shortAction.setSelectedId(c.shortAction+1,dontSendNotification);holdAction.setSelectedId(c.holdAction+1,dontSendNotification);
            start.changeItemText(3,owner.getQuantized()?"Count-in":"Wait for sound");
            start.setSelectedId(c.start==1?(owner.getQuantized()?3:1):c.start+1,dontSendNotification);boundary.setSelectedId(c.overwrite?2:1,dontSendNotification);
            for(auto pair:{std::pair<Slider*,double>{&bars,c.start==1?1:c.bars+1},{&loops,std::max(1,c.loops)}})if(!pair.first->isMouseButtonDown())pair.first->setValue(pair.second,dontSendNotification);
            stopAfter.setToggleState(c.stopAfter,dontSendNotification);
            finish.setSelectedId(c.finishFixed?2:c.loops>0?3:1,dontSendNotification);roundEnd.setToggleState(c.finishAtBoundary,dontSendNotification);hear.setToggleState(c.hearPrevious,dontSendNotification);
            metronome.setToggleState(c.metronome,dontSendNotification);withinTake.setToggleState(c.feedbackWithinTake,dontSendNotification);
            for(auto* comp:std::initializer_list<Component*>{&shortAction,&holdAction,&start,&boundary,&bars,&loops,&finish,&hear,&metronome,&withinTake,&stopAfter})comp->setEnabled(c.enabled&&!locked);
            hear.setEnabled(c.enabled&&!locked&&c.overwrite);bars.setEnabled(c.enabled&&!locked&&owner.getQuantized()&&c.start!=0);metronome.setEnabled(c.enabled&&!locked&&owner.getQuantized()&&c.start!=0);
            const auto context=owner.view();
            finish.setEnabled(c.enabled&&!locked);loops.setEnabled(c.enabled&&!locked&&!c.finishFixed&&c.loops>0);roundEnd.setEnabled(loops.isEnabled());
            toggle.setVisible(index>=2);toggle.setEnabled(!locked);toggle.setButtonText(c.enabled?"Remove":"Enable profile");
            Component* fields[]{&shortAction,&holdAction,&start,&bars,&metronome,&finish,&boundary,&hear,&loops,&withinTake,&stopAfter};
            for(int n=0;n<11;++n)labels[n].setEnabled(fields[n]->isEnabled());
        }
        void resized() override {
            group.setBounds(getLocalBounds());auto a=getLocalBounds().reduced(10);a.removeFromTop(16);
            if(index>=2){toggle.setBounds(a.removeFromTop(26));a.removeFromTop(4);}
            Component* fields[]{&shortAction,&holdAction,&start,&bars,&metronome,&finish,&boundary,&hear,&loops,&withinTake,&stopAfter};
            for(int n=0;n<11;++n){auto row=a.removeFromTop(28);labels[n].setBounds(row.removeFromLeft(n==4||n==9?225:125));
                if(n==3 || n==8){fields[n]->setBounds(row.removeFromLeft(64).reduced(1));if(n==8){loopUnits.setBounds(row.removeFromLeft(36));roundEnd.setBounds(row);}else barUnits.setBounds(row);}
                else if(n==4 || n==9)fields[n]->setBounds(row.removeFromLeft(26).reduced(1));
                else fields[n]->setBounds(row.reduced(1));}

        }
    };
    LooperProcessor& owner;Viewport viewport;Component body;Label notice;
    std::array<std::unique_ptr<Card>,4> cards;
public:
    explicit RecordingSettings(LooperProcessor& p):owner(p){
        addAndMakeVisible(notice);addAndMakeVisible(viewport);viewport.setViewedComponent(&body,false);
        for(int i=0;i<4;++i){cards[i]=std::make_unique<Card>(p,i);body.addAndMakeVisible(*cards[i]);}startTimerHz(10);timerCallback();
    }
    void resized() override {auto a=getLocalBounds();notice.setBounds(a.removeFromTop(40));viewport.setBounds(a);
        int y=0;for(int i=0;i<4;++i){int h=i<2?344:374;cards[i]->setBounds(0,y,std::max(300,viewport.getWidth()-16),h);y+=h+8;}
        body.setSize(std::max(300,viewport.getWidth()-16),y);}
    void timerCallback() override {const bool locked=owner.recordingSettingsLocked();
        notice.setText(locked?"Finish or cancel recording before changing controllers.":"Custom profiles have no live buttons. Use MIDI or a Manager recording action.",dontSendNotification);
        for(auto& c:cards)c->refresh(locked);}
};
class LooperEditor final : public AudioProcessorEditor, private Timer, private KeyListener
{
public:
    Node editNode;
    std::function<void()> showLayers;
    void collapseLayers() { if (!layersCollapsed) foldLayers.onClick(); }
    Label customNameLabel,customNameCount;TextEditor customName;
    explicit LooperEditor (LooperProcessor& p, bool separateLayers=false) : AudioProcessorEditor (p), owner (p), progressBar (progress), recordingSettings(p), layersOnly(separateLayers)
    {
        setLookAndFeel(&playingLook);
        addAndMakeVisible(importButton);importButton.onClick=[this]{chooseImport();};
        importButton.getProperties().set("jgodHelpBrief","Import an audio file as a recording in this looper.");
        importButton.setTooltip("Import a mono/stereo audio file as a recording. WAV, AIFF, FLAC, MP3 and supported M4A/AAC files are decoded and resampled to the rig rate. The first import defines length unless a fixed/master length constrains it; shorter additions are padded with silence, longer additions become One Take parts. No tempo stretching, beat alignment or extra seam fades are applied. Imported audio is saved with the session.");
        addAndMakeVisible(captureButton);captureButton.setTooltip("Capture [H]: Sync/Fixed saves the last completed cycle; Free saves the latest loop-length interval. Without a known length, freeze history for range selection. Live sound continues.");
        captureButton.onClick=[this]{if(!owner.requestHistory())feedback.setText("Capture unavailable: check the audio device.",dontSendNotification);};
        if(layersOnly) {
            contextHelpPanel.layout(false,{},{});
            addAndMakeVisible(liveWaveButton);addAndMakeVisible(selectHistoryButton);addAndMakeVisible(captureSelectionButton);addAndMakeVisible(rangeStart);addAndMakeVisible(rangeEnd);
            liveWaveButton.setTooltip("Show the rolling live input history. Returning to this view does not affect live audio or existing layers.");
            selectHistoryButton.setTooltip("Freeze available input history for range selection. Adjust Start/End or drag the selection edges, then Capture selection. Live audio continues while the selected history stays fixed.");
            captureSelectionButton.setTooltip("Import the selected history range as a recording. Selections shorter than History minimum are ignored. Existing loop length is preserved; longer audio is split into One Take parts.");
            previewButton.setTooltip("Show or hide the enlarged waveform preview. Select a layer's waveform to inspect it; the drawing uses recorded audio, not its playback gain.");
            liveWaveButton.onClick=[this]{historyView=1;owner.layerPreviewVisible=true;selectedWave.rangeEditing=false;resized();};
            selectHistoryButton.onClick=[this]{owner.requestHistory(true);};
            captureSelectionButton.onClick=[this]{if(owner.historySelection && !owner.importHistorySelection(int(std::llround(rangeStart.getValue()*owner.historySelection->rate)),int(std::llround(rangeEnd.getValue()*owner.historySelection->rate))))feedback.setText("Selection too short or another capture is pending.",dontSendNotification);};
            for(auto* slider:{&rangeStart,&rangeEnd}) {slider->getProperties().set("numberOnly",true);slider->setTextBoxStyle(Slider::TextBoxLeft,false,90,24);slider->setTextValueSuffix(" s");slider->setScrollWheelEnabled(false);}
            rangeStart.textFromValueFunction=[](double v){return "Start "+String(v,3);};rangeEnd.textFromValueFunction=[](double v){return "End "+String(v,3);};
            for(auto* slider:{&rangeStart,&rangeEnd})slider->valueFromTextFunction=[](const String& text){return text.retainCharacters("0123456789.-").getDoubleValue();};
            rangeStart.setTooltip("Selection start, seconds from the beginning of this frozen history.");rangeEnd.setTooltip("Selection end, seconds from the beginning of this frozen history.");
            const auto rangeChanged=[this]{const double duration=historyDuration();if(duration>0){selectedWave.rangeStart=rangeStart.getValue()/duration;selectedWave.rangeEnd=rangeEnd.getValue()/duration;selectedWave.repaint();}};
            rangeStart.onValueChange=rangeChanged;rangeEnd.onValueChange=rangeChanged;
            selectedWave.setTooltip("Enlarged view of the selected layer or input history. In frozen history selection, drag an edge or edit Start/End to choose the captured range. This does not change playback until Capture selection is applied.");
            selectedWave.onRange=[this](double a,double b){rangeStart.setValue(a*historyDuration(),dontSendNotification);rangeEnd.setValue(b*historyDuration(),dontSendNotification);};
            for(Component* c:std::initializer_list<Component*>{&layerStatus,&layerMasterStatus,&layerProgress,&masterProgressBar,&selectedWave,&previewButton})addAndMakeVisible(c);
            layerProgress.setPercentageDisplay(false);masterProgressBar.setPercentageDisplay(false);
            previewButton.onClick=[this]{owner.layerPreviewVisible.store(!owner.layerPreviewVisible.load());resized();};
        }
        if(layersOnly) {
            layerViewport.setViewedComponent(&layerBody,false);addAndMakeVisible(layerViewport);
            addAndMakeVisible(titleLabel);addAndMakeVisible(feedback);
            initializeLayers();setWantsKeyboardFocus(true);setMouseClickGrabsKeyboardFocus(true);
            registerKeyboardListeners(*this);setResizable(true,true);setResizeLimits(440,360,10000,10000);
            setSize(940,680);startTimerHz(15);timerCallback();return;
        }
        for(int i=0;i<2;++i){addAndMakeVisible(stopAfterButtons[i]);stopAfterButtons[i].setButtonText("Stop after");
            stopAfterButtons[i].setTooltip("Stop this looper when recording finishes. Persistent setting for this controller.");
            stopAfterButtons[i].onClick=[this,i]{auto c=owner.recordingControl(i);c.stopAfter=stopAfterButtons[i].getToggleState();owner.setRecordingControl(i,c);};}
        const char* names[] { "Record [S]", "Play / Stop [D]", "Overdub [A]", "Stop", "Undo [Z]", "Clear", "Redo [X]" };
        const LooperEngine::Action actions[] { LooperEngine::Action::record, LooperEngine::Action::playStop, LooperEngine::Action::overdub, LooperEngine::Action::stop, LooperEngine::Action::undo, LooperEngine::Action::clear, LooperEngine::Action::redo };
        for (int i = 0; i < 7; ++i)
        {
            buttons[i].setButtonText (names[i]);
            addAndMakeVisible (buttons[i]);
            buttons[i].onClick = [this, action = actions[i]] { if (! owner.request (action)) feedback.setText ("Command queue full; retry", dontSendNotification); };
        }
        buttons[5].setButtonText(String("Clear [") + String::charToString(0x21e7) + "C]");
        buttons[5].setTooltip("Clear all layers in this looper (Shift+C). Establish a new loop length on the next recording, subject to Fixed bars or master constraints. Use Undo/Redo for take history rather than treating Clear as a playback stop.");
        buttons[1].setTooltip("Toggle this looper's playback (D). General settings choose Play/Stop timing and whether Play keeps alignment or restarts from the beginning. This does not select a different looper or start the rig metronome.");
        buttons[3].setTooltip("Stop this looper without deleting its layers. Playback follows the configured Stop timing and fade.");
        buttons[4].setTooltip("Undo the last supported recording or edit (Z). One Take is undone as a whole, not one pass at a time. Playback stays stopped if it was stopped. History depth is set in General.");
        buttons[6].setTooltip("Redo the last undone operation (X). Playback stays stopped if it was stopped. Recording and pending capture cannot be changed by Redo.");
        buttons[0].setTooltip ("Recording controller: default records a new layer and mutes earlier layers. Configure its actions, timing and loop-boundary behavior in Settings > Recording.");
        buttons[2].setTooltip ("Recording controller: default adds layers; an empty looper starts its first recording. Configure its actions, timing and loop-boundary behavior in Settings > Recording.");
        for (Component* component : std::initializer_list<Component*> { &progressBar, &volumeSlider, &monitorButton, &bypassButton, &muteButton, &endMode, &graceSlider, &help, &feedback, &panSlider, &feedbackSlider, &playFadeSlider, &stopFadeSlider, &undoDepthSlider, &crossfadeSlider, &captureTailSlider })
            addAndMakeVisible (component);
        volumeSlider.setTextBoxStyle (Slider::TextBoxLeft, false, 52, 24);
        volumeSlider.setDoubleClickReturnValue (true, 1.0, ModifierKeys::noModifiers);
        volumeSlider.setTooltip ("Loop playback volume; double-click slider to reset to 1.0");
        graceSlider.setTextBoxStyle (Slider::TextBoxRight, false, 150, 24);
        const auto numericText=[](const String& text){return text.retainCharacters("0123456789.-").getDoubleValue();};
        panSlider.setRange(-1,1,0.01); panSlider.setDoubleClickReturnValue(true,0);
        panSlider.setTextBoxStyle(Slider::TextBoxLeft,false,95,24);
        panSlider.textFromValueFunction=[](double v){ return std::abs(v)<0.005 ? String("Pan: center") : String("Pan: ")+String(int(std::lround(std::abs(v)*100)))+(v<0?" L":" R"); };
        panSlider.valueFromTextFunction=parsePan;
        panSlider.setTooltip("Stereo pan: center preserves stereo; extremes sum both channels to one side. Double-click centers.");
        panSlider.onValueChange=[this]{owner.setPan(float(panSlider.getValue()));};
        recordingLimitSlider.setRange(1,LooperEngine::maxRecordings,1);
        takeLimitSlider.setRange(1,LooperEngine::maxTakePasses,1);
        durationSlider.setRange(1,LooperEngine::maxDurationSeconds,1);
        for(auto* slider:{&recordingLimitSlider,&takeLimitSlider,&durationSlider}) {
            slider->setChangeNotificationOnlyOnRelease(true);
            slider->setTextBoxStyle(Slider::TextBoxLeft,false,165,24);addAndMakeVisible(slider);
        }
        durationSlider.textFromValueFunction=[](double v){return "Max length: "+String(int(v))+" s";};
        durationSlider.valueFromTextFunction=numericText;
        durationSlider.setTooltip("Maximum loop length / reserved seconds per layer (1–300 s). Change while stopped. Existing loops and Undo/Redo must fit.");
        addAndMakeVisible(memoryGroup);memoryGroup.setText("Memory & Limits");memoryGroup.setInterceptsMouseClicks(false,false);memoryGroup.toBack();
        recordingLimitSlider.textFromValueFunction=[](double v){return "Recordings: "+String(int(v));};
        takeLimitSlider.textFromValueFunction=[](double v){return "One Take layers: "+String(int(v));};
        recordingLimitSlider.valueFromTextFunction=numericText;takeLimitSlider.valueFromTextFunction=numericText;
        capacityMode.addItem("Forget oldest",1);capacityMode.addItem("Stop when full",2);
        addAndMakeVisible(capacityMode);addAndMakeVisible(capacityWarning);
        capacityWarning.setFont(FontOptions(12.0f));
        auto capacityChanged=[this]{
            if(!owner.setMemoryLimits(int(durationSlider.getValue()),int(recordingLimitSlider.getValue()),int(takeLimitSlider.getValue()),capacityMode.getSelectedId()!=2))
                feedback.setText("Cannot resize: stop and finish capture; existing loops/Undo must fit and memory must be available.",dontSendNotification);
        };
        durationSlider.onValueChange=capacityChanged;recordingLimitSlider.onValueChange=capacityChanged;takeLimitSlider.onValueChange=capacityChanged;capacityMode.onChange=capacityChanged;
        layerViewport.setViewedComponent(&layerBody,false);layerViewport.setScrollBarsShown(true,false);addAndMakeVisible(layerViewport);
        takeFadesButton.setButtonText("One Take: fade broken joins");
        takeFadesButton.setTooltip("Blend broken joins within a One Take. Applies when a neighbouring part is muted, excluded by Solo or deleted. This is unrelated to linking loopers to a master.");
        takeFadesButton.onClick=[this]{owner.setTakeFades(takeFadesButton.getToggleState());};
        addAndMakeVisible(takeFadesButton);
        feedbackSlider.setRange(0,100,1); feedbackSlider.setTextBoxStyle(Slider::TextBoxLeft,false,125,24);
        feedbackSlider.textFromValueFunction=[](double v){return String("Feedback: ")+String(v,0)+"%";};
        feedbackSlider.setTooltip("Fade eligible older layers during overdub. 75% leaves 75% of their current volume after each full loop, decreasing smoothly; 100% preserves them and 0% fades them over one cycle. Ignore feedback protects a layer; Feedback within One Take is chosen per recording profile.");
        feedbackSlider.onValueChange=[this]{owner.setFeedback(float(feedbackSlider.getValue()/100));};
        for(auto* slider:{&playFadeSlider,&stopFadeSlider}) { slider->setRange(0,10000,1); slider->setSkewFactorFromMidPoint(300); slider->setTextBoxStyle(Slider::TextBoxLeft,false,130,24); }
        playFadeSlider.textFromValueFunction=[](double v){return String("Play fade: ")+String(v,0)+" ms";};
        stopFadeSlider.textFromValueFunction=[](double v){return String("Stop fade: ")+String(v,0)+" ms";};
        playFadeSlider.onValueChange=[this]{owner.setPlayFade(int(playFadeSlider.getValue()));};
        stopFadeSlider.onValueChange=[this]{owner.setStopFade(int(stopFadeSlider.getValue()));};
        crossfadeSlider.setRange(0,250,1); captureTailSlider.setRange(0,2000,1);
        for(auto* slider : {&crossfadeSlider, &captureTailSlider}) {
            slider->setTextBoxStyle(Slider::TextBoxLeft,false,150,24);
            slider->valueFromTextFunction=numericText;
        }
        crossfadeSlider.textFromValueFunction=[](double v){return String("Crossfade: ")+String(v,0)+" ms";};
        captureTailSlider.textFromValueFunction=[](double v){return String("Capture tail: ")+String(v,0)+" ms";};
        crossfadeSlider.setTooltip("Playback blend at recording boundaries; original audio is preserved.");
        captureTailSlider.setTooltip("Continue capturing after Record, Overdub or Replace ends. Captures all input, including new notes. Zero disables it.");
        crossfadeSlider.onValueChange=[this]{owner.setCrossfade(int(crossfadeSlider.getValue()));};
        captureTailSlider.onValueChange=[this]{owner.setCaptureTail(int(captureTailSlider.getValue()));};
        crossfadeSlider.updateText(); captureTailSlider.updateText();
        undoDepthSlider.setRange(1,9,1); undoDepthSlider.setTextBoxStyle(Slider::TextBoxLeft,false,100,24);
        undoDepthSlider.textFromValueFunction=[](double v){return String("Undo: ")+String(v,0);};
        for(auto* slider:{&feedbackSlider,&playFadeSlider,&stopFadeSlider,&undoDepthSlider})slider->valueFromTextFunction=numericText;
        undoDepthSlider.setChangeNotificationOnlyOnRelease(true);
        undoDepthSlider.setTooltip("Undo/redo history depth (1–9). Change while empty or stopped; higher values reserve more audio memory.");
        undoDepthSlider.onValueChange=[this]{owner.setUndoDepth(int(undoDepthSlider.getValue()));};
        for(auto* slider:{&panSlider,&feedbackSlider,&playFadeSlider,&stopFadeSlider,&undoDepthSlider}) slider->updateText();
        volumeSlider.setRange (0, 1, 0.001);
        volumeSlider.onValueChange = [this] { owner.setVolume (static_cast<float> (volumeSlider.getValue())); };
        bypassButton.setButtonText ("Bypass");
        bypassButton.setTooltip ("Pass input unchanged; loop stays in time. Finalize capture immediately; Record/Overdub are disabled. External effects stay active.");
        bypassButton.onClick = [this] { owner.setBypassed (bypassButton.getToggleState()); };
        monitorButton.setTooltip("Hear the live input through this looper. Turning Monitor off does not stop input recording or mute recorded layers. Avoid duplicate live routes if the input is also wired directly to the output.");
        muteButton.setTooltip("Silence recorded loop playback while its position continues. Live monitoring is controlled separately. Unmute returns at the current position.");
        monitorButton.setButtonText ("Monitor");
        monitorButton.onClick = [this] { owner.setMonitor (monitorButton.getToggleState()); };
        muteButton.setButtonText ("Mute");
        muteButton.onClick = [this] { owner.setMuted (muteButton.getToggleState()); };
        endMode.addItem ("Free", 1);
        endMode.addItem ("Sync", 2);
        endMode.addItem ("Fixed bars", 3);
        addAndMakeVisible(fixedBarsSlider);addAndMakeVisible(modeHint);
        fixedBarsSlider.setRange(1,256,1);fixedBarsSlider.setTextBoxStyle(Slider::TextBoxLeft,false,150,24);
        fixedBarsSlider.textFromValueFunction=[](double v){return "Length: "+String(int(v))+" bars";};
        fixedBarsSlider.valueFromTextFunction=numericText;
        fixedBarsSlider.onValueChange=[this]{owner.setFixedBarCount(int(fixedBarsSlider.getValue()));};


        endMode.setTooltip ("Applies to the next new recording. Sync follows the rig clock; existing audio is not time-stretched. Free follows pedal timing exactly.");
        endMode.onChange = [this] { owner.setLoopMode(endMode.getSelectedId()-1); };
        graceSlider.setRange (0, 500, 1);
        graceSlider.setTextValueSuffix (" ms late grace");
        graceSlider.onValueChange = [this] { owner.setGraceMs (static_cast<int> (graceSlider.getValue())); };
        const char* midiNames[] { "Play/Stop / Clear", "Record", "Overdub", "Explicit Stop", "Explicit Play", "Undo / Redo", "Custom 1", "Custom 2", "Capture history" };
        for (int i = 0; i < LooperProcessor::midiActions; ++i)
        {
            midiLabels[i].setText (midiNames[i], dontSendNotification);
            addAndMakeVisible (midiLabels[i]);
            midiCC[i].setRange (-1, 127, 1);
            midiCC[i].setSliderStyle (Slider::IncDecButtons);
            midiCC[i].setTextValueSuffix (" CC (-1 off)");
            midiChannel[i].setRange (0, 16, 1);
            midiChannel[i].setSliderStyle (Slider::IncDecButtons);
            midiChannel[i].setTextValueSuffix (" ch (0 any)");
            for (auto* slider : { &midiCC[i], &midiChannel[i] })
            {
                slider->setTextBoxStyle (Slider::TextBoxLeft, false, 125, 24);
                addAndMakeVisible (slider);
            }
            const auto changed = [this, i] {
                if (! owner.setMidiMapping (i, { static_cast<int> (midiCC[i].getValue()), static_cast<int> (midiChannel[i].getValue()) }))
                    feedback.setText ("MIDI mapping overlaps another action; choose another CC/channel", dontSendNotification);
            };
            midiCC[i].onValueChange = changed;
            midiChannel[i].onValueChange = changed;
        }
        addAndMakeVisible(recordingSettings);addAndMakeVisible(resetDefaults);
        resetDefaults.setTooltip("Restore settings and MIDI assignments after confirmation. Custom profiles are removed, but recorded layers remain. Stop playback and finish capture first; retained audio and Undo history must fit default memory limits.");
        resetDefaults.onClick=[this]{Component::SafePointer<LooperEditor> safe(this);
            AlertWindow::showOkCancelBox(AlertWindow::WarningIcon,"Reset settings?","Restore all looper settings and MIDI assignments to defaults? Custom controllers will be removed. Recorded layers are kept. Stop playback first; retained audio must fit the default memory limits.","Reset","Cancel",this,
                ModalCallbackFunction::create([safe](int result){if(safe && result && !safe->owner.resetSettings())safe->feedback.setText("Reset unavailable: stop recording/playback; retained audio and Undo must fit default limits.",dontSendNotification);}));};
        initializeLayers();
        help.setText("Set each action's MIDI CC and channel. CC -1 disables the assignment; channel 0 accepts all channels. Hover over an action for its press/hold behavior.",dontSendNotification);
        help.setFont (FontOptions (12.0f));
        feedback.setFont (FontOptions (12.0f));
        setWantsKeyboardFocus (true);
        setMouseClickGrabsKeyboardFocus (true);
        generalControls={&endMode,&fixedBarsSlider,&modeHint,&graceSlider,&memoryGroup,&durationSlider,&undoDepthSlider,&recordingLimitSlider,&takeLimitSlider,&capacityMode,&capacityWarning};
        fadeControls={&playFadeSlider,&stopFadeSlider,&crossfadeSlider,&captureTailSlider,&takeFadesButton};
        const char* descriptions[]{"Play Fade: gradually raises loop playback volume when starting.","Stop Fade: gradually lowers loop playback volume when stopping.","Crossfade: blends recording/edit boundaries; preserves the raw recording.","Capture Tail: records input briefly after capture ends, including any new notes; this is not a reverb effect."};
        for(int i=0;i<4;++i){fadeDescriptions[i].setText(descriptions[i],dontSendNotification);addAndMakeVisible(fadeDescriptions[i]);fadeControls.push_back(&fadeDescriptions[i]);}
        for(auto* slider:{&historySecondsSlider,&historyMinSlider}){slider->getProperties().set("numberOnly",true);slider->setTextBoxStyle(Slider::TextBoxLeft,false,64,24);slider->setScrollWheelEnabled(false);addAndMakeVisible(slider);generalControls.push_back(slider);}
        historySecondsSlider.setRange(1,300,1);historyMinSlider.setRange(.1,10,.1);
        historySecondsSlider.setValue(owner.getHistorySeconds(),dontSendNotification);historyMinSlider.setValue(owner.getHistoryMinimum(),dontSendNotification);
        historySecondsLabel.setText("History (s):",dontSendNotification);historyMinLabel.setText("Capture min (s):",dontSendNotification);
        for(auto* label:{&historySecondsLabel,&historyMinLabel}){addAndMakeVisible(label);generalControls.push_back(label);}
        historySecondsSlider.setTooltip("Input history capacity, independent of loop length. Default 30 seconds, maximum 300. More history uses more memory. Changing capacity clears history.");
        historyMinSlider.setTooltip("Ignore Capture selections shorter than this duration. This does not change pedal hold detection or ordinary recording.");
        auto historyLimitsChanged=[this]{if(!owner.setHistoryLimits(int(historySecondsSlider.getValue()),historyMinSlider.getValue()))feedback.setText("Cannot change history limits now.",dontSendNotification);};
        historySecondsSlider.onValueChange=historyLimitsChanged;historyMinSlider.onValueChange=historyLimitsChanged;
        for(int i=0;i<LooperProcessor::midiActions;++i)for(Component* c:{static_cast<Component*>(&midiLabels[i]),static_cast<Component*>(&midiCC[i]),static_cast<Component*>(&midiChannel[i])})midiControls.push_back(c);
        playControls={&volumeSlider,&panSlider,&feedbackSlider,&muteButton,&monitorButton,&bypassButton,&layerViewport};
        for(auto& button:buttons)playControls.push_back(&button);
        addAndMakeVisible(settingsButton);
        settingsButton.onClick=[this]{showingSettings=!showingSettings;
            if(!showingSettings)contextHelpPanel.open=false;
            if(layersCollapsed){setResizeLimits(440,420,1000,1400);setSize(getWidth(),420);}
            resized();};
        const char* tabs[]{"General","Recording","MIDI","Fades"};
        for(int i=0;i<4;++i){settingsTabs[i].setButtonText(tabs[i]);addAndMakeVisible(settingsTabs[i]);settingsTabs[i].onClick=[this,i]{settingsPage=i==1?3:i==2?1:i==3?2:0;settingsViewport.setViewPosition(0,0);resized();};}
        volumeSlider.setSliderStyle(Slider::LinearVertical);feedbackSlider.setSliderStyle(Slider::LinearVertical);
        volumeSlider.setTextBoxStyle(Slider::TextBoxAbove,false,66,30);
        feedbackSlider.setTextBoxStyle(Slider::TextBoxAbove,false,66,30);
        feedbackSlider.textFromValueFunction=[](double v){return String(v,0)+"%";};
        feedbackSlider.updateText();
        panSlider.setTextBoxStyle(Slider::TextBoxRight,false,55,28);
        panSlider.textFromValueFunction=[](double v){return std::abs(v)<0.005?String("C"):(v<0?"L ":"R ")+String(int(std::round(std::abs(v)*100)));};panSlider.updateText();
        syncLine.setPercentageDisplay(false);syncLine.setTextToDisplay("");
        progressBar.setPercentageDisplay(false);progressBar.setTextToDisplay("");
        for(auto* c:std::initializer_list<Component*>{&activeButton,&recordReady,&overdubReady,&titleLabel,&syncLine,&display,&playPosition,&playTiming,&stopTiming,&triggerSlider})addAndMakeVisible(c);
        customName.setTooltip("Name this looper for identification in the rig and Managers. New names are limited to 12 characters. Confirm with Enter or leave the field; an empty name is not applied.");
        customNameLabel.setTooltip(customName.getTooltip());
        customName.setComponentID("looper-name");customNameLabel.setText("Name",dontSendNotification);addAndMakeVisible(customNameLabel);addAndMakeVisible(customName);addAndMakeVisible(customNameCount);
        generalControls.push_back(&customNameCount);generalControls.push_back(&customNameLabel);generalControls.push_back(&customName);
        customName.onTextChange=[this]{if(customName.getText().length()>12)customName.setText(customName.getText().substring(0,12),false);customNameCount.setText(String(customName.getText().length())+"/12",dontSendNotification);};
        customName.onReturnKey=[this]{commitName();};customName.onFocusLost=[this]{commitName();};
        titleLabel.setText("QLooper",dontSendNotification);titleLabel.setFont(FontOptions(15.0f,Font::bold));
        activeButton.onClick=[this]{if(!activeButton.active)owner.activeRequested=true;};
        activeButton.setTooltip("Select this looper for pedal control. Select another looper to transfer control.");
        display.setComponentID("looper-status-display");display.setJustificationType(Justification::centred);display.setFont(FontOptions(19.0f));
        display.setColour(Label::backgroundColourId,Colour(0xff111820));
        display.setColour(Label::outlineColourId,Colour(0xff465564));
        recordReady.onClick=[this]{auto c=owner.recordingControl(0);if(c.start==1)c.bars=0;c.start=recordReady.getToggleState()?2:0;owner.setRecordingControl(0,c);};
        overdubReady.onClick=[this]{auto c=owner.recordingControl(1);if(c.start==1)c.bars=0;c.start=overdubReady.getToggleState()?2:0;owner.setRecordingControl(1,c);};
        playPosition.addItem("Keep alignment",1);playPosition.addItem("From beginning",2);
        playTiming.addItem("Immediately",1);playTiming.addItem("Next bar",2);
        stopTiming.addItem("Immediately",1);stopTiming.addItem("Next bar",2);
        playPosition.onChange=[this]{owner.preserveTimeline=playPosition.getSelectedId()==1;};
        playTiming.onChange=[this]{owner.playOnBar=playTiming.getSelectedId()==2;};
        stopTiming.onChange=[this]{owner.stopOnBar=stopTiming.getSelectedId()==2;};
        triggerSlider.setRange(-80,0,1);triggerSlider.setTextBoxStyle(Slider::TextBoxLeft,false,180,24);
        triggerSlider.textFromValueFunction=[](double v){return "Sound threshold: "+String(int(v))+" dB";};
        triggerSlider.valueFromTextFunction=numericText;triggerSlider.onValueChange=[this]{owner.triggerDb=float(triggerSlider.getValue());};
        for(auto* c:std::initializer_list<Component*>{&playPosition,&playTiming,&stopTiming,&triggerSlider})generalControls.push_back(c);
        const char* labels[]{"Length:","Late grace:","Record:","Overdub:","Count-in:","Sound threshold:","Overdub passes:","Play:","Stop:","Play position:","Max length:","Undo history:","Recordings:","One Take layers:","When full:"};
        for(int i=0;i<15;++i){generalLabels[i].setText(labels[i],dontSendNotification);addAndMakeVisible(generalLabels[i]);generalControls.push_back(&generalLabels[i]);}
        const char* notes[]{"bars","ms","bars\n(Only for Sync and Fixed bars modes)","dB  (Only for Free mode)","0 = Unlimited","seconds per layer","steps"};
        for(int i=0;i<7;++i){generalNotes[i].setText(notes[i],dontSendNotification);generalNotes[i].setFont(FontOptions(12));addAndMakeVisible(generalNotes[i]);generalControls.push_back(&generalNotes[i]);}
        for(auto* slider:{&fixedBarsSlider,&graceSlider,&triggerSlider,&durationSlider,&undoDepthSlider,&recordingLimitSlider,&takeLimitSlider}){
            slider->getProperties().set("numberOnly",true);slider->setScrollWheelEnabled(false);
            slider->setTextBoxStyle(Slider::TextBoxLeft,false,64,24);slider->setTextValueSuffix("");
            slider->textFromValueFunction=[](double v){return String(int(v));};slider->valueFromTextFunction=numericText;slider->updateText();
        }
        fixedBarsSlider.setTooltip("New length applies after clearing the loop. Must fit memory and master synchronization.");
        graceSlider.setTooltip("Sync: stopping within this many milliseconds after a bar boundary trims back to that boundary. Otherwise capture finishes at the next bar.");
        modeHint.setText("Fixed bars sets the loop length. Each recording controller\nchooses whether to stop or continue at this length.",dontSendNotification);
        modeHint.setFont(FontOptions(12));
        recordGroup.setText("Shared recording settings");playGroup.setText("Play and Stop");
        for(auto* group:{&recordGroup,&playGroup}){addAndMakeVisible(group);group->setInterceptsMouseClicks(false,false);group->toBack();generalControls.push_back(group);}
        const auto labelFor=[this](Label& label,const String& text,std::vector<Component*>& group){
            label.setText(text,dontSendNotification);addAndMakeVisible(label);group.push_back(&label);
        };
        labelFor(modeLabel,"Mode:",generalControls);
        const auto tip=[](Label& label,auto& control,const String& text){label.setTooltip(text);control.setTooltip(text);};
        tip(modeLabel,endMode,"Free follows manual timing. Sync aligns recording boundaries to rig bars; Fixed bars also defines the first loop length. Recording profiles choose start timing. Sync and Fixed bars use the rig clock. In Element, starting capture can resume the stopped transport from its current position; playback alone does not. Existing audio is never time-stretched when tempo changes.");
        Component* generalFields[]{&fixedBarsSlider,&graceSlider,nullptr,nullptr,nullptr,&triggerSlider,nullptr,&playTiming,&stopTiming,&playPosition,&durationSlider,&undoDepthSlider,&recordingLimitSlider,&takeLimitSlider,&capacityMode};
        const char* tips[]{
            "Sets a new loop length in Fixed bars mode. Applies when there is no existing loop; clear the loop before defining another length. The result must fit reserved layer capacity and any master relationship. Each recording profile decides whether capture ends or continues at this length.",
            "Allows a slightly late stop to snap back to the preceding bar boundary. A later press finishes recording at the next bar boundary. Used only in Sync mode.",
            "Chooses when Record starts: now, next bar, or after a count-in. In Free mode, preparation waits for sound instead. Existing layers are muted when a new Record take begins.",
            "Chooses when Overdub starts independently of Record. Overdub adds to existing layers; an empty looper starts its first recording. In Free mode, preparation waits for sound.",
            "Number of full bars to count before recording starts. The count starts at the next bar boundary. Used when Record or Overdub preparation is enabled in Sync or Fixed bars.",
            "Input level in dB that triggers prepared recording in Free mode. A short input history preserves the attack of the triggering sound. A lower value responds to quieter sounds, including noise.",
            "Automatically finishes overdub after this many loop passes. Zero means unlimited passes; memory and One Take limits still apply.",
            "Starts playback immediately or waits for the next bar. Available in Sync and Fixed bars modes. Press again to cancel a pending start.",
            "Stops playback immediately or at the next bar. Available in Sync and Fixed bars modes. Press again to cancel a pending stop.",
            "Keep alignment resumes at the current synchronized position. From beginning restarts at the start of the loop. Play timing separately controls when playback begins.",
            "Maximum loop duration and reserved audio capacity per layer, in seconds. Longer limits use more memory. Change while stopped; existing loops and Undo history must fit.",
            "Number of Undo/Redo steps to retain, from 1 to 9. More steps use more memory. Change while stopped.",
            "Maximum number of recordings in this looper. Each One Take counts as one recording, even if it contains several layers. Higher limits reserve more memory.",
            "Limits the layers in one continuous One Take. New layer mode creates a part at each loop wrap; these parts play together, not in sequence. When full decides whether to stop or forget the oldest part. Overwrite current layer reuses a layer instead of growing this stack.",
            "Stop when full finishes the take at its layer limit. Forget oldest keeps recording and removes the oldest pass from this take. Forgotten passes cannot be restored with Undo."
        };
        for(int i=0;i<15;++i){generalLabels[i].setTooltip(tips[i]);
            if(auto* client=dynamic_cast<SettableTooltipClient*>(generalFields[i]))client->setTooltip(tips[i]);}
        historySecondsLabel.setTooltip(historySecondsSlider.getTooltip());historyMinLabel.setTooltip(historyMinSlider.getTooltip());
        recordReady.setTooltip("Enable this profile's prepared start. In Sync/Fixed bars this uses Count-in; in Free it waits for the shared sound threshold. Configure the profile in Recording settings.");
        overdubReady.setTooltip(recordReady.getTooltip());
        const char* shortTips[]{"Length of a new loop in Fixed bars mode.","Tolerance for a late recording stop in Sync mode.","Start timing for the Record profile.","Start timing for the Overdub profile.","Full count-in bars for this recording profile.","Input level that triggers Wait for sound in Free mode.","Loop lengths to record; zero means unlimited.","When Play takes effect in Sync/Fixed bars.","When Stop takes effect in Sync/Fixed bars.","Keep the shared position or restart from the beginning.","Maximum seconds per layer; affects memory use.","Undo/Redo history depth; affects memory use.","Maximum recordings; each One Take counts as one.","Maximum layers within one continuous One Take.","Stop at the layer limit or forget the oldest pass."};
        for(int i=0;i<15;++i){if(!generalFields[i])continue;generalFields[i]->getProperties().set("jgodHelpBrief",shortTips[i]);generalLabels[i].getProperties().set("jgodHelpBrief",shortTips[i]);}
        const char* midiTips[]{
            "Short press toggles Play/Stop on release. Hold for 0.8 seconds to clear this looper, deleting all its layers. Send CC 127 on press and 0 on release (64-127 means pressed).",
            "Record controller. Configure short/hold actions in Recording settings. Send CC 127 on press and 0 on release; hold recognition takes 0.8 seconds.",
            "Overdub controller. Configure short/hold actions in Recording settings. Send CC 127 on press and 0 on release; held Replace includes input from the original press.",
            "Stops this looper explicitly. This is separate from Play/Stop and does not clear its layers.",
            "Starts this looper explicitly. This is separate from Play/Stop.",
            "Release a short press for Undo. Hold for 0.8 seconds for Redo instead. Undo/Redo operate on the whole One Take and preserve stopped playback.",
            "Custom recording profile 1. Enable and configure it in Recording settings. Use this MIDI mapping or invoke it from a Manager; no MIDI mapping is required for Manager use.","Custom recording profile 2. Enable and configure it in Recording settings. Use this MIDI mapping or invoke it from a Manager; no MIDI mapping is required for Manager use.", "Capture history on press. With a known length, captures the last cycle; otherwise opens range selection. No hold gesture."
        };
        const char* midiBrief[]{"Play/Stop on short release; hold clears this looper.","MIDI for the configurable Record profile.","MIDI for the configurable Overdub profile.","MIDI command to stop this looper.","MIDI command to play this looper.","Short release: Undo. Long hold: Redo.","MIDI for Custom recording profile 1.","MIDI for Custom recording profile 2.","Capture audio already held in input history."};
        for(int i=0;i<LooperProcessor::midiActions;++i){midiLabels[i].getProperties().set("jgodHelpBrief",midiBrief[i]);midiCC[i].getProperties().set("jgodHelpBrief",midiBrief[i]);midiLabels[i].setTooltip(midiTips[i]);
            midiCC[i].setTooltip(String(midiTips[i])+" CC number: 0-127; -1 disables this action.");
            midiChannel[i].setTooltip("MIDI channel: 1-16. Zero accepts all channels. Assignments must not overlap other actions.");}
        labelFor(fadeWarning,"If your loops sound clean, leave these settings unchanged.",fadeControls);
        fadeWarning.setFont(FontOptions(13));
        const char* fadeNames[]{"Play fade:","Stop fade:","Crossfade:","Capture tail:"};
        const char* fadeUses[]{"When loop playback starts.","When loop playback stops.","At recording and edit boundaries.","Immediately after recording ends."};
        const char* fadeTips[]{
            "Gradually raises loop playback volume when starting. A short fade avoids an abrupt start; a longer fade produces a softer entrance.",
            "Gradually lowers loop playback volume when stopping. A short fade avoids an abrupt cut; a longer fade lets playback fade away.",
            "Blends recording and edit boundaries to reduce clicks. Continuous joins within a One Take are preserved. Raw recorded audio is preserved.",
            "Continues capturing input briefly after recording ends for the boundary transition. It includes any new notes you play during this time. This is not a reverb effect."
        };
        Slider* fades[]{&playFadeSlider,&stopFadeSlider,&crossfadeSlider,&captureTailSlider};
        for(int i=0;i<4;++i){labelFor(fadeLabels[i],fadeNames[i],fadeControls);labelFor(fadeUnits[i],"ms",fadeControls);
            fadeDescriptions[i].setText(fadeUses[i],dontSendNotification);fadeDescriptions[i].setFont(FontOptions(12));
            fades[i]->getProperties().set("numberOnly",true);fades[i]->setScrollWheelEnabled(false);
            fades[i]->setTextBoxStyle(Slider::TextBoxLeft,false,64,24);fades[i]->textFromValueFunction=[](double v){return String(int(v));};fades[i]->updateText();
            tip(fadeLabels[i],*fades[i],fadeTips[i]);fadeDescriptions[i].setTooltip(fadeTips[i]);}
        labelFor(takeFadeLabel,"Fade broken One Take joins",fadeControls);takeFadeLabel.setTooltip(takeFadesButton.getTooltip());takeFadesButton.setButtonText("");
        for(auto* c:std::initializer_list<Component*>{&activeButton,&recordReady,&overdubReady,&titleLabel,&syncLine,&display})playControls.push_back(c);
        addAndMakeVisible(openLayers);addAndMakeVisible(foldLayers);
        openLayers.setTooltip("Open a separate layers window for this looper. It shows waveforms, layer controls, loop progress and input history; it does not change the active pedal target.");
        foldLayers.setTooltip("Hide or show layers below the live controls. Audio and recordings are unchanged; the separate layers window remains available.");
        openLayers.onClick=[this]{openLayerWindow();};
        foldLayers.onClick=[this]{layersCollapsed=!layersCollapsed;foldLayers.setButtonText(layersCollapsed?"Show":"Hide");
            setResizeLimits(440,layersCollapsed?420:780,1000,1400);
            if(layersCollapsed){expandedHeight=getHeight();setSize(getWidth(),420);}
            else setSize(getWidth(),std::max(780,expandedHeight));resized();};
        addAndMakeVisible(settingsViewport);settingsViewport.setViewedComponent(&settingsBody,false);
        settingsViewport.setScrollBarsShown(true,false);
        for(auto* group:{&generalControls,&midiControls,&fadeControls})for(auto* c:*group)settingsBody.addChildComponent(c);
        for(Component* c:std::initializer_list<Component*>{&recordingSettings,&resetDefaults,&help})settingsBody.addChildComponent(c);
        registerKeyboardListeners (*this);
        setResizable(true,true);setResizeLimits(440,layersOnly?240:780,layersOnly?10000:1000,layersOnly?10000:1400);
        setSize (layersOnly?940:460, layersOnly?680:846);
        startTimerHz (15);
        timerCallback();
    }
    static double parsePan(const String& text) {
        if(text.containsIgnoreCase("center"))return 0;
        const auto value=text.retainCharacters("0123456789.-").getDoubleValue()/100.0;
        return text.containsIgnoreCase("L")?-std::abs(value):value;
    }
    void initializeLayers() {
        for (int i = 0; i < LooperEngine::maxLayers; ++i)
        {
            auto& row = rows[i];
            row.name.setText ("Layer " + String (i + 1), dontSendNotification);
            row.wave.setTooltip("Select this layer for the enlarged waveform preview. Waveform height reflects recorded audio, not playback volume or feedback. The position marker follows the loop timeline.");
            row.wave.setComponentID("layer-wave-"+String(i));
            row.wave.onSelect=[this,i]{historyView=0;selectedWave.rangeEditing=false;selectedLayer=rows[i].id;updateWaveSelection();resized();};
            row.enabled.setButtonText ("On");
            row.solo.setButtonText ("Solo");
            row.mode.addItem("Add",1); row.mode.addItem("Replace",2);
            row.mode.setTooltip("Choose Add or Replace for this layer. Add mixes with earlier layers; Replace masks them only in this layer's recorded region. The earlier audio is retained, so this choice can be changed later.");
            row.enabled.setTooltip("Enable or silence this layer without deleting it. The loop position continues; broken One Take joins use the configured fade policy.");
            row.solo.setTooltip("Hear only Solo layers within this looper. Multiple layers can be Solo together. Other loopers are not affected.");
            row.mode.onChange=[this,i]{edit(rows[i].id,5,rows[i].mode.getSelectedId()==2?1.0f:0.0f);};
            row.remove.setTooltip ("Delete this layer only. Other parts of its One Take remain; boundary fades follow the current join settings.");
            row.exportButton.setTooltip ("Export this layer as a WAV file. This is an individual audio layer, not the complete live rig mix; use Recorder for the live mix.");
            DrawablePath trashIcon, downloadIcon;
            trashIcon.setPath (getIcons().farTrashAlt);
            trashIcon.setFill (findColour (Label::textColourId));
            Path arrow, outline;
            arrow.startNewSubPath (10, 2);
            arrow.lineTo (10, 13);
            arrow.startNewSubPath (5, 8);
            arrow.lineTo (10, 13);
            arrow.lineTo (15, 8);
            arrow.startNewSubPath (3, 14);
            arrow.lineTo (3, 18);
            arrow.lineTo (17, 18);
            arrow.lineTo (17, 14);
            PathStrokeType (1.8f, PathStrokeType::curved, PathStrokeType::rounded).createStrokedPath (outline, arrow);
            downloadIcon.setPath (outline);
            downloadIcon.setFill (findColour (Label::textColourId));
            row.remove.setImages (&trashIcon);
            row.exportButton.setImages (&downloadIcon);
            row.remove.setEdgeIndent (7);
            row.exportButton.setEdgeIndent (7);
            row.gain.setRange (0, 2, 0.001);
            row.gain.setTextBoxStyle (Slider::TextBoxRight, false, 61, 27);
            row.gain.setDoubleClickReturnValue (true, 1.0, ModifierKeys::noModifiers);
            row.gain.setTooltip ("Layer volume including feedback. Locked during overdub. Double-click to restore 1.0");
            for (Component* c : std::initializer_list<Component*> { &row.wave, &row.name, &row.mode, &row.enabled, &row.solo, &row.ignoreFeedback, &row.gain, &row.pan, &row.remove, &row.exportButton })
                layerBody.addAndMakeVisible (c);
            for(Component* c:std::initializer_list<Component*>{&row.mode,&row.enabled,&row.solo,&row.ignoreFeedback})c->getProperties().set("compactLayer",true);
            row.ignoreFeedback.setButtonText("No FB");
            row.ignoreFeedback.setTooltip("Protect this layer from overdub feedback. Its current volume is preserved during later overdubs, including within a One Take. This does not restore volume already lost; adjust Layer volume after recording to raise it again.");
            row.ignoreFeedback.onClick=[this,i]{edit(rows[i].id,6,rows[i].ignoreFeedback.getToggleState());};
            row.enabled.onClick = [this, i] { edit (rows[i].id, 0, rows[i].enabled.getToggleState()); };
            row.solo.onClick = [this, i] { edit (rows[i].id, 1, rows[i].solo.getToggleState()); };
            row.pan.setRange(-1,1,0.01); row.pan.setDoubleClickReturnValue(true,0);
            row.pan.setTextBoxStyle(Slider::TextBoxRight,false,61,27);
            row.pan.textFromValueFunction=[](double v){return std::abs(v)<0.005?String("C"):(v<0?"L ":"R ")+String(int(std::lround(std::abs(v)*100)));};
            row.pan.valueFromTextFunction=parsePan;
            row.pan.updateText();
            row.pan.setTooltip("Layer stereo pan; double-click to center");
            row.pan.onValueChange=[this,i]{edit(rows[i].id,4,float(rows[i].pan.getValue()));};
            row.gain.onValueChange = [this, i] { edit (rows[i].id, 2, static_cast<float> (rows[i].gain.getValue())); };
            row.remove.onClick = [this, i] { edit (rows[i].id, 3, 0); };
            row.exportButton.onClick = [this, i] { chooseExport (rows[i].id, i + 1); };
        }
    }
    void commitName(){const auto value=customName.getText().trim();if(editNode.isValid() && value.isNotEmpty())editNode.setProperty(tags::name,value);}
    ~LooperEditor() override
    {
        if(waveFuture.valid())waveFuture.wait();
        if(importFuture.valid())importFuture.wait();
        layerWindow.reset();
        settingsViewport.setViewedComponent(nullptr,false);
        setLookAndFeel(nullptr);
        for (auto& component : keyboardTargets)
            if (component != nullptr)
                component->removeKeyListener (this);
    }
    void focusGained (FocusChangeType) override { suppressHeldKeyboardKeys(); }
    void focusOfChildComponentChanged (FocusChangeType) override
    {
        if (hasKeyboardFocus (true))
            suppressHeldKeyboardKeys();
    }
    void paint (Graphics& graphics) override
    {
        graphics.fillAll(Colour(0xff202731));
        graphics.setColour(frameColour);graphics.drawRoundedRectangle(getLocalBounds().toFloat().reduced(3),10,3);
        if(!showingSettings && !layersOnly){
            graphics.setFont(FontOptions(10.0f));graphics.setColour(Colour(0xffaebcc9));
            graphics.drawText("FEEDBACK",12,57,60,18,Justification::centred);
            graphics.drawText("VOLUME",getWidth()-72,57,60,18,Justification::centred);
            graphics.drawText("PAN",76,320,32,20,Justification::centredLeft);
            graphics.drawText("LAYERS",100,364,getWidth()-240,20,Justification::centred);
        }

    }
    void resized() override
    {
        if(layersOnly) {
            for(auto* c:getChildren())c->setVisible(false);
            const int w=getWidth();
            titleLabel.setVisible(true);titleLabel.setBounds(12,8,80,24);
            titleLabel.setJustificationType(Justification::centredLeft);titleLabel.setText("LAYERS",dontSendNotification);
            importButton.setVisible(true);importButton.setBounds(94,8,62,24);
            previewButton.setVisible(true);previewButton.setBounds(w-110,8,98,24);
            for(auto* button:{&captureButton,&liveWaveButton,&selectHistoryButton})button->setVisible(true);
            const bool wide=w>=760;
            const int cy=wide?8:36, cx=wide?164:12;
            captureButton.setBounds(cx,cy,75,24);liveWaveButton.setBounds(cx+81,cy,95,24);selectHistoryButton.setBounds(cx+182,cy,120,24);
            liveWaveButton.setToggleState(historyView==1,dontSendNotification);
            previewButton.setButtonText(owner.layerPreviewVisible.load()?"Hide preview":"Show preview");
            int y=wide?36:64;
            layerStatus.setVisible(true);layerStatus.setBounds(12,y,(w-24)/2,20);
            layerMasterStatus.setVisible(true);layerMasterStatus.setBounds(w/2,y,w/2-12,20);y+=20;
            layerProgress.setVisible(true);layerProgress.setBounds(12,y,w-24,14);y+=16;
            masterProgressBar.setVisible(true);masterProgressBar.setBounds(12,y,w-24,6);y+=8;
            selectedWave.setVisible(owner.layerPreviewVisible.load());
            if(owner.layerPreviewVisible.load()){selectedWave.setBounds(12,y,w-24,28);y+=32;}
            const bool selecting=historyView==2 && owner.layerPreviewVisible.load();
            captureSelectionButton.setVisible(selecting);rangeStart.setVisible(selecting);rangeEnd.setVisible(selecting);
            if(selecting){rangeStart.setBounds(12,y,100,24);rangeEnd.setBounds(120,y,100,24);captureSelectionButton.setBounds(230,y,150,24);y+=28;}
            layerViewport.setVisible(true);layerViewport.setBounds(12,y,w-24,std::max(20,getHeight()-y-24));
            feedback.setVisible(true);feedback.setBounds(12,getHeight()-23,w-24,20);
            layoutLayers();return;
        }
        contextHelpPanel.layout(showingSettings,{getWidth()-44,12,30,26},{12,getHeight()-192,getWidth()-24,164});
        settingsViewport.setVisible(showingSettings);
        captureButton.setVisible(!showingSettings);
        for(auto& b:stopAfterButtons)b.setVisible(!showingSettings);
        importButton.setVisible(!showingSettings);importButton.setBounds(18,362,70,21);
        openLayers.setVisible(!showingSettings && !showLayers);foldLayers.setVisible(!showingSettings && !showLayers);
        openLayers.setBounds(getWidth()-132,362,70,21);foldLayers.setBounds(getWidth()-58,362,46,21);
        for(auto* c:generalControls)c->setVisible(showingSettings && settingsPage==0);
        for(auto* c:midiControls)c->setVisible(showingSettings && settingsPage==1);
        for(auto* c:fadeControls)c->setVisible(showingSettings && settingsPage==2);
        for(auto* c:playControls)c->setVisible(!showingSettings);
        for(auto& tab:settingsTabs)tab.setVisible(showingSettings);
        recordingSettings.setVisible(showingSettings && settingsPage==3);
        resetDefaults.setVisible(showingSettings && settingsPage==0);
        for(auto* c:std::initializer_list<Component*>{&generalLabels[2],&generalLabels[3],&generalLabels[4],&generalNotes[2],&generalLabels[6],&generalNotes[4]})c->setVisible(false);
        settingsButton.setButtonText(showingSettings?"Back to looper":"Settings");
        help.setVisible(showingSettings && settingsPage==1);
        buttons[3].setVisible(false);
        progressBar.setVisible(!showingSettings);
        auto area=getLocalBounds().reduced(12);
        auto row=area.removeFromTop(32);settingsButton.setBounds(row.withTrimmedRight(showingSettings?38:0).removeFromRight(150));
        area.removeFromTop(32);progressBar.setBounds(area.removeFromTop(22));area.removeFromTop(8);
        feedback.setBounds(area.removeFromBottom(25));if(showingSettings && settingsPage==1)help.setBounds(area.removeFromBottom(95));
        if(showingSettings){
            area=getLocalBounds().reduced(12);area.removeFromTop(40);area.removeFromBottom(25+(contextHelpPanel.open?180:0));
            row=area.removeFromTop(34);for(int i=0;i<4;++i){settingsTabs[i].setBounds(row.removeFromLeft((getWidth()-24)/4).reduced(2));settingsTabs[i].setToggleState(settingsPage==(i==1?3:i==2?1:i==3?2:0),dontSendNotification);}
            area.removeFromTop(10);
            settingsViewport.setBounds(area);
            const int contentHeight=settingsPage==0?580:settingsPage==1?600:settingsPage==2?390:area.getHeight();
            settingsBody.setSize(std::max(300,area.getWidth()-14),std::max(area.getHeight(),contentHeight));
            area=settingsBody.getLocalBounds();
            if(settingsPage==1)help.setBounds(area.removeFromBottom(95));
            if(settingsPage==0){
                row=area.removeFromTop(28);customNameLabel.setBounds(row.removeFromLeft(64));customNameCount.setBounds(row.removeFromRight(50));customName.setBounds(row.reduced(2));
                row=area.removeFromTop(28);
                modeLabel.setBounds(row.removeFromLeft(48));endMode.setBounds(row.removeFromLeft(130).reduced(2));generalLabels[0].setBounds(row.removeFromLeft(58));
                fixedBarsSlider.setBounds(row.removeFromLeft(60).reduced(1,2));generalNotes[0].setBounds(row);
                modeHint.setBounds(area.removeFromTop(44));
                auto recording=area.removeFromTop(84);recordGroup.setBounds(recording);recording=recording.reduced(10);recording.removeFromTop(12);
                const auto field=[&](Rectangle<int> r,int label,Component& c,int note=-1,int width=64){
                    generalLabels[label].setBounds(r.removeFromLeft(126));c.setBounds(r.removeFromLeft(width).reduced(1,2));
                    if(note>=0)generalNotes[note].setBounds(r);
                };
                field(recording.removeFromTop(28),1,graceSlider,1);
                field(recording.removeFromTop(28),5,triggerSlider,3);
                area.removeFromTop(8);auto playback=area.removeFromTop(88);playGroup.setBounds(playback);
                playback=playback.reduced(10);playback.removeFromTop(12);row=playback.removeFromTop(28);
                auto half=row.removeFromLeft(row.getWidth()/2);generalLabels[7].setBounds(half.removeFromLeft(42));playTiming.setBounds(half.reduced(2,1));
                generalLabels[8].setBounds(row.removeFromLeft(42));stopTiming.setBounds(row.reduced(2,1));
                field(playback.removeFromTop(28),9,playPosition,-1,playback.getWidth()-126);
                area.removeFromTop(8);auto memory=area.removeFromTop(244);memoryGroup.setBounds(memory);
                memory=memory.reduced(10);memory.removeFromTop(12);
                field(memory.removeFromTop(27),10,durationSlider,5);
                field(memory.removeFromTop(27),11,undoDepthSlider,6);
                field(memory.removeFromTop(27),12,recordingLimitSlider);
                field(memory.removeFromTop(27),13,takeLimitSlider);
                field(memory.removeFromTop(27),14,capacityMode,-1,memory.getWidth()-126);
                row=memory.removeFromTop(27);historySecondsLabel.setBounds(row.removeFromLeft(88));historySecondsSlider.setBounds(row.removeFromLeft(58));
                historyMinLabel.setBounds(row.removeFromLeft(120));historyMinSlider.setBounds(row.removeFromLeft(58));
                capacityWarning.setBounds(memory);
                area.removeFromTop(10);resetDefaults.setBounds(area.removeFromTop(30));

            }else if(settingsPage==1){
                for(int i:{0,1,2,3,4,5,6,7,8}){midiLabels[i].setBounds(area.removeFromTop(22));row=area.removeFromTop(32);
                    midiCC[i].setBounds(row.removeFromLeft(row.getWidth()/2).reduced(2));midiChannel[i].setBounds(row.reduced(2));}
            }else if(settingsPage==3){recordingSettings.setBounds(area);
            }else{
                fadeWarning.setBounds(area.removeFromTop(50));area.removeFromTop(12);
                Slider* sliders[]{&playFadeSlider,&stopFadeSlider,&crossfadeSlider,&captureTailSlider};
                for(int i=0;i<4;++i){row=area.removeFromTop(30);fadeLabels[i].setBounds(row.removeFromLeft(130));
                    sliders[i]->setBounds(row.removeFromLeft(66).reduced(1,2));fadeUnits[i].setBounds(row.removeFromLeft(32));
                    fadeDescriptions[i].setBounds(area.removeFromTop(26));area.removeFromTop(12);}
                row=area.removeFromTop(30);takeFadeLabel.setBounds(row.removeFromLeft(250));takeFadesButton.setBounds(row.removeFromLeft(26));
            }
            return;
        }
        const int w=getWidth(), left=76, right=w-76, middle=right-left;
        titleLabel.setBounds(16,10,70,26);settingsButton.setBounds(90,10,70,26);activeButton.setBounds(w-126,10,110,26);
        monitorButton.setBounds(left,39,100,28);muteButton.setBounds(left+100,39,75,28);bypassButton.setBounds(left+175,39,95,28);
        feedbackSlider.setBounds(12,78,60,236);volumeSlider.setBounds(w-72,78,60,236);
        progressBar.setBounds(left,78,middle,36);syncLine.setBounds(left,120,middle,4);
        buttons[4].setBounds(left,140,66,30);buttons[6].setBounds(left,176,66,30);
        captureButton.setBounds(right-66,140,66,30);buttons[5].setBounds(right-66,176,66,30);
        display.setBounds(left+72,140,middle-144,66);
        const int bw=(middle-12)/3;
        buttons[2].getProperties().set("recordingCard",true);buttons[0].getProperties().set("recordingCard",true);
        buttons[2].setBounds(left,224,bw,100);buttons[0].setBounds(left+bw+6,224,bw,100);buttons[1].setBounds(left+2*(bw+6),224,middle-2*(bw+6),80);
        recordReady.setBounds(left+bw+8,268,bw-4,25);overdubReady.setBounds(left+2,268,bw-4,25);
        stopAfterButtons[1].setBounds(left+2,295,bw-4,25);stopAfterButtons[0].setBounds(left+bw+8,295,bw-4,25);
        recordReady.toFront(false);overdubReady.toFront(false);
        for(auto& stopAfter:stopAfterButtons)stopAfter.toFront(false);
        panSlider.setBounds(left+32,328,middle-32,28);
        layerViewport.setBounds(12,390,w-24,getHeight()-423);
        layerViewport.setVisible(!layersCollapsed);
        layoutLayers();
    }

private:
    class LayersWindow final : public DocumentWindow {
    public:
        LayersWindow(const String& title,Component* content):DocumentWindow(title,Colour(0xff202731),DocumentWindow::allButtons) {
            setUsingNativeTitleBar(true);setContentOwned(content,true);setResizable(true,false);
            setResizeLimits(440,360,10000,10000);centreWithSize(940,680);setVisible(true);
        }
        void closeButtonPressed() override {setVisible(false);}
    };
    void openLayerWindow() {
        if(showLayers){showLayers();return;}
        if(!layerWindow) {
            auto title=getTopLevelComponent()->getName();if(title.isEmpty())title=owner.getName();
            layerWindow=std::make_unique<LayersWindow>(title+" - Layers",new LooperEditor(owner,true));
        }
        layerWindow->setVisible(true);layerWindow->toFront(true);
    }
    void layoutLayers() {
        const int vw=layerViewport.getWidth();
        // Fixed-size cells; fit complete rows into the current viewport height.
        // Reflow only when height crosses a row boundary or layers change.
        const int slots=layersOnly?std::max(1,(layerViewport.getHeight()-16)/82):LooperEngine::maxLayers;
        const int cw=layersOnly?280:std::max(400,vw-16);
        std::vector<uint32_t> ids;for(int i=0;i<visibleRows;++i)ids.push_back(rows[i].id);
        const bool append=ids.size()>=layoutIDs.size() && std::equal(layoutIDs.begin(),layoutIDs.end(),ids.begin());
        if(!append || layoutSlots!=slots || layoutCellWidth!=cw)layerBody.cells.clear();
        layoutSlots=slots;layoutCellWidth=cw;layoutIDs=ids;
        int col=0,slot=0;
        if(!layerBody.cells.empty()){auto last=layerBody.cells.back();col=last.getX()/cw;slot=last.getY()/82+1;}
        for(int i=int(layerBody.cells.size());i<visibleRows;++i) {
            if(slot>=slots){++col;slot=0;}
            // Fill every row; a take crossing columns is marked by open brackets
            // rather than leaving unused holes in the live overview.
            layerBody.cells.emplace_back(col*cw,slot*82,cw,82);++slot;
        }
        layerBody.setSize(layersOnly?std::max(vw-16,(col+1)*cw):cw,layersOnly?std::max(1,slots*82):std::max(1,visibleRows)*82);
        layerViewport.setScrollBarsShown(true,layersOnly);
        for(int i=0;i<visibleRows;++i){auto& layer=rows[i];auto r=layerBody.cells[i];const int x=r.getX(),y=r.getY()+24,lw=r.getWidth();
            layer.wave.setBounds(x+19,y-22,lw-31,20);
            layer.name.setBounds(0,0,0,0);
            layer.remove.setBounds(x+16,y+1,20,22);layer.exportButton.setBounds(x+16,y+29,20,22);
            layer.gain.setBounds(x+40,y,lw-50,26);
            layer.mode.setBounds(x+40,y+29,52,24);layer.enabled.setBounds(x+92,y+29,36,24);
            layer.solo.setBounds(x+128,y+29,42,24);layer.ignoreFeedback.setBounds(x+170,y+29,54,24);
            layer.pan.getProperties().set("numberOnly",layersOnly);layer.pan.setTextBoxStyle(Slider::TextBoxRight,false,38,24);layer.pan.setBounds(x+224,y+27,lw-234,27);
        }
    }
    struct Row
    {
        uint32_t id = 0, take = 0;
        Label name;
        LayerWaveform wave;
        ComboBox mode;
        ToggleButton enabled, solo, ignoreFeedback;
        Slider gain, pan;
        DrawableButton remove { "Delete layer", DrawableButton::ImageOnButtonBackground };
        DrawableButton exportButton { "Export layer as WAV", DrawableButton::ImageOnButtonBackground };
    };
    void registerKeyboardListeners (Component& component)
    {
        component.addKeyListener (this);
        keyboardTargets.emplace_back (&component);
        for (auto* child : component.getChildren())
            registerKeyboardListeners (*child);
    }
    void suppressHeldKeyboardKeys()
    {
        std::array<bool, 6> down;
        for (int i = 0; i < 6; ++i)
            down[i] = KeyPress::isKeyCurrentlyDown (LooperKeyboard::keys[i]);
        keyboard.suppressHeldKeys (down);
    }
    void releaseKeyboardKeys()
    {
        std::array<bool, 6> down;
        for (int i = 0; i < 6; ++i)
            down[i] = KeyPress::isKeyCurrentlyDown (LooperKeyboard::keys[i]);
        keyboard.releaseUpKeys (down);
    }
    bool keyPressed (const KeyPress& key, Component*) override
    {
        if(key.getKeyCode()=='H' && LooperKeyboard::allows(key,Component::getCurrentlyFocusedComponent(),*this)) {
            if(!captureKeyDown){captureKeyDown=true;owner.requestHistory();}return true;
        }
        const auto result = keyboard.press (key.getKeyCode(), LooperKeyboard::allows (key, Component::getCurrentlyFocusedComponent(), *this), key.getModifiers().isShiftDown());
        if (result.action && ! owner.request (*result.action))
            feedback.setText ("Command queue full; retry", dontSendNotification);
        return result.consumed;
    }
    bool keyStateChanged (bool, Component*) override
    {
        releaseKeyboardKeys();
        return false;
    }
    void edit (uint32_t id, int field, float value)
    {
        if (! owner.editLayer (id, field, value))
            feedback.setText ("Command queue full; retry", dontSendNotification);
    }
    double historyDuration() const {return owner.historySelection?owner.historySelection->audio[0].size()/owner.historySelection->rate:0;}
    void updateHistoryView() {
        if(lastHistoryMessage!=owner.historyMessage){lastHistoryMessage=owner.historyMessage;feedback.setText(lastHistoryMessage,dontSendNotification);
            if(lastHistoryMessage.startsWith("Capture ignored")){displayNotice="Not enough history / too short";displayNoticeUntil=Time::getMillisecondCounterHiRes()+3500;}
        }
        if(seenHistoryVersion!=owner.historySelectionVersion) {
            seenHistoryVersion=owner.historySelectionVersion;
            if(!owner.historySelection){historyView=0;frozenWave.reset();selectedWave.rangeEditing=false;resized();return;}
            if(!layersOnly){openLayerWindow();return;}
            historyView=2;owner.layerPreviewVisible=true;selectedWave.rangeEditing=true;
            const double duration=historyDuration();rangeStart.setRange(0,duration,.001);rangeEnd.setRange(0,duration,.001);
            rangeStart.setValue(0,dontSendNotification);rangeEnd.setValue(duration,dontSendNotification);
            selectedWave.rangeStart=0;selectedWave.rangeEnd=1;
            auto wave=std::make_shared<LayerWaveData>();wave->low=owner.historySelection->low;wave->high=owner.historySelection->high;
            frozenWave=wave;resized();
        }
        if(layersOnly){liveWaveButton.setColour(TextButton::buttonColourId,historyView==1?Colours::darkorange:Colour(0xff2b3440));}
        if(layersOnly && historyView!=0){selectedWave.setName(historyView==1?"Live input history":"Frozen input history");selectedWave.data=historyView==1?owner.liveHistoryWaveform():frozenWave;selectedWave.position=1;selectedWave.audible=true;selectedWave.repaint();}
    }
    void updateWaveSelection() {
        if(layersOnly && historyView!=0)return;
        selectedWave.setName(selectedLayer?"Selected layer "+String(selectedLayer):"No selected layer");
        selectedWave.setComponentID("selected-layer-wave");
        selectedWave.data.reset();
        for(auto& row:rows){row.wave.selected=row.id!=0 && row.id==selectedLayer;
            if(row.wave.selected){selectedWave.data=row.wave.data;selectedWave.audible=row.wave.audible;}row.wave.repaint();}
        selectedWave.repaint();
    }
    void beginImport(const File& file,bool switchToFree=false) {
        const auto view=owner.view();const int recordings=owner.getRecordingLimit(),parts=owner.getTakeLimit();
        feedback.setText("Preparing audio import...",dontSendNotification);
        importFuture=std::async(std::launch::async,[file,view,recordings,parts,switchToFree]{auto result=prepareLooperFile(file,view,recordings,parts);if(result.job)result.job->switchToFree=switchToFree;return result;});
    }
    void chooseImport() {
        if(importFuture.valid() || waitingImport)return;
        chooser=std::make_unique<FileChooser>("Import a layer",File(),"*.wav;*.wave;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.aac");
        Component::SafePointer<LooperEditor> safe(this);
        chooser->launchAsync(FileBrowserComponent::openMode|FileBrowserComponent::canSelectFiles,[safe](const FileChooser& dialog){
            if(!safe || dialog.getResult()==File())return;
            const auto file=dialog.getResult();const auto v=safe->owner.view();
            if(v.count==0 && v.importLength==0 && safe->owner.getLoopMode()==1) {
                AlertWindow::showAsync(MessageBoxOptions().withIconType(MessageBoxIconType::QuestionIcon)
                    .withTitle("Import timing").withMessage("Use Free mode for this file? Keeping Sync does not stretch or align its tempo.")
                    .withButton("Use Free").withButton("Keep Sync").withButton("Cancel"),[safe,file](int choice){
                        if(!safe || choice==0 || choice==3)return;
                        safe->beginImport(file,choice==1);
                    });
            } else safe->beginImport(file);
        });
    }
    void updateLayerAudio(const LooperProcessor::View& data) {
        if(importFuture.valid() && importFuture.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            auto result=importFuture.get();
            if(!result.job)feedback.setText(result.error,dontSendNotification);
            else if(owner.submitImport(std::move(result.job))) {waitingImport=true;feedback.setText("Applying import...",dontSendNotification);}
            else feedback.setText("Another import is pending.",dontSendNotification);
        }
        const int imported=owner.hasHistoryImport()?0:owner.collectImport();
        if(imported!=0){waitingImport=false;feedback.setText(imported>0?"Audio imported; Undo removes this One Take.":"Looper changed or is busy. Nothing imported; please retry.",dontSendNotification);waveSignature.clear();}
        waitingImport=owner.hasImport();
        importButton.setEnabled(!importFuture.valid() && !waitingImport && !data.armed && !data.finishingEdit && data.state!=1 && data.state!=4 && data.state!=5);
        if(waveFuture.valid() && waveFuture.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            try {waves=waveFuture.get();}catch(...) {waves.clear();}
        }
        std::vector<uint32_t> ids;for(int i=0;i<data.count;++i)ids.push_back(data.layers[i].id);
        // Refresh only when contents can have changed, never for gain/pan edits.
        const bool recording=data.state==1 || data.state==4 || data.state==5 || data.finishingEdit;
        const bool changed=ids!=waveSignature || (wasWaveRecording && !recording);
        waveDirty|=changed;
        if(!waveFuture.valid() && !recording && waveDirty) {
            waveSignature=ids;waveDirty=false;
            waveFuture=std::async(std::launch::async,[this]{return owner.waveforms();});
        }
        wasWaveRecording=recording;
        if(selectedLayer!=0 && std::find(ids.begin(),ids.end(),selectedLayer)==ids.end())selectedLayer=0;
        const double pos=data.length>0?double(data.state==3?data.displaySyncPosition:data.position)/data.length:0;
        if(historyView==0)selectedWave.position=pos;
        for(int i=0;i<data.count;++i) {
            auto& wave=rows[i].wave;wave.data.reset();wave.position=pos;
            wave.audible=data.layers[i].enabled && data.state!=3;
            for(const auto& cached:waves)if(cached->id==data.layers[i].id){wave.data=cached;break;}
        }
        updateWaveSelection();
        layerPosition=data.length>0?double(data.position)/data.length:0;
        masterPosition=data.masterLength>0?double(data.displayMasterPosition)/data.masterLength:0;
        const char* states[]{"Empty","Recording","Playing","Stopped","Overdub","Replace"};
        String state=states[jlimit(0,5,data.state)];if(data.armed)state+=" | Waiting to start";if(data.pending)state+=" | Waiting to finish";
        layerStatus.setText(state+"  |  "+String(data.position/std::max(1.0,data.rate),2)+" / "+String(data.length/std::max(1.0,data.rate),2)+" s",dontSendNotification);
        layerProgress.bars=owner.getQuantized() && data.barFrames>0?double(data.length)/data.barFrames:0;layerProgress.beats=std::max(1,data.numerator);
        layerMasterStatus.setText(data.masterLength>0?"Master cycle  |  "+String(data.displayMasterPosition/std::max(1.0,data.rate),2)+" / "+String(data.masterLength/std::max(1.0,data.rate),2)+" s":"Master cycle: not linked",dontSendNotification);
    }
    void chooseExport (uint32_t id, int number)
    {
        chooser = std::make_unique<FileChooser> ("Export raw stereo layer (before gain/mute)", File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Loop layer " + String (number) + ".wav"), "*.wav");
        Component::SafePointer<LooperEditor> safe (this);
        chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting,
                              [safe, id] (const FileChooser& dialog) {
                                  if (safe == nullptr || dialog.getResult() == File())
                                      return;
                                  const auto result = safe->owner.exportLayer (id, dialog.getResult());
                                  safe->feedback.setText (result.wasOk() ? "Layer exported as stereo WAV" : result.getErrorMessage(), dontSendNotification);
                              });
    }
    void timerCallback() override
    {
        if(editNode.isValid() && !customName.hasKeyboardFocus(true))customName.setText(editNode.getName(),false);
        customNameCount.setText(String(customName.getText().length())+"/12",dontSendNotification);
        releaseKeyboardKeys();if(!KeyPress::isKeyCurrentlyDown('H'))captureKeyDown=false;
        updateHistoryView();
        const auto data = owner.view();
        visibleRows=data.count;
        if(!layersOnly) {
        const bool capacityEditable=!owner.recordingSettingsLocked() && (data.state==int(LooperEngine::State::empty)||data.state==int(LooperEngine::State::stopped))&&!data.armed&&!data.finishingEdit;
        durationSlider.setEnabled(capacityEditable);
        if(!historySecondsSlider.isMouseButtonDown())historySecondsSlider.setValue(owner.getHistorySeconds(),dontSendNotification);
        if(!historyMinSlider.isMouseButtonDown())historyMinSlider.setValue(owner.getHistoryMinimum(),dontSendNotification);
        historySecondsSlider.setEnabled(!owner.recordingSettingsLocked());historyMinSlider.setEnabled(!owner.recordingSettingsLocked());
        for(int i=0;i<2;++i){stopAfterButtons[i].setToggleState(owner.recordingControl(i).stopAfter,dontSendNotification);stopAfterButtons[i].setEnabled(!owner.recordingSettingsLocked());}
        if(!durationSlider.isMouseButtonDown())durationSlider.setValue(owner.getDurationSeconds(),dontSendNotification);
        recordingLimitSlider.setEnabled(capacityEditable);takeLimitSlider.setEnabled(capacityEditable);capacityMode.setEnabled(capacityEditable);
        if(!recordingLimitSlider.isMouseButtonDown())recordingLimitSlider.setValue(owner.getRecordingLimit(),dontSendNotification);
        if(!takeLimitSlider.isMouseButtonDown())takeLimitSlider.setValue(owner.getTakeLimit(),dontSendNotification);
        capacityMode.setSelectedId(owner.getRolling()?1:2,dontSendNotification);
        const double reserve=(std::max(data.count,owner.getRecordingLimit()*owner.getTakeLimit())+owner.getUndoDepth()*LooperEngine::maxTakePasses+2)*data.rate*(owner.getDurationSeconds()+2.25)*8/(1024*1024);
        capacityWarning.setText("More / longer layers use more memory.\nPool: ~"+String(reserve,1)+" MiB / allocated: "+String(double(owner.getMemoryBytes())/(1024*1024),1)+" MiB. Change while stopped.",dontSendNotification);
        activeButton.active=owner.pedalSectionActive.load() && (owner.managerOwner.load()==0 || owner.managerSelected.load());
        activeButton.setEnabled(true);activeButton.repaint();
        const bool running=data.state==2||data.state==4||data.state==5;
        frameColour=data.armed||data.state==1||data.state==4||data.state==5?Colours::orange:running?Colours::lightgreen:Colour(0xff657180);
        display.show(looperDisplayStatus(data,owner.audioAvailable.load(),owner.getMuted() || data.momentaryMuted,owner.getBypassed(),owner.triggerDb.load(),
            Time::getMillisecondCounterHiRes()<displayNoticeUntil?displayNotice:String()));
        display.setColour(Label::textColourId,data.armed||running||data.state==1?frameColour:Colour(0xffc9d4df));
        syncLine.setVisible(!layersOnly && !showingSettings && owner.getQuantized());
        syncProgress=data.length>0?double(data.displaySyncPosition)/data.length:0;
        syncLine.setColour(ProgressBar::foregroundColourId,Colours::skyblue);
        const String prepareLabel=owner.getQuantized()?"Count-in":"Wait for sound";
        recordReady.setButtonText(prepareLabel);overdubReady.setButtonText(prepareLabel);
        recordReady.setToggleState((owner.recordingControl(0).start==2 || (owner.getQuantized()&&owner.recordingControl(0).start==1)),dontSendNotification);
        overdubReady.setToggleState((owner.recordingControl(1).start==2 || (owner.getQuantized()&&owner.recordingControl(1).start==1)),dontSendNotification);
        playPosition.setSelectedId(owner.preserveTimeline.load()?1:2,dontSendNotification);
        playTiming.setSelectedId(owner.playOnBar.load()?2:1,dontSendNotification);stopTiming.setSelectedId(owner.stopOnBar.load()?2:1,dontSendNotification);
        for(auto* c:{&playPosition,&playTiming,&stopTiming})c->setEnabled(owner.getQuantized());
        if(!triggerSlider.isMouseButtonDown())triggerSlider.setValue(owner.triggerDb.load(),dontSendNotification);
        triggerSlider.setEnabled(!owner.getQuantized());
        progress = data.length > 0 ? static_cast<double> (data.position) / data.length : 0;
        if (data.state == static_cast<int> (LooperEngine::State::recording))
            progress = data.rate > 0 ? data.length / (data.rate * owner.getDurationSeconds()) : 0;
        if (! volumeSlider.isMouseButtonDown())
            volumeSlider.setValue (owner.getVolume(), dontSendNotification);
        takeFadesButton.setToggleState(owner.getTakeFades(),dontSendNotification);
        bypassButton.setToggleState (owner.getBypassed(), dontSendNotification);
        monitorButton.setEnabled(!owner.getBypassed());muteButton.setEnabled(!owner.getBypassed());
        monitorButton.setToggleState (owner.getMonitor(), dontSendNotification);
        muteButton.setToggleState (owner.getMuted(), dontSendNotification);
        endMode.setSelectedId (owner.getLoopMode()+1, dontSendNotification);
        fixedBarsSlider.setEnabled(owner.getLoopMode()==2 && !owner.recordingSettingsLocked() && !data.armed);
        if(!fixedBarsSlider.isMouseButtonDown())fixedBarsSlider.setValue(owner.getFixedBarCount(),dontSendNotification);
        const bool capturing=data.state==1||data.state==4||data.state==5;
        const bool controllerLocked=owner.recordingSettingsLocked();
        recordReady.setEnabled(!controllerLocked);overdubReady.setEnabled(!controllerLocked);
        resetDefaults.setEnabled(capacityEditable && !controllerLocked);
        for(int i=0;i<LooperProcessor::midiActions;++i){const bool enabled=!controllerLocked && (i<6 || i==8 || owner.recordingControl(i-4).enabled);midiCC[i].setEnabled(enabled);midiChannel[i].setEnabled(enabled);}
        endMode.setEnabled(!controllerLocked && !data.armed);
        const int displayLength=data.captureTarget>0?data.captureTarget:data.length;
        progressBar.bars=owner.getQuantized() && data.barFrames>0 ? double(displayLength)/data.barFrames : 0;
        if(owner.getLoopMode()==2 && data.length==0)progressBar.bars=owner.getFixedBarCount();
        if(data.state==1 && data.captureTarget==0)progressBar.bars=0;
        progressBar.beats=std::max(1,data.numerator);
        if(data.state==1 && data.captureTarget>0)progress=double(data.capturedProgress)/data.captureTarget;

        if(!running && data.state!=1)progress=0;
        graceSlider.setEnabled (owner.getLoopMode()==1);
        if (! graceSlider.isMouseButtonDown())
            graceSlider.setValue (owner.getGraceMs(), dontSendNotification);
        if(!panSlider.isMouseButtonDown()) panSlider.setValue(owner.getPan(),dontSendNotification);
        if(!feedbackSlider.isMouseButtonDown()) feedbackSlider.setValue(owner.getFeedback()*100,dontSendNotification);
        if(!playFadeSlider.isMouseButtonDown()) playFadeSlider.setValue(owner.getPlayFade(),dontSendNotification);
        if(!crossfadeSlider.isMouseButtonDown()) crossfadeSlider.setValue(owner.getCrossfade(),dontSendNotification);
        if(!captureTailSlider.isMouseButtonDown()) captureTailSlider.setValue(owner.getCaptureTail(),dontSendNotification);
        if(!stopFadeSlider.isMouseButtonDown()) stopFadeSlider.setValue(owner.getStopFade(),dontSendNotification);
        if(!undoDepthSlider.isMouseButtonDown()) undoDepthSlider.setValue(owner.getUndoDepth(),dontSendNotification);
        undoDepthSlider.setEnabled(capacityEditable);
        buttons[0].setButtonText ((capturing && data.controller==0) ? "End Record [S]" : "Record [S]");
        buttons[2].setButtonText (capturing && data.controller==1 ? "End Overdub [A]" : "Overdub [A]");
        for (int index : { 0, 2 })
        {
            const bool waiting = (data.armed || data.pending) && data.controller==(index==0?0:1);
            if (waiting) buttons[index].setColour (TextButton::buttonColourId, Colours::darkorange);
            else buttons[index].removeColour (TextButton::buttonColourId);
        }
        buttons[1].setEnabled(owner.audioAvailable.load());
        buttons[0].setEnabled (owner.audioAvailable.load() && ! owner.getBypassed() && !data.finishingEdit);
        buttons[2].setEnabled (owner.audioAvailable.load() && ! owner.getBypassed() && (!data.finishingEdit || data.state==4) && (data.recordings < owner.getRecordingLimit() || data.state == 4));
        buttons[4].setEnabled (data.undo);
        buttons[6].setEnabled (data.redo);
        for (int i = 0; i < LooperProcessor::midiActions; ++i)
        {
            const auto mapping = owner.getMidiMapping (i);
            if (! midiCC[i].isMouseButtonDown())
                midiCC[i].setValue (mapping.cc, dontSendNotification);
            if (! midiChannel[i].isMouseButtonDown())
                midiChannel[i].setValue (mapping.channel, dontSendNotification);
        }
        }
        layerBody.takes.clear();
        for (int i = 0; i < LooperEngine::maxLayers; ++i)
        {
            auto& row = rows[i];
            const bool exists = i < data.count;
            row.id = data.layers[i].id;row.take=data.layers[i].take;
            for (Component* c : std::initializer_list<Component*> { &row.wave, &row.name, &row.mode, &row.enabled, &row.solo, &row.ignoreFeedback, &row.gain, &row.pan, &row.remove, &row.exportButton })
                c->setVisible (exists);
            row.name.setVisible(false);
            if (! exists)
                continue;
            layerBody.takes.push_back(data.layers[i].take);
            row.name.setText(data.layers[i].take!=0
                ? "| Take "+String(data.layers[i].take)+" / "+String(data.layers[i].part+1)
                : "Layer "+String(i+1),dontSendNotification);
            row.ignoreFeedback.setToggleState(data.layers[i].ignoreFeedback,dontSendNotification);
            row.mode.setSelectedId(data.layers[i].replace ? 2 : 1, dontSendNotification);
            row.mode.setEnabled(data.state!=1 && data.state!=4 && data.state!=5 && !data.finishingEdit);
            row.enabled.setToggleState (data.layers[i].enabled, dontSendNotification);
            row.solo.setToggleState (data.layers[i].solo, dontSendNotification);
            if(!row.pan.isMouseButtonDown()) row.pan.setValue(data.layers[i].pan,dontSendNotification);
            const bool volumeLocked = data.state == int(LooperEngine::State::overdubbing)
                || (data.state == int(LooperEngine::State::recording) && data.captureOverdub);
            row.gain.setEnabled(!volumeLocked);
            if (volumeLocked || ! row.gain.isMouseButtonDown())
                row.gain.setValue (data.layers[i].gain, dontSendNotification);
            row.remove.setEnabled (data.state != 1 && data.state != 4 && data.state != 5 && !data.finishingEdit);
        }
        updateLayerAudio(data);
        layoutLayers();
        layerBody.repaint();
        repaint();
    }
    ToggleButton takeFadesButton;
    Slider recordingLimitSlider,takeLimitSlider,durationSlider;
    GroupComponent memoryGroup,recordGroup,playGroup;
    Label modeLabel,fadeWarning,takeFadeLabel;
    std::array<Label,4> fadeLabels,fadeUnits;
    std::array<Label,15> generalLabels;
    std::array<Label,7> generalNotes;
    ComboBox capacityMode;
    Label capacityWarning;
    LooperLayerPanel layerBody;
    Viewport layerViewport;
    int visibleRows=0;
    LooperKeyboard keyboard;
    std::vector<Component::SafePointer<Component>> keyboardTargets;
    LooperProcessor& owner;
    LooperLook playingLook;
    ContextHelp contextHelpPanel {*this};
    Component settingsBody;
    Viewport settingsViewport;
    Colour frameColour {Colours::grey};
    LooperActiveButton activeButton;
    Label titleLabel;LooperStatusDisplay display;
    String displayNotice;double displayNoticeUntil=0;
    ToggleButton recordReady,overdubReady;
    ComboBox playPosition,playTiming,stopTiming;
    Slider triggerSlider;
    double syncProgress=0;
    ProgressBar syncLine {syncProgress};
    double progress = 0;
    MusicalProgress progressBar;
    TextButton captureButton {"Capture [H]"},liveWaveButton {"Live waveform"},selectHistoryButton {"Select from history"},captureSelectionButton {"Capture selection"};
    Slider rangeStart,rangeEnd,historySecondsSlider,historyMinSlider;
    Label historySecondsLabel,historyMinLabel;
    std::array<ToggleButton,2> stopAfterButtons;
    int historyView=0;uint64_t seenHistoryVersion=0;bool captureKeyDown=false;
    String lastHistoryMessage;std::shared_ptr<const LayerWaveData> frozenWave;
    TextButton settingsButton {"Settings"};
    std::array<TextButton,4> settingsTabs;
    RecordingSettings recordingSettings;
    TextButton resetDefaults {"Reset to default settings"};
    bool showingSettings=false;int settingsPage=0;
    std::vector<Component*> generalControls,midiControls,fadeControls,playControls;
    std::array<Label,4> fadeDescriptions;
    Label modeHint;
    Slider fixedBarsSlider;
    std::array<TextButton, 7> buttons;
    Label help, feedback;
    Slider volumeSlider, graceSlider, panSlider, feedbackSlider, playFadeSlider, stopFadeSlider, undoDepthSlider, crossfadeSlider, captureTailSlider;
    ToggleButton monitorButton, muteButton, bypassButton;
    ComboBox endMode;
    std::array<Label, LooperProcessor::midiActions> midiLabels;
    std::array<Slider, LooperProcessor::midiActions> midiCC, midiChannel;
    std::array<Row, LooperEngine::maxLayers> rows;
    bool layersOnly=false,layersCollapsed=false;
    int expandedHeight=846,layoutSlots=0,layoutCellWidth=0;
    std::vector<uint32_t> layoutIDs;
    TextButton openLayers {"Layers..."},foldLayers {"Hide"},importButton {"Import"},previewButton {"Hide preview"};
    Label layerStatus,layerMasterStatus;
    double layerPosition=0,masterPosition=0;
    MusicalProgress layerProgress {layerPosition};
    ProgressBar masterProgressBar {masterPosition};
    LayerWaveform selectedWave;
    uint32_t selectedLayer=0;
    bool waitingImport=false,wasWaveRecording=false,waveDirty=false;
    std::vector<uint32_t> waveSignature;
    LayerWaves waves;
    std::future<LayerWaves> waveFuture;
    std::future<LooperFileImport> importFuture;
    std::unique_ptr<LayersWindow> layerWindow;
    std::unique_ptr<FileChooser> chooser;
};
} // namespace
AudioProcessorEditor* LooperProcessor::createEditor() { return new LooperEditor (*this); }
AudioProcessorEditor* LooperProcessor::createEditorForNode(const Node& node) {auto* editor=new LooperEditor(*this);editor->editNode=node;return editor;}
AudioProcessorEditor* LooperProcessor::createLayersEditorForNode(const Node& node) {auto* editor=new LooperEditor(*this,true);editor->editNode=node;return editor;}
AudioProcessorEditor* LooperProcessor::createCompactEditorForNode(const Node& node,std::function<void()> showLayers) {auto* editor=new LooperEditor(*this);editor->editNode=node;editor->showLayers=std::move(showLayers);editor->collapseLayers();return editor;}
} // namespace element
