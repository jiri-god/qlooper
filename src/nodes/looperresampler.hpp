// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "looperengine.hpp"

namespace element {
/** Offline, band-limited conversion of a periodic loop. Never call on audio thread.
    Phase table bounds coefficient generation cost; the source wraps at its seam
    so no silence/padded tail or filter latency is inserted into the loop. */
namespace looper_resampling_detail {
inline void convertToLength (LooperEngine::Snapshot& snapshot, int n)
{
    const int oldLength = snapshot.length;
    // Use the rounded period itself: every converted layer closes at precisely
    // the same sample, without a fractional seam leftover from rate rounding.
    const double ratio = static_cast<double> (n) / oldLength;
    const double cutoff = ratio < 1 ? 0.94 * ratio : 1.0;
    constexpr int phases = 1024;
    constexpr double pi = 3.14159265358979323846;
    const int radius = static_cast<int> (std::ceil (32.0 / std::min (1.0, ratio)));
    const int taps = radius * 2;
    std::vector<double> kernel (static_cast<size_t> (phases) * taps);
    for (int phase = 0; phase < phases; ++phase)
    {
        double sum = 0;
        for (int tap = 0; tap < taps; ++tap)
        {
            const double x = tap - radius + 1 - static_cast<double> (phase) / phases;
            const double sinc = std::abs (x) < 1.0e-12 ? cutoff : std::sin (pi * cutoff * x) / (pi * x);
            const double window = std::abs (x) >= radius ? 0.0 : 0.42 + 0.5 * std::cos (pi * x / radius) + 0.08 * std::cos (2 * pi * x / radius);
            sum += kernel[static_cast<size_t> (phase) * taps + tap] = sinc * window;
        }
        for (int tap = 0; tap < taps; ++tap)
            kernel[static_cast<size_t> (phase) * taps + tap] /= sum;
    }
    for (auto& layer : snapshot.layers)
    {
        std::array<std::vector<float>, 2> converted;
        for (auto& channel : converted) channel.resize (n);
        for (int i = 0; i < n; ++i)
        {
            const double source = static_cast<double> (i) * oldLength / n;
            int center = static_cast<int> (source);
            int phase = static_cast<int> (std::llround ((source - center) * phases));
            if (phase == phases) { phase = 0; ++center; }
            const auto* weights = kernel.data() + static_cast<size_t> (phase) * taps;
            int index = (center - radius + 1) % oldLength;
            if (index < 0) index += oldLength;
            double left = 0, right = 0;
            for (int tap = 0; tap < taps; ++tap)
            {
                left += layer.audio[0][index] * weights[tap];
                right += layer.audio[1][index] * weights[tap];
                if (++index == oldLength) index = 0;
            }
            converted[0][i] = static_cast<float> (left);
            converted[1][i] = static_cast<float> (right);
        }
        layer.audio.swap (converted);
        layer.coverageStart = std::min(n-1, int(std::llround(layer.coverageStart * ratio)));
        if(layer.coverageFrames >= 0)
            layer.coverageFrames = std::min(n, int(std::llround(layer.coverageFrames * ratio)));
        if(layer.captureEnd>=0) layer.captureEnd=std::min(n-1,int(std::llround(layer.captureEnd*ratio)));
        // Capture handles are finite audio, not periodic loops. Zero extension
        // avoids wrapping their end back into their beginning during conversion.
        for(auto* handle : {&layer.pre, &layer.post}) {
            const int oldFrames=int((*handle)[0].size());
            const int newFrames=int(std::llround(oldFrames*ratio));
            std::array<std::vector<float>,2> output;
            for(auto& channel : output) channel.resize(newFrames);
            for(int i=0;i<newFrames;++i) {
                const double source=double(i)/ratio;
                int center=int(source), phase=int(std::llround((source-center)*phases));
                if(phase==phases) {phase=0; ++center;}
                const auto* weights=kernel.data()+size_t(phase)*taps;
                for(int c=0;c<2;++c) {
                    double value=0;
                    for(int tap=0;tap<taps;++tap) {
                        const int index=center-radius+1+tap;
                        if(index>=0 && index<oldFrames) value+=(*handle)[c][index]*weights[tap];
                    }
                    output[c][i]=float(value);
                }
            }
            handle->swap(output);
        }
    }
    snapshot.length = n;
}
} // namespace looper_resampling_detail
inline void resampleLooperSnapshot (LooperEngine::Snapshot& snapshot, double targetRate)
{
    if (snapshot.rate > 0 && snapshot.rate != targetRate) {
        snapshot.fixedLength = std::min(int(targetRate * LooperEngine::maxDurationSeconds), int(std::llround(snapshot.fixedLength * targetRate / snapshot.rate)));
        snapshot.phaseOffset = std::llround(snapshot.phaseOffset * targetRate / snapshot.rate);
        if (snapshot.fixedLength > 0) snapshot.phaseOffset %= snapshot.fixedLength;
    }
    if (snapshot.rate <= 0 || snapshot.rate == targetRate || snapshot.length == 0)
    {
        snapshot.rate = targetRate;
        return;
    }
    const int n = std::min (static_cast<int> (targetRate * LooperEngine::maxDurationSeconds), std::max (1, static_cast<int> (std::llround (snapshot.length * targetRate / snapshot.rate))));
    // Bound filter-table memory even for unusual device-rate ratios. Each
    // decimation stage is filtered; the final length is computed only once.
    while (static_cast<int64_t> (snapshot.length) > static_cast<int64_t> (n) * 4)
        looper_resampling_detail::convertToLength (snapshot, (snapshot.length + 1) / 2);
    looper_resampling_detail::convertToLength (snapshot, n);
    // Very short rounded periods can have a slightly different conversion
    // ratio from the device rate. Respect finite capture-handle storage limits.
    for(auto& layer : snapshot.layers) {
        for(auto& channel : layer.pre) channel.resize(std::min(channel.size(),size_t(std::max(1,int(targetRate*0.25)))));
        for(auto& channel : layer.post) channel.resize(std::min(channel.size(),size_t(std::max(1,int(targetRate*2.0)))));
    }
    snapshot.rate = targetRate;
}
} // namespace element
