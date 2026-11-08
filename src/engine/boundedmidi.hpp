// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <element/juce/audio_basics.hpp>
#include <atomic>

namespace element {
/** Reserved storage is 64 KiB at every swap partner. Limits include JUCE's
    six-byte event header. Leave separate headroom for control and release events.
    Overload drops whole events deterministically, never truncates SysEx. */
struct BoundedMidi {
    static constexpr int storage = 65536, ordinary = 32768, control = 49152, critical = 61440;
    inline static std::atomic<uint64_t> dropped {0};
    static int limit(const juce::uint8* p,int n) noexcept {
        if(n<=0)return ordinary;
        const int kind=p[0]&0xf0;
        // JGod addressed protocol: stop, stop-all, finish, held releases.
        if(n==16 && p[0]==0xf0 && p[1]==0x7d && p[2]=='J' && p[3]=='L'
            && (p[14]==2 || p[14]==7 || p[14]==24 || p[14]==41
                || (p[14]>=20 && p[14]<=33 && (p[14]&1))))return critical;
        if(p[0]==0xfc || p[0]==0xff || (n==3 && (kind==0x80 || (kind==0x90 && p[2]==0)
            || (kind==0xb0 && (p[2]<64 || p[1]>=120)))))return critical;
        // All CCs may be mapped to Panic, including a positive press. Internal
        // addressed manager messages are small and also use control headroom.
        if((n==3 && kind==0xb0) || (n>=4 && n<=32 && p[0]==0xf0 && p[1]==0x7d))return control;
        return ordinary;
    }
    static bool fits(int used,const juce::uint8* p,int n) noexcept {
        if(n<=0 || n>limit(p,n)-used-6){dropped.fetch_add(1,std::memory_order_relaxed);return false;}
        return true;
    }
    static bool add(juce::MidiBuffer& out,const void* data,int n,int at) {
        auto* p=static_cast<const juce::uint8*>(data);
        return fits(out.data.size(),p,n) && out.addEvent(p,n,at);
    }
    static bool add(juce::MidiBuffer& out,const juce::MidiMessage& m,int at) {
        return add(out,m.getRawData(),m.getRawDataSize(),at);
    }
    static void append(juce::MidiBuffer& out,const juce::MidiBuffer& in,int start,int frames,int delta) {
        if(&out==&in)return;
        for(const auto m:in)if(m.samplePosition>=start && (frames<0 || m.samplePosition-start<frames))
            add(out,m.data,m.numBytes,m.samplePosition+delta);
    }
};
}
