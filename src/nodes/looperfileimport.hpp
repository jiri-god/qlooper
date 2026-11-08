// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "looper.hpp"
#include <element/juce/audio_formats.hpp>
namespace element {
struct LooperFileImport {
    std::unique_ptr<LooperProcessor::ImportJob> job;
    juce::String error;
};
// Finite input: zero extension, no periodic seam and no time stretching.
inline std::array<std::vector<float>,2> resampleImportedAudio(const juce::AudioBuffer<float>& input, double sourceRate, double targetRate)
{
    const int old=input.getNumSamples();
    const int n=int(std::llround(old*targetRate/sourceRate));
    std::array<std::vector<float>,2> out;
    for(auto& ch:out)ch.resize(size_t(n));
    if(sourceRate==targetRate) {
        for(int c=0;c<2;++c)std::copy_n(input.getReadPointer(c),n,out[c].begin());return out;
    }
    const double ratio=targetRate/sourceRate,cutoff=ratio<1?ratio*.94:1;
    const int radius=int(std::ceil(32/std::min(1.0,ratio))),taps=2*radius,phases=1024;
    constexpr double pi=3.14159265358979323846;
    std::vector<double> kernel(size_t(taps)*phases);
    for(int p=0;p<phases;++p) {
        double sum=0;
        for(int t=0;t<taps;++t) {
            const double x=t-radius+1-double(p)/phases;
            const double sinc=std::abs(x)<1e-12?cutoff:std::sin(pi*cutoff*x)/(pi*x);
            const double w=std::abs(x)>=radius?0:.42+.5*std::cos(pi*x/radius)+.08*std::cos(2*pi*x/radius);
            sum+=kernel[size_t(p)*taps+t]=sinc*w;
        }
        for(int t=0;t<taps;++t)kernel[size_t(p)*taps+t]/=sum;
    }
    for(int i=0;i<n;++i) {
        const double x=i/ratio;int center=int(x),phase=int(std::llround((x-center)*phases));
        if(phase==phases){phase=0;++center;}
        for(int c=0;c<2;++c) {
            double value=0;
            for(int t=0;t<taps;++t) {const int j=center-radius+1+t;if(j>=0 && j<old)value+=input.getSample(c,j)*kernel[size_t(phase)*taps+t];}
            out[c][i]=float(value);
        }
    }
    return out;
}
inline LooperFileImport prepareLooperFile(const juce::File& file, const LooperProcessor::View& v,int recordings,int parts)
{
    using namespace juce;
    try {
        if(v.capacity<=0 || v.rate<=0)return {{},"Initialize an audio device before importing."};
        if(v.importLength<0)return {{},"Set a valid master / Fixed Bar length before importing."};
        if(v.recordings>=recordings)return {{},"Recording limit reached. Import was not added."};
        AudioFormatManager formats;formats.registerBasicFormats();
        std::unique_ptr<AudioFormatReader> reader(formats.createReaderFor(file));
        if(!reader)return {{},"Unsupported or unreadable audio file."};
        if(reader->numChannels<1 || reader->numChannels>2)return {{},"Import supports mono or stereo files; convert multichannel audio first."};
        if(!std::isfinite(reader->sampleRate)||reader->sampleRate<8000||reader->sampleRate>384000||reader->lengthInSamples<1)
            return {{},"Invalid or unsupported sample rate / empty audio."};
        const double converted=std::round(double(reader->lengthInSamples)*v.rate/reader->sampleRate);
        const int target=v.importLength>0?v.importLength:converted<=v.capacity?int(converted):0;
        if(target<=0||target>v.capacity||converted<1||converted>double(target)*parts||reader->lengthInSamples>std::numeric_limits<int>::max())
            return {{},"File exceeds the duration or One Take layer limit. Nothing was imported."};
        const int count=int(std::ceil(converted/target));
        if(v.count+count>LooperEngine::maxLayers)return {{},"Layer capacity exceeded."};
        AudioBuffer<float> input(2,int(reader->lengthInSamples));
        if(!reader->read(&input,0,input.getNumSamples(),0,true,true))return {{},"Could not decode the complete file."};
        if(reader->numChannels==1)input.copyFrom(1,0,input,0,0,input.getNumSamples());
        for(int c=0;c<2;++c)for(int i=0;i<input.getNumSamples();++i)if(!std::isfinite(input.getSample(c,i)))return {{},"File contains non-finite audio samples."};
        auto audio=resampleImportedAudio(input,reader->sampleRate,v.rate);
        auto job=std::make_unique<LooperProcessor::ImportJob>();job->expectedMode=v.loopMode;auto& plan=job->plan;
        plan.rate=v.rate;plan.capacity=v.capacity;plan.previousLength=v.length;plan.target=target;
        plan.mode=v.linkMode;plan.base=v.linkBase;plan.bars=v.requestedBars;
        for(int i=0;i<v.count;++i)plan.ids.push_back(v.layers[i].id);
        plan.parts.resize(size_t(count));
        for(int part=0;part<count;++part)for(int c=0;c<2;++c) {
            auto& dst=plan.parts[size_t(part)][c];dst.resize(size_t(v.capacity),0);
            const int start=part*target,frames=std::min(target,int(audio[c].size())-start);
            std::copy_n(audio[c].begin()+start,frames,dst.begin());
        }
        return {std::move(job),{}};
    } catch(const std::bad_alloc&) {return {{},"Not enough memory for this import. Nothing was changed."};}
      catch(...) {return {{},"Audio import failed. Nothing was changed."};}
}
}
