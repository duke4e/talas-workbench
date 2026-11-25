#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::runSampleExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:text-backed-sample-table"};
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
                    "C34 Sample smoke restore")
                && error.empty())
            {
                error = "C34 Sample smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error = "C34 Sample smoke could not restore the prior project";
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
            "C34 packaged Sample entry omitted open or audition authority");
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
        return finish("C34 packaged Sample entry could not be selected");

    startSoundLibraryAuditionWithProfile(
        std::string{stableId},
        sampleRate,
        blockFrames,
        deviceGeneration_.load(std::memory_order_acquire));
    if (auto error = waitForSoundLibraryAuditionSmoke())
        return finish(*error);
    std::array<float, blockFrames> audition{};
    std::array<float*, 1> auditionChannels{audition.data()};
    const auto auditionRendered = renderSoundLibraryAudition(
        0.0,
        auditionChannels.data(),
        auditionChannels.size(),
        blockFrames);
    if (soundLibraryAuditionActive_ == nullptr
        || soundLibraryAuditionActive_->entry.stableId != stableId
        || auditionRendered != std::optional<bool>{true}
        || !finiteAndAudible(audition))
    {
        return finish(
            "C34 packaged Sample audition did not load and render audible mono");
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
    const auto hasExactSample = opened.assets.size() == 1
        && opened.assets[0].id == "Samples/Hit"
        && opened.assets[0].kind == talas::project::ProjectAssetKind::Table
        && opened.assets[0].contentHash
            == "ce5449356b67e84f906f73a1743e5d8aebed474e3bc3feb60f0e3001f0f42e98";
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Text-Backed Sample Table")
        || countType("talas.sample") != 1
        || countType("talas.audioOutput") != 1
        || !hasExactSample)
    {
        return finish(
            "C34 Open Example action did not install exact Sample project authority");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "C34 packaged Sample example"))
    {
        return finish(
            "C34 Sample example did not stage: "
            + statusLabel_.getText().toStdString());
    }
    std::array<float, blockFrames> first{};
    std::array<float*, 1> firstChannels{first.data()};
    projectBridge_.setPlaying(true);
    if (!projectBridge_.render(
            {},
            0.0,
            firstChannels.data(),
            firstChannels.size(),
            blockFrames)
        || !finiteAndAudible(first))
    {
        return finish(
            "C34 opened Sample example did not play finite audible mono");
    }

    if (!stageProject(settings, "C34 Sample explicit restage"))
        return finish("C34 Sample example did not accept restage");
    std::array<float, blockFrames> restaged{};
    std::array<float*, 1> restagedChannels{restaged.data()};
    if (!projectBridge_.render(
            {},
            0.0,
            restagedChannels.data(),
            restagedChannels.size(),
            blockFrames)
        || first != restaged)
    {
        return finish(
            "C34 Sample restage did not restart exact playback state");
    }
    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    return runMultisampleExampleSmoke();
}

std::optional<std::string>
WorkbenchComponent::runMultisampleExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:bounded-multisample"};
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
                    "C35 Multisample smoke restore")
                && error.empty())
            {
                error =
                    "C35 Multisample smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error = "C35 Multisample smoke could not restore the prior project";
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
            "C35 packaged Multisample entry omitted open or audition authority");
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
        return finish("C35 packaged Multisample entry could not be selected");

    startSoundLibraryAuditionWithProfile(
        std::string{stableId},
        sampleRate,
        blockFrames,
        deviceGeneration_.load(std::memory_order_acquire));
    if (auto error = waitForSoundLibraryAuditionSmoke())
        return finish(*error);
    std::array<float, blockFrames> audition{};
    std::array<float*, 1> auditionChannels{audition.data()};
    const auto auditionRendered = renderSoundLibraryAudition(
        0.0,
        auditionChannels.data(),
        auditionChannels.size(),
        blockFrames);
    if (soundLibraryAuditionActive_ == nullptr
        || soundLibraryAuditionActive_->entry.stableId != stableId
        || auditionRendered != std::optional<bool>{true}
        || !finiteAndAudible(audition))
    {
        return finish(
            "C35 packaged Multisample audition did not load and render audible mono");
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
    const auto hasExactAssets = opened.assets.size() == 2
        && opened.assets[0].id == "Samples/C35Hit"
        && opened.assets[0].kind == talas::project::ProjectAssetKind::Table
        && opened.assets[0].contentHash
            == "ce5449356b67e84f906f73a1743e5d8aebed474e3bc3feb60f0e3001f0f42e98"
        && opened.assets[1].id == "Samples/C35Loop"
        && opened.assets[1].kind == talas::project::ProjectAssetKind::Table
        && opened.assets[1].contentHash
            == "54e58fa19e33c65f013a9758f497efe8481f684876756f6ab60e79ef6865dcd3";
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Bounded Multisample")
        || countType("talas.multisample") != 1
        || countType("talas.audioOutput") != 1
        || !hasExactAssets)
    {
        return finish(
            "C35 Open Example action did not install exact Multisample project authority");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "C35 packaged Multisample example"))
    {
        return finish(
            "C35 Multisample example did not stage: "
            + statusLabel_.getText().toStdString());
    }
    std::array<float, blockFrames> first{};
    std::array<float*, 1> firstChannels{first.data()};
    projectBridge_.setPlaying(true);
    if (!projectBridge_.render(
            {},
            0.0,
            firstChannels.data(),
            firstChannels.size(),
            blockFrames)
        || !finiteAndAudible(first))
    {
        return finish(
            "C35 opened Multisample example did not play finite audible mono");
    }

    if (!stageProject(settings, "C35 Multisample explicit restage"))
        return finish("C35 Multisample example did not accept restage");
    std::array<float, blockFrames> restaged{};
    std::array<float*, 1> restagedChannels{restaged.data()};
    if (!projectBridge_.render(
            {},
            0.0,
            restagedChannels.data(),
            restagedChannels.size(),
            blockFrames)
        || first != restaged)
    {
        return finish(
            "C35 Multisample restage did not restart exact playback state");
    }
    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    return runKeyMappedMultisampleExampleSmoke();
}

std::optional<std::string>
WorkbenchComponent::runKeyMappedMultisampleExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:bounded-key-mapped-multisample"};
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
                    "C36 Key-Mapped Multisample smoke restore")
                && error.empty())
            {
                error =
                    "C36 Key-Mapped Multisample smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
        {
            error =
                "C36 Key-Mapped Multisample smoke could not restore the prior project";
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
            "C36 packaged Key-Mapped Multisample entry omitted open or audition authority");
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
            "C36 packaged Key-Mapped Multisample entry could not be selected");
    }

    startSoundLibraryAuditionWithProfile(
        std::string{stableId},
        sampleRate,
        blockFrames,
        deviceGeneration_.load(std::memory_order_acquire));
    if (auto error = waitForSoundLibraryAuditionSmoke())
        return finish(*error);
    std::array<float, blockFrames> audition{};
    std::array<float*, 1> auditionChannels{audition.data()};
    const auto auditionRendered = renderSoundLibraryAudition(
        0.0,
        auditionChannels.data(),
        auditionChannels.size(),
        blockFrames);
    if (soundLibraryAuditionActive_ == nullptr
        || soundLibraryAuditionActive_->entry.stableId != stableId
        || auditionRendered != std::optional<bool>{true}
        || !finiteAndAudible(audition))
    {
        return finish(
            "C36 packaged Key-Mapped Multisample audition did not load and render audible mono");
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
    const auto hasExactAsset = opened.assets.size() == 1
        && opened.assets[0].id == "Samples/C36Root"
        && opened.assets[0].kind == talas::project::ProjectAssetKind::Table
        && opened.assets[0].contentHash
            == "54e58fa19e33c65f013a9758f497efe8481f684876756f6ab60e79ef6865dcd3";
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Bounded Key-Mapped Multisample")
        || countType("talas.keyMappedMultisample") != 1
        || countType("talas.audioOutput") != 1
        || !hasExactAsset)
    {
        return finish(
            "C36 Open Example action did not install exact Key-Mapped Multisample authority");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(
            settings, "C36 packaged Key-Mapped Multisample example"))
    {
        return finish(
            "C36 Key-Mapped Multisample example did not stage: "
            + statusLabel_.getText().toStdString());
    }
    std::array<float, blockFrames> first{};
    std::array<float*, 1> firstChannels{first.data()};
    projectBridge_.setPlaying(true);
    if (!projectBridge_.render(
            {},
            0.0,
            firstChannels.data(),
            firstChannels.size(),
            blockFrames)
        || !finiteAndAudible(first))
    {
        return finish(
            "C36 opened Key-Mapped Multisample example did not play finite audible mono");
    }

    if (!stageProject(
            settings, "C36 Key-Mapped Multisample explicit restage"))
    {
        return finish(
            "C36 Key-Mapped Multisample example did not accept restage");
    }
    std::array<float, blockFrames> restaged{};
    std::array<float*, 1> restagedChannels{restaged.data()};
    if (!projectBridge_.render(
            {},
            0.0,
            restagedChannels.data(),
            restagedChannels.size(),
            blockFrames)
        || first != restaged)
    {
        return finish(
            "C36 Key-Mapped Multisample restage did not restart exact playback state");
    }
    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    return runBoundedRawMidiOutputExampleSmoke();
}
}
