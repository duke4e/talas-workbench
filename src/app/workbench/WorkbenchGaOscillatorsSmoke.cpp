#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::runGaOscillatorsExampleSmoke()
{
    constexpr std::string_view stableId{"example:bounded-ga-oscillators"};
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
                    "C32 GA Oscillators smoke restore")
                && error.empty())
            {
                error =
                    "C32 GA Oscillators smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
        {
            error =
                "C32 GA Oscillators smoke could not restore the prior project";
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
            "C32 packaged GA Oscillators entry omitted open or audition authority");
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
            "C32 packaged GA Oscillators entry could not be selected");
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
            "C32 packaged GA Oscillators audition did not load both immutable tables and render finite audible output");
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
    const auto hasExactTables = opened.assets.size() == 2
        && opened.assets[0].id == "Analysis/C32A"
        && opened.assets[0].kind == talas::project::ProjectAssetKind::Table
        && opened.assets[0].contentHash
            == "2b16b0eaac7e79df0230adea1f7ccb8dc430a233ceb5b60b62b618d76dc24ae7"
        && opened.assets[1].id == "Analysis/C32B"
        && opened.assets[1].kind == talas::project::ProjectAssetKind::Table
        && opened.assets[1].contentHash
            == "b6e94e9f13c483a94830697f0bc48bc1cffd1f6f048cde75c0bb6d84f5900bf8";
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Bounded GA Oscillators")
        || countType("talas.gaOscillators") != 1
        || countType("talas.audioOutput") != 1
        || !hasExactTables)
    {
        return finish(
            "C32 Open Example action did not install the exact GA Oscillators project and table identities");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "C32 packaged GA Oscillators example"))
    {
        return finish(
            "C32 GA Oscillators example did not stage: "
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
            "C32 opened GA Oscillators example did not play finite audible output");
    }

    if (!stageProject(settings, "C32 GA Oscillators explicit restage"))
    {
        return finish(
            "C32 GA Oscillators example did not accept restage");
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
            "C32 GA Oscillators restage did not restart exact source state");
    }
    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    return runSumOfSinesDefaultAnalysisFrequenciesExampleSmoke();
}

std::optional<std::string>
WorkbenchComponent::runSumOfSinesDefaultAnalysisFrequenciesExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:sum-of-sines-default-analysis-frequencies"};
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
                    "C61 SumOfSines smoke restore")
                && error.empty())
            {
                error =
                    "C61 SumOfSines smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error = "C61 SumOfSines smoke could not restore the prior project";
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
            "C61 packaged SumOfSines entry omitted open or audition authority");
    }

    const auto selectPackagedEntry = [&] {
        auto state = soundDefinitionsPanel_->libraryState();
        state.collection = talas::project::WorkspaceSoundLibraryCollection::All;
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
        return finish("C61 packaged SumOfSines entry could not be selected");

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
            "C61 packaged SumOfSines audition did not render finite audible output");
    }
    stopSoundLibraryAudition();

    selectPackagedEntry();
    openSoundLibraryExampleNow(std::string{stableId});
    const auto& opened = projectBridge_.editor().document();
    const auto source = std::ranges::find(
        opened.graph.nodes,
        std::string_view{"talas.sumOfSines"},
        &talas::project::ProjectNode::typeId);
    const auto frequencyOmitted = [&](std::string_view id) {
        return source != opened.graph.nodes.end()
            && std::ranges::none_of(
                source->parameters,
                [&](const auto& parameter) {
                    return parameter.parameterId == id;
                });
    };
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Sum Of Sines Default Analysis Frequencies")
        || source == opened.graph.nodes.end()
        || opened.assets.size() != 1
        || opened.assets.front().id != "Analysis/C61"
        || opened.assets.front().contentHash
            != "4355a46b19d348dc2f57c046f8ef63d4538ebb936000f3c9ee954a27460dd865"
        || !frequencyOmitted("frequency0")
        || !frequencyOmitted("frequency1"))
    {
        return finish(
            "C61 Open Example action did not install the exact sentinel project and table identity");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "C61 packaged SumOfSines example"))
    {
        return finish(
            "C61 SumOfSines example did not stage: "
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
        || firstLeft.front() != 0.0F)
    {
        return finish(
            "C61 opened SumOfSines example did not start its finite 2 Hz phase sequence");
    }

    if (!stageProject(settings, "C61 SumOfSines explicit restage"))
        return finish("C61 SumOfSines example did not accept restage");
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
            "C61 SumOfSines restage did not restart exact resolved-frequency state");
    }
    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    return runSumOfSinesControlledTimeFramesExampleSmoke();
}

std::optional<std::string>
WorkbenchComponent::runSumOfSinesControlledTimeFramesExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:sum-of-sines-controlled-time-frames"};
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
                    "C62 SumOfSines smoke restore")
                && error.empty())
            {
                error =
                    "C62 SumOfSines smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error = "C62 SumOfSines smoke could not restore the prior project";
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
            "C62 packaged SumOfSines entry omitted open or audition authority");
    }

    auto state = soundDefinitionsPanel_->libraryState();
    state.collection = talas::project::WorkspaceSoundLibraryCollection::All;
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
    if (soundDefinitionsPanel_->selectedEntryId() != stableId)
        return finish("C62 packaged SumOfSines entry could not be selected");

    startSoundLibraryAuditionWithProfile(
        std::string{stableId}, sampleRate, blockFrames,
        deviceGeneration_.load(std::memory_order_acquire));
    if (auto error = waitForSoundLibraryAuditionSmoke())
        return finish(*error);
    std::array<float, blockFrames> auditionLeft{};
    std::array<float, blockFrames> auditionRight{};
    std::array<float*, 2> auditionChannels{
        auditionLeft.data(), auditionRight.data()};
    const auto auditionRendered = renderSoundLibraryAudition(
        0.0, auditionChannels.data(), auditionChannels.size(), blockFrames);
    if (soundLibraryAuditionActive_ == nullptr
        || soundLibraryAuditionActive_->entry.stableId != stableId
        || auditionRendered != std::optional<bool>{true}
        || !finiteAndAudible(auditionLeft))
    {
        return finish(
            "C62 packaged SumOfSines audition did not render controlled frames");
    }
    stopSoundLibraryAudition();

    state.selectedEntryId = std::string{stableId};
    workspaceSession_.soundLibrary = state;
    soundDefinitionsPanel_->setLibraryState(state);
    openSoundLibraryExampleNow(std::string{stableId});
    const auto& opened = projectBridge_.editor().document();
    const auto source = std::ranges::find(
        opened.graph.nodes,
        std::string_view{"talas.sumOfSines"},
        &talas::project::ProjectNode::typeId);
    const auto parameter = [&](std::string_view id) {
        return source == opened.graph.nodes.end()
            ? opened.graph.nodes.front().parameters.end()
            : std::ranges::find(
                source->parameters, id,
                &talas::project::ProjectParameterExpression::parameterId);
    };
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Sum Of Sines Controlled-Time Frames")
        || source == opened.graph.nodes.end()
        || opened.assets.size() != 2
        || parameter("analysisFrames0") == source->parameters.end()
        || parameter("ctrlTime") == source->parameters.end())
    {
        return finish(
            "C62 Open Example action did not install exact controlled-time authority");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "C62 packaged SumOfSines example"))
    {
        return finish(
            "C62 SumOfSines example did not stage: "
            + statusLabel_.getText().toStdString());
    }
    std::array<float, blockFrames> firstLeft{};
    std::array<float, blockFrames> firstRight{};
    std::array<float*, 2> firstChannels{
        firstLeft.data(), firstRight.data()};
    projectBridge_.setPlaying(true);
    if (!projectBridge_.render(
            {}, 0.0, firstChannels.data(), firstChannels.size(), blockFrames)
        || !finiteAndAudible(firstLeft)
        || firstLeft.front() != 0.0F)
    {
        return finish(
            "C62 opened SumOfSines example did not scan controlled frames");
    }

    if (!stageProject(settings, "C62 SumOfSines explicit restage"))
        return finish("C62 SumOfSines example did not accept restage");
    std::array<float, blockFrames> restagedLeft{};
    std::array<float, blockFrames> restagedRight{};
    std::array<float*, 2> restagedChannels{
        restagedLeft.data(), restagedRight.data()};
    if (!projectBridge_.render(
            {}, 0.0, restagedChannels.data(), restagedChannels.size(),
            blockFrames)
        || firstLeft != restagedLeft
        || firstRight != restagedRight)
    {
        return finish(
            "C62 SumOfSines restage did not restart controlled-time state");
    }
    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    return runSumOfSinesAutomaticGateLoopExampleSmoke();
}

std::optional<std::string>
WorkbenchComponent::runSumOfSinesAutomaticGateLoopExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:sum-of-sines-automatic-gate-loop"};
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
                    "C63 SumOfSines smoke restore")
                && error.empty())
            {
                error =
                    "C63 SumOfSines smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error = "C63 SumOfSines smoke could not restore the prior project";
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
            "C63 packaged SumOfSines entry omitted open or audition authority");
    }

    auto state = soundDefinitionsPanel_->libraryState();
    state.collection = talas::project::WorkspaceSoundLibraryCollection::All;
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
    if (soundDefinitionsPanel_->selectedEntryId() != stableId)
        return finish("C63 packaged SumOfSines entry could not be selected");

    startSoundLibraryAuditionWithProfile(
        std::string{stableId}, sampleRate, blockFrames,
        deviceGeneration_.load(std::memory_order_acquire));
    if (auto error = waitForSoundLibraryAuditionSmoke())
        return finish(*error);
    std::array<float, blockFrames> auditionLeft{};
    std::array<float, blockFrames> auditionRight{};
    std::array<float*, 2> auditionChannels{
        auditionLeft.data(), auditionRight.data()};
    const auto auditionRendered = renderSoundLibraryAudition(
        0.0, auditionChannels.data(), auditionChannels.size(), blockFrames);
    if (soundLibraryAuditionActive_ == nullptr
        || soundLibraryAuditionActive_->entry.stableId != stableId
        || auditionRendered != std::optional<bool>{true}
        || !finiteAndAudible(auditionLeft))
    {
        return finish(
            "C63 packaged SumOfSines audition did not render automatic traversal");
    }
    stopSoundLibraryAudition();

    state.selectedEntryId = std::string{stableId};
    workspaceSession_.soundLibrary = state;
    soundDefinitionsPanel_->setLibraryState(state);
    openSoundLibraryExampleNow(std::string{stableId});
    const auto& opened = projectBridge_.editor().document();
    const auto source = std::ranges::find(
        opened.graph.nodes,
        std::string_view{"talas.sumOfSines"},
        &talas::project::ProjectNode::typeId);
    const auto parameter = [&](std::string_view id) {
        return source == opened.graph.nodes.end()
            ? opened.graph.nodes.front().parameters.end()
            : std::ranges::find(
                source->parameters, id,
                &talas::project::ProjectParameterExpression::parameterId);
    };
    const auto loop = parameter("loop");
    const auto ctrlTime = parameter("ctrlTime");
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Sum Of Sines Automatic Gate Loop")
        || source == opened.graph.nodes.end()
        || opened.assets.size() != 2
        || loop == source->parameters.end()
        || !loop->boolean.value_or(false)
        || ctrlTime == source->parameters.end()
        || ctrlTime->boolean.value_or(true))
    {
        return finish(
            "C63 Open Example action did not install exact automatic loop authority");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "C63 packaged SumOfSines example"))
    {
        return finish(
            "C63 SumOfSines example did not stage: "
            + statusLabel_.getText().toStdString());
    }
    std::array<float, blockFrames> firstLeft{};
    std::array<float, blockFrames> firstRight{};
    std::array<float*, 2> firstChannels{
        firstLeft.data(), firstRight.data()};
    projectBridge_.setPlaying(true);
    if (!projectBridge_.render(
            {}, 0.0, firstChannels.data(), firstChannels.size(), blockFrames)
        || !finiteAndAudible(firstLeft)
        || firstLeft.front() != 0.0F)
    {
        return finish(
            "C63 opened SumOfSines example did not traverse automatic Gate/Loop state");
    }

    if (!stageProject(settings, "C63 SumOfSines explicit restage"))
        return finish("C63 SumOfSines example did not accept restage");
    std::array<float, blockFrames> restagedLeft{};
    std::array<float, blockFrames> restagedRight{};
    std::array<float*, 2> restagedChannels{
        restagedLeft.data(), restagedRight.data()};
    if (!projectBridge_.render(
            {}, 0.0, restagedChannels.data(), restagedChannels.size(),
            blockFrames)
        || firstLeft != restagedLeft
        || firstRight != restagedRight)
    {
        return finish(
            "C63 SumOfSines restage did not restart cursor and Gate state");
    }
    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    return runGenericSourceExampleSmoke();
}
}
