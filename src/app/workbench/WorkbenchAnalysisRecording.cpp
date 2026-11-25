#include "app/workbench/WorkbenchComponentInternal.h"
#include "talas/app/TalasFiles.h"
#include "talas/audio/ProjectAssetLoader.h"

#include <fstream>
#include <iterator>

namespace talas::app::workbench
{
namespace
{
juce::String diskRecorderSuggestedFileName(std::string_view hint);

// The sample file format a DiskRecorder's Format names, SDI, SDII, AIFF,
// IRCAM or WAVE; WAVE for an empty Format.
talas::audio::SampleFileFormat diskRecorderFileFormat(std::string_view format)
{
    return talas::audio::sampleFileFormatNamed(format).value_or(talas::audio::SampleFileFormat::Wave);
}

// The file dialog's wildcard for a take in the format.
juce::String sampleFileFormatWildcard(talas::audio::SampleFileFormat format)
{
    return format == talas::audio::SampleFileFormat::Aiff
        ? juce::String{"*.aif;*.aiff"}
        : "*" + juceString(talas::audio::sampleFileFormatExtension(format));
}

juce::String diskRecorderDestinationMustUse(talas::audio::SampleFileFormat format)
{
    return "Disk Recorder destination must use " + juceString(talas::audio::sampleFileFormatExtension(format));
}
}

std::optional<std::string>
WorkbenchComponent::runPreparedStreamDetailSmoke()
{
    talas::runtime::AudioStreamHealthSnapshot fixture;
    fixture.bufferedBlocks = 3;
    fixture.bufferCapacity = 10;
    fixture.lowWatermark = 2;
    fixture.highWatermark = 7;
    fixture.pendingRequests = 4;
    fixture.workerState = talas::runtime::AudioStreamWorkerState::Filling;
    fixture.underrunEvents = 5;
    fixture.missingFrames = 6;
    fixture.requestRejections = 7;
    fixture.responseQueueFull = 8;
    fixture.staleResponseBlocks = 9;
    fixture.decodedBlocks = 10;
    fixture.decodeFailures = 11;
    const auto detail = preparedStreamHealthDetail(fixture);
    if (detail.contains("fill 3/10")
        && detail.contains("low/high 2/7")
        && detail.contains("pending 4")
        && detail.contains("worker filling")
        && detail.contains("underrun/missing 5/6")
        && detail.contains("reject/full 7/8")
        && detail.contains("stale 9")
        && detail.contains("decoded 10")
        && detail.contains("decode errors 11")
        && diskRecorderSuggestedFileName(
               "../Protected/Recorder")
            == "Recorder.wav"
        && diskRecorderSuggestedFileName(
               "..\\Protected\\Recorder Two.aiff")
            == "Recorder Two.wav")
    {
        return {};
    }
    return "prepared stream detail omitted live health authority: "
        + detail.toStdString();
}

void WorkbenchComponent::applyDiskRecorderOutputTakeState(
    AnalysisOutputTakePanelState& state,
    const talas::runtime::PreparedGraphStatusSnapshot& stageStatus)
{
    // The terminal DiskRecorder, or the first within the Sound.
    if (stageStatus.prepared == nullptr
        || (!stageStatus.prepared->diskRecorderCapture.has_value()
            && stageStatus.prepared->diskRecorderTaps.empty()))
    {
        return;
    }
    const auto& taps = stageStatus.prepared->diskRecorderTaps;
    const auto& recorder = stageStatus.prepared->diskRecorderCapture.has_value()
        ? *stageStatus.prepared->diskRecorderCapture
        : taps.front();
    state.diskRecorder = true;
    state.diskRecorderWritable = std::ranges::all_of(taps, [](const auto& tap) {
        return tap.writeSupport == talas::runtime::DiskRecorderWriteSupport::Writable;
    }) && recorder.writeSupport == talas::runtime::DiskRecorderWriteSupport::Writable;
    state.destinationNameHint = recorder.destinationNameHint;
    const auto exactStageActive = !stageStatus.awaitingActivation
        && stageStatus.activeStageId != 0
        && stageStatus.activeStageId == stageStatus.prepared->stageId;
    state.deviceReady = state.deviceReady && exactStageActive;
    if (analysisOutputTake_ != nullptr
        || analysisOutputTakeStatus_ != "Output take idle")
    {
        return;
    }
    if (!state.diskRecorderWritable)
    {
        state.status = "Disk Recorder preview only · "
            + std::string{talas::runtime::diskRecorderWriteSupportName(
                recorder.writeSupport)}
            + " · use mono or stereo WAV/16 to enable local recording";
    }
    else if (!exactStageActive)
    {
        state.status =
            "Disk Recorder is waiting for audio to adopt the prepared stage";
    }
    else
    {
        state.status =
            "Disk Recorder ready · choose an explicit local WAV destination";
        if (!recorder.destinationNameHint.empty())
            state.status += " · name hint " + recorder.destinationNameHint;
    }
}

std::optional<std::string> WorkbenchComponent::runFileOperationSmoke(
    const std::filesystem::path& smokeDirectory,
    std::string_view oscillatorId)
{
    const auto staleOperation = fileOperationCoordinator_.begin(
        talas::app::WorkbenchOperationKind::AudioExport);
    if (!staleOperation.has_value())
        return "E9 file-operation authority fixture could not begin";
    const auto staleAuthority =
        projectBridge_.editor().documentAuthorityGeneration();
    const auto staleDocument = projectBridge_.editor().document();
    const auto moved = projectBridge_.editor().moveNode(
        oscillatorId, {111, 170});
    if (!moved.ok())
        return "E9 file-operation authority edit failed: " + moved.message;

    const auto staleExportPath =
        smokeDirectory / "workbench-stale-authority-export.wav";
    {
        std::ofstream output(staleExportPath, std::ios::binary);
        output << "preserve";
    }
    talas::audio::ProjectWavExportOptions options;
    options.outputPath = staleExportPath;
    options.sampleRate = 8000.0;
    options.durationSeconds = 0.01;
    options.maxBlockFrames = 64;
    options.shouldCancel = [this,
                            operation = *staleOperation,
                            staleAuthority] {
        return fileOperationShouldCancel(operation, staleAuthority);
    };
    options.admitPublication = [this,
                                operation = *staleOperation,
                                staleAuthority] {
        return admitFileOperationPublication(operation, staleAuthority);
    };
    const auto staleExport = talas::audio::exportProjectToPcm16Wav(
        staleDocument,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(options));
    std::ifstream preserved(staleExportPath, std::ios::binary);
    const std::string preservedBytes{
        std::istreambuf_iterator<char>{preserved},
        std::istreambuf_iterator<char>{}};
    if (!staleExport.cancelled()
        || preservedBytes != "preserve"
        || !fileOperationShouldCancel(*staleOperation, staleAuthority)
        || admitFileOperationPublication(*staleOperation, staleAuthority))
    {
        return "E9 Workbench file-operation callbacks admitted or replaced a destination after the ProjectEditor mutation boundary";
    }
    (void) fileOperationCoordinator_.finish(
        *staleOperation,
        talas::app::WorkbenchOperationOutcome::Cancelled);
    documentChanged();

    const auto publishingOperation = fileOperationCoordinator_.begin(
        talas::app::WorkbenchOperationKind::AudioExport);
    if (!publishingOperation.has_value()
        || !admitFileOperationPublication(
            *publishingOperation,
            projectBridge_.editor().documentAuthorityGeneration()))
    {
        return "E9 publication-state command fixture could not begin";
    }
    exportRunning_ = true;
    fileOperationToken_ = *publishingOperation;
    exportThread_ = std::thread([] {});
    juce::ApplicationCommandInfo command(commandExportAudio);
    getCommandInfo(commandExportAudio, command);
    cancelAudioExport();
    exportThread_.join();
    exportRunning_ = false;
    fileOperationToken_.reset();
    const auto truthful =
        (command.flags & juce::ApplicationCommandInfo::isDisabled) != 0
        && command.shortName.contains("Finishing")
        && statusLabel_.getText().contains("can no longer be cancelled");
    (void) fileOperationCoordinator_.finish(
        *publishingOperation,
        talas::app::WorkbenchOperationOutcome::Cancelled);
    if (!truthful)
        return "E9 Workbench command/status claimed cancellation after publication became irrevocable";
    return {};
}

namespace
{
juce::String diskRecorderSuggestedFileName(std::string_view hint)
{
    auto normalized = juceString(hint).replaceCharacter('\\', '/');
    auto leaf = normalized.fromLastOccurrenceOf("/", false, false).trim();
    leaf = juce::File::createLegalFileName(leaf);
    if (leaf.isEmpty())
        leaf = "Talas-Disk-Recorder";
    if (!leaf.endsWithIgnoreCase(".wav"))
    {
        const auto extension = leaf.lastIndexOfChar('.');
        if (extension > 0)
            leaf = leaf.substring(0, extension);
        leaf += ".wav";
    }
    return leaf;
}

juce::String outputTakeTerminalStatus(
    const talas::audio::RealtimeOutputTakeSnapshot& snapshot)
{
    const auto fileName = jucePathString(
        snapshot.destination.filename());
    switch (snapshot.state)
    {
    case talas::audio::RealtimeOutputTakeState::Completed:
    {
        auto status = "Output take saved: " + fileName + juceString(" · ")
            + juce::String(static_cast<juce::int64>(snapshot.writtenFrames))
            + juceString(" frames · ")
            + juce::String(static_cast<int>(snapshot.channelCount))
            + juceString(" ch · ")
            + juce::String(snapshot.sampleRate, 0) + " Hz";
        if (snapshot.incomplete)
        {
            status += juceString(" · incomplete (");
            if (snapshot.droppedBlocks != 0)
            {
                status += juce::String(
                              static_cast<juce::int64>(
                                  snapshot.droppedBlocks))
                    + (snapshot.droppedBlocks == 1
                        ? " dropped block"
                        : " dropped blocks");
            }
            if (snapshot.rejectedBlocks != 0)
            {
                if (snapshot.droppedBlocks != 0)
                    status += ", ";
                status += juce::String(
                              static_cast<juce::int64>(
                                  snapshot.rejectedBlocks))
                    + (snapshot.rejectedBlocks == 1
                        ? " rejected block"
                        : " rejected blocks");
            }
            status += ")";
        }
        if (!snapshot.diagnostic.empty())
            status += juceString(" · ") + juceString(snapshot.diagnostic);
        return status;
    }
    case talas::audio::RealtimeOutputTakeState::Cancelled:
        return snapshot.diagnostic.empty()
            ? juce::String{"Output take cancelled"}
            : juceString(snapshot.diagnostic);
    case talas::audio::RealtimeOutputTakeState::Failed:
        if (snapshot.incomplete && !snapshot.diagnostic.empty())
            return juceString(snapshot.diagnostic);
        return "Output take failed: "
            + (snapshot.diagnostic.empty()
                ? juce::String{"unknown writer failure"}
                : juceString(snapshot.diagnostic));
    case talas::audio::RealtimeOutputTakeState::Recording:
    case talas::audio::RealtimeOutputTakeState::Stopping:
        break;
    }
    return "Output take unavailable";
}
}

void WorkbenchComponent::chooseAnalysisOutputTakeDestination()
{
    finishAnalysisOutputTakeIfReady();
    if (analysisOutputTake_ != nullptr)
    {
        setStatus("Stop or cancel the current output take first");
        return;
    }
    if (fileChooserActive_)
    {
        setStatus("A file dialog is already open");
        return;
    }

    const auto stageStatus = projectBridge_.preparedGraphStatus();
    const auto recorderCapture = stageStatus.prepared == nullptr
        ? std::optional<talas::runtime::PreparedDiskRecorderCaptureSummary>{}
        : stageStatus.prepared->diskRecorderCapture;
    if (recorderCapture.has_value()
        && recorderCapture->writeSupport
            != talas::runtime::DiskRecorderWriteSupport::Writable)
    {
        analysisOutputTakeStatus_ =
            "Disk Recorder preview is not writable: use mono or stereo WAVE, AIFF, IRCAM or SDII, or mono SDI, at 8, 16 or 24 bits";
        setStatus(analysisOutputTakeStatus_);
        refreshAnalysisWorkspace();
        return;
    }

    diskRecorderTapDestinations_.clear();
    if (recorderCapture.has_value())
    {
        // A DiskRecorder records into the file its
        // FileName names, without asking.
        if (!recorderCapture->destinationNameHint.empty())
        {
            juce::String error;
            const auto destination = diskRecorderDestination(*recorderCapture, error);
            if (!destination.has_value())
            {
                analysisOutputTakeStatus_ = error;
                setStatus(analysisOutputTakeStatus_);
                refreshAnalysisWorkspace();
                return;
            }
            std::error_code ignored;
            countOffThenRecord(*destination,
                std::filesystem::exists(*destination, ignored) ? talas::audio::ExistingAudioFilePolicy::Replace
                                                               : talas::audio::ExistingAudioFilePolicy::Reject);
            return;
        }
        chooseAnalysisOutputTakeFile();
        return;
    }
    // Each DiskRecorder within the Sound records into the file its
    // FileName names, all at once.
    if (stageStatus.prepared != nullptr && !stageStatus.prepared->diskRecorderTaps.empty())
    {
        const auto& taps = stageStatus.prepared->diskRecorderTaps;
        std::vector<std::filesystem::path> destinations;
        juce::String error;
        for (const auto& tap : taps)
        {
            if (tap.writeSupport != talas::runtime::DiskRecorderWriteSupport::Writable)
                error = "Disk Recorder preview is not writable: use mono or stereo WAVE, AIFF, IRCAM or SDII, or mono SDI, at 8, 16 or 24 bits";
            else if (tap.destinationNameHint.empty())
                error = "Name the file each DiskRecorder within the Sound records into";
            else if (taps.size() > diskRecorderTapTakeCallbacks_.size() + 1)
                error = "At most nine DiskRecorders within a Sound can record at once";
            else if (auto destination = diskRecorderDestination(tap, error))
            {
                if (std::ranges::find(destinations, *destination) != destinations.end())
                    error = "Two DiskRecorders within the Sound name the same file";
                else
                    destinations.push_back(std::move(*destination));
            }
            if (error.isNotEmpty())
            {
                analysisOutputTakeStatus_ = error;
                setStatus(analysisOutputTakeStatus_);
                refreshAnalysisWorkspace();
                return;
            }
        }
        diskRecorderTapDestinations_ = destinations;
        std::error_code ignored;
        countOffThenRecord(destinations.front(),
            std::filesystem::exists(destinations.front(), ignored) ? talas::audio::ExistingAudioFilePolicy::Replace
                                                                   : talas::audio::ExistingAudioFilePolicy::Reject);
        return;
    }
    // Record to disk first asks for the file format, stereo or monaural and
    // the bit resolution; a Disk Recorder names its
    // own.
    // Any of the sample file formats Talas writes, SD-II on a Macintosh
    // only.
    std::vector<std::string> formats{"WAV", "AIFF", "IRCAM", "SDI"};
    if (talas::audio::soundDesignerIIAvailable())
        formats.emplace_back("SDII");
    chooseInput("Record in which file format?", std::move(formats), [this](std::string format) {
        const auto file = diskRecorderFileFormat(format);
        const auto chooseBits = [this, file](bool monaural) {
            chooseInput("Record with which bit resolution?", {"8 bits", "16 bits", "24 bits"},
                [this, file, monaural](std::string bits) {
                    analysisOutputTakeFormat_ = {
                        monaural, static_cast<std::uint16_t>(bits == "8 bits" ? 8 : bits == "24 bits" ? 24 : 16), file};
                    chooseAnalysisOutputTakeFile();
                });
        };
        // A Sound Designer I file holds one channel.
        if (file == talas::audio::SampleFileFormat::SoundDesignerI)
        {
            chooseBits(true);
            return;
        }
        chooseInput("Record in stereo or monaural?", {"Stereo", "Monaural"},
            [chooseBits](std::string channels) { chooseBits(channels == "Monaural"); });
    });
}

void WorkbenchComponent::chooseAnalysisOutputTakeFile()
{
    if (fileChooserActive_)
    {
        setStatus("A file dialog is already open");
        return;
    }
    const auto stageStatus = projectBridge_.preparedGraphStatus();
    const auto recorderCapture = stageStatus.prepared == nullptr
        ? std::optional<talas::runtime::PreparedDiskRecorderCaptureSummary>{}
        : stageStatus.prepared->diskRecorderCapture;
    auto initialFile = juce::File::getSpecialLocation(
        juce::File::userDocumentsDirectory)
                           .getChildFile(
                               recorderCapture.has_value()
                                   ? diskRecorderSuggestedFileName(
                                       recorderCapture
                                           ->destinationNameHint)
                                   : juce::String{
                                       "Talas-Take.wav"});
    if (!recorderCapture.has_value() && projectPath().has_value())
    {
        const auto projectFile = juce::File(
            jucePathString(*projectPath()));
        initialFile = projectFile.getSiblingFile(
            projectFile.getFileNameWithoutExtension()
            + "-Take.wav");
    }
    const auto file = recorderCapture.has_value() ? diskRecorderFileFormat(recorderCapture->format)
                                                  : analysisOutputTakeFormat_.file;
    initialFile = initialFile.withFileExtension(juceString(talas::audio::sampleFileFormatExtension(file)));
    fileChooser_ = std::make_unique<juce::FileChooser>(
        recorderCapture.has_value()
            ? "Record Disk Recorder"
            : "Record Output Take",
        initialFile,
        sampleFileFormatWildcard(file));
    fileChooserActive_ = true;
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    const auto diskRecorderSelection = recorderCapture.has_value();
    fileChooser_->launchAsync(
        juce::FileBrowserComponent::saveMode
            | juce::FileBrowserComponent::canSelectFiles,
        [safeThis, diskRecorderSelection, file](const juce::FileChooser& chooser) {
            if (safeThis == nullptr)
                return;
            safeThis->fileChooserActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            auto selected = chooser.getResult();
            if (selected == juce::File{})
            {
                safeThis->analysisOutputTakeStatus_ =
                    "Output take selection cancelled";
                safeThis->refreshAnalysisWorkspace();
                return;
            }
            if (selected.getFileExtension().isEmpty())
                selected = selected.withFileExtension(juceString(talas::audio::sampleFileFormatExtension(file)));
            if (diskRecorderSelection
                && !talas::audio::sampleFileHasFormatExtension(filesystemPath(selected.getFullPathName()), file))
            {
                safeThis->analysisOutputTakeStatus_ = diskRecorderDestinationMustUse(file);
                safeThis->setStatus(
                    safeThis->analysisOutputTakeStatus_);
                safeThis->refreshAnalysisWorkspace();
                return;
            }
            const auto existingPolicy = selected.existsAsFile()
                ? talas::audio::ExistingAudioFilePolicy::Replace
                : talas::audio::ExistingAudioFilePolicy::Reject;
            const auto destination = filesystemPath(
                selected.getFullPathName());
            safeThis->scheduleContinuation(
                [safeThis, destination, existingPolicy] {
                    if (safeThis != nullptr)
                    {
                        safeThis->countOffThenRecord(
                            destination,
                            existingPolicy);
                    }
                });
        });
}

// Record to disk's count-off: a Sound with live
// input asks for the beats and tempo of a count-off, beeps on each beat and
// begins recording on the beat after; zero beats records at once.
void WorkbenchComponent::countOffThenRecord(
    std::filesystem::path destination,
    talas::audio::ExistingAudioFilePolicy existingPolicy)
{
    if (!talas::app::takesLiveInput(talas::app::loadedSoundInputs(projectBridge_.editor().document())))
    {
        startAnalysisOutputTake(std::move(destination), existingPolicy);
        return;
    }
    chooseInput("Count off how many beats?", {"0 beats", "1 beat", "2 beats", "3 beats", "4 beats", "8 beats"},
        [this, destination, existingPolicy](std::string beats) {
            chooseInput("At what tempo?", {"60 BPM", "90 BPM", "120 BPM", "144 BPM"},
                [this, destination, existingPolicy, beats](std::string tempo) {
                    const auto countOff = talas::app::recordingCountOff(
                        static_cast<unsigned>(std::stoul(beats)), std::stod(tempo));
                    if (countOff.beepSeconds.empty())
                    {
                        startAnalysisOutputTake(destination, existingPolicy);
                        return;
                    }
                    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
                    const auto total = countOff.beepSeconds.size();
                    for (std::size_t beat = 0; beat < total; ++beat)
                    {
                        juce::Timer::callAfterDelay(juce::roundToInt(countOff.beepSeconds[beat] * 1000.0),
                            [safeThis, beat, total] {
                                if (safeThis == nullptr)
                                    return;
                                juce::LookAndFeel::getDefaultLookAndFeel().playAlertSound();
                                safeThis->setStatus("Count-off " + juce::String(static_cast<int>(beat) + 1) + " of "
                                    + juce::String(static_cast<int>(total)));
                            });
                    }
                    juce::Timer::callAfterDelay(juce::roundToInt(countOff.recordingStartSeconds * 1000.0),
                        [safeThis, destination, existingPolicy] {
                            if (safeThis != nullptr)
                                safeThis->startAnalysisOutputTake(destination, existingPolicy);
                        });
                });
        });
}

void WorkbenchComponent::startAnalysisOutputTake(
    std::filesystem::path destination,
    talas::audio::ExistingAudioFilePolicy existingPolicy)
{
    auto* device = deviceManager_.getCurrentAudioDevice();
    startAnalysisOutputTakeWithProfile(
        std::move(destination),
        existingPolicy,
        device == nullptr ? 0.0 : device->getCurrentSampleRate(),
        device == nullptr || device->getCurrentBufferSizeSamples() <= 0
            ? 0
            : static_cast<std::size_t>(
                device->getCurrentBufferSizeSamples()),
        analysisOutputTakeDeviceEpoch_.load(std::memory_order_acquire),
        deviceGeneration_.load(std::memory_order_acquire),
        device,
        true);
}

void WorkbenchComponent::startAnalysisOutputTakeWithProfile(
    std::filesystem::path destination,
    talas::audio::ExistingAudioFilePolicy existingPolicy,
    double sampleRate,
    std::size_t maxBlockFrames,
    std::uint64_t sourceDeviceEpoch,
    std::uint64_t sourceDeviceGeneration,
    juce::AudioIODevice* sourceDevice,
    bool requireCurrentDevice)
{
    finishAnalysisOutputTakeIfReady();
    if (analysisOutputTake_ != nullptr)
    {
        setStatus("An output take is already active");
        return;
    }
    if (analysisOutputTakeDestinationConflicts(destination))
    {
        analysisOutputTakeStatus_ =
            "Output take cannot replace the open project, its package, or a source asset";
        setStatus(analysisOutputTakeStatus_);
        refreshAnalysisWorkspace();
        return;
    }
    const auto sourceAuthorityGeneration =
        analysisOutputTakeAuthorityGeneration_.load(
            std::memory_order_acquire);
    const auto sourceDocumentAuthorityGeneration =
        projectBridge_.editor().documentAuthorityGeneration();
    const auto stageStatus = projectBridge_.preparedGraphStatus();
    if (stageStatus.prepared == nullptr
        || stageStatus.awaitingActivation
        || stageStatus.activeStageId == 0
        || stageStatus.prepared->stageId != stageStatus.activeStageId)
    {
        analysisOutputTakeStatus_ =
            "Output take requires the prepared project stage to be adopted by audio";
        setStatus(analysisOutputTakeStatus_);
        refreshAnalysisWorkspace();
        return;
    }
    // The DiskRecorders within the Sound record, the first into
    // this take, when the Sound has no terminal one.
    const auto& sourceTaps = stageStatus.prepared->diskRecorderTaps;
    const auto fromTaps = !stageStatus.prepared->diskRecorderCapture.has_value() && !sourceTaps.empty()
        && diskRecorderTapDestinations_.size() == sourceTaps.size()
        && diskRecorderTapDestinations_.front() == destination;
    if (!stageStatus.prepared->diskRecorderCapture.has_value() && !sourceTaps.empty() && !fromTaps)
    {
        analysisOutputTakeStatus_ = "The DiskRecorders within the Sound record into the files they name";
        setStatus(analysisOutputTakeStatus_);
        refreshAnalysisWorkspace();
        return;
    }
    const auto sourceDiskRecorderCapture = fromTaps
        ? std::optional<talas::runtime::PreparedDiskRecorderCaptureSummary>{sourceTaps.front()}
        : stageStatus.prepared->diskRecorderCapture;
    if (sourceDiskRecorderCapture.has_value()
        && sourceDiskRecorderCapture->writeSupport
            != talas::runtime::DiskRecorderWriteSupport::Writable)
    {
        analysisOutputTakeStatus_ =
            "Disk Recorder preview is not writable: choose mono or stereo WAV/16";
        setStatus(analysisOutputTakeStatus_);
        refreshAnalysisWorkspace();
        return;
    }
    if (sourceDiskRecorderCapture.has_value()
        && !talas::audio::sampleFileHasFormatExtension(
            destination, diskRecorderFileFormat(sourceDiskRecorderCapture->format)))
    {
        analysisOutputTakeStatus_ =
            diskRecorderDestinationMustUse(diskRecorderFileFormat(sourceDiskRecorderCapture->format));
        setStatus(analysisOutputTakeStatus_);
        refreshAnalysisWorkspace();
        return;
    }
    if (!deviceMappingAttached_.load(std::memory_order_acquire)
        || sampleRate <= 0.0
        || maxBlockFrames == 0
        || (requireCurrentDevice && sourceDevice == nullptr)
        || !currentDocumentStaged_)
    {
        analysisOutputTakeStatus_ =
            "Output take requires one prepared project and ready audio device";
        setStatus(analysisOutputTakeStatus_);
        refreshAnalysisWorkspace();
        return;
    }
    const auto sourceStageId = stageStatus.activeStageId;
    // In monaural mode only the left channel is recorded.
    const auto channelCount = fromTaps
        ? std::size_t{sourceDiskRecorderCapture->channels == "stereo" ? 2U : 1U}
        : !sourceDiskRecorderCapture.has_value() && analysisOutputTakeFormat_.monaural
            && projectBridge_.activeOutputChannelCount() > 0
        ? std::size_t{1}
        : projectBridge_.activeOutputChannelCount();
    if (channelCount == 0
        || channelCount
            > talas::runtime::maximumRealtimeOutputRecorderChannels)
    {
        analysisOutputTakeStatus_ =
            juceString("Output take requires 1–8 active project outputs");
        setStatus(analysisOutputTakeStatus_);
        refreshAnalysisWorkspace();
        return;
    }
    // A mono recording has one output channel and a stereo one two
    //.
    if (sourceDiskRecorderCapture.has_value()
        && channelCount != (sourceDiskRecorderCapture->channels == "stereo" ? 2U : 1U))
    {
        analysisOutputTakeStatus_ =
            sourceDiskRecorderCapture->channels == "stereo"
            ? "Disk Recorder recording requires exactly two mapped stereo outputs"
            : "Disk Recorder recording requires exactly one mapped mono output";
        setStatus(analysisOutputTakeStatus_);
        refreshAnalysisWorkspace();
        return;
    }
    for (std::size_t channel = 0; !fromTaps && channel < channelCount; ++channel)
    {
        if (projectOutputChannelMap_[channel].load(
                std::memory_order_acquire)
            >= 0)
        {
            continue;
        }
        analysisOutputTakeStatus_ =
            "Output take requires every active project output to be mapped to the audio device";
        setStatus(analysisOutputTakeStatus_);
        refreshAnalysisWorkspace();
        return;
    }

    // An AIFF take, or one in another format than
    // WAV, is recorded as a WAV beside its file and saved in its format
    // once it is done.
    analysisOutputTakeConversion_.reset();
    // A DiskRecorder whose Format is AIFF, IRCAM, SDI or SDII the same way,
    // at its WordSize.
    analysisOutputTakeConversionBits_ = sourceDiskRecorderCapture.has_value()
        ? static_cast<std::uint16_t>(sourceDiskRecorderCapture->wordSize == "8" ? 8 : sourceDiskRecorderCapture->wordSize == "24" ? 24 : 16)
        : analysisOutputTakeFormat_.bits;
    analysisOutputTakeConversionFormat_ = sourceDiskRecorderCapture.has_value()
        ? diskRecorderFileFormat(sourceDiskRecorderCapture->format)
        : analysisOutputTakeFormat_.file;
    if (analysisOutputTakeConversionFormat_ != talas::audio::SampleFileFormat::Wave)
    {
        auto staging = destination;
        staging += ".talas-take.wav";
        analysisOutputTakeConversion_.emplace(destination, staging);
        destination = std::move(staging);
        existingPolicy = talas::audio::ExistingAudioFilePolicy::Replace;
    }
    std::vector<std::filesystem::path> protectedFiles;
    std::vector<std::filesystem::path> protectedDirectories;
    collectAnalysisOutputTakeProtectedPaths(
        protectedFiles, protectedDirectories);
    std::string error;
    talas::audio::RealtimeOutputTakeOptions takeOptions{
        std::move(destination),
        existingPolicy,
        sampleRate,
        channelCount,
        maxBlockFrames,
        talas::audio::defaultRealtimeOutputTakeQueueCapacity,
        sourceStageId,
        sourceAuthorityGeneration,
        sourceDocumentAuthorityGeneration,
        std::move(protectedFiles),
        std::move(protectedDirectories),
    };
    // A DiskRecorder writes the word size its WordSize asks for: 8, 16 or 24
    // bits.
    if (sourceDiskRecorderCapture.has_value()
        && (sourceDiskRecorderCapture->wordSize == "8"
            || sourceDiskRecorderCapture->wordSize == "24"))
    {
        takeOptions.wordSize.bits =
            sourceDiskRecorderCapture->wordSize == "8" ? 8 : 24;
    }
    if (!sourceDiskRecorderCapture.has_value())
        takeOptions.wordSize.bits = analysisOutputTakeFormat_.bits;
    auto candidate = talas::audio::RealtimeOutputTake::start(
        std::move(takeOptions),
        error);
    if (!candidate)
    {
        analysisOutputTakeStatus_ =
            "Output take could not start: " + juceString(error);
        setStatus(analysisOutputTakeStatus_);
        refreshAnalysisWorkspace();
        return;
    }
    const auto projectAuthorityChangedDuringPreparation =
        sourceAuthorityGeneration
                != analysisOutputTakeAuthorityGeneration_.load(
                    std::memory_order_acquire)
            || sourceDocumentAuthorityGeneration
                != projectBridge_.editor().documentAuthorityGeneration();
    if (sourceDeviceGeneration
            != deviceGeneration_.load(std::memory_order_acquire)
        || projectAuthorityChangedDuringPreparation
        || (requireCurrentDevice
            && deviceManager_.getCurrentAudioDevice() != sourceDevice))
    {
        candidate->requestCancel();
        candidate->join();
        analysisOutputTakeStatus_ = projectAuthorityChangedDuringPreparation
            ? "Output take cancelled: project authority changed during preparation"
            : "Output take cancelled: audio device changed during preparation";
        setStatus(analysisOutputTakeStatus_);
        refreshAnalysisWorkspace();
        return;
    }
    const auto confirmedStage = projectBridge_.preparedGraphStatus();
    if (confirmedStage.awaitingActivation
        || confirmedStage.activeStageId != sourceStageId
        || confirmedStage.prepared == nullptr
        || confirmedStage.prepared->stageId != sourceStageId)
    {
        candidate->requestCancel();
        candidate->join();
        analysisOutputTakeStatus_ =
            "Output take cancelled: prepared stage changed during admission";
        setStatus(analysisOutputTakeStatus_);
        refreshAnalysisWorkspace();
        return;
    }
    if ((fromTaps ? std::optional<talas::runtime::PreparedDiskRecorderCaptureSummary>{
                        confirmedStage.prepared->diskRecorderTaps.empty()
                            ? talas::runtime::PreparedDiskRecorderCaptureSummary{}
                            : confirmedStage.prepared->diskRecorderTaps.front()}
                  : confirmedStage.prepared->diskRecorderCapture)
        != sourceDiskRecorderCapture)
    {
        candidate->requestCancel();
        candidate->join();
        analysisOutputTakeStatus_ =
            "Output take cancelled: Disk Recorder identity changed during admission";
        setStatus(analysisOutputTakeStatus_);
        refreshAnalysisWorkspace();
        return;
    }

    // The other DiskRecorders within the Sound, each into its own file at
    // its own channels, word size and format.
    std::error_code ignoredError;
    for (std::size_t tap = 1; fromTaps && tap < sourceTaps.size(); ++tap)
    {
        const auto& recorder = sourceTaps[tap];
        DiskRecorderTapTake tapTake;
        auto tapDestination = diskRecorderTapDestinations_[tap];
        auto tapPolicy = std::filesystem::exists(tapDestination, ignoredError)
            ? talas::audio::ExistingAudioFilePolicy::Replace
            : talas::audio::ExistingAudioFilePolicy::Reject;
        tapTake.bits = static_cast<std::uint16_t>(recorder.wordSize == "8" ? 8 : recorder.wordSize == "24" ? 24 : 16);
        tapTake.format = diskRecorderFileFormat(recorder.format);
        if (tapTake.format != talas::audio::SampleFileFormat::Wave)
        {
            auto staging = tapDestination;
            staging += ".talas-take.wav";
            tapTake.conversion.emplace(tapDestination, staging);
            tapDestination = std::move(staging);
            tapPolicy = talas::audio::ExistingAudioFilePolicy::Replace;
        }
        std::vector<std::filesystem::path> tapProtectedFiles;
        std::vector<std::filesystem::path> tapProtectedDirectories;
        collectAnalysisOutputTakeProtectedPaths(tapProtectedFiles, tapProtectedDirectories);
        talas::audio::RealtimeOutputTakeOptions tapOptions{
            std::move(tapDestination),
            tapPolicy,
            sampleRate,
            recorder.channels == "stereo" ? 2U : 1U,
            maxBlockFrames,
            talas::audio::defaultRealtimeOutputTakeQueueCapacity,
            sourceStageId,
            sourceAuthorityGeneration,
            sourceDocumentAuthorityGeneration,
            std::move(tapProtectedFiles),
            std::move(tapProtectedDirectories),
        };
        tapOptions.wordSize.bits = tapTake.bits;
        std::string tapError;
        tapTake.take = talas::audio::RealtimeOutputTake::start(std::move(tapOptions), tapError);
        if (!tapTake.take)
        {
            candidate->requestCancel();
            candidate->join();
            for (auto& started : diskRecorderTapTakes_)
            {
                started.take->requestCancel();
                started.take->join();
            }
            diskRecorderTapTakes_.clear();
            analysisOutputTakeStatus_ = "A DiskRecorder within the Sound could not start: " + juceString(tapError);
            setStatus(analysisOutputTakeStatus_);
            refreshAnalysisWorkspace();
            return;
        }
        diskRecorderTapTakes_.push_back(std::move(tapTake));
    }
    for (std::size_t tap = 0; tap < diskRecorderTapTakes_.size(); ++tap)
        diskRecorderTapTakeCallbacks_[tap].store(diskRecorderTapTakes_[tap].take.get(), std::memory_order_release);
    analysisOutputTakeFromTaps_.store(fromTaps, std::memory_order_release);

    analysisOutputTakeLastSnapshot_.reset();
    analysisOutputTakeDiskRecorderCapture_ =
        sourceDiskRecorderCapture;
    analysisOutputTake_ = std::move(candidate);
    analysisOutputTakeCallback_.store(
        analysisOutputTake_.get(),
        std::memory_order_release);
    analysisOutputTakeLifecycle_.store(1, std::memory_order_seq_cst);
    const auto projectAuthorityChangedDuringPublication =
        sourceAuthorityGeneration
                != analysisOutputTakeAuthorityGeneration_.load(
                    std::memory_order_acquire)
            || sourceDocumentAuthorityGeneration
                != projectBridge_.editor().documentAuthorityGeneration();
    if (sourceDeviceEpoch
            != analysisOutputTakeDeviceEpoch_.load(
                std::memory_order_acquire)
        || sourceDeviceGeneration
            != deviceGeneration_.load(std::memory_order_acquire)
        || projectAuthorityChangedDuringPublication
        || (requireCurrentDevice
            && deviceManager_.getCurrentAudioDevice() != sourceDevice))
    {
        revokeAnalysisOutputTakeCallbackAndWait();
        analysisOutputTake_->requestCancel();
        analysisOutputTake_->join();
        analysisOutputTakeLastSnapshot_ =
            analysisOutputTake_->snapshot();
        analysisOutputTake_.reset();
        analysisOutputTakeDiskRecorderCapture_.reset();
        finishDiskRecorderTapTakes();
        analysisOutputTakeStatus_ = projectAuthorityChangedDuringPublication
            ? "Output take cancelled: project authority changed during publication"
            : "Output take cancelled: audio device changed during publication";
        setStatus(analysisOutputTakeStatus_);
        refreshAnalysisWorkspace();
        return;
    }
    if (projectBridge_.activeStageId() != sourceStageId)
    {
        revokeAnalysisOutputTakeCallbackAndWait();
        analysisOutputTake_->requestCancel();
        analysisOutputTake_->join();
        analysisOutputTakeLastSnapshot_ =
            analysisOutputTake_->snapshot();
        analysisOutputTake_.reset();
        analysisOutputTakeDiskRecorderCapture_.reset();
        finishDiskRecorderTapTakes();
        analysisOutputTakeStatus_ =
            "Output take cancelled: active render stage changed during publication";
        setStatus(analysisOutputTakeStatus_);
        refreshAnalysisWorkspace();
        return;
    }
    const auto snapshot = analysisOutputTake_->snapshot();
    analysisOutputTakeStatus_ =
        (analysisOutputTakeDiskRecorderCapture_.has_value()
                ? juceString("Recording Disk Recorder take · ")
                : juceString("Recording output take · "))
        + jucePathString(snapshot.destination.filename()) + juceString(" · ")
        + juce::String(static_cast<int>(snapshot.channelCount))
        + juceString(" ch · ")
        + juce::String(snapshot.sampleRate, 0) + " Hz";
    setStatus(analysisOutputTakeStatus_);
    refreshAnalysisWorkspace();
}

bool WorkbenchComponent::analysisOutputTakeDestinationConflicts(
    const std::filesystem::path& destination) const noexcept
{
    try
    {
        std::vector<std::filesystem::path> protectedFiles;
        std::vector<std::filesystem::path> protectedDirectories;
        collectAnalysisOutputTakeProtectedPaths(
            protectedFiles, protectedDirectories);
        return talas::audio::audioFileDestinationConflicts(
            destination,
            protectedFiles,
            protectedDirectories);
    }
    catch (...)
    {
        return true;
    }
}

void WorkbenchComponent::collectAnalysisOutputTakeProtectedPaths(
    std::vector<std::filesystem::path>& protectedFiles,
    std::vector<std::filesystem::path>& protectedDirectories) const
{
    if (projectPath().has_value())
    {
        protectedFiles.push_back(*projectPath());
        const auto packageDisplay =
            talas::project::projectPackageDisplayPath(*projectPath());
        if (packageDisplay != *projectPath())
            protectedDirectories.push_back(packageDisplay);
    }
    for (const auto& asset : projectBridge_.editor().document().assets)
    {
        const auto resolved = talas::project::resolveProjectAssetPath(
            asset,
            projectPath());
        if (!resolved.empty())
            protectedFiles.push_back(resolved);
    }
}

void WorkbenchComponent::stopAnalysisOutputTake()
{
    revokeAnalysisOutputTakeCallbackAndWait();
    if (analysisOutputTake_ == nullptr)
        return;
    const auto destination = analysisOutputTake_->snapshot().destination;
    const auto destinationConflict =
        analysisOutputTakeDestinationConflicts(destination);
    if (destinationConflict)
        analysisOutputTake_->requestCancel();
    else
        analysisOutputTake_->requestStop();
    analysisOutputTakeStatus_ = destinationConflict
        ? "Cancelling output take: destination became a protected project path..."
        : analysisOutputTakeDiskRecorderCapture_.has_value()
            ? "Finalizing Disk Recorder take..."
            : "Finalizing output take...";
    setStatus(analysisOutputTakeStatus_);
    analysisOutputTake_->join();
    analysisOutputTakeLastSnapshot_ = analysisOutputTake_->snapshot();
    analysisOutputTakeStatus_ = destinationConflict
        ? juce::String{
            "Output take cancelled: destination became the open project, its package, or a source asset"}
        : outputTakeTerminalStatus(*analysisOutputTakeLastSnapshot_);
    analysisOutputTake_.reset();
    analysisOutputTakeDiskRecorderCapture_.reset();
    finishDiskRecorderTapTakes();
    saveAnalysisOutputTakeInItsFormat();
    setStatus(analysisOutputTakeStatus_);
    refreshAnalysisWorkspace();
}

// Shift+click stops the take recording to disk, or the
// Sound playing from the disk; any other click is left alone.
bool WorkbenchComponent::stopDiskOperationOnClick(juce::ModifierKeys modifiers)
{
    const auto recording = analysisOutputTake_ != nullptr;
    const auto diskPlaying = modifiers.isShiftDown() && transportPlaying_
        && talas::app::loadedSoundInputs(projectBridge_.editor().document()).usesDisk;
    if (!talas::app::shiftClickStopsDiskOperation(recording, diskPlaying, modifiers.isShiftDown()))
        return false;
    if (recording)
        stopAnalysisOutputTake();
    else
    {
        stopCurrentProject();
        setStatus("Shift+click stopped the Sound playing from the disk");
    }
    return true;
}

// The WAV a take in another format was recorded into saved as the file
// chosen, in its format (AIFF, IRCAM, SD-I or SD-II), or removed when the
// take did not complete.
void WorkbenchComponent::saveAnalysisOutputTakeInItsFormat()
{
    const auto conversion = std::exchange(analysisOutputTakeConversion_, std::nullopt);
    if (!conversion.has_value())
        return;
    if (!analysisOutputTakeLastSnapshot_.has_value()
        || analysisOutputTakeLastSnapshot_->state != talas::audio::RealtimeOutputTakeState::Completed)
    {
        std::error_code ignored;
        std::filesystem::remove(conversion->second, ignored);
        return;
    }
    const auto error = talas::audio::convertRecordingToSampleFile(
        conversion->second, conversion->first, analysisOutputTakeConversionFormat_, analysisOutputTakeConversionBits_);
    analysisOutputTakeStatus_ = error.empty()
        ? analysisOutputTakeStatus_ + " · saved as " + jucePathString(conversion->first.filename())
        : "The take could not be saved as "
            + juceString(talas::audio::sampleFileFormatName(analysisOutputTakeConversionFormat_)) + ": "
            + juceString(error);
}

void WorkbenchComponent::cancelAnalysisOutputTake()
{
    revokeAnalysisOutputTakeCallbackAndWait();
    if (analysisOutputTake_ == nullptr)
        return;
    analysisOutputTake_->requestCancel();
    analysisOutputTakeStatus_ = "Cancelling output take...";
    setStatus(analysisOutputTakeStatus_);
    analysisOutputTake_->join();
    analysisOutputTakeLastSnapshot_ = analysisOutputTake_->snapshot();
    analysisOutputTakeStatus_ = outputTakeTerminalStatus(
        *analysisOutputTakeLastSnapshot_);
    analysisOutputTake_.reset();
    analysisOutputTakeDiskRecorderCapture_.reset();
    finishDiskRecorderTapTakes();
    saveAnalysisOutputTakeInItsFormat();
    setStatus(analysisOutputTakeStatus_);
    refreshAnalysisWorkspace();
}

void WorkbenchComponent::invalidateAnalysisOutputTakeForProjectAuthorityChange()
{
    auto authority = analysisOutputTakeAuthorityGeneration_.load(
        std::memory_order_acquire);
    if (authority != std::numeric_limits<std::uint64_t>::max())
    {
        (void) analysisOutputTakeAuthorityGeneration_.compare_exchange_strong(
            authority,
            authority + 1U,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }
    revokeAnalysisOutputTakeCallbackAndWait();
    if (analysisOutputTake_ == nullptr)
        return;
    analysisOutputTake_->requestAuthorityInvalidation();
    analysisOutputTakeStatus_ =
        "Cancelling output take: project authority changed...";
    setStatus(analysisOutputTakeStatus_);
}

void WorkbenchComponent::requestAnalysisOutputTakeStopFromDevice() noexcept
{
    // Device callbacks only revoke callback reachability. The message-thread
    // continuation owns requestStop(), join(), publication, and destruction,
    // so the device thread never dereferences a concurrently retired session.
    analysisOutputTakeLifecycle_.store(0, std::memory_order_seq_cst);
    analysisOutputTakeCallback_.store(nullptr, std::memory_order_release);
}

void WorkbenchComponent::publishAnalysisOutputTakeFromAudio(
    float* const* projectOutputChannels,
    std::size_t outputFrames,
    bool sourceRendered) noexcept
{
    if (analysisOutputTakeLifecycle_.load(std::memory_order_seq_cst) != 1)
        return;

    analysisOutputTakeAudioOperations_.fetch_add(
        1, std::memory_order_seq_cst);
    if (analysisOutputTakeLifecycle_.load(std::memory_order_seq_cst) == 1)
    {
        if (auto* outputTake = analysisOutputTakeCallback_.load(
                std::memory_order_acquire))
        {
            // The DiskRecorders within the Sound record what they hear,
            // handed over as it renders, not the output.
            if (analysisOutputTakeFromTaps_.load(std::memory_order_acquire))
            {
                if (!sourceRendered)
                {
                    outputTake->rejectRenderFailureFromAudio();
                    for (auto& callback : diskRecorderTapTakeCallbacks_)
                    {
                        if (auto* tapTake = callback.load(std::memory_order_acquire))
                            tapTake->rejectRenderFailureFromAudio();
                    }
                }
            }
            else if (!sourceRendered)
            {
                outputTake->rejectRenderFailureFromAudio();
            }
            else
            {
                (void) outputTake->publishFromAudio(
                    projectOutputChannels,
                    outputTake->channelCount(),
                    outputFrames,
                    projectBridge_.activeStageId(),
                    analysisOutputTakeAuthorityGeneration_.load(
                        std::memory_order_acquire),
                    projectBridge_.editor().documentAuthorityGeneration());
            }
        }
    }
    analysisOutputTakeAudioOperations_.fetch_sub(
        1, std::memory_order_seq_cst);
}

void WorkbenchComponent::diskRecorderTapAudio(void* context, std::size_t tap, float* const* channels,
    std::size_t channelCount, std::size_t frameCount) noexcept
{
    static_cast<WorkbenchComponent*>(context)->publishDiskRecorderTapFromAudio(
        tap, channels, channelCount, frameCount);
}

void WorkbenchComponent::publishDiskRecorderTapFromAudio(
    std::size_t tap, float* const* channels, std::size_t channelCount, std::size_t frameCount) noexcept
{
    if (!analysisOutputTakeFromTaps_.load(std::memory_order_acquire)
        || analysisOutputTakeLifecycle_.load(std::memory_order_seq_cst) != 1)
        return;
    analysisOutputTakeAudioOperations_.fetch_add(1, std::memory_order_seq_cst);
    if (analysisOutputTakeLifecycle_.load(std::memory_order_seq_cst) == 1)
    {
        auto* take = tap == 0 ? analysisOutputTakeCallback_.load(std::memory_order_acquire)
            : tap - 1 < diskRecorderTapTakeCallbacks_.size()
            ? diskRecorderTapTakeCallbacks_[tap - 1].load(std::memory_order_acquire)
            : nullptr;
        if (take != nullptr && channelCount >= take->channelCount())
        {
            (void) take->publishFromAudio(channels, take->channelCount(), frameCount,
                projectBridge_.activeStageId(),
                analysisOutputTakeAuthorityGeneration_.load(std::memory_order_acquire),
                projectBridge_.editor().documentAuthorityGeneration());
        }
    }
    analysisOutputTakeAudioOperations_.fetch_sub(1, std::memory_order_seq_cst);
}

// The other DiskRecorders within the Sound stop with the first: kept when
// its take completed, dropped otherwise.
void WorkbenchComponent::finishDiskRecorderTapTakes()
{
    analysisOutputTakeFromTaps_.store(false, std::memory_order_seq_cst);
    for (auto& callback : diskRecorderTapTakeCallbacks_)
        callback.store(nullptr, std::memory_order_seq_cst);
    while (analysisOutputTakeAudioOperations_.load(std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
    if (diskRecorderTapTakes_.empty())
        return;
    const auto keep = analysisOutputTakeLastSnapshot_.has_value()
        && analysisOutputTakeLastSnapshot_->state == talas::audio::RealtimeOutputTakeState::Completed;
    std::size_t kept = 0;
    for (auto& tapTake : diskRecorderTapTakes_)
    {
        if (keep)
            tapTake.take->requestStop();
        else
            tapTake.take->requestCancel();
        tapTake.take->join();
        const auto completed = tapTake.take->snapshot().state == talas::audio::RealtimeOutputTakeState::Completed;
        if (tapTake.conversion.has_value())
        {
            std::error_code ignored;
            if (!completed
                || !talas::audio::convertRecordingToSampleFile(
                    tapTake.conversion->second, tapTake.conversion->first, tapTake.format, tapTake.bits)
                        .empty())
            {
                std::filesystem::remove(tapTake.conversion->second, ignored);
                continue;
            }
        }
        kept += completed ? 1U : 0U;
    }
    if (kept != 0)
    {
        analysisOutputTakeStatus_ += juceString(" · ") + juce::String(static_cast<int>(kept))
            + (kept == 1 ? " more DiskRecorder saved" : " more DiskRecorders saved");
    }
    diskRecorderTapTakes_.clear();
}

std::optional<std::filesystem::path> WorkbenchComponent::diskRecorderDestination(
    const talas::runtime::PreparedDiskRecorderCaptureSummary& recorder, juce::String& error) const
{
    const auto& assets = projectBridge_.editor().document().assets;
    const auto asset = std::ranges::find(assets, recorder.destinationNameHint, &talas::project::ProjectAsset::id);
    const auto programFolder = filesystemPath(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                                  .getChildFile("Talas")
                                                  .getChildFile("Program")
                                                  .getFullPathName());
    const auto format = diskRecorderFileFormat(recorder.format);
    auto destination = asset != assets.end()
        ? talas::audio::resolveProjectAssetPath(*asset, projectPath())
        : talas::app::diskRecorderFileOfFormat(
            recorder.destinationNameHint, programFolder, talas::audio::sampleFileFormatExtension(format));
    if (!talas::audio::sampleFileHasFormatExtension(destination, format))
    {
        error = diskRecorderDestinationMustUse(format);
        return std::nullopt;
    }
    std::error_code ignored;
    std::filesystem::create_directories(destination.parent_path(), ignored);
    return destination;
}

void WorkbenchComponent::revokeAnalysisOutputTakeCallbackAndWait() noexcept
{
    analysisOutputTakeLifecycle_.store(0, std::memory_order_seq_cst);
    analysisOutputTakeCallback_.store(nullptr, std::memory_order_release);
    while (analysisOutputTakeAudioOperations_.load(
               std::memory_order_seq_cst)
           != 0)
    {
        std::this_thread::yield();
    }
}

void WorkbenchComponent::finishAnalysisOutputTakeIfReady()
{
    if (analysisOutputTake_ == nullptr)
        return;
    auto snapshot = analysisOutputTake_->snapshot();
    if (!snapshot.terminal())
    {
        const auto seconds = snapshot.sampleRate > 0.0
            ? static_cast<double>(snapshot.writtenFrames)
                / snapshot.sampleRate
            : 0.0;
        analysisOutputTakeStatus_ =
            (snapshot.state
                    == talas::audio::RealtimeOutputTakeState::Stopping
                ? analysisOutputTakeDiskRecorderCapture_.has_value()
                    ? "Finalizing Disk Recorder take"
                    : "Finalizing output take"
                : analysisOutputTakeDiskRecorderCapture_.has_value()
                    ? "Recording Disk Recorder take"
                    : "Recording output take")
            + juceString(" · ")
            + juce::String(static_cast<juce::int64>(snapshot.writtenFrames))
            + juceString(" frames · ") + juce::String(seconds, 1) + " s";
        if (snapshot.droppedBlocks != 0)
        {
            analysisOutputTakeStatus_ += juceString(" · ")
                + juce::String(
                    static_cast<juce::int64>(snapshot.droppedBlocks))
                + " dropped";
        }
        if (snapshot.rejectedBlocks != 0)
        {
            analysisOutputTakeStatus_ += juceString(" · ")
                + juce::String(
                    static_cast<juce::int64>(snapshot.rejectedBlocks))
                + " rejected";
        }
        return;
    }

    revokeAnalysisOutputTakeCallbackAndWait();
    analysisOutputTake_->join();
    analysisOutputTakeLastSnapshot_ = analysisOutputTake_->snapshot();
    analysisOutputTakeStatus_ = outputTakeTerminalStatus(
        *analysisOutputTakeLastSnapshot_);
    analysisOutputTake_.reset();
    analysisOutputTakeDiskRecorderCapture_.reset();
    finishDiskRecorderTapTakes();
    saveAnalysisOutputTakeInItsFormat();
    setStatus(analysisOutputTakeStatus_);
}

void WorkbenchComponent::stopAnalysisOutputTakeForLifecycle()
{
    revokeAnalysisOutputTakeCallbackAndWait();
    if (analysisOutputTake_ == nullptr)
        return;
    const auto destination = analysisOutputTake_->snapshot().destination;
    const auto destinationConflict =
        analysisOutputTakeDestinationConflicts(destination);
    const auto diskRecorderLifecycleRevocation =
        analysisOutputTakeDiskRecorderCapture_.has_value();
    if (destinationConflict || diskRecorderLifecycleRevocation)
        analysisOutputTake_->requestCancel();
    else
        analysisOutputTake_->requestStop();
    analysisOutputTake_->join();
    analysisOutputTakeLastSnapshot_ = analysisOutputTake_->snapshot();
    analysisOutputTakeStatus_ = destinationConflict
        ? juce::String{
            "Output take cancelled: destination became the open project, its package, or a source asset"}
        : diskRecorderLifecycleRevocation
            ? juce::String{
                "Disk Recorder take cancelled: lifecycle authority changed; destination was not published"}
        : outputTakeTerminalStatus(*analysisOutputTakeLastSnapshot_);
    analysisOutputTake_.reset();
    analysisOutputTakeDiskRecorderCapture_.reset();
    finishDiskRecorderTapTakes();
    saveAnalysisOutputTakeInItsFormat();
}

bool WorkbenchComponent::adoptPreparedOutputTakeStageForSmoke()
{
    const auto preparedBefore = projectBridge_.preparedGraphStatus().prepared;
    if (preparedBefore != nullptr
        && projectBridge_.activeStageId() != preparedBefore->stageId)
    {
        std::array<float, 128> output{};
        std::array<float*, 1> channels{output.data()};
        (void) projectBridge_.render(
            channels.data(), channels.size(), output.size());
    }
    const auto preparedAfter = projectBridge_.preparedGraphStatus().prepared;
    return preparedAfter != nullptr
        && projectBridge_.activeStageId() != 0
        && projectBridge_.activeStageId() == preparedAfter->stageId;
}
}
