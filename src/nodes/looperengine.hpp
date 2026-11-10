// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include <atomic>
#include <type_traits>

namespace element {
/** Stereo layered looper. Allocate only in prepare/load/snapshot, off audio thread.
    Storage capacity is configurable; retained takes and Undo share immutable slots. */
class LooperEngine
{
public:
    static constexpr int maxRecordings = 20, maxTakePasses = 8, maxDurationSeconds = 300;
    static constexpr int maxLayers = maxRecordings*maxTakePasses, maxUndoDepth = 9;
    static constexpr int maxSlots = maxLayers+maxUndoDepth*maxTakePasses+2;
    enum class Action
    {
        record,
        play,
        stop,
        overdub,
        clear,
        undo,
        playStop,
        redo = 10, // 7–9 are reserved for Manager transport MIDI commands.
        replace,
        endReplace,
        freshRecord
    };
    enum class State
    {
        empty,
        recording,
        playing,
        stopped,
        overdubbing,
        replacing
    };
    struct LayerInfo
    {
        uint32_t id = 0;
        bool enabled = true, solo = false, replace = false, ignoreFeedback = false;
        uint32_t take = 0;
        int part = 0;
        bool continued = false;
        float gain = 1, pan = 0, feedbackGain = 1;
    };
    struct SavedLayer
    {
        LayerInfo info;
        std::array<std::vector<float>, 2> audio;
        int coverageStart = 0, coverageFrames = -1, captureEnd = -1;
        std::array<std::vector<float>, 2> pre, post;
    };
    struct Snapshot
    {
        double rate = 0;
        int length = 0, fixedLength = 0, multiple = 0;
        int64_t phaseOffset = 0;
        std::vector<SavedLayer> layers;
    };

    // Prepared off-thread; commit swaps buffers only. The caller retains replaced
    // storage until the callback completes, so no allocation/free occurs there.
    struct ImportPlan {
        bool alignTimeline=false; int64_t timelineOrigin=0;
        double rate=0; int capacity=0, previousLength=0, target=0, mode=0, base=0, bars=0;
        std::vector<uint32_t> ids;
        std::vector<std::array<std::vector<float>,2>> parts;
    };
    int importTarget() const noexcept {
        if(length>0)return length;
        if(fixedLength>0)return fixedLength;
        if(requestedBars>0) {
            const int64_t n=int64_t(clockBar)*requestedBars;
            if(n<=0 || n>capacity || (linkMode>1 && (linkBase<=0 || (linkMode==2?n!=linkBase:n%linkBase!=0))))return -1;
            return int(n);
        }
        if(linkMode>1)return linkBase>0 && linkBase<=capacity?linkBase:-1;
        return 0;
    }
    int64_t musicalFrame() const noexcept {return hostFrame;}
    int historyCyclePhase(int frames) const noexcept {return frames>0?(length>0?getSyncPosition():modulo(linkMode>1?linkFrame-referenceOrigin:hostFrame,frames)):0;}
    int getLinkMode() const noexcept {return linkMode;}
    int getLinkBase() const noexcept {return linkBase;}
    int getRequestedBars() const noexcept {return requestedBars;}
    int masterPosition() const noexcept {const int n=linkMode==1?fixedLength:linkBase;return n>0?modulo(linkFrame-(linkMode==1?ownOrigin:referenceOrigin),n):0;}
    int masterLength() const noexcept {return linkMode==1?fixedLength:linkMode>1?linkBase:0;}
    bool commitImport(ImportPlan& plan) noexcept {
        const int n=int(plan.parts.size());
        if(rate!=plan.rate || capacity!=plan.capacity || length!=plan.previousLength
            || linkMode!=plan.mode || linkBase!=plan.base || requestedBars!=plan.bars
            || plan.target<=0 || plan.target>capacity || n<1 || n>takeLimit
            || n+count>maxLayers || !canAddRecording() || int(plan.ids.size())!=count
            || (state!=State::empty && state!=State::stopped && state!=State::playing)
            || isStartPending() || isEndPending() || isClonePending())return false;
        const int target=importTarget();if(target<0 || (target>0 && target!=plan.target))return false;
        for(int i=0;i<count;++i)if(slots[active[i]].info.id!=plan.ids[size_t(i)])return false;
        for(const auto& part:plan.parts)for(const auto& ch:part)if(int(ch.size())!=capacity)return false;
        // Preflight free slots against the history which preserveForUndo will retain.
        // No state is mutated until every destination is known.
        std::array<int,maxTakePasses> available {};int found=0;
        const int first=undoCursor>=undoDepth?1:0;
        for(int si=0;si<int(slots.size()) && found<n;++si) {
            if(int(slots[si].audio[0].size())!=capacity)continue;
            bool used=false;for(int i=0;i<count;++i)used|=active[i]==si;
            for(int h=first;h<undoCursor;++h)for(int i=0;i<undoHistory[h].count;++i)used|=undoHistory[h].active[i]==si;
            if(!used)available[found++]=si;
        }
        if(found!=n)return false;
        preserveForUndo();
        const bool firstLayer=count==0;const auto take=nextTake++;
        if(firstLayer) {
            length=plan.target;position=0;runFrames=0;state=State::stopped;
            timelineOrigin=plan.alignTimeline?plan.timelineOrigin:hostFrame-(quantized?clockPhase:0);playbackDetached=false;
            if(linkMode>0) {fixedLength=length;multiple=linkMode==1?1:length/linkBase;phaseOffset=0;if(linkMode==1)ownOrigin=plan.alignTimeline?linkFrame-hostFrame+timelineOrigin:linkFrame-(quantized?clockPhase:0);}
        }
        for(int i=0;i<n;++i) {
            const int index=available[i];auto& slot=slots[index];
            slot.audio.swap(plan.parts[size_t(i)]);
            slot.info={};slot.info.id=nextID++;slot.info.take=take;slot.info.part=i;slot.info.continued=i>0;
            slot.start=slot.offset=slot.missingPrefix=0;slot.written=length;slot.captureEnd=-1;
            slot.preFrames=slot.preCopied=slot.postFrames=0;
            slot.renderGain=slot.gainTarget=1;slot.renderPan=0;slot.feedbackAccumulator=1;
            slot.gainRemaining=slot.recordMaskRemaining=slot.feedbackFrames=0;
            active[count++]=index;
        }
        rebuildTakeLinks();return true;
    }

    struct SnapshotLayerView
    {
        LayerInfo info;
        std::array<float*, 2> channels {};
        int start = 0, written = 0, offset = 0, missingPrefix = 0;
        std::array<float*, 2> fallbackChannels {};
        int fallbackOffset = 0;
        int cloneStart = 0, cloneWritten = 0;
        int fallbackCapacity = 0, cloneLength = 0;
        int coverageStart = 0, coverageFrames = -1, captureEnd = -1;
        std::array<float*, 2> pre {}, post {};
        int preFrames = 0, postFrames = 0;
    };
    struct SnapshotView
    {
        double rate = 0;
        int length = 0, capacity = 0, count = 0;
        int fixedLength = 0, multiple = 0;
        int64_t phaseOffset = 0;
        uint64_t epoch = 0;
        std::array<SnapshotLayerView, maxLayers> layers {};
    };
    SnapshotView snapshotView() noexcept
    {
        SnapshotView result;
        result.rate = rate; result.length = initialFixedTarget>0 && !linkedCapture?initialFixedTarget:length; result.capacity = capacity; result.count = count;
        result.fixedLength = fixedLength; result.multiple = multiple; result.phaseOffset = phaseOffset;
        for (int i = 0; i < count; ++i) {
            auto& slot = slots[active[i]];
            result.layers[i] = { slot.info, { slot.audio[0].data(), slot.audio[1].data() },
                                slot.start, slot.written, slot.offset, slot.missingPrefix };
            auto& handles = result.layers[i];
            handles.coverageStart = slot.start == 0 ? slot.missingPrefix : slot.start;
            handles.coverageFrames = slot.written - (slot.start == 0 ? std::min(slot.written,slot.missingPrefix) : 0);
            handles.captureEnd = slot.captureEnd;
            handles.pre = { slot.pre[0].data(), slot.pre[1].data() };
            handles.post = { slot.post[0].data(), slot.post[1].data() };
            handles.preFrames = slot.preCopied == slot.preFrames ? slot.preFrames : 0; handles.postFrames = slot.postFrames;
            if (active[i] == heldBackfillSlot) {
                auto& view=result.layers[i];
                view.fallbackChannels={recent[0].data(),recent[1].data()};
                view.fallbackOffset=heldSourceStart;
                view.fallbackCapacity=int(recent[0].size());
                view.cloneStart=heldStart; view.cloneWritten=heldCopied; view.cloneLength=heldCount;
            }
            for(int j=0;j<heldAddCount;++j)if(heldAdds[j].slot==active[i]) {
                const auto& fill=heldAdds[j];auto& view=result.layers[i];
                view.fallbackChannels={recent[0].data(),recent[1].data()};view.fallbackOffset=fill.source;
                view.fallbackCapacity=int(recent[0].size());view.cloneStart=fill.start;
                view.cloneWritten=std::min(fill.copied,fill.frames);view.cloneLength=fill.frames;
            }

        }
        return result;
    }
    using WriteObserver = void (*) (void*, float*, float) noexcept;
    void setWriteObserver (void* context, WriteObserver observer) noexcept
    { writeContext = context; writeObserver = observer; }

    void prepare (double sampleRate)
    {
        cancelPerformance();hostFrame=timelineOrigin=0;playbackDetached=false;
        heldBackfillSlot=-1;heldAddCount=0;
        postBackfillSlot = -1;
        preCopySlot = postCaptureSlot = -1; recentWrite = recentCount = 0; recordLayer = false; freshUndoPrepared=false;
        rate = sampleRate;
        maxPreFrames = std::max(1, int(rate * 0.250));
        maxPostFrames = std::max(1, int(rate * 2.0));
        maxHeldLookback=std::max(1,int(rate));
        // Independent of host-clock resets. The 1.5-second ring retains the
        // 0.8-second gesture, pre-roll and ample bounded-copy headroom.
        for (auto& channel : recent) channel.assign(std::max(maxPreFrames*2+1,int(rate*1.5)+1), 0);
        for (auto& slot : slots) {

            slot.preFrames = slot.preCopied = slot.postFrames = 0; slot.captureEnd = -1;
            slot.recordMaskRemaining = 0;
        }
        setTransitionOptions(crossfadeMs, captureTailMs);
        rate = sampleRate; panStep = float(1.0 / std::max(1.0, rate * 0.01)); renderPan = panTarget;
        fadeFrames = std::max (0, static_cast<int> (rate * 0.002));
        fadeRemaining = 0;
        transportGain=transportTarget=0; transportRemaining=tailRemaining=0; previouslyAudible=false;
        lastLoop = correction = {};
        declick = false;
        capacity = std::max (1, static_cast<int> (rate * durationSeconds));
        rollingSlot=-1;rollingRemaining=0;
        for(auto& slot:slots) { slot.audio={};slot.pre={};slot.post={}; }
        auto storage=allocateUndoStorage(undoAllocation(undoDepth));
        installUndoStorage(storage,undoDepth);
        undoSize = undoCursor = 0;
        for (auto& channel : history) channel.assign (capacity, 0);
        historyWrite = historyCount = 0;
        fixedLength = multiple = 0; phaseOffset = ownOrigin = 0; linkedCapture = false; linkedEndFrame = -1;
        armed = false;
        count = length = position = 0;
        undoCount = 0;
        undoKind = UndoKind::none;
        state = State::empty;
        pendingEnd = 0;initialFixedTarget=0;rebuildTakeLinks();
    }
    void setOptions (bool quantize, double graceMs, int autoPasses) noexcept
    {
        if(quantized!=quantize){cancelPerformance();armed=false;}
        quantized = quantize;
        grace = std::max (0, static_cast<int> (rate * graceMs / 1000));
        passes = std::clamp (autoPasses, 0, 64);
    }
    struct Storage { std::array<std::vector<float>,2> audio,pre,post; };
    using UndoStorage = std::array<Storage,maxSlots>;
    struct UndoAllocation { std::array<bool,maxSlots> needed {}; int capacity=0,pre=0,post=0; };
    int recordingCount() const noexcept {
        int result=0;
        for(int i=0;i<count;++i) {
            const auto take=slots[active[i]].info.take;bool found=false;
            if(take!=0)for(int j=0;j<i;++j)found|=slots[active[j]].info.take==take;
            if(!found)++result;
        }
        return result;
    }
    bool canAddRecording() const noexcept { return recordingCount()<recordingLimitCount && count<maxLayers; }
    int getRecordingLimit() const noexcept { return recordingLimitCount; }
    int getTakeLimit() const noexcept { return takeLimit; }
    bool getRolling() const noexcept { return rolling; }
    void configureLimits(int recordings,int parts,bool roll) noexcept {
        recordingLimitCount=std::clamp(recordings,1,maxRecordings);
        takeLimit=std::clamp(parts,1,maxTakePasses);rolling=roll;
    }
    UndoAllocation undoAllocation(int depth,int recordings=0,int parts=0,int minimum=0) const noexcept {
        UndoAllocation request; request.capacity=capacity;request.pre=maxPreFrames;request.post=maxPostFrames;
        const int required=std::min(maxSlots,std::max({minimum,count,
            (recordings>0?recordings:recordingLimitCount)*(parts>0?parts:takeLimit)})
            +std::clamp(depth,1,maxUndoDepth)*maxTakePasses+2);
        for(int i=0;i<required;++i)request.needed[i]=int(slots[i].audio[0].size())!=capacity;
        return request;
    }
    static UndoStorage allocateUndoStorage(const UndoAllocation& request) {
        UndoStorage buffers;
        for(int i=0;i<int(buffers.size());++i) if(request.needed[i]) {
            for(auto& channel:buffers[i].audio)channel.resize(request.capacity);
            for(auto& channel:buffers[i].pre)channel.resize(request.pre);
            for(auto& channel:buffers[i].post)channel.resize(request.post);
        }
        return buffers;
    }
    void installUndoStorage(UndoStorage& buffers,int depth) noexcept {
        for(int i=0;i<int(slots.size());++i)if(!buffers[i].audio[0].empty()) {
            slots[i].audio.swap(buffers[i].audio);slots[i].pre.swap(buffers[i].pre);slots[i].post.swap(buffers[i].post);
        }
        undoDepth=std::clamp(depth,1,maxUndoDepth);
        while(undoSize>undoDepth) { for(int i=0;i<undoSize;++i) undoHistory[i]=undoHistory[i+1]; --undoSize; undoCursor=std::max(0,undoCursor-1); }
    }
    int getDurationSeconds() const noexcept { return durationSeconds; }
    void configureDuration(int seconds) noexcept { durationSeconds=std::clamp(seconds,1,maxDurationSeconds); }
    size_t memoryBytes() const noexcept {
        size_t total=0;
        for(const auto& slot:slots)for(const auto* channels:{&slot.audio,&slot.pre,&slot.post})
            for(const auto& c:*channels)total+=c.capacity()*sizeof(float);
        for(const auto* channels:{&history,&recent})for(const auto& c:*channels)total+=c.capacity()*sizeof(float);
        return total;
    }
    // Non-audio lifecycle operation. Caller excludes engine processing; the
    // processor still passes live monitoring through during this configuration.
    // Build a complete replacement first: allocation failure leaves state intact.
    bool resizeMemory(int seconds,int recordings,int parts,bool roll,int depth) {
        if((state!=State::empty && state!=State::stopped) || isStartPending() || isClonePending()
            || tailRemaining>0 || rollingSlot>=0) return false;
        seconds=std::clamp(seconds,1,maxDurationSeconds);
        recordings=std::clamp(recordings,1,maxRecordings);parts=std::clamp(parts,1,maxTakePasses);
        depth=std::clamp(depth,1,maxUndoDepth);
        if(seconds==durationSeconds && recordings==recordingLimitCount && parts==takeLimit && depth==undoDepth) { rolling=roll;return true; }
        if(rate<=0) { configureDuration(seconds);configureLimits(recordings,parts,roll);undoDepth=depth;return true; }
        const int newCapacity=std::max(1,int(rate*seconds));
        auto retainedHistory=undoHistory;int retainedSize=undoSize,retainedCursor=undoCursor;
        while(retainedSize>depth) {
            for(int i=0;i<retainedSize;++i)retainedHistory[i]=retainedHistory[i+1];
            --retainedSize;retainedCursor=std::max(0,retainedCursor-1);
        }
        std::array<int,maxSlots> usedLengths {};
        auto retain=[&](const auto& ids,int n,int frames) {
            for(int i=0;i<n;++i)usedLengths[ids[i]]=std::max(usedLengths[ids[i]],frames);
        };
        retain(active,count,length);
        for(int h=0;h<retainedSize;++h)retain(retainedHistory[h].active,retainedHistory[h].count,retainedHistory[h].length);
        if(retainedCursor<retainedSize)retain(retainedHistory[retainedSize].active,retainedHistory[retainedSize].count,retainedHistory[retainedSize].length);
        if(length>newCapacity || fixedLength>newCapacity) return false;
        for(int n:usedLengths)if(n>newCapacity)return false;
        UndoAllocation allocation;allocation.capacity=newCapacity;allocation.pre=maxPreFrames;allocation.post=maxPostFrames;
        int used=0;for(int i=0;i<maxSlots;++i)if(usedLengths[i]>0){allocation.needed[i]=true;++used;}
        const int required=std::min(maxSlots,std::max(used+2,recordings*parts+depth*maxTakePasses+2));
        for(int i=0;i<maxSlots && used<required;++i)if(!allocation.needed[i]){allocation.needed[i]=true;++used;}
        auto replacement=allocateUndoStorage(allocation);
        std::array<std::vector<float>,2> newHistory;
        const int retainedHistoryFrames=std::min(historyCount,newCapacity);
        for(int c=0;c<2;++c) {
            newHistory[c].resize(newCapacity);
            for(int f=0;f<retainedHistoryFrames;++f)
                newHistory[c][f]=history[c][modulo(historyWrite-retainedHistoryFrames+f,capacity)];
        }
        for(int i=0;i<maxSlots;++i)if(usedLengths[i]>0) {
            const auto& source=slots[i];auto& dest=replacement[i];
            for(int c=0;c<2;++c) {
                for(int f=0;f<usedLengths[i];++f)dest.audio[c][f]=source.audio[c][(source.offset+f)%capacity];
                std::copy(source.pre[c].begin(),source.pre[c].end(),dest.pre[c].begin());
                std::copy(source.post[c].begin(),source.post[c].end(),dest.post[c].begin());
            }
        }
        // Commit consists of swaps/scalars; destruction stays on the caller thread.
        for(int i=0;i<maxSlots;++i) {
            slots[i].audio.swap(replacement[i].audio);slots[i].pre.swap(replacement[i].pre);slots[i].post.swap(replacement[i].post);
            slots[i].offset=0;
        }
        history.swap(newHistory);historyCount=retainedHistoryFrames;historyWrite=retainedHistoryFrames%newCapacity;
        capacity=newCapacity;configureDuration(seconds);configureLimits(recordings,parts,roll);undoDepth=depth;
        undoHistory=retainedHistory;undoSize=retainedSize;undoCursor=retainedCursor;
        undoCount=0;undoKind=UndoKind::none;freshUndoPrepared=false;
        rebuildTakeLinks();return true;
    }
    void setLimits(int recordings,int parts,bool roll) {
        auto storage=allocateUndoStorage(undoAllocation(undoDepth,recordings,parts));
        installUndoStorage(storage,undoDepth);configureLimits(recordings,parts,roll);
    }
    // Lifecycle/test helper; allocation must stay outside the callback.
    void setUndoDepth(int depth) { auto buffers=allocateUndoStorage(undoAllocation(depth)); installUndoStorage(buffers,depth); }
    int getUndoDepth() const noexcept { return undoDepth; }
    void setTransitionOptions(double crossfade, double tail) noexcept
    {
        crossfadeMs = std::clamp(crossfade, 0.0, 250.0);
        captureTailMs = std::clamp(tail, 0.0, 2000.0);
        crossfadeSamples = std::max(0, int(rate * crossfadeMs / 1000));
        captureTailSamples = std::max(0, int(rate * captureTailMs / 1000));
    }
    void setTakeOptions(bool fades, bool feedback) noexcept { takeFades=fades; takeFeedback=feedback; }
    /** Called only when an Overdub hold is recognized, with elapsed input
        samples since its physical press. Ordinary Replace never looks back. */
    bool beginHeldReplace(int lookbackFrames) noexcept
    {
        if (heldBackfillSlot>=0 || length<=0 || !canAddRecording() || armed || pendingEnd>0 || linkedCapture
            || state==State::recording || state==State::replacing) return false;
        const bool wasStopped=state==State::stopped;
        const int requested=std::clamp(lookbackFrames,0,maxHeldLookback);
        startReplace();
        if (state!=State::replacing) return false;
        if (wasStopped && linkMode==0) {
            position=requested%length; runFrames=requested; replaceCursor=position;
        }
        const int slotIndex=active[count-1];
        auto& slot=slots[slotIndex];
        // Replace keeps the latest pass at each position for a short loop.
        // A missing history prefix is not fabricated and never masks old audio.
        const int available=std::min(requested,recentCount);
        const int retained=std::min(available,length);
        slot.start=modulo(position-retained,length); slot.written=retained;
        preSourceSlot=-1;
        preSourceStart=modulo(recentWrite-requested-slot.preFrames,int(recent[0].size()));
        preSourceMissing=std::max(0,slot.preFrames-std::max(0,recentCount-requested));
        heldStart=slot.start; heldCount=retained; heldCopied=0; heldLoopLength=length;
        heldSourceStart=modulo(recentWrite-retained,int(recent[0].size()));
        heldBackfillSlot=retained>0?slotIndex:-1;
        return true;
    }
    // Momentary Add: timing belongs to the physical gesture, not count-in.
    bool beginHeldRecord(int lookbackFrames) noexcept
    {
        if (capturingNow() || isStartPending() || isClonePending() || !canAddRecording() || needsClock() || !fixedBarsCompatible()) return false;
        const int target=importTarget();if(target<0)return false;
        if (length > 0 || target>0) {
            const int requested=std::clamp(lookbackFrames,0,maxHeldLookback);
            const int available=std::min(requested,recentCount);
            const bool stopped=state==State::stopped;
            if(length>0) {
                if(!startOverdub())return false;
                if(stopped && !quantized && linkMode==0){position=requested%length;runFrames=requested;}
            } else {
                // Establish the fixed/master cycle without waiting for a bar.
                // Historical audio may span several parts of this first take.
                preserveForUndo();const int index=freeSlot();if(index<0)return false;
                length=target;fixedLength=target;multiple=linkMode>1?target/linkBase:1;
                const int prefix=quantized && clockBar>0?modulo(clockPhase-available,clockBar):0;
                const int elapsed=prefix+available;
                position=linkMode>1?modulo(linkFrame-referenceOrigin,target):elapsed%target;
                timelineOrigin=hostFrame-elapsed;captureOrigin=linkFrame-elapsed;
                phaseOffset=0;if(linkMode==1)ownOrigin=captureOrigin;
                initialize(index,0);active[count++]=index;slots[index].info.take=nextTake++;
                state=State::overdubbing;captureOverdub=true;recordLayer=false;runFrames=elapsed;
                feedbackStep=feedbackLevel>0?std::pow(double(feedbackLevel),1.0/length):1;
            }
            int64_t captureLimit=passes>0?int64_t(length)*passes:INT64_MAX;
            if(requestedBars>0 && fixedFinish>0)captureLimit=std::min(captureLimit,int64_t(length));
            int start=modulo(position-available,length),skip=0,n=int(std::min(int64_t(available),captureLimit));
            const bool timedEnd=available>=captureLimit;
            if(overwritePass){skip=std::max(0,n-length);n-=skip;start=modulo(start+skip,length);}
            const int originalParts=overwritePass?1:std::max(1,(start+n+length-1)/length);
            int parts=std::min(originalParts,takeLimit);
            const bool ended=timedEnd || (originalParts>takeLimit && !rolling);
            if(originalParts>takeLimit) {
                if(rolling){skip=length-start+(originalParts-takeLimit-1)*length;start=0;n-=skip;}
                else n=takeLimit*length-start;
            }
            const auto take=slots[active[count-1]].info.take;
            const int basePart=rolling?originalParts-parts:0;
            int consumed=0;
            for(int part=0;part<parts;++part) {
                int index=active[count-1];
                if(part>0) {
                    index=freeSlot();
                    if(index<0 || count>=maxLayers){state=State::playing;break;}
                    initialize(index,0);active[count++]=index;
                }
                auto& slot=slots[index];const int begin=part==0?start:0;
                const int frames=overwritePass?n:std::min(n-consumed,length-begin);
                slot.start=begin;slot.written=frames;slot.info.take=take;slot.info.part=basePart+part;
                slot.info.continued=part+1<parts;
                const int age=available-skip-consumed;
                auto& fill=heldAdds[heldAddCount++];fill={};fill.slot=index;fill.start=begin;fill.frames=frames;
                fill.source=modulo(recentWrite-age,int(recent[0].size()));
                fill.pre=slot.preFrames;fill.missing=std::max(0,fill.pre-std::max(0,recentCount-age));
                if(part+1<parts || ended) {
                    slot.captureEnd=(begin+frames)%length;
                    fill.post=std::min({std::max(crossfadeSamples,captureTailSamples),maxPostFrames,length,std::max(0,age-frames)});
                }
                consumed+=frames;
            }
            preCopySlot=-1;overdubFrames=available;
            if(ended)state=State::playing;
            rebuildTakeLinks();return true;
        }
        const int n=std::min({std::max(0,lookbackFrames),maxHeldLookback,historyCount,capacity-1});
        const int prefix=quantized && clockBar>0 ? modulo(clockPhase-n,clockBar) : 0;
        if(prefix+n>=capacity || (target>0 && prefix+n>target))return false;
        const bool sync=quantized; const int bars=requestedBars,link=linkMode;
        quantized=false;requestedBars=0;linkMode=0;
        startRecord(true);
        quantized=sync;requestedBars=bars;linkMode=link;
        if(state!=State::recording)return false;
        auto& slot=slots[active[0]];
        length=prefix+n;
        slot.offset=modulo(historyWrite-n-prefix,capacity);
        slot.missingPrefix=prefix;slot.written=length;
        slot.audio.swap(history);historyWrite=historyCount=0;
        captureSync=sync;recordingBar=clockBar;recordingPhase=0;
        recordingLimit=sync && clockBar>0 ? capacity/clockBar*clockBar : capacity;
        timelineOrigin=hostFrame-length;captureOrigin=linkFrame-length;
        if(link==1)ownOrigin=captureOrigin;
        slot.info.take=nextTake++;
        if(target>0) {
            length=target;position=(prefix+n)%target;state=State::overdubbing;
            fixedLength=target;multiple=link>1?target/linkBase:1;
            if(link>1)phaseOffset=modulo(captureOrigin-referenceOrigin,length);
            overdubFrames=n;
        }
        runFrames=prefix+n;return true;
    }
    void endHeldRecord() noexcept
    {
        if(state==State::overdubbing) { finishLayerCapture(); return; }
        if(state!=State::recording)return;
        const int end=length;
        if(captureSync && recordingBar>0)
            length=std::min(recordingLimit,((length+recordingBar-1)/recordingBar)*recordingBar);
        finishRecording(false);
        if(count>0 && length>0)beginPostCapture(active[0],end%length);
    }
    bool isTailPending() const noexcept { return postCaptureSlot >= 0 || postBackfillSlot >= 0; }
    void setMixOptions(float pan, float feedback, double playMs, double stopMs) noexcept
    {
        panTarget = std::clamp(pan, -1.0f, 1.0f);
        feedbackLevel = std::clamp(feedback, 0.0f, 1.0f);
        feedbackStep = length > 0 && feedbackLevel > 0 ? std::pow(double(feedbackLevel), 1.0 / length) : 1;
        playFadeFrames = std::max(0, int(rate * std::clamp(playMs, 0.0, 10000.0) / 1000));
        stopFadeFrames = std::max(0, int(rate * std::clamp(stopMs, 0.0, 10000.0) / 1000));
    }
    static void panStereo(float& left, float& right, float pan) noexcept
    {
        // Center is identity. At either extreme both channels fold to that side.
        const float l = left, r = right;
        if (pan < 0) { left = l + r * -pan; right = r * (1 + pan); }
        else { left = l * (1 - pan); right = r + l * pan; }
    }
    void managerBeginOverdub() noexcept
    {
        if (!canBeginOverdub() || (linkMode>1 && linkBase<=0)) return;
        apply(Action::overdub);
    }
    bool canBeginOverdub() const noexcept
    { return !isStartPending() && pendingEnd == 0 && !isClonePending() && canAddRecording()
        && state != State::recording && state != State::overdubbing && state != State::replacing
        && !needsClock() && fixedBarsCompatible(); }
    int64_t getCaptureEndFrame() const noexcept { return linkedCapture && !growingCapture ? linkedEndFrame : -1; }
    void setRecordNextBar (bool value) noexcept { recordNextBar = value; }
    void setOverdubNextBar(bool value) noexcept { overdubNextBar=value; }
    void setFixedBars(int bars) noexcept { requestedBars=std::clamp(bars,0,256); }
    int getBarFrames() const noexcept { return clockBar; }
    int getCaptureTarget() const noexcept { return linkedCapture && initialFixedTarget>0?length:initialFixedTarget; }
    int getCapturedProgress() const noexcept { return linkedCapture?int(capturedFrames):length; }
    bool fixedBarsCompatible() const noexcept {
        if(requestedBars==0 || length>0)return true;
        const int64_t target=int64_t(clockBar)*requestedBars;
        if(!hostRunning || clockBar<=0 || target>capacity)return false;
        if(fixedLength>0 && target!=fixedLength)return false;
        if(linkMode>1 && (linkBase<=0 || (linkMode==2 ? target!=linkBase : target%linkBase!=0)))return false;
        return true;
    }

    // Manager validates lengths and stopped state before requesting this on audio.
    // Adopt the new reference without the tempo-resize behavior of setLinkContext.
    void prepareMasterChange(int mode,int base,int64_t origin) noexcept {
        fixedLength=length>0?length:fixedLength;
        multiple=mode==1?1:(base>0?fixedLength/base:0);
        ownOrigin=origin; phaseOffset=0; position=0;
    }
    // 0 independent, 1 master, 2 same length, 3 multiples. Called on audio only.
    void setLinkContext (int mode, int base, int64_t frame, int64_t origin) noexcept
    {
        linkMode = mode; linkBase = base; linkFrame = frame; referenceOrigin = origin;
        if (mode > 1 && fixedLength > 0 && multiple > 0 && base > 0
            && int64_t(multiple) * base <= capacity) {
            const int wanted = multiple * base;
            if (wanted != fixedLength && state != State::recording && state != State::overdubbing && state != State::replacing) {
                if (length > 0) {
                    for (int i=0;i<count;++i) slots[active[i]].written = std::min(slots[active[i]].written, length);
                    length = wanted;
                }
                phaseOffset = modulo(int64_t(std::llround(double(phaseOffset) * multiple / fixedLength)) * base, wanted);
                fixedLength = wanted;
            }
        }
    }
    void clearLinkContext() noexcept { linkMode=0; }
    void alignLinkedTimeline(uint64_t frames) noexcept { if(linkMode>0) linkFrame=(linkMode==1?ownOrigin:referenceOrigin)+int64_t(frames); }
    int64_t getLinkFrame() const noexcept { return linkFrame; }
    int getFixedLength() const noexcept { return fixedLength; }
    int getMultiple() const noexcept { return multiple; }
    int64_t getOrigin() const noexcept { return ownOrigin; }
    void restoreRuntimeOrigin(int64_t origin, int64_t frame) noexcept { ownOrigin = origin; linkFrame = frame; }
    void resetAll() noexcept
    {
        heldBackfillSlot=-1;heldAddCount=0;
        postBackfillSlot = -1;
        preCopySlot = postCaptureSlot = -1; recordLayer = false; freshUndoPrepared=false;
        cancelPerformance();timelineOrigin=hostFrame;
        count = length = position = undoCount = undoLength = fixedLength = multiple = 0;
        historyWrite = historyCount = 0; phaseOffset = ownOrigin = 0;
        pendingEnd = 0; armed = linkedCapture = false; linkedEndFrame = -1;initialFixedTarget=0;
        undoSize = undoCursor = 0;
        undoKind = UndoKind::none; state = State::empty; runFrames = 0;
        transportGain=transportTarget=0; transportRemaining=tailRemaining=0; previouslyAudible=false;
        rollingSlot=-1;rollingRemaining=0;
        lastLoop = correction = {}; fadeRemaining = 0; declick = false;
    }
    bool needsClock() const noexcept { return quantized && (!hostRunning || clockBar == 0); }
    bool startedByOverdub() const noexcept { return performancePending ? performanceAction==Action::overdub : captureOverdub; }
    bool isStartPending() const noexcept { return armed || performancePending; }
    bool isCaptureStartPending() const noexcept { return armed || (performancePending &&
        (performanceAction==Action::record || performanceAction==Action::overdub || performanceAction==Action::freshRecord)); }
    void setClock (double bpm, int numerator, int denominator, double ppq = 0, bool running = false) noexcept
    {
        const double frames = bpm > 0 && numerator > 0 && denominator > 0 ? rate * 60 / bpm * numerator * 4 / denominator : 0;
        const int newBar = std::isfinite (frames) && frames >= 1 && frames <= capacity
            ? static_cast<int> (std::llround (frames)) : 0;
        const double phase = newBar > 0 && std::isfinite (ppq)
            ? std::fmod (std::max (0.0, ppq), numerator * 4.0 / denominator) * rate * 60 / bpm : 0;
        const int newPhase = std::isfinite (phase) && newBar > 0
            ? static_cast<int> (std::llround (phase)) % newBar : 0;
        const int delta = std::abs (newPhase - clockPhase);
        if (quantized && hostRunning && running && (newBar != clockBar
            || (delta > 1 && delta < newBar - 1)))
            historyCount = 0;
        if (quantized && (!running || newBar==0 || (hostRunning && newBar!=clockBar))) { armed=false; cancelPerformance(); }
        if(quantized && hostRunning && delta>1 && delta<newBar-1)cancelPerformance();
        if(std::isfinite(ppq) && std::isfinite(bpm) && bpm>0)hostFrame=int64_t(std::llround(std::clamp(ppq*rate*60/bpm,-1.0e15,1.0e15)));
        hostRunning = running;
        clockBar = newBar;
        clockPhase = newPhase;
    }
    // Performance scheduling is separate from unconditional manager/panic commands.
    // Latched by the processor before capture; never changed during a take.
    void setCaptureBehavior(bool overwrite, bool hearPrevious, int finishFixed=-1) noexcept
    { overwritePass=overwrite; hearPreviousPass=hearPrevious; fixedFinish=finishFixed; }
    void setFinishAtBoundary(bool value) noexcept { finishAtBoundary=value; }
    void setControllerCountIn(int bars) noexcept { countInBars=std::clamp(bars,0,8); }
    bool capturingNow() const noexcept { return state==State::recording || state==State::overdubbing || state==State::replacing; }
    void setPerformanceOptions(bool preserve, bool playBar, bool stopBar,
                               bool recPrepare, bool odPrepare, int bars, float threshold) noexcept
    {
        preserveTimeline=preserve; playOnBar=playBar; stopOnBar=stopBar;
        prepareRecord=recPrepare; prepareOverdub=odPrepare;
        countInBars=std::clamp(bars,0,8); triggerLevel=std::clamp(threshold,0.00001f,1.0f);
    }
    int getWaitingAction() const noexcept { return performancePending ? int(performanceAction)+1 : 0; }
    int getCountInRemaining() const noexcept {
        return performancePending && quantized && clockBar>0
            ? int((waitFrames+clockBar-1)/clockBar) : 0;
    }
    int getBarPhase() const noexcept { return clockPhase; }
    bool countInAudible() const noexcept { return performancePending && quantized && !waitForSound && (performanceAction==Action::record || performanceAction==Action::overdub || performanceAction==Action::freshRecord) && waitFrames>0 && waitFrames<=int64_t(countInBars)*clockBar; }
    bool waitingForSound() const noexcept { return performancePending && waitForSound; }
    int getSyncPosition() const noexcept {
        if(length<=0)return 0;
        if(linkMode>0)return modulo(linkFrame-(linkMode==1?ownOrigin:referenceOrigin+phaseOffset),length);
        return modulo(hostFrame-timelineOrigin,length);
    }
    void cancelCaptureStart() noexcept { armed=false;cancelPerformance(); }
    void cancelPerformance() noexcept { performancePending=false;waitFrames=0;waitForSound=false; }
    void setPlayToEnd(bool enabled,int masterFrames,int64_t masterOrigin) noexcept {
        if(enabled && !playToEndEnabled){endCycleArmed=true;endCycleStarted=false;}
        if(!enabled)endCycleArmed=false;
        playToEndEnabled=enabled;endMasterFrames=std::max(0,masterFrames);endMasterOrigin=masterOrigin;
    }
    bool momentaryMuted() const noexcept {return momentaryTarget==0;}
    void clearMomentaryGate() noexcept {setMomentaryGate(true);}
    void setMomentaryGate(bool open) noexcept {
        const float target=open?1.0f:0.0f;
        if(open && playToEndEnabled){endCycleArmed=true;endCycleStarted=false;}
        if(target==momentaryTarget)return;
        momentaryTarget=target;momentaryRemaining=open?playFadeFrames:stopFadeFrames;
    }
    void applyMomentary(bool down) noexcept {
        if(capturingNow() || isCaptureStartPending()) return;
        if(down && state!=State::playing) {
            const bool playWait=playOnBar;playOnBar=false;cancelPerformance();
            applyPerformance(Action::play);playOnBar=playWait;
        }
        setMomentaryGate(down); // Release gates playback only; phase continues.
    }
    void setManagerMix(float volume,float pan) noexcept {
        managerVolumeTarget=std::isfinite(volume)?std::clamp(volume,0.0f,1.0f):1.0f;managerPanTarget=std::isfinite(pan)?std::clamp(pan,-1.0f,1.0f):0.0f;
    }
    void applyPerformance(Action action) noexcept
    {
        if((action==Action::play || action==Action::playStop) && state==State::playing && momentaryMuted()) {
            clearMomentaryGate();return;
        }

        // Unavailable history commands must not cancel preparation, finalize
        // capture, or change any capture bookkeeping (keyboard/MIDI bypass UI).
        if ((action == Action::undo && !canUndo()) || (action == Action::redo && !canRedo())) return;

        if(performancePending) {
            const bool same=action==performanceAction || (action==Action::playStop &&
                (performanceAction==Action::play || performanceAction==Action::stop));
            cancelPerformance();
            if(same)return;
        }
        // A second press cancels arming without stopping backing playback.
        if(armed && (action==Action::record || action==Action::overdub || action==Action::playStop)) {
            armed=false;return;
        }
        const bool capturing=state==State::recording||state==State::overdubbing||state==State::replacing;
        if(action==Action::playStop)action=(capturing||state==State::playing)?Action::stop:Action::play;
        const bool captureStart=!capturing && (action==Action::record||action==Action::overdub||action==Action::freshRecord);
        const bool preparation=captureStart && (action==Action::overdub?prepareOverdub:prepareRecord);
        const bool transportWait=!capturing && quantized && length>0 &&
            ((action==Action::play && state==State::stopped && playOnBar)
            || (action==Action::stop && state==State::playing && stopOnBar));
        if(preparation || transportWait) {
            if((quantized && needsClock()) || (captureStart && (!fixedBarsCompatible() || (action!=Action::freshRecord && !canAddRecording()))))return;
            performancePending=true;performanceAction=action;
            waitForSound=preparation&&!quantized;
            // Always use the next boundary, then N complete count-in bars.
            waitFrames=waitForSound?0:int64_t(clockBar-clockPhase)+(preparation?int64_t(countInBars)*clockBar:0);
            return;
        }
        executePerformance(action,false);
    }
    void executePerformance(Action action,bool prepared) noexcept
    {
        if(prepared && action==Action::freshRecord) {
            apply(action);
            if(armed){armed=false;startRecord();}
            return;
        }
        if(prepared && (action==Action::record || action==Action::overdub)) {
            if(length>0) {
                if(action==Action::record)startRecordLayer();else startOverdub();
            } else startRecord(action==Action::overdub);
            return;
        }
        const bool resuming=action==Action::play && state==State::stopped && length>0;
        const int phase=getSyncPosition();
        apply(action);
        if(resuming && state==State::playing && quantized) {
            playbackDetached=!preserveTimeline;
            if(preserveTimeline)position=phase;
            else timelineOrigin=hostFrame;
        }
    }

    void apply (Action action) noexcept
    {
        // Unavailable history commands must not cancel preparation, finalize
        // capture, or change any capture bookkeeping (keyboard/MIDI bypass UI).
        if ((action == Action::undo && !canUndo()) || (action == Action::redo && !canRedo())) return;

        if(action==Action::play || action==Action::record || action==Action::overdub || action==Action::freshRecord)clearMomentaryGate();
        LinkRefresh refresh{*this};
        cancelPerformance();
        if((action==Action::record || action==Action::overdub || action==Action::freshRecord)
            && state!=State::recording && state!=State::overdubbing && !fixedBarsCompatible())return;

        // A pending clone still reads its old slot. Do not recycle its backing
        // storage for another destructive edit until bounded background work finishes.
        if (action == Action::freshRecord) {
            if(requestedBars>0) {
                const int64_t target=int64_t(clockBar)*requestedBars;
                if(!hostRunning || clockBar<=0 || target>capacity || (fixedLength>0 && target!=fixedLength)
                    || (linkMode>1 && (linkBase<=0 || (linkMode==2?target!=linkBase:target%linkBase!=0)))) return;
            }
            if (heldBackfillSlot>=0 || heldAddCount>0) return; // preserve in-flight Undo/Redo backing storage
            rollingSlot=-1;rollingRemaining=0;
            postBackfillSlot = -1;
            preCopySlot = postCaptureSlot = -1; armed = false; recordLayer = false;
            preserveForUndo(); freshUndoPrepared = true;
            count = length = position = 0; state = State::empty; pendingEnd = 0;initialFixedTarget=0;
            linkedCapture = false; linkedEndFrame = -1;
            if (linkMode == 0) { fixedLength = multiple = 0; phaseOffset = ownOrigin = 0; }
            captureOverdub = false;
            if (needsClock() || (linkMode > 1 && linkBase <= 0)) return;
            if (quantized && (recordNextBar || requestedBars>0)) armed = true; else startRecord();
            return;
        }
        if (isClonePending() && state != State::recording && state != State::overdubbing && state != State::replacing
            && (action == Action::record || action == Action::overdub)) return;
        if (action == Action::replace) { startReplace(); return; }
        if (action == Action::endReplace) { if (state == State::replacing) finishLayerCapture(); return; }
        if (state == State::replacing) finishLayerCapture();
        if (recordLayer && state == State::overdubbing && action == Action::record) { finishLayerCapture(); return; }
        if ((action == Action::undo || action == Action::redo || action == Action::clear)) {
            postBackfillSlot = -1;rollingSlot=-1;rollingRemaining=0;
            preCopySlot = postCaptureSlot = -1; recordLayer = false; freshUndoPrepared=false;
        }
        if (action == Action::record && length > 0 && state != State::recording && state != State::overdubbing) {
            if (armed) { armed = false; return; }
            if (needsClock() || !canAddRecording()) return;
            captureOverdub = false;
            if (quantized && recordNextBar) armed = true; else startRecordLayer();
            return;
        }
        declick = true;
        if (action == Action::playStop)
            action = armed || state == State::playing || state == State::recording || state == State::overdubbing ? Action::stop : Action::play;
        if (armed && linkMode > 0 && (fixedLength > 0 || linkMode > 1)
            && (action == Action::record || action == Action::overdub)) { armed=false; return; }
        if (linkedCapture && state == State::recording
            && (action == Action::record || action == Action::overdub || action == Action::play || action == Action::stop)) {
            endLinkedRecording(action == Action::stop); return;
        }
        if (linkMode > 1 && (action == Action::record || (action == Action::overdub && length == 0))
            && (linkBase <= 0 || linkBase > capacity)) return;
        if (requestedBars==0 && linkMode > 0 && (fixedLength > 0 || linkMode > 1)
            && !armed && (action == Action::record || (action == Action::overdub && length == 0))) {
            if (needsClock()) return;
            captureOverdub = action == Action::overdub;
            if (quantized && (captureOverdub?overdubNextBar:recordNextBar)) armed = true;
            else startRecord(captureOverdub);
            return;
        }
        if (armed && (action == Action::record || action == Action::overdub || action == Action::stop || action == Action::clear || action == Action::undo || action == Action::redo))
        {
            armed = false;
            if (action == Action::record || action == Action::overdub) return;
        }
        if (pendingEnd > 0 && (action == Action::record || action == Action::overdub)) return;
        switch (action)
        {
            case Action::record:
                if(state!=State::recording)captureOverdub=false;
                if (state == State::recording)
                    endRecording (false);
                else if (needsClock())
                    break;
                else if (quantized && (recordNextBar || requestedBars>0) && hostRunning && clockBar > 0)
                    armed = true;
                else
                    startRecord();
                break;
            case Action::play:
                if (state == State::overdubbing) finishLayerCapture();
                if (state == State::recording)
                    endRecording (false);
                else if (length > 0)
                {
                    if (state == State::stopped) { position = 0; runFrames = 0; }
                    state = State::playing;
                }
                break;
            case Action::stop:
                if (state == State::overdubbing) finishLayerCapture();
                if (state == State::recording)
                    endRecording (true);
                else
                {
                    state = length ? State::stopped : State::empty;
                    position = 0;
                }
                break;
            case Action::overdub:
                armed = false;
                if (state == State::recording)
                    endRecording (false);
                else if (state == State::overdubbing)
                    finishLayerCapture();
                else if (length == 0) {
                    if (needsClock()) break;
                    captureOverdub = true;
                    if (quantized && (overdubNextBar || requestedBars>0)) armed = true; else startRecord(true);
                }
                else if(canAddRecording() && !needsClock()) {
                    captureOverdub=true;
                    if(quantized && overdubNextBar)armed=true;else startOverdub();
                }
                break;
            case Action::clear:
                linkedCapture = false; linkedEndFrame = -1;initialFixedTarget=0;
                preserveForUndo();
                count = length = position = 0;
                state = State::empty;
                pendingEnd = 0;
                break;
            case Action::undo:
            case Action::redo:
                linkedCapture = false; linkedEndFrame = -1;initialFixedTarget=0;
                if ((action == Action::undo && canUndo()) || (action == Action::redo && canRedo())) {
                    const bool keepStopped = state == State::stopped || state == State::empty;
                    if (action == Action::undo) {
                        if (undoCursor == undoSize) undoHistory[undoSize] = captureUndo();
                        restoreUndo(undoHistory[--undoCursor]);
                    } else restoreUndo(undoHistory[++undoCursor]);
                    position = length > 0 ? position % length : 0;
                    state = length ? (keepStopped ? State::stopped : State::playing) : State::empty;
                }
                pendingEnd = 0;
                break;
            case Action::replace:
            case Action::endReplace:
            case Action::freshRecord:
                break;
            case Action::playStop:
                break;
        }
    }
    /** Finish capture immediately without quantization or discarding recorded samples. */
    uint64_t getRunFrames() const noexcept { return runFrames; }
    void rewind() noexcept
    {
        if(linkMode>1 || capturingNow() || isStartPending() || length<=0)return;
        cancelPerformance();position=0;runFrames=0;timelineOrigin=hostFrame;
        playbackDetached=true;declick=true;
    }
    void playFromTimeline (uint64_t frames) noexcept
    {
        if (armed || pendingEnd > 0 || state == State::recording || state == State::overdubbing
            || state == State::playing || state == State::replacing || length == 0) return;
        position = static_cast<int> (frames % static_cast<uint64_t> (length));
        timelineOrigin=hostFrame-position;playbackDetached=false;
        runFrames = frames;
        state = State::playing;
        declick = true;
    }
    void finalizeCapture() noexcept
    {
        armed = false;cancelPerformance();
        if (state == State::recording)
            finishRecording (false);
        else if (state == State::overdubbing || state == State::replacing)
            finishLayerCapture();
    }
    /** Host stopped: finish existing copies, but never invent future input or advance transport.
        Called only off the audio thread with exclusive ownership. */
    void finalizeOffline() noexcept
    {
        // Finish available material before finalizing capture can replace its handles.
        while (heldBackfillSlot >= 0 || heldAddCount>0) advanceHeldBackfill();
        while (preCopySlot >= 0) advancePreCopy();
        while (postBackfillSlot >= 0) advancePostBackfill();
        finalizeCapture();
        apply(Action::stop);
        postCaptureSlot = -1;
        rollingSlot = -1;rollingRemaining = 0;
        rebuildTakeLinks();
        tailRemaining = transportRemaining = 0;
        transportGain = transportTarget = 0;
        previouslyAudible = false;
    }
    /** Build privately; no live state or snapshot pointers change on allocation failure. */
    void restorePrepared(const Snapshot& saved, double sampleRate, int seconds,
                         int recordings, int parts, bool roll, int depth)
    {
        LooperEngine next;
        next.configureDuration(seconds);
        next.configureLimits(recordings, parts, roll);
        next.undoDepth = std::clamp(depth, 1, maxUndoDepth);
        next.nextID = nextID;
        next.nextTake = nextTake;
        next.prepare(sampleRate);
        next.load(saved);
        static_assert(std::is_nothrow_move_assignable_v<LooperEngine>);
        *this = std::move(next);
    }
    /** Process input with independent live monitor and loop mute (mute preserves transport). */
    void process (float& left, float& right, float loopGain, bool monitor = true, bool mute = false, float* loopLeft = nullptr, float* loopRight = nullptr) noexcept
    {
        if(performancePending) {
            const bool triggered=waitForSound && std::max(std::abs(left),std::abs(right))>=triggerLevel;
            if(triggered || (!waitForSound && waitFrames==0)) {
                const Action action=performanceAction;
                const bool capture=action==Action::record||action==Action::overdub||action==Action::freshRecord;
                // Start linked capture at the beginning of the recovered attack,
                // not at the trigger. Keep its master-relative origin and length.
                int linkedPreRoll = 0;
                const bool linkedStart = triggered && (length == 0 || action==Action::freshRecord)
                    && (linkMode > 1 || (linkMode == 1 && fixedLength > 0));
                const auto triggerFrame = linkFrame;
                if (linkedStart) {
                    const int base = linkMode == 1 ? fixedLength : linkBase;
                    if (base > 0 && base <= capacity) {
                        linkedPreRoll = std::min({recentCount, int(rate * 0.005), base - 1});
                        if (linkMode == 3 && fixedLength == 0 && requestedBars == 0) {
                            const int phase = modulo(linkFrame - referenceOrigin, base);
                            // Reserve room for the triggering sample. If history
                            // crosses a cycle and capacity cannot hold both, clip
                            // only the unavailable prefix rather than overrun.
                            if (modulo(linkFrame-linkedPreRoll-referenceOrigin,base) + linkedPreRoll >= capacity/base*base)
                                linkedPreRoll = std::min(linkedPreRoll, phase);
                        }
                        linkFrame -= linkedPreRoll;
                    }
                }
                cancelPerformance();executePerformance(action,capture);
                linkFrame = triggerFrame;
                if (linkedStart && linkedCapture && state == State::recording && count > 0) {
                    auto& slot = slots[active[0]];
                    for (int i=0;i<linkedPreRoll;++i) {
                        const int dest = growingCapture ? length++ : captureCursor;
                        for (int c=0;c<2;++c)
                            writeSample(slot.audio[c][dest],recent[c][modulo(recentWrite-linkedPreRoll+i,int(recent[c].size()))]);
                        if (growingCapture) slot.written=length;
                        else { slot.written=std::min(length,slot.written+1);captureCursor=modulo(captureCursor+1,length); }
                    }
                    // Attack recovery may begin in the previous cycle. Do not
                    // end the first pass at a boundary already crossed by history.
                    if(initialFixedTarget>0 && linkedPreRoll>=initialFixedTarget)
                        initialFixedTarget+=length;
                    capturedFrames=linkedPreRoll;
                    preSourceStart=modulo(recentWrite-linkedPreRoll-slot.preFrames,int(recent[0].size()));
                    preSourceMissing=std::max(0,slot.preFrames-std::max(0,recentCount-linkedPreRoll));
                }
                else if(triggered && !linkedCapture && count>0 && (state==State::recording||state==State::overdubbing)) {
                    // Fixed 5ms attack recovery, bounded independently of loop duration.
                    const int n=std::min({recentCount,int(rate*0.005),length>0?length:capacity-1});
                    auto& slot=slots[active[count-1]];
                    if(state==State::recording) {
                        for(int i=0;i<n;++i)for(int c=0;c<2;++c)
                            writeSample(slot.audio[c][i],recent[c][modulo(recentWrite-n+i,int(recent[c].size()))]);
                        slot.written=length=n;runFrames=n;
                        // Sample zero now precedes the trigger by the recovered attack.
                        // Linked captures retain their master-defined origin above.
                        timelineOrigin-=n;captureOrigin=linkFrame-n;
                        if(linkMode==1)ownOrigin-=n;
                    } else {
                        slot.start=modulo(position-n,length);slot.written=n;
                        for(int i=0;i<n;++i)for(int c=0;c<2;++c)
                            writeSample(slot.audio[c][modulo(slot.start+i,length)],recent[c][modulo(recentWrite-n+i,int(recent[c].size()))]);
                    }
                    preSourceStart=modulo(recentWrite-n-slot.preFrames,int(recent[0].size()));
                    preSourceMissing=std::max(0,slot.preFrames-std::max(0,recentCount-n));
                }
            } else if(!waitForSound && waitFrames>0)--waitFrames;
        }
        struct HostAdvance { LooperEngine& e; ~HostAdvance(){if(e.hostRunning)++e.hostFrame;} } hostAdvance{*this};
        // Split lazily: ending exactly at a cycle boundary creates no empty part.
        if(state==State::overdubbing && !overwritePass && count>0
            && ((position==0 && slots[active[count-1]].written>0) || slots[active[count-1]].written>=length)) {
            if(!continueTake()) finishLayerCapture();
        }
        advancePreCopy();
        advanceHeldBackfill();
        advancePostBackfill();
        livePostSlot=-1;
        if (loopLeft) *loopLeft = 0;
        if (loopRight) *loopRight = 0;
        struct Advance { int64_t& frame; ~Advance() { ++frame; } } advance { linkFrame };
        if (armed && clockPhase == 0) {
            armed=false;
            if(fixedBarsCompatible()) {
            if(length>0 && state!=State::recording) {
                if(captureOverdub)startOverdub();else startRecordLayer();
            } else startRecord(captureOverdub);
            }
        }
        if (linkMode > 0 && length > 0 && !(state==State::playing && playbackDetached) && (state == State::playing || state == State::overdubbing || state == State::replacing))
            position = modulo(linkFrame - (linkMode == 1 ? ownOrigin : referenceOrigin + phaseOffset), length);
        if(playToEndEnabled && endCycleArmed && state==State::playing && length>0 && !momentaryMuted()) {
            const int phase=endMasterFrames>0?modulo(linkFrame-endMasterOrigin,endMasterFrames):position;
            if(endCycleStarted && (phase==0 || phase<endCyclePrevious)) {
                setMomentaryGate(false);endCycleArmed=false;
            } else {endCyclePrevious=phase;endCycleStarted=true;}
        }
        // A host seek can jump the linked phase while holding Replace. End
        // the edit rather than write ahead of the bounded clone frontier.
        if (state == State::replacing && position != replaceCursor) {
            finishLayerCapture();
        }
        const bool audible = state == State::playing || state == State::overdubbing || state == State::replacing;
        if(audible != previouslyAudible) {
            if(audible) { transportTarget=1; transportRemaining=playFadeFrames; tailRemaining=0; }
            else { transportTarget=0; transportRemaining=stopFadeFrames; tailRemaining=stopFadeFrames; tailPosition=nextTailPosition; }
            previouslyAudible=audible;
        }
        if(transportRemaining>0) transportGain += (transportTarget-transportGain)/transportRemaining--;
        else transportGain=transportTarget;
        const bool tail = !audible && state == State::stopped && tailRemaining>0 && length>0;
        if(tail) position=tailPosition;
        const float input[] { left, right };
        if (!recent[0].empty()) {
            for (int c=0; c<2; ++c) writeSample(recent[c][recentWrite], input[c]);
            recentWrite = (recentWrite + 1) % int(recent[0].size());
            recentCount = std::min(recentCount + 1, int(recent[0].size()));
        }
        if (postCaptureSlot >= 0) {
            auto& slot = slots[postCaptureSlot];
            if(monitor) { livePostSlot=postCaptureSlot; livePostFrame=modulo(slot.captureEnd+postLiveWritten,length); }
            for (int c=0; c<2; ++c) writeSample(slot.post[c][postLiveWritten], input[c]);
            ++postLiveWritten;
            if (postBackfillSlot < 0) slot.postFrames = postLiveWritten;
            if (postLiveWritten >= postTarget) postCaptureSlot = -1;
        }
        if (capacity > 0) {
            writeSample (history[0][historyWrite], left);
            writeSample (history[1][historyWrite], right);
            historyWrite = (historyWrite + 1) % capacity;
            historyCount = std::min (capacity, historyCount + 1);
        }
        if (hostRunning && clockBar > 0 && ++clockPhase >= clockBar)
            clockPhase = 0;
        left = monitor ? left : 0;
        right = monitor ? right : 0;
        if (capacity == 0)
            return;
        if (state == State::recording || state == State::playing || state == State::overdubbing || state == State::replacing) ++runFrames;
        if (state == State::recording && linkedCapture) {
            auto& layer = slots[active[0]];
            if (growingCapture) {
                for (int c=0;c<2;++c) writeSample(layer.audio[c][length], input[c]);
                layer.written = ++length;
            } else {
                for (int c=0;c<2;++c) writeSample(layer.audio[c][captureCursor], input[c]);
                layer.written = std::min(length, layer.written + 1);
                captureCursor = (captureCursor + 1) % length;
            }
            ++capturedFrames;
            emitLoop(0,0,left,right,loopLeft,loopRight);
            if ((initialFixedTarget>0 && capturedFrames>=uint64_t(initialFixedTarget))
                || (growingCapture && length >= recordingLimit)
                || (linkedEndFrame >= 0 && linkFrame + 1 >= linkedEndFrame)) finishLinkedRecording(pendingStop,linkFrame+1);
            return;
        }
        if (state == State::recording)
        {
            auto& layer = slots[active[0]];
            for (int c = 0; c < 2; ++c)
                writeSample (layer.audio[c][(layer.offset + length) % capacity], input[c]);
            layer.written = ++length;
            emitLoop (0, 0, left, right, loopLeft, loopRight);
            if (length >= recordingLimit || (pendingEnd > 0 && length >= pendingEnd))
                finishRecording (pendingStop);
            return;
        }
        if ((!audible && !tail) || length == 0)
        {
            rollingSlot=-1;rollingRemaining=0;
            emitLoop (0, 0, left, right, loopLeft, loopRight);
            return;
        }
        bool anySolo = false;
        for (int i = 0; i < count; ++i)
            anySolo |= getLayer (i).enabled && getLayer (i).solo;
        float loopL = 0, loopR = 0;
        for (int i = 0; i < count; ++i)
        {
            auto& slot = slots[active[i]];
            const float target = slot.info.enabled && (!anySolo || slot.info.solo) ? slot.info.gain : 0;
            if (target != slot.gainTarget)
            {
                slot.gainTarget = target;
                slot.gainRemaining = std::max(1,int(rate*0.01));
            }
            if (slot.gainRemaining > 0)
                slot.renderGain += (target - slot.renderGain) / slot.gainRemaining--;
            else slot.renderGain = target;
            slot.renderPan += std::clamp(slot.info.pan - slot.renderPan, -panStep, panStep);
            const bool capturing = (state == State::overdubbing || state == State::replacing) && i == count - 1;
            float opacity = layerOpacity(slot, position);
            // During Replace the monitored input occupies the selected region.
            // Avoid playing the same newly captured frame twice on the live pass.
            if (capturing && state == State::replacing)
                opacity = crossfadeSamples > 0
                    ? std::min(1.0f,float(overdubFrames + 1) / crossfadeSamples) : 1.0f;
            const bool retainedRecordMask = slot.recordMaskRemaining > 0;
            if (slot.recordMaskRemaining > 0) --slot.recordMaskRemaining;
            const float audibleMask = (slot.info.enabled && (!anySolo || slot.info.solo)) || retainedRecordMask ? opacity : 0;
            if (slot.info.replace) { loopL *= 1 - audibleMask; loopR *= 1 - audibleMask; }
            if (!(capturing && (state == State::replacing || (overwritePass && !hearPreviousPass)))) {
                float l = layerContribution(i,0,position,true,active[i]==livePostSlot && position==livePostFrame,capturing);
                float r = layerContribution(i,1,position,true,active[i]==livePostSlot && position==livePostFrame,capturing);
                panStereo(l, r, slot.renderPan); loopL += l; loopR += r;
            }
        }
        if (mute != previousMute) { declick = true; previousMute = mute; }
        renderPan += std::clamp(panTarget - renderPan, -panStep, panStep);
        panStereo(loopL, loopR, renderPan);
        emitLoop (mute ? 0 : loopL * loopGain * transportGain, mute ? 0 : loopR * loopGain * transportGain,
                  left, right, loopLeft, loopRight);
        if (state == State::overdubbing)
        {
            auto& layer = slots[active[count - 1]];
            // Old layers are attenuated non-destructively, independently of user gain.
            // At zero feedback fade them out over one cycle rather than hard-muting.
            for(int i=0;i<count-1;++i) {
                auto& slot=slots[active[i]]; auto& old=slot.feedbackAccumulator;
                if(slot.info.ignoreFeedback || (!takeFeedback && slot.info.take!=0 && slot.info.take==layer.info.take)) continue;
                old = feedbackLevel > 0 ? old * feedbackStep
                    : old * (1.0f - 1.0f / float(std::max<int64_t>(1, int64_t(length) - slot.feedbackFrames)));
                if (old < 1.0e-20f) old=0;
                slot.info.feedbackGain=float(old);
                slot.feedbackFrames=std::min(length,slot.feedbackFrames+1);
            }
            for (int c = 0; c < 2; ++c)
                writeSample (layer.audio[c][(layer.offset+position)%capacity], input[c]);
            layer.written = std::min (length, layer.written + 1);
            if(overwritePass && layer.written==length) {
                // The original punch-in history no longer belongs to this
                // fully overwritten pass. Never blend stale first-pass input.
                layer.preFrames=layer.preCopied=0;
                if(preCopySlot==active[count-1])preCopySlot=-1;
            }
            ++overdubFrames;
            int parts=0;
            if(!overwritePass && !rolling && position+1==length)
                for(int i=0;i<count;++i)parts+=slots[active[i]].info.take==layer.info.take;
            if (((fixedFinish>0 && position+1==length) || (fixedFinish<0 && requestedBars>0 && recordLayer && overdubFrames>=length))
                || (passes > 0 && overdubFrames >= static_cast<int64_t> (length) * passes && (!finishAtBoundary || position+1==length))
                || (!overwritePass && !rolling && parts>=takeLimit))
                finishLayerCapture((position + 1) % length);
        }
        if (state == State::replacing) {
            auto& layer = slots[active[count - 1]];
            for (int c = 0; c < 2; ++c) writeSample(layer.audio[c][position], input[c]);
            layer.written = std::min(length, layer.written + 1);
            ++overdubFrames;
            replaceCursor = (position + 1) % length;
        }
        if(rollingSlot>=0 && --rollingRemaining<=0) { rollingSlot=-1;rebuildTakeLinks(); }
        if (++position >= length)
        {
            position = 0;
            // A timeline wrap is not a capture boundary. Per-layer handles
            // treat actual joins; do not modify continuous audio across zero.
        }
        nextTailPosition=position;
        if(tail) { tailPosition=position; --tailRemaining; position=0; }
    }
    float layerSample (int index, int channel, int frame) const noexcept
    {
        if (index < 0 || index >= count || frame < 0 || frame >= length)
            return 0;
        int slot = active[index];
        return slotSample(slot, channel, frame, length);
    }
    float sample (int channel, int frame) const noexcept
    {
        bool solo = false;
        for (int i = 0; i < count; ++i)
            solo |= getLayer (i).solo && getLayer (i).enabled;
        float value = 0;
        for (int i = 0; i < count; ++i)
        {
            const auto& info = getLayer (i);
            if (info.enabled && (! solo || info.solo)) {
                if (info.replace) value *= 1 - layerOpacity(slots[active[i]], frame);
                value += layerContribution(i,channel,frame);
            }
        }
        return value;
    }
    LayerInfo getLayer (int index) const noexcept { return index >= 0 && index < count ? slots[active[index]].info : LayerInfo {}; }
    void editLayer (uint32_t id, int field, float value) noexcept
    {
        if (!std::isfinite(value)) return;
        LinkRefresh refresh{*this};
        for (int i = 0; i < count; ++i)
        {
            auto& info = slots[active[i]].info;
            if (info.id != id)
                continue;
            if ((field == 3 || field == 5) && (armed || isClonePending()
                || state == State::recording || state == State::overdubbing || state == State::replacing)) return;
            if (field == 0)
                info.enabled = value != 0;
            else if (field == 1)
                info.solo = value != 0;
            else if (field == 2) {
                // The editor controls effective volume, including feedback.
                // Reject queued/automated edits too, not just mouse gestures.
                if (state == State::overdubbing || (state == State::recording && captureOverdub)) return;
                auto& slot = slots[active[i]];
                slot.renderGain *= info.feedbackGain;
                slot.gainTarget *= info.feedbackGain;
                info.feedbackGain = 1;
                slot.feedbackAccumulator = 1;
                info.gain = std::clamp (value, 0.0f, 2.0f);
            }
            else if (field == 5) { info.replace = value != 0; declick = true; }
            else if (field == 6) info.ignoreFeedback = value != 0;
            else if (field == 4) info.pan = std::clamp(value, -1.0f, 1.0f);
            else if (field == 3)
            {
                // Deleting while capturing is ambiguous: require finalization first.
                if (state == State::recording || state == State::overdubbing || state == State::replacing)
                    return;
                for (int j = i; j + 1 < count; ++j)
                    active[j] = active[j + 1];
                --count;
                declick = true;
                undoKind = UndoKind::none;
                undoCount = 0; undoSize = undoCursor = 0;
                if (count == 0)
                {
                    length = position = 0;
                    state = State::empty;
                }
            }
            return;
        }
    }
    Snapshot snapshot() const
    {
        Snapshot result;
        result.rate = rate;
        result.length = initialFixedTarget>0 && !linkedCapture?initialFixedTarget:length;
        result.fixedLength = fixedLength; result.multiple = multiple; result.phaseOffset = phaseOffset;
        for (int i = 0; i < count; ++i)
        {
            SavedLayer layer;
            layer.info = getLayer (i);
            const auto& slot = slots[active[i]];
            layer.coverageStart = slot.start == 0 ? slot.missingPrefix : slot.start;
            layer.coverageFrames = slot.written - (slot.start == 0 ? std::min(slot.written,slot.missingPrefix) : 0);
            layer.captureEnd = slot.captureEnd;
            for (int c=0;c<2;++c) {
                layer.pre[c].assign(slot.pre[c].begin(),slot.pre[c].begin()+(slot.preCopied==slot.preFrames?slot.preFrames:0));
                layer.post[c].assign(slot.post[c].begin(),slot.post[c].begin()+slot.postFrames);
            }
            for (int c = 0; c < 2; ++c)
            {
                layer.audio[c].resize (result.length);
                for (int f = 0; f < length; ++f)
                    layer.audio[c][f] = layerSample (i, c, f);
            }
            result.layers.push_back (std::move (layer));
        }
        return result;
    }
    void load (const Snapshot& saved)
    {
        heldBackfillSlot=-1;heldAddCount=0; undoSize = undoCursor = 0;
        armed = linkedCapture = false; linkedEndFrame = -1;initialFixedTarget=0;
        postBackfillSlot = -1;
        preCopySlot = postCaptureSlot = -1; recordLayer = false; freshUndoPrepared=false;
        fixedLength = std::clamp(saved.fixedLength, 0, capacity);
        multiple = saved.multiple; phaseOffset = saved.phaseOffset; ownOrigin = 0;
        historyCount = 0;
        count = std::min (maxLayers, static_cast<int> (saved.layers.size()));
        auto storage=allocateUndoStorage(undoAllocation(undoDepth,0,0,count));installUndoStorage(storage,undoDepth);
        rollingSlot=-1;rollingRemaining=0;
        length = std::min (capacity, saved.length);
        position = 0;
        runFrames = 0;
        undoKind = UndoKind::none;
        undoCount = 0;
        pendingEnd = 0;
        for (int i = 0; i < count; ++i)
        {
            active[i] = i;
            auto& slot = slots[i];
            slot.info = saved.layers[i].info; nextTake=std::max(nextTake,slot.info.take+1); slot.feedbackAccumulator=slot.info.feedbackGain;
            slot.info.id = nextID++;
            slot.start = modulo(saved.layers[i].coverageStart, length); slot.offset = slot.missingPrefix = 0;
            slot.written = saved.layers[i].coverageFrames < 0 ? length : std::clamp(saved.layers[i].coverageFrames,0,length);
            slot.captureEnd = saved.layers[i].captureEnd;
            slot.preFrames = slot.preCopied = std::min(maxPreFrames, int(std::min(saved.layers[i].pre[0].size(),saved.layers[i].pre[1].size())));
            slot.postFrames = std::min(maxPostFrames, int(std::min(saved.layers[i].post[0].size(),saved.layers[i].post[1].size())));
            for (int c=0;c<2;++c) {
                std::copy_n(saved.layers[i].pre[c].begin(),slot.preCopied,slot.pre[c].begin());
                std::copy_n(saved.layers[i].post[c].begin(),slot.postFrames,slot.post[c].begin());
            }
            slot.renderGain = slot.gainTarget = slot.info.gain;
            slot.gainRemaining = slot.recordMaskRemaining = 0; slot.renderPan = slot.info.pan;
            for (int c = 0; c < 2; ++c)
                std::copy_n (saved.layers[i].audio[c].begin(), length, slot.audio[c].begin());
        }
        state = length && count ? State::stopped : State::empty;rebuildTakeLinks();
    }
    void load (const std::array<std::vector<float>, 2>& audio)
    {
        Snapshot saved;
        saved.length = static_cast<int> (std::min (audio[0].size(), audio[1].size()));
        if (saved.length)
            saved.layers.push_back ({ {}, audio });
        load (saved);
    }
    State getState() const noexcept { return state; }
    int getLength() const noexcept { return length; }
    int getPosition() const noexcept { return state == State::recording ? length : position; }
    int getCapacity() const noexcept { return capacity; }
    int getLayerCount() const noexcept { return count; }
    double getRate() const noexcept { return rate; }
    bool canUndo() const noexcept { return undoCursor > 0; }
    bool canRedo() const noexcept { return undoCursor < undoSize; }
    bool isReplacing() const noexcept { return state == State::replacing; }
    bool isClonePending() const noexcept { return heldBackfillSlot>=0 || heldAddCount>0 || preCopySlot >= 0 || isTailPending(); }
    bool isEndPending() const noexcept { return pendingEnd > 0 || linkedEndFrame >= 0; }
    bool hasBarClock() const noexcept { return clockBar > 0; }

private:
    struct Slot;
    void advanceHeldBackfill() noexcept
    {
        // At most 32 historical stereo frames per live frame, independent of
        // loop duration and layer count. Core audio is readable via fallback
        // while it is copied; pre/post handles are published only when ready.
        int budget=32;
        for(int j=0;j<heldAddCount && budget>0;++j) {
            auto& fill=heldAdds[j];if(fill.slot<0)continue;
            auto& slot=slots[fill.slot];const int total=fill.frames+fill.pre+fill.post;
            while(fill.copied<total && budget-->0) {
                const int f=fill.copied++;
                for(int c=0;c<2;++c) {
                    if(f<fill.frames)writeSample(slot.audio[c][(slot.offset+modulo(fill.start+f,length))%capacity],readSample(recent[c][(fill.source+f)%recent[c].size()]));
                    else if(f<fill.frames+fill.pre) {
                        const int p=f-fill.frames;
                        writeSample(slot.pre[c][p],p<fill.missing?0:readSample(recent[c][modulo(fill.source-fill.pre+p,int(recent[c].size()))]));
                    } else {
                        const int p=f-fill.frames-fill.pre;
                        writeSample(slot.post[c][p],readSample(recent[c][(fill.source+fill.frames+p)%recent[c].size()]));
                    }
                }
            }
            if(fill.copied==total){slot.preCopied=fill.pre;if(fill.post>0)slot.postFrames=fill.post;fill.slot=-1;}
        }
        bool pending=false;for(int j=0;j<heldAddCount;++j)pending|=heldAdds[j].slot>=0;
        if(!pending)heldAddCount=0;
        if (heldBackfillSlot<0) return;
        auto& slot=slots[heldBackfillSlot];
        const int end=std::min(heldCount,heldCopied+32);
        for(;heldCopied<end;++heldCopied) {
            const int frame=modulo(heldStart+heldCopied,heldLoopLength);
            for(int c=0;c<2;++c)
                writeSample(slot.audio[c][frame],readSample(recent[c][(heldSourceStart+heldCopied)%recent[c].size()]));
        }
        if(heldCopied==heldCount) heldBackfillSlot=-1;
    }
    void advancePreCopy() noexcept
    {
        if (preCopySlot < 0) return;
        auto& slot = slots[preCopySlot];
        const int end = std::min(slot.preFrames, slot.preCopied + 32);
        for (;slot.preCopied < end;++slot.preCopied)
            for (int c=0;c<2;++c)
                writeSample(slot.pre[c][slot.preCopied],slot.preCopied < preSourceMissing ? 0
                    : preSourceSlot < 0 ? readSample(recent[c][(preSourceStart + slot.preCopied) % recent[c].size()])
                    : readSample(slots[preSourceSlot].audio[c][(preSourceStart + slot.preCopied) % capacity]));
        if (slot.preCopied == slot.preFrames) preCopySlot = -1;
    }
    void advancePostBackfill() noexcept
    {
        if (postBackfillSlot < 0) return;
        auto& slot = slots[postBackfillSlot];
        const int end=std::min(postBackfillCount,slot.postFrames+32);
        for(;slot.postFrames<end;++slot.postFrames)
            for(int c=0;c<2;++c)
                writeSample(slot.post[c][slot.postFrames],readSample(slot.audio[c][(slot.offset+length+slot.postFrames)%capacity]));
        if(slot.postFrames==postBackfillCount) { slot.postFrames=postLiveWritten; postBackfillSlot=-1; }
    }
    void beginPostCapture(int slotIndex,int end) noexcept
    {
        if (slotIndex < 0 || length <= 0) return;
        auto& slot=slots[slotIndex]; slot.captureEnd=modulo(end,length); slot.postFrames=0;
        postLiveWritten=0; postBackfillSlot=-1;
        postTarget=std::min({std::max(crossfadeSamples,captureTailSamples),maxPostFrames,length});
        postCaptureSlot=postTarget>0?slotIndex:-1;
    }
    void finishLayerCapture(int end=-1) noexcept
    {
        if (count>0 && (state==State::overdubbing || state==State::replacing))
            beginPostCapture(active[count-1],end<0?position:end);
        recordLayer=false; state=length>0?State::playing:State::empty; declick=true;
    }
    void startRecordLayer() noexcept
    {
        LinkRefresh refresh { *this };
        if (!canAddRecording() || length<=0) return;
        if (!startOverdub()) return;
        bool oldSolo=false;
        for(int i=0;i<count-1;++i) oldSolo|=slots[active[i]].info.enabled && slots[active[i]].info.solo;
        for(int i=0;i<count-1;++i) {
            auto& slot=slots[active[i]];
            const bool wasAudible=slot.info.enabled && (!oldSolo || slot.info.solo);
            // Keep the old ordered composite while its contributors fade
            // together. Even a zero-gain Replace must retain its silent mask.
            slot.recordMaskRemaining=wasAudible && slot.info.replace ? crossfadeSamples : 0;
            slot.info.enabled=false;
            slot.gainTarget=0;
            slot.gainRemaining=wasAudible?crossfadeSamples:0;
            if(!wasAudible) slot.renderGain=0;
        }
        recordLayer=true;captureOverdub=false;
    }
    // Raw handles remain editable. At an end, the existing core fades in over
    // Crossfade while the captured continuation fades out over Tail (at least
    // Crossfade). A longer musical tail is additive after the short blend.
    std::array<int,maxSlots> previousPart {},nextPart {};
    void rebuildTakeLinks() noexcept {
        previousPart.fill(-1);nextPart.fill(-1);
        bool solo=false;for(int i=0;i<count;++i)solo|=getLayer(i).enabled&&getLayer(i).solo;
        for(int i=0;i<count;++i) {
            const auto& a=slots[active[i]].info;if(a.take==0||a.replace)continue;
            for(int j=0;j<count;++j) {
                const auto& b=slots[active[j]].info;
                if(b.take!=a.take||b.replace||!b.enabled||(solo&&!b.solo))continue;
                if(b.part==a.part-1)previousPart[active[i]]=active[j];
                if(b.part==a.part+1)nextPart[active[i]]=active[j];
            }
            if(rollingSlot>=0) {
                const auto& lost=slots[rollingSlot].info;
                if(lost.take==a.take && lost.part==a.part-1 && lost.enabled && (!solo||lost.solo) && !lost.replace)
                    previousPart[active[i]]=rollingSlot;
            }
        }
    }
    struct LinkRefresh { LooperEngine& engine; ~LinkRefresh(){engine.rebuildTakeLinks();} };
    template<class Layer> int takeNeighbour(const Layer& layer,int direction) const noexcept {
        const int slot=int(&layer-slots.data());
        return direction<0?previousPart[slot]:nextPart[slot];
    }
    float layerContribution(int index,int channel,int frame,bool smooth=false,bool skipPost=false,bool capturing=false) const noexcept
    {
        const auto& slot=slots[active[index]];
        const float own=(smooth?slot.renderGain:slot.info.gain)*slot.info.feedbackGain;
        float value=(capturing?layerSample(index,channel,frame):renderedLayerSample(index,channel,frame,skipPost,smooth))*own;
        const int previous=takeNeighbour(slot,-1);
        const int relative=modulo(frame-slot.start,length);
        const int ramp=std::min(length,std::max(1,int(rate*0.01)));
        if(previous>=0 && relative<ramp && relative<slot.written && slot.info.enabled) {
            const auto& prev=slots[previous];
            const float from=(smooth?prev.renderGain:prev.info.gain)*prev.info.feedbackGain;
            value+=slotSample(active[index],channel,frame,length)*(from-own)*(1.0f-float(relative)/ramp);
        }
        return value;
    }
    bool continueTake() noexcept
    {
        const auto take=slots[active[count-1]].info.take;
        int first=-1,parts=0;
        for(int i=0;i<count;++i)if(slots[active[i]].info.take==take){if(first<0)first=i;++parts;}
        const bool forget=parts>=takeLimit;
        if(forget && !rolling)return false;
        if((!forget && count>=maxLayers) || rollingSlot>=0)return false;
        const int fresh=freeSlot();if(fresh<0)return false;
        auto& old=slots[active[count-1]];
        if(old.info.part==INT32_MAX-1)return false;
        const int part=old.info.part+1;
        old.info.continued=true;
        beginPostCapture(active[count-1],position);
        if(forget) {
            rollingSlot=active[first];
            rollingRemaining=std::min(length,std::max(1,int(rate*0.01)));
            for(int i=first;i+1<count;++i)active[i]=active[i+1];--count;
        }
        initialize(fresh,position);
        slots[fresh].info.take=take;slots[fresh].info.part=part;
        active[count++]=fresh;
        rebuildTakeLinks();return true;
    }
    int recordingLimitCount=10,takeLimit=4,durationSeconds=60;
    bool rolling=true;
    bool overwritePass=false,hearPreviousPass=false,finishAtBoundary=false;
    int fixedFinish=-1;
    int rollingSlot=-1,rollingRemaining=0;
    uint32_t nextTake=1;
    bool takeFades=true,takeFeedback=true;
    float layerOpacity(const Slot& slot,int frame,bool skipPost=false) const noexcept
    {
        float weight=0; layerWindow(slot,frame,weight,nullptr,0,skipPost); return std::min(1.0f,weight);
    }
    void layerWindow(const Slot& slot,int frame,float& weight,float* value,int channel,bool skipPost=false) const noexcept
    {
        weight=0; if(value)*value=0;
        const int rel=modulo(frame-slot.start,length);
        const bool core=rel<slot.written && frame>=slot.missingPrefix;
        float coreGain=core?1.0f:0.0f,preGain=0,postGain=0;
        int preIndex=0,postIndex=0;
        const bool fullTake=slot.info.take!=0 && slot.written>=length;
        const bool joinedBefore = slot.info.take!=0 && slot.info.part>0
            && (!takeFades || takeNeighbour(slot,-1)>=0);
        const bool joinedAfter = slot.info.take!=0 && slot.info.continued
            && (!takeFades || takeNeighbour(slot,1)>=0);
        // A folded full pass has two DIFFERENT musical times at this seam.
        // If both ends are orphaned, use one end->start blend, not pre AND post
        // blends that switch anchors at the seam. If its end remains connected,
        // pre-roll must be additive: that outgoing end belongs to the next pass.
        const bool useEndBlend=fullTake && !joinedBefore && !joinedAfter
            && slot.captureEnd==slot.start && slot.postFrames>0;
        const int pre=!joinedBefore && !useEndBlend && slot.preCopied==slot.preFrames
            && (slot.written<length || slot.info.take!=0)
            ?std::min({crossfadeSamples,slot.preCopied,length}):0;
        const int start=slot.start==0?slot.missingPrefix:slot.start;
        const int before=modulo(start-frame,length);
        if(pre>0 && before>0 && before<=pre) {
            preIndex=slot.preCopied-before;
            preGain=float(pre-before+1)/pre;
            if(!fullTake || !joinedAfter) coreGain*=1-preGain;
        }
        const bool pending=&slot==(postCaptureSlot>=0?&slots[postCaptureSlot]:nullptr)
            || &slot==(postBackfillSlot>=0?&slots[postBackfillSlot]:nullptr);
        const int duration=std::min({std::max(crossfadeSamples,captureTailSamples),
            pending?postTarget:slot.postFrames,length});
        if(!joinedAfter && slot.captureEnd>=0 && duration>0) {
            const int after=modulo(frame-slot.captureEnd,length);
            if(after<std::min(duration,slot.postFrames)) {
                postIndex=after;
                postGain=skipPost?0:float(duration-after)/duration;
                const int blend=std::min(crossfadeSamples,duration);
                // On the live Add pass the input is already monitored; keep
                // the earlier accumulated core instead of dropping it too.
                if(blend>0 && !(skipPost && !slot.info.replace)
                    && !(joinedBefore && slot.captureEnd==slot.start))
                    coreGain*=std::min(1.0f,float(after)/blend);
            }
        }
        weight=coreGain+preGain+postGain;
        if(value) {
            if(coreGain>0) *value+=slotSample(int(&slot-slots.data()),channel,frame,length)*coreGain;
            if(preGain>0) *value+=readSample(slot.pre[channel][preIndex])*preGain;
            if(postGain>0) {
                float continuation = readSample(slot.post[channel][postIndex]);
                // A full layer may contain earlier overdub passes. Its core
                // immediately before the edit end is then not the raw input
                // stored in post[]. Anchor that continuation to the audible
                // endpoint, releasing the difference over Crossfade. Without
                // this, the first post frame abruptly drops accumulated audio.
                const int blend = std::min(crossfadeSamples,duration);
                if(slot.written>=length && blend>0 && postIndex<blend) {
                    const float endpoint = slotSample(int(&slot-slots.data()),channel,
                        modulo(slot.captureEnd-1,length),length);
                    continuation += (endpoint-readSample(slot.post[channel][0]))
                        * (1.0f-float(postIndex)/blend);
                }
                *value+=continuation*postGain;
            }
        }
    }
    float renderedLayerSample(int index,int channel,int frame,bool skipPost=false,bool live=false) const noexcept
    {
        const auto& slot=slots[active[index]];
        float weight=0,value=0; layerWindow(slot,frame,weight,&value,channel,skipPost);
        // Legacy full takes without saved handles still need a local seam
        // repair. Never apply it to another layer's continuous wrap.
        if(live && slot.info.take==0 && slot.start==0 && slot.written>=length
            && slot.captureEnd<=0 && slot.postFrames==0 && frame<fadeFrames) {
            const float last=slotSample(active[index],channel,length-1,length);
            const float first=slotSample(active[index],channel,0,length);
            value+=(last-first)*(1.0f-float(frame)/fadeFrames);
        }
        return value;
    }
    float slotSample(int slot, int channel, int frame, int loopLength) const noexcept
    {
        const auto& layer = slots[slot];
        const int relative = modulo(frame - layer.start, loopLength);
        for(int j=0;j<heldAddCount;++j)if(heldAdds[j].slot==slot) {
            const auto& fill=heldAdds[j];const int f=modulo(frame-fill.start,loopLength);
            if(f>=fill.copied && f<fill.frames)return readSample(recent[channel][(fill.source+f)%recent[channel].size()]);
        }
        if(slot==heldBackfillSlot) {
            const int historical=modulo(frame-heldStart,loopLength);
            if(historical>=heldCopied && historical<heldCount)
                return readSample(recent[channel][(heldSourceStart+historical)%recent[channel].size()]);
        }
        return relative < layer.written && frame >= layer.missingPrefix
            ? readSample(layer.audio[channel][(layer.offset + frame) % capacity]) : 0;
    }
    void startReplace() noexcept
    {
        if (length <= 0 || !canAddRecording() || armed || pendingEnd > 0 || linkedCapture
            || heldBackfillSlot>=0 || state == State::recording || state == State::replacing) return;
        // A hold may follow an ordinary overdub. Its bounded tail is abandoned
        // at the intentional edit boundary; no old audio is modified.
        if (state == State::overdubbing) finishLayerCapture();
        postCaptureSlot = postBackfillSlot = -1;
        if (!startOverdub()) return;
        slots[active[count - 1]].info.take = 0;
        slots[active[count - 1]].info.replace = true;
        state = State::replacing; replaceCursor = position; declick = true;rebuildTakeLinks();
    }
    int replaceCursor = 0;
    static int modulo (int64_t value, int size) noexcept { return size > 0 ? int((value % size + size) % size) : 0; }
    void startLinkedRecording (bool viaOverdub) noexcept
    {
        LinkRefresh refresh { *this };
        const int base = linkMode == 1 ? fixedLength : linkBase;
        if (base <= 0 || base > capacity) return;
        captureOverdub = viaOverdub; captureSync = quantized; captureGrace = grace;
        previousState = state; previousPosition = position; previousRunFrames = runFrames;
        if (!freshUndoPrepared) preserveForUndo(); freshUndoPrepared=false; count = 0;
        const int slot = freeSlot(); if (slot < 0) { count = undoCount; return; }
        active[0] = slot; count = 1;
        growingCapture = requestedBars==0 && linkMode == 3 && fixedLength == 0;
        const int64_t origin = linkMode == 1 ? ownOrigin : referenceOrigin;
        const int phase = modulo(linkFrame - origin, base);
        if (growingCapture) {
            length = phase; initialize(slot,0); slots[slot].missingPrefix = phase;
            slots[slot].written = phase; captureOrigin = linkFrame - phase;
        } else {
            length = requestedBars>0 ? clockBar*requestedBars : fixedLength > 0 ? fixedLength : base;
            if (fixedLength == 0) phaseOffset = 0;
            captureOrigin = linkMode == 1 ? ownOrigin : referenceOrigin + phaseOffset;
            captureCursor = modulo(linkFrame - captureOrigin,length);
            initialize(slot,captureCursor);
        }
        recordingBar = base; recordingLimit = capacity / base * base;
        armed = false; pendingEnd = 0; linkedEndFrame = -1; pendingStop = false;
        linkedCapture = true; capturedFrames = 0; state = State::recording;
        initialFixedTarget=growingCapture?0:length-captureCursor;
        if(initialFixedTarget>0)slots[slot].info.take=nextTake++;

    }
    void endLinkedRecording (bool stop) noexcept
    {
        if (linkedEndFrame >= 0) return;
        if (capturedFrames == 0) {
            active=undoActive; count=undoCount; length=undoLength; position=previousPosition; runFrames=previousRunFrames;
            state=previousState==State::overdubbing?State::playing:previousState;
            linkedCapture=false; initialFixedTarget=0; undoKind=UndoKind::none; undoCount=0; undoSize=undoCursor=std::max(0,undoCursor-1); return;
        }
        if(initialFixedTarget>0) { finishLinkedRecording(stop);return; }
        pendingStop = stop;
        if (growingCapture) {
            const int previous = length / recordingBar * recordingBar;
            if (previous > 0 && length - previous <= captureGrace) {
                const int late=length-previous;
                length = previous; slots[active[0]].written = length; finishLinkedRecording(stop);
                beginPostCapture(active[0],0);
                postBackfillCount=std::min(late,postTarget); postLiveWritten=postBackfillCount;
                postBackfillSlot=postBackfillCount>0?active[0]:-1;
                if(postLiveWritten>=postTarget) postCaptureSlot=-1;
            } else {
                const int end = std::min(recordingLimit, previous + recordingBar);
                linkedEndFrame = linkFrame + end - length;
            }
        } else if (captureSync && clockBar > 0 && clockPhase > captureGrace) {
            linkedEndFrame = linkFrame + clockBar - clockPhase;
        } else finishLinkedRecording(stop);
    }
    void finishLinkedRecording (bool stop, int64_t nextFrame = -1) noexcept
    {
        const bool keepOverdubbing=initialFixedTarget>0 && capturedFrames>=uint64_t(initialFixedTarget) && (fixedFinish<0?captureOverdub:fixedFinish==0) && !stop;
        initialFixedTarget=0;

        linkedCapture = false; linkedEndFrame = -1; pendingEnd = 0;
        if (growingCapture && length % recordingBar != 0) {
            // Panic/bypass finalization must not establish a fractional cycle.
            const int rounded = std::min(recordingLimit, ((length + recordingBar - 1) / recordingBar) * recordingBar);
            length = rounded; // unwritten suffix remains silent through written metadata
        }
        if (fixedLength == 0 && capturedFrames > 0 && length > 0) {
            fixedLength = length; multiple = linkMode == 1 ? 1 : length / linkBase;
            if (linkMode > 1) phaseOffset = modulo(captureOrigin - referenceOrigin, length);
        }
        position = modulo((nextFrame>=0?nextFrame:linkFrame) - captureOrigin,length);
        beginPostCapture(active[0], position);
        state = length > 0 ? (stop ? State::stopped : State::playing) : State::empty;
        if(keepOverdubbing && !((!overwritePass && !rolling && takeLimit==1) || (passes>0 && capturedFrames>=uint64_t(length)*passes))) {
            state=State::overdubbing;recordLayer=false;overdubFrames=int64_t(capturedFrames);
            feedbackStep=feedbackLevel>0?std::pow(double(feedbackLevel),1.0/length):1;
        }
        rebuildTakeLinks();declick = true;
    }
    int linkMode = 0, linkBase = 0, fixedLength = 0, multiple = 0, captureCursor = 0;
    int64_t linkFrame = 0, ownOrigin = 0, referenceOrigin = 0, phaseOffset = 0, captureOrigin = 0, linkedEndFrame = -1;
    uint64_t capturedFrames = 0;
    bool linkedCapture = false, growingCapture = false;
    float readSample (const float& source) const noexcept
    {
        return writeObserver ? std::atomic_ref<float>(const_cast<float&>(source)).load() : source;
    }
    void writeSample (float& destination, float value) noexcept
    {
        if (writeObserver) writeObserver (writeContext, &destination, value);
        else destination = value;
    }
    void* writeContext = nullptr;
    WriteObserver writeObserver = nullptr;
    struct Slot
    {
        std::array<std::vector<float>, 2> audio, pre, post;
        int preFrames = 0, preCopied = 0, postFrames = 0, captureEnd = -1;
        LayerInfo info;
        float renderGain = 1, gainTarget = 1, renderPan = 0;
        double feedbackAccumulator = 1;
        int gainRemaining = 0, recordMaskRemaining = 0, feedbackFrames = 0;
        int start = 0, written = 0, offset = 0, missingPrefix = 0;
    };
    enum class UndoKind
    {
        none,
        record,
        overdub
    };
    struct UndoState {
        std::array<int,maxLayers> active {};
        std::array<LayerInfo,maxLayers> info {};
        int count=0, length=0;
    };
    UndoState captureUndo() const noexcept {
        UndoState result; result.active=active; result.count=count; result.length=length;
        for(int i=0;i<count;++i) result.info[i]=slots[active[i]].info;
        return result;
    }
    void restoreUndo(const UndoState& saved) noexcept {
        active=saved.active; count=saved.count; length=saved.length;
        for(int i=0;i<count;++i) { slots[active[i]].info=saved.info[i]; slots[active[i]].feedbackAccumulator=saved.info[i].feedbackGain; slots[active[i]].recordMaskRemaining=0; }
    }
    void preserveForUndo() noexcept
    {
        undoSize = undoCursor;
        if (undoSize >= undoDepth) {
            for(int i=0;i<undoSize;++i) undoHistory[i]=undoHistory[i+1];
            --undoSize;
        }
        undoHistory[undoSize++] = captureUndo(); undoCursor=undoSize;
        undoActive = active;
        undoCount = count;
        undoLength = length;
        undoKind = UndoKind::record;
    }
    int freeSlot() const noexcept
    {
        for (int s = 0; s < int(slots.size()); ++s)
        {
            if(int(slots[s].audio[0].size()) != capacity) continue;
            bool used = s == heldBackfillSlot || s == rollingSlot || s == postCaptureSlot || s == preCopySlot || s == postBackfillSlot;
            for(int j=0;j<heldAddCount;++j)used|=heldAdds[j].slot==s;
            for (int i = 0; i < count; ++i)
                used |= active[i] == s;
            for(int h=0;h<undoSize;++h)
                for(int i=0;i<undoHistory[h].count;++i) used |= undoHistory[h].active[i]==s;
            if(undoCursor<undoSize)
                for(int i=0;i<undoHistory[undoSize].count;++i) used |= undoHistory[undoSize].active[i]==s;
            if (! used)
                return s;
        }
        return -1;
    }
    void initialize (int slot, int start) noexcept
    {
        slots[slot].info = {}; slots[slot].feedbackAccumulator=1; slots[slot].feedbackFrames=0;
        slots[slot].info.id = nextID++;
        slots[slot].start = start;
        slots[slot].offset = slots[slot].missingPrefix = 0;
        slots[slot].written = 0;
        slots[slot].captureEnd = -1; slots[slot].postFrames = 0;
        slots[slot].preFrames = std::min(crossfadeSamples, maxPreFrames);
        slots[slot].preCopied = 0;
        preCopySlot = slots[slot].preFrames > 0 ? slot : -1;
        preSourceSlot = -1;
        preSourceStart = modulo(recentWrite - slots[slot].preFrames, int(recent[0].size()));
        preSourceMissing = std::max(0, slots[slot].preFrames - recentCount);
        slots[slot].renderGain = slots[slot].gainTarget = 1;
        slots[slot].gainRemaining = slots[slot].recordMaskRemaining = 0; slots[slot].renderPan = 0;
    }
    void startRecord (bool viaOverdub = false) noexcept
    {
        LinkRefresh refresh { *this };
        if (needsClock() || !fixedBarsCompatible()) return;
        if (length > 0 && !viaOverdub) { startRecordLayer(); return; }
        if (linkMode > 1 || (linkMode == 1 && fixedLength > 0)) { startLinkedRecording(viaOverdub); return; }
        timelineOrigin=hostFrame-(quantized?clockPhase:0);
        if (linkMode == 1) ownOrigin = linkFrame - (quantized ? clockPhase : 0);
        captureOverdub = viaOverdub;
        captureSync = quantized;
        captureGrace = grace;
        previousState = state;
        previousPosition = position;
        previousRunFrames = runFrames;
        armed = false;
        if (!freshUndoPrepared) preserveForUndo(); freshUndoPrepared=false;
        count = 0;
        const int slot = freeSlot();
        if (slot < 0)
            return;
        active[0] = slot;
        count = 1;
        initialize (slot, 0);
        length = position = 0;
        state = State::recording;
        pendingEnd = 0;
        pendingStop = false;
        recordingBar = clockBar;
        recordingLimit = captureSync && recordingBar > 0 ? capacity / recordingBar * recordingBar : capacity;
        recordingPhase = 0;
        initialFixedTarget=requestedBars>0?clockBar*requestedBars:0;
        if(initialFixedTarget>0) {
            recordingLimit=initialFixedTarget;captureOrigin=linkFrame;
            slots[slot].info.take=nextTake++;
        }
        if (initialFixedTarget==0 && captureSync && hostRunning && clockBar > 0) {
            // Missing history (just started transport) is silence, never stale audio.
            length = std::clamp (clockPhase, 0, capacity - 1);
            // Transfer ownership of the circular history instead of copying a bar
            // in the pedal callback. The old spare slot becomes the next history.
            slots[slot].offset = (historyWrite - length + capacity) % capacity;
            slots[slot].missingPrefix = std::max (0, length - historyCount);
            slots[slot].audio.swap (history);
            // Pre-handle belongs before the logical bar start, not before the
            // late pedal press. Its source is the retained circular history.
            preSourceSlot = slot;
            preSourceStart = modulo(slots[slot].offset-slots[slot].preFrames,capacity);
            preSourceMissing = std::max(0,slots[slot].preFrames-std::max(0,historyCount-length));
            historyWrite = historyCount = 0;
            slots[slot].written = length;
        }
        runFrames = static_cast<uint64_t> (length);
    }
    bool startOverdub() noexcept
    {
        LinkRefresh refresh { *this };
        if (!canAddRecording() || length <= 0)
            return false;
        preserveForUndo();
        const int slot = freeSlot();
        if (slot < 0)
            return false;
        if (state == State::stopped) { position = quantized && preserveTimeline ? getSyncPosition() : 0; runFrames = uint64_t(position); }
        if (linkMode > 0) position = modulo(linkFrame - (linkMode == 1 ? ownOrigin : referenceOrigin + phaseOffset),length);
        initialize (slot, position);
        active[count++] = slot;
        undoKind = UndoKind::record;
        state = State::overdubbing; captureOverdub=true; recordLayer=false;
        slots[slot].info.take = nextTake++;
        for(int i=0;i<count;++i)slots[active[i]].feedbackFrames=0;
        overdubFrames = 0;
        feedbackStep = feedbackLevel > 0 ? std::pow(double(feedbackLevel), 1.0 / length) : 1;
        return true;
    }
    void endRecording (bool stop) noexcept
    {
        if(initialFixedTarget>0) { finishRecording(stop);return; }
        if (captureSync && recordingBar > 0)
        {
            const int previous = (length + recordingPhase) / recordingBar * recordingBar - recordingPhase;
            if (previous == 0 && length <= captureGrace)
            {
                active = undoActive;
                count = undoCount;
                length = undoLength;
                position = previousPosition;
                runFrames = previousRunFrames;
                state = previousState == State::overdubbing ? State::playing : previousState;
                undoKind = UndoKind::none;
                undoCount = 0; undoSize=undoCursor=std::max(0,undoCursor-1);
                pendingEnd = 0;
                return;
            }
            if (previous > 0 && length - previous <= captureGrace)
            {
                const int lengthBeforeTrim = length;
                length = previous;
                slots[active[0]].written = length;
                const int late = lengthBeforeTrim - previous;
                finishRecording (stop);
                postBackfillCount=std::min(late,postTarget);
                postLiveWritten=postBackfillCount;
                postBackfillSlot=postBackfillCount>0?active[0]:-1;
                if(postLiveWritten>=postTarget) postCaptureSlot=-1;
                if (!stop && length > 0) position = late % length;
            }
            else
            {
                pendingEnd = std::min (recordingLimit, previous + recordingBar);
                pendingStop = stop;
            }
        }
        else
            finishRecording (stop);
    }
    void finishRecording (bool stop) noexcept
    {
        if (linkedCapture) { finishLinkedRecording(stop); return; }
        if(initialFixedTarget>0) {
            const int captured=length;const bool keepOverdubbing=captured>=initialFixedTarget && (fixedFinish<0?captureOverdub:fixedFinish==0) && !stop;
            length=initialFixedTarget;initialFixedTarget=0;
            if(linkMode>0 && fixedLength==0) {
                fixedLength=length;multiple=linkMode==1?1:length/linkBase;
                phaseOffset=linkMode==1?0:modulo(captureOrigin-referenceOrigin,length);
            }
            position=captured%length;pendingEnd=0;
            beginPostCapture(active[0],position);
            state=stop?State::stopped:State::playing;
            if(keepOverdubbing && !((!rolling && takeLimit==1) || passes==1)) {
                state=State::overdubbing;recordLayer=false;overdubFrames=length;
                feedbackStep=feedbackLevel>0?std::pow(double(feedbackLevel),1.0/length):1;
            }
            if(stop)position=0;
            rebuildTakeLinks();return;
        }
        if (linkMode == 1 && length > 0 && fixedLength == 0) { fixedLength = length; multiple = 1; phaseOffset = 0; }
        declick = true;
        pendingEnd = 0;
        position = 0;
        if (length == 0)
            count = 0;
        if (length > 0) beginPostCapture(active[0], 0);
        state = length ? (stop ? State::stopped : State::playing) : State::empty;
    }
    // Playback-only de-click correction: recorded samples and loop length stay
    // untouched. Only commanded transitions and loop joins start a short ramp.
    void emitLoop (float l, float r, float& left, float& right, float* loopLeft, float* loopRight) noexcept
    {
        const std::array<float, 2> next { l, r };
        if (declick)
        {
            for (int c = 0; c < 2; ++c) correction[c] = lastLoop[c] - next[c];
            fadeRemaining = fadeFrames;
            declick = false;
        }
        for (int c = 0; c < 2; ++c)
            lastLoop[c] = next[c] + (fadeRemaining > 0 ? correction[c] * fadeRemaining / fadeFrames : 0);
        if (fadeRemaining > 0) --fadeRemaining;
        managerVolumeRender+=std::clamp(managerVolumeTarget-managerVolumeRender,-panStep,panStep);
        managerPanRender+=std::clamp(managerPanTarget-managerPanRender,-panStep,panStep);
        if(momentaryRemaining>0)momentaryGain+=(momentaryTarget-momentaryGain)/momentaryRemaining--;
        else momentaryGain=momentaryTarget;
        float outL=lastLoop[0]*managerVolumeRender*momentaryGain,outR=lastLoop[1]*managerVolumeRender*momentaryGain;
        panStereo(outL,outR,managerPanRender);
        left += outL; right += outR;
        if (loopLeft) *loopLeft = outL;
        if (loopRight) *loopRight = outR;
    }
    std::array<std::vector<float>,2> recent;
    int recentWrite=0,recentCount=0,maxPreFrames=0,maxPostFrames=0;
    struct HeldAddFill {int slot=-1,start=0,frames=0,source=0,copied=0,pre=0,post=0,missing=0;};
    std::array<HeldAddFill,maxTakePasses> heldAdds {};int heldAddCount=0;
    int heldBackfillSlot=-1,heldStart=0,heldCount=0,heldCopied=0,heldSourceStart=0,maxHeldLookback=0,heldLoopLength=0;
    int preCopySlot=-1,preSourceSlot=-1,preSourceStart=0,preSourceMissing=0,postCaptureSlot=-1,postTarget=0;
    int postBackfillSlot=-1,postBackfillCount=0,postLiveWritten=0,livePostSlot=-1,livePostFrame=0;
    int crossfadeSamples=0,captureTailSamples=0;
    double crossfadeMs=10,captureTailMs=10;
    bool recordLayer=false, freshUndoPrepared=false;
    bool playToEndEnabled=false,endCycleArmed=false,endCycleStarted=false;
    int endMasterFrames=0,endCyclePrevious=0;int64_t endMasterOrigin=0;
    float momentaryGain=1,momentaryTarget=1;int momentaryRemaining=0;
    float managerVolumeTarget=1,managerVolumeRender=1,managerPanTarget=0,managerPanRender=0;
    float panTarget = 0, renderPan = 0, panStep = 1, feedbackLevel = 1;
    double feedbackStep = 1;
    int playFadeFrames = 0, stopFadeFrames = 0;
    int transportRemaining=0, tailRemaining=0, tailPosition=0, nextTailPosition=0;
    float transportGain=0, transportTarget=0;
    bool previouslyAudible=false;
    std::array<float, 2> lastLoop {}, correction {};
    int fadeFrames = 0, fadeRemaining = 0;
    bool declick = false, previousMute = false;
    std::array<std::vector<float>, 2> history;
    int historyWrite = 0, historyCount = 0;
    bool playbackDetached=false;
    bool preserveTimeline=true,playOnBar=false,stopOnBar=false,prepareRecord=false,prepareOverdub=false;
    bool performancePending=false,waitForSound=false;
    Action performanceAction=Action::play;
    int countInBars=2;
    int64_t waitFrames=0,hostFrame=0,timelineOrigin=0;
    float triggerLevel=0.01f;
    bool armed = false, recordNextBar = false, overdubNextBar = true, captureSync = false, captureOverdub = false;
    int requestedBars=0,initialFixedTarget=0;
    int captureGrace = 0, previousPosition = 0;
    State previousState = State::empty;
    std::array<Slot, maxSlots> slots;
    std::array<UndoState,maxUndoDepth+1> undoHistory {};
    int undoSize=0, undoCursor=0, undoDepth=1;
    std::array<int, maxLayers> active {}, undoActive {};
    int count = 0, undoCount = 0, undoLength = 0;
    int recordingLimit = 0;
    int capacity = 0, length = 0, position = 0, clockBar = 0, recordingBar = 0, clockPhase = 0, recordingPhase = 0, grace = 0, passes = 0, pendingEnd = 0;
    uint64_t runFrames = 0, previousRunFrames = 0;
    int64_t overdubFrames = 0;
    uint32_t nextID = 1;
    double rate = 0;
    bool quantized = false, pendingStop = false, hostRunning = false;
    State state = State::empty;
    UndoKind undoKind = UndoKind::none;
};
} // namespace element
