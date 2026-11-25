#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace talas::app::workbench
{
// The demos in the Sound Library: one Demos group, effects then
// instruments; an effect explains that it needs live input instead of
// auditioning silence; an instrument auditions its demo phrase; both open
// as untitled projects, and the opened instrument stages and plays on the
// Workbench's bridge, as does a project opened after it whose !Cutoff is in
// hertz where the demo's is scalar.
std::optional<std::string> WorkbenchComponent::runDemoLibrarySmoke(
    const std::filesystem::path& smokeDirectory)
{
    if (soundLibraryRoot_.empty())
        return std::nullopt;
    constexpr std::string_view effectId{"example:demo-stereo-delay"};
    constexpr std::string_view instrumentId{"example:demo-sub-bass"};
    constexpr double sampleRate = 48'000.0;
    constexpr std::size_t blockFrames = 128;
    // Two seconds of blocks: long enough for any demo phrase to sound.
    constexpr int maximumBlocks = 750;

    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto previousDocumentStaged = currentDocumentStaged_;
    const auto previousTransportPlaying = transportPlaying_;
    const auto previousLibraryState = workspaceSession_.soundLibrary;
    const auto contextHelpWasVisible =
        workspaceSession_.accessibility.contextHelpVisible;
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        stopSoundLibraryAudition();
        projectBridge_.setPlaying(false);
        const auto restored =
            projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(
            previousCleanDocument, previousProjectPath);
        documentChanged();
        workspaceSession_.accessibility.contextHelpVisible =
            contextHelpWasVisible;
        applyAccessibilityPreferences();
        resized();
        workspaceSession_.soundLibrary = previousLibraryState;
        soundDefinitionsPanel_->setLibraryState(previousLibraryState);
        if (previousDocumentStaged)
        {
            talas::runtime::RenderSettings restoreSettings{
                sampleRate, blockFrames};
            if (!stageProject(
                    std::move(restoreSettings), "Demo library smoke restore")
                && error.empty())
            {
                error = "Demo library smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error = "Demo library smoke could not restore the prior project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    const auto showDemos = [&](std::optional<std::string_view> selected) {
        auto state = previousLibraryState;
        state.collection = talas::project::WorkspaceSoundLibraryCollection::All;
        state.sourceFilter =
            talas::project::WorkspaceSoundLibrarySourceFilter::All;
        state.compatibilityFilter =
            talas::project::WorkspaceSoundLibraryCompatibilityFilter::All;
        state.healthFilter = talas::project::WorkspaceSoundLibraryHealthFilter::All;
        state.category = std::string{talas::app::soundLibraryDemoCategory};
        state.origin.reset();
        state.inputSubstitutions.clear();
        state.selectedEntryId = selected.has_value()
            ? std::optional<std::string>{std::string{*selected}}
            : std::nullopt;
        workspaceSession_.soundLibrary = state;
        soundDefinitionsPanel_->setLibraryState(state);
    };
    const auto audible = [](const auto& samples) {
        return std::ranges::any_of(
            samples, [](float sample) { return std::abs(sample) > 1.0e-6F; });
    };
    const auto finite = [](const auto& samples) {
        return std::ranges::all_of(
            samples, [](float sample) { return std::isfinite(sample); });
    };

    // Listed: twenty-four demos in the Demos category, effects first.
    refreshSoundLibraryProjection();
    std::vector<std::string> demoIds;
    std::size_t effects = 0;
    for (const auto& entry : soundLibraryEntries_)
    {
        if (entry.demoKind == talas::app::SoundLibraryDemoKind::None)
            continue;
        if (entry.sourceKind
                != talas::app::SoundLibrarySourceKind::OriginalExample
            || entry.category != talas::app::soundLibraryDemoCategory
            || !entry.projectPath.has_value()
            || !entry.projectPath->starts_with(
                talas::app::soundLibraryDemoFolder)
            || !talas::app::soundLibraryHasAction(
                entry.actions, talas::app::SoundLibraryAction::OpenExample))
        {
            return finish("Demo " + entry.stableId
                + " is not an openable Demos entry from examples/demos/");
        }
        if (entry.demoKind == talas::app::SoundLibraryDemoKind::Effect)
            ++effects;
        demoIds.push_back(entry.stableId);
    }
    if (demoIds.size() != 24 || effects != 13)
    {
        return finish("The Sound Library listed "
            + std::to_string(demoIds.size()) + " demos ("
            + std::to_string(effects)
            + " effects) instead of 13 effects and 11 instruments");
    }

    // Grouped: the Demos category leads the filter and shows exactly them.
    const auto categories = soundDefinitionsPanel_->categoryChoicesForSmoke();
    if (categories.size() < 3 || categories[0] != "All categories"
        || categories[1] != talas::app::soundLibraryDemoCategory)
    {
        return finish("The Category filter did not offer Demos first");
    }
    showDemos(std::nullopt);
    if (soundDefinitionsPanel_->visibleEntryIdsForSmoke() != demoIds)
        return finish("The Demos category did not show exactly the demos in order");
    const auto effectRow = soundDefinitionsPanel_->rowTextForSmoke(effectId);
    const auto instrumentRow =
        soundDefinitionsPanel_->rowTextForSmoke(instrumentId);
    if (!effectRow.startsWith("Stereo Delay\nDEMO")
        || !effectRow.contains("Effect") || !effectRow.contains("needs live input")
        || !instrumentRow.startsWith("Sub Bass\nDEMO")
        || !instrumentRow.contains("Instrument"))
    {
        return finish("Demo rows did not name their group and kind: "
            + effectRow.toStdString() + " / " + instrumentRow.toStdString());
    }
    auto everything = workspaceSession_.soundLibrary;
    everything.category.reset();
    workspaceSession_.soundLibrary = everything;
    soundDefinitionsPanel_->setLibraryState(everything);
    // Across every category the demos lead the original examples.
    const auto all = soundDefinitionsPanel_->visibleEntryIdsForSmoke();
    const auto firstExample = std::ranges::find_if(all, [](const auto& id) {
        return id.starts_with("example:");
    });
    const auto examplesShown = static_cast<std::size_t>(all.end() - firstExample);
    if (examplesShown < demoIds.size()
        || !std::equal(demoIds.begin(), demoIds.end(), firstExample))
    {
        return finish("The demos did not lead the original examples in All categories");
    }

    // An effect: no silent audition, a note on live input instead.
    showDemos(effectId);
    if (soundDefinitionsPanel_->selectedEntryId() != effectId
        || soundDefinitionsPanel_->auditionEnabledForSmoke()
        || !soundDefinitionsPanel_->detailTextForSmoke().contains(
            "live audio input 1"))
    {
        return finish("The Stereo Delay demo did not explain that it needs live input: "
            + soundDefinitionsPanel_->detailTextForSmoke().toStdString());
    }
    startSoundLibraryAuditionWithProfile(
        std::string{effectId},
        sampleRate,
        blockFrames,
        deviceGeneration_.load(std::memory_order_acquire));
    if (soundLibraryAuditionActive_ != nullptr
        || !soundLibraryAuditionWorkerFinished_.load(std::memory_order_acquire)
        || !statusLabel_.getText().contains("live audio input 1"))
    {
        return finish("The Stereo Delay demo started a silent audition: "
            + statusLabel_.getText().toStdString());
    }
    handleSoundAction(SoundDefinitionsPanel::Action::OpenHelp, std::string{effectId});
    const auto helpId = "sound-library.help:" + juceString(effectId);
    if (!workspaceSession_.accessibility.contextHelpVisible
        || !contextHelpPanel_->contentContainsForSmoke(helpId, "Demo effect help")
        || !contextHelpPanel_->contentContainsForSmoke(
            helpId, "examples/demos/README.md#effects")
        || !contextHelpPanel_->contentContainsForSmoke(helpId, "live audio input 1"))
    {
        return finish("Open Help did not show the demo effect's help: "
            + contextHelpPanel_->contentSummaryForSmoke());
    }
    openSoundLibraryExampleNow(std::string{effectId});
    const auto& effect = projectBridge_.editor().document();
    if (projectPath().has_value() || effect.metadata.title != "Stereo Delay"
        || std::ranges::count(effect.graph.nodes, std::string_view{"talas.audioInput"},
               &talas::project::ProjectNode::typeId)
            != 1
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of demo Stereo Delay")
        || !statusLabel_.getText().contains("live audio input 1"))
    {
        return finish("Open Example did not install the untitled Stereo Delay demo: "
            + statusLabel_.getText().toStdString());
    }

    // An instrument: its audition plays the demo phrase.
    showDemos(instrumentId);
    if (soundDefinitionsPanel_->selectedEntryId() != instrumentId
        || !soundDefinitionsPanel_->auditionEnabledForSmoke()
        || !soundDefinitionsPanel_->detailTextForSmoke().contains(
            "Audition plays its demo phrase"))
    {
        return finish("The Sub Bass demo was not offered for audition");
    }
    startSoundLibraryAuditionWithProfile(
        std::string{instrumentId},
        sampleRate,
        blockFrames,
        deviceGeneration_.load(std::memory_order_acquire));
    if (auto error = waitForSoundLibraryAuditionSmoke())
        return finish(*error);
    std::array<float, blockFrames> left{};
    std::array<float, blockFrames> right{};
    std::array<float*, 2> channels{left.data(), right.data()};
    auto heard = false;
    for (int block = 0; block < maximumBlocks && !heard; ++block)
    {
        const auto rendered = renderSoundLibraryAudition(
            static_cast<double>(block) * blockFrames / sampleRate,
            channels.data(),
            channels.size(),
            blockFrames);
        if (soundLibraryAuditionActive_ == nullptr
            || soundLibraryAuditionActive_->entry.stableId != instrumentId
            || rendered != std::optional<bool>{true} || !finite(left)
            || !finite(right))
        {
            return finish("The Sub Bass demo audition did not render finite output");
        }
        heard = audible(left) || audible(right);
    }
    if (!heard)
        return finish("The Sub Bass demo audition stayed silent");
    stopSoundLibraryAudition();

    // Opened as a normal project: it stages and plays its phrase.
    showDemos(instrumentId);
    openSoundLibraryExampleNow(std::string{instrumentId});
    const auto& instrument = projectBridge_.editor().document();
    if (projectPath().has_value() || instrument.metadata.title != "Sub Bass"
        || instrument.soundDefinitions.empty()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of demo Sub Bass"))
    {
        return finish("Open Example did not install the untitled Sub Bass demo: "
            + statusLabel_.getText().toStdString());
    }
    // Staged and played on the Workbench's own bridge, as Play does.
    const auto stagesAndPlays = [&](const std::string& what) -> std::optional<std::string> {
        if (!stageProject(
                talas::runtime::RenderSettings{sampleRate, blockFrames},
                "Demo library smoke"))
        {
            return what + " did not stage: " + statusLabel_.getText().toStdString();
        }
        projectBridge_.setPlaying(true);
        auto sounded = false;
        for (int block = 0; block < maximumBlocks && !sounded; ++block)
        {
            left.fill(0.0F);
            right.fill(0.0F);
            if (!projectBridge_.render(channels.data(), channels.size(), blockFrames)
                || !finite(left) || !finite(right))
            {
                return what + " did not render finite output";
            }
            sounded = audible(left) || audible(right);
        }
        projectBridge_.setPlaying(false);
        if (!sounded)
            return what + " stayed silent";
        return std::nullopt;
    };
    if (auto error = stagesAndPlays("The opened Sub Bass demo"))
        return finish(*error);
    if (!projectBridge_.realtimeEventValueIdForLatestStage(
            "Cutoff", talas::expr::ValueDomain::Scalar))
    {
        return finish("The opened Sub Bass demo did not stage its scalar !Cutoff");
    }

    // Then a project of its own, opened from a file, whose !Cutoff is in
    // hertz: replacing the project lets the name take its new domain on the
    // same bridge instead of failing to stage.
    talas::project::ProjectDocument hertz;
    hertz.metadata.title = "Hertz Cutoff";
    hertz.virtualControls.push_back({"Cutoff",
        talas::expr::ValueDomain::Hertz,
        talas::project::VirtualControlKind::Fader,
        220.0,
        20.0,
        2000.0});
    talas::project::ProjectParameterExpression frequency;
    frequency.parameterId = "frequency";
    frequency.expression = "!Cutoff";
    talas::project::ProjectParameterExpression amplitude;
    amplitude.parameterId = "amplitude";
    amplitude.expression = "0.25";
    hertz.graph.nodes.push_back({"osc", "talas.oscillator", {frequency, amplitude}});
    hertz.graph.nodes.push_back({"output", "talas.audioOutput", {}});
    hertz.graph.connections.push_back({"osc", "audioOut", "output", "audioIn"});
    talas::project::reconcileProjectPresentation(hertz);
    const auto hertzPath = smokeDirectory / "workbench-demo-hertz-cutoff.talas.json";
    {
        std::ofstream written(hertzPath, std::ios::binary | std::ios::trunc);
        written << talas::project::serializeProjectDocument(hertz, registry_);
        if (!written)
            return finish("The hertz Cutoff project could not be written");
    }
    loadProjectFromPath(hertzPath);
    const auto& opened = projectBridge_.editor().document();
    if (opened.metadata.title != "Hertz Cutoff" || opened.virtualControls.size() != 1
        || opened.virtualControls.front().domain != talas::expr::ValueDomain::Hertz)
    {
        return finish("Open did not install the hertz Cutoff project: "
            + statusLabel_.getText().toStdString());
    }
    if (auto error = stagesAndPlays("The hertz Cutoff project opened after the demo"))
        return finish(*error);
    if (!projectBridge_.realtimeEventValueIdForLatestStage(
            "Cutoff", talas::expr::ValueDomain::Hertz))
    {
        return finish("The hertz Cutoff project did not stage !Cutoff in hertz");
    }
    std::filesystem::remove(hertzPath);
    return finish();
}
}
