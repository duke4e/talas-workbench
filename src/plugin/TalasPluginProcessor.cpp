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

static_assert(std::atomic<double>::is_always_lock_free, "the host's tempo reaches the worker without a lock");

[[nodiscard]] std::uint32_t packedMeter(HostMeter meter) noexcept
{
    return (static_cast<std::uint32_t>(meter.numerator) << 8U) | meter.denominator;
}

[[nodiscard]] HostMeter unpackedMeter(std::uint32_t packed) noexcept
{
    return {static_cast<std::uint8_t>(packed >> 8U), static_cast<std::uint8_t>(packed & 0xffU)};
}
}

// ADR-0030: polls what the audio thread publishes about the host's tempo,
// and stages the project again when it should follow it. Staging can take a
// while, so it runs here rather than on the host's message thread.
class TalasPluginProcessor::HostTempoWorker final : public juce::Thread
{
public:
    explicit HostTempoWorker(TalasPluginProcessor& processor)
        : juce::Thread("Talas host tempo", 8U * 1024U * 1024U),
          processor_(processor)
    {
    }

    void run() override
    {
        while (!threadShouldExit())
        {
            processor_.followHostTempo();
            wait(50);
        }
    }

private:
    TalasPluginProcessor& processor_;
};

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
        playsATimeline_ = projectPlaysATimeline(*project_.document);
        readsTheMusicalMap_ = projectReadsTheMusicalMap(*project_.document, registry_);
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
    stopHostTempoWorker();
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
    juce::String status;
    {
        const std::scoped_lock lock{statusMutex_};
        status = status_;
    }
    // ADR-0030: the tempo the host gives, while the plugin follows it.
    const auto tempo = hostTempoBpm_.load(std::memory_order_relaxed);
    if (engineReady() && tempo > 0.0)
        status << " · host at " << juce::String(tempo, 1) << " BPM";
    return status;
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

PluginStageSettings TalasPluginProcessor::stageSettings() const
{
    PluginStageSettings settings;
    settings.sampleRate = std::round(sampleRate_);
    settings.maxBlockFrames = preparedBlockFrames_;
    for (std::size_t index = 0; index < specs_.size(); ++index)
    {
        const auto value = parameterValue(*parameters_[index], specs_[index]);
        settings.eventValues.push_back({specs_[index].eventName, value, specs_[index].domain});
    }
    return settings;
}

void TalasPluginProcessor::startHostTempoWorker()
{
    stopHostTempoWorker();
    hostTempoWorker_ = std::make_unique<HostTempoWorker>(*this);
    hostTempoWorker_->startThread();
}

void TalasPluginProcessor::stopHostTempoWorker()
{
    if (hostTempoWorker_ == nullptr)
        return;
    hostTempoWorker_->signalThreadShouldExit();
    hostTempoWorker_->notify();
    // A stage under way finishes first: it cannot be interrupted.
    (void)hostTempoWorker_->waitForThreadToExit(-1);
}

void TalasPluginProcessor::followHostTempo()
{
    const std::unique_lock lock{stageMutex_, std::try_to_lock};
    if (!lock.owns_lock() || bridge_ == nullptr || !engineReady_.load(std::memory_order_acquire))
        return;
    (void)bridge_->collectRetired();
    const auto tempo = hostTempoBpm_.load(std::memory_order_acquire);
    if (tempo <= 0.0 || hostPlaying_.load(std::memory_order_acquire))
        return;
    const auto packed = hostMeter_.load(std::memory_order_acquire);
    const auto meter = packed == 0 ? stagedMeter_ : unpackedMeter(packed);
    if (!readsTheMusicalMap_ || sameMusicalMap(tempo, meter, stagedTempoBpm_, stagedMeter_)
        || (refusedMap_.has_value() && sameMusicalMap(tempo, meter, refusedMap_->first, refusedMap_->second)))
    {
        followHostPosition();
        return;
    }
    if (!bridge_->editor().setMusicalTime(hostMusicalTime(tempo, meter)).ok())
    {
        refusedMap_ = std::pair{tempo, meter};
        return;
    }
    auto settings = stageSettings();
    settings.resources = resources_;
    const auto staged = stagePluginProject(*bridge_, registry_, project_.projectFile, settings);
    if (!staged.ok())
    {
        // A stage the engine has not taken up yet is tried again; another
        // failure is not.
        if (staged.error.find("not ready for another prepared graph") == std::string::npos)
        {
            refusedMap_ = std::pair{tempo, meter};
            setStatus("The project could not follow the host's tempo: " + text(staged.error));
        }
        return;
    }
    stagedTempoBpm_ = tempo;
    stagedMeter_ = meter;
}

// Under stageMutex_. A Timeline's transport left where the host stopped is
// where Play continues; after a locate while the host is stopped, it is
// moved there by a relocation prepared here, which rebuilds the clips under
// way at that point, so that Play sounds them at once. (A move the audio
// thread makes at Play cannot rebuild them: a clip already under way stays
// silent until the next one starts.)
void TalasPluginProcessor::followHostPosition()
{
    if (!playsATimeline_ || hostTempoBpm_.load(std::memory_order_acquire) <= 0.0
        || hostPlaying_.load(std::memory_order_acquire)
        || bridge_->activeStageId() != bridge_->latestPreparedStageId())
    {
        return;
    }
    const auto rate = static_cast<std::uint32_t>(std::round(sampleRate_));
    const auto target = talas::project::musicalTickToSampleFrame(bridge_->editor().document().timeline.musicalTime,
        musicalTickForQuarters(hostQuarterPosition_.load(std::memory_order_acquire)), rate);
    if (!target.has_value())
        return;
    const auto current = bridge_->musicalTransportSnapshot().transport.absoluteFrame;
    const auto tolerance = static_cast<std::uint64_t>(sampleRate_ * 0.002);
    if ((*target > current ? *target - current : current - *target) > tolerance)
        (void)bridge_->requestTransportRelocation(*target);
}

void TalasPluginProcessor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock)
{
    stopHostTempoWorker();
    const std::scoped_lock stageLock{stageMutex_};
    stopEngine();
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    preparedBlockFrames_ = pluginPreparedBlockFrames(maximumExpectedSamplesPerBlock);
    inputScratch_.assign(2, std::vector<float>(preparedBlockFrames_, 0.0F));
    eventIds_.assign(specs_.size(), std::nullopt);
    sentValues_.assign(specs_.size(), std::numeric_limits<double>::quiet_NaN());
    for (auto& channel : heldNotes_)
        channel.reset();
    callbackTimeSeconds_ = 1.0;
    follower_.reset();
    lastActiveStageId_ = 0;
    hostPlaying_.store(false, std::memory_order_release);
    refusedMap_.reset();
    setLatencySamples(0);
    if (!project_.ok())
        return;

    // The Sound starts from the parameters' values, which win over the
    // values its controls saved (those it fixes when it starts take them
    // too), and the first block sends every value again. A project that
    // follows the host's tempo map starts from the host's last one.
    auto settings = stageSettings();
    auto bridge = std::make_unique<talas::runtime::ProjectRuntimeBridge>(*project_.document, registry_);
    stagedTempoBpm_ = musicalTimeStartTempo(project_.document->timeline.musicalTime);
    stagedMeter_ = musicalTimeStartMeter(project_.document->timeline.musicalTime);
    const auto lastHostTempo = hostTempoBpm_.load(std::memory_order_acquire);
    const auto lastPackedMeter = hostMeter_.load(std::memory_order_acquire);
    const auto lastHostMeter = lastPackedMeter == 0 ? musicalTimeStartMeter(project_.document->timeline.musicalTime)
                                                    : unpackedMeter(lastPackedMeter);
    if (readsTheMusicalMap_ && lastHostTempo > 0.0
        && !sameMusicalMap(lastHostTempo, lastHostMeter, stagedTempoBpm_, stagedMeter_)
        && bridge->editor().setMusicalTime(hostMusicalTime(lastHostTempo, lastHostMeter)).ok())
    {
        stagedTempoBpm_ = lastHostTempo;
        stagedMeter_ = lastHostMeter;
    }
    hostTempoBpm_.store(0.0, std::memory_order_release);
    const auto staged = stagePluginProject(*bridge, registry_, project_.projectFile, settings);
    if (!staged.ok())
    {
        bridge->clearAfterAudioStopped();
        setStatus(text(staged.error));
        return;
    }
    resources_ = staged.resources;
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
    if (readsTheMusicalMap_ || playsATimeline_)
        startHostTempoWorker();
}

void TalasPluginProcessor::releaseResources()
{
    stopHostTempoWorker();
    const std::scoped_lock stageLock{stageMutex_};
    stopEngine();
    setStatus("Stopped");
}

void TalasPluginProcessor::followHost(int frames) noexcept
{
    HostPlayheadReading reading;
    if (auto* playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
        {
            reading.hasPlayhead = true;
            if (const auto bpm = position->getBpm())
                reading.bpm = *bpm;
            if (const auto ppq = position->getPpqPosition())
                reading.ppqPosition = *ppq;
            if (const auto barStart = position->getPpqPositionOfLastBarStart())
                reading.ppqPositionOfLastBarStart = *barStart;
            if (const auto signature = position->getTimeSignature())
            {
                reading.timeSignatureNumerator = signature->numerator;
                reading.timeSignatureDenominator = signature->denominator;
            }
            reading.playing = position->getIsPlaying();
            reading.looping = position->getIsLooping();
            if (const auto loop = position->getLoopPoints())
            {
                reading.loopStartPpq = loop->ppqStart;
                reading.loopEndPpq = loop->ppqEnd;
            }
        }
    }
    const auto& block = follower_.follow(reading, static_cast<std::size_t>(std::max(frames, 0)), sampleRate_);
    hostTempoBpm_.store(block.followsHost ? block.tempoBpm : 0.0, std::memory_order_release);
    hostQuarterPosition_.store(block.quarterPosition, std::memory_order_release);
    // No time signature from the host keeps the project's meter.
    hostMeter_.store(block.meterFromHost ? packedMeter(block.meter) : 0U, std::memory_order_release);
    hostPlaying_.store(block.playing, std::memory_order_release);
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
    followHost(frames);
    // A project staged again (at the host's tempo) starts from the values
    // it was staged with: every parameter is sent again.
    if (const auto stage = bridge_->activeStageId(); stage != lastActiveStageId_)
    {
        lastActiveStageId_ = stage;
        std::fill(sentValues_.begin(), sentValues_.end(), std::numeric_limits<double>::quiet_NaN());
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
        if (follower_.block().followsHost)
            bridge_->followHostTransport(follower_.transportAt(static_cast<std::size_t>(start), playsATimeline_));
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
