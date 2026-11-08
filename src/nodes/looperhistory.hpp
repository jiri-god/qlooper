// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <array>
#include <bit>
#include <memory>
#include <vector>
#include <algorithm>
#include <cmath>
namespace element {
/** One audio writer, non-audio readers. Extra ring space gives a frozen range
    time to be copied. Sequence stamps reject overwritten/torn reads; never wait
    or allocate in push(). The reader reports expiration rather than wrong audio. */
class LooperHistory {
    struct Cell { std::atomic<uint64_t> sequence{0}, stereo{0}; };
    std::unique_ptr<Cell[]> cells;
    uint64_t cursor=0;
public:
    const double rate;
    const int window, capacity;
    std::atomic<uint64_t> written{0};
    LooperHistory(double hz,int seconds):rate(hz),window(int(hz*seconds)),capacity(window*2),cells(nullptr) {
        cells=std::make_unique<Cell[]>(size_t(capacity));
    }
    size_t memoryBytes() const noexcept {return size_t(capacity)*sizeof(Cell);}
    void push(float l,float r) noexcept {
        auto& cell=cells[size_t(cursor%capacity)];
        cell.sequence.store(cursor*2+1,std::memory_order_seq_cst);
        const uint64_t bits=uint64_t(std::bit_cast<uint32_t>(std::isfinite(l)?l:0.f))
            |(uint64_t(std::bit_cast<uint32_t>(std::isfinite(r)?r:0.f))<<32);
        cell.stereo.store(bits,std::memory_order_seq_cst);
        cell.sequence.store(cursor*2+2,std::memory_order_seq_cst);
        written.store(++cursor,std::memory_order_release);
    }
    bool sample(uint64_t frame,float& l,float& r) const noexcept {
        const auto& cell=cells[size_t(frame%capacity)];const auto expected=frame*2+2;
        if(cell.sequence.load(std::memory_order_seq_cst)!=expected)return false;
        const auto bits=cell.stereo.load(std::memory_order_seq_cst);
        if(cell.sequence.load(std::memory_order_seq_cst)!=expected)return false;
        l=std::bit_cast<float>(uint32_t(bits));r=std::bit_cast<float>(uint32_t(bits>>32));return true;
    }
    bool copy(uint64_t end,int frames,std::array<std::vector<float>,2>& out) const {
        if(frames<=0 || frames>window || uint64_t(frames)>end)return false;
        for(auto& c:out)c.resize(size_t(frames));
        for(int i=0;i<frames;++i)if(!sample(end-frames+i,out[0][i],out[1][i]))return false;
        return true;
    }
};
}
