#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <array>

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::runSpectrumInRamExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:spectrum-in-ram-normalized-time-index"};
    constexpr double sampleRate = 48'000.0;
    constexpr std::size_t blockFrames = 128;
    struct Endpoint
    {
        double timeIndex;
        float amplitude;
        float frequency;
    };
    constexpr std::array endpoints{
        Endpoint{-1.0, 0.4F, 100.0F},
        Endpoint{0.0, 0.6F, 300.0F},
        Endpoint{1.0, 0.8F, 500.0F},
    };

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
                    "C58 SpectrumInRAM smoke restore")
                && error.empty())
            {
                error =
                    "C58 SpectrumInRAM smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
        {
            error =
                "C58 SpectrumInRAM smoke could not restore the prior project";
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
            "C58 packaged SpectrumInRAM entry omitted open or audition authority");
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
            "C58 packaged SpectrumInRAM entry could not be selected");
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
        || !blockEquals(auditionLeft, endpoints[0].amplitude)
        || !blockEquals(auditionRight, endpoints[0].frequency))
    {
        return finish(
            "C58 packaged SpectrumInRAM audition did not publish the exact -1 endpoint");
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
        && opened.assets.front().id == "Analysis/C58"
        && opened.assets.front().kind
            == talas::project::ProjectAssetKind::Table
        && opened.assets.front().contentHash
            == "094edff186973a2365770c4ca8975d0ed994804cc4e9a96c61c673c15228a1a5";
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Spectrum In RAM Normalized TimeIndex")
        || countType("talas.constant") != 1
        || countType("talas.spectrumInRam") != 1
        || countType("talas.spectrumTrackSelector") != 1
        || countType("talas.audioOutput") != 1
        || !hasExactTableAsset)
    {
        return finish(
            "C58 Open Example action did not install the exact normalized SpectrumInRAM project and table identity");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "C58 packaged normalized SpectrumInRAM example"))
    {
        return finish(
            "C58 normalized SpectrumInRAM example did not stage: "
            + statusLabel_.getText().toStdString());
    }
    projectBridge_.setPlaying(true);
    for (const auto endpoint : endpoints)
    {
        std::array<float, blockFrames> left{};
        std::array<float, blockFrames> right{};
        std::array<float*, 2> channels{left.data(), right.data()};
        if (!projectBridge_.updateEventValue(
                "C58TimeIndex",
                endpoint.timeIndex,
                talas::expr::ValueDomain::Scalar)
            || !projectBridge_.render(
                {},
                0.0,
                channels.data(),
                channels.size(),
                blockFrames)
            || !blockEquals(left, endpoint.amplitude)
            || !blockEquals(right, endpoint.frequency))
        {
            return finish(
                "C58 opened SpectrumInRAM example did not publish exact normalized endpoint "
                + std::to_string(endpoint.timeIndex));
        }
    }

    projectBridge_.setPlaying(false);
    if (!stageProject(settings, "C58 SpectrumInRAM explicit restage"))
    {
        return finish(
            "C58 normalized SpectrumInRAM example did not accept restage");
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
        || !blockEquals(restagedLeft, endpoints[0].amplitude)
        || !blockEquals(restagedRight, endpoints[0].frequency))
    {
        return finish(
            "C58 SpectrumInRAM restage did not restore the exact -1 endpoint");
    }
    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    return runSpectrumInRamDefaultMetadataExampleSmoke();
}

std::optional<std::string>
WorkbenchComponent::runSpectrumInRamDefaultMetadataExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:spectrum-in-ram-default-analysis-metadata"};
    constexpr double sampleRate = 48'000.0;
    constexpr std::size_t blockFrames = 128;
    constexpr float expectedAmplitude = 0.6F;
    constexpr float expectedFrequency = 375.0F;

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
                    "C59 SpectrumInRAM smoke restore")
                && error.empty())
            {
                error =
                    "C59 SpectrumInRAM smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
        {
            error =
                "C59 SpectrumInRAM smoke could not restore the prior project";
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
            "C59 packaged SpectrumInRAM entry omitted open or audition authority");
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
            "C59 packaged SpectrumInRAM entry could not be selected");
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
        || !blockEquals(auditionLeft, expectedAmplitude)
        || !blockEquals(auditionRight, expectedFrequency))
    {
        return finish(
            "C59 packaged SpectrumInRAM audition did not publish the exact default metadata result");
    }
    stopSoundLibraryAudition();

    selectPackagedEntry();
    openSoundLibraryExampleNow(std::string{stableId});
    const auto opened = projectBridge_.editor().document();
    const auto source = std::ranges::find(
        opened.graph.nodes,
        std::string_view{"source"},
        &talas::project::ProjectNode::instanceId);
    const auto hasExactSource = source != opened.graph.nodes.end()
        && source->typeId == "talas.spectrumInRam"
        // The example's format 35 keeps its pairs in hertz (format 93).
        && source->parameters.size() == 3
        && std::ranges::any_of(source->parameters, [](const auto& parameter) {
               return parameter.parameterId == "pairFrequencies"
                   && parameter.choice == std::optional<std::string>{"legacyHertz"};
           })
        && std::ranges::find(
            source->parameters,
            std::string_view{"analysis"},
            &talas::project::ProjectParameterExpression::parameterId)
            != source->parameters.end()
        && std::ranges::find(
            source->parameters,
            std::string_view{"analysisFrequency"},
            &talas::project::ProjectParameterExpression::parameterId)
            != source->parameters.end();
    const auto hasExactTableAsset = opened.assets.size() == 1
        && opened.assets.front().id == "Analysis/C59"
        && opened.assets.front().kind
            == talas::project::ProjectAssetKind::Table
        && opened.assets.front().contentHash
            == "33229df355e62d037191851a5467322e95f22d884f5c25a03de264d004901139";
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Spectrum In RAM Default Analysis Metadata")
        || !hasExactSource
        || !hasExactTableAsset)
    {
        return finish(
            "C59 Open Example action did not install the exact minimal SpectrumInRAM project and table identity");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "C59 packaged default SpectrumInRAM example"))
    {
        return finish(
            "C59 default SpectrumInRAM example did not stage: "
            + statusLabel_.getText().toStdString());
    }
    const auto renderExact = [&] {
        projectBridge_.setPlaying(true);
        std::array<float, blockFrames> left{};
        std::array<float, blockFrames> right{};
        std::array<float*, 2> channels{left.data(), right.data()};
        const auto rendered = projectBridge_.render(
            {},
            0.0,
            channels.data(),
            channels.size(),
            blockFrames);
        projectBridge_.setPlaying(false);
        return rendered
            && blockEquals(left, expectedAmplitude)
            && blockEquals(right, expectedFrequency);
    };
    if (!renderExact())
    {
        return finish(
            "C59 opened SpectrumInRAM example did not publish exact default metadata output");
    }
    if (!stageProject(settings, "C59 SpectrumInRAM explicit restage")
        || !renderExact())
    {
        return finish(
            "C59 SpectrumInRAM restage did not preserve the exact default metadata output");
    }
    if (auto error = finish())
        return error;
    return runGaOscillatorsExampleSmoke();
}
}
