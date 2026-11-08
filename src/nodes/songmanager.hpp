// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <element/processor.hpp>
#include <vector>
#include <element/signals.hpp>

namespace element {
class LooperManagerNode;
class LooperProcessor;
/** Coordinates prepared sibling graphs. GraphNode calls plan() before any graph
    operation, so section control has no MIDI/audio scheduling cycle. */
class SongManagerNode final : public Processor, private juce::AsyncUpdater {
public:
    SongManagerNode();
    ~SongManagerNode() override;
    struct Section { ProcessorPtr node; GraphNode* graph; };
    const std::vector<Section>& sections() const noexcept { return roster; }
    const std::vector<uint32>& orderIDs() const noexcept { return order; }
    void setOrder(const std::vector<uint32>&);
    void configure(GraphNode&); // message thread, graph sequence lock
    struct LoopTarget {
        uint32 section=0;
        ProcessorPtr node, ownerNode;
        LooperProcessor* looper=nullptr;
        LooperManagerNode* owner=nullptr;
    };
    const std::vector<LoopTarget>& loopTargets() const noexcept { return loops; }
    void selectLooper(const LooperProcessor*) noexcept; // message thread, validated by audio thread
    bool toggleLooperPlayback(LooperProcessor*) noexcept; // message thread
    bool selectionLocked() const noexcept;
    std::atomic<uint32> pendingSection {0};
    std::atomic<const LooperProcessor*> pendingLooper {nullptr};
    void serviceOfflineSelection(); // Host stopped, graph lock held.
    void jumpTo(int index) noexcept {jumpRequest.store(index);}
    std::atomic<int> switchTiming {1},countInBars {1},recordingProfile {-1},remainingBars {0},recordingNotice {0};
    std::array<std::atomic<int>,4> actionCC {{{-1},{-1},{-1},{-1}}}; // next, rewind, panic, restore
    void next() noexcept { nextRequest.store(true); }
    void panic() noexcept { panicRequest.store(true); }
    void restoreAudio() noexcept { restoreRequest.store(true); }
    void rewind() noexcept { rewindRequest.store(true); }
    std::atomic<int> pedalCC {30}, channel {1}, learn {0};
    std::atomic<bool> effective {false};
    std::atomic<bool> pending {false}, muted {false}, ended {false};
    std::atomic<int> currentIndex {-1};
    std::atomic<uint32> activeSection {0};
    // Display-only clock publication; never used to schedule audio.
    std::atomic<float> displayBarPhase {0}, displayBpm {0};
    std::atomic<int> displayBeat {0}, displayBeatsPerBar {4};
    std::atomic<bool> displayClockRunning {false};
    void refreshPorts() override;
    bool wantsContext() const noexcept override { return true; }
    bool supportsOversampling() const noexcept override { return false; }
    void prepareToRender(double, int) override;
    void releaseResources() override {}
    void render(RenderContext&) override;
    void getPluginDescription(juce::PluginDescription&) const override;
    void getState(juce::MemoryBlock&) override;
    void setState(const void*,int) override;
    void stopForPanic() noexcept; // Audio thread: stop structure without owning a mute latch.
    void plan(juce::MidiBuffer&, int frames) noexcept; // parent audio thread
    int panicOffset() const noexcept { return blockMuted ? 0 : muteAt; }
    void applyOutputMute(juce::AudioBuffer<float>&) const noexcept;
private:
    std::vector<Section> roster;
    std::vector<LoopTarget> loops;
    std::vector<SignalConnection> topologyConnections;
    void handleAsyncUpdate() override;
    uint32 targetGeneration=0;
    std::atomic<uint64_t> selectionRequest {0};
    int pendingTarget=-1, pendingIndex=-1;
    std::atomic<int> jumpRequest {-1};
    std::array<bool,4> actionHeld {};
    bool pendingImmediate=false;
    bool captureLocked(uint32 section) const noexcept;
    void cancelSelection() noexcept;
    void rebuildLoopTargets();
    std::vector<uint32> order, preparedOrder;
    std::atomic<bool> nextRequest {false}, panicRequest {false}, restoreRequest {false}, rewindRequest {false};
    double rate=0, pendingBeat=0;
    int64_t frame=0, heldSince=0;
    bool held=false, fired=false, initial=true, blockMuted=false;
    int muteAt=-1;
    juce::MidiBuffer filtered;
};
}
