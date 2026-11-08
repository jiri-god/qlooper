// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
// Separate executable: allocation injection must never affect JUCE or other suites.
#include <cstdlib>
#include <new>
#include <cstdio>
#include "nodes/looperengine.hpp"
static long failAfter=-1;
void* operator new(std::size_t n) {
    if(failAfter>=0 && failAfter--==0)throw std::bad_alloc();
    if(void* p=std::malloc(n?n:1))return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
int main() {
    using E=element::LooperEngine;E e;e.configureDuration(1);e.configureLimits(1,1,true);e.prepare(100);
    e.setTransitionOptions(0,0);e.apply(E::Action::record);
    for(int i=0;i<20;++i){float l=.25f,r=.5f;e.process(l,r,1);}
    e.finalizeOffline();const auto original=e.snapshot();const auto id=e.getLayer(0).id;
    int failures=0;
    for(int boundary=0;boundary<500;++boundary) {
        bool failed=false;failAfter=boundary;
        try {e.restorePrepared(original,100,1,2,2,true,2);}catch(const std::bad_alloc&){failed=true;}
        failAfter=-1;
        if(!failed){std::printf("Recovered from %d allocation failures; transaction committed.\n",failures);return failures>10?0:1;}
        ++failures;
        if(e.getLength()!=20 || e.getLayerCount()!=1 || e.getLayer(0).id!=id || e.getRecordingLimit()!=1)return 2;
        const auto retained=e.snapshot();
        if(retained.layers[0].audio!=original.layers[0].audio)return 3;
        // Old storage remains usable, not merely metadata that points to freed buffers.
        e.apply(E::Action::play);float l=0,r=0;e.process(l,r,1,false);
        if(l!=.25f || r!=.5f)return 4;
        e.apply(E::Action::stop);
    }
    return 5;
}
