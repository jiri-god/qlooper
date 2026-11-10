// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nodes/loopermanager.hpp"
#include "nodes/looperwidgets.hpp"
#include "nodes/contexthelp.hpp"
#include <element/node.h>
#include "ui/viewhelpers.hpp"

namespace element {
using namespace juce;
namespace {
using Action = LooperManagerNode::BoxAction;
String profileName(int index) { return StringArray{"Record", "Overdub", "Custom 1", "Custom 2"}[jlimit(0,3,index)]; }
void addProfiles(ComboBox& box) { for(int i=0;i<4;++i)box.addItem(profileName(i),i+1); }
void addTiming(ComboBox& box) {box.addItem("Now",1);box.addItem("Count-in",3);}
Colour stateColour(const LooperProcessor::View& v) {
    return v.state==1 || v.state==4 || v.state==5 || v.armed ? Colours::orange
        : v.state==2 ? Colours::lightgreen : Colours::lightgrey;
}
}

struct LooperManagerEditor::Impl final : Component, Timer, DragAndDropContainer {
    LooperManagerEditor& owner;
    LooperManagerNode& manager;
    ManagerEditorLook look;
    ContextHelp contextHelpPanel {*this};
    TextButton settings {"Settings"}, previous {"Prev"}, next {"Next"}, playAll {"PLAY ALL"}, stopAll {"STOP ALL"},clearMaster {"Clear"};
    Label title, controlledName,volumeLabel,panLabel;
    Slider groupVolume,groupPan;LookAndFeel_V4 mixLook;
    uint64_t seenStop=0;bool stoppedDisplay=false;
    LooperStatusDisplay display;
    GroupComponent pedalGroup {"pedal-group","Pedal / MIDI Control"};
    double masterProgress=0;
    MusicalProgress cycle {masterProgress};
    Viewport viewport;
    Component canvas;
    bool inSettings=false;
    int settingsTab=0;
    TextButton generalTab {"General"}, midiTab {"MIDI"};
    String signature;
    uint32 progressMaster=0;
    String masterLockReason() const {
        return "Stop all loopers and finish or cancel pending actions before choosing a master.";
    }
    struct DropZone final : Component, DragAndDropTarget, SettableTooltipClient {
        Impl& ui; int mode; String name; bool hovering=false;
        DropZone(Impl& u,int m,String n):ui(u),mode(m),name(std::move(n)){}
        bool canDrop(uint32 id) const {
            if(mode<0)return ui.manager.masterChangeReason(id).isEmpty();
            if(ui.manager.isLocked() || id==ui.manager.masterID())return false;
            for(const auto& t:ui.manager.members())if(t.node->nodeId==id)
                return t.looper->managerLength.load()==0 && t.looper->managerFixedLength.load()==0;
            return false;
        }
        bool isInterestedInDragSource(const SourceDetails& d) override {return d.description.toString().startsWith("jgod-looper:");}
        uint32 id(const SourceDetails& d) const {return uint32(d.description.toString().fromFirstOccurrenceOf(":",false,false).getLargeIntValue());}
        bool dragOver=false;
        void itemDragEnter(const SourceDetails& d) override {dragOver=true;hovering=canDrop(id(d));setTooltip(hovering?String("Release to assign"):(mode<0?ui.manager.masterChangeReason(id(d)):String("This looper already has a loop length, or capture is pending. Existing loop lengths are preserved.")));repaint();}
        void itemDragExit(const SourceDetails&) override {dragOver=false;hovering=false;repaint();}
        void itemDropped(const SourceDetails& d) override {
            dragOver=false;hovering=false;const auto target=id(d);const int destination=mode;
            if(canDrop(target)) {Component::SafePointer<Impl> safe(&ui);MessageManager::callAsync([safe,target,destination]{if(!safe)return;if(destination<0)safe->manager.setMaster(target);else safe->manager.setSyncMode(target,destination);safe->refresh();});}
            repaint();
        }
        void paint(Graphics& g) override {
            auto r=getLocalBounds().toFloat().reduced(1);
            g.setColour(Colour(0xff252e3b));g.fillRoundedRectangle(r,6);
            g.setColour(dragOver?(hovering?Colours::lightgreen:Colours::orange):Colour(0xff485668));g.drawRoundedRectangle(r,6,hovering?2.0f:1.0f);
            g.setColour(Colours::lightgrey);g.setFont(12.0f);g.drawText(dragOver&&!hovering?name+" — locked":name,getLocalBounds().removeFromTop(25).reduced(8,0),Justification::centredLeft);
            if(mode<0 && ui.manager.masterID()==0){g.setFont(12.0f);g.drawFittedText(ui.manager.structureLocked()?ui.masterLockReason():String("Drop looper here"),getLocalBounds().reduced(10).withTrimmedTop(26),Justification::centred,6);}
        }
    } masterZone {*this,-1,"MASTER"}, linkedZone {*this,1,"LINKED"}, independentZone {*this,0,"INDEPENDENT"};

    struct ProgressButton final : TextButton {
        ProgressButton():TextButton("Play"){setWantsKeyboardFocus(true);}
        bool momentary=false,holding=false,keyboardHolding=false;std::function<void(bool)> hold;
        void release(){if(holding){holding=false;keyboardHolding=false;if(hold)hold(false);repaint();}}
        void mouseDown(const MouseEvent& e)override{if(!momentary){TextButton::mouseDown(e);return;}if(isEnabled() && e.mods.isLeftButtonDown()){grabKeyboardFocus();holding=true;if(hold)hold(true);repaint();}}
        void mouseUp(const MouseEvent& e)override{if(momentary)release();else TextButton::mouseUp(e);}
        bool keyPressed(const KeyPress& key)override{if(!momentary)return TextButton::keyPressed(key);if(key.getKeyCode()!=KeyPress::spaceKey)return false;if(!holding&&isEnabled()){keyboardHolding=true;holding=true;if(hold)hold(true);}return true;}
        bool keyStateChanged(bool down)override{if(momentary){if(keyboardHolding && !KeyPress::isKeyCurrentlyDown(KeyPress::spaceKey))release();return true;}return TextButton::keyStateChanged(down);}
        void focusLost(FocusChangeType type)override{release();TextButton::focusLost(type);}
        void visibilityChanged()override{if(!isShowing())release();}

        float progress=0;bool running=false;Colour progressColour=Colours::lightgreen;
        void paintButton(Graphics& g,bool over,bool down) override {
            auto& lf=getLookAndFeel();lf.drawButtonBackground(g,*this,findColour(TextButton::buttonColourId),over,down);
            {Graphics::ScopedSaveState save(g);Path clip;clip.addRoundedRectangle(getLocalBounds().toFloat().reduced(1),5);g.reduceClipRegion(clip);
             g.setColour(progressColour.withAlpha(running?.30f:.08f));g.fillRect(getLocalBounds().toFloat().withWidth(getWidth()*jlimit(0.0f,1.0f,progress)));}
            lf.drawButtonText(g,*this,over,down);
        }
    };
    struct CompactModeLook final : LookAndFeel_V4 {
        void positionComboBoxText(ComboBox& box,Label& label) override {
            label.setBounds(2,1,box.getWidth()-11,box.getHeight()-2);
            label.setBorderSize(BorderSize<int>(0));
            label.setFont(FontOptions(9.0f));
            label.setMinimumHorizontalScale(1.0f);
        }
        void drawComboBox(Graphics& g,int width,int height,bool,int,int,int,int,ComboBox& box) override {
            g.setColour(Colour(0xff354354));g.fillRoundedRectangle(0,0,float(width),float(height),3);
            g.setColour(Colours::lightgrey);g.drawRoundedRectangle(.5f,.5f,width-1.f,height-1.f,3,1);
            Path arrow;arrow.startNewSubPath(width-8.f,height*.45f);arrow.lineTo(width-5.f,height*.6f);arrow.lineTo(width-2.f,height*.45f);
            g.setOpacity(box.isEnabled()?1.f:.4f);g.strokePath(arrow,PathStrokeType(1));
        }
    };
    struct Card final : Component, DragAndDropTarget {
        Impl& ui; uint32 id; int number=0; LooperProcessor* looper=nullptr;
        Label name;
        String displayName;
        Slider volume,pan;
        LookAndFeel_V4 faderLook;
        ProgressButton play;ToggleButton toEnd {"To end"};
        TextButton open {"Open"}, record {"Record"}, rewind {"Rewind"}, mute {"Mute"}, solo {"Solo"};
        CompactModeLook modeLook;
        ComboBox profile, relationship,playMode;
        LooperProcessor::View view;
        bool selected=false, dragStarted=false;
        Card(Impl& u,uint32 target):ui(u),id(target) {
            for(Component* c:std::initializer_list<Component*>{&name,&open,&play,&record,&rewind,&mute,&solo,&profile,&relationship,&playMode,&volume,&pan})addAndMakeVisible(c);
            addAndMakeVisible(toEnd);toEnd.setComponentID("manager-to-end-"+String(id));toEnd.setTooltip("Play to the next master-cycle boundary, then mute while phase keeps running. Without a master, use this loop’s end. Stays enabled for the next Play or Hold.");toEnd.onClick=[this]{ui.manager.setPlayToEnd(id,toEnd.getToggleState());};
            name.setInterceptsMouseClicks(false,false);name.setFont(FontOptions(11.0f));
            setComponentID("manager-card-"+String(id));play.setComponentID("manager-play-"+String(id));record.setComponentID("manager-record-"+String(id));
            volume.setComponentID("manager-volume-"+String(id));pan.setComponentID("manager-pan-"+String(id));
            for(auto* slider:{&volume,&pan}){slider->setLookAndFeel(&faderLook);slider->setScrollWheelEnabled(false);}
            volume.setSliderStyle(Slider::LinearVertical);volume.setRange(0,1,.001);volume.setTextBoxStyle(Slider::TextBoxAbove,false,44,20);volume.setDoubleClickReturnValue(true,1);
            volume.setTooltip("Looper playback volume. Same value as the looper editor; double-click restores 1.0.");
            volume.onValueChange=[this]{if(looper)looper->setVolume(float(volume.getValue()));};
            pan.setSliderStyle(Slider::LinearHorizontal);pan.setRange(-1,1,.01);pan.setTextBoxStyle(Slider::TextBoxRight,false,48,20);pan.setDoubleClickReturnValue(true,0);
            pan.setTooltip("Looper pan, -1 left / 0 centre / +1 right. Double-click centres.");
            pan.onValueChange=[this]{if(looper)looper->setPan(float(pan.getValue()));};
            addProfiles(profile);relationship.setLookAndFeel(&modeLook);relationship.addItem("1x",2);relationship.addItem("Nx",3);
            open.setTooltip("Open this looper's editor without changing pedal focus.");
            open.onClick=[this]{ViewHelpers::presentPluginWindow(this,ui.owner.getNode().getParentGraph().getNodeById(id));};
            playMode.setLookAndFeel(&modeLook);playMode.addItem("Toggle",1);playMode.addItem("Hold",2);
            playMode.onChange=[this]{play.release();ui.manager.setMomentary(id,playMode.getSelectedId()==2);play.momentary=ui.manager.momentary(id);playMode.setSelectedId(play.momentary?2:1,dontSendNotification);};
            playMode.setTooltip("Toggle: ordinary Play/Stop. Hold: press and hold to hear the loop immediately; release fades its sound out while its position keeps running. Entering Hold silences playback until pressed. This does not use delayed Play timing.");
            play.hold=[this](bool down){ui.manager.holdBox(id,down);};
            play.onClick=[this]{if(!play.momentary)ui.manager.requestBox(id,Action::playStop);};
            record.onClick=[this]{ui.manager.requestBox(id,Action::record);};
            rewind.onClick=[this]{ui.manager.requestBox(id,Action::rewind);};
            mute.onClick=[this]{ui.manager.requestBox(id,Action::mute);};
            solo.onClick=[this]{ui.manager.requestBox(id,Action::solo);};
            profile.onChange=[this]{ui.manager.setRecordingProfile(id,profile.getSelectedId()-1);};
            relationship.onChange=[this]{ui.manager.setSyncMode(id,relationship.getSelectedId()-1);};
            play.setTooltip("Play/Stop using this looper's timing. Click again to cancel a pending action. Does not change pedal focus.");
            record.setTooltip("Invoke the selected recording profile. The looper owns its recording settings and timing.");
            profile.setTooltip("Recording profile for this box. Its settings remain in the looper. Locked while recording or waiting.");
            mute.setTooltip("Silence loop playback without stopping its position. Turning Mute on clears this looper's Solo.");
            solo.setTooltip("Hear only Solo loopers in this Manager. Multiple Solo loopers can play together. Enabling Solo clears this looper's Mute; muting it clears Solo. Does not start playback.");
            relationship.setTooltip("Choose the linked length rule. 1x uses the master length; Nx allows whole multiples. The first recording fixes the resulting length. A master length must exist before a linked looper can record. Existing recordings are never stretched or truncated.");
            setMouseCursor(MouseCursor::PointingHandCursor);
        }
        ~Card()override{relationship.setLookAndFeel(nullptr);playMode.setLookAndFeel(nullptr);play.release();volume.setLookAndFeel(nullptr);pan.setLookAndFeel(nullptr);}
        DropZone& dropZone(){return id==ui.manager.masterID()?ui.masterZone:ui.manager.syncMode(id)>0?ui.linkedZone:ui.independentZone;}
        bool isInterestedInDragSource(const SourceDetails& d)override{return dropZone().isInterestedInDragSource(d);}
        void itemDragEnter(const SourceDetails& d)override{dropZone().itemDragEnter(d);}
        void itemDragExit(const SourceDetails& d)override{dropZone().itemDragExit(d);}
        void itemDropped(const SourceDetails& d)override{dropZone().itemDropped(d);}
        void mouseDown(const MouseEvent&) override {dragStarted=false;}
        void mouseUp(const MouseEvent&) override {if(!dragStarted)ui.manager.select(id);}
        void mouseDrag(const MouseEvent& e) override {
            if(!dragStarted && e.getDistanceFromDragStart()>6 && e.getMouseDownY()<25) {
                dragStarted=true;ui.startDragging("jgod-looper:"+String(id),this);
            }
        }
        void resized() override {
            auto r=getLocalBounds().reduced(7);auto heading=r.removeFromTop(20);open.setBounds(heading.removeFromRight(42));heading.removeFromRight(4);name.setBounds(heading);
            volume.setBounds(r.removeFromRight(46));r.removeFromRight(5);
            auto panRow=r.removeFromBottom(23);pan.setBounds(panRow);r.removeFromBottom(4);
            auto lower=r.removeFromBottom(24);
            if(relationship.isVisible()){relationship.setBounds(lower.removeFromRight(52));lower.removeFromRight(3);}
            else relationship.setBounds({});
            const int width=lower.getWidth()/3;
            rewind.setBounds(lower.removeFromLeft(width).reduced(1,0));mute.setBounds(lower.removeFromLeft(width).reduced(1,0));solo.setBounds(lower.reduced(1,0));
            r.removeFromBottom(5);auto right=r.removeFromRight(jlimit(82,104,r.getWidth()/2));r.removeFromRight(5);
            profile.setBounds(right.removeFromBottom(24));right.removeFromBottom(4);record.setBounds(right);
            playMode.setBounds(r.getRight()-42,heading.getY(),42,20);name.setBounds(heading.withRight(std::max(heading.getX(),r.getRight()-46)));play.setBounds(r);toEnd.setBounds(play.getRight()-76,play.getBottom()-23,74,22);
        }
        void update(const LooperManagerNode::Target& t,int index) {
            looper=t.looper;number=index;view=looper->view();selected=ui.manager.selectedID()==id;
            auto model=ui.owner.getNode().getParentGraph().getNodeById(id);
            displayName=String(number)+". "+(model.isValid()?model.getName():t.node->getName());
            auto status=looperDisplayStatus(view,looper->audioAvailable.load(),looper->getMuted() || view.momentaryMuted,looper->getBypassed(),looper->triggerDb.load(),{});
            String text=status.main;
            if(looper->managerSoloMuted.load())text+=" · Muted by solo";
            play.progress=view.length>0?float(view.position)/view.length:0;
            play.running=view.state==2||view.state==1||view.state==4||view.state==5;play.progressColour=stateColour(view);play.repaint();
            if(!volume.isMouseButtonDown()&&!volume.isMouseOverOrDragging(true))volume.setValue(looper->getVolume(),dontSendNotification);
            if(!pan.isMouseButtonDown()&&!pan.isMouseOverOrDragging(true))pan.setValue(looper->getPan(),dontSendNotification);
            const bool pendingTransport=view.waitingAction==int(LooperEngine::Action::play)+1 || view.waitingAction==int(LooperEngine::Action::stop)+1;
            const bool busy=looper->recordingSettingsLocked() && !pendingTransport;
            const bool othersBusy=ui.manager.isLocked() && !busy;
            play.setButtonText(ui.manager.momentary(id)?"Hold":view.waitingAction?"Cancel":(view.state==2 && !view.momentaryMuted)||busy?"Stop":"Play");
            toEnd.setToggleState(ui.manager.playToEnd(id),dontSendNotification);
            play.setEnabled(!busy && (view.length>0 || view.waitingAction));
            play.setTooltip(busy?"Finish or cancel capture with the recording button.":ui.manager.momentary(id)?"Hold to hear the loop. Release fades playback out; its position keeps running. Stop All stops it.":"Play/Stop using looper timing. Click again to cancel waiting. Does not change pedal focus.");
            record.setButtonText(!pendingTransport && (view.armed||view.soundWaiting||view.transportWaiting)?"Cancel":busy?"Finish":"Record");
            const int chosen=ui.manager.recordingProfile(id);
            const bool waitingForMaster=view.linkMode>1 && view.linkBase<=0;
            const bool available=t.node->isEnabled() && !t.node->isSuspended() && looper->pedalSectionActive.load();
            record.setEnabled(available && !othersBusy && looper->audioAvailable.load() && !looper->getBypassed() && (busy||(!waitingForMaster&&looper->recordingControl(chosen).enabled)));
            record.setTooltip(waitingForMaster?"Record the master first to establish the linked length.":!looper->recordingControl(chosen).enabled?"Enable this Custom recording profile in the looper's settings first.":"Invoke the selected recording profile. Click again to finish or cancel its pending recording.");
            play.setEnabled(available && play.isEnabled());mute.setEnabled(available);solo.setEnabled(available);
            profile.setSelectedId(chosen+1,dontSendNotification);profile.setEnabled(!ui.manager.isLocked());
            const bool linked=ui.manager.syncMode(id)>0 && id!=ui.manager.masterID();
            rewind.setEnabled(available && !linked && !busy && view.length>0);
            rewind.setTooltip(linked?"Unavailable while Linked: keep alignment with the master.":"Return to the beginning, preserving playing/stopped state.");
            mute.setToggleState(looper->getMuted(),dontSendNotification);solo.setToggleState(ui.manager.soloed(id),dontSendNotification);
            for(auto* b:{&mute,&solo})b->setColour(TextButton::buttonOnColourId,Colour(0xff8b652c));
            const bool relayout=relationship.isVisible()!=linked;relationship.setVisible(linked);if(relayout)resized();relationship.setSelectedId(ui.manager.syncMode(id)+1,dontSendNotification);
            const bool linkLocked=ui.manager.isLocked()||view.length>0||looper->managerFixedLength.load()>0;
            relationship.setEnabled(!linkLocked);
            play.momentary=ui.manager.momentary(id);playMode.setSelectedId(play.momentary?2:1,dontSendNotification);playMode.setEnabled(!ui.manager.isLocked() && !play.holding);
            if(play.holding && !Process::isForegroundProcess())play.release();
            name.setText(displayName,dontSendNotification);
            name.setTooltip(text+". Drag this title to Master, Linked or Independent. Each destination checks compatibility; existing recordings are never resized.");
            repaint();
        }
        void paint(Graphics& g) override {
            auto r=getLocalBounds().toFloat().reduced(1);auto colour=stateColour(view);
            g.setColour(colour.withAlpha(.08f));g.fillRoundedRectangle(r,6);
            g.setColour(selected?Colours::white:colour.withAlpha(.4f));g.drawRoundedRectangle(r,6,selected?2.5f:1.0f);

        }
    };
    std::vector<std::unique_ptr<Card>> cards;

    struct SettingsPanel final : Component, ListBoxModel {
        Impl& ui;
        Label managerNameLabel,managerNameCount;TextEditor managerName;
        Label orderTitle, navTitle, orderLabel, shortLabel, longLabel, profileLabel, shortBarsLabel,longBarsLabel,shortHint,longHint,help;
        ListBox order {"Looper order",this};
        std::vector<uint32> orderIds;StringArray orderNames;
        ComboBox shortTiming,longTiming,navProfile,longProfile;
        Label longProfileLabel,shortSwitch,longSwitch;
        TextButton first {"Top"},last {"Bottom"};
        int getNumRows() override{return int(orderIds.size());}
        void paintListBoxItem(int row,Graphics& g,int w,int h,bool selected) override {if(selected){g.setColour(Colour(0xff465564));g.fillAll();}g.setColour(Colours::white);g.setFont(14.0f);g.drawText(orderNames[row],4,0,w-8,h,Justification::centredLeft);}
        void moveSelected(int delta){int row=order.getSelectedRow();if(row>=0&&row<int(orderIds.size()))ui.manager.move(orderIds[size_t(row)],delta);}
        TextButton up {"Up"},down {"Down"},reset {"RESET ALL LOOPS..."};
        Slider shortBars,longBars;
        std::array<Label,5> labels;
        std::array<Slider,5> values;
        std::array<TextButton,4> assigns;
        String globalConflict;
        struct MappingRow final : Component {
            Impl& ui;uint32 id;int action;String conflict;
            Label label;Slider cc,channel;TextButton assign {"Assign"};
            MappingRow(Impl& u,uint32 target,int a):ui(u),id(target),action(a){
                for(Component* c:std::initializer_list<Component*>{&label,&cc,&channel,&assign})addAndMakeVisible(c);
                managerLabel(label,StringArray{"Play / Stop","Recording","Rewind","Mute","Solo"}[a]);
                managerNumber(cc,"CC number (-1 disables this assignment).");cc.setRange(-1,127,1);
                managerNumber(channel,"MIDI channel: 0 accepts any, 1–16 selects a channel.");channel.setRange(0,16,1);
                auto apply=[this]{conflict=ui.manager.setBoxMapping(id,action,{int(cc.getValue()),int(channel.getValue())})?String():"Conflict";assign.setTooltip(conflict.isEmpty()?"Learn this command from a CC press.":"CC/channel already used by a pedal or another Manager command. Choose a different CC/channel.");};
                const String actions[]{
                    "Control this looper's Play/Stop independently of pedal focus. Toggle uses its normal playback settings. In Hold mode the positive press opens playback immediately and release closes the audio gate while phase continues.",
                    "Invoke the recording profile selected on this looper's box. The profile owns timing and recording behavior. It must be enabled; another capture in this Manager blocks starting a new one.",
                    "Return this looper to the beginning without changing playing/stopped state. Unavailable for linked loopers, which must preserve master alignment.",
                    "Toggle this looper's playback mute without stopping its position. Turning Mute on clears its Solo. Live monitoring is separate.",
                    "Toggle Solo within this Manager. Several loopers can be Solo; others are silenced. Enabling Solo clears this looper's Mute but does not start playback."};
                label.setTooltip(actions[a]);cc.setTooltip(actions[a]+" CC -1 disables this mapping. Send 127 on press and 0 on release.");
                assign.setTooltip("Learn this looper-box action from the next CC press. The CC and channel are stored for this action only; conflicting assignments are rejected. Click again to cancel.");
                cc.onValueChange=apply;channel.onValueChange=apply;
                assign.onClick=[this]{if(ui.manager.learningTarget.load()==id && ui.manager.learningAction.load()==action)ui.manager.learningTarget=0;else {ui.manager.learn=0;ui.manager.learnBox(id,action);}};
            }
            void refresh(){auto m=ui.manager.getBoxMapping(id,action);cc.setValue(m.cc,dontSendNotification);channel.setValue(m.channel,dontSendNotification);assign.setButtonText(ui.manager.learningTarget.load()==id && ui.manager.learningAction.load()==action?"Cancel":conflict.isNotEmpty()?conflict:"Assign");}
            void resized()override{auto r=getLocalBounds();label.setBounds(r.removeFromLeft(145));assign.setBounds(r.removeFromRight(96).reduced(2));channel.setBounds(r.removeFromRight(70).reduced(2));cc.setBounds(r.removeFromRight(80).reduced(2));}
        };
        std::vector<std::unique_ptr<Label>> memberTitles;
        std::vector<std::unique_ptr<MappingRow>> mappings;
        SettingsPanel(Impl& u):ui(u){
            for(Component* c:std::initializer_list<Component*>{&managerNameLabel,&managerNameCount,&managerName,&orderTitle,&navTitle,&orderLabel,&shortLabel,&longLabel,&profileLabel,&shortBarsLabel,&longBarsLabel,&shortHint,&longHint,&help,&order,&shortTiming,&longTiming,&navProfile,&longProfile,&longProfileLabel,&shortSwitch,&longSwitch,&first,&last,&up,&down,&reset,&shortBars,&longBars})addAndMakeVisible(c);
            managerName.setTooltip("Name this Manager in the rig. Custom names are limited to 12 characters; the default QLooper Manager name is exempt. Enter or leaving the field applies the name; an empty name is ignored.");managerNameLabel.setTooltip(managerName.getTooltip());
            managerName.setComponentID("manager-name");managerLabel(managerNameLabel,"Name");
            const auto rename=[this]{auto value=managerName.getText().trim();if(value.isNotEmpty())ui.owner.getNode().setProperty(tags::name,value);};
            managerName.onTextChange=[this]{if(managerName.getText()!="QLooper Manager" && managerName.getText().length()>12)managerName.setText(managerName.getText().substring(0,12),false);managerNameCount.setText(managerName.getText()=="QLooper Manager"?String("Default"):String(managerName.getText().length())+"/12",dontSendNotification);};
            managerName.onReturnKey=rename;managerName.onFocusLost=rename;
            managerLabel(orderTitle,"LOOPER ORDER");managerLabel(navTitle,"MIDI LOOPER SWITCHING");managerLabel(orderLabel,"Looper");
            managerLabel(shortLabel,"Short press");managerLabel(longLabel,"Long press");managerLabel(profileLabel,"Recording action");managerLabel(longProfileLabel,"Recording action");
            managerLabel(shortSwitch,"Switch");managerLabel(longSwitch,"Switch");managerLabel(shortBarsLabel,"Count-in");managerLabel(longBarsLabel,"Count-in");managerLabel(shortHint,"1 = Next bar");managerLabel(longHint,"1 = Next bar");
            addTiming(shortTiming);addTiming(longTiming);
            for(auto* box:{&navProfile,&longProfile}){box->addItem("None",1);for(int i=0;i<4;++i)box->addItem(profileName(i),i+2);box->setTooltip("Invoke the target looper recording profile. None switches only.");}
            order.setTooltip("Set the order used by Previous and Next. Select a looper, then move it with Top, Up, Down or Bottom. This order does not alter cables, audio layers or master relationships. Reordering is locked while capture is pending or active.");
            orderTitle.setTooltip(order.getTooltip());
            for(auto* button:{&first,&last,&up,&down})button->setTooltip("Move the selected looper in the navigation order. Audio routing and recorded layers are unchanged.");
            const String switchHelp="Choose when the target looper becomes active. Now switches immediately. Count-in counts upcoming bar boundaries: 1 = Next bar, 2 = one complete bar later. Timed switching requires a running rig clock. This timing is separate from any recording delay in the target profile.";
            shortTiming.setTooltip(switchHelp);longTiming.setTooltip(switchHelp);shortSwitch.setTooltip(switchHelp);longSwitch.setTooltip(switchHelp);
            shortLabel.setTooltip("Settings for a short Previous/Next MIDI press, recognized on release. The default selects a neighbour and invokes its Overdub profile. Choose None to select without recording.");
            longLabel.setTooltip("Settings for a Previous/Next MIDI hold of 0.8 seconds. The default only selects a neighbour. The on-screen Prev/Next buttons always select only, using this Long press timing even if its MIDI recording action is configured.");
            const String profileHelp="Choose the target looper's recording profile, or None to switch only. Record, Overdub and Custom use that looper's own actions, count-in and limits; they are not fixed recording commands. Enable a Custom profile in the looper before using it. No separate MIDI mapping is needed for this invocation.";
            for(auto* box:{&navProfile,&longProfile})box->setTooltip(profileHelp);
            profileLabel.setTooltip(profileHelp);longProfileLabel.setTooltip(profileHelp);
            order.setRowHeight(26);
            shortTiming.onChange=[this]{ui.manager.shortTiming=shortTiming.getSelectedId()==1?0:shortBars.getValue()==1?1:2;};longTiming.onChange=[this]{ui.manager.longTiming=longTiming.getSelectedId()==1?0:longBars.getValue()==1?1:2;};
            navProfile.onChange=[this]{ui.manager.navigationProfile=navProfile.getSelectedId()-2;};
            longProfile.onChange=[this]{ui.manager.longProfile=longProfile.getSelectedId()-2;};
            for(auto* s:{&shortBars,&longBars}){managerNumber(*s,"Upcoming boundaries before switching (1 = Next bar). The target recording profile may then wait separately.");s->setRange(1,33,1);}
            shortBarsLabel.setTooltip(shortBars.getTooltip());longBarsLabel.setTooltip(longBars.getTooltip());
            shortBars.setTooltip("Upcoming bar boundaries: 1 = Next bar, 2 = one full bar later. Used only with Count-in for a short navigation press. The target recording profile may add its own preparation after switching.");
            longBars.setTooltip("Upcoming bar boundaries: 1 = Next bar, 2 = one full bar later. Used only with Count-in for a long navigation press or on-screen Prev/Next. The target recording profile may add its own preparation after switching.");
            shortBarsLabel.setTooltip(shortBars.getTooltip());longBarsLabel.setTooltip(longBars.getTooltip());
            shortBars.onValueChange=[this]{ui.manager.shortBars=std::max(1,int(shortBars.getValue())-1);if(ui.manager.shortTiming.load()!=0)ui.manager.shortTiming=shortBars.getValue()==1?1:2;};longBars.onValueChange=[this]{ui.manager.longBars=std::max(1,int(longBars.getValue())-1);if(ui.manager.longTiming.load()!=0)ui.manager.longTiming=longBars.getValue()==1?1:2;};
            up.onClick=[this]{moveSelected(-1);};down.onClick=[this]{moveSelected(1);};first.onClick=[this]{moveSelected(-int(orderIds.size()));};last.onClick=[this]{moveSelected(int(orderIds.size()));};
            reset.onClick=[this]{Component::SafePointer<SettingsPanel> safe(this);AlertWindow::showOkCancelBox(MessageBoxIconType::WarningIcon,"Reset all loopers?","Erase all layers, undo history and fixed lengths? Routing, MIDI and sync settings are kept.","Reset all","Cancel",this,ModalCallbackFunction::create([safe](int result){if(result&&safe)safe->ui.manager.resetAll();}));};
            reset.setTooltip("Erase recordings and reset loop lengths in every managed looper after confirmation. Undo history is erased too. Routing, MIDI mappings and sync assignments remain; a new first master recording can define the structure again.");
            reset.setColour(TextButton::buttonColourId,Colours::darkred);
            const String names[]{"Previous CC","Next CC","Stop All CC","Play All CC","Channel"};
            for(int i=0;i<5;++i){addAndMakeVisible(labels[i]);addAndMakeVisible(values[i]);managerLabel(labels[i],names[i]);managerNumber(values[i],i==4?"Zero accepts any MIDI channel.":"CC number; -1 disables this command.");values[i].setRange(i==4?0:-1,i==4?16:127,1);values[i].onValueChange=[this,i]{const int value=int(values[i].getValue());if(ui.manager.setGlobalMapping(i==4?1:i+1,i==4?ui.manager.previousCC.load():value,i==4?value:ui.manager.channel.load())){globalConflict.clear();}else globalConflict="Mapping conflict: choose another CC or channel.";};if(i<4){addAndMakeVisible(assigns[i]);assigns[i].setButtonText("Assign");assigns[i].onClick=[this,i]{ui.manager.learningTarget=0;ui.manager.learn=ui.manager.learn.load()==i+1?0:i+1;};}}
            const char* brief[]{"Previous looper: separate short and long press actions.","Next looper: separate short and long press actions.","Stop all on press; hold 0.8 seconds to clear all recordings.","Start all managed loops on a shared timeline.","Channel for the shared Manager controls; 0 accepts any."};
            for(int i=0;i<5;++i){values[i].getProperties().set("jgodHelpBrief",brief[i]);labels[i].getProperties().set("jgodHelpBrief",brief[i]);}
            for(auto* c:std::initializer_list<Component*>{&shortTiming,&longTiming,&shortSwitch,&longSwitch})c->getProperties().set("jgodHelpBrief","When the target looper becomes active.");
            for(auto* c:std::initializer_list<Component*>{&navProfile,&longProfile,&profileLabel,&longProfileLabel})c->getProperties().set("jgodHelpBrief","Optional target recording profile; None switches only.");
            const String globalHelp[]{
                "Previous looper MIDI CC. Short release uses Short press settings; holding 0.8 seconds uses Long press settings. Send 127 on press and 0 on release. Repeating the same pending navigation cancels it; capture blocks navigation.",
                "Next looper MIDI CC. Short release uses Short press settings; holding 0.8 seconds uses Long press settings. Send 127 on press and 0 on release. Repeating the same pending navigation cancels it; capture blocks navigation.",
                "Stop all managed loopers on press. Holding this MIDI control for 0.8 seconds also clears all managed recordings. Send 127 on press and 0 on release. The on-screen STOP ALL only stops; use Reset all for explicit clearing.",
                "Start all managed loops on press. After Stop All, restart together from the beginning; with all stopped, a running rig clock and any Sync loop, wait for the next bar. If some already play, join the shared timeline. Capture blocks Play All. Send 127 on press and 0 on release.",
                "Channel for the four shared Manager MIDI mappings. Zero accepts any channel; 1-16 selects one. Assign learns the channel too. Per-looper box mappings below have their own channels. Conflicting assignments are rejected."};
            for(int i=0;i<5;++i){values[i].setTooltip(globalHelp[i]+(i<4?String(" CC -1 disables the assignment."):String()));labels[i].setTooltip(values[i].getTooltip());if(i<4)assigns[i].setTooltip("Learn this Manager action from the next CC press. Assign saves the CC and shared channel. Click again to cancel. Use unique mappings; conflicts are rejected.");}
            managerLabel(help,"These mappings control loopers in this QLooper Manager, independently of their pedal mappings.");
        }
        std::atomic<int>& navValue(int i){return i==0?ui.manager.previousCC:i==1?ui.manager.nextCC:i==2?ui.manager.stopAllCC:i==3?ui.manager.playAllCC:ui.manager.channel;}
        void rebuild(){
            const auto row=order.getSelectedRow();const uint32 old=row>=0&&row<int(orderIds.size())?orderIds[size_t(row)]:0;orderIds.clear();orderNames.clear();memberTitles.clear();mappings.clear();int n=0;
            for(const auto& t:ui.manager.members()){
                auto name=ui.owner.getNode().getParentGraph().getNodeById(t.node->nodeId).getName();if(name.isEmpty())name=t.node->getName();
                name=String(++n)+". "+name;orderIds.push_back(t.node->nodeId);orderNames.add(name);
                auto label=std::make_unique<Label>();managerLabel(*label,name+"    —    CC / Channel");addAndMakeVisible(*label);memberTitles.push_back(std::move(label));
                for(int a=0;a<5;++a){auto row=std::make_unique<MappingRow>(ui,t.node->nodeId,a);addAndMakeVisible(*row);mappings.push_back(std::move(row));}
            }
            order.updateContent();auto found=std::find(orderIds.begin(),orderIds.end(),old);order.selectRow(found==orderIds.end()?0:int(found-orderIds.begin()));
        }
        void refresh(){
            if(!managerName.hasKeyboardFocus(true))managerName.setText(ui.owner.getNode().getName(),false);managerNameCount.setText(managerName.getText()=="QLooper Manager"?String("Default"):String(managerName.getText().length())+"/12",dontSendNotification);
            const bool locked=ui.manager.isLocked();for(Component* c:std::initializer_list<Component*>{&order,&first,&last,&up,&down,&longProfile,&shortTiming,&longTiming,&shortBars,&longBars,&navProfile})c->setEnabled(!locked);
            shortTiming.setSelectedId(ui.manager.shortTiming.load()==0?1:3,dontSendNotification);longTiming.setSelectedId(ui.manager.longTiming.load()==0?1:3,dontSendNotification);
            navProfile.setSelectedId(ui.manager.navigationProfile.load()+2,dontSendNotification);
            longProfile.setSelectedId(ui.manager.longProfile.load()+2,dontSendNotification);
            shortBars.setValue(ui.manager.shortTiming.load()==1?1:ui.manager.shortBars.load()+1,dontSendNotification);longBars.setValue(ui.manager.longTiming.load()==1?1:ui.manager.longBars.load()+1,dontSendNotification);
            shortBars.setEnabled(!locked&&shortTiming.getSelectedId()==3);longBars.setEnabled(!locked&&longTiming.getSelectedId()==3);
            for(int i=0;i<5;++i){values[i].setValue(navValue(i).load(),dontSendNotification);if(i<4)assigns[i].setButtonText(ui.manager.learn.load()==i+1?"Cancel":"Assign");}
            for(auto& row:mappings)row->refresh();
            help.setText(ui.manager.lastLearnRejected.load()?"MIDI assignment rejected: this CC/channel is already used.":globalConflict.isNotEmpty()?globalConflict:"These mappings control loopers in this QLooper Manager, independently of their pedal mappings.",dontSendNotification);
        }
        int contentHeight()const{return ui.settingsTab==0?564:280+int(memberTitles.size())*205;}
        void resized()override{
            for(auto* c:getChildren())c->setVisible(false);
            auto show=[](Component& c,Rectangle<int> r){c.setBounds(r);c.setVisible(true);};auto r=getLocalBounds().reduced(14);
            if(ui.settingsTab==0){
                auto nameRow=r.removeFromTop(34);show(managerNameLabel,nameRow.removeFromLeft(60));show(managerName,nameRow.removeFromLeft(330).reduced(2));show(managerNameCount,nameRow.removeFromLeft(55));
                show(orderTitle,r.removeFromTop(28));auto list=r.removeFromTop(134).removeFromLeft(440);auto moves=list.removeFromRight(80);show(order,list.reduced(2));
                for(auto* button:{&first,&up,&down,&last})show(*button,moves.removeFromTop(33).reduced(2));
                r.removeFromTop(12);show(navTitle,r.removeFromTop(28));
                for(int i=0;i<2;++i){
                    show(i?longLabel:shortLabel,r.removeFromTop(24));auto row=r.removeFromTop(34);show(i?longSwitch:shortSwitch,row.removeFromLeft(60));show(i?longTiming:shortTiming,row.removeFromLeft(150).reduced(2));show(i?longBarsLabel:shortBarsLabel,row.removeFromLeft(70));show(i?longBars:shortBars,row.removeFromLeft(65).reduced(2));show(i?longHint:shortHint,row);
                    row=r.removeFromTop(34);show(i?longProfileLabel:profileLabel,row.removeFromLeft(150));show(i?longProfile:navProfile,row.removeFromLeft(160).reduced(2));r.removeFromTop(8);
                }
                r.removeFromTop(12);show(reset,r.removeFromTop(34).removeFromLeft(220));
            }else{
                for(int i=0;i<5;++i){auto row=r.removeFromTop(34);show(labels[i],row.removeFromLeft(150));show(values[i],row.removeFromLeft(90).reduced(2));if(i<4)show(assigns[i],row.removeFromLeft(100).reduced(2));}
                show(help,r.removeFromTop(60));
                for(size_t i=0;i<memberTitles.size();++i){r.removeFromTop(8);show(*memberTitles[i],r.removeFromTop(27));for(int a=0;a<5;++a)show(*mappings[i*5+size_t(a)],r.removeFromTop(34));}
            }
        }
    } settingsPanel {*this};

    Impl(LooperManagerEditor& o,LooperManagerNode& m):owner(o),manager(m){
        setLookAndFeel(&look);
        for(Component* c:std::initializer_list<Component*>{&settings,&previous,&next,&playAll,&stopAll,&clearMaster,&title,&display,&controlledName,&cycle,&volumeLabel,&panLabel,&groupVolume,&groupPan,&pedalGroup,&viewport,&generalTab,&midiTab})addAndMakeVisible(c);
        managerLabel(volumeLabel,"Volume");managerLabel(panLabel,"Pan");
        for(auto* slider:{&groupVolume,&groupPan}){slider->setLookAndFeel(&mixLook);slider->setSliderStyle(Slider::LinearHorizontal);slider->setTextBoxStyle(Slider::TextBoxRight,false,56,24);slider->setScrollWheelEnabled(false);}
        groupVolume.setRange(0,1,.001);groupPan.setRange(-1,1,.01);groupVolume.setDoubleClickReturnValue(true,1);groupPan.setDoubleClickReturnValue(true,0);
        groupVolume.onValueChange=[this]{manager.groupVolume=float(groupVolume.getValue());};groupPan.onValueChange=[this]{manager.groupPan=float(groupPan.getValue());};
        groupVolume.setTooltip("Group playback volume. Live monitoring and individual looper settings are unchanged.");groupPan.setTooltip("Group playback pan. Live monitoring is unchanged.");
        managerLabel(title,"QLooper Manager");title.setFont(FontOptions(15.0f,Font::bold));display.large=true;display.setComponentID("manager-status-display");display.setColour(Label::backgroundColourId,Colour(0xff111820));display.setColour(Label::outlineColourId,Colour(0xff465564));
        cycle.setComponentID("manager-cycle");cycle.setPercentageDisplay(false);controlledName.setJustificationType(Justification::centred);controlledName.setFont(FontOptions(12));
        pedalGroup.setTextLabelPosition(Justification::centred);pedalGroup.setInterceptsMouseClicks(false,false);
        controlledName.setTooltip("Target of shared pedal/MIDI controls. Direct box MIDI assignments remain independent.");
        linkedZone.setTooltip("Loopers linked to the master length. Drag an empty looper title here, then choose 1x or Nx. Existing lengths are preserved; record a master before recording a linked looper.");
        independentZone.setTooltip("Loopers independent of the master length. Independent does not force Free mode: each looper keeps its own Free, Sync or Fixed bars setting. Moving an established loop between these groups is restricted to preserve recordings.");
        masterZone.setComponentID("manager-master-drop");addAndMakeVisible(masterZone);canvas.addAndMakeVisible(linkedZone);canvas.addAndMakeVisible(independentZone);
        viewport.setViewedComponent(&canvas,false);viewport.setScrollBarsShown(true,true);
        settings.setComponentID("manager-settings");
        settings.onClick=[this]{inSettings=!inSettings;manager.learn=0;manager.learningTarget=0;viewport.setViewedComponent(inSettings?static_cast<Component*>(&settingsPanel):&canvas,false);viewport.setViewPosition(0,0);resized();};
        generalTab.onClick=[this]{settingsTab=0;resized();};midiTab.onClick=[this]{settingsTab=1;resized();};
        previous.onClick=[this]{manager.step(-1);};next.onClick=[this]{manager.step(1);};
        previous.setTooltip("Select the previous looper without recording. Uses Long press timing from General; ignores its recording action. Capture blocks switching.");next.setTooltip("Select the next looper without recording. Uses Long press timing from General; ignores its recording action. Capture blocks switching.");
        clearMaster.onClick=[this]{manager.setMaster(0);refresh();};clearMaster.setTooltip("Remove the master when stopped, provided no recorded linked loops depend on it.");
        playAll.onClick=[this]{manager.playAll();};stopAll.onClick=[this]{manager.stopAll();};
        playAll.setTooltip("Start all managed loops together. After Stop All, restart from the beginning. When all are stopped, wait for the next bar if the rig clock runs and any loop is Sync; otherwise start immediately. If a loop already plays, others join its timeline. Recording blocks this action; manual Mute and Solo still apply.");
        stopAll.setTooltip("Stop all managed loops and cancel pending starts. Recordings are retained and the next Play All restarts from the beginning. Live input monitoring continues. Unlike a long MIDI Stop All hold, this button never clears layers.");
        volumeLabel.setTooltip(groupVolume.getTooltip());panLabel.setTooltip(groupPan.getTooltip());
        playAll.setColour(TextButton::buttonColourId,Colour(0xff275641));stopAll.setColour(TextButton::buttonColourId,Colour(0xff703b3b));
        refresh();startTimerHz(15);
    }
    ~Impl()override{groupVolume.setLookAndFeel(nullptr);groupPan.setLookAndFeel(nullptr);stopTimer();viewport.setViewedComponent(nullptr,false);setLookAndFeel(nullptr);}
    void timerCallback()override{refresh();}
    void refresh(){
        title.setText(owner.getNode().getName().isNotEmpty()?owner.getNode().getName():String("QLooper Manager"),dontSendNotification);
        String sig;for(const auto& t:manager.members())sig+=String(t.node->nodeId)+":"+owner.getNode().getParentGraph().getNodeById(t.node->nodeId).getName()+":"+String(manager.syncMode(t.node->nodeId))+";";sig+="m"+String(manager.masterID());
        if(sig!=signature){signature=sig;cards.clear();for(const auto& t:manager.members()){auto c=std::make_unique<Card>(*this,t.node->nodeId);if(t.node->nodeId==manager.masterID())addAndMakeVisible(*c);else canvas.addAndMakeVisible(*c);cards.push_back(std::move(c));}settingsPanel.rebuild();resized();}
        int n=0;Card* active=nullptr;Card* master=nullptr;
        for(const auto& t:manager.members()){auto& c=*cards[size_t(n)];++n;c.update(t,n);if(c.id==manager.selectedID())active=&c;if(c.id==manager.masterID())master=&c;}
        std::vector<ManagerDisplayMember> summary;
        summary.reserve(cards.size());
        String switchTarget;
        const auto target=manager.handoffTarget.load();
        for(const auto& c:cards) {
            const auto& v=c->view;
            ManagerDisplayMember m;
            m.name=c->displayName;m.online=c->looper->audioAvailable.load();
            m.content=v.count>0;m.recording=v.state==1 || v.state==4 || v.state==5;
            m.playing=v.state==2 || m.recording;m.waiting=v.armed && !m.recording;
            m.muted=v.momentaryMuted || c->looper->getMuted() || c->looper->managerSoloMuted.load();
            m.bypass=c->looper->getBypassed();m.silent=c->looper->getVolume()<=0;
            m.solo=manager.soloed(c->id);
            m.status=looperDisplayStatus(v,m.online,false,false,c->looper->triggerDb.load());
            summary.push_back(std::move(m));
            if(c->id==target)switchTarget=c->displayName;
        }
        const auto status=managerDisplayStatus(summary,manager.playPending.load(),switchTarget,
            manager.navigationRemaining.load(),manager.handoffNeedsClock.load(),
            manager.displayNavigationProfile.load()<0?String("Select looper"):profileName(manager.displayNavigationProfile.load()),manager.groupVolume.load());
        controlledName.setText("Active looper:\n"+(active?active->displayName:String("None")),dontSendNotification);
        display.show(status);
        bool anyPlaying=false;
        for(const auto& c:cards)anyPlaying|=c->view.state==1 || c->view.state==2 || c->view.state==4 || c->view.state==5;
        if(seenStop!=manager.stopGeneration.load()){seenStop=manager.stopGeneration.load();stoppedDisplay=true;masterProgress=0;}
        else if(anyPlaying)stoppedDisplay=false;
        const bool cycleRunning=!stoppedDisplay&&(anyPlaying||manager.transportRunning.load());
        if(!groupVolume.isMouseButtonDown())groupVolume.setValue(manager.groupVolume.load(),dontSendNotification);
        if(!groupPan.isMouseButtonDown())groupPan.setValue(manager.groupPan.load(),dontSendNotification);
        if(progressMaster!=manager.masterID()){progressMaster=manager.masterID();masterProgress=0;}
        if(!master)masterProgress=0;
        else if(cycleRunning && master->view.masterLength>0)masterProgress=double(master->view.masterPosition)/master->view.masterLength;
        cycle.bars=0;cycle.beats=master?std::max(1,master->view.numerator):4;
        if(master && master->looper->getQuantized()) {
            const auto& v=master->view;
            const double barFrames=v.barFrames>0?double(v.barFrames):
                (v.bpm>0 && v.denominator>0?v.rate*60.0/v.bpm*v.numerator*4.0/v.denominator:0);
            if(barFrames>0)cycle.bars=double(std::max(v.masterLength,v.length))/barFrames;
            if(v.length==0 && master->looper->getLoopMode()==2)cycle.bars=master->looper->getFixedBarCount();
        }
        cycle.repaint();
        previous.setEnabled(!manager.isLocked()&&!cards.empty());next.setEnabled(previous.isEnabled());playAll.setEnabled(!manager.isLocked());
        clearMaster.setEnabled(manager.masterChangeReason(0).isEmpty());masterZone.setTooltip(manager.structureLocked()?masterLockReason():"Drop a looper here by dragging its title. Clear removes the master assignment without deleting recordings.");
        settingsPanel.refresh();repaint();canvas.repaint();
    }
    void resized()override{
        auto r=getLocalBounds().reduced(12);auto top=r.removeFromTop(26);title.setBounds(top.removeFromLeft(150));top.removeFromLeft(4);settings.setBounds(top.removeFromLeft(70));settings.setButtonText(inSettings?"Back":"Settings");
        contextHelpPanel.layout(inSettings,{getWidth()-44,12,30,26},{12,getHeight()-180,getWidth()-24,168});
        generalTab.setVisible(inSettings);midiTab.setVisible(inSettings);
        for(Component* c:std::initializer_list<Component*>{&display,&controlledName,&pedalGroup,&cycle,&volumeLabel,&panLabel,&groupVolume,&groupPan,&previous,&next,&playAll,&stopAll,&masterZone})c->setVisible(!inSettings);
        r.removeFromTop(8);clearMaster.setVisible(!inSettings&&manager.masterID()!=0);for(auto& c:cards)if(c->id==manager.masterID())c->setVisible(!inSettings);
        if(inSettings){if(contextHelpPanel.open)r.removeFromBottom(180);auto tabs=r.removeFromTop(32);generalTab.setBounds(tabs.removeFromLeft(120));midiTab.setBounds(tabs.removeFromLeft(120));r.removeFromTop(8);viewport.setBounds(r);settingsPanel.setSize(std::max(520,r.getWidth()-14),settingsPanel.contentHeight());settingsPanel.resized();return;}
        const int cardHeight=getHeight()<500?144:174;
        auto header=r.removeFromTop(cardHeight+40);
        const int masterWidth=jlimit(280,320,header.getWidth()/3);
        auto masterArea=header.removeFromLeft(masterWidth);masterZone.setBounds(masterArea);
        clearMaster.setBounds(masterArea.getRight()-52,masterArea.getY()+3,46,20);clearMaster.toFront(false);header.removeFromLeft(8);
        header.removeFromRight(14);
        cycle.setBounds(header.removeFromTop(18));header.removeFromTop(5);
        auto mix=header.removeFromBottom(30);header.removeFromBottom(6);
        auto volumeArea=mix.removeFromLeft(mix.getWidth()*2/3);volumeLabel.setBounds(volumeArea.removeFromLeft(54));groupVolume.setBounds(volumeArea.reduced(3,0));panLabel.setBounds(mix.removeFromLeft(30));groupPan.setBounds(mix.reduced(3,0));
        auto controls=header.removeFromRight(190);header.removeFromRight(8);
        auto transport=controls.removeFromBottom(30);playAll.setBounds(transport.removeFromLeft(93));transport.removeFromLeft(4);stopAll.setBounds(transport);
        controls.removeFromBottom(5);pedalGroup.setBounds(controls);
        auto nav=controls.reduced(6,12);controlledName.setBounds(nav.removeFromTop(32));
        auto buttons=nav.removeFromBottom(26);const int buttonWidth=(buttons.getWidth()-6)/2;
        previous.setBounds(buttons.removeFromLeft(buttonWidth));buttons.removeFromLeft(6);next.setBounds(buttons);
        display.setBounds(header);
        for(auto& c:cards)if(c->id==manager.masterID())c->setBounds(masterArea.reduced(5).withTrimmedTop(22));
        r.removeFromTop(8);viewport.setBounds(r);
        const int width=std::max(560,r.getWidth()-14),gap=8;
        int linked=0,independent=0;for(const auto& c:cards)if(c->id!=manager.masterID()){if(manager.syncMode(c->id)>0)++linked;else ++independent;}
        // Share columns according to occupancy rather than forcing equal halves.
        const int totalColumns=std::max(2,(width+gap)/288);
        int lc=jlimit(1,totalColumns-1,int(std::round(double(totalColumns)*linked/std::max(1,linked+independent))));
        int ic=totalColumns-lc;
        int leftWidth=(width-gap)*lc/totalColumns;
        if(linked==0){leftWidth=100;lc=1;ic=std::max(1,(width-leftWidth-gap)/288);}
        if(independent==0){leftWidth=width-gap-100;ic=1;lc=std::max(1,leftWidth/288);}
        const int rightWidth=width-gap-leftWidth;
        const int rows=std::max(1,std::max((linked+lc-1)/lc,(independent+ic-1)/ic));
        const int areaHeight=26+rows*(cardHeight+gap);
        canvas.setSize(width,areaHeight);linkedZone.setBounds(0,0,leftWidth,areaHeight);independentZone.setBounds(leftWidth+gap,0,rightWidth,areaHeight);
        masterZone.name="MASTER";
        int li=0,ii=0;for(auto& c:cards)if(c->id!=manager.masterID()){
            const bool l=manager.syncMode(c->id)>0;const int index=l?li++:ii++,columns=l?lc:ic;
            const int zoneWidth=l?leftWidth:rightWidth,cardWidth=(zoneWidth-12-(columns-1)*gap)/columns;
            c->setBounds((l?0:leftWidth+gap)+(index%columns)*(cardWidth+gap)+6,26+(index/columns)*(cardHeight+gap),cardWidth,cardHeight);
        }

    }
    void paint(Graphics& g)override{paintManager(g,getLocalBounds());}

};
LooperManagerEditor::LooperManagerEditor(const Node& node):NodeEditor(node){
    if(auto* manager=dynamic_cast<LooperManagerNode*>(node.getObject())){impl=std::make_unique<Impl>(*this,*manager);addAndMakeVisible(*impl);}
    setResizable(true);setSize(1040,650);
}
LooperManagerEditor::~LooperManagerEditor()=default;
void LooperManagerEditor::resized(){if(getWidth()<900||getHeight()<430){setSize(std::max(900,getWidth()),std::max(430,getHeight()));return;}if(impl)impl->setBounds(getLocalBounds());}
} // namespace element
