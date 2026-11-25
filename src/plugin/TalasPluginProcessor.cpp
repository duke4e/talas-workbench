#include "plugin/TalasPluginProcessor.h"

#include "plugin/TalasPluginEditor.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace talas::plugin
{
namespace
{
[[nodiscard]] juce::String text(const std::string& value)
{
    return juce::String::fromUTF8(value.c_str());
}

[[nodiscard]] double parameterValue(const juce::RangedAudioParameter& parameter, const PluginParameterSpec& spec)
{
    return parameterValueFromNormalized(spec, parameter.getValue());
}
}

juce::AudioProcessor::BusesProperties TalasPluginProcessor::busesFor(PluginKind kind)
{
    BusesProperties buses;
    if (kind == PluginKind::Effect)
        buses = buses.withInput("Input", juce::AudioChannelSet::stereo(), true);
    return buses.withOutput("Output", juce::AudioChannelSet::stereo(), true);
}

TalasPluginProcessor::TalasPluginProcessor()
    : juce::AudioProcessor(busesFor(runtime_identity::templateKind())),
      kind_(runtime_identity::templateKind()),
      identity_(runtime_identity::identity()),
      registry_(talas::core::makeBuiltinNodeRegistry())
{
    project_ = loadPluginProject(runtime_identity::resourcesDirectory(), registry_, kind_);
    if (project_.ok())
    {
        auto mapping = mapProjectParameters(*project_.document, registry_, kind_);
        specs_ = std::move(mapping.parameters);
        inputChannels_ = projectAudioInputChannels(*project_.document);
        monoSumInput_ = inputChannels_[0]
            && std::none_of(inputChannels_.begin() + 1, inputChannels_.end(), [](bool channel) { return channel; });
        setStatus(runtime_identity::identityProblem().empty() ? juce::String{"Loading"}
                                                              : text(runtime_identity::identityProblem()));
    }
    else
    {
        setStatus(text(project_.error));
    }
    for (const auto& spec : specs_)
    {
        auto parameter = makePluginParameter(spec);
        parameters_.push_back(parameter.get());
        addParameter(parameter.release());
    }
}

TalasPluginProcessor::~TalasPluginProcessor()
{
    stopEngine();
}

const juce::String TalasPluginProcessor::getName() const
{
    return text(identity_.name);
}

bool TalasPluginProcessor::acceptsMidi() const
{
    return kind_ == PluginKind::Instrument;
}

double TalasPluginProcessor::getTailLengthSeconds() const
{
    // A project's echoes and releases have no length known in advance: an
    // effect asks to keep being processed.
    return kind_ == PluginKind::Effect ? std::numeric_limits<double>::infinity() : 0.0;
}

bool TalasPluginProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    if (kind_ == PluginKind::Instrument)
        return layouts.getMainInputChannelSet().isDisabled();
    const auto input = layouts.getMainInputChannelSet();
    return input == juce::AudioChannelSet::mono() || input == juce::AudioChannelSet::stereo();
}

juce::RangedAudioParameter* TalasPluginProcessor::parameterFor(std::size_t index) const noexcept
{
    return index < parameters_.size() ? parameters_[index] : nullptr;
}

juce::String TalasPluginProcessor::statusText() const
{
    const std::scoped_lock lock{statusMutex_};
    return status_;
}

void TalasPluginProcessor::setStatus(juce::String status)
{
    const std::scoped_lock lock{statusMutex_};
    status_ = std::move(status);
}

void TalasPluginProcessor::stopEngine()
{
    engineReady_.store(false, std::memory_order_release);
    if (bridge_ != nullptr)
    {
        bridge_->clearAfterAudioStopped();
        bridge_.reset();
    }
}

void TalasPluginProcessor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock)
{
    stopEngine();
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    preparedBlockFrames_ = pluginPreparedBlockFrames(maximumExpectedSamplesPerBlock);
    inputScratch_.assign(2, std::vector<float>(preparedBlockFrames_, 0.0F));
    eventIds_.assign(specs_.size(), std::nullopt);
    sentValues_.assign(specs_.size(), std::numeric_limits<double>::quiet_NaN());
    for (auto& channel : heldNotes_)
        channel.reset();
    callbackTimeSeconds_ = 1.0;
    setLatencySamples(0);
    if (!project_.ok())
        return;

    PluginStageSettings settings;
    settings.sampleRate = std::round(sampleRate_);
    settings.maxBlockFrames = preparedBlockFrames_;
    for (std::size_t index = 0; index < specs_.size(); ++index)
    {
        const auto value = parameterValue(*parameters_[index], specs_[index]);
        settings.eventValues.push_back({specs_[index].eventName, value, specs_[index].domain});
    }
    // The Sound starts from the parameters' values (those it fixes when it
    // starts take them too), and the first block sends every value again.
    auto document = *project_.document;
    applyPluginParameterValues(document, settings.eventValues);
    auto bridge = std::make_unique<talas::runtime::ProjectRuntimeBridge>(std::move(document), registry_);
    const auto staged = stagePluginProject(*bridge, registry_, project_.projectFile, settings);
    if (!staged.ok())
    {
        bridge->clearAfterAudioStopped();
        setStatus(text(staged.error));
        return;
    }
    bridge->setPlaying(true);
    for (std::size_t index = 0; index < specs_.size(); ++index)
    {
        eventIds_[index] = bridge->realtimeEventValueIdForLatestStage(specs_[index].eventName, specs_[index].domain);
    }
    bridge_ = std::move(bridge);
    setLatencySamples(static_cast<int>(std::min<std::size_t>(staged.latencyFrames, 1U << 24U)));
    juce::String status = "Playing at " + juce::String(settings.sampleRate / 1000.0, 1) + " kHz";
    if (staged.latencyFrames > 0)
        status << ", " << static_cast<int>(staged.latencyFrames) << " frames of latency";
    if (!runtime_identity::identityProblem().empty())
        status << " (" << text(runtime_identity::identityProblem()) << ")";
    setStatus(status);
    engineReady_.store(true, std::memory_order_release);
}

void TalasPluginProcessor::releaseResources()
{
    stopEngine();
    setStatus("Stopped");
}

void TalasPluginProcessor::sendParameterChanges() noexcept
{
    for (std::size_t index = 0; index < specs_.size(); ++index)
    {
        if (!eventIds_[index].has_value())
            continue;
        const auto value = parameterValue(*parameters_[index], specs_[index]);
        if (value == sentValues_[index])
            continue;
        if (bridge_->enqueueEventValue(*eventIds_[index], value, specs_[index].domain, 0))
            sentValues_[index] = value;
    }
}

void TalasPluginProcessor::releaseHeldNotes(int channel) noexcept
{
    auto& held = heldNotes_[static_cast<std::size_t>(channel - 1)];
    for (int note = 0; note < 128; ++note)
    {
        if (held.test(static_cast<std::size_t>(note)))
            (void)bridge_->enqueueMidiNoteOff(channel, note, callbackTimeSeconds_);
    }
    held.reset();
}

void TalasPluginProcessor::sendMidi(const juce::MidiMessage& message, int frameOffset) noexcept
{
    const auto arrival = callbackTimeSeconds_ + static_cast<double>(frameOffset) / sampleRate_;
    const auto channel = std::clamp(message.getChannel(), 1, 16);
    auto& held = heldNotes_[static_cast<std::size_t>(channel - 1)];
    if (message.isNoteOn())
    {
        const auto note = message.getNoteNumber();
        if (bridge_->enqueueMidiNoteOn(channel, note, std::clamp<int>(message.getVelocity(), 1, 127), arrival))
            held.set(static_cast<std::size_t>(note));
    }
    else if (message.isNoteOff())
    {
        const auto note = message.getNoteNumber();
        (void)bridge_->enqueueMidiNoteOff(channel, note, arrival);
        held.reset(static_cast<std::size_t>(note));
    }
    else if (message.isAllNotesOff() || message.isAllSoundOff())
    {
        releaseHeldNotes(channel);
    }
    else if (message.isController())
    {
        (void)bridge_->enqueueMidiControlChange(
            channel, message.getControllerNumber(), message.getControllerValue(), arrival);
    }
    else if (message.isPitchWheel())
    {
        (void)bridge_->enqueueMidiPitchBend(channel, message.getPitchWheelValue(), arrival);
    }
    else if (message.isChannelPressure())
    {
        (void)bridge_->enqueueMidiChannelPressure(channel, message.getChannelPressureValue(), arrival);
    }
    else if (message.isProgramChange())
    {
        (void)bridge_->enqueueMidiProgramChange(channel, message.getProgramChangeNumber(), arrival);
    }
}

void TalasPluginProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const auto frames = buffer.getNumSamples();
    const auto inputs = kind_ == PluginKind::Effect ? getTotalNumInputChannels() : 0;
    const auto outputs = getTotalNumOutputChannels();
    if (frames <= 0 || outputs < 2 || buffer.getNumChannels() < 2)
    {
        buffer.clear();
        return;
    }
    if (!engineReady_.load(std::memory_order_acquire))
    {
        // An effect whose project does not play lets its input through; an
        // instrument is silent.
        if (inputs == 1)
            buffer.copyFrom(1, 0, buffer, 0, 0, frames);
        else if (inputs == 0)
            buffer.clear();
        return;
    }
    sendParameterChanges();

    auto midiEvent = midi.cbegin();
    const auto midiEnd = midi.cend();
    for (int start = 0; start < frames;)
    {
        const auto part = std::min(frames - start, static_cast<int>(preparedBlockFrames_));
        const float* inputPointers[2] = {nullptr, nullptr};
        std::size_t inputCount = 0;
        if (inputs > 0)
        {
            // The input is copied out first: the host's buffer is also the
            // output the engine writes.
            const auto* left = buffer.getReadPointer(0, start);
            const auto* right = inputs > 1 ? buffer.getReadPointer(1, start) : left;
            auto* first = inputScratch_[0].data();
            auto* second = inputScratch_[1].data();
            if (monoSumInput_ && inputs > 1)
            {
                for (int frame = 0; frame < part; ++frame)
                    first[frame] = 0.5F * (left[frame] + right[frame]);
            }
            else
            {
                std::copy_n(left, part, first);
            }
            std::copy_n(right, part, second);
            inputPointers[0] = first;
            inputPointers[1] = second;
            inputCount = 2;
        }
        for (; midiEvent != midiEnd && (*midiEvent).samplePosition < start + part; ++midiEvent)
        {
            const auto metadata = *midiEvent;
            sendMidi(metadata.getMessage(), std::max(0, metadata.samplePosition - start));
        }
        const auto nextCallbackTime = callbackTimeSeconds_ + static_cast<double>(part) / sampleRate_;
        float* outputPointers[2] = {buffer.getWritePointer(0, start), buffer.getWritePointer(1, start)};
        if (!bridge_->render(
                talas::runtime::RealtimeAudioInputBlock{inputPointers, inputCount},
                nextCallbackTime,
                outputPointers,
                2,
                static_cast<std::size_t>(part)))
        {
            juce::FloatVectorOperations::clear(outputPointers[0], part);
            juce::FloatVectorOperations::clear(outputPointers[1], part);
        }
        callbackTimeSeconds_ = nextCallbackTime;
        start += part;
    }
    for (int channel = 2; channel < buffer.getNumChannels(); ++channel)
        buffer.clear(channel, 0, frames);
}

juce::AudioProcessorEditor* TalasPluginProcessor::createEditor()
{
    return new TalasPluginEditor(*this);
}

void TalasPluginProcessor::getStateInformation(juce::MemoryBlock& destination)
{
    PluginState state;
    state.pluginName = identity_.name;
    state.projectHash = project_.projectHash;
    for (std::size_t index = 0; index < specs_.size(); ++index)
        state.parameters.emplace_back(specs_[index].eventName, parameterValue(*parameters_[index], specs_[index]));
    const auto serialized = serializePluginState(state);
    destination.replaceAll(serialized.data(), serialized.size());
}

void TalasPluginProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;
    std::string problem;
    const auto state = parsePluginState(
        std::string_view{static_cast<const char*>(data), static_cast<std::size_t>(sizeInBytes)}, &problem);
    if (!state.has_value())
    {
        setStatus("The saved state was not restored: " + text(problem));
        return;
    }
    for (const auto& [name, value] : state->parameters)
    {
        const auto found = std::ranges::find(specs_, name, &PluginParameterSpec::eventName);
        if (found == specs_.end())
            continue;
        const auto index = static_cast<std::size_t>(std::distance(specs_.begin(), found));
        parameters_[index]->setValueNotifyingHost(static_cast<float>(parameterValueToNormalized(*found, value)));
    }
    if (!state->projectHash.empty() && state->projectHash != project_.projectHash)
        setStatus("This state was saved with another version of the project; its parameters were matched by name");
}
}

// JUCE's plugin entry point.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new talas::plugin::TalasPluginProcessor();
}
