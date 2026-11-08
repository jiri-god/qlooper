// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nodes/samplepad.hpp"
#include "nodes/samplepadbank.hpp"
#include "binarydata.hpp"
#include <cmath>
#include <cstring>
namespace element {
using namespace juce;
SamplePadNode::SamplePadNode() : Processor (0) { setName ("Sample Pad"); }
void SamplePadNode::refreshPorts()
{
    if (getNumPorts())
        return;
    PortList p;
    p.add (PortType::Audio, 0, 0, "left", "Left", false);
    p.add (PortType::Audio, 1, 1, "right", "Right", false);
    p.add (PortType::Midi, 2, 0, "midi", "MIDI", true);
    setPorts (p);
}
void SamplePadNode::getPluginDescription (PluginDescription& d) const
{
    d.name = d.descriptiveName = "Sample Pad";
    d.fileOrIdentifier = "element.samplePad";
    d.uniqueId = 0x53504431;
    d.pluginFormatName = "Element";
    d.manufacturerName = "Jgod";
    d.category = "Instrument";
    d.version = "1.0.0";
    d.numInputChannels = 0;
    d.numOutputChannels = 2;
    d.isInstrument = true;
}
Result SamplePadNode::loadAudio (const AudioBuffer<float>& audio, double sr, const String& name)
{
    const ScopedLock controlGuard (controlLock);
    if (! std::isfinite (sr) || sr < 8000 || sr > 384000 || audio.getNumChannels() < 1 || audio.getNumChannels() > 2 || audio.getNumSamples() < 1 || audio.getNumSamples() > sr * 10 || int64 (audio.getNumSamples()) * audio.getNumChannels() * 4 > 16 * 1024 * 1024)
        return Result::fail ("Use mono/stereo audio, up to 10 seconds and 16 MB decoded (8–384 kHz).");
    for (int c = 0; c < audio.getNumChannels(); ++c)
        for (int i = 0; i < audio.getNumSamples(); ++i)
            if (! std::isfinite (audio.getSample (c, i)))
                return Result::fail ("Sound contains invalid audio values.");
    // Reclaim only objects neither published nor protected by the audio reader.
    // Sequential consistency pairs with its publish/validate hazard protocol.
    for (auto& slot : owned)
        if (slot && slot.get() != published.load() && slot.get() != hazard.load())
            slot.reset();
    auto next = std::make_unique<Sample>();
    next->audio.makeCopyOf (audio);
    next->rate = sr;
    next->name = name;
    convertSample (*next, rate);
    for (auto& slot : owned)
        if (! slot)
        {
            slot = std::move (next);
            published.store (slot.get());
            return Result::ok();
        }
    return Result::fail ("Sound is busy.");
}
Result SamplePadNode::loadFile (const File& file)
{
    AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> reader (formats.createReaderFor (file));
    if (! reader)
        return Result::fail ("Cannot read this audio file.");
    if (reader->lengthInSamples < 1 || reader->lengthInSamples > reader->sampleRate * 10 || reader->numChannels < 1 || reader->numChannels > 2 || reader->lengthInSamples * reader->numChannels * 4 > 16 * 1024 * 1024)
        return Result::fail ("Use a mono/stereo sound up to 10 seconds and 16 MB decoded.");
    AudioBuffer<float> audio (int (reader->numChannels), int (reader->lengthInSamples));
    if (! reader->read (&audio, 0, audio.getNumSamples(), 0, true, true))
        return Result::fail ("Could not read all audio samples.");
    return loadAudio (audio, reader->sampleRate, file.getFileNameWithoutExtension());
}
Result SamplePadNode::loadSampleData (const void* data, size_t bytes, const String& name)
{
    AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> reader (formats.createReaderFor (std::make_unique<MemoryInputStream> (data, bytes, false)));
    if (! reader)
        return Result::fail ("Cannot decode bank sound.");
    if (reader->lengthInSamples < 1 || reader->lengthInSamples > reader->sampleRate * 10 || reader->numChannels < 1 || reader->numChannels > 2 || reader->lengthInSamples * reader->numChannels * 4 > 16 * 1024 * 1024)
        return Result::fail ("Bank sound exceeds size limits.");
    AudioBuffer<float> audio (int (reader->numChannels), int (reader->lengthInSamples));
    if (! reader->read (&audio, 0, audio.getNumSamples(), 0, true, true))
        return Result::fail ("Cannot read bank sound.");
    return loadAudio (audio, reader->sampleRate, name);
}
String SamplePadNode::sampleName() const
{
    const ScopedLock controlGuard (controlLock);
    auto* p = published.load();
    return p ? p->name : String ("No sound loaded");
}
void SamplePadNode::convertSample (Sample& sample, double target)
{
    sample.playback.makeCopyOf (sample.audio);
    if (sample.rate == target)
        return;
    // Offline Blackman-windowed sinc, zero padded for a one-shot (never wraps).
    auto convert = [] (AudioBuffer<float>& input, int count) {
        const int old = input.getNumSamples();
        const double ratio = double (count) / old, cutoff = ratio < 1 ? .94 * ratio : 1.;
        const int radius = int (std::ceil (32 / std::min (1., ratio))), taps = radius * 2, phases = 1024;
        constexpr double pi = 3.14159265358979323846;
        std::vector<double> kernel (size_t (phases) * taps);
        for (int phase = 0; phase < phases; ++phase)
        {
            double sum = 0;
            for (int t = 0; t < taps; ++t)
            {
                const double x = t - radius + 1 - double (phase) / phases;
                const double sinc = std::abs (x) < 1.e-12 ? cutoff : std::sin (pi * cutoff * x) / (pi * x);
                const double w = std::abs (x) >= radius ? 0 : .42 + .5 * std::cos (pi * x / radius) + .08 * std::cos (2 * pi * x / radius);
                sum += kernel[size_t (phase) * taps + t] = sinc * w;
            }
            for (int t = 0; t < taps; ++t)
                kernel[size_t (phase) * taps + t] /= sum;
        }
        AudioBuffer<float> output (input.getNumChannels(), count);
        for (int i = 0; i < count; ++i)
        {
            const double pos = double (i) * old / count;
            int center = int (pos), phase = int (std::llround ((pos - center) * phases));
            if (phase == phases)
            {
                phase = 0;
                ++center;
            }
            for (int c = 0; c < input.getNumChannels(); ++c)
            {
                double sum = 0;
                for (int t = 0; t < taps; ++t)
                {
                    const int at = center - radius + 1 + t;
                    if (at >= 0 && at < old)
                        sum += input.getSample (c, at) * kernel[size_t (phase) * taps + t];
                }
                output.setSample (c, i, float (sum));
            }
        }
        input = std::move (output);
    };
    const int count = jmax (1, int (std::llround (sample.audio.getNumSamples() * target / sample.rate)));
    while (int64 (sample.playback.getNumSamples()) > int64 (count) * 4)
        convert (sample.playback, (sample.playback.getNumSamples() + 1) / 2);
    convert (sample.playback, count);
}
void SamplePadNode::prepareToRender (double sr, int)
{
    const ScopedLock controlGuard (controlLock);
    rate = std::isfinite (sr) && sr >= 8000 && sr <= 384000 ? sr : 44100;
    releaseResources();
    for (auto& sample : owned)
        if (sample)
            convertSample (*sample, rate);
}
void SamplePadNode::releaseResources()
{
    for (auto& v : voices)
        v.position = -1;
    held = false;
    triggerRequest.store (false);
}
void SamplePadNode::startVoice (float velocity) noexcept
{
    if (! current)
        return;
    Voice* target = &voices[0];
    for (auto& v : voices)
    {
        if (v.position < 0)
        {
            target = &v;
            break;
        }
        if (v.age < target->age)
            target = &v;
    }
    *target = { 0, velocity, ++age };
}
void SamplePadNode::process (AudioBuffer<float>& audio, MidiBuffer& midi, bool bypass) noexcept
{
    Sample* sample;
    bool changed = false;
    do
    {
        sample = published.load();
        changed |= sample != current;
        hazard.store (sample);
    } while (sample != published.load());
    if (changed)
    {
        for (auto& v : voices)
            v.position = -1;
        current = sample;
    }
    audio.clear();
    const int map = mapping.load(), type = kind.load(), ch = channel.load();
    if (map != lastMapping || type != lastKind || ch != lastChannel)
    {
        held = false;
        lastMapping = map;
        lastKind = type;
        lastChannel = ch;
    }
    if (bypass)
    {
        releaseResources();
        midi.clear();
        return;
    }
    if (triggerRequest.exchange (false))
        startVoice (1);
    auto it = midi.begin();
    const auto end = midi.end();
    const float volume = jlimit (0.f, 2.f, gain.load());
    for (int frame = 0; frame < audio.getNumSamples(); ++frame)
    {
        while (it != end && (*it).samplePosition <= frame)
        {
            const auto metadata = *it;
            ++it;
            if (metadata.numBytes > 3)
                continue;
            const auto msg = metadata.getMessage();
            if (msg.isAllSoundOff() || msg.isAllNotesOff())
            {
                for (auto& v : voices)
                    v.position = -1;
                held = false;
                continue;
            }
            if (learn.load() && ((msg.isController() && msg.getControllerValue() > 0) || msg.isNoteOn()))
            {
                kind.store (msg.isNoteOn() ? 1 : 0);
                mapping.store (msg.isNoteOn() ? msg.getNoteNumber() : msg.getControllerNumber());
                channel.store (msg.getChannel());
                learn.store (false);
                held = true;
                lastMapping = mapping.load();
                lastKind = kind.load();
                lastChannel = channel.load();
                continue;
            }
            if (msg.getChannel() != channel.load())
                continue;
            if (kind.load() == 0 && msg.isController() && msg.getControllerNumber() == mapping.load())
            {
                const bool down = msg.getControllerValue() > 0;
                if (down && ! held)
                    startVoice (1);
                held = down;
            }
            else if (kind.load() == 1 && msg.isNoteOn() && msg.getNoteNumber() == mapping.load())
                startVoice (msg.getFloatVelocity());
        }
        if (! current)
            continue;
        const int n = current->playback.getNumSamples();
        const double step = 1;
        for (auto& v : voices)
            if (v.position >= 0)
            {
                const int a = int (v.position), b = jmin (a + 1, n - 1);
                const float mix = float (v.position - a);
                for (int c = 0; c < jmin (2, audio.getNumChannels()); ++c)
                {
                    const float* src = current->playback.getReadPointer (jmin (c, current->playback.getNumChannels() - 1));
                    audio.addSample (c, frame, (src[a] + (src[b] - src[a]) * mix) * volume * v.velocity);
                }
                v.position += step;
                if (v.position >= n)
                    v.position = -1;
            }
    }
    midi.clear();
}
void SamplePadNode::render (RenderContext& rc) { process (rc.audio, *rc.midi.getWriteBuffer (0)); }
void SamplePadNode::renderBypassed (RenderContext& rc) { process (rc.audio, *rc.midi.getWriteBuffer (0), true); }
void SamplePadNode::getState (MemoryBlock& data)
{
    const ScopedLock controlGuard (controlLock);
    MemoryOutputStream out (data, false);
    out.writeInt (0x53504431);
    out.writeInt (1);
    out.writeInt (mapping.load());
    out.writeInt (channel.load());
    out.writeInt (kind.load());
    out.writeFloat (gain.load());
    auto* s = published.load();
    out.writeString (s ? s->name : String());
    out.writeDouble (s ? s->rate : 44100);
    out.writeInt (s ? s->audio.getNumChannels() : 0);
    out.writeInt (s ? s->audio.getNumSamples() : 0);
    if (s)
        for (int c = 0; c < s->audio.getNumChannels(); ++c)
            for (int i = 0; i < s->audio.getNumSamples(); ++i)
                out.writeFloat (s->audio.getSample (c, i));
}
void SamplePadNode::setState (const void* data, int size)
{
    const ScopedLock controlGuard (controlLock);
    if (! data || size < 41 || size > 16 * 1024 * 1024 + 8192)
        return;
    // The variable UTF-8 name must terminate before the fixed sample header.
    const auto* raw = static_cast<const char*> (data);
    if (! std::memchr (raw + 24, 0, size_t (jmin (4096, size - 40))))
        return;
    MemoryInputStream in (data, size, false);
    if (in.readInt() != 0x53504431 || in.readInt() != 1)
        return;
    const int map = in.readInt(), ch = in.readInt(), type = in.readInt();
    const float vol = in.readFloat();
    const auto name = in.readString();
    const double sr = in.readDouble();
    const int channels = in.readInt(), frames = in.readInt();
    if (! std::isfinite (sr) || sr < 8000 || sr > 384000)
        return;
    if (channels < 0 || channels > 2 || frames < 0 || ((channels == 0) != (frames == 0)) || int64 (channels) * frames * 4 > 16 * 1024 * 1024 || in.getNumBytesRemaining() != int64 (channels) * frames * 4 || ! std::isfinite (vol))
        return;
    if (channels && frames)
    {
        AudioBuffer<float> audio (channels, frames);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < frames; ++i)
                audio.setSample (c, i, in.readFloat());
        if (loadAudio (audio, sr, name).failed())
            return;
    }
    else
        published.store (nullptr);
    mapping.store (jlimit (-1, 127, map));
    channel.store (jlimit (1, 16, ch));
    kind.store (jlimit (0, 1, type));
    gain.store (jlimit (0.f, 2.f, vol));
}
SamplePadEditor::SamplePadEditor (const Node& n) : NodeEditor (n), pad (dynamic_cast<SamplePadNode*> (n.getObject()))
{
    for (Component* c : std::initializer_list<Component*> { &name, &status, &midiLabel, &load, &hit, &assign, &clear, &volume, &bank })
        addAndMakeVisible (c);
    bank.setTextWhenNothingSelected ("Choose a factory drum sound...");
    int bankId = 1;
    for (const auto& entry : samplePadBank)
        bank.addItem (entry.name, bankId++);
    bank.onChange = [this] {if(!pad||bank.getSelectedId()<1)return;const auto& entry=samplePadBank[bank.getSelectedId()-1];int bytes=0;const auto* data=res::getNamedResource(entry.resource,bytes);const auto result=data?pad->loadSampleData(data,size_t(bytes),entry.name):Result::fail("Bank sound is unavailable.");status.setText(result.failed()?result.getErrorMessage():"Factory sound saved inside the session.",dontSendNotification);timerCallback(); };
    volume.setRange (0, 2, .001);
    volume.setSliderStyle (Slider::LinearHorizontal);
    volume.setTextBoxStyle (Slider::TextBoxRight, false, 65, 26);
    volume.setDoubleClickReturnValue (true, 1);
    volume.onValueChange = [this] {if(pad)pad->gain.store(float(volume.getValue())); };
    hit.onClick = [this] {if(pad)pad->trigger(); };
    assign.onClick = [this] {if(pad)pad->learn.store(!pad->learn.load()); };
    clear.onClick = [this] {if(pad){pad->mapping.store(-1);pad->learn.store(false);} };
    load.onClick = [this] {chooser=std::make_unique<FileChooser>("Choose a short drum sound",File(),"*.wav;*.aif;*.aiff;*.flac");Component::SafePointer<SamplePadEditor> safe(this);chooser->launchAsync(FileBrowserComponent::openMode|FileBrowserComponent::canSelectFiles,[safe](const FileChooser& f){if(safe&&safe->pad&&f.getResult().existsAsFile()){const auto result=safe->pad->loadFile(f.getResult());safe->status.setText(result.failed()?result.getErrorMessage():"Sound saved inside the session.",dontSendNotification);safe->timerCallback();}}); };
    status.setText ("One press = one hit. Release lets the sound decay.", dontSendNotification);
    setSize (480, 270);
    startTimerHz (10);
    timerCallback();
}
void SamplePadEditor::timerCallback()
{
    if (! pad)
        return;
    name.setText (pad->sampleName(), dontSendNotification);
    volume.setValue (pad->gain.load(), dontSendNotification);
    assign.setButtonText (pad->learn.load() ? "Listening... (cancel)" : "Assign pedal");
    midiLabel.setText (pad->mapping.load() < 0 ? "MIDI: unassigned" : "MIDI: " + String (pad->kind.load() ? "Note " : "CC ") + String (pad->mapping.load()) + " / channel " + String (pad->channel.load()), dontSendNotification);
}
void SamplePadEditor::resized()
{
    auto r = getLocalBounds().reduced (12);
    name.setBounds (r.removeFromTop (28));
    bank.setBounds (r.removeFromTop (36).reduced (2));
    auto row = r.removeFromTop (34);
    load.setBounds (row.removeFromLeft (180).reduced (2));
    hit.setBounds (row.reduced (2));
    volume.setBounds (r.removeFromTop (36));
    midiLabel.setBounds (r.removeFromTop (26));
    row = r.removeFromTop (34);
    assign.setBounds (row.removeFromLeft (220).reduced (2));
    clear.setBounds (row.reduced (2));
    status.setBounds (r);
}
void SamplePadEditor::paint (Graphics& g) { g.fillAll (Colours::darkgrey.darker()); }
} // namespace element
