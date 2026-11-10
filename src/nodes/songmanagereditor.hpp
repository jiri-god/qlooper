// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <element/ui/nodeeditor.hpp>
#include "nodes/songmanager.hpp"
#include "nodes/managereditorstyle.hpp"
namespace element {
class SongManagerEditor final : public NodeEditor, private juce::Timer, private juce::ListBoxModel {
public:
    explicit SongManagerEditor(const Node&);
    ~SongManagerEditor() override {stopTimer();list.setModel(nullptr);setLookAndFeel(nullptr);}
    void resized() override;
    void paint(juce::Graphics&) override;
private:
    void timerCallback() override;
    void mouseEnter(const juce::MouseEvent&) override;
    void updateHelpMode();
    struct HelpEntry {juce::Component* control;juce::String shortText,longText;};
    std::vector<HelpEntry> helpEntries;
    void addHelp(juce::Component&,const juce::String&,const juce::String&);
    juce::TextButton helpToggle {"?"};
    juce::TextEditor helpPanel;
    bool helpOpen=false;
    int getNumRows() override;
    void paintListBoxItem(int,juce::Graphics&,int,int,bool) override;
    void listBoxItemDoubleClicked(int row,const juce::MouseEvent&) override {if(manager)manager->jumpTo(row);}
    void changeOrder(int);
    juce::String sectionName(uint32) const;
    ManagerEditorLook look;
    int settingsTab=0;
    juce::TextButton generalTab {"General"}, midiTab {"MIDI"};
    bool showingSettings=false;
    SongManagerNode* manager;
    juce::TextButton settings {"Settings"};
    juce::Label title,status,help,switchLabel,barsLabel, countHint,recordLabel,channelLabel;
    juce::ComboBox sections,timing,recording;
    juce::Slider bars,channel;
    juce::ListBox list;
    juce::TextButton add {"Add"},remove {"-"},up {"Up"},down {"Down"};
    juce::TextButton next {"Next section"},panic {"PANIC"},restore {"Restore audio"},rewind {"Back to start"};
    std::array<juce::Label,5> midiLabels;
    std::array<juce::Slider,5> midiCC;
    std::array<juce::TextButton,5> midiAssign;
    juce::String rosterSignature;
};
}
