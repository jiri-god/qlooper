// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <element/juce/audio_processors.hpp>
#include <element/node.h>
#include <array>
namespace element {
// Private MIDI heartbeat: F0 7D 'J' 'C' <28-bit source, four 7-bit bytes> <on> F7.
// Each source publishes its state at offset zero every block and sample-timed edges.
// Rebuilding membership each block prevents stuck clicks after cable removal.
class CountInGate final : public juce::AudioPluginInstance {
public:
    CountInGate():AudioPluginInstance(BusesProperties().withInput("TICK audio",juce::AudioChannelSet::stereo(),true)
        .withOutput("Count-in audio",juce::AudioChannelSet::stereo(),true)){}
    const juce::String getName() const override{return "Count-in Gate";}
    void fillInPluginDescription(juce::PluginDescription& d) const override {
        d.name=getName();d.descriptiveName="TICK audio gated by Looper MIDI count-in";
        d.fileOrIdentifier="element.countInGate";d.pluginFormatName="Element";
        d.manufacturerName=EL_NODE_FORMAT_AUTHOR;d.version="1.0.0";
        d.numInputChannels=d.numOutputChannels=2;d.isInstrument=false;d.hasSharedContainer=false;
    }
    bool acceptsMidi() const override{return true;}bool producesMidi() const override{return false;}
    void prepareToPlay(double rate,int) override{gain=0;step=float(1/std::max(1.0,rate*0.002));}
    void releaseResources() override{}
    void processBlock(juce::AudioBuffer<float>& audio,juce::MidiBuffer& midi) override {
        std::array<uint32_t,128> ids{};std::array<bool,128> states{};int used=0,active=0;
        auto it=midi.begin();const auto end=midi.end();
        for(int f=0;f<audio.getNumSamples();++f) {
            while(it!=end && (*it).samplePosition<=f) {
                const auto m=*it;const auto* b=m.data;
                if(m.numBytes==10 && b[0]==0xf0 && b[1]==0x7d && b[2]=='J' && b[3]=='C' && b[8]<=1 && b[9]==0xf7) {
                    uint32_t id=0;for(int n=0;n<4;++n)id|=uint32_t(b[4+n]&127)<<(7*n);
                    int slot=0;while(slot<used && ids[slot]!=id)++slot;
                    if(slot<128){if(slot==used){ids[used++]=id;}const bool on=b[8]!=0;
                        if(on!=states[slot]){active+=on?1:-1;states[slot]=on;}}
                }
                ++it;
            }
            const float target=active>0?1.0f:0.0f;
            gain+=juce::jlimit(-step,step,target-gain);
            for(int c=0;c<audio.getNumChannels();++c)audio.setSample(c,f,audio.getSample(c,f)*gain);
        }
        midi.clear();
    }
    bool hasEditor() const override{return false;}juce::AudioProcessorEditor* createEditor() override{return nullptr;}
    double getTailLengthSeconds() const override{return 0.002;}
    int getNumPrograms() override{return 1;}int getCurrentProgram() override{return 0;}
    void setCurrentProgram(int) override{}const juce::String getProgramName(int) override{return {};}
    void changeProgramName(int,const juce::String&) override{}
    void getStateInformation(juce::MemoryBlock& b) override{b.reset();}
    void setStateInformation(const void*,int) override{gain=0;}
private:float gain=0,step=1;
};
}
