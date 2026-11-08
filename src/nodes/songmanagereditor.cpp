// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nodes/songmanagereditor.hpp"
#include "nodes/loopermanager.hpp"
#include "engine/graphnode.hpp"
namespace element {
using namespace juce;
SongManagerEditor::SongManagerEditor(const Node& n):NodeEditor(n),manager(dynamic_cast<SongManagerNode*>(n.getObject())) {
    setLookAndFeel(&look);getProperties().set("songReadableHelp",true);
    addMouseListener(this,true);
    addAndMakeVisible(helpToggle);addAndMakeVisible(helpPanel);
    helpPanel.setMultiLine(true);helpPanel.setReadOnly(true);helpPanel.setScrollbarsShown(true);
    helpPanel.setFont(Font(FontOptions(16.0f)));helpPanel.setColour(TextEditor::backgroundColourId,Colour(0xff111820));
    helpPanel.setText("Help\nPoint to a setting or its label to read more. The explanation stays here when you move the pointer away.");
    helpToggle.setTooltip("Show or hide detailed help.");
    helpToggle.onClick=[this]{helpOpen=!helpOpen;updateHelpMode();resized();};
    for(Component* c:std::initializer_list<Component*>{&generalTab,&midiTab,&title,&settings,&status,&help,&sections,&list,&add,&remove,&up,&down,&next,&panic,&restore,&rewind,&switchLabel,&barsLabel,&recordLabel,&channelLabel,&timing,&recording,&bars,&channel})addAndMakeVisible(c);
    title.setText("Song Manager",dontSendNotification);title.setFont(FontOptions(15.0f,Font::bold));
    status.setComponentID("song-status");status.setFont(FontOptions(16.0f,Font::bold));status.setJustificationType(Justification::centredLeft);
    status.setColour(Label::backgroundColourId,Colour(0xff111820));status.setColour(Label::outlineColourId,Colour(0xff465564));
    list.setModel(this);list.setRowHeight(28);list.setComponentID("song-order");sections.setComponentID("song-sections");
    list.setTooltip("Double-click to switch immediately without starting loops or recording. Single click only selects an occurrence for reordering or removal.");
    next.setColour(TextButton::buttonColourId,Colours::darkgreen);panic.setColour(TextButton::buttonColourId,Colours::darkred);
    next.onClick=[this]{if(manager)manager->next();};panic.onClick=[this]{if(manager)manager->panic();};
    restore.onClick=[this]{if(manager)manager->restoreAudio();};rewind.onClick=[this]{if(manager)manager->rewind();};
    next.setTooltip("Advance using shared transition timing. Press again while waiting to cancel. With stopped transport, switch immediately.");
    rewind.setTooltip("Return to the first section with live sound ready and loops stopped. Does not restore Panic.");
    panic.setTooltip("Cancel pending transitions, stop loops and mute all song output.");restore.setTooltip("Restore audio after Panic. Loops stay stopped.");
    add.onClick=[this]{if(!manager||sections.getSelectedId()<=0)return;auto ids=manager->orderIDs();ids.push_back(uint32(sections.getSelectedId()));manager->setOrder(ids);list.updateContent();list.selectRow(int(ids.size())-1);};
    remove.onClick=[this]{changeOrder(0);};up.onClick=[this]{changeOrder(-1);};down.onClick=[this]{changeOrder(1);};
    up.setButtonText(String::charToString(0x2191));down.setButtonText(String::charToString(0x2193));
    up.setTooltip("Move section up");down.setTooltip("Move section down");
    remove.setTooltip("Remove this occurrence only; keep its graph, effects and recordings.");
    settings.onClick=[this]{showingSettings=!showingSettings;if(manager)manager->learn=0;setSize(420,showingSettings?500:290);updateHelpMode();resized();};
    generalTab.onClick=[this]{settingsTab=0;if(manager)manager->learn=0;resized();};
    midiTab.onClick=[this]{settingsTab=1;resized();};
    generalTab.setTooltip("Section transition timing and recording action.");
    midiTab.setTooltip("Assign MIDI controls for this Song Manager.");
    sections.setTooltip("Choose an existing section to add to the song order.");
    add.setTooltip("Append the selected section. Add it again to repeat it later in the song.");
    managerLabel(switchLabel,"Switch");managerLabel(barsLabel,"Count-in bars");managerLabel(recordLabel,"Recording action");managerLabel(channelLabel,"MIDI channel");
    timing.addItem("Now",1);timing.addItem("Next bar",2);timing.addItem("Count-in",3);
    recording.addItem("None",1);recording.addItem("Record",2);recording.addItem("Overdub",3);recording.addItem("Custom 1",4);recording.addItem("Custom 2",5);
    recording.setTooltip("After Next, invoke this profile on the first looper in the destination's single Looper Manager, in its Looper order. The looper owns recording timing. Double-clicking a section skips this action.");
    timing.setTooltip("Now switches immediately. Next bar waits for the next rig bar. Count-in adds the selected number of complete bars. Stopped transport switches immediately.");
    managerNumber(bars,"Number of complete count-in bars after the next bar boundary.");bars.setRange(1,32,1);
    managerNumber(channel,"Shared MIDI channel for all Song Manager controls.");channel.setRange(1,16,1);
    switchLabel.setTooltip(timing.getTooltip());recordLabel.setTooltip(recording.getTooltip());
    barsLabel.setTooltip(bars.getTooltip());channelLabel.setTooltip(channel.getTooltip());
    timing.onChange=[this]{if(manager)manager->switchTiming=timing.getSelectedId()-1;};
    recording.onChange=[this]{if(manager)manager->recordingProfile=recording.getSelectedId()-2;};
    bars.onValueChange=[this]{if(manager)manager->countInBars=int(bars.getValue());};channel.onValueChange=[this]{if(manager)manager->channel=int(channel.getValue());};
    const StringArray names{"Next / Hold Panic","Next section","Back to start","Panic","Restore audio"};
    for(int i=0;i<5;++i) {
        addAndMakeVisible(midiLabels[i]);addAndMakeVisible(midiCC[i]);addAndMakeVisible(midiAssign[i]);managerLabel(midiLabels[i],names[i]);
        managerNumber(midiCC[i],i==0?"CC for short release Next and long hold Panic / Restore. -1 disables.":"CC activates on press (positive value); release resets it. -1 disables.");midiCC[i].setRange(-1,127,1);
        midiLabels[i].setTooltip(midiCC[i].getTooltip());
        midiAssign[i].setTooltip("Listen for the next MIDI CC and save its number and channel. Click again to cancel learning.");
        midiCC[i].onValueChange=[this,i]{if(!manager)return;const int value=int(midiCC[i].getValue());if(i==0)manager->pedalCC=value;else manager->actionCC[i-1]=value;};
        midiAssign[i].onClick=[this,i]{if(manager)manager->learn=manager->learn.load()==i+1?0:i+1;};
    }
    help.setText("MIDI uses the parent graph input; no MIDI cable to this box. Use distinct CC mappings. Set a CC to -1 to disable it.",dontSendNotification);help.setJustificationType(Justification::topLeft);
    auto pairHelp=[this](Component& label,Component& control,const String& brief,const String& detail){addHelp(label,brief,detail);addHelp(control,brief,detail);};
    pairHelp(switchLabel,timing,"When Next switches to the following section.","Switch\nNow switches immediately. Next bar waits for the next rig bar boundary. Count-in waits for that boundary and then the chosen number of complete bars. If the rig transport is stopped, switching is immediate. Press Next again while waiting to cancel.");
    pairHelp(barsLabel,bars,"Extra full bars to wait in Count-in mode.","Count-in bars\nApplies only when Switch is Count-in. For example, 2 waits for the next bar boundary, counts two complete bars, then switches section. This controls the section transition; a target looper may have its own additional recording count-in.");
    pairHelp(recordLabel,recording,"Optional recording profile to invoke after Next.","Recording action\nNone only changes section. Otherwise the chosen Record, Overdub, Custom 1 or Custom 2 profile is invoked on the first looper in the target section's Looper Manager order. That profile decides how and when recording starts. A missing or ambiguous manager or unavailable profile skips recording and shows a notice. Double-clicking a section never invokes this action.");
    pairHelp(channelLabel,channel,"MIDI channel used by all Song Manager mappings.","MIDI channel\nOnly CC messages on this channel control Song Manager. Assign learns both the CC number and its channel; learning a different channel therefore changes the channel for all mappings here. MIDI comes from the parent graph input, without a cable to this node.");
    const StringArray actions{
        "A short press and release requests Next. Holding for 0.8 seconds triggers Panic, or restores audio if Panic is already active. Releasing after a long hold does not request Next.",
        "Requests Next using General's transition timing and recording action. A second request while waiting cancels the transition.",
        "Returns to the first section with live audio ready and loops stopped. It does not clear Panic.",
        "Cancels pending transitions, stops loops and mutes song output. Use Restore audio to hear the rig again.",
        "Clears Panic and restores the live audio route. Stopped loops do not restart automatically."};
    for(int i=0;i<5;++i){
        const String detail=names[i]+"\n"+actions[i]+"\nSet CC to -1 to disable this mapping. Use a distinct CC for each action. Direct mappings trigger on a positive value and need a zero release before the next press.";
        pairHelp(midiLabels[i],midiCC[i],i==0?"Short press: Next. Long hold: Panic or Restore.":"MIDI CC for "+names[i]+". -1 disables it.",detail);
        addHelp(midiAssign[i],"Learn this action from the next MIDI CC.","Assign: "+names[i]+"\nClick Assign, then press the desired controller button. The next CC saves its number and the shared MIDI channel. Click Cancel to stop learning. Use separate CC numbers for different actions; this does not change the controller's own preset.");
    }
    setSize(420,290);timerCallback();startTimerHz(20);
}
void SongManagerEditor::addHelp(Component& c,const String& brief,const String& detail){
    helpEntries.push_back({&c,brief,detail});
    if(auto* client=dynamic_cast<SettableTooltipClient*>(&c))client->setTooltip(brief);
}
void SongManagerEditor::updateHelpMode(){
    const bool expanded=showingSettings&&helpOpen;
    for(const auto& entry:helpEntries)if(auto* client=dynamic_cast<SettableTooltipClient*>(entry.control))client->setTooltip(expanded?String():entry.shortText);
    generalTab.setTooltip(expanded?String():"Section transition timing and recording action.");
    midiTab.setTooltip(expanded?String():"Assign MIDI controls for this Song Manager.");
    helpToggle.setTooltip(expanded?String():"Show detailed help.");
}
void SongManagerEditor::mouseEnter(const MouseEvent& event){
    if(!showingSettings||!helpOpen)return;
    for(auto* c=event.eventComponent;c&&c!=this;c=c->getParentComponent())
        for(const auto& entry:helpEntries)if(entry.control==c){helpPanel.setText(entry.longText);return;}
}
String SongManagerEditor::sectionName(uint32 id) const {
    auto model=getNode().getParentGraph().getNodeById(id);if(model.isValid()&&model.getName().isNotEmpty())return model.getName();
    if(manager)for(const auto& s:manager->sections())if(s.node->nodeId==id)return s.node->getName();return "Missing section";
}
int SongManagerEditor::getNumRows(){return manager?int(manager->orderIDs().size()):0;}
void SongManagerEditor::paintListBoxItem(int row,Graphics& g,int width,int height,bool selected){
    if(!manager||row<0||row>=getNumRows())return;
    const bool active=row==manager->currentIndex.load() || (manager->currentIndex.load()<0 && row==0 && manager->activeSection.load()==manager->orderIDs()[size_t(row)]);
    if(active)g.fillAll(Colour(0xff356248));else if(selected)g.fillAll(Colour(0xff354354));
    if(manager->pending.load() && row==manager->currentIndex.load()+1){g.setColour(Colours::orange);g.drawRect(0,0,width,height,2);}
    if(selected){g.setColour(Colours::white.withAlpha(.6f));g.drawRect(1,1,width-2,height-2);}
    g.setColour(Colours::white);g.setFont(FontOptions(14));g.drawText(String(row+1)+". "+sectionName(manager->orderIDs()[size_t(row)]),8,0,width-16,height,Justification::centredLeft);
}
void SongManagerEditor::changeOrder(int delta){
    if(!manager)return;auto ids=manager->orderIDs();const int row=list.getSelectedRow();if(row<0||row>=int(ids.size()))return;
    if(delta==0)ids.erase(ids.begin()+row);else{const int to=row+delta;if(to<0||to>=int(ids.size()))return;std::swap(ids[size_t(row)],ids[size_t(to)]);}
    manager->setOrder(ids);list.updateContent();list.selectRow(jlimit(0,jmax(0,int(ids.size())-1),row+delta));
}
void SongManagerEditor::timerCallback(){
    if(!manager)return;const bool effective=manager->effective.load();for(auto* c:getChildren())c->setEnabled(effective);
    if(!effective){status.setEnabled(true);status.setText("Inactive: another Song Manager controls this rig",dontSendNotification);return;}
    String sig;for(const auto& s:manager->sections())sig+=String(s.node->nodeId)+sectionName(s.node->nodeId);
    if(sig!=rosterSignature){const int id=sections.getSelectedId();sections.clear(dontSendNotification);for(const auto& s:manager->sections())sections.addItem(sectionName(s.node->nodeId),int(s.node->nodeId));sections.setSelectedId(id,dontSendNotification);if(sections.getSelectedId()==0&&sections.getNumItems())sections.setSelectedItemIndex(0,dontSendNotification);rosterSignature=sig;}
    String text;
    if(manager->muted.load())text="PANIC: all output muted";
    else if(manager->pending.load()){const int to=manager->currentIndex.load()+1;text="Next: "+(manager->pendingSection.load()?sectionName(manager->pendingSection.load()):to<getNumRows()?sectionName(manager->orderIDs()[size_t(to)]):String("End"));if(manager->remainingBars.load()>0)text+=": "+String(manager->remainingBars.load())+" bars";}
    else if(manager->recordingNotice.load())text="Live: "+sectionName(manager->activeSection.load())+": Recording unavailable";
    else if(manager->ended.load())text="Song ended: Live: "+sectionName(manager->activeSection.load());
    else if(manager->orderIDs().empty())text=manager->activeSection.load()?"Live: "+sectionName(manager->activeSection.load())+": No song sequence":"No sections: Use a direct audio route";
    else text=(manager->currentIndex.load()<0?"Ready: ":"Section: ")+sectionName(manager->activeSection.load());
    status.setText(text,dontSendNotification);status.setColour(Label::textColourId,manager->muted.load()?Colours::orange:Colours::white);
    const bool editable=!manager->selectionLocked()&&!manager->pending.load();for(Component* c:std::initializer_list<Component*>{&sections,&add,&remove,&up,&down})c->setEnabled(editable);
    next.setEnabled(!manager->muted.load()&&!manager->ended.load()&&!manager->orderIDs().empty()&&!manager->selectionLocked());next.setButtonText(manager->pending.load()?"Cancel next":"Next section");
    rewind.setEnabled(!manager->selectionLocked());restore.setEnabled(manager->muted.load());
    timing.setSelectedId(manager->switchTiming.load()+1,dontSendNotification);recording.setSelectedId(manager->recordingProfile.load()+2,dontSendNotification);
    bars.setValue(manager->countInBars.load(),dontSendNotification);bars.setEnabled(manager->switchTiming.load()==2);channel.setValue(manager->channel.load(),dontSendNotification);
    for(int i=0;i<5;++i){midiCC[i].setValue(i==0?manager->pedalCC.load():manager->actionCC[i-1].load(),dontSendNotification);midiAssign[i].setButtonText(manager->learn.load()==i+1?"Cancel":"Assign");}
    list.updateContent();list.repaint();
}
void SongManagerEditor::paint(Graphics& g){paintManager(g,getLocalBounds(),manager&&manager->muted.load());}
void SongManagerEditor::resized(){
    for(auto* child:getChildren())child->setVisible(false);auto show=[](Component& c,Rectangle<int> r){c.setVisible(true);c.setBounds(r);};
    auto r=getLocalBounds().reduced(12);auto row=r.removeFromTop(26);show(title,row.removeFromLeft(130));show(settings,row.removeFromLeft(70));settings.setButtonText(showingSettings?"Back":"Settings");
    r.removeFromTop(8);if(!showingSettings){show(status,r.removeFromTop(36));r.removeFromTop(10);}
    if(!showingSettings){
        auto controls=r.removeFromRight(100);r.removeFromRight(8);
        auto footer=r.removeFromBottom(30);r.removeFromBottom(6);
        show(list,r);
        controls.setHeight(r.getHeight());
        show(next,controls.removeFromTop(32));controls.removeFromTop(6);
        show(rewind,controls.removeFromTop(32));
        show(restore,controls.removeFromBottom(32));controls.removeFromBottom(6);
        show(panic,controls.removeFromBottom(32));
        constexpr int footerButtonWidth=38;
        show(remove,footer.removeFromLeft(footerButtonWidth));footer.removeFromLeft(6);
        show(up,footer.removeFromLeft(footerButtonWidth));footer.removeFromLeft(2);
        show(down,footer.removeFromLeft(footerButtonWidth));footer.removeFromLeft(6);
        show(add,footer.removeFromLeft(footerButtonWidth));footer.removeFromLeft(4);
        show(sections,footer);return;}
    show(helpToggle,Rectangle<int>(getWidth()-44,12,32,26));helpToggle.setToggleState(helpOpen,dontSendNotification);
    if(helpOpen){show(helpPanel,r.removeFromBottom(160));r.removeFromBottom(8);}
    row=r.removeFromTop(28);show(generalTab,row.removeFromLeft(85));row.removeFromLeft(6);show(midiTab,row.removeFromLeft(70));
    generalTab.setToggleState(settingsTab==0,dontSendNotification);midiTab.setToggleState(settingsTab==1,dontSendNotification);r.removeFromTop(12);
    if(settingsTab==0){
    row=r.removeFromTop(32);show(switchLabel,row.removeFromLeft(120));show(timing,row.removeFromLeft(140).reduced(2));row=r.removeFromTop(32);show(barsLabel,row.removeFromLeft(120));show(bars,row.removeFromLeft(70).reduced(2));
    row=r.removeFromTop(32);show(recordLabel,row.removeFromLeft(120));show(recording,row.removeFromLeft(180).reduced(2));
    return;}
    row=r.removeFromTop(32);show(channelLabel,row.removeFromLeft(120));show(channel,row.removeFromLeft(70).reduced(2));r.removeFromTop(8);
    for(int i=0;i<5;++i){row=r.removeFromTop(34);show(midiLabels[i],row.removeFromLeft(175));show(midiCC[i],row.removeFromLeft(75).reduced(2));show(midiAssign[i],row.removeFromLeft(85).reduced(2));}
    r.removeFromTop(8);show(help,r);
}
}
