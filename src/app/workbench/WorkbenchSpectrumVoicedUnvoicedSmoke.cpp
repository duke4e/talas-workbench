#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <array>

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::runSpectrumVoicedUnvoicedExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:spectrum-voiced-unvoiced-transition"};
    constexpr double sampleRate = 48'000.0;
    constexpr std::size_t blockFrames = 128;
    constexpr std::array expected{1.0F, 0.5F, 0.0F, 0.5F, 1.0F};

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
                    "C57 voiced/unvoiced smoke restore")
                && error.empty())
            {
                error =
                    "C57 voiced/unvoiced smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
        {
            error =
                "C57 voiced/unvoiced smoke could not restore the prior project";
        }
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    const auto blockEquals = [](const auto& samples, float value) {
        return std::ranges::all_of(
            samples,
            [value](float sample) { return sample == value; });
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
            "C57 packaged voiced/unvoiced entry omitted open or audition authority");
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
            "C57 packaged voiced/unvoiced entry could not be selected");
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
        || !blockEquals(auditionLeft, 1.0F)
        || !blockEquals(auditionRight, 1.0F))
    {
        return finish(
            "C57 packaged voiced/unvoiced audition did not publish the exact first voiced endpoint");
    }
    stopSoundLibraryAudition();

    selectPackagedEntry();
    openSoundLibraryExampleNow(std::string{stableId});
    const auto opened = projectBridge_.editor().document();
    const auto countType = [&](std::string_view typeId) {
        return std::ranges::count(
            opened.graph.nodes,
            typeId,
            &talas::project::ProjectNode::typeId);
    };
    const auto hasExactTableAsset = opened.assets.size() == 1
        && opened.assets.front().id == "Analysis/C57"
        && opened.assets.front().kind
            == talas::project::ProjectAssetKind::Table
        && opened.assets.front().contentHash
            == "0af93d5e6d03bec92bc4b965bb48d7222ec2e5029cdbd4de2073ceca3074aa88";
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Spectrum Voiced Unvoiced Transition")
        || countType("talas.spectrumOnDisk") != 1
        || countType("talas.spectrumVoicedUnvoiced") != 1
        || countType("talas.audioOutput") != 1
        || !hasExactTableAsset)
    {
        return finish(
            "C57 Open Example action did not install the exact voiced/unvoiced transition project and table identity");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "C57 packaged voiced/unvoiced transition example"))
    {
        return finish(
            "C57 voiced/unvoiced transition example did not stage: "
            + statusLabel_.getText().toStdString());
    }
    projectBridge_.setPlaying(true);
    for (std::size_t transaction = 0;
         transaction < expected.size();
         ++transaction)
    {
        std::array<float, blockFrames> left{};
        std::array<float, blockFrames> right{};
        std::array<float*, 2> channels{left.data(), right.data()};
        if (!projectBridge_.render(
                {},
                static_cast<double>(transaction * blockFrames) / sampleRate,
                channels.data(),
                channels.size(),
                blockFrames)
            || !blockEquals(left, expected[transaction])
            || !blockEquals(right, expected[transaction]))
        {
            return finish(
                "C57 opened voiced/unvoiced example did not publish exact public transition frame "
                + std::to_string(transaction));
        }
    }

    projectBridge_.setPlaying(false);
    if (!stageProject(settings, "C57 voiced/unvoiced explicit restage"))
    {
        return finish(
            "C57 voiced/unvoiced transition example did not accept restage");
    }
    projectBridge_.setPlaying(true);
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
        || !blockEquals(restagedLeft, 1.0F)
        || !blockEquals(restagedRight, 1.0F))
    {
        return finish(
            "C57 voiced/unvoiced restage did not restart at the exact voiced endpoint");
    }
    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    return runSpectrumInRamExampleSmoke();
}
}
