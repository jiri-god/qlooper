// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include <boost/test/unit_test.hpp>
#include <cstdlib>
#include <element/node.hpp>
#include <element/tags.hpp>
#include "fixture/PreparedGraph.h"
#include "fixture/TestNode.h"
#include "engine/ionode.hpp"
#include "engine/internalformat.hpp"
#include "nodes/audioprocessor.hpp"
#include "nodes/looper.hpp"
#include "nodes/contexthelp.hpp"
#include "nodes/looperkeyboard.hpp"
#include "nodes/loopermanager.hpp"
#include "nodes/songmanager.hpp"
#include "nodes/songmanagereditor.hpp"
#include "nodes/countingate.hpp"
#include "ui/nodeeditorfactory.hpp"
#include "ui/looperrigview.hpp"

using namespace element;
using namespace juce;

namespace {
class RecordingEffect final : public TestNode
{
public:
    RecordingEffect() : TestNode (2, 2, 0, 0) {}
    float multiplier = 2;
    void render (RenderContext& rc) override
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < rc.audio.getNumSamples(); ++i)
                rc.audio.setSample (c, i, rc.audio.getSample (c, i) * multiplier);
    }
};

class MeterPlayHead final : public AudioPlayHead
{
public:
    Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (120);
        info.setTimeSignature (AudioPlayHead::TimeSignature { 3, 8 });
        info.setPpqPosition (0);
        info.setIsPlaying (true);
        return info;
    }
};

bool containsButton (Component& root, const String& label)
{
    if (auto* button = dynamic_cast<Button*> (&root))
        if (button->getButtonText().containsIgnoreCase (label)) return true;
    for (auto* child : root.getChildren())
        if (containsButton (*child, label)) return true;
    return false;
}

void fillInput (AudioBuffer<float>& audio, float left, float right)
{
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        audio.setSample (0, i, left);
        audio.setSample (1, i, right);
    }
}
} // namespace

BOOST_AUTO_TEST_SUITE (LooperIntegrationTests)

BOOST_AUTO_TEST_CASE(ContextHelpKeepsDetailsAndScopesSuppression)
{
    Component root,child;Label setting;root.addAndMakeVisible(setting);setting.addAndMakeVisible(child);
    contextHelp(setting,"Short hint","Detailed explanation. Second sentence.");
    ContextHelp help(root);help.open=true;help.layout(true,{0,0,30,25},{0,30,400,160});
    BOOST_CHECK(bool(root.getProperties()["jgodHelpOpen"]));
    setting.setEnabled(false);
    const auto enter=[&](Component& c){help.mouseEnter(MouseEvent(Desktop::getInstance().getMainMouseSource(),{},ModifierKeys(),0,0,0,0,0,&c,&c,Time(),{},Time(),0,false));};
    enter(child);BOOST_CHECK_EQUAL(help.panel.getText(),"Detailed explanation. Second sentence.");
    enter(root);BOOST_CHECK_EQUAL(help.panel.getText(),"Detailed explanation. Second sentence.");
    help.layout(false,{},{});BOOST_CHECK(!bool(root.getProperties()["jgodHelpOpen"]));BOOST_CHECK(!help.panel.isVisible());
    BOOST_CHECK_EQUAL(setting.getTooltip(),"Detailed explanation. Second sentence.");
}

BOOST_AUTO_TEST_CASE (GraphRecordsEffectsWithoutReprocessingStoredLayers)
{
    PreparedGraph fixture (100, 4);
    auto& graph = fixture.graph;
    auto input = graph.addNode (new IONode (IONode::audioInputNode));
    auto* effect = new RecordingEffect();
    auto fx = graph.addNode (effect);
    auto* looper = new LooperProcessor();
    looper->setMonitor (false);
    looper->setCrossfade(0); looper->setCaptureTail(0);
    auto loop = graph.addNode (new AudioProcessorNode (looper));
    auto* playbackEffect = new RecordingEffect();
    playbackEffect->multiplier = 1;
    auto postFx = graph.addNode (playbackEffect);
    auto output = graph.addNode (new IONode (IONode::audioOutputNode));
    for (int c = 0; c < 2; ++c)
    {
        BOOST_REQUIRE (graph.connectChannels (PortType::Audio, input->nodeId, c, fx->nodeId, c));
        BOOST_REQUIRE (graph.connectChannels (PortType::Audio, fx->nodeId, c, loop->nodeId, c));
        BOOST_REQUIRE (graph.connectChannels (PortType::Audio, loop->nodeId, c, postFx->nodeId, c));
        BOOST_REQUIRE (graph.connectChannels (PortType::Audio, postFx->nodeId, c, output->nodeId, c));
    }
    graph.rebuild();
    AudioBuffer<float> audio (2, 4);
    MidiBuffer midi;
    const auto render = [&] { RenderContext rc (audio, audio, midi, 4); graph.render (rc); };
    BOOST_REQUIRE (looper->request (LooperEngine::Action::record));
    fillInput (audio, 0.125f, -0.25f);
    render();
    BOOST_REQUIRE (looper->request (LooperEngine::Action::play));
    effect->multiplier = -3;
    fillInput (audio, 0.125f, -0.25f);
    render();
    for (int i = 0; i < 4; ++i)
    {
        BOOST_CHECK_EQUAL (audio.getSample (0, i), 0.25f);
        BOOST_CHECK_EQUAL (audio.getSample (1, i), -0.5f);
    }
    BOOST_REQUIRE (looper->request (LooperEngine::Action::overdub));
    fillInput (audio, 0.125f, -0.25f);
    render();
    BOOST_REQUIRE (looper->request (LooperEngine::Action::play));
    effect->multiplier = 17;
    audio.clear();
    render();
    for (int i = 0; i < 4; ++i)
    {
        BOOST_CHECK_EQUAL (audio.getSample (0, i), -0.125f);
        BOOST_CHECK_EQUAL (audio.getSample (1, i), 0.25f);
    }
    // Downstream effects change the complete stored mix immediately. Upstream
    // changes affect neither stored layer; returning the post gain to unity
    // also demonstrates that playback processing was not baked into storage.
    effect->multiplier = -91;
    for (const float postGain : { -2.0f, 0.5f, 1.0f })
    {
        playbackEffect->multiplier = postGain;
        audio.clear();
        render();
        for (int i = 0; i < 4; ++i)
        {
            BOOST_CHECK_EQUAL (audio.getSample (0, i), -0.125f * postGain);
            BOOST_CHECK_EQUAL (audio.getSample (1, i), 0.25f * postGain);
        }
    }
}

BOOST_AUTO_TEST_CASE (HostMeterAndMidiOffsetQuantizeToThreeEighths)
{
    MeterPlayHead clock;
    PreparedGraph fixture (100, 100);
    fixture.graph.setPlayHead (&clock);
    auto* processor = new LooperProcessor();
    auto& looper = *processor;
    auto* wrapper = new AudioProcessorNode(processor);
    fixture.graph.addNode(wrapper);
    auto* manager = new LooperManagerNode();
    auto* song = new SongManagerNode();
    fixture.graph.addNode(manager); fixture.graph.addNode(song);
    for (auto* node : std::initializer_list<Processor*> {wrapper, manager, song}) {
        node->setOversamplingFactor(4);
        BOOST_CHECK_EQUAL(node->getOversamplingFactor(), 1);
        BOOST_REQUIRE(node->getPlayHead() != nullptr);
        const auto position = node->getPlayHead()->getPosition();
        BOOST_REQUIRE(position.hasValue());
        BOOST_CHECK_EQUAL(*position->getBpm(), 120);
        BOOST_CHECK_EQUAL(position->getTimeSignature()->numerator, 3);
        BOOST_CHECK_EQUAL(position->getTimeSignature()->denominator, 8);
    }
    looper.setCrossfade(0); looper.setCaptureTail(0);
    looper.view(); // Drain the initial UI snapshot before checking the rendered block.
    looper.setQuantized (true);
    looper.setGraceMs (0);
    looper.setRecordNextBar(false); // This test exercises immediate bar-start recovery.
    looper.setMonitor (false);
    // Mapping slot 1 is Record; both short releases must be honored within this block.
    BOOST_REQUIRE (looper.setMidiMapping (1, { 61, 1 }));
    AudioBuffer<float> audio (2, 100);
    fillInput (audio, 0.125f, -0.25f);
    MidiBuffer midi;
    midi.addEvent (MidiMessage::controllerEvent (1, 61, 127), 5);
    midi.addEvent (MidiMessage::controllerEvent (1, 61, 0), 6);
    midi.addEvent (MidiMessage::controllerEvent (1, 61, 127), 10);
    midi.addEvent (MidiMessage::controllerEvent (1, 61, 0), 11);
    looper.processBlock (audio, midi);
    // 120 BPM, 3/8: 75 frames/bar. Record release at 6 backfills from 0, finishes before frame 75.
    BOOST_CHECK_EQUAL (audio.getSample (0, 74), 0);
    BOOST_CHECK_EQUAL (audio.getSample (0, 75), 0.125f);
    BOOST_CHECK_EQUAL (audio.getSample (1, 75), -0.25f);
    const auto view = looper.view();
    BOOST_CHECK_EQUAL (view.length, 75);
    BOOST_CHECK_EQUAL (view.numerator, 3);
    BOOST_CHECK_EQUAL (view.denominator, 8);
    BOOST_CHECK_EQUAL (view.bpm, 120);
}

BOOST_AUTO_TEST_CASE (NativeEditorFactoryExposesTransportControls)
{
    auto* looper = new LooperProcessor();
    ProcessorPtr object = new AudioProcessorNode (looper);
    if (std::getenv ("EL_LOOPER_EDITOR_SNAPSHOT") != nullptr)
    {
        looper->prepareToPlay (100, 4);
        AudioBuffer<float> audio (2, 4);
        MidiBuffer midi;
        for (auto action : { LooperEngine::Action::record, LooperEngine::Action::play,
                             LooperEngine::Action::overdub, LooperEngine::Action::stop })
        {
            looper->view();
            BOOST_REQUIRE (looper->request (action));
            fillInput (audio, 0.125f, -0.25f);
            looper->processBlock (audio, midi);
        }
        looper->view();
    }
    ValueTree data (types::Node);
    data.setProperty (tags::object, object.get(), nullptr);
    Node node (data, false);
    auto editor = NodeEditorFactory::createAudioProcessorEditor (node);
    BOOST_REQUIRE (editor != nullptr);
    BOOST_CHECK (containsButton (*editor, "record"));
    BOOST_CHECK (containsButton (*editor, "play"));
    BOOST_CHECK (containsButton (*editor, "overdub"));
    if (std::getenv ("EL_LOOPER_EDITOR_SNAPSHOT") != nullptr)
    {
        File target ("/tmp/element-looper-v2.png");
        auto stream = target.createOutputStream();
        BOOST_REQUIRE (stream != nullptr);
        stream->setPosition (0);
        stream->truncate();
        PNGImageFormat png;
        BOOST_CHECK (png.writeImageToStream (editor->createComponentSnapshot (editor->getLocalBounds()), *stream));
        auto findButton=[](auto&& self,Component& component,const String& text)->TextButton* {
            if(auto* b=dynamic_cast<TextButton*>(&component);b && b->getButtonText()==text)return b;
            for(auto* child:component.getChildren())if(auto* result=self(self,*child,text))return result;
            return nullptr;
        };
        auto* settings=findButton(findButton,*editor,"Settings");BOOST_REQUIRE(settings);settings->onClick();
        auto* helpToggle=findButton(findButton,*editor,"?");BOOST_REQUIRE(helpToggle);const auto settingsSize=editor->getLocalBounds();helpToggle->onClick();BOOST_CHECK(editor->getLocalBounds()==settingsSize);
        for(const String page:{"General","Recording","MIDI","Fades"}){
            auto* tab=findButton(findButton,*editor,page);BOOST_REQUIRE(tab);BOOST_CHECK(tab->isVisible());tab->onClick();
            File image("/tmp/element-looper-settings-"+page+".png");auto output=image.createOutputStream();BOOST_REQUIRE(output);
            output->setPosition(0);output->truncate();BOOST_CHECK(png.writeImageToStream(editor->createComponentSnapshot(editor->getLocalBounds()),*output));
        }
        auto saveTips=[&](auto&& self,Component& c)->void {
            if(auto* label=dynamic_cast<Label*>(&c);label && (label->getText()=="One Take layers:" || label->getText()=="Play position:" || label->getText()=="Crossfade:")) {
                auto text=label->getTooltip();auto& look=LookAndFeel::getDefaultLookAndFeel();
                auto bounds=look.getTooltipBounds(text,{0,0},{0,0,1200,1200});
                Image tip(Image::ARGB,bounds.getWidth(),bounds.getHeight(),true);Graphics g(tip);
                look.drawTooltip(g,text,tip.getWidth(),tip.getHeight());
                File f("/tmp/looper-tooltip-"+label->getText().retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")+".png");
                auto out=f.createOutputStream();BOOST_REQUIRE(out);out->setPosition(0);out->truncate();BOOST_CHECK(png.writeImageToStream(tip,*out));
            }
            for(auto* child:c.getChildren())self(self,*child);
        };saveTips(saveTips,*editor);
        auto* back=findButton(findButton,*editor,"Back to looper");BOOST_REQUIRE(back);back->onClick();
        BOOST_CHECK(!findButton(findButton,*editor,"General")->isVisible());
        MeterPlayHead clock;looper->setPlayHead(&clock);looper->setLoopMode(2);looper->setFixedBarCount(4);
        AudioBuffer<float> block(2,4);block.clear();MidiBuffer midi;
        looper->request(LooperEngine::Action::clear);looper->processBlock(block,midi);
        std::unique_ptr<AudioProcessorEditor> fixedEditor(looper->createEditor());
        File fixedImage("/tmp/element-looper-fixed-bars.png");auto fixedOutput=fixedImage.createOutputStream();BOOST_REQUIRE(fixedOutput);
        fixedOutput->setPosition(0);fixedOutput->truncate();BOOST_CHECK(png.writeImageToStream(fixedEditor->createComponentSnapshot(fixedEditor->getLocalBounds()),*fixedOutput));
        looper->setPlayHead(nullptr);


    }
}

BOOST_AUTO_TEST_CASE (LooperFloatingEditorWhileNavigationEditorIsOpen)
{
    auto* looper = new LooperProcessor();
    ProcessorPtr object = new AudioProcessorNode (looper);
    ValueTree data (types::Node);
    data.setProperty (tags::object, object.get(), nullptr);
    Node node (data, false);
    std::unique_ptr<AudioProcessorEditor> navigation (looper->createEditorAndMakeActive());
    BOOST_REQUIRE (navigation != nullptr);
    auto window = NodeEditorFactory::createAudioProcessorEditor (node);
    BOOST_REQUIRE (window != nullptr);
    BOOST_CHECK (window.get() != navigation.get());
    BOOST_CHECK (containsButton (*window, "record"));
    BOOST_CHECK (containsButton (*window, "overdub"));
    window.reset();
    BOOST_CHECK (looper->getActiveEditor() == navigation.get());
    window = NodeEditorFactory::createAudioProcessorEditor (node);
    BOOST_REQUIRE (window != nullptr);
    looper->editorBeingDeleted (navigation.get());
    navigation.reset();
    BOOST_CHECK (containsButton (*window, "record"));
}

BOOST_AUTO_TEST_CASE(CountInGatePortsPreserveLegacyLooperMidiInput)
{
    AudioProcessorNode looper(new LooperProcessor());
    AudioProcessorNode gate(new CountInGate());
    looper.refreshPorts();gate.refreshPorts();
    BOOST_CHECK_EQUAL(looper.getPortForChannel(PortType::Midi,0,true),5);
    BOOST_CHECK_EQUAL(looper.getPortForChannel(PortType::Midi,0,false),6);
    BOOST_CHECK_EQUAL(gate.getPortForChannel(PortType::Midi,0,true),4);
    BOOST_CHECK_EQUAL(gate.getPortForChannel(PortType::Audio,0,false),2);
    OwnedArray<PluginDescription> desc;
    ElementAudioPluginFormat format(*element::test::context());
    format.findAllTypesForFile(desc,"element.countInGate");
    BOOST_REQUIRE_EQUAL(desc.size(),1);BOOST_CHECK_EQUAL(desc[0]->name,"Count-in Gate");
}
BOOST_AUTO_TEST_CASE(DetachedLayersWindowResizesReopensAndClosesWithEditor)
{
    LooperProcessor p;p.prepareToPlay(100,8);p.setCrossfade(0);p.setCaptureTail(0);p.setCapacityLimits(20,4,true);
    AudioBuffer<float> audio(2,8);audio.clear();MidiBuffer midi;
    for(int i=0;i<18;++i){
        for(int c=0;c<2;++c)for(int f=0;f<8;++f)audio.setSample(c,f,float(std::sin(f*.8+i*.2))*.4f);
        p.request(LooperEngine::Action::overdub);p.processBlock(audio,midi);p.view();
        if(i==1)for(int pass=0;pass<2;++pass){p.processBlock(audio,midi);p.view();}
        p.request(LooperEngine::Action::stop);p.processBlock(audio,midi);p.view();
    }
    std::unique_ptr<AudioProcessorEditor> editor(p.createEditor());
    auto find=[](auto&& self,Component& c,const String& name)->TextButton* {
        if(auto* b=dynamic_cast<TextButton*>(&c);b && b->getButtonText()==name)return b;
        for(auto* child:c.getChildren())if(auto* b=self(self,*child,name))return b;
        return nullptr;
    };
    auto* open=find(find,*editor,"Layers...");BOOST_REQUIRE(open);open->onClick();
    DocumentWindow* window=nullptr;
    for(int i=0;i<TopLevelWindow::getNumTopLevelWindows();++i)
        if(auto* w=dynamic_cast<DocumentWindow*>(TopLevelWindow::getTopLevelWindow(i));w && w->getName().endsWith(" - Layers"))window=w;
    BOOST_REQUIRE(window);Component::SafePointer<Component> safe(window);
    BOOST_CHECK(window->isVisible());window->setSize(1400,700);
    auto* content=window->getContentComponent();BOOST_REQUIRE(content);
    BOOST_CHECK(content->getWidth()>1000);
    for(const String name:{"Capture [H]","Live waveform","Select from history"}) {
        auto* b=find(find,*content,name);BOOST_REQUIRE(b);BOOST_CHECK(b->isVisible());BOOST_CHECK(!b->getBounds().isEmpty());
    }
    const auto beforeLive=p.view();find(find,*content,"Live waveform")->onClick();
    BOOST_CHECK_EQUAL(p.view().state,beforeLive.state);

    auto findID=[](auto&& self,Component& c,const String& id)->Component* {
        if(c.getComponentID()==id)return &c;
        for(auto* child:c.getChildren())if(auto* found=self(self,*child,id))return found;
        return nullptr;
    };
    MessageManager::getInstance()->runDispatchLoopUntil(150);
    auto* wave=findID(findID,*content,"layer-wave-0");auto* preview=findID(findID,*content,"selected-layer-wave");
    BOOST_REQUIRE(wave);BOOST_REQUIRE(preview);
    const auto before=p.view();
    wave->mouseDown(MouseEvent(Desktop::getInstance().getMainMouseSource(),{},ModifierKeys(),1,0,0,0,0,wave,wave,Time::getCurrentTime(),{},Time::getCurrentTime(),1,false));
    BOOST_CHECK_EQUAL(preview->getName(),"Selected layer "+String(before.layers[0].id));
    BOOST_CHECK_EQUAL(p.view().state,before.state);BOOST_CHECK_EQUAL(p.view().count,before.count);
    auto* hidePreview=find(find,*content,"Hide preview");BOOST_REQUIRE(hidePreview);hidePreview->onClick();BOOST_CHECK(!preview->isVisible());
    auto* showPreview=find(find,*content,"Show preview");BOOST_REQUIRE(showPreview);showPreview->onClick();BOOST_CHECK(preview->isVisible());
    if(std::getenv("EL_LOOPER_EDITOR_SNAPSHOT")){
        File f("/tmp/element-looper-detached-layers.png");auto stream=f.createOutputStream();BOOST_REQUIRE(stream);
        stream->setPosition(0);stream->truncate();PNGImageFormat png;
        BOOST_CHECK(png.writeImageToStream(content->createComponentSnapshot(content->getLocalBounds()),*stream));
    }
    // History selection shares the large preview, but never changes playback.
    find(find,*content,"Select from history")->onClick();audio.clear();p.processBlock(audio,midi);p.view();
    for(int i=0;i<60 && !p.historySelection;++i)MessageManager::getInstance()->runDispatchLoopUntil(10);
    BOOST_REQUIRE(p.historySelection);MessageManager::getInstance()->runDispatchLoopUntil(80);
    auto* captureSelection=find(find,*content,"Capture selection");BOOST_REQUIRE(captureSelection);BOOST_CHECK(captureSelection->isVisible());
    if(std::getenv("EL_LOOPER_EDITOR_SNAPSHOT")){
        File f("/tmp/element-looper-history-selection.png");auto stream=f.createOutputStream();BOOST_REQUIRE(stream);
        stream->setPosition(0);stream->truncate();PNGImageFormat png;
        BOOST_CHECK(png.writeImageToStream(content->createComponentSnapshot(content->getLocalBounds()),*stream));
    }
    // Return to the layer preview for the existing edit/delete lifecycle checks.
    wave->mouseDown(MouseEvent(Desktop::getInstance().getMainMouseSource(),{},ModifierKeys(),1,0,0,0,0,wave,wave,Time::getCurrentTime(),{},Time::getCurrentTime(),1,false));
    auto gain=[](auto&& self,Component& c)->Slider* {
        if(auto* slider=dynamic_cast<Slider*>(&c);slider && slider->getTooltip().startsWith("Layer volume"))return slider;
        for(auto* child:c.getChildren())if(auto* slider=self(self,*child))return slider;
        return nullptr;
    };
    auto* detachedGain=gain(gain,*content);auto* mainGain=gain(gain,*editor);
    BOOST_REQUIRE(detachedGain);BOOST_REQUIRE(mainGain);
    const auto bounds=detachedGain->getBounds();
    const auto checkColumns=[&] {
        auto* first=findID(findID,*content,"layer-wave-0");BOOST_REQUIRE(first);
        auto* body=first->getParentComponent();auto* viewport=dynamic_cast<Viewport*>(body->getParentComponent()->getParentComponent());
        BOOST_REQUIRE(viewport);
        BOOST_CHECK_LE(body->getHeight(),viewport->getHeight());
        for(int i=0;i<p.view().count;++i) {
            auto* row=findID(findID,*content,"layer-wave-"+String(i));BOOST_REQUIRE(row);
            BOOST_CHECK_LT(row->getBottom(),viewport->getHeight());
            BOOST_CHECK_EQUAL(row->getWidth(),first->getWidth());
        }
    };
    checkColumns();
    window->setSize(700,600);checkColumns();
    window->setSize(700,400);BOOST_CHECK_EQUAL(detachedGain->getWidth(),bounds.getWidth());BOOST_CHECK_EQUAL(detachedGain->getHeight(),bounds.getHeight());
    window->setSize(1700,1000);BOOST_CHECK_EQUAL(detachedGain->getWidth(),bounds.getWidth());BOOST_CHECK_EQUAL(detachedGain->getHeight(),bounds.getHeight());

    detachedGain->setValue(.42,sendNotificationSync);p.serviceOfflineControls();
    for(int attempt=0;attempt<20 && std::abs(mainGain->getValue()-.42)>.001;++attempt)
        MessageManager::getInstance()->runDispatchLoopUntil(50);
    BOOST_CHECK_CLOSE(mainGain->getValue(),.42,.01);
    BOOST_CHECK_CLOSE(detachedGain->getValue(),.42,.01);
    p.request(LooperEngine::Action::overdub);p.processBlock(audio,midi);
    p.request(LooperEngine::Action::stop);p.processBlock(audio,midi);
    MessageManager::getInstance()->runDispatchLoopUntil(100);
    BOOST_CHECK_EQUAL(preview->getName(),"Selected layer "+String(before.layers[0].id));
    BOOST_REQUIRE(p.editLayer(before.layers[0].id,3,0));p.serviceOfflineControls();
    MessageManager::getInstance()->runDispatchLoopUntil(100);
    BOOST_CHECK_EQUAL(preview->getName(),"No selected layer");
    window->closeButtonPressed();BOOST_CHECK(!window->isVisible());open->onClick();BOOST_CHECK(window->isVisible());
    auto* fold=find(find,*editor,"Hide");BOOST_REQUIRE(fold);fold->onClick();BOOST_CHECK_EQUAL(editor->getHeight(),420);
    auto* settings=find(find,*editor,"Settings");BOOST_REQUIRE(settings);settings->onClick();BOOST_CHECK_EQUAL(editor->getHeight(),420);
    editor.reset();BOOST_CHECK(safe==nullptr);
}
BOOST_AUTO_TEST_CASE(OpeningEditorWithoutAudioDoesNotFillCommandQueue)
{
    LooperProcessor p;p.prepareToPlay(100,4);
    AudioBuffer<float> audio(2,4);audio.clear();MidiBuffer midi;
    p.request(LooperEngine::Action::record);p.processBlock(audio,midi);p.view();
    p.request(LooperEngine::Action::stop);p.processBlock(audio,midi);p.view();
    std::unique_ptr<AudioProcessorEditor> editor(p.createEditor());
    MessageManager::getInstance()->runDispatchLoopUntil(200);
    int accepted=0;while(accepted<64 && p.request(LooperEngine::Action::stop))++accepted;
    BOOST_CHECK_EQUAL(accepted,63);
}

BOOST_AUTO_TEST_CASE(UnavailableRedoKeyboardAndHeldMidiPreserveSerializableRecordLayer)
{
    for(bool midiHold:{false,true}) {
        LooperProcessor p;p.setCapacityLimits(3,4,true);p.prepareToPlay(1000,100);
        p.setQuantized(false);p.setCrossfade(0);p.setCaptureTail(0);p.setRecordNextBar(false);
        AudioBuffer<float> audio(2,100);MidiBuffer midi;
        const auto run=[&]{fillInput(audio,.25f,.5f);p.processBlock(audio,midi);midi.clear();return p.view();};
        p.request(LooperEngine::Action::record);run();
        p.request(LooperEngine::Action::play);run();
        auto control=p.recordingControl(0);control.overwrite=true;BOOST_REQUIRE(p.setRecordingControl(0,control));
        p.request(LooperEngine::Action::record);run();
        BOOST_REQUIRE(!p.view().redo);
        if(midiHold) {
            const auto mapping=p.getMidiMapping(5);
            BOOST_REQUIRE(mapping.cc>=0);
            midi.addEvent(MidiMessage::controllerEvent(mapping.channel?mapping.channel:1,mapping.cc,127),0);
            for(int i=0;i<9;++i)run();
            midi.addEvent(MidiMessage::controllerEvent(mapping.channel?mapping.channel:1,mapping.cc,0),0);run();
        } else {
            LooperKeyboard keyboard;const auto key=keyboard.press('X',true);
            BOOST_REQUIRE(key.action.has_value());p.request(*key.action);
            for(int i=0;i<10;++i)run();
        }
        p.request(LooperEngine::Action::play);const auto before=run();
        BOOST_REQUIRE_EQUAL(before.count,2);BOOST_REQUIRE_EQUAL(before.length,100);
        for(int i=0;i<before.count;++i) {
            if(i==0)BOOST_CHECK_EQUAL(before.layers[i].take,0);else BOOST_CHECK_NE(before.layers[i].take,0);BOOST_CHECK_EQUAL(before.layers[i].part,0);
            BOOST_CHECK(!before.layers[i].continued);
        }
        MemoryBlock saved;p.getStateInformation(saved);
        LooperProcessor restored;restored.setStateInformation(saved.getData(),int(saved.getSize()));
        const auto after=restored.view();
        BOOST_CHECK_EQUAL(after.count,before.count);BOOST_CHECK_EQUAL(after.length,before.length);
        MemoryBlock roundtrip;restored.getStateInformation(roundtrip);
        BOOST_CHECK(saved==roundtrip); // Includes stereo samples, coverage and take metadata.
        // A rejected loader leaves an empty looper; a successful roundtrip must
        // retain audio and remain usable for a subsequent overdub.
        restored.prepareToPlay(1000,100);restored.request(LooperEngine::Action::overdub);
        fillInput(audio,.5f,.75f);restored.processBlock(audio,midi);
        BOOST_CHECK_EQUAL(restored.view().length,100);
        BOOST_CHECK_EQUAL(restored.view().count,3);
    }
}


BOOST_AUTO_TEST_CASE(StatusDisplayShowsLargeCountThenPersistentRecording)
{
    struct DisplayClock:AudioPlayHead {double ppq=0;Optional<PositionInfo> getPosition()const override{PositionInfo p;p.setBpm(60);p.setTimeSignature(TimeSignature{1,4});p.setPpqPosition(ppq);p.setIsPlaying(true);return p;}} clock;
    LooperProcessor p;p.prepareToPlay(100,10);p.setPlayHead(&clock);p.setLoopMode(1);
    auto c=p.recordingControl(1);c.start=2;c.bars=2;c.metronome=false;p.setRecordingControl(1,c);
    std::unique_ptr<AudioProcessorEditor> editor(p.createEditor());
    auto find=[](auto&& self,Component& c)->Label*{if(c.getComponentID()=="looper-status-display")return dynamic_cast<Label*>(&c);for(auto* child:c.getChildren())if(auto* r=self(self,*child))return r;return nullptr;};
    auto* display=find(find,*editor);BOOST_REQUIRE(display);
    AudioBuffer<float> audio(2,10);MidiBuffer midi;
    auto run=[&]{audio.clear();p.processBlock(audio,midi);p.view();midi.clear();clock.ppq+=.1;};
    p.request(LooperEngine::Action::overdub);run();MessageManager::getInstance()->runDispatchLoopUntil(100);
    BOOST_CHECK(display->getText().startsWith("2\n"));
    auto snapshot=[&](const char* name){if(!std::getenv("EL_LOOPER_EDITOR_SNAPSHOT"))return;File file(name);auto out=file.createOutputStream();BOOST_REQUIRE(out);out->setPosition(0);out->truncate();PNGImageFormat format;BOOST_CHECK(format.writeImageToStream(editor->createComponentSnapshot(editor->getLocalBounds()),*out));};
    snapshot("/tmp/element-looper-display-countdown.png");
    for(int i=0;i<32;++i)run();p.setMuted(true);MessageManager::getInstance()->runDispatchLoopUntil(100);
    BOOST_CHECK(display->getText().startsWith("Recording\n"));BOOST_CHECK(display->getText().contains("MUTE"));
    snapshot("/tmp/element-looper-display-recording.png");
}

BOOST_AUTO_TEST_CASE (NamesUseNodePropertiesAndMomentarySpaceReleases)
{
    PreparedGraph fixture(100,8);auto& graph=fixture.graph;
    auto* manager=new LooperManagerNode();auto* mn=graph.addNode(manager);auto* p=new LooperProcessor();auto* pn=graph.addNode(new AudioProcessorNode(p));
    BOOST_REQUIRE(graph.connectChannels(PortType::Midi,mn->nodeId,0,pn->nodeId,0));graph.rebuild();
    ValueTree md(types::Node);md.setProperty(tags::object,mn,nullptr);Node managerModel(md,false);
    ValueTree ld(types::Node);ld.setProperty(tags::object,pn,nullptr);Node looperModel(ld,false);
    LooperManagerEditor editor(managerModel);std::unique_ptr<AudioProcessorEditor> le(p->createEditorForNode(looperModel));
    std::function<Component*(Component&,const String&)> find=[&](Component& c,const String& id)->Component*{if(c.getComponentID()==id)return &c;for(auto* child:c.getChildren())if(auto* result=find(*child,id))return result;return nullptr;};
    auto* settings=dynamic_cast<Button*>(find(editor,"manager-settings"));BOOST_REQUIRE(settings);settings->onClick();
    auto* name=dynamic_cast<TextEditor*>(find(editor,"manager-name"));BOOST_REQUIRE(name);name->setText("LM - Verse");name->onReturnKey();BOOST_CHECK_EQUAL(managerModel.getName().toStdString(),"LM - Verse");
    name=dynamic_cast<TextEditor*>(find(*le,"looper-name"));BOOST_REQUIRE(name);name->setText("Rhythm");name->onReturnKey();BOOST_CHECK_EQUAL(looperModel.getName().toStdString(),"Rhythm");
    settings->onClick();
    AudioBuffer<float> audio(2,8);MidiBuffer midi;auto render=[&]{audio.clear();RenderContext rc(audio,audio,midi,8);graph.render(rc);p->view();};
    p->request(LooperEngine::Action::record);render();p->request(LooperEngine::Action::record);render();manager->stopAll();render();
    manager->setMomentary(pn->nodeId,true);MessageManager::getInstance()->runDispatchLoopUntil(100);
    auto* button=find(editor,"manager-play-"+String(pn->nodeId));BOOST_REQUIRE(button);
    BOOST_CHECK(button->keyPressed(KeyPress(KeyPress::spaceKey)));render();BOOST_CHECK_EQUAL(p->view().state,int(LooperEngine::State::playing));
    button->keyPressed(KeyPress(KeyPress::spaceKey));button->keyStateChanged(false);render();BOOST_CHECK_EQUAL(p->view().state,int(LooperEngine::State::playing));
    button->keyPressed(KeyPress(KeyPress::spaceKey));render();button->focusLost(Component::focusChangedDirectly);render();BOOST_CHECK_EQUAL(p->view().state,int(LooperEngine::State::playing));
    const auto now=Time::getCurrentTime();
    MouseEvent down(Desktop::getInstance().getMainMouseSource(),{10,10},ModifierKeys(ModifierKeys::leftButtonModifier),1,0,0,0,0,button,button,now,{10,10},now,1,false);
    button->mouseDown(down);render();
    BOOST_CHECK_EQUAL(p->view().state,int(LooperEngine::State::playing));
    BOOST_CHECK(!p->view().momentaryMuted);
    button->keyStateChanged(false);render(); // unrelated keyboard event must not release mouse hold
    BOOST_CHECK(!p->view().momentaryMuted);

    render();BOOST_CHECK_EQUAL(p->view().state,int(LooperEngine::State::playing));
    MouseEvent up(Desktop::getInstance().getMainMouseSource(),{10,10},ModifierKeys(),1,0,0,0,0,button,button,now,{10,10},now,1,false);
    button->mouseUp(up);render();BOOST_CHECK(p->view().momentaryMuted);
    BOOST_CHECK_EQUAL(p->view().state,int(LooperEngine::State::playing));

}

BOOST_AUTO_TEST_CASE (ManagerGridKeepsFocusAndShowsLinkedLocks)
{
    PreparedGraph fixture(100,8);auto& graph=fixture.graph;
    auto* manager=new LooperManagerNode();auto* managerNode=graph.addNode(manager);
    std::vector<LooperProcessor*> loopers;std::vector<Processor*> nodes;
    for(int i=0;i<5;++i){auto* p=new LooperProcessor();loopers.push_back(p);auto* n=graph.addNode(new AudioProcessorNode(p));nodes.push_back(n);BOOST_REQUIRE(graph.connectChannels(PortType::Midi,managerNode->nodeId,0,n->nodeId,0));}
    graph.rebuild();BOOST_REQUIRE(manager->setMaster(nodes[0]->nodeId));BOOST_REQUIRE(manager->setSyncMode(nodes[1]->nodeId,1));
    ValueTree data(types::Node);data.setProperty(tags::object,managerNode,nullptr);Node node(data,false);
    LooperManagerEditor editor(node);
    std::vector<Button*> buttons;
    std::function<void(Component&)> collect=[&](Component& c){if(auto* b=dynamic_cast<Button*>(&c))buttons.push_back(b);for(auto* child:c.getChildren())collect(*child);};collect(editor);
    int solo=0,rewind=0,disabledRewind=0;
    for(auto* b:buttons){if(b->getButtonText()=="Solo")++solo;if(b->getButtonText()=="Rewind"){++rewind;if(!b->isEnabled())++disabledRewind;}}
    BOOST_CHECK_EQUAL(solo,5);BOOST_CHECK_EQUAL(rewind,5);BOOST_CHECK(disabledRewind>=1);
    const auto selected=manager->selectedID();
    Button* secondSolo=nullptr;for(auto* b:buttons)if(b->getButtonText()=="Solo")for(auto* sibling:b->getParentComponent()->getChildren())if(auto* label=dynamic_cast<Label*>(sibling);label && label->getText().startsWith("2. "))secondSolo=b;
    BOOST_REQUIRE(secondSolo);secondSolo->triggerClick();MessageManager::getInstance()->runDispatchLoopUntil(50);
    AudioBuffer<float> audio(2,8);audio.clear();MidiBuffer midi;RenderContext rc(audio,audio,midi,8);graph.render(rc);
    BOOST_CHECK_EQUAL(manager->selectedID(),selected);BOOST_CHECK(manager->soloed(nodes[1]->nodeId));
    MessageManager::getInstance()->runDispatchLoopUntil(100);
    auto snapshot=[&](const char* path){if(!std::getenv("EL_LOOPER_EDITOR_SNAPSHOT"))return;File f(path);auto out=f.createOutputStream();BOOST_REQUIRE(out);out->setPosition(0);out->truncate();PNGImageFormat format;BOOST_CHECK(format.writeImageToStream(editor.createComponentSnapshot(editor.getLocalBounds()),*out));};
    snapshot("/tmp/element-manager-grid.png");
    for(auto* b:buttons)if(b->getButtonText()=="Settings"){b->triggerClick();break;}
    MessageManager::getInstance()->runDispatchLoopUntil(100);snapshot("/tmp/element-manager-settings.png");
    buttons.clear();collect(editor);for(auto* b:buttons)if(b->getButtonText()=="MIDI"){b->triggerClick();break;}
    MessageManager::getInstance()->runDispatchLoopUntil(100);snapshot("/tmp/element-manager-midi.png");
    buttons.clear();collect(editor);for(auto* b:buttons)if(b->getButtonText()=="?"){b->triggerClick();break;}
    MessageManager::getInstance()->runDispatchLoopUntil(100);snapshot("/tmp/element-manager-help.png");
}

BOOST_AUTO_TEST_CASE (ManagerCompactControlsAndMasterDropRegression)
{
    PreparedGraph fixture(100,8);auto& graph=fixture.graph;
    auto* manager=new LooperManagerNode();auto* managerNode=graph.addNode(manager);
    auto* a=new LooperProcessor();auto* b=new LooperProcessor();
    auto* an=graph.addNode(new AudioProcessorNode(a));auto* bn=graph.addNode(new AudioProcessorNode(b));
    for(auto* n:{an,bn})BOOST_REQUIRE(graph.connectChannels(PortType::Midi,managerNode->nodeId,0,n->nodeId,0));
    graph.rebuild();a->setQuantized(true);
    AudioBuffer<float> audio(2,8);audio.clear();MidiBuffer midi;
    auto render=[&]{RenderContext rc(audio,audio,midi,8);graph.render(rc);a->view();b->view();};render();
    ValueTree data(types::Node);data.setProperty(tags::object,managerNode,nullptr);Node node(data,false);LooperManagerEditor editor(node);
    std::function<Component*(Component&,const String&)> find=[&](Component& c,const String& id)->Component*{if(c.getComponentID()==id)return &c;for(auto* child:c.getChildren())if(auto* result=find(*child,id))return result;return nullptr;};
    const auto aid=String(an->nodeId),bid=String(bn->nodeId);
    auto* record=dynamic_cast<Button*>(find(editor,"manager-record-"+aid));BOOST_REQUIRE(record);
    BOOST_REQUIRE(a->view().needsClock);BOOST_CHECK_EQUAL(record->getButtonText().toStdString(),"Record");
    auto* volume=dynamic_cast<Slider*>(find(editor,"manager-volume-"+aid));auto* pan=dynamic_cast<Slider*>(find(editor,"manager-pan-"+aid));BOOST_REQUIRE(volume);BOOST_REQUIRE(pan);
    const auto selected=manager->selectedID();volume->setValue(.37,sendNotificationSync);pan->setValue(-.45,sendNotificationSync);
    BOOST_CHECK_CLOSE(a->getVolume(),.37f,.01f);BOOST_CHECK_CLOSE(a->getPan(),-.45f,.01f);BOOST_CHECK_EQUAL(manager->selectedID(),selected);
    a->setVolume(.6f);a->setPan(.3f);MessageManager::getInstance()->runDispatchLoopUntil(100);
    BOOST_CHECK_CLOSE(float(volume->getValue()),.6f,.01f);BOOST_CHECK_CLOSE(float(pan->getValue()),.3f,.01f);
    auto* target=dynamic_cast<DragAndDropTarget*>(find(editor,"manager-master-drop"));BOOST_REQUIRE(target);
    DragAndDropTarget::SourceDetails first("jgod-looper:"+aid,find(editor,"manager-card-"+aid),{10,10});
    BOOST_REQUIRE(target->isInterestedInDragSource(first));target->itemDropped(first);MessageManager::getInstance()->runDispatchLoopUntil(100);
    BOOST_CHECK_EQUAL(manager->masterID(),an->nodeId);
    // The populated master card must itself accept a drop (it covers its zone).
    auto* card=find(editor,"manager-card-"+aid);BOOST_REQUIRE(card);target=dynamic_cast<DragAndDropTarget*>(card);BOOST_REQUIRE(target);
    DragAndDropTarget::SourceDetails second("jgod-looper:"+bid,find(editor,"manager-card-"+bid),{10,10});
    target->itemDropped(second);MessageManager::getInstance()->runDispatchLoopUntil(100);BOOST_CHECK_EQUAL(manager->masterID(),bn->nodeId);
    // Existing recording locks master replacement; no destructive workaround.
    BOOST_REQUIRE(b->request(LooperEngine::Action::record));render();BOOST_REQUIRE(b->request(LooperEngine::Action::play));render();
    BOOST_REQUIRE(manager->structureLocked());target=dynamic_cast<DragAndDropTarget*>(find(editor,"manager-card-"+bid));BOOST_REQUIRE(target);
    DragAndDropTarget::SourceDetails locked("jgod-looper:"+aid,find(editor,"manager-card-"+aid),{10,10});target->itemDropped(locked);MessageManager::getInstance()->runDispatchLoopUntil(100);
    BOOST_CHECK_EQUAL(manager->masterID(),bn->nodeId);
    AudioBuffer<float> partial(2,3);partial.clear();RenderContext partialContext(partial,partial,midi,3);graph.render(partialContext);MessageManager::getInstance()->runDispatchLoopUntil(100);
    auto* display=find(editor,"manager-status-display");BOOST_REQUIRE(display);BOOST_CHECK(display->isVisible());BOOST_CHECK_GE(display->getWidth(),180);
    if(std::getenv("EL_LOOPER_EDITOR_SNAPSHOT")){File f("/tmp/element-manager-compact-two.png");auto out=f.createOutputStream();BOOST_REQUIRE(out);out->setPosition(0);out->truncate();PNGImageFormat().writeImageToStream(editor.createComponentSnapshot(editor.getLocalBounds()),*out);}
}

BOOST_AUTO_TEST_CASE (RigViewBrowsesWithoutChangingPedalTargetAndReleasesRemovedEditors)
{
    PreparedGraph fixture(100,8);auto& graph=fixture.graph;
    auto* manager=new LooperManagerNode();auto* mn=graph.addNode(manager);
    Node root=Node::createGraph("Test rig");
    auto addModel=[&](Processor* object,const String& name){
        ValueTree tree(types::Node);tree.setProperty(tags::id,int(object->nodeId),nullptr);
        tree.setProperty(tags::name,name,nullptr);tree.setProperty(tags::object,object,nullptr);
        root.getNodesValueTree().addChild(tree,-1,nullptr);return Node(tree,false);
    };
    addModel(mn,"Verse");std::vector<Node> models;
    for(int i=0;i<4;++i){auto* p=new LooperProcessor();auto* n=graph.addNode(new AudioProcessorNode(p));
        BOOST_REQUIRE(graph.connectChannels(PortType::Midi,mn->nodeId,0,n->nodeId,0));models.push_back(addModel(n,"Loop "+String(i+1)));
    }
    graph.rebuild();
    BOOST_REQUIRE(manager->setMaster(models[0].getNodeId()));
    BOOST_REQUIRE(manager->setSyncMode(models[1].getNodeId(),1));
    BOOST_REQUIRE(manager->setSyncMode(models[2].getNodeId(),1));
    // Populate one looper so layout checks include actual layer controls/waves.
    auto* first=dynamic_cast<LooperProcessor*>(models[0].getObject()->getAudioProcessor());
    first->setQuantized(false);AudioBuffer<float> capture(2,8);MidiBuffer events;fillInput(capture,.12f,-.08f);
    first->request(LooperEngine::Action::record);first->processBlock(capture,events);first->view();
    first->request(LooperEngine::Action::record);first->processBlock(capture,events);first->view();
    for(int i=0;i<5;++i){first->request(LooperEngine::Action::overdub);first->processBlock(capture,events);first->view();first->request(LooperEngine::Action::overdub);first->processBlock(capture,events);first->view();}
    const auto selected=manager->selectedID();
    LooperRigView view([&]{return root;});view.setSize(1480,900);view.refresh();
    BOOST_CHECK(view.displayedLooper()==models[0]);view.browse(1);
    BOOST_CHECK(view.displayedLooper()==models[1]);BOOST_CHECK_EQUAL(manager->selectedID(),selected);
    view.browse(-1);BOOST_CHECK(view.displayedLooper()==models[0]);
    auto snapshot=[&](const char* name){if(!std::getenv("EL_LOOPER_EDITOR_SNAPSHOT"))return;
        File f(name);auto out=f.createOutputStream();BOOST_REQUIRE(out);out->setPosition(0);out->truncate();
        PNGImageFormat format;BOOST_CHECK(format.writeImageToStream(view.createComponentSnapshot(view.getLocalBounds()),*out));};
    MessageManager::getInstance()->runDispatchLoopUntil(150);
    snapshot("/tmp/element-rig-split.png");
    for(int mode:{1,2,0}){view.setViewMode(mode);BOOST_CHECK_EQUAL(manager->selectedID(),selected);BOOST_CHECK(view.displayedLooper()==models[0]);}
    view.setViewMode(1);snapshot("/tmp/element-rig-loopers.png");view.setViewMode(2);snapshot("/tmp/element-rig-layers.png");
    // Session replacement must not leave old live controls in the view.
    root=Node::createGraph("Empty rig");view.refresh();BOOST_CHECK(!view.displayedLooper().isValid());
    view.browse(1);view.setViewMode(0);BOOST_CHECK(!view.displayedLooper().isValid());
}


BOOST_AUTO_TEST_CASE (RigViewFollowsSectionButNotPedalSelection)
{
    Node root=Node::createGraph("Song");
    ProcessorPtr songObject=new SongManagerNode();auto* song=dynamic_cast<SongManagerNode*>(songObject.get());
    auto model=[](Processor* object,int id,const String& name){ValueTree t(types::Node);t.setProperty(tags::id,id,nullptr);t.setProperty(tags::name,name,nullptr);t.setProperty(tags::object,object,nullptr);return Node(t,false);};
    root.getNodesValueTree().addChild(model(songObject.get(),1,"Song Manager").data(),-1,nullptr);
    Node verse=Node::createGraph("Verse"),chorus=Node::createGraph("Chorus");verse.setProperty(tags::id,2);chorus.setProperty(tags::id,3);
    root.getNodesValueTree().addChild(verse.data(),-1,nullptr);root.getNodesValueTree().addChild(chorus.data(),-1,nullptr);
    ProcessorPtr a=new AudioProcessorNode(new LooperProcessor()),b=new AudioProcessorNode(new LooperProcessor());
    auto na=model(a.get(),10,"Rhythm"),nb=model(b.get(),10,"Chords");
    verse.getNodesValueTree().addChild(na.data(),-1,nullptr);chorus.getNodesValueTree().addChild(nb.data(),-1,nullptr);
    song->activeSection=2;
    LooperRigView view([&]{return root;});view.refresh();BOOST_CHECK(view.displayedLooper()==na);
    song->activeSection=3;view.refresh();BOOST_CHECK(view.displayedLooper()==nb);
    BOOST_CHECK_EQUAL(song->activeSection.load(),3u);
    chorus.getNodesValueTree().removeChild(nb.data(),nullptr);view.refresh();BOOST_CHECK(!view.displayedLooper().isValid());
}


BOOST_AUTO_TEST_CASE (RigMetronomeDoesNotMuteCountInAndCompactSettingsStayNarrow)
{
    Node root=Node::createGraph("Rig");
    auto add=[&](int id,const String& name){ValueTree t(types::Node);t.setProperty(tags::id,id,nullptr);t.setProperty(tags::name,name,nullptr);root.getNodesValueTree().addChild(t,-1,nullptr);return Node(t,false);};
    // Put count-in first: discovery must not accidentally choose it.
    auto countIn=add(1,"TICK - Count-in"),tick=add(2,"TICK");
    LooperRigView view([&]{return root;});
    auto find=[](auto&& self,Component& c,const String& name)->Button* {
        if(auto* b=dynamic_cast<Button*>(&c);b && b->getButtonText()==name)return b;
        for(auto* child:c.getChildren())if(auto* result=self(self,*child,name))return result;
        return nullptr;
    };
    auto* toggle=find(find,view,"Metronome");BOOST_REQUIRE(toggle);BOOST_CHECK(toggle->getToggleState());
    toggle->setToggleState(false,dontSendNotification);toggle->onClick();
    BOOST_CHECK(tick.isMuted());BOOST_CHECK(!countIn.isMuted());
    tick.setMuted(false);view.refresh();BOOST_CHECK(toggle->getToggleState());
    LooperProcessor p;auto compact=std::unique_ptr<AudioProcessorEditor>(p.createCompactEditorForNode(Node(),[]{}));
    BOOST_CHECK(!find(find,*compact,"Layers...")->isVisible());BOOST_CHECK(!find(find,*compact,"Show")->isVisible());
    compact->setSize(440,420);find(find,*compact,"Settings")->onClick();BOOST_CHECK_EQUAL(compact->getWidth(),440);BOOST_CHECK_EQUAL(compact->getHeight(),420);
    SongManagerEditor song{Node()};find(find,song,"Settings")->onClick();BOOST_CHECK_EQUAL(song.getWidth(),420);
}

// Opt-in documentation fixture: actual editors and synthetic audio, no audio device.
BOOST_AUTO_TEST_CASE (Rc1DocumentationScreenshots)
{
    const auto* destination=std::getenv("EL_RC1_SCREENSHOTS");if(!destination)return;
    File folder(destination);BOOST_REQUIRE(folder.createDirectory().wasOk());
    PreparedGraph fixture(48000,480);Node root=Node::createGraph("Demo Song");
    auto model=[&](const Node& parent,Processor* object,const String& name,bool graph=false){
        Node node=graph?Node::createGraph(name):Node(ValueTree(types::Node),false);
        node.setProperty(tags::id,int(object->nodeId));node.setProperty(tags::name,name);node.setProperty(tags::object,object);
        parent.getNodesValueTree().addChild(node.data(),-1,nullptr);return node;
    };
    auto* song=new SongManagerNode();auto songModel=model(root,fixture.graph.addNode(song),"Song Manager");
    std::vector<Node> sections;std::vector<GraphNode*> graphs;
    for(const auto& name:{"Verse","Chorus","Bridge"}){auto* graph=new GraphNode(*element::test::context());fixture.graph.addNode(graph);graphs.push_back(graph);sections.push_back(model(root,graph,name,true));}
    auto* manager=new LooperManagerNode();auto* mn=graphs[0]->addNode(manager);auto managerModel=model(sections[0],mn,"LM - Verse");
    std::vector<LooperProcessor*> processors;std::vector<Node> models;
    for(const auto& name:{"Rhythm","Bass","Harmony","Percussion","Texture","Lead"}){
        auto* p=new LooperProcessor();p->setMemoryLimits(10,4,4,true);p->setQuantized(false);
        auto* node=graphs[0]->addNode(new AudioProcessorNode(p));processors.push_back(p);models.push_back(model(sections[0],node,name));
        BOOST_REQUIRE(graphs[0]->connectChannels(PortType::Midi,mn->nodeId,0,node->nodeId,0));
    }
    for(auto* graph:graphs)graph->rebuild();fixture.graph.rebuild();
    song->setOrder({graphs[0]->nodeId,graphs[1]->nodeId,graphs[0]->nodeId,graphs[1]->nodeId,graphs[2]->nodeId});song->currentIndex=0;song->activeSection=graphs[0]->nodeId;
    BOOST_REQUIRE(manager->setMaster(models[0].getNodeId()));BOOST_REQUIRE(manager->setSyncMode(models[1].getNodeId(),1));BOOST_REQUIRE(manager->setSyncMode(models[2].getNodeId(),2));
    AudioBuffer<float> audio(2,480);MidiBuffer midi;
    for(size_t i=0;i<processors.size();++i){auto* p=processors[i];int64_t frame=0;
        auto render=[&](int blocks){for(int b=0;b<blocks;++b){for(int f=0;f<480;++f,++frame){const double t=double(frame)/48000;const float sample=float(.22*std::sin(t*(110+27*i)*MathConstants<double>::twoPi)*(.2+.8*std::pow(std::max(0.,std::sin(t*5)),2)));audio.setSample(0,f,sample);audio.setSample(1,f,sample*.85f);}p->processBlock(audio,midi);p->view();}};
        p->request(LooperEngine::Action::record);render(200);p->request(LooperEngine::Action::record);render(1);
        if(i==0)for(int take=0;take<2;++take){p->request(LooperEngine::Action::overdub);render(599);p->request(LooperEngine::Action::overdub);render(1);}
        if(i==2||i==4)p->request(LooperEngine::Action::stop);
        if(i==5)p->request(LooperEngine::Action::overdub);
        render(20);if(i<3)p->setQuantized(true);
    }
    manager->select(models[5].getNodeId());audio.clear();RenderContext rc(audio,audio,midi,480);manager->render(rc);
    auto save=[&](Component& editor,const String& filename){MessageManager::getInstance()->runDispatchLoopUntil(180);auto out=folder.getChildFile(filename).createOutputStream();BOOST_REQUIRE(out);out->setPosition(0);out->truncate();BOOST_CHECK(PNGImageFormat().writeImageToStream(editor.createComponentSnapshot(editor.getLocalBounds(),true,2),*out));};
    LooperManagerEditor managerEditor(managerModel);managerEditor.setSize(1016,700);save(managerEditor,"manager.png");
    SongManagerEditor songEditor(songModel);save(songEditor,"song-manager.png");
    processors[5]->request(LooperEngine::Action::overdub);for(int b=0;b<20;++b){audio.clear();processors[5]->processBlock(audio,midi);processors[5]->view();}save(managerEditor,"manager-playing.png");
    auto small=std::unique_ptr<AudioProcessorEditor>(processors[0]->createCompactEditorForNode(models[0],[]{}));save(*small,"looper.png");
    auto layers=std::unique_ptr<AudioProcessorEditor>(processors[0]->createLayersEditorForNode(models[0]));layers->setSize(900,430);save(*layers,"layers.png");
    auto button=[](auto&& self,Component& c,const String& name)->Button*{if(auto* b=dynamic_cast<Button*>(&c);b && b->getButtonText()==name)return b;for(auto* child:c.getChildren())if(auto* b=self(self,*child,name))return b;return nullptr;};
    button(button,*small,"Settings")->onClick();save(*small,"looper-settings.png");
    button(button,managerEditor,"Settings")->onClick();save(managerEditor,"manager-settings.png");
    button(button,songEditor,"Settings")->onClick();save(songEditor,"song-settings.png");
    LooperRigView rig([&]{return root;});rig.setSize(1480,900);save(rig,"rig-demo.png");
}

// Opt-in native-editor presentation images. Audio is synthetic; no user session is loaded.
BOOST_AUTO_TEST_CASE (LooperPresentationScreenshots)
{
    const auto* destination=std::getenv("EL_LOOPER_PRESENTATION_SCREENSHOTS");if(!destination)return;
    File folder(destination);BOOST_REQUIRE(folder.createDirectory().wasOk());
    LooperProcessor p;p.setMemoryLimits(4,10,4,true);p.setQuantized(false);p.prepareToPlay(48000,480);
    Node model(ValueTree(types::Node),false);model.setProperty(tags::name,"Rhythm");
    AudioBuffer<float> audio(2,480);MidiBuffer midi;int64_t frame=0;int voice=0;bool silence=false;
    auto render=[&](int blocks){for(int b=0;b<blocks;++b){for(int f=0;f<480;++f,++frame){
        const double t=double(frame)/48000, beat=std::fmod(t*4,1.0), slow=std::fmod(t*2,1.0);
        const double env=voice%3==0?std::exp(-beat*(6+voice)):
            voice%3==1?std::pow(std::sin(slow*MathConstants<double>::pi),2):
            (beat<.65?std::sin(beat/.65*MathConstants<double>::pi):0);
        const double accent=.45+.45*std::pow(std::sin(t*3.1+voice),2);
        const float sample=float(env*accent*(.75*std::sin(t*(82.4+voice*41.2)*MathConstants<double>::twoPi)+.25*std::sin(t*(164.8+voice*61.8)*MathConstants<double>::twoPi)));
        audio.setSample(0,f,silence?0:sample);audio.setSample(1,f,silence?0:sample*.9f);
    }p.processBlock(audio,midi);p.view();}};
    auto save=[&](Component& editor,const String& filename){for(int tick=0;tick<(filename.startsWith("01")?6:60);++tick){render(1);MessageManager::getInstance()->runDispatchLoopUntil(10);}
        auto out=folder.getChildFile(filename).createOutputStream();BOOST_REQUIRE(out);out->setPosition(0);out->truncate();
        BOOST_REQUIRE(PNGImageFormat().writeImageToStream(editor.createComponentSnapshot(editor.getLocalBounds(),true,2),*out));};
    p.request(LooperEngine::Action::record);render(125);BOOST_REQUIRE_EQUAL(p.view().state,1);
    auto compact=std::unique_ptr<AudioProcessorEditor>(p.createCompactEditorForNode(model,[]{}));save(*compact,"01-compact-recording.png");
    render(69);p.request(LooperEngine::Action::record);render(5);
    for(voice=1;voice<=3;++voice){p.request(LooperEngine::Action::overdub);render(voice==3?380:180);p.request(LooperEngine::Action::overdub);render(5);}
    render(65);BOOST_REQUIRE_EQUAL(p.view().state,2);BOOST_REQUIRE_GE(p.view().count,4);
    auto full=std::unique_ptr<AudioProcessorEditor>(p.createEditorForNode(model));full->setSize(460,960);save(*full,"02-looper-playing-layers.png");
    struct DemoClock:AudioPlayHead {int64_t& frame;explicit DemoClock(int64_t& f):frame(f){} Optional<PositionInfo> getPosition()const override{PositionInfo info;info.setBpm(120);info.setTimeSignature(TimeSignature{4,4});info.setPpqPosition(double(frame)/24000);info.setIsPlaying(true);return info;}} clock(frame);
    p.setPlayHead(&clock);p.setQuantized(true);
    auto control=p.recordingControl(1);control.start=2;control.bars=2;BOOST_REQUIRE(p.setRecordingControl(1,control));
    silence=true;p.request(LooperEngine::Action::overdub);render(1);BOOST_REQUIRE(p.view().armed);
    auto layers=std::unique_ptr<AudioProcessorEditor>(p.createLayersEditorForNode(model));layers->setSize(900,440);
    render(1);MessageManager::getInstance()->runDispatchLoopUntil(40);
    auto findWave=[](auto&& self,Component& c)->Component*{if(String(typeid(c).name()).contains("LayerWaveform") && c.getHeight()==20 && c.isVisible())return &c;for(auto* child:c.getChildren())if(auto* result=self(self,*child))return result;return nullptr;};
    auto* wave=findWave(findWave,*layers);BOOST_REQUIRE(wave);
    wave->mouseDown(MouseEvent(Desktop::getInstance().getMainMouseSource(),{10,10},ModifierKeys::leftButtonModifier,1,0,0,0,0,wave,wave,Time::getCurrentTime(),{10,10},Time::getCurrentTime(),1,false));
    save(*layers,"03-layers-waiting.png");BOOST_REQUIRE(p.view().armed);
}

BOOST_AUTO_TEST_SUITE_END()
