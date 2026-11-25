#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::runTimeStopperExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:time-stopper-sample-hold"};
    constexpr double sampleRate = 8.0;
    constexpr std::size_t blockFrames = 8;

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
                    "C68 Time Stopper smoke restore")
                && error.empty())
            {
                error =
                    "C68 Time Stopper smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error = "C68 Time Stopper smoke could not restore the prior project";
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
            "C68 packaged Time Stopper entry omitted open or audition authority");
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
            "C68 packaged Time Stopper entry could not be selected in the Sound Library");
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
        || !std::ranges::any_of(
            auditionLeft,
            [](float sample) { return std::abs(sample) > 1.0e-6F; }))
    {
        return finish(
            "C68 packaged Time Stopper audition did not render audible finite output");
    }
    stopSoundLibraryAudition();

    selectPackagedEntry();
    openSoundLibraryExampleNow(std::string{stableId});
    const auto& opened = projectBridge_.editor().document();
    const auto timeStopperCount = std::ranges::count(
        opened.graph.nodes,
        std::string_view{"talas.timeStopper"},
        &talas::project::ProjectNode::typeId);
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Time Stopper Sample Hold")
        || timeStopperCount != 1
        || !opened.assets.empty())
    {
        return finish(
            "C68 packaged Time Stopper Open Example action did not install its exact untitled project");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "C68 packaged Time Stopper example"))
    {
        return finish(
            "C68 packaged Time Stopper example did not stage: "
            + statusLabel_.getText().toStdString());
    }
    if (!projectBridge_.requestTransportRelocation(0))
    {
        return finish(
            "C68 packaged Time Stopper example did not accept an exact transport rewind");
    }
    const auto renderOpened = [&] {
        std::array<float, blockFrames> left{};
        std::array<float, blockFrames> right{};
        std::array<float*, 2> channels{left.data(), right.data()};
        projectBridge_.setPlaying(true);
        const auto rendered = projectBridge_.render(
            {}, 0.0, channels.data(), channels.size(), blockFrames);
        projectBridge_.setPlaying(false);
        return std::pair{rendered, left};
    };
    const auto [firstRendered, first] = renderOpened();
    const auto pi = std::acos(-1.0);
    const auto quarter = static_cast<float>(std::sin(pi / 4.0));
    const auto held = static_cast<float>(
        std::sin(3.0 * pi / 4.0));
    const std::array<float, blockFrames> expected{
        0.0F,
        quarter,
        1.0F,
        held,
        held,
        held,
        held,
        held,
    };
    if (!firstRendered || first != expected)
    {
        return finish(
            "C68 opened Time Stopper example did not play its exact sample-hold lifecycle");
    }
    if (!stageProject(settings, "C68 Time Stopper explicit restage"))
    {
        return finish(
            "C68 opened Time Stopper example did not accept an exact restage");
    }
    if (!projectBridge_.requestTransportRelocation(0))
    {
        return finish(
            "C68 Time Stopper restage did not accept an exact transport rewind");
    }
    const auto [restagedRendered, restaged] = renderOpened();
    if (!restagedRendered || restaged != first)
    {
        return finish(
            "C68 Time Stopper restage did not replace and restart exact held state");
    }
    if (auto error = finish())
        return error;
    return runTimeOffsetExampleSmoke();
}
}
