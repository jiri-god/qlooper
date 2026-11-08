// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <element/processor.hpp>
#include <element/ui/nodeeditor.hpp>
#include <map>

namespace element {
class LooperProcessor;
/** Stereo pass-through master recorder with optional taps of sibling node outputs. */
class RecorderNode final : public Processor, private juce::Thread
{
public:
    RecorderNode();
    ~RecorderNode() override;
    void refreshPorts() override;
    bool wantsContext() const noexcept override { return true; }
    void render (RenderContext&) override {} // identical audio, independent of recording
    void prepareToRender (double, int) override;
    void releaseResources() override;
    void getPluginDescription (juce::PluginDescription&) const override;
    void getState (juce::MemoryBlock&) override;
    void setState (const void*, int) override;
    struct Source { uint32 id; juce::String name; int channels; bool looper; };
    std::vector<Source> sources() const; // message thread
    std::map<uint32, bool> selected; // id -> loops-only; message thread, locked during a take
    std::map<uint32, juce::String> sourceLabels; // display names supplied by the editor
    juce::File destination, lastTake;
    juce::Result startRecording(); // message thread only
    void stopRecording() noexcept { accepting.store(false); }
    bool isRecording() const { return accepting.load(); }
    bool isBusy() const { return busy.load(); }
    double seconds() const { return rate > 0 ? double(captured.load()) / rate : 0; }
    juce::String statusText() const;
    void graphChanged() noexcept;
    // Called by the parent graph under its rendering lock. Never touches disk.
    void beginBlock (int) noexcept;
    void captureNode (Processor&, const juce::AudioBuffer<float>&, int channels,
                      float gainStart, float gainEnd, bool processed) noexcept;
    void endBlock() noexcept;
private:
    struct Track {
        uint32 id; juce::String name, filename; int channels, offset; bool loopsOnly;
        std::unique_ptr<juce::AudioFormatWriter> writer;
    };
    void run() override;
    void fail (int code) noexcept { error.store(code); accepting.store(false); }
    void writeManifest (bool complete);
    std::vector<Track> tracks;
    juce::AudioBuffer<float> staging, queued;
    juce::AbstractFifo fifo { 1 };
    std::atomic<bool> accepting { false }, busy { false }, starting { false }, audioBusy { false };
    std::atomic<int> error { 0 };
    std::atomic<int64_t> captured { 0 }, written { 0 };
    bool blockActive = false;
    int blockFrames = 0, maximumBlock = 0;
    double rate = 0;
};

class RecorderEditor final : public NodeEditor, private juce::Timer
{
public:
    explicit RecorderEditor (const Node&);
    void resized() override;
    void paint (juce::Graphics&) override;
private:
    struct Row { uint32 id; std::unique_ptr<juce::ToggleButton> selected, loopsOnly; };
    void timerCallback() override;
    void refreshSources();
    RecorderNode* recorder;
    juce::Label heading, status, path, help;
    juce::TextButton record { "RECORD" }, stop { "STOP" }, folder { "Choose folder..." }, show { "Show take" };
    juce::Viewport viewport;
    juce::Component list;
    std::vector<Row> rows;
    juce::String signature, lastError;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace element
