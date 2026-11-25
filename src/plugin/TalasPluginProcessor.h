#pragma once

#include "plugin/TalasPluginRuntimeIdentity.h"
#include "talas/core/NodeRegistry.h"
#include "talas/plugin/PluginHostTransport.h"
#include "talas/plugin/PluginParameters.h"
#include "talas/plugin/PluginRuntime.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace talas::runtime
{
class ProjectRuntimeBridge;
}

namespace talas::plugin
{
// ADR-0029: a template plugin. It plays the project embedded in its bundle
// (or the template's default) through the Workbench's prepared realtime
// engine, the ProjectRuntimeBridge, and shows the project's Event Values to
// its host as parameters.
class TalasPluginProcessor final : public juce::AudioProcessor
{
public:
    TalasPluginProcessor();
    ~TalasPluginProcessor() override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using juce::AudioProcessor::processBlock;

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void getStateInformation(juce::MemoryBlock& destination) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // For the editor (message thread).
    [[nodiscard]] const PluginIdentity& identity() const noexcept { return identity_; }
    [[nodiscard]] PluginKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::vector<PluginParameterSpec>& parameterSpecs() const noexcept { return specs_; }
    [[nodiscard]] juce::RangedAudioParameter* parameterFor(std::size_t index) const noexcept;
    // What the plugin is doing: ready, or why the project does not play.
    [[nodiscard]] juce::String statusText() const;
    [[nodiscard]] bool engineReady() const noexcept { return engineReady_.load(std::memory_order_acquire); }

private:
    class HostTempoWorker;

    [[nodiscard]] static BusesProperties busesFor(PluginKind kind);
    void setStatus(juce::String status);
    void stopEngine();
    // ADR-0030: reads the host's playhead for a block of `frames` frames.
    void followHost(int frames) noexcept;
    // On the host-tempo worker, while the host is stopped: stages the
    // project again at the host's tempo and meter when they differ from the
    // musical map it plays and its sound depends on it, and puts a Timeline
    // where the host stands, its clips under way rebuilt, ready for Play.
    void followHostTempo();
    void followHostPosition();
    void startHostTempoWorker();
    void stopHostTempoWorker();
    [[nodiscard]] PluginStageSettings stageSettings() const;
    void sendParameterChanges() noexcept;
    void sendMidi(const juce::MidiMessage& message, int frameOffset) noexcept;
    void releaseHeldNotes(int channel) noexcept;

    const PluginKind kind_;
    const PluginIdentity identity_;
    const talas::core::NodeRegistry registry_;
    PluginProjectLoad project_;
    std::vector<PluginParameterSpec> specs_;
    std::vector<juce::RangedAudioParameter*> parameters_;
    std::array<bool, 8> inputChannels_{};
    // Project input 1 reads the host's left and right summed when the
    // project reads only its first input channel.
    bool monoSumInput_ = false;

    std::unique_ptr<talas::runtime::ProjectRuntimeBridge> bridge_;
    std::atomic<bool> engineReady_{false};
    double sampleRate_ = 48000.0;
    std::size_t preparedBlockFrames_ = 512;
    // The callback clock the bridge maps MIDI arrival times against: it
    // advances by each rendered part's length, so an event `offset` frames
    // into a part arrives at the previous part's time plus offset frames.
    double callbackTimeSeconds_ = 1.0;
    std::vector<std::vector<float>> inputScratch_;
    std::vector<std::optional<std::uint32_t>> eventIds_;
    std::vector<double> sentValues_;
    std::array<std::bitset<128>, 16> heldNotes_{};

    // ADR-0030: the host's transport. The audio thread reads the playhead;
    // the host-tempo worker reads what it publishes.
    HostTransportFollower follower_;
    bool playsATimeline_ = false;
    bool readsTheMusicalMap_ = false;
    std::uint64_t lastActiveStageId_ = 0;
    std::atomic<double> hostTempoBpm_{0.0};
    std::atomic<double> hostQuarterPosition_{0.0};
    std::atomic<std::uint32_t> hostMeter_{0};
    std::atomic<bool> hostPlaying_{false};
    // Under stageMutex_, which the audio thread never takes.
    std::mutex stageMutex_;
    double stagedTempoBpm_ = 120.0;
    HostMeter stagedMeter_;
    std::optional<std::pair<double, HostMeter>> refusedMap_;
    std::vector<talas::core::ResourceSnapshotPtr> resources_;
    std::unique_ptr<HostTempoWorker> hostTempoWorker_;

    mutable std::mutex statusMutex_;
    juce::String status_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TalasPluginProcessor)
};
}
