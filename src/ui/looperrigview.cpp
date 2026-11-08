// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/looperrigview.hpp"
#include "nodes/looper.hpp"
#include "nodes/loopermanager.hpp"
#include "nodes/songmanagereditor.hpp"
#include <element/ui.hpp>
#include <element/ui/commands.hpp>
#include <element/context.hpp>
#include <element/session.hpp>
#include "ui/tempoandmeterbar.hpp"
#include "ui/transportbar.hpp"
#include "ui/nodeeditorfactory.hpp"

namespace element {
using namespace juce;
namespace {
/** Keeps the processor alive until its editors have been destroyed. A child may
    request more space for Settings; scroll it instead of resizing the rig. */
class EditorPane final : public Component, private ComponentListener {
public:
    EditorPane(const String& message):empty(message){viewport.setScrollBarsShown(true,true);addAndMakeVisible(viewport);}
    ~EditorPane() override {clear();}
    void clear(){if(editor)editor->removeComponentListener(this);viewport.setViewedComponent(nullptr,false);editor.reset();object=nullptr;node=Node();repaint();}
    void set(const Node& n,std::unique_ptr<Component> e,int minW,int minH,bool flexible=true){
        clear();node=n;object=n.getObject();editor=std::move(e);minimumWidth=minW;minimumHeight=minH;elastic=flexible;
        requestedWidth=minW;requestedHeight=minH;
        if(editor){if(auto* ap=dynamic_cast<AudioProcessorEditor*>(editor.get()))ap->setResizable(false,false);editor->addComponentListener(this);viewport.setViewedComponent(editor.get(),false);}
        resized();repaint();
    }
    void resized()override{
        viewport.setBounds(getLocalBounds());if(!editor||layingOut)return;
        const ScopedValueSetter<bool> guard(layingOut,true);
        const int h=elastic?std::max(minimumHeight,getHeight()):std::max(minimumHeight,requestedHeight);
        const int w=std::max(minimumWidth,elastic?getWidth():requestedWidth);
        editor->setSize(std::max(minimumWidth,w-(h>getHeight()?14:0)),h);
    }
    void paint(Graphics& g)override{g.fillAll(Colour(0xff202731));if(!editor){g.setColour(Colours::lightgrey);g.setFont(FontOptions(14));g.drawFittedText(empty,getLocalBounds().reduced(14),Justification::centred,3);}}
    Node node; ProcessorPtr object; std::unique_ptr<Component> editor;
private:
    void componentMovedOrResized(Component& c,bool,bool size)override{
        if(size&&!layingOut){requestedWidth=c.getWidth();requestedHeight=c.getHeight();resized();}
    }
    Viewport viewport;String empty;int minimumWidth=0,minimumHeight=0,requestedWidth=0,requestedHeight=0;bool elastic=true,layingOut=false;
};
Node firstOf(const Node& graph,const std::function<bool(const Node&)>& match){
    for(int i=0;i<graph.getNumNodes();++i){auto n=graph.getNode(i);if(match(n))return n;}
    return {};
}
LooperProcessor* looperOf(const Node& n){auto* p=n.getObject();return p?dynamic_cast<LooperProcessor*>(p->getAudioProcessor()):nullptr;}
bool same(const Node& a,const Node& b){return a==b && a.getObject()==b.getObject();}
}
struct LooperRigView::Impl final : Component, Timer {
    LooperRigView& owner;std::function<Node()> rootGetter;GuiService* gui;
    EditorPane song{"No Song Manager in this graph"}, manager{"No Looper Manager in this section"},small{"No loopers in this section"},layers{"No looper selected"};
    EditorPane tunerPane{"Open the tuner here after closing its other editor"};
    Label viewName,sectionName,tunerStatus;
    std::unique_ptr<TempoAndMeterBar> tempo;std::unique_ptr<TransportBar> transport;
    SessionPtr boundSession;
    TextButton previous{"<"},next{">"},tunerOpen{"Open tuner"};
    ToggleButton metronome{"Metronome"};
    std::array<TextButton,3> modes;
    Node root,section,shown,tuner,attemptedTuner; ProcessorPtr attemptedTunerObject;std::vector<Node> loopers;int mode=0;
    explicit Impl(LooperRigView& o,std::function<Node()> getter,GuiService* g):owner(o),rootGetter(std::move(getter)),gui(g){
        for(Component* c:std::initializer_list<Component*>{&tunerPane,&song,&manager,&small,&layers,&viewName,&sectionName,&tunerStatus,&previous,&next,&tunerOpen,&metronome})addAndMakeVisible(c);
        sectionName.setJustificationType(Justification::centredLeft);viewName.setJustificationType(Justification::centred);
        previous.setComponentID("rig-view-previous");next.setComponentID("rig-view-next");
        previous.setTooltip("Show the previous looper. For switching pedal control, timed transitions or recording, use the Looper Manager above right. Configure behaviour in the looper and manager settings.");next.setTooltip("Show the next looper. For switching pedal control, timed transitions or recording, use the Looper Manager above right. Configure behaviour in the looper and manager settings.");
        viewName.setTooltip("Currently displayed looper. Browsing here does not change pedal control or playback. Use the Looper Manager to control which looper is active.");
        previous.onClick=[this]{browse(-1);};next.onClick=[this]{browse(1);};
        const StringArray names{"Split","Loopers","Layers"};for(int i=0;i<3;++i){auto& b=modes[i];b.setButtonText(names[i]);b.setComponentID("rig-mode-"+String(i));addAndMakeVisible(b);b.onClick=[this,i]{setMode(i);};}
        tunerOpen.onClick=[this]{if(gui&&tuner.isValid())gui->presentPluginWindow(tuner,true);};
        metronome.setTooltip("Enable the main TICK sound while transport runs. Looper count-in is independent.");
        metronome.onClick=[this]{auto tick=mainTick();if(tick.isValid())tick.setMuted(!metronome.getToggleState());};
        if(gui){tempo=std::make_unique<TempoAndMeterBar>();transport=std::make_unique<TransportBar>();addAndMakeVisible(*tempo);addAndMakeVisible(*transport);}
        refresh();setMode(0);startTimerHz(10);
    }
    ~Impl()override{stopTimer();}
    Node mainTick() const {return firstOf(root,[](const Node& n){auto name=n.getName();return name.containsIgnoreCase("tick")&&!name.containsIgnoreCase("gate")&&!name.containsIgnoreCase("count-in");});}
    void timerCallback()override{refresh();}
    void show(const Node& n){
        shown=n;
        if(auto* lp=looperOf(n)){
            Component::SafePointer<Impl> safe(this);
            small.set(n,std::unique_ptr<Component>(lp->createCompactEditorForNode(n,[safe]{if(safe)safe->setMode(2);})),440,420,false);
            layers.set(n,std::unique_ptr<Component>(lp->createLayersEditorForNode(n)),440,240);
        }else {small.clear();layers.clear();}
        resized();
    }
    void browse(int delta){
        if(loopers.empty())return;
        int i=0;for(int j=0;j<int(loopers.size());++j)if(same(loopers[j],shown)){i=j;break;}
        show(loopers[size_t((i+delta+int(loopers.size()))%int(loopers.size()))]);updateName();
    }
    void updateName(){viewName.setText(shown.isValid()?shown.getName():"No looper",dontSendNotification);}
    void refresh(){
        if(gui&&tempo){auto current=gui->session();if(current!=boundSession){
            boundSession=current;
            if(current){tempo->getTempoValue().referTo(current->getPropertyAsValue(tags::tempo));
                tempo->getExternalSyncValue().referTo(current->getPropertyAsValue(tags::externalSync));
                tempo->getBeatsPerBarValue().referTo(current->getPropertyAsValue(tags::beatsPerBar));
                tempo->getBeatDivisorValue().referTo(current->getPropertyAsValue(tags::beatDivisor));
                tempo->stabilizeWithSession(false);}
        }}
        const Node newRoot=rootGetter();
        const Node songNode=firstOf(newRoot,[](const Node& n){return dynamic_cast<SongManagerNode*>(n.getObject())!=nullptr;});
        if(!same(song.node,songNode)||song.object.get()!=songNode.getObject()){if(songNode.isValid())song.set(songNode,std::make_unique<SongManagerEditor>(songNode),420,290,false);else song.clear();}
        Node newSection=newRoot;
        if(auto* sm=dynamic_cast<SongManagerNode*>(songNode.getObject())){
            const auto id=sm->activeSection.load();
            if(id!=0)newSection=newRoot.getNodeById(id);
            else if(!sm->sections().empty())newSection=Node();
        }
        const bool changed=!same(section,newSection)||!same(root,newRoot);
        root=newRoot;section=newSection;
        const Node managerNode=firstOf(section,[](const Node& n){return dynamic_cast<LooperManagerNode*>(n.getObject())!=nullptr;});
        if(!same(manager.node,managerNode)||manager.object.get()!=managerNode.getObject()){if(managerNode.isValid())manager.set(managerNode,std::make_unique<LooperManagerEditor>(managerNode),900,430);else manager.clear();}
        loopers.clear();
        if(auto* lm=dynamic_cast<LooperManagerNode*>(managerNode.getObject())){
            for(const auto& target:lm->members()){auto n=section.getNodeById(target.node->nodeId);if(looperOf(n))loopers.push_back(n);}
        }else for(int i=0;i<section.getNumNodes();++i){auto n=section.getNode(i);if(looperOf(n))loopers.push_back(n);}
        bool present=false;for(const auto& n:loopers)present|=same(n,shown);
        if(changed||!present||small.object.get()!=shown.getObject()){Node desired;if(!loopers.empty())desired=loopers.front();
            if(auto* lm=dynamic_cast<LooperManagerNode*>(managerNode.getObject()))for(const auto& n:loopers)if(n.getNodeId()==lm->selectedID())desired=n;
            if(!same(desired,shown)||changed||small.object.get()!=desired.getObject())show(desired);
        }
        previous.setEnabled(loopers.size()>1);next.setEnabled(loopers.size()>1);updateName();
        sectionName.setText(section.isValid()?section.getName():"No active section",dontSendNotification);
        tuner=firstOf(root,[](const Node& n){return n.getIdentifier().toString().contains("/tuna#")||n.getName().containsIgnoreCase("tuner");});
        tunerStatus.setText(tuner.isValid()?tuner.getName():"No tuner in this graph",dontSendNotification);tunerOpen.setEnabled(gui&&tuner.isValid());
        auto tick=mainTick();metronome.setEnabled(tick.isValid());metronome.setToggleState(tick.isValid()&&!tick.isMuted(),dontSendNotification);
        if(gui && (!same(attemptedTuner,tuner)||attemptedTunerObject.get()!=tuner.getObject())){
            tunerPane.clear();attemptedTuner=tuner;attemptedTunerObject=tuner.getObject();
            if(tuner.isValid()){
                gui->closePluginWindowsFor(tuner,false);
                auto* processor=tuner.getObject()?tuner.getObject()->getAudioProcessor():nullptr;
                auto editor=processor && !processor->getActiveEditor()?NodeEditorFactory::createAudioProcessorEditor(tuner):nullptr;
                if(editor && editor->getHeight()<=70)tunerPane.set(tuner,std::move(editor),420,70,false);
            }
        }
        tunerPane.setVisible(bool(tunerPane.editor));tunerOpen.setVisible(!tunerPane.editor);tunerStatus.setVisible(!tunerPane.editor);
    }
    void setMode(int value){mode=jlimit(0,2,value);for(int i=0;i<3;++i)modes[i].setToggleState(i==mode,dontSendNotification);manager.setVisible(mode!=2);layers.setVisible(mode!=1);resized();}
    void resized()override{
        auto area=getLocalBounds().reduced(8);auto top=area.removeFromTop(28);if(tempo){tempo->setBounds(top.removeFromLeft(165));top.removeFromLeft(8);}if(transport){transport->setBounds(top.removeFromLeft(235));top.removeFromLeft(8);}
        metronome.setBounds(top.removeFromLeft(95));top.removeFromLeft(12);sectionName.setBounds(top.removeFromLeft(180));
        for(int i=2;i>=0;--i){modes[i].setBounds(top.removeFromRight(80));top.removeFromRight(4);}area.removeFromTop(8);
        auto left=area.removeFromLeft(440);area.removeFromLeft(8);
        auto tunerArea=left.removeFromTop(70);tunerPane.setBounds(tunerArea);tunerArea.reduce(8,8);tunerStatus.setBounds(tunerArea.removeFromTop(24));tunerOpen.setBounds(tunerArea.removeFromLeft(130));left.removeFromTop(8);
        song.setBounds(left.removeFromTop(290));left.removeFromTop(8);
        auto selector=left.removeFromTop(28).reduced(48,0);previous.setBounds(selector.removeFromLeft(36));next.setBounds(selector.removeFromRight(36));viewName.setBounds(selector);left.removeFromTop(8);small.setBounds(left);
        if(mode==0){manager.setBounds(area.removeFromTop(430));area.removeFromTop(8);layers.setBounds(area);}
        else if(mode==1)manager.setBounds(area);else layers.setBounds(area);
    }
    void paint(Graphics& g)override{g.fillAll(Colour(0xff161c23));}
};
LooperRigView::LooperRigView(std::function<Node()> root,GuiService* gui):impl(std::make_unique<Impl>(*this,std::move(root),gui)){addAndMakeVisible(*impl);setSize(1480,900);}
LooperRigView::~LooperRigView()=default;
void LooperRigView::resized(){if(impl)impl->setBounds(getLocalBounds());}
void LooperRigView::refresh(){impl->refresh();}
void LooperRigView::browse(int delta){impl->browse(delta);}
void LooperRigView::setViewMode(int mode){impl->setMode(mode);}
Node LooperRigView::displayedLooper()const{return impl->shown;}
}
