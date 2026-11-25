#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::runThresholdExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:threshold-hysteresis-gate"};
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
                    "C66 Threshold smoke restore")
                && error.empty())
            {
                error = "C66 Threshold smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error = "C66 Threshold smoke could not restore the prior project";
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
            "C66 packaged Threshold entry omitted open or audition authority");
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
            "C66 packaged Threshold entry could not be selected in the Sound Library");
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
            "C66 packaged Threshold audition did not render audible finite output");
    }
    stopSoundLibraryAudition();

    selectPackagedEntry();
    openSoundLibraryExampleNow(std::string{stableId});
    const auto& opened = projectBridge_.editor().document();
    const auto thresholdCount = std::ranges::count(
        opened.graph.nodes,
        std::string_view{"talas.threshold"},
        &talas::project::ProjectNode::typeId);
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Threshold Hysteresis Gate")
        || thresholdCount != 1
        || !opened.assets.empty())
    {
        return finish(
            "C66 packaged Threshold Open Example action did not install its exact untitled project");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "C66 packaged Threshold example"))
    {
        return finish(
            "C66 packaged Threshold example did not stage: "
            + statusLabel_.getText().toStdString());
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
    const std::array<float, blockFrames> expected{
        0.0F,
        0.0F,
        static_cast<float>(std::sin(pi / 4.0)),
        static_cast<float>(std::sin(3.0 * pi / 8.0)),
        1.0F,
        0.0F,
        0.0F,
        0.0F,
    };
    if (!firstRendered || first != expected)
    {
        return finish(
            "C66 opened Threshold example did not adopt and play its exact latch lifecycle");
    }
    if (!stageProject(settings, "C66 Threshold explicit restage"))
    {
        return finish(
            "C66 opened Threshold example did not accept an exact restage");
    }
    const auto [restagedRendered, restaged] = renderOpened();
    if (!restagedRendered || restaged != first)
    {
        return finish(
            "C66 Threshold restage did not replace and restart exact latch state");
    }
    if (auto error = finish())
        return error;
    return runTimeControlExampleSmoke();
}
}
