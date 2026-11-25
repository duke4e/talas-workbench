#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::runSpectrumOnDiskExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:spectrum-on-disk-frame-scan"};
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
                    "C56 Spectrum On Disk smoke restore")
                && error.empty())
            {
                error =
                    "C56 Spectrum On Disk smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
        {
            error =
                "C56 Spectrum On Disk smoke could not restore the prior project";
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
            "C56 packaged Spectrum On Disk frame-scan entry omitted open or audition authority");
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
            "C56 packaged Spectrum On Disk frame-scan entry could not be selected");
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
            "C56 packaged Spectrum On Disk frame-scan audition did not load its immutable table and render finite audible output");
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
        && opened.assets.front().id == "Analysis/C56"
        && opened.assets.front().kind
            == talas::project::ProjectAssetKind::Table
        && opened.assets.front().contentHash
            == "e5c6e5c0ed78294f2fda94de56775e2dfbabc06cc1eabc26a79d2a4248840e5e";
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Spectrum On Disk Frame Scan")
        || countType("talas.constant") != 1
        || countType("talas.spectrumOnDisk") != 1
        || countType("talas.oscillatorBank") != 1
        || countType("talas.audioOutput") != 1
        || !hasExactTableAsset)
    {
        return finish(
            "C56 Open Example action did not install the exact Spectrum On Disk frame-scan project and table identity");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "C56 packaged Spectrum On Disk frame-scan example"))
    {
        return finish(
            "C56 Spectrum On Disk frame-scan example did not stage: "
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
            "C56 opened Spectrum On Disk frame-scan example did not play finite audible output");
    }

    std::array<float, blockFrames> scannedLeft{};
    std::array<float, blockFrames> scannedRight{};
    std::array<float*, 2> scannedChannels{
        scannedLeft.data(), scannedRight.data()};
    if (!projectBridge_.render(
            {},
            static_cast<double>(blockFrames) / sampleRate,
            scannedChannels.data(),
            scannedChannels.size(),
            blockFrames)
        || !finiteAndAudible(scannedLeft)
        || !std::ranges::all_of(
            scannedRight,
            [](float sample) { return std::isfinite(sample); })
        || scannedLeft == firstLeft)
    {
        return finish(
            "C56 opened Spectrum On Disk frame-scan example did not advance its prepared scan transaction");
    }

    auto frozenDocument = opened;
    const auto frozenSource = std::ranges::find(
        frozenDocument.graph.nodes,
        std::string_view{"source"},
        &talas::project::ProjectNode::instanceId);
    if (frozenSource == frozenDocument.graph.nodes.end())
    {
        return finish(
            "C56 Spectrum On Disk frame-scan reference could not find its source node");
    }
    const auto frozenRate = std::ranges::find(
        frozenSource->parameters,
        std::string_view{"rateScale"},
        &talas::project::ProjectParameterExpression::parameterId);
    if (frozenRate == frozenSource->parameters.end())
    {
        return finish(
            "C56 Spectrum On Disk frame-scan reference could not find its rateScale parameter");
    }
    frozenRate->expression = "0";
    projectBridge_.setPlaying(false);
    if (!projectBridge_.editor().replaceDocument(frozenDocument).ok())
    {
        return finish(
            "C56 Spectrum On Disk frozen-frame reference could not replace the opened project");
    }
    documentChanged();
    if (!stageProject(settings, "C56 Spectrum On Disk frozen-frame reference"))
    {
        return finish(
            "C56 Spectrum On Disk frozen-frame reference did not stage");
    }
    std::array<float, blockFrames> frozenFirstLeft{};
    std::array<float, blockFrames> frozenFirstRight{};
    std::array<float*, 2> frozenFirstChannels{
        frozenFirstLeft.data(), frozenFirstRight.data()};
    std::array<float, blockFrames> frozenSecondLeft{};
    std::array<float, blockFrames> frozenSecondRight{};
    std::array<float*, 2> frozenSecondChannels{
        frozenSecondLeft.data(), frozenSecondRight.data()};
    projectBridge_.setPlaying(true);
    if (!projectBridge_.render(
            {},
            0.0,
            frozenFirstChannels.data(),
            frozenFirstChannels.size(),
            blockFrames)
        || !projectBridge_.render(
            {},
            static_cast<double>(blockFrames) / sampleRate,
            frozenSecondChannels.data(),
            frozenSecondChannels.size(),
            blockFrames)
        || frozenFirstLeft != firstLeft
        || frozenFirstRight != firstRight
        || (frozenSecondLeft == scannedLeft
            && frozenSecondRight == scannedRight))
    {
        return finish(
            "C56 opened Spectrum On Disk scan did not diverge from an exact frozen-frame oscillator-phase reference");
    }

    projectBridge_.setPlaying(false);
    if (!projectBridge_.editor().replaceDocument(opened).ok())
    {
        return finish(
            "C56 Spectrum On Disk frame-scan project could not be restored after its frozen-frame reference");
    }
    documentChanged();
    if (!stageProject(settings, "C56 Spectrum On Disk explicit restage"))
    {
        return finish(
            "C56 Spectrum On Disk frame-scan example did not accept restage");
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
        || firstLeft != restagedLeft
        || firstRight != restagedRight)
    {
        return finish(
            "C56 Spectrum On Disk frame-scan restage did not restart exact source state");
    }
    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    return runSpectrumOnDiskDefaultMetadataExampleSmoke();
}

std::optional<std::string>
WorkbenchComponent::runSpectrumOnDiskDefaultMetadataExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:spectrum-on-disk-default-analysis-metadata"};
    constexpr double sampleRate = 48'000.0;
    constexpr std::size_t blockFrames = 128;
    constexpr float firstAmplitude = 0.6F;
    constexpr float firstFrequency = 375.0F;
    constexpr float secondAmplitude = 1.2F;
    constexpr float secondFrequency = 437.5F;

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
                    "C60 Spectrum On Disk default-metadata smoke restore")
                && error.empty())
            {
                error =
                    "C60 Spectrum On Disk default-metadata smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
        {
            error =
                "C60 Spectrum On Disk default-metadata smoke could not restore the prior project";
        }
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    const auto blockEquals = [](const auto& block, float expected) {
        return std::ranges::all_of(
            block,
            [&](float sample) {
                return std::isfinite(sample) && sample == expected;
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
            "C60 packaged Spectrum On Disk default-metadata entry omitted open or audition authority");
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
            "C60 packaged Spectrum On Disk default-metadata entry could not be selected");
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
        || !blockEquals(auditionLeft, firstAmplitude)
        || !blockEquals(auditionRight, firstFrequency))
    {
        return finish(
            "C60 packaged Spectrum On Disk audition did not publish exact first default scan output");
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
        && opened.assets.front().id == "Analysis/C60"
        && opened.assets.front().kind
            == talas::project::ProjectAssetKind::Table
        && opened.assets.front().contentHash
            == "5313322d694f8c1553acdfb1ea0330a88abe562f3d675abc93c6a8526d2608b2";
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Spectrum On Disk Default Analysis Metadata")
        || countType("talas.spectrumOnDisk") != 1
        || countType("talas.spectrumTrackSelector") != 1
        || countType("talas.audioOutput") != 1
        || !hasExactTableAsset)
    {
        return finish(
            "C60 Open Example action did not install the exact default Spectrum On Disk project and table identity");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(
            settings,
            "C60 packaged Spectrum On Disk default-metadata example"))
    {
        return finish(
            "C60 Spectrum On Disk default-metadata example did not stage: "
            + statusLabel_.getText().toStdString());
    }
    const auto renderExact = [&] (float amplitude, float frequency) {
        std::array<float, blockFrames> left{};
        std::array<float, blockFrames> right{};
        std::array<float*, 2> channels{left.data(), right.data()};
        const auto rendered = projectBridge_.render(
            {},
            0.0,
            channels.data(),
            channels.size(),
            blockFrames);
        return rendered
            && blockEquals(left, amplitude)
            && blockEquals(right, frequency);
    };
    projectBridge_.setPlaying(true);
    if (!renderExact(firstAmplitude, firstFrequency)
        || !renderExact(secondAmplitude, secondFrequency))
    {
        return finish(
            "C60 opened Spectrum On Disk example did not publish both exact default scan transactions");
    }
    projectBridge_.setPlaying(false);
    if (!stageProject(
            settings,
            "C60 Spectrum On Disk default-metadata explicit restage"))
    {
        return finish(
            "C60 Spectrum On Disk default-metadata example did not accept restage");
    }
    projectBridge_.setPlaying(true);
    if (!renderExact(firstAmplitude, firstFrequency))
    {
        return finish(
            "C60 Spectrum On Disk restage did not restore exact default metadata and scan state");
    }
    if (auto error = finish())
        return error;
    return runSpectrumVoicedUnvoicedExampleSmoke();
}
}
