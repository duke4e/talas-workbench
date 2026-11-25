#include "app/workbench/WorkbenchComponentInternal.h"
#include "talas/audio/WavReader.h"

#include <fstream>
#include <iterator>

namespace talas::app::workbench
{
talas::project::ProjectParameterExpression expressionParameter(
    std::string id,
    std::string expression);

talas::core::Connection connection(
    std::string sourceNode,
    std::string sourcePort,
    std::string targetNode,
    std::string targetPort);

std::optional<std::string>
WorkbenchComponent::runDiskRecorderOutputTakeSmoke(
    const std::filesystem::path& smokeDirectory)
{
    const auto makeTextParameter = [](
        std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.text = std::move(value);
        return parameter;
    };
    const auto makeResourceParameter = [](
        std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.resource = std::move(value);
        return parameter;
    };
    const auto makeChoiceParameter = [](
        std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };
    const auto makeBooleanParameter = [](
        std::string id, bool value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.boolean = value;
        return parameter;
    };

    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "recorder",
        "talas.diskRecorder",
        {
            makeResourceParameter(
                "fileName", "Ignored/Smoke Disk Capture"),
            makeTextParameter("format", "WAV"),
            makeTextParameter("wordSize", "16"),
            makeChoiceParameter("channels", "mono"),
            expressionParameter("trigger", "1"),
            expressionParameter("captureDuration", "0 s"),
            makeBooleanParameter("gated", false),
        },
    });
    document.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {
            expressionParameter("frequency", "25 hz"),
            expressionParameter("amplitude", "0.5"),
        },
    });
    document.graph.connections.push_back(connection(
        "osc", "audioOut", "recorder", "audioIn"));

    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto previousDocumentStaged = currentDocumentStaged_;
    const auto previousTransportPlaying = transportPlaying_;
    const auto previousAnalysisWorkspace = workspaceSession_.analysis;
    const auto previousDeviceMapping = deviceMappingAttached_.load(
        std::memory_order_acquire);
    const auto previousOutputMapping = projectOutputChannelMap_[0].load(
        std::memory_order_relaxed);
    const auto previousRightOutputMapping = projectOutputChannelMap_[1].load(
        std::memory_order_relaxed);
    const auto previousTakeFormat = analysisOutputTakeFormat_;
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        if (analysisOutputTake_ != nullptr)
        {
            revokeAnalysisOutputTakeCallbackAndWait();
            analysisOutputTake_->requestCancel();
            analysisOutputTake_->join();
            analysisOutputTake_.reset();
            analysisOutputTakeDiskRecorderCapture_.reset();
        }
        const auto restored =
            projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(
            previousCleanDocument, previousProjectPath);
        documentChanged();
        workspaceSession_.analysis = previousAnalysisWorkspace;
        if (previousDocumentStaged)
        {
            talas::runtime::RenderSettings restoreSettings{8000.0, 128};
            if (!stageProject(
                    std::move(restoreSettings),
                    "Disk Recorder smoke restore")
                && error.empty())
            {
                error = "C13 Disk Recorder smoke could not restore the prior stage";
            }
            projectBridge_.setPlaying(true);
            (void) adoptPreparedOutputTakeStageForSmoke();
        }
        setPlaying(previousTransportPlaying);
        deviceMappingAttached_.store(
            previousDeviceMapping, std::memory_order_release);
        projectOutputChannelMap_[0].store(
            previousOutputMapping, std::memory_order_relaxed);
        projectOutputChannelMap_[1].store(
            previousRightOutputMapping, std::memory_order_relaxed);
        analysisOutputTakeFormat_ = previousTakeFormat;
        refreshAnalysisWorkspace();
        if (!restored.ok() && error.empty())
            error = "C13 Disk Recorder smoke could not restore the prior project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };

    if (!projectBridge_.editor().replaceDocument(document).ok())
        return finish("C13 Disk Recorder smoke project was rejected");
    editBoundary_.markRecovered(document, std::nullopt);
    documentChanged();
    talas::runtime::RenderSettings settings{8000.0, 128};
    if (!stageProject(std::move(settings), "Disk Recorder smoke"))
    {
        return finish(
            "C13 Disk Recorder smoke could not stage the Workbench project: "
            + statusLabel_.getText().toStdString());
    }
    projectBridge_.setPlaying(true);
    if (!adoptPreparedOutputTakeStageForSmoke())
        return finish("C13 Disk Recorder smoke could not stage and adopt the Workbench project");
    deviceMappingAttached_.store(true, std::memory_order_release);
    projectOutputChannelMap_[0].store(0, std::memory_order_relaxed);
    refreshAnalysisWorkspace();
    const auto stageStatus = projectBridge_.preparedGraphStatus();
    if (stageStatus.prepared == nullptr
        || !stageStatus.prepared->diskRecorderCapture.has_value()
        || stageStatus.prepared->diskRecorderCapture->writeSupport
            != talas::runtime::DiskRecorderWriteSupport::Writable
        || stageStatus.prepared->diskRecorderCapture->destinationNameHint
            != "Ignored/Smoke Disk Capture"
        || !analysisPanel_->outputTakeActionContainsForSmoke(
            "Record Disk Recorder..."))
    {
        return finish("C13 Workbench did not expose the exact writable Disk Recorder action and name hint");
    }

    const auto startBoundTake = [&] (
        const std::filesystem::path& destination,
        talas::audio::ExistingAudioFilePolicy policy) {
        startAnalysisOutputTakeWithProfile(
            destination,
            policy,
            8000.0,
            128,
            analysisOutputTakeDeviceEpoch_.load(
                std::memory_order_acquire),
            deviceGeneration_.load(std::memory_order_acquire),
            nullptr,
            false);
        return analysisOutputTake_ != nullptr
            && analysisOutputTakeDiskRecorderCapture_.has_value();
    };
    const auto wrongExtension =
        smokeDirectory / "workbench-disk-recorder-wrong.aiff";
    startBoundTake(
        wrongExtension,
        talas::audio::ExistingAudioFilePolicy::Reject);
    if (analysisOutputTake_ != nullptr
        || !analysisOutputTakeStatus_.contains("must use .wav")
        || std::filesystem::exists(wrongExtension))
    {
        return finish("C13 Workbench admitted a Disk Recorder destination without the .wav contract");
    }

    const auto takePath =
        smokeDirectory / "workbench-disk-recorder-take.wav";
    if (!startBoundTake(
            takePath,
            talas::audio::ExistingAudioFilePolicy::Replace))
    {
        return finish(
            "C13 Workbench Disk Recorder admission did not bind the prepared stage: "
            + analysisOutputTakeStatus_.toStdString());
    }
    std::array<float, 128> callbackOutput{};
    std::array<float*, 1> callbackChannels{callbackOutput.data()};
    const juce::AudioIODeviceCallbackContext callbackContext{};
    audioDeviceIOCallbackWithContext(
        nullptr,
        0,
        callbackChannels.data(),
        callbackChannels.size(),
        callbackOutput.size(),
        callbackContext);
    stopAnalysisOutputTake();

    const auto snapshot = analysisOutputTakeLastSnapshot_;
    const auto wave = talas::audio::readPcm16Wav(takePath);
    const auto samplesMatch = wave.ok()
        && wave.sampleRate == 8000
        && wave.channels.size() == 1
        && wave.channels.front().size() == callbackOutput.size()
        && std::ranges::all_of(
            std::views::iota(std::size_t{0}, callbackOutput.size()),
            [&](std::size_t frame) {
                return std::abs(
                           wave.channels.front()[frame]
                           - std::clamp(
                               static_cast<double>(callbackOutput[frame]),
                               -1.0,
                               1.0))
                    <= 1.0 / 32767.0 + 1.0e-9;
            });
    if (!snapshot.has_value()
        || snapshot->state
            != talas::audio::RealtimeOutputTakeState::Completed
        || snapshot->writtenFrames != callbackOutput.size()
        || !std::ranges::any_of(
            callbackOutput,
            [](float sample) {
                return std::isfinite(sample)
                    && std::abs(sample) > 1.0e-6F;
            })
        || !samplesMatch)
    {
        return finish("C13 Disk Recorder did not bind, capture, finalize, and publish the exact Workbench callback preview");
    }

    const auto readBytes = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::string{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
    };
    const auto installBoundTake = [&] (
        const std::filesystem::path& destination,
        std::string_view existingBytes) -> std::optional<std::string> {
        {
            std::ofstream output(destination, std::ios::binary);
            output.write(
                existingBytes.data(),
                static_cast<std::streamsize>(existingBytes.size()));
        }
        if (!startBoundTake(
                destination,
                talas::audio::ExistingAudioFilePolicy::Replace))
        {
            return "C13 bound lifecycle take could not start through Workbench admission: "
                + analysisOutputTakeStatus_.toStdString();
        }
        callbackOutput.fill(0.0F);
        audioDeviceIOCallbackWithContext(
            nullptr,
            0,
            callbackChannels.data(),
            callbackChannels.size(),
            callbackOutput.size(),
            callbackContext);
        return {};
    };

    const auto lifecyclePath =
        smokeDirectory / "workbench-disk-recorder-lifecycle.wav";
    constexpr std::string_view lifecycleBytes =
        "preserve lifecycle destination";
    if (auto error = installBoundTake(lifecyclePath, lifecycleBytes))
        return finish(*error);
    stopAnalysisOutputTakeForLifecycle();
    if (!analysisOutputTakeLastSnapshot_.has_value()
        || analysisOutputTakeLastSnapshot_->state
            != talas::audio::RealtimeOutputTakeState::Cancelled
        || readBytes(lifecyclePath) != lifecycleBytes
        || !analysisOutputTakeStatus_.contains(
            "lifecycle authority changed"))
    {
        return finish("C13 lifecycle revocation published instead of discarding the private Disk Recorder take");
    }

    const auto renderFailurePath =
        smokeDirectory / "workbench-disk-recorder-render-failure.wav";
    constexpr std::string_view renderFailureBytes =
        "preserve render failure destination";
    if (auto error = installBoundTake(
            renderFailurePath, renderFailureBytes))
    {
        return finish(*error);
    }
    publishAnalysisOutputTakeFromAudio(
        callbackChannels.data(), callbackOutput.size(), false);
    analysisOutputTake_->join();
    finishAnalysisOutputTakeIfReady();
    if (!analysisOutputTakeLastSnapshot_.has_value()
        || analysisOutputTakeLastSnapshot_->state
            != talas::audio::RealtimeOutputTakeState::Failed
        || !analysisOutputTakeLastSnapshot_->incomplete
        || analysisOutputTakeLastSnapshot_->diagnostic.find(
               "source render failed") == std::string::npos
        || readBytes(renderFailurePath) != renderFailureBytes)
    {
        return finish("C13 failed render was converted into publishable zero-filled Disk Recorder evidence");
    }

    // Record to disk in monaural mode keeps only the left channel, at the bit
    // resolution asked for.
    talas::project::ProjectDocument stereo;
    stereo.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {
            expressionParameter("frequency", "25 hz"),
            expressionParameter("amplitude", "0.5"),
        },
    });
    stereo.graph.nodes.push_back({"pan", "talas.pan", {expressionParameter("pan", "0.25")}});
    stereo.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    stereo.graph.connections.push_back(connection("osc", "audioOut", "pan", "audioIn"));
    stereo.graph.connections.push_back(connection("pan", "leftOut", "out", "audioIn"));
    stereo.graph.connections.push_back(connection("pan", "rightOut", "out", "rightIn"));
    if (!projectBridge_.editor().replaceDocument(stereo).ok())
        return finish("C13 Record to disk smoke project was rejected");
    editBoundary_.markRecovered(stereo, std::nullopt);
    documentChanged();
    talas::runtime::RenderSettings stereoSettings{8000.0, 128};
    if (!stageProject(std::move(stereoSettings), "Record to disk smoke"))
        return finish("C13 Record to disk smoke could not stage: " + statusLabel_.getText().toStdString());
    projectBridge_.setPlaying(true);
    if (!adoptPreparedOutputTakeStageForSmoke())
        return finish("C13 Record to disk smoke could not adopt its stage");
    projectOutputChannelMap_[1].store(1, std::memory_order_relaxed);
    if (projectBridge_.activeOutputChannelCount() != 2)
        return finish("C13 Record to disk smoke Sound is not stereo");
    analysisOutputTakeFormat_ = {true, 24};
    const auto monauralPath = smokeDirectory / "workbench-record-to-disk-monaural.wav";
    startAnalysisOutputTakeWithProfile(monauralPath, talas::audio::ExistingAudioFilePolicy::Replace, 8000.0, 128,
        analysisOutputTakeDeviceEpoch_.load(std::memory_order_acquire), deviceGeneration_.load(std::memory_order_acquire),
        nullptr, false);
    if (analysisOutputTake_ == nullptr)
        return finish("C13 Record to disk did not start a monaural take: " + analysisOutputTakeStatus_.toStdString());
    std::array<float, 128> left{};
    std::array<float, 128> right{};
    std::array<float*, 2> stereoChannels{left.data(), right.data()};
    audioDeviceIOCallbackWithContext(nullptr, 0, stereoChannels.data(), stereoChannels.size(), left.size(), callbackContext);
    // A click leaves the take recording; Shift+click stops it.
    if (stopDiskOperationOnClick({}) || analysisOutputTake_ == nullptr)
        return finish("C13 a click without Shift stopped the Record to disk take");
    if (!stopDiskOperationOnClick(juce::ModifierKeys::shiftModifier) || analysisOutputTake_ != nullptr)
        return finish("C13 Shift+click did not stop the Record to disk take");
    const auto monaural = readBytes(monauralPath);
    const auto u16 = [&](std::size_t at) {
        return static_cast<unsigned>(static_cast<unsigned char>(monaural[at]))
            | static_cast<unsigned>(static_cast<unsigned char>(monaural[at + 1])) << 8U;
    };
    const auto sample24 = [&](std::size_t frame) {
        const auto at = 44 + frame * 3;
        auto value = static_cast<std::int32_t>(static_cast<unsigned char>(monaural[at])
            | static_cast<unsigned char>(monaural[at + 1]) << 8U | static_cast<unsigned char>(monaural[at + 2]) << 16U);
        if ((value & 0x800000) != 0)
            value -= 0x1000000;
        return static_cast<double>(value) / 8388607.0;
    };
    const auto holdsLeftAlone = monaural.size() == 44 + left.size() * 3 && u16(22) == 1 && u16(34) == 24
        && std::ranges::all_of(std::views::iota(std::size_t{0}, left.size()),
            [&](std::size_t frame) { return std::abs(sample24(frame) - left[frame]) <= 2.0 / 8388607.0; })
        && std::ranges::any_of(std::views::iota(std::size_t{0}, left.size()),
            [&](std::size_t frame) { return std::abs(left[frame] - right[frame]) > 1.0e-3F; });
    if (!holdsLeftAlone)
        return finish("C13 monaural Record to disk did not keep the left channel alone at 24 bits");

    // An AIFF take, recorded as a WAV beside its file and saved as
    // AIFF when it stops.
    analysisOutputTakeFormat_ = {false, 16, talas::audio::SampleFileFormat::Aiff};
    const auto aiffPath = smokeDirectory / "workbench-record-to-disk.aif";
    startAnalysisOutputTakeWithProfile(aiffPath, talas::audio::ExistingAudioFilePolicy::Reject, 8000.0, 128,
        analysisOutputTakeDeviceEpoch_.load(std::memory_order_acquire), deviceGeneration_.load(std::memory_order_acquire),
        nullptr, false);
    if (analysisOutputTake_ == nullptr)
        return finish("C13 Record to disk did not start an AIFF take: " + analysisOutputTakeStatus_.toStdString());
    audioDeviceIOCallbackWithContext(nullptr, 0, stereoChannels.data(), stereoChannels.size(), left.size(), callbackContext);
    stopAnalysisOutputTake();
    analysisOutputTakeFormat_ = {};
    const auto aiffBytes = readBytes(aiffPath);
    const auto aiff = talas::audio::readSampleFile(aiffPath);
    auto staging = aiffPath;
    staging += ".talas-take.wav";
    if (aiffBytes.size() < 12 || std::string(aiffBytes.begin(), aiffBytes.begin() + 4) != "FORM" || !aiff.ok()
        || aiff.channels.size() != 2 || aiff.channels[0].size() != left.size() || std::filesystem::exists(staging)
        || std::abs(aiff.channels[1][64] - right[64]) > 1.0e-3)
        return finish("C13 an AIFF Record to disk was not saved as AIFF: " + analysisOutputTakeStatus_.toStdString());

    // Two DiskRecorders within a Mixer each record
    // what they hear into the file they name, one WAV and one 24-bit AIFF,
    // while the Mixer plays both.
    const auto firstPath = smokeDirectory / "workbench-within-first.wav";
    const auto secondPath = smokeDirectory / "workbench-within-second.aif";
    const auto named = [](std::string id, const std::filesystem::path& path) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.resource = talas::project::pathToUtf8(path);
        return parameter;
    };
    const auto written = [](std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.text = std::move(value);
        return parameter;
    };
    talas::project::ProjectDocument within;
    within.graph.nodes = {
        {"low", "talas.oscillator", {expressionParameter("frequency", "25 hz"), expressionParameter("amplitude", "0.5")}},
        {"high", "talas.oscillator", {expressionParameter("frequency", "50 hz"), expressionParameter("amplitude", "0.25")}},
        {"first", "talas.diskRecorder", {named("fileName", firstPath)}},
        {"second", "talas.diskRecorder", {named("fileName", secondPath), written("format", "AIFF"), written("wordSize", "24")}},
        {"mix", "talas.mixerV2", {}},
        {"out", "talas.audioOutput", {}},
    };
    within.graph.connections = {connection("low", "audioOut", "first", "audioIn"),
        connection("high", "audioOut", "second", "audioIn"), connection("first", "audioOut", "mix", "inputs"),
        connection("second", "audioOut", "mix", "inputs"), connection("mix", "leftOut", "out", "audioIn")};
    if (!projectBridge_.editor().replaceDocument(within).ok())
        return finish("C13 DiskRecorders within a Sound smoke project was rejected");
    editBoundary_.markRecovered(within, std::nullopt);
    documentChanged();
    if (!stageProject(talas::runtime::RenderSettings{8000.0, 128}, "DiskRecorders within a Sound smoke"))
        return finish("C13 DiskRecorders within a Sound could not stage: " + statusLabel_.getText().toStdString());
    projectBridge_.setPlaying(true);
    if (!adoptPreparedOutputTakeStageForSmoke()
        || projectBridge_.preparedGraphStatus().prepared->diskRecorderTaps.size() != 2)
        return finish("C13 DiskRecorders within a Sound were not prepared");
    diskRecorderTapDestinations_ = {firstPath, secondPath};
    startAnalysisOutputTakeWithProfile(firstPath, talas::audio::ExistingAudioFilePolicy::Replace, 8000.0, 128,
        analysisOutputTakeDeviceEpoch_.load(std::memory_order_acquire), deviceGeneration_.load(std::memory_order_acquire),
        nullptr, false);
    if (analysisOutputTake_ == nullptr || diskRecorderTapTakes_.size() != 1)
        return finish("C13 DiskRecorders within a Sound did not start: " + analysisOutputTakeStatus_.toStdString());
    std::array<float, 128> mixed{};
    std::array<float*, 1> mixedChannels{mixed.data()};
    audioDeviceIOCallbackWithContext(nullptr, 0, mixedChannels.data(), mixedChannels.size(), mixed.size(), callbackContext);
    stopAnalysisOutputTake();
    const auto first = talas::audio::readSampleFile(firstPath);
    const auto second = talas::audio::readSampleFile(secondPath);
    if (!first.ok() || !second.ok() || first.channels.size() != 1 || second.channels.size() != 1
        || first.channels[0].size() != mixed.size() || second.channels[0].size() != mixed.size()
        || readBytes(secondPath).substr(0, 4) != "FORM"
        || !std::ranges::all_of(std::views::iota(std::size_t{0}, mixed.size()), [&](std::size_t frame) {
               return std::abs(first.channels[0][frame] + second.channels[0][frame] - mixed[frame]) <= 1.0e-3;
           })
        || std::ranges::all_of(first.channels[0], [](double sample) { return sample == 0.0; })
        || std::ranges::all_of(second.channels[0], [](double sample) { return sample == 0.0; }))
    {
        return finish("C13 DiskRecorders within a Sound did not each record what they hear: "
            + analysisOutputTakeStatus_.toStdString());
    }
    return finish();
}
}
