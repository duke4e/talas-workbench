#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::runWarpedTimeIndexExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:bounded-warped-time-index"};
    constexpr double sampleRate = 48'000.0;
    constexpr std::size_t blockFrames = 750;

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
                    "C30 Warped Time Index smoke restore")
                && error.empty())
            {
                error =
                    "C30 Warped Time Index smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
        {
            error =
                "C30 Warped Time Index smoke could not restore the prior project";
        }
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
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
            "C30 packaged Warped Time Index entry omitted open or audition authority");
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
            "C30 packaged Warped Time Index entry could not be selected");
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
    if (soundLibraryAuditionActive_ == nullptr
        || soundLibraryAuditionActive_->entry.stableId != stableId
        || auditionRendered != std::optional<bool>{true}
        || !finiteAndAudible(auditionLeft)
        || !std::ranges::all_of(
            auditionRight,
            [](float sample) { return std::isfinite(sample); }))
    {
        return finish(
            "C30 packaged Warped Time Index audition did not render finite audible output");
    }
    stopSoundLibraryAudition();

    selectPackagedEntry();
    openSoundLibraryExampleNow(std::string{stableId});
    const auto& opened = projectBridge_.editor().document();
    const auto countType = [&](std::string_view typeId) {
        return std::ranges::count(
            opened.graph.nodes,
            typeId,
            &talas::project::ProjectNode::typeId);
    };
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Bounded Warped Time Index")
        || countType("talas.warpedTimeIndex") != 1
        || countType("talas.clock") != 1
        || countType("talas.level") != 1
        || countType("talas.oscillator") != 1
        || countType("talas.audioOutput") != 1
        || !opened.assets.empty())
    {
        return finish(
            "C30 Open Example action did not install the exact Warped Time Index project");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(
            settings,
            "C30 packaged Warped Time Index example"))
    {
        return finish(
            "C30 Warped Time Index example did not stage: "
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
        || !std::ranges::all_of(
            firstRight,
            [](float sample) { return std::isfinite(sample); }))
    {
        return finish(
            "C30 opened Warped Time Index example did not play finite audible output");
    }

    if (!stageProject(
            settings,
            "C30 Warped Time Index explicit restage"))
    {
        return finish(
            "C30 Warped Time Index example did not accept restage");
    }
    std::array<float, blockFrames> restagedLeft{};
    std::array<float, blockFrames> restagedRight{};
    std::array<float*, 2> restagedChannels{
        restagedLeft.data(), restagedRight.data()};
    if (!projectBridge_.render(
            {},
            0.0,
            restagedChannels.data(),
            restagedChannels.size(),
            blockFrames)
        || firstLeft != restagedLeft
        || firstRight != restagedRight)
    {
        return finish(
            "C30 Warped Time Index restage did not restart exact source state");
    }
    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    return runSpectrumOnDiskExampleSmoke();
}
}
