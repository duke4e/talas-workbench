#include "app/workbench/WorkbenchComponentInternal.h"

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::waitForSoundLibraryAuditionSmoke()
{
    const auto deadline =
        juce::Time::getMillisecondCounterHiRes() + 10000.0;
    while (juce::Time::getMillisecondCounterHiRes() < deadline)
    {
        while (!soundLibraryAuditionWorkerFinished_.load(
                   std::memory_order_acquire)
               && juce::Time::getMillisecondCounterHiRes() < deadline)
        {
            std::this_thread::yield();
        }
        if (!soundLibraryAuditionWorkerFinished_.load(
                std::memory_order_acquire))
        {
            return "Sound Library audition worker exceeded its bounded smoke deadline";
        }
        finishSoundLibraryAuditionIfReady();
        if (!soundLibraryAuditionThread_.joinable())
            return {};
    }
    return "Sound Library audition publication exceeded its bounded smoke deadline";
}

std::optional<std::string>
WorkbenchComponent::waitForSoundLibraryAuditionBarrierSmoke() const
{
    const auto deadline =
        juce::Time::getMillisecondCounterHiRes() + 10000.0;
    while (!soundLibraryAuditionSmokeWorkerPaused_.load(
               std::memory_order_acquire)
           && juce::Time::getMillisecondCounterHiRes() < deadline)
    {
        std::this_thread::yield();
    }
    if (!soundLibraryAuditionSmokeWorkerPaused_.load(
            std::memory_order_acquire))
    {
        return "Sound Library audition worker did not reach its terminal publication barrier";
    }
    return {};
}

std::optional<std::string>
WorkbenchComponent::runSoundLibraryPendingSwitchPauseSmoke(
    std::string_view retainedDefinitionId,
    std::string_view replacementDefinitionId)
{
    const auto require = [](bool condition, std::string message)
        -> std::optional<std::string> {
        return condition ? std::optional<std::string>{}
                         : std::optional<std::string>{std::move(message)};
    };
    soundDefinitionsPanel_->selectDefinition(replacementDefinitionId);
    if (auto error = require(
            soundDefinitionsPanel_->auditionActionIsSwitchForSmoke(),
            "Sound Library audition switch did not expose its exact enabled action"))
    {
        return error;
    }
    soundLibraryAuditionAudioOperations_.store(
        1, std::memory_order_seq_cst);
    soundLibraryAuditionSmokeUsePreparedProfile_ = true;
    soundDefinitionsPanel_->triggerAuditionActionForSmoke();
    soundLibraryAuditionSmokeUsePreparedProfile_ = false;
    if (auto error = require(
            soundLibraryAuditionPending_.has_value()
                && soundLibraryAuditionActive_ == nullptr
                && soundDefinitionsPanel_->hasActiveAuditionState(
                    replacementDefinitionId),
            "Sound Library switch did not retain an exact stoppable pending request while the old runtime retired"))
    {
        soundLibraryAuditionAudioOperations_.store(
            0, std::memory_order_seq_cst);
        return error;
    }
    soundDefinitionsPanel_->triggerAuditionActionForSmoke();
    if (auto error = require(
            !soundLibraryAuditionPending_.has_value()
                && soundDefinitionsPanel_->auditionStateIsIdle(),
            "Stop Preview did not cancel the exact pending Sound Library switch"))
    {
        soundLibraryAuditionAudioOperations_.store(
            0, std::memory_order_seq_cst);
        return error;
    }
    soundDefinitionsPanel_->selectDefinition(replacementDefinitionId);
    soundLibraryAuditionSmokeUsePreparedProfile_ = true;
    soundDefinitionsPanel_->triggerAuditionActionForSmoke();
    soundLibraryAuditionSmokeUsePreparedProfile_ = false;
    if (auto error = require(
            soundLibraryAuditionPending_.has_value()
                && soundDefinitionsPanel_->hasActiveAuditionState(
                    replacementDefinitionId),
            "Sound Library switch did not restore its stoppable pending presentation"))
    {
        soundLibraryAuditionAudioOperations_.store(
            0, std::memory_order_seq_cst);
        return error;
    }
    soundDefinitionsPanel_->selectDefinition(retainedDefinitionId);
    soundLibraryAuditionSmokeUsePreparedProfile_ = true;
    soundDefinitionsPanel_->triggerAuditionActionForSmoke();
    soundLibraryAuditionSmokeUsePreparedProfile_ = false;
    if (auto error = require(
            soundLibraryAuditionPending_.has_value()
                && soundLibraryAuditionPending_->stableEntryId
                    == "project:" + std::string{retainedDefinitionId},
            "A third Sound Library action did not replace the prior pending switch"))
    {
        soundLibraryAuditionAudioOperations_.store(
            0, std::memory_order_seq_cst);
        return error;
    }
    soundDefinitionsPanel_->selectDefinition(replacementDefinitionId);
    soundLibraryAuditionSmokeUsePreparedProfile_ = true;
    soundDefinitionsPanel_->triggerAuditionActionForSmoke();
    soundLibraryAuditionSmokeUsePreparedProfile_ = false;
    if (auto error = require(
            soundLibraryAuditionPending_.has_value()
                && soundLibraryAuditionPending_->stableEntryId
                    == "project:" + std::string{replacementDefinitionId},
            "The latest Sound Library switch did not own the single pending slot"))
    {
        soundLibraryAuditionAudioOperations_.store(
            0, std::memory_order_seq_cst);
        return error;
    }
    playCurrentProject();
    if (auto error = require(
            !soundLibraryAuditionPending_.has_value()
                && soundDefinitionsPanel_->auditionStateIsIdle(),
            "Play did not revoke the queued Sound Library switch authority"))
    {
        soundLibraryAuditionAudioOperations_.store(
            0, std::memory_order_seq_cst);
        return error;
    }
    soundLibraryAuditionAudioOperations_.store(
        0, std::memory_order_seq_cst);
    retireSoundLibraryAuditionIfQuiescent();
    startPendingSoundLibraryAuditionIfReady();
    if (auto error = require(
            soundLibraryAuditionActive_ == nullptr
                && !soundLibraryAuditionPending_.has_value(),
            "A queued Sound Library switch started after Play completed its revocation"))
    {
        return error;
    }
    soundDefinitionsPanel_->selectDefinition(retainedDefinitionId);
    startSoundLibraryAuditionWithProfile(
        "project:" + std::string{retainedDefinitionId},
        8000.0,
        64,
        deviceGeneration_.load(std::memory_order_acquire));
    if (auto error = waitForSoundLibraryAuditionSmoke())
        return error;
    soundDefinitionsPanel_->selectDefinition(replacementDefinitionId);
    soundLibraryAuditionAudioOperations_.store(
        1, std::memory_order_seq_cst);
    soundLibraryAuditionSmokeUsePreparedProfile_ = true;
    soundDefinitionsPanel_->triggerAuditionActionForSmoke();
    soundLibraryAuditionSmokeUsePreparedProfile_ = false;
    pauseCurrentProject();
    soundLibraryAuditionAudioOperations_.store(
        0, std::memory_order_seq_cst);
    retireSoundLibraryAuditionIfQuiescent();
    startPendingSoundLibraryAuditionIfReady();
    if (auto error = require(
            !soundLibraryAuditionPending_.has_value()
                && soundLibraryAuditionActive_ == nullptr
                && !auditionActive_
                && !transportPlaying_,
            "Pause allowed a queued Sound Library switch to start afterward"))
    {
        return error;
    }
    soundLibraryAuditionPending_ = PendingSoundLibraryAudition{
        "project:" + std::string{replacementDefinitionId},
        8000.0,
        0,
        deviceGeneration_.load(std::memory_order_acquire),
    };
    soundDefinitionsPanel_->setAuditionState(
        soundLibraryAuditionPending_->stableEntryId);
    soundDefinitionsPanel_->selectDefinition(replacementDefinitionId);
    startPendingSoundLibraryAuditionIfReady();
    if (auto error = require(
            !soundLibraryAuditionPending_.has_value()
                && soundDefinitionsPanel_->auditionStateIsIdle()
                && soundLibraryAuditionActive_ == nullptr,
            "Synchronous pending audition rejection left a phantom Stop Preview action"))
    {
        return error;
    }
    soundDefinitionsPanel_->selectDefinition(retainedDefinitionId);
    startSoundLibraryAuditionWithProfile(
        "project:" + std::string{retainedDefinitionId},
        8000.0,
        64,
        deviceGeneration_.load(std::memory_order_acquire));
    if (auto error = waitForSoundLibraryAuditionSmoke())
        return error;
    soundDefinitionsPanel_->selectDefinition(replacementDefinitionId);
    return require(
        soundDefinitionsPanel_->auditionActionIsSwitchForSmoke(),
        "Sound Library audition switch action was not restored after cancellation");
}

std::optional<std::string>
WorkbenchComponent::runSoundLibraryPrivateAuditionSmoke(
    const std::filesystem::path& smokeDirectory,
    std::string_view definitionId)
{
    const auto require = [](bool condition, std::string message)
        -> std::optional<std::string> {
        return condition ? std::optional<std::string>{}
                         : std::optional<std::string>{std::move(message)};
    };
    const auto waitForPublication = [this] {
        return waitForSoundLibraryAuditionSmoke();
    };
    const auto waitForBarrier = [this] {
        return waitForSoundLibraryAuditionBarrierSmoke();
    };
    const auto start = [&] {
        soundDefinitionsPanel_->selectDefinition(definitionId);
        startSoundLibraryAuditionWithProfile(
            "project:" + std::string{definitionId},
            8000.0,
            64,
            deviceGeneration_.load(std::memory_order_acquire));
    };

    const auto beforeDocument = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    const auto beforeStage = projectBridge_.activeStageId();
    const auto beforeStaged = currentDocumentStaged_;
    const auto beforeTransport = transportPlaying_;
    const auto takePath = smokeDirectory
        / "workbench-sound-audition-output-take.wav";
    std::string takeError;
    auto take = talas::audio::RealtimeOutputTake::start(
        {takePath,
         talas::audio::ExistingAudioFilePolicy::Replace,
         8000.0,
         2,
         64,
         4,
         beforeStage,
         analysisOutputTakeAuthorityGeneration_.load(
             std::memory_order_acquire),
         projectBridge_.editor().documentAuthorityGeneration()},
        takeError);
    if (take == nullptr)
        return "Sound audition output-take fixture could not start: " + takeError;
    std::array<float, 64> takeLeft{};
    std::array<float, 64> takeRight{};
    std::array<float*, 2> takeChannels{takeLeft.data(), takeRight.data()};
    analysisOutputTakeLastSnapshot_.reset();
    analysisOutputTake_ = std::move(take);
    analysisOutputTakeCallback_.store(
        analysisOutputTake_.get(), std::memory_order_release);
    analysisOutputTakeLifecycle_.store(1, std::memory_order_seq_cst);
    if (!analysisOutputTake_->publishFromAudio(
            takeChannels.data(),
            takeChannels.size(),
            takeLeft.size(),
            beforeStage,
            analysisOutputTakeAuthorityGeneration_.load(
                std::memory_order_acquire),
            projectBridge_.editor().documentAuthorityGeneration()))
    {
        return "Sound audition output-take fixture rejected its source stage";
    }

    start();
    if (auto error = waitForPublication())
        return error;
    std::array<float, 64> left{};
    std::array<float, 64> right{};
    std::array<float*, 2> channels{left.data(), right.data()};
    if (auto error = require(
            auditionActive_
                && currentDocumentStaged_ == beforeStaged
                && projectBridge_.activeStageId() == beforeStage
                && analysisOutputTake_ == nullptr
                && analysisOutputTakeLastSnapshot_.has_value()
                && analysisOutputTakeLastSnapshot_->state
                    == talas::audio::RealtimeOutputTakeState::Completed
                && std::filesystem::exists(takePath)
                && soundDefinitionsPanel_->hasActiveAuditionState(definitionId)
                && projectStateLabel_.getText().contains(
                    "Previewing project:" + juceString(definitionId))
                && renderSoundLibraryAudition(
                       0.0, channels.data(), channels.size(), left.size())
                       .value_or(false)
                && std::ranges::any_of(left, [](float sample) {
                       return std::isfinite(sample)
                           && std::abs(sample) > 1.0e-6F;
                   })
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == beforeDocument,
            "isolated reusable Sound audition did not render without mutating the project"))
    {
        return error;
    }

    soundLibraryAuditionActive_->previousTransportPlaying = true;
    pauseCurrentProject();
    std::ranges::fill(left, 0.0F);
    std::ranges::fill(right, 0.0F);
    if (auto error = require(
            !transportPlaying_
                && soundLibraryAuditionActive_ != nullptr
                && !soundLibraryAuditionActive_->previousTransportPlaying
                && renderSoundLibraryAudition(
                       0.01, channels.data(), channels.size(), left.size())
                       .value_or(false)
                && std::ranges::all_of(left, [](float sample) {
                       return sample == 0.0F;
                   })
                && std::ranges::all_of(right, [](float sample) {
                       return sample == 0.0F;
                   }),
            "Pause did not silence private audition and revoke main-transport restoration"))
    {
        return error;
    }
    stopCurrentProject();
    if (auto error = require(
            !transportPlaying_ && soundLibraryAuditionActive_ == nullptr
                && soundDefinitionsPanel_->auditionStateIsIdle(),
            "stopping a paused audition incorrectly restarted the main transport"))
    {
        return error;
    }
    start();
    if (auto error = waitForPublication())
        return error;

    std::error_code cleanupError;
    std::filesystem::remove(takePath, cleanupError);
    const auto image = createComponentSnapshot(getLocalBounds(), true, 1.0F);
    auto stream = juce::File{juceString(
        (smokeDirectory / "workbench-audition.png").string())}
                      .createOutputStream();
    if (!image.isValid() || stream == nullptr || !stream->openedOk()
        || !stream->setPosition(0) || stream->truncate().failed()
        || !juce::PNGImageFormat{}.writeImageToStream(image, *stream))
    {
        return "Sound Library audition snapshot could not be written";
    }
    stream->flush();

    if (auto error = require(
            soundDefinitionsPanel_->auditionInputSubstitutionsWorkForSmoke()
                && soundLibraryAuditionActive_ == nullptr
                && soundDefinitionsPanel_->auditionStateIsIdle(),
            "Changing the closed audition input plan did not revoke the active private runtime"))
    {
        return error;
    }
    start();
    if (auto error = waitForPublication())
        return error;
    start();
    if (auto error = require(
            !auditionActive_ && currentDocumentStaged_ == beforeStaged
                && projectBridge_.activeStageId() == beforeStage
                && transportPlaying_ == beforeTransport
                && soundDefinitionsPanel_->auditionStateIsIdle(),
            "second audition action did not leave isolated preview"))
    {
        return error;
    }

    soundLibraryAuditionSmokeHoldNextWorker_ = true;
    start();
    if (auto error = waitForBarrier())
        return error;
    start();
    if (auto error = waitForPublication())
        return error;
    if (auto error = require(
            !auditionActive_ && soundLibraryAuditionActive_ == nullptr
                && soundLibraryAuditionRetiring_ == nullptr
                && soundDefinitionsPanel_->auditionStateIsIdle()
                && currentDocumentStaged_ == beforeStaged
                && projectBridge_.activeStageId() == beforeStage
                && transportPlaying_ == beforeTransport
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == beforeDocument,
            "Sound Library preparation cancellation changed project, stage, transport, or preview authority"))
    {
        return error;
    }

    soundLibraryAuditionSmokeHoldNextWorker_ = true;
    start();
    if (auto error = waitForBarrier())
        return error;
    const auto retainedInputs = workspaceSession_.soundLibrary.inputSubstitutions;
    workspaceSession_.soundLibrary.inputSubstitutions.push_back({
        "staleTerminalInput",
        talas::project::WorkspaceSoundLibraryAuditionSource::Constant,
        0.25,
        std::nullopt,
    });
    soundLibraryAuditionSmokeHoldWorker_.store(false, std::memory_order_release);
    if (auto error = waitForPublication())
        return error;
    workspaceSession_.soundLibrary.inputSubstitutions = retainedInputs;
    if (auto error = require(
            soundLibraryAuditionActive_ == nullptr
                && soundDefinitionsPanel_->auditionStateIsIdle(),
            "Sound Library audition survived a terminal input-authority change"))
    {
        return error;
    }

    soundLibraryAuditionSmokeHoldNextWorker_ = true;
    start();
    if (auto error = waitForBarrier())
        return error;
    uiDocumentGeneration_.fetch_add(1, std::memory_order_acq_rel);
    soundLibraryAuditionSmokeHoldWorker_.store(false, std::memory_order_release);
    if (auto error = waitForPublication())
        return error;
    if (auto error = require(
            soundLibraryAuditionActive_ == nullptr
                && soundDefinitionsPanel_->auditionStateIsIdle(),
            "Sound Library audition survived a terminal document-generation change"))
    {
        return error;
    }

    soundLibraryAuditionSmokeHoldNextWorker_ = true;
    start();
    if (auto error = waitForBarrier())
        return error;
    deviceGeneration_.fetch_add(1, std::memory_order_acq_rel);
    soundLibraryAuditionSmokeHoldWorker_.store(false, std::memory_order_release);
    if (auto error = waitForPublication())
        return error;
    return require(
        soundLibraryAuditionActive_ == nullptr
            && soundDefinitionsPanel_->auditionStateIsIdle(),
        "Sound Library audition survived a terminal device-generation change");
}
}
