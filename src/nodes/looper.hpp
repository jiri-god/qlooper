// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <element/juce/audio_processors.hpp>
#include <element/node.hpp>
#include "nodes/looperengine.hpp"
#include <atomic>
#include <mutex>
#include <future>
#include "nodes/looperhistory.hpp"

namespace element {
/** Built-in stereo layered looper; settings may be edited on the message thread. */
class LooperProcessor final : public juce::AudioPluginInstance, private juce::Timer
{
public:
    static constexpr int midiActions = 9;
    struct MidiMapping
    {
        int cc = -1, channel = 0;
    };
    // 0/1 are the permanent Record/Overdub buttons; 2/3 are MIDI-only.
    struct RecordingControl {
        int shortAction=0, holdAction=2, start=1, bars=2;
        bool finishFixed=false, overwrite=false, hearPrevious=false, enabled=true;
        int loops=0;
        bool metronome=false, feedbackWithinTake=true, stopAfter=false;
    };
    RecordingControl recordingControl(int index) const noexcept;
    bool setRecordingControl(int index, RecordingControl);
    bool recordingSettingsLocked() const noexcept { return captureBusy.load() || captureRequested.load()>0; }
    bool resetSettings();
    static int controlMidi(int index) noexcept { return index==0?1:index==1?2:index+4; }
    struct View
    {
        int loopMode=0, controller=-1;
        int capacity=0, importLength=0, linkMode=0, linkBase=0, requestedBars=0, masterLength=0, masterPosition=0;
        int state = 0, length = 0, position = 0, count = 0, recordings = 0, barFrames=0, captureTarget=0, capturedProgress=0;
        int displaySyncPosition=0,displayMasterPosition=0;
        int waitingAction=0,countIn=0,syncPosition=0; bool soundWaiting=false;
        bool momentaryMuted=false;
        bool fixedCompatible=true,overwrite=false,transportWaiting=false;
        double rate = 44100, bpm = 0;
        int numerator = 4, denominator = 4;
        bool pending = false, undo = false, redo = false, finishingEdit = false, armed = false, needsClock = false, captureOverdub = false;
        std::array<LooperEngine::LayerInfo, LooperEngine::maxLayers> layers {};
    };
    // Manager bindings are rebuilt under the graph rendering lock. UI observes atomics.
    static constexpr int managerStopAllCommand = 7, managerPlayAllCommand = 8, managerRestartAllCommand = 9;
    static constexpr int managerFinishCaptureCommand=24, managerBeginOverdubCommand=25;
    std::atomic<bool> activeRequested {false};
    std::atomic<bool> transportStartRequested {false}, transportStartSupported {false};
    std::atomic<bool> preserveTimeline {true},playOnBar {false},stopOnBar {false};
    std::atomic<bool> recordPreparation {false},overdubPreparation {false},countInClick {false};
    std::atomic<int> countInBars {2};
    std::atomic<float> triggerDb {-40};
    std::atomic<bool> managerCanOverdub { true }, managerSoloMuted {false};
    static constexpr int managerProfileFirst=35, managerRewindCommand=39, managerMomentaryStart=40, managerMomentaryStop=41;
    static constexpr int managerGestureFirst = 20, managerGestureLast = 33;
    // Called on the graph audio thread before processBlock. Events are bounded and sample-timed.
    void scheduleSectionEvent(int offset, bool start) noexcept;
    // Audio-thread publication, independent of the UI snapshot FIFO.
    std::atomic<float> managerVolume {1}, managerPan {0};
    std::atomic<bool> managerMomentaryRelease {false};
    bool lastMomentaryEnabled=false;
    bool managerMomentaryActive=false; // audio thread only
    std::atomic<bool> managerReanchor {false}, managerTransportPending {false};
    std::atomic<int> managerState { 0 }, managerLength { 0 }, managerFixedLength { 0 };
    std::atomic<int64_t> managerOrigin { 0 }, managerClockFrame { 0 };
    std::atomic<uint32_t> managerResetSeen { 0 };
    std::atomic<uint64_t> managerClockEpoch { 0 };
    void setManagerClockEpoch(uint64_t epoch) noexcept { linkClockEpoch = epoch; }
    // Manager calls only on graph audio thread, before this processor's block.
    void setManagerContext(int mode, int base, int64_t frame, int64_t origin, uint32_t generation) noexcept
    { linkContext = {mode,base,frame,origin,generation,linkContext.serial+1}; offeredResetGeneration.store(generation); }
    std::atomic<uint64_t> managerRunFrames { 0 };
    std::atomic<uint32_t> managerOwner { 0 }, managerTarget { 0 };
    std::shared_ptr<std::atomic<uint32_t>> managerCaptureGate; // graph-lock configuration, audio-only claims
    std::atomic<bool> managerSelected { false }, captureBusy { false };
    std::atomic<bool> managerDisplayRunning {false}, managerMomentaryEnabled {false}, managerPlayToEnd {false};
    std::atomic<bool> pedalSectionActive {true};
    std::atomic<int> captureRequested { 0 };
    const juce::AudioBuffer<float>* recorderLoopAudio (int frames) const noexcept
    { return loopCaptureValid && loopCapture.getNumSamples() >= frames ? &loopCapture : nullptr; }
    LooperProcessor();
    ~LooperProcessor() override;
    bool requestHistory(bool select=false) noexcept;
    bool setHistoryLimits(int seconds,double minimum);
    int getHistorySeconds() const noexcept {return historySeconds.load();}
    double getHistoryMinimum() const noexcept {return historyMinimum.load();}
    struct HistorySelection { uint64_t generation=0; double rate=0; std::array<std::vector<float>,2> audio; std::array<float,1024> low{},high{}; };
    std::shared_ptr<const HistorySelection> historySelection; // message thread
    uint64_t historySelectionVersion=0;
    juce::String historyMessage; // message thread
    bool importHistorySelection(int first,int last);

    const juce::String getName() const override { return "Looper"; }
    void fillInPluginDescription (juce::PluginDescription&) const override;
    void prepareToPlay (double, int) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    double getTailLengthSeconds() const override { return 60.0; }
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    juce::AudioProcessorEditor* createEditorForNode(const Node&);
    juce::AudioProcessorEditor* createLayersEditorForNode(const Node&);
    juce::AudioProcessorEditor* createCompactEditorForNode(const Node&, std::function<void()> showLayers);
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;
    void getCurrentProgramStateInformation (juce::MemoryBlock& b) override { b.reset(); }
    void setCurrentProgramStateInformation (const void*, int) override {}
    bool request (LooperEngine::Action) noexcept;
    bool editLayer (uint32_t id, int field, float value) noexcept;
    /** Consume the latest queued display snapshot; message thread only. */
    View view() const;
    // Host message thread only, with device stopped and graph sequence locked.
    void serviceOfflineControls();
    std::atomic<bool> audioAvailable {true};
    juce::String status() const;
    juce::Result exportLayer (uint32_t id, const juce::File&);
    struct ImportJob { uint64_t historyGeneration=0; int expectedMode=-1;bool switchToFree=false; LooperEngine::ImportPlan plan; std::atomic<int> result {0}; };
    bool submitImport(std::unique_ptr<ImportJob>);
    bool hasHistoryImport() const noexcept {return historyWaitingImport;}
    bool hasImport() const noexcept {return importOwned!=nullptr;}
    int collectImport(); // 0 pending/none, 1 success, -1 changed/busy
    struct Waveform { uint32_t id=0;std::array<float,1024> low{},high{}; };
    std::shared_ptr<const Waveform> liveHistoryWaveform();
    using Waveforms=std::vector<std::shared_ptr<const Waveform>>;
    Waveforms waveforms(); // bounded thumbnails, no full audio copy; non-audio thread
    std::atomic<bool> layerPreviewVisible {true};
    float getVolume() const { return volume->get(); }
    void setVolume (float);
    void setMonitor (bool value) { monitor.store (value); }
    bool getMonitor() const { return monitor.load(); }
    void setBypassed (bool value) { bypassed.store (value); }
    bool getBypassed() const { return bypassed.load(); }
    void setMuted (bool value) { muted.store (value); }
    bool getMuted() const { return muted.load(); }
    void setRecordNextBar (bool value) { recordNextBar.store(value); }
    bool getRecordNextBar() const { return recordNextBar.load(); }
    void setQuantized (bool value) { quantized.store (value);fixedMode.store(false); }
    void setLoopMode(int mode) { fixedMode.store(mode==2);quantized.store(mode!=0); }
    int getLoopMode() const { return fixedMode.load()?2:quantized.load()?1:0; }
    void setFixedBarCount(int count) { fixedBarCount.store(juce::jlimit(1,256,count)); }
    int getFixedBarCount() const { return fixedBarCount.load(); }
    void setOverdubNextBar(bool value) { overdubNextBar.store(value); }
    bool getOverdubNextBar() const { return overdubNextBar.load(); }
    bool getQuantized() const { return quantized.load(); }
    void setGraceMs (int value) { graceMs.store (juce::jlimit (0, 500, value)); }
    int getGraceMs() const { return graceMs.load(); }
    void setAutoPasses (int value) { autoPasses.store (juce::jlimit (0, 64, value)); }
    int getAutoPasses() const { return autoPasses.load(); }
    void setPan(float value) { pan.store(juce::jlimit(-1.0f,1.0f,value)); }
    float getPan() const { return pan.load(); }
    void setFeedback(float value) { overdubFeedback.store(juce::jlimit(0.0f,1.0f,value)); }
    float getFeedback() const { return overdubFeedback.load(); }
    void setPlayFade(int value) { playFadeMs.store(juce::jlimit(0,10000,value)); }
    int getPlayFade() const { return playFadeMs.load(); }
    void setStopFade(int value) { stopFadeMs.store(juce::jlimit(0,10000,value)); }
    int getStopFade() const { return stopFadeMs.load(); }
    void setCrossfade(int value) { crossfadeMs.store(juce::jlimit(0,250,value)); }
    int getCrossfade() const { return crossfadeMs.load(); }
    void setCaptureTail(int value) { captureTailMs.store(juce::jlimit(0,2000,value)); }
    int getCaptureTail() const { return captureTailMs.load(); }
    void setTakeFades(bool v) { takeFades.store(v); }
    bool getTakeFades() const { return takeFades.load(); }
    void setTakeFeedback(bool v) { takeFeedback.store(v); }
    bool getTakeFeedback() const { return takeFeedback.load(); }
    bool setCapacityLimits(int recordings,int parts,bool rolling);
    bool setMemoryLimits(int seconds,int recordings,int parts,bool rolling,int depth=-1);
    int getDurationSeconds() const noexcept { return durationSeconds.load(); }
    size_t getMemoryBytes() const noexcept { return allocatedBytes.load()+historyAllocatedBytes.load(); }
    int getRecordingLimit() const { return recordingLimit.load(); }
    int getTakeLimit() const { return takeLimit.load(); }
    bool getRolling() const { return rollOldest.load(); }
    void setUndoDepth(int);
    int getUndoDepth() const { return undoDepth.load(); }
    bool setMidiMapping (int action, MidiMapping);
    MidiMapping getMidiMapping (int action) const;

protected:
    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainInputChannelSet() == juce::AudioChannelSet::stereo() && l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

private:
    void timerCallback() override;
    void triggerHistory(bool select) noexcept;
    struct HistoryTicket { uint64_t generation=0,end=0;int64_t timelineOrigin=0;bool alignTimeline=false;int frames=0;bool select=false;View view; } historyTicket;
    std::atomic<uint64_t> historyGeneration{1};
    uint64_t historyDisplayGeneration=1;
    void invalidateHistory() noexcept; // lifecycle under historyMutex + Exclusive
    std::atomic<int> historyRequest{0}; // 0 idle, 1 filling, 2 ready, 3 copying
    std::atomic<size_t> historyAllocatedBytes{0};
    int64_t historySyncAvailable=0,expectedHostFrame=0;
    int historyClockBar=0;bool historyClockValid=false;
    std::atomic<int> historySeconds{30};
    std::atomic<double> historyMinimum{1.0};
    std::shared_ptr<LooperHistory> history;
    std::mutex historyMutex; // lifecycle/readers only, never audio
    struct HistoryResult {uint64_t generation=0;std::shared_ptr<HistorySelection> selection;std::unique_ptr<ImportJob> job;juce::String error;};
    std::future<HistoryResult> historyFuture;
    bool historyWaitingImport=false;
    class Exclusive;
    void consumeImport() noexcept;
    std::unique_ptr<ImportJob> importOwned;
    std::atomic<ImportJob*> importPending {nullptr};
    struct Command
    {
        int kind = 0;
        uint32_t id = 0;
        int field = 0;
        float value = 0;
        LooperEngine::Action action = LooperEngine::Action::stop;
    };
    bool enqueue (Command) noexcept;
    void applyAction (LooperEngine::Action) noexcept;
    void publish() noexcept;
    void pedalEdge(int action, bool pressed) noexcept;
    void cancelGestures() noexcept;
    void fireHold(int gesture) noexcept;
    void applySectionEvent(bool start) noexcept;
    struct PedalGesture { bool down=false, fired=false; int64_t elapsed=0; };
    std::array<PedalGesture,6> gestures {};
    std::array<std::atomic<uint32_t>,4> recordingControls;
    int currentController=-1;
    RecordingControl activeControl;
    bool claimManagerCapture() noexcept;
    void controlAction(int index, bool hold=false, int lookback=0) noexcept;
    void configureCapture(int index) noexcept;
    int transportWaitController=-1, transportWaitLookback=0;
    bool transportWaitHold=false, hostPlaying=false, captureWasRunning=false;
    int displaySyncPosition=0,displayMasterPosition=0,displayLinkMode=-1;
    bool heldRecordActive=false;
    void stopAfterCapture() noexcept;

    bool pedalClearPending = false;
    struct SectionEvent { int offset=0; bool start=false; };
    std::array<SectionEvent,8> sectionEvents {};
    int sectionEventCount=0, sectionEventCursor=0;
    void publishSnapshotView() noexcept;
    LooperEngine::Snapshot takeSnapshot();
    struct SnapshotRequest;
    static void snapshotWrite (void*, float*, float) noexcept;
    // Single non-audio consumer, single audio producer; the middle slot transfers ownership.
    std::array<LooperEngine::SnapshotView, 3> snapshotViews;
    std::atomic<int> snapshotMiddle { 1 };
    int snapshotBack = 2, snapshotFront = 0;
    std::atomic<uint64_t> snapshotGate { 0 }; // bit0 callback, bit1 pinned snapshot, upper bits epoch
    std::atomic<int> snapshotReaders { 0 };
    std::atomic<SnapshotRequest*> snapshotRequest { nullptr };
    std::recursive_mutex snapshotMutex; // never acquired by processBlock

    void processRange (juce::AudioBuffer<float>&, int, int) noexcept;
    struct LinkContext { int mode=0,base=0; int64_t frame=0,origin=0; uint32_t generation=0; uint64_t serial=0; } linkContext;
    uint32_t resetGeneration = 0, lastManagerOwner = 0;
    uint64_t linkClockEpoch = 0, consumedClockEpoch = 0;
    std::atomic<uint32_t> offeredResetGeneration { 0 };
    uint64_t consumedContext = 0;
    std::atomic<bool> viewPublishPending {false};
    LooperEngine engine;
    juce::AudioBuffer<float> loopCapture;
    bool loopCaptureValid = false;
    double preparedRate = 0;
    juce::AudioParameterFloat* volume = nullptr;
    juce::SmoothedValue<float> gain;
    std::atomic<bool> occupied { false }, monitor { true }, muted { false }, quantized { false }, bypassed { false }, recordNextBar { true };
    std::atomic<float> pan {0}, overdubFeedback {1};
    std::atomic<int> playFadeMs {0}, stopFadeMs {0}, undoDepth {1};
    std::atomic<int> crossfadeMs {10}, captureTailMs {10};
    std::atomic<bool> takeFades {true}, takeFeedback {true};
    std::atomic<int> recordingLimit {10}, takeLimit {4}, durationSeconds {60};
    std::atomic<size_t> allocatedBytes {0};
    std::atomic<bool> rollOldest {true};
    std::atomic<bool> fixedMode {false},overdubNextBar {true};
    std::atomic<int> fixedBarCount {4};
    std::atomic<int> graceMs { 100 }, autoPasses { 0 };
    std::array<std::atomic<int>, midiActions> mappings;
    juce::AbstractFifo commands { 64 };
    std::array<Command, 64> actions;
    std::array<std::array<bool, 128>, 16> held {};
    // UI receives a bounded lock-free FIFO of complete coherent snapshots.
    mutable juce::AbstractFifo views { 4 };
    mutable std::array<View, 4> viewData;
    mutable View cachedView;
    juce::MidiBuffer countInMidi;
    uint32_t countInSource=0;
    bool lastCountIn=false;
    void publishCountIn(int offset,bool force=false) noexcept;
    bool activeBypass = false;
    bool resetHeldAfterGap = false;
    double hostBpm = 0;
    int hostNumerator = 4, hostDenominator = 4;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LooperProcessor)
};
} // namespace element
