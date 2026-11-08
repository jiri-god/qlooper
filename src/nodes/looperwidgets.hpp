// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "nodes/looperdisplay.hpp"
namespace element {
using namespace juce;
class MusicalProgress final : public ProgressBar {
public:
    explicit MusicalProgress(double& value):ProgressBar(value),current(value){}
    double& current;
    double bars=0;int beats=4;
    void paint(Graphics& g) override {
        g.setColour(Colour(0xff10171f));g.fillRoundedRectangle(getLocalBounds().toFloat(),4);
        g.setColour(Colours::lightgreen.withAlpha(.5f));
        g.fillRoundedRectangle(0,0,float(getWidth()*std::clamp(current,0.0,1.0)),float(getHeight()),4);
        if(bars<=0 || !std::isfinite(bars))return;
        const double total=bars*beats;
        const int stride=std::max(1,int(std::ceil(total/std::max(1,getWidth()/5))));
        for(int beat=stride;beat<total;beat+=stride){
            const bool bar=beat%beats==0;const float x=float(beat/total*getWidth());
            g.setColour(Colours::white.withAlpha(bar?0.8f:0.35f));
            g.drawVerticalLine(int(x),bar?0.0f:float(getHeight())*0.55f,float(getHeight()));
        }
    }
};
class LooperStatusDisplay final : public Label {
public:
    bool large=false;
    LooperDisplayStatus status;
    void show(LooperDisplayStatus value) {
        status=std::move(value);
        setText(status.main+"\n"+status.detail+(status.flags.isEmpty()?"":"\n"+status.flags),dontSendNotification);
        repaint();
    }
    void paint(Graphics& g) override {
        g.setColour(findColour(Label::backgroundColourId));g.fillAll();
        g.setColour(findColour(Label::outlineColourId));g.drawRect(getLocalBounds());
        auto bounds=getLocalBounds().reduced(4,2);
        if(status.flags.isNotEmpty()) {
            g.setColour(Colours::yellow);g.setFont(FontOptions(large?17.0f:10.0f));
            g.drawFittedText(status.flags,bounds.removeFromTop(large?25:12),Justification::centred,1);
        }
        auto detail=bounds.removeFromBottom(large?28:17);
        g.setColour(findColour(Label::textColourId));
        g.setFont(FontOptions(large?(status.countdown?52.0f:status.main.length()>16?27.0f:36.0f):(status.countdown?32.0f:status.main.length()>16?15.0f:20.0f)).withStyle("Bold"));
        g.drawFittedText(status.main,bounds,Justification::centred,status.main.length()>16?2:1);
        g.setFont(FontOptions(large?17.0f:11.0f));g.drawFittedText(status.detail,detail,Justification::centred,1);
    }
};
} // namespace element
