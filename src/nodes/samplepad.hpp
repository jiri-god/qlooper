// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <element/processor.hpp>
#include <element/ui/nodeeditor.hpp>
#include <array>
namespace element {
class SamplePadNode final : public Processor
{
public:
    SamplePadNode();
    // Control/message thread only. Failed loads leave the previous sound intact.
    juce::Result loadSampleData (const void*, size_t, const juce::String& name);
    juce::Result loadFile (const juce::File&);
    juce::Result loadAudio (const juce::AudioBuffer<float>&, double sourceRate, const juce::String& name);
    juce::String sampleName() const;
    void trigger() noexcept { triggerRequest.store (true); }
    std::atomic<int> mapping { -1 }, channel { 1 }, kind { 0 }; // kind: 0 CC, 1 note
    std::atomic<bool> learn { false };
    std::atomic<float> gain { 1.f };
    void refreshPorts() override;
    void prepareToRender (double, int) override;
    void releaseResources() override;
    void render (RenderContext&) override;
    void renderBypassed (RenderContext&) override;
    void process (juce::AudioBuffer<float>&, juce::MidiBuffer&, bool bypass = false) noexcept;
    void getPluginDescription (juce::PluginDescription&) const override;
    void getState (juce::MemoryBlock&) override;
    void setState (const void*, int) override;
    bool supportsOversampling() const noexcept override { return false; }

private:
    mutable juce::CriticalSection controlLock;
    struct Sample
    {
        juce::AudioBuffer<float> audio, playback;
        double rate;
        juce::String name;
    };
    // Single control-thread producer, audio consumer. Ownership never reaches RT.
    std::array<std::unique_ptr<Sample>, 3> owned;
    std::atomic<Sample*> published { nullptr }, hazard { nullptr };
    Sample* current = nullptr;
    struct Voice
    {
        double position = -1;
        float velocity = 1;
        uint64_t age = 0;
    };
    std::array<Voice, 16> voices;
    uint64_t age = 0;
    double rate = 44100;
    bool held = false;
    int lastMapping = -2, lastKind = -1, lastChannel = -1;
    std::atomic<bool> triggerRequest { false };
    void startVoice (float) noexcept;
    static void convertSample (Sample&, double);
};
class SamplePadEditor final : public NodeEditor, private juce::Timer
{
public:
    explicit SamplePadEditor (const Node&);
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    SamplePadNode* pad;
    juce::Label name, status, midiLabel;
    juce::TextButton load { "Load sound..." }, hit { "TRIGGER" }, assign { "Assign pedal" }, clear { "Clear assignment" };
    juce::Slider volume;
    juce::ComboBox bank;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace element
