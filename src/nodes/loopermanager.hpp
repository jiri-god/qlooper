// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "nodes/managereditorstyle.hpp"
#include <element/processor.hpp>
#include <element/ui/nodeeditor.hpp>
#include "nodes/looper.hpp"
#include <map>

namespace element {
// Refreshed only during graph rebuild under its rendering-sequence lock.
class LooperManagerNode final : public Processor
{
public:
    LooperManagerNode();
    void refreshPorts() override;
    bool wantsContext() const noexcept override { return true; }
    bool supportsOversampling() const noexcept override { return false; }
    void prepareToRender (double, int) override;
    void releaseResources() override {}
    void render (RenderContext&) override;
    void getPluginDescription (juce::PluginDescription&) const override;
    void getState (juce::MemoryBlock&) override;
    void setState (const void*, int) override;
    void configure (GraphNode&); // graph sequence lock held, message thread
    void move (uint32 id, int direction); // message thread
    void serviceOfflineSelection(); // Host stopped, graph lock held.
    void select (uint32 id) { requested.store (id); }
    void playAll() { playRequested.store (true); }
    void stopAll() { stopRequested.store (true); }
    void selectForSection(uint32 id, int offset,int profile=-1) noexcept; // audio thread, before render
    void cancelPendingForSection(int offset, bool start) noexcept; // graph audio thread
    void resetAll() { resetRequested.store(true); }
    bool setMaster(uint32 id);
    bool setSyncMode(uint32 id, int mode);
    int syncMode(uint32 id) const { auto it=syncModes.find(id); return it==syncModes.end()?0:it->second; }
    uint32 masterID() const { return master.load(); }
    bool structureLocked() const;
    juce::String masterChangeReason(uint32 id) const;
    std::atomic<bool> masterReanchor {false};
    std::atomic<int> masterLength {0};
    std::atomic<float> groupVolume {1},groupPan {0};
    std::atomic<uint64_t> stopGeneration {0};
    std::atomic<bool> transportRunning {false};
    void step (int delta) { stepRequested.store (delta); }
    std::atomic<uint32> handoffTarget {0};
    std::atomic<bool> handoffNeedsClock {false};
    uint32 selectedID() const { return selected.load(); }
    bool isLocked() const;
    enum class BoxAction { playStop, record, rewind, mute, solo };
    static constexpr int boxActions=5;
    struct MemberControl {
        std::atomic<int> profile {1}, request {-1};
        std::atomic<bool> solo {false},momentary {false},uiHeld {false},playToEnd {false};
        bool midiHeld=false,playingHeld=false; // audio thread

        std::array<std::atomic<int>,boxActions> midi {}; // packed (CC+1) | (channel<<8), zero disabled
    };
    struct Target { ProcessorPtr node; LooperProcessor* looper; bool active = false; int mode = 0; std::shared_ptr<MemberControl> control; };
    bool requestBox(uint32,BoxAction) noexcept;
    bool playToEnd(uint32 id) const noexcept;
    void setPlayToEnd(uint32 id,bool value) noexcept;
    bool momentary(uint32 id) const noexcept;
    void setMomentary(uint32 id,bool value) noexcept;
    void holdBox(uint32 id,bool down) noexcept;
    int recordingProfile(uint32) const noexcept;
    bool setRecordingProfile(uint32,int) noexcept;
    bool soloed(uint32) const noexcept;
    LooperProcessor::MidiMapping getBoxMapping(uint32,int) const noexcept;
    bool setBoxMapping(uint32,int,LooperProcessor::MidiMapping) noexcept;
    bool setGlobalMapping(int action,int cc,int midiChannel) noexcept; // 1 previous,2 next,3 stop,4 play
    void learnBox(uint32 id,int action) noexcept { lastLearnRejected=false;learningAction=action;learningTarget=id; }
    std::atomic<bool> lastLearnRejected {false};
    std::atomic<uint32> learningTarget {0};
    std::atomic<int> learningAction {-1};
    std::atomic<int> shortTiming {0},longTiming {0},shortBars {1},longBars {1},navigationProfile {1},longProfile {-1};
    std::atomic<bool> pendingSelectionOnly {false};
    std::atomic<int> navigationRemaining {0}, displayNavigationProfile {-1};
    const std::vector<Target>& members() const { return targets; } // message thread
    std::atomic<int> previousCC { 24 }, nextCC { 25 }, stopAllCC { 27 }, playAllCC { 28 }, channel { 1 };
    std::atomic<int> learn { 0 }; // 1 previous, 2 next, 3 stop all, 4 play all; positive CC edges only
    std::atomic<bool> blocked { false }, playPending { false }, playNeedsClock { false };
private:
    void choose (uint32 id);
    void advance (int delta);
    void cancelHandoff() noexcept { handoffTarget=0; handoffNeedsClock=false; handoffDirection=0; pendingSelectionOnly=false; navigationRemaining=0; }
    uint32 handoffSource=0;
    int handoffDirection=0;
    bool handoffSourceActive=false;
    int handoffProfile=1;
    int handoffNumerator=4, handoffDenominator=4;
    double handoffBeatsPerSample=0, handoffBarBeats=0, handoffBarOrigin=0;
    int64_t handoffFrame=0, gestureFrame=0;
    struct StopHold { int channel=0, cc=0; int64_t start=0; };
    StopHold stopHold {};
    struct NavigationGesture { int direction=0; int64_t start=0; bool fired=false; };
    std::array<std::array<NavigationGesture,128>,16> navigation {};
    std::shared_ptr<std::atomic<uint32_t>> captureGate=std::make_shared<std::atomic<uint32_t>>(0);
    std::vector<Target> targets;
    std::map<uint32,std::shared_ptr<MemberControl>> memberControls;
    MemberControl* controlFor(uint32) const noexcept;
    void updateSoloMute() noexcept;
    std::vector<uint32> order;
    std::map<uint32,int> syncModes;
    std::atomic<uint32> master {0};
    std::atomic<bool> resetRequested {false};
    uint32 resetEpoch = 0;
    uint64_t clockEpoch = 0;
    int64_t sharedFrame = 0;
    struct SectionEvent { int offset=0; bool start=false; uint32 selection=0;int profile=-1; };
    std::array<SectionEvent,8> sectionEvents {};
    int sectionEventCount=0;
    std::atomic<uint32> selected { 0 }, requested { 0 };
    std::atomic<int> stepRequested { 0 };
    std::array<std::array<bool, 128>, 16> held {};
    std::array<std::array<uint32,128>,16> gestureTargets {};
    std::array<std::array<uint8,128>,16> gestureCommands {};
    std::atomic<bool> stopRequested { false }, playRequested { false };
    double renderRate = 0, pendingBeat = 0, expectedBeat = 0;
    bool hadClock = false, timelineValid = false;
    int64_t timelineFrames = 0; // elapsed at start of this audio block

    juce::MidiBuffer outgoing;
};

class LooperManagerEditor final : public NodeEditor
{
public:
    explicit LooperManagerEditor(const Node&);
    ~LooperManagerEditor() override;
    void resized() override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace element
