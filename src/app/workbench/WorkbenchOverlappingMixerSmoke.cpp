#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::runOverlappingMixerExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:bounded-overlapping-mixer"};
    constexpr double sampleRate = 48'000.0;
    constexpr std::size_t blockFrames = 256;

    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto previousDocumentStaged = currentDocumentStaged_;
    const auto previousTransportPlaying = transportPlaying_;
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        stopSoundLibraryAudition();
        projectBridge_.setPlaying(false);
        const auto restored =
            projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(
            previousCleanDocument, previousProjectPath);
        documentChanged();
        if (previousDocumentStaged)
        {
            talas::runtime::RenderSettings restoreSettings{
                sampleRate, blockFrames};
            if (!stageProject(
                    std::move(restoreSettings),
                    "C23 Overlapping Mixer smoke restore")
                && error.empty())
            {
                error = "C23 Overlapping Mixer smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error = "C23 Overlapping Mixer smoke could not restore the prior project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };

    refreshSoundLibraryProjection();
    const auto* entry = soundLibraryEntry(stableId);
    if (entry == nullptr
        || entry->sourceKind
            != talas::app::SoundLibrarySourceKind::OriginalExample
        || !talas::app::soundLibraryHasAction(
            entry->actions,
            talas::app::SoundLibraryAction::OpenExample)
        || !talas::app::soundLibraryHasAction(
            entry->actions,
            talas::app::SoundLibraryAction::Audition))
    {
        return finish(
            "C23 packaged Overlapping Mixer entry omitted open or audition authority");
    }

    const auto selectPackagedEntry = [&] {
        auto state = soundDefinitionsPanel_->libraryState();
        state.collection =
            talas::project::WorkspaceSoundLibraryCollection::All;
        state.sourceFilter =
            talas::project::WorkspaceSoundLibrarySourceFilter::All;
        state.compatibilityFilter =
            talas::project::WorkspaceSoundLibraryCompatibilityFilter::All;
        state.healthFilter =
            talas::project::WorkspaceSoundLibraryHealthFilter::All;
        state.category.reset();
        state.origin.reset();
        state.selectedEntryId = std::string{stableId};
        workspaceSession_.soundLibrary = state;
        soundDefinitionsPanel_->setLibraryState(state);
    };
    selectPackagedEntry();
    if (soundDefinitionsPanel_->selectedEntryId() != stableId)
    {
        return finish(
            "C23 packaged Overlapping Mixer entry could not be selected in the Sound Library");
    }
    startSoundLibraryAuditionWithProfile(
        std::string{stableId},
        sampleRate,
        blockFrames,
        deviceGeneration_.load(std::memory_order_acquire));
    if (auto error = waitForSoundLibraryAuditionSmoke())
        return finish(*error);

    std::array<float, blockFrames> auditionLeft{};
    std::array<float, blockFrames> auditionRight{};
    std::array<float*, 2> auditionChannels{
        auditionLeft.data(), auditionRight.data()};
    const auto auditionRendered = renderSoundLibraryAudition(
        0.0,
        auditionChannels.data(),
        auditionChannels.size(),
        blockFrames);
    const auto finiteAndAudible = [](const auto& samples) {
        return std::ranges::all_of(
                   samples,
                   [](float sample) { return std::isfinite(sample); })
            && std::ranges::any_of(
                   samples,
                   [](float sample) {
                       return std::abs(sample) > 1.0e-6F;
                   });
    };
    if (soundLibraryAuditionActive_ == nullptr
        || soundLibraryAuditionActive_->entry.stableId != stableId
        || auditionRendered != std::optional<bool>{true}
        || !finiteAndAudible(auditionLeft)
        || !finiteAndAudible(auditionRight)
        || auditionLeft == auditionRight)
    {
        return finish(
            "C23 packaged Overlapping Mixer audition did not render distinct audible finite stereo channels");
    }

    const std::array previousMappings{
        projectOutputChannelMap_[0].load(std::memory_order_relaxed),
        projectOutputChannelMap_[1].load(std::memory_order_relaxed)};
    projectOutputChannelMap_[0].store(0, std::memory_order_relaxed);
    projectOutputChannelMap_[1].store(1, std::memory_order_relaxed);
    const juce::AudioIODeviceCallbackContext callbackContext{};
    audioDeviceIOCallbackWithContext(
        nullptr, 0, auditionChannels.data(), auditionChannels.size(),
        blockFrames, callbackContext);
    projectOutputChannelMap_[0].store(previousMappings[0], std::memory_order_relaxed);
    projectOutputChannelMap_[1].store(previousMappings[1], std::memory_order_relaxed);
    refreshOutputMeterLabel();
    if (soundLibraryAuditionOutputChannels_.load(std::memory_order_acquire) != 2
        || outputMeterPeaks_[0].load(std::memory_order_relaxed) <= 1.0e-6F
        || outputMeterPeaks_[1].load(std::memory_order_relaxed) <= 1.0e-6F
        || !outputMeterLabel_.getText().contains(" 2")
        || outputMeterLabel_.getText().contains(" 3"))
    {
        return finish("Stereo whole-example audition must publish and display both live meters");
    }
    stopSoundLibraryAudition();

    selectPackagedEntry();
    openSoundLibraryExampleNow(std::string{stableId});
    const auto& opened = projectBridge_.editor().document();
    const auto mixerCount = std::ranges::count(
        opened.graph.nodes,
        std::string_view{"talas.overlappingMixer"},
        &talas::project::ProjectNode::typeId);
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Bounded Overlapping Mixer")
        || mixerCount != 1
        || !opened.assets.empty())
    {
        return finish(
            "C23 packaged Overlapping Mixer Open Example action did not install its exact untitled project");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(
            settings,
            "C23 packaged Overlapping Mixer example"))
    {
        return finish(
            "C23 packaged Overlapping Mixer example did not stage: "
            + statusLabel_.getText().toStdString());
    }
    std::array<float, blockFrames> firstLeft{};
    std::array<float, blockFrames> firstRight{};
    std::array<float*, 2> firstChannels{
        firstLeft.data(), firstRight.data()};
    projectBridge_.setPlaying(true);
    if (!projectBridge_.render(
            {},
            0.0,
            firstChannels.data(),
            firstChannels.size(),
            blockFrames)
        || !finiteAndAudible(firstLeft)
        || !finiteAndAudible(firstRight)
        || firstLeft == firstRight)
    {
        return finish(
            "C23 opened Overlapping Mixer example did not adopt and play finite stereo output");
    }

    if (!stageProject(
            settings,
            "C23 Overlapping Mixer explicit restage"))
    {
        return finish(
            "C23 opened Overlapping Mixer example did not accept an exact restage");
    }
    std::array<float, blockFrames> restagedLeft{};
    std::array<float, blockFrames> restagedRight{};
    std::array<float*, 2> restagedChannels{
        restagedLeft.data(), restagedRight.data()};
    if (!projectBridge_.render(
            {},
            1.0,
            restagedChannels.data(),
            restagedChannels.size(),
            blockFrames)
        || firstLeft != restagedLeft
        || firstRight != restagedRight)
    {
        return finish(
            "C23 Overlapping Mixer restage did not replace and restart exact upstream phase state");
    }
    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    return runOscillatorBankExampleSmoke();
}
}
