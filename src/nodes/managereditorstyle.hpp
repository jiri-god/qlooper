// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <element/juce/gui_basics.hpp>
namespace element {
/** Shared compact settings: labels precede editable numbers, no slider track. */
struct ManagerEditorLook final : juce::LookAndFeel_V4 {
    ManagerEditorLook() {
        setColour(juce::TextButton::buttonColourId,juce::Colour(0xff344153));
        setColour(juce::ComboBox::backgroundColourId,juce::Colour(0xff283344));
        setColour(juce::Label::textColourId,juce::Colour(0xffe5ebf3));
    }
    juce::Slider::SliderLayout getSliderLayout(juce::Slider& s) override {
        juce::Slider::SliderLayout layout;layout.textBoxBounds=s.getLocalBounds();return layout;
    }
    void drawLinearSlider(juce::Graphics&,int,int,int,int,float,float,float,
                          juce::Slider::SliderStyle,juce::Slider&) override {}
};
inline void managerNumber(juce::Slider& s,const juce::String& tip) {
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::TextBoxLeft,false,70,28);
    s.setTextValueSuffix("");s.setScrollWheelEnabled(false);s.setTooltip(tip);
}
inline void managerLabel(juce::Label& label,const juce::String& name,const juce::String& tip={}) {
    label.setText(name,juce::dontSendNotification);label.setTooltip(tip);
    label.setJustificationType(juce::Justification::centredLeft);
}
inline void paintManager(juce::Graphics& g,juce::Rectangle<int> bounds,bool panic=false) {
    g.fillAll(juce::Colour(0xff202731));
    g.setColour(panic?juce::Colours::orangered:juce::Colour(0xff657180));
    g.drawRoundedRectangle(bounds.toFloat().reduced(3),10,2);
}
}
