// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <element/juce/gui_basics.hpp>
namespace element {
// Full explanation stays on the control; the host shows its short introduction.
inline void contextHelp(juce::Component& c,const juce::String& brief,const juce::String& detail) {
    c.getProperties().set("jgodHelpBrief",brief);
    if(auto* client=dynamic_cast<juce::SettableTooltipClient*>(&c))client->setTooltip(detail);
}
class ContextHelp final : private juce::MouseListener {
    juce::Component& root;
public:
    juce::TextButton toggle {"?"};
    juce::TextEditor panel;
    bool open=false;
    explicit ContextHelp(juce::Component& parent):root(parent){
        root.getProperties().set("jgodReadableHelp",true);
        root.addAndMakeVisible(toggle);root.addAndMakeVisible(panel);root.addMouseListener(this,true);
        panel.setMultiLine(true);panel.setReadOnly(true);panel.setScrollbarsShown(true);
        panel.setFont(juce::Font(juce::FontOptions(16.0f)));
        panel.setColour(juce::TextEditor::backgroundColourId,juce::Colour(0xff111820));
        panel.setColour(juce::TextEditor::textColourId,juce::Colours::white);
        panel.setText("Point to a setting or its label to read more. The explanation stays here when you move the pointer away.");
        toggle.setTooltip("Show or hide detailed help.");
        toggle.onClick=[this]{open=!open;if(onToggle)onToggle();root.resized();};
    }
    ~ContextHelp(){root.removeMouseListener(this);root.getProperties().remove("jgodHelpOpen");}
    std::function<void()> onToggle;
    void layout(bool settings,juce::Rectangle<int> button,juce::Rectangle<int> text){
        toggle.setVisible(settings);panel.setVisible(settings&&open);toggle.setBounds(button);panel.setBounds(text);
        toggle.setToggleState(open,juce::dontSendNotification);root.getProperties().set("jgodHelpOpen",settings&&open);
    }
    void mouseEnter(const juce::MouseEvent& e) override {
        if(!panel.isVisible()||e.eventComponent==&toggle||panel.isParentOf(e.eventComponent))return;
        for(auto* c=e.eventComponent;c&&c!=&root;c=c->getParentComponent())
            if(auto* client=dynamic_cast<juce::TooltipClient*>(c)){
                auto text=client->getTooltip();if(text.isNotEmpty()){panel.setText(text);return;}
            }
    }
};
}
