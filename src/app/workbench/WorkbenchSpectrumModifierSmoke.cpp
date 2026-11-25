#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::runSpectrumModifierExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:bounded-spectrum-modifier"};
    constexpr std::string_view remainderStableId{
        "example:spectrum-modifier-remainder-probability-profile"};
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
                    "C27 Spectrum Modifier smoke restore")
                && error.empty())
            {
                error = "C27 Spectrum Modifier smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error = "C27 Spectrum Modifier smoke could not restore the prior project";
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
            "C27 packaged Spectrum Modifier entry omitted open or audition authority");
    }

    const auto selectPackagedEntry = [&](std::string_view selectedStableId) {
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
        state.selectedEntryId = std::string{selectedStableId};
        workspaceSession_.soundLibrary = state;
        soundDefinitionsPanel_->setLibraryState(state);
    };
    selectPackagedEntry(stableId);
    if (soundDefinitionsPanel_->selectedEntryId() != stableId)
    {
        return finish(
            "C27 packaged Spectrum Modifier entry could not be selected in the Sound Library");
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
        || !std::ranges::all_of(
            auditionRight,
            [](float sample) { return std::isfinite(sample); }))
    {
        return finish(
            "C27 packaged Spectrum Modifier audition did not render audible finite mono output");
    }
    stopSoundLibraryAudition();

    selectPackagedEntry(stableId);
    openSoundLibraryExampleNow(std::string{stableId});
    const auto& opened = projectBridge_.editor().document();
    const auto modifierCount = std::ranges::count(
        opened.graph.nodes,
        std::string_view{"talas.spectrumModifier"},
        &talas::project::ProjectNode::typeId);
    const auto spectrumCount = std::ranges::count(
        opened.graph.nodes,
        std::string_view{"talas.syntheticSpectrumFromArray"},
        &talas::project::ProjectNode::typeId);
    const auto bankCount = std::ranges::count(
        opened.graph.nodes,
        std::string_view{"talas.oscillatorBank"},
        &talas::project::ProjectNode::typeId);
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Bounded Spectrum Modifier")
        || modifierCount != 1
        || spectrumCount != 1
        || bankCount != 1
        || !opened.assets.empty())
    {
        return finish(
            "C27 packaged Spectrum Modifier Open Example action did not install its exact untitled project");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(
            settings,
            "C27 packaged Spectrum Modifier example"))
    {
        return finish(
            "C27 packaged Spectrum Modifier example did not stage: "
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
            "C27 opened Spectrum Modifier example did not adopt and play finite mono output");
    }

    if (!stageProject(
            settings,
            "C27 Spectrum Modifier explicit restage"))
    {
        return finish(
            "C27 opened Spectrum Modifier example did not accept an exact restage");
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
            "C27 Spectrum Modifier restage did not replace and restart exact oscillator-bank state");
    }

    refreshSoundLibraryProjection();
    const auto* remainderEntry = soundLibraryEntry(remainderStableId);
    if (remainderEntry == nullptr
        || remainderEntry->sourceKind
            != talas::app::SoundLibrarySourceKind::OriginalExample
        || !talas::app::soundLibraryHasAction(
            remainderEntry->actions,
            talas::app::SoundLibraryAction::OpenExample)
        || !talas::app::soundLibraryHasAction(
            remainderEntry->actions,
            talas::app::SoundLibraryAction::Audition))
    {
        return finish(
            "C55 packaged remainder-profile entry omitted open or audition authority");
    }

    selectPackagedEntry(remainderStableId);
    if (soundDefinitionsPanel_->selectedEntryId() != remainderStableId)
    {
        return finish(
            "C55 packaged remainder-profile entry could not be selected in the Sound Library");
    }
    startSoundLibraryAuditionWithProfile(
        std::string{remainderStableId},
        sampleRate,
        blockFrames,
        deviceGeneration_.load(std::memory_order_acquire));
    if (auto error = waitForSoundLibraryAuditionSmoke())
        return finish(*error);

    std::array<float, blockFrames> remainderAuditionLeft{};
    std::array<float, blockFrames> remainderAuditionRight{};
    std::array<float*, 2> remainderAuditionChannels{
        remainderAuditionLeft.data(), remainderAuditionRight.data()};
    const auto remainderAuditionRendered = renderSoundLibraryAudition(
        0.0,
        remainderAuditionChannels.data(),
        remainderAuditionChannels.size(),
        blockFrames);
    if (soundLibraryAuditionActive_ == nullptr
        || soundLibraryAuditionActive_->entry.stableId
            != remainderStableId
        || remainderAuditionRendered != std::optional<bool>{true}
        || !finiteAndAudible(remainderAuditionLeft)
        || !std::ranges::all_of(
            remainderAuditionRight,
            [](float sample) { return std::isfinite(sample); }))
    {
        return finish(
            "C55 packaged remainder-profile audition did not render audible finite mono output");
    }
    stopSoundLibraryAudition();

    selectPackagedEntry(remainderStableId);
    openSoundLibraryExampleNow(std::string{remainderStableId});
    const auto& remainderOpened = projectBridge_.editor().document();
    const auto remainderModifier = std::ranges::find(
        remainderOpened.graph.nodes,
        std::string_view{"talas.spectrumModifier"},
        &talas::project::ProjectNode::typeId);
    const auto probability = remainderModifier
            == remainderOpened.graph.nodes.end()
        ? std::vector<talas::project::ProjectParameterExpression>::const_iterator{}
        : std::ranges::find(
            remainderModifier->parameters,
            std::string_view{"probability"},
            &talas::project::ProjectParameterExpression::parameterId);
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Spectrum Modifier Remainder Probability Profile")
        || remainderOpened.expressionLanguage
            != talas::expr::ExpressionLanguageVersion::VariablesV2
        || remainderModifier == remainderOpened.graph.nodes.end()
        || probability == remainderModifier->parameters.end()
        || probability->expression
            != std::optional<std::string>{
                "(TrackNumber - 1) rem: 2"})
    {
        return finish(
            "C55 packaged remainder-profile Open Example action did not install exact VariablesV2 bytes");
    }

    if (!stageProject(
            settings,
            "C55 packaged remainder-profile example"))
    {
        return finish(
            "C55 packaged remainder-profile example did not stage: "
            + statusLabel_.getText().toStdString());
    }
    std::array<float, blockFrames> remainderLeft{};
    std::array<float, blockFrames> remainderRight{};
    std::array<float*, 2> remainderChannels{
        remainderLeft.data(), remainderRight.data()};
    projectBridge_.setPlaying(true);
    if (!projectBridge_.render(
            {},
            0.0,
            remainderChannels.data(),
            remainderChannels.size(),
            blockFrames)
        || !finiteAndAudible(remainderLeft)
        || !std::ranges::all_of(
            remainderRight,
            [](float sample) { return std::isfinite(sample); }))
    {
        return finish(
            "C55 opened remainder-profile example did not adopt and play finite audible output");
    }

    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    return runPairedStereoOutputExampleSmoke();
}
}
