#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <array>
#include <ranges>

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::runBoundedRawMidiOutputExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:bounded-raw-midi-output"};
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
                    "C39 Raw MIDI Output smoke restore")
                && error.empty())
            {
                error =
                    "C39 Raw MIDI Output smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error =
                "C39 Raw MIDI Output smoke could not restore the prior project";
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
            "C39 packaged Raw MIDI Output entry omitted open or audition authority");
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
            "C39 packaged Raw MIDI Output entry could not be selected");
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
    auditionLeft.fill(1.0F);
    auditionRight.fill(-1.0F);
    std::array<float*, 2> auditionChannels{
        auditionLeft.data(), auditionRight.data()};
    const auto auditionRendered = renderSoundLibraryAudition(
        0.0,
        auditionChannels.data(),
        auditionChannels.size(),
        blockFrames);
    const auto* audition = soundLibraryAuditionActive_.get();
    if (audition == nullptr
        || audition->entry.stableId != stableId
        || auditionRendered != std::optional<bool>{true}
        || audition->terminalMidiEventCount.load(
               std::memory_order_acquire)
            != 1
        || audition->terminalStageId.load(std::memory_order_acquire)
            == 0
        || audition->terminalMidiEvents[0].frameOffset != 0
        || audition->terminalMidiEvents[0].byteCount != 3
        || audition->terminalMidiEvents[0].bytes[0] != 0x90
        || audition->terminalMidiEvents[0].bytes[1] != 64
        || audition->terminalMidiEvents[0].bytes[2] != 0x7f
        || !std::ranges::all_of(
            auditionLeft,
            [](float sample) { return sample == 0.0F; })
        || !std::ranges::all_of(
            auditionRight,
            [](float sample) { return sample == 0.0F; }))
    {
        return finish(
            "C39 private audition did not capture the exact packet with silent device output");
    }
    const auto auditionStageId = audition->terminalStageId.load(
        std::memory_order_acquire);
    if (renderSoundLibraryAudition(
            0.0,
            auditionChannels.data(),
            auditionChannels.size(),
            blockFrames)
            != std::optional<bool>{true}
        || audition->terminalMidiEventCount.load(
               std::memory_order_acquire)
            != 0
        || audition->terminalStageId.load(std::memory_order_acquire)
            != auditionStageId)
    {
        return finish(
            "C39 private audition did not deduplicate its unchanged packet");
    }
    stopSoundLibraryAudition();

    selectPackagedEntry();
    openSoundLibraryExampleNow(std::string{stableId});
    const auto& opened = projectBridge_.editor().document();
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Bounded Raw MIDI Output")
        || opened.graph.nodes.size() != 1
        || opened.graph.nodes.front().typeId
            != "talas.midiOutputEventInBytes"
        || !opened.assets.empty())
    {
        return finish(
            "C39 Open Example action did not install exact terminal-only authority");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "C39 packaged Raw MIDI Output example"))
    {
        return finish(
            "C39 Raw MIDI Output example did not stage: "
            + statusLabel_.getText().toStdString());
    }
    projectBridge_.setPlaying(true);
    std::array<talas::runtime::RealtimeMidiOutputEvent, 2> firstEvents{};
    talas::runtime::RealtimeTerminalOutputBlock firstOutput{firstEvents};
    if (projectBridge_.renderTerminalEvents(blockFrames, firstOutput)
            != talas::runtime::RealtimeTerminalRenderResult::Rendered
        || firstOutput.midiEventCount != 1
        || firstOutput.stageId == 0
        || firstEvents[0].byteCount != 3
        || firstEvents[0].bytes[0] != 0x90
        || firstEvents[0].bytes[1] != 64
        || firstEvents[0].bytes[2] != 0x7f)
    {
        return finish(
            "C39 opened example did not publish the exact terminal event");
    }

    if (!stageProject(settings, "C39 Raw MIDI Output explicit restage"))
        return finish("C39 Raw MIDI Output example did not accept restage");
    std::array<talas::runtime::RealtimeMidiOutputEvent, 2> restagedEvents{};
    talas::runtime::RealtimeTerminalOutputBlock restagedOutput{
        restagedEvents};
    if (projectBridge_.renderTerminalEvents(
            blockFrames, restagedOutput)
            != talas::runtime::RealtimeTerminalRenderResult::Rendered
        || restagedOutput.midiEventCount != 1
        || restagedOutput.stageId == firstOutput.stageId
        || restagedOutput.stageId != projectBridge_.activeStageId()
        || restagedEvents[0].bytes[0] != 0x90
        || restagedEvents[0].bytes[1] != 64
        || restagedEvents[0].bytes[2] != 0x7f)
    {
        return finish(
            "C39 Raw MIDI Output restage did not restart exact stage-bound publication");
    }
    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    return runBoundedMidiControllerOutputExampleSmoke();
}
}
