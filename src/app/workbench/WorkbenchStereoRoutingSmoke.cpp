#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::runStereoRoutingExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:stereo-routing-utilities"};
    constexpr double sampleRate = 48'000.0;
    constexpr std::size_t blockFrames = 128;

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
                    "C15 stereo-routing smoke restore")
                && error.empty())
            {
                error =
                    "C15 stereo-routing smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
        {
            error =
                "C15 stereo-routing smoke could not restore the prior project";
        }
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
            entry->actions, talas::app::SoundLibraryAction::OpenExample)
        || !talas::app::soundLibraryHasAction(
            entry->actions, talas::app::SoundLibraryAction::Audition))
    {
        return finish(
            "C15 packaged stereo-routing entry omitted open or audition authority");
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
            "C15 packaged stereo-routing entry could not be selected in the Sound Library");
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
        || !std::ranges::all_of(
            auditionLeft,
            [](float sample) { return std::isfinite(sample); })
        || !std::ranges::all_of(
            auditionRight,
            [](float sample) { return std::isfinite(sample); })
        || !std::ranges::any_of(
            auditionLeft,
            [](float sample) { return std::abs(sample) > 1.0e-6F; })
        || !std::ranges::any_of(
            auditionRight,
            [](float sample) { return std::abs(sample) > 1.0e-6F; }))
    {
        return finish(
            "C15 packaged stereo-routing private audition did not render audible finite stereo output");
    }
    stopSoundLibraryAudition();

    selectPackagedEntry();
    openSoundLibraryExampleNow(std::string{stableId});
    const auto& opened = projectBridge_.editor().document();
    constexpr std::array requiredTypes{
        std::string_view{"talas.level"},
        std::string_view{"talas.channelCrosser"},
        std::string_view{"talas.channelJoin"},
        std::string_view{"talas.channeller"},
    };
    const auto containsAllRequiredTypes = std::ranges::all_of(
        requiredTypes,
        [&](std::string_view typeId) {
            return std::ranges::any_of(
                opened.graph.nodes,
                [&](const talas::project::ProjectNode& node) {
                    return node.typeId == typeId;
                });
        });
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Stereo Routing Utilities")
        || !containsAllRequiredTypes)
    {
        return finish(
            "C15 packaged stereo-routing Open Example action did not install its exact untitled project");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "C15 packaged stereo-routing example"))
    {
        return finish(
            "C15 packaged stereo-routing example did not stage: "
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
        || !std::ranges::all_of(
            firstLeft,
            [](float sample) { return std::isfinite(sample); })
        || !std::ranges::all_of(
            firstRight,
            [](float sample) { return std::isfinite(sample); })
        || !std::ranges::any_of(
            firstLeft,
            [](float sample) { return std::abs(sample) > 1.0e-6F; })
        || !std::ranges::any_of(
            firstRight,
            [](float sample) { return std::abs(sample) > 1.0e-6F; }))
    {
        return finish(
            "C15 opened stereo-routing example did not adopt and play finite stereo output");
    }

    if (!stageProject(settings, "C15 stereo-routing explicit restage"))
    {
        return finish(
            "C15 opened stereo-routing example did not accept an exact restage");
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
        || firstLeft != restagedLeft || firstRight != restagedRight)
    {
        return finish(
            "C15 stereo-routing restage did not replace and restart exact prepared state");
    }
    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    return runShelvingPresenceExampleSmoke();
}
}
