#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <array>
#include <ranges>

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::runBoundedMidiControllerOutputExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:bounded-midi-controller-output"};
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
                    "C40 MIDI Controller smoke restore")
                && error.empty())
            {
                error =
                    "C40 MIDI Controller smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error =
                "C40 MIDI Controller smoke could not restore the prior project";
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
            "C40 packaged MIDI Controller entry omitted open or audition authority");
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
            "C40 packaged MIDI Controller entry could not be selected");
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
        || audition->terminalMidiEvents[0].bytes[0] != 0xb2
        || audition->terminalMidiEvents[0].bytes[1] != 10
        || audition->terminalMidiEvents[0].bytes[2] != 127
        || !std::ranges::all_of(
            auditionLeft,
            [](float sample) { return sample == 0.0F; })
        || !std::ranges::all_of(
            auditionRight,
            [](float sample) { return sample == 0.0F; }))
    {
        return finish(
            "C40 private audition did not capture the exact CC packet with silent device output");
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
            "C40 private audition did not deduplicate its unchanged CC packet");
    }
    stopSoundLibraryAudition();

    selectPackagedEntry();
    openSoundLibraryExampleNow(std::string{stableId});
    const auto& opened = projectBridge_.editor().document();
    if (projectPath().has_value()
        || !statusLabel_.getText().contains(
            "Opened an untitled copy of original example Bounded MIDI Controller Output")
        || opened.graph.nodes.size() != 1
        || opened.graph.nodes.front().typeId
            != "talas.midiOutputController"
        || !opened.assets.empty())
    {
        return finish(
            "C40 Open Example action did not install exact controller terminal authority");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "C40 packaged MIDI Controller example"))
    {
        return finish(
            "C40 MIDI Controller example did not stage: "
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
        || firstEvents[0].bytes[0] != 0xb2
        || firstEvents[0].bytes[1] != 10
        || firstEvents[0].bytes[2] != 127)
    {
        return finish(
            "C40 opened example did not publish the exact CC terminal event");
    }

    if (!stageProject(settings, "C40 MIDI Controller explicit restage"))
        return finish("C40 MIDI Controller example did not accept restage");
    std::array<talas::runtime::RealtimeMidiOutputEvent, 2> restagedEvents{};
    talas::runtime::RealtimeTerminalOutputBlock restagedOutput{
        restagedEvents};
    if (projectBridge_.renderTerminalEvents(
            blockFrames, restagedOutput)
            != talas::runtime::RealtimeTerminalRenderResult::Rendered
        || restagedOutput.midiEventCount != 1
        || restagedOutput.stageId == firstOutput.stageId
        || restagedOutput.stageId != projectBridge_.activeStageId()
        || restagedEvents[0].bytes[0] != 0xb2
        || restagedEvents[0].bytes[1] != 10
        || restagedEvents[0].bytes[2] != 127)
    {
        return finish(
            "C40 MIDI Controller restage did not restart exact stage-bound publication");
    }
    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    return runBoundedMidiNoteOutputExampleSmoke();
}

std::optional<std::string>
WorkbenchComponent::runBoundedMidiNoteOutputExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:bounded-midi-note-output"};
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
                    "C41 MIDI Note smoke restore")
                && error.empty())
            {
                error = "C41 MIDI Note smoke could not restore prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error = "C41 MIDI Note smoke could not restore prior project";
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
        return finish("C41 packaged MIDI Note entry lacks authority");
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
    const auto* audition = soundLibraryAuditionActive_.get();
    if (audition == nullptr
        || auditionRendered != std::optional<bool>{true}
        || audition->terminalMidiEventCount.load(
               std::memory_order_acquire)
            != 1
        || audition->terminalStageId.load(std::memory_order_acquire) == 0
        || audition->terminalMidiEvents[0].frameOffset != 0
        || audition->terminalMidiEvents[0].byteCount != 3
        || audition->terminalMidiEvents[0].bytes[0] != 0x91
        || audition->terminalMidiEvents[0].bytes[1] != 69
        || audition->terminalMidiEvents[0].bytes[2] != 127
        || !std::ranges::all_of(
            auditionLeft, [](float sample) { return sample == 0.0F; })
        || !std::ranges::all_of(
            auditionRight, [](float sample) { return sample == 0.0F; }))
    {
        return finish("C41 private audition did not capture exact note-on");
    }
    stopSoundLibraryAudition();

    state.selectedEntryId = std::string{stableId};
    workspaceSession_.soundLibrary = state;
    soundDefinitionsPanel_->setLibraryState(state);
    openSoundLibraryExampleNow(std::string{stableId});
    const auto& opened = projectBridge_.editor().document();
    if (projectPath().has_value()
        || opened.graph.nodes.size() != 1
        || opened.graph.nodes.front().typeId != "talas.midiOutputEvent"
        || !opened.assets.empty())
    {
        return finish("C41 Open Example did not install exact note terminal");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "C41 packaged MIDI Note example"))
        return finish("C41 MIDI Note example did not stage");
    projectBridge_.setPlaying(true);
    std::array<talas::runtime::RealtimeMidiOutputEvent, 2> firstEvents{};
    talas::runtime::RealtimeTerminalOutputBlock firstOutput{firstEvents};
    if (projectBridge_.renderTerminalEvents(blockFrames, firstOutput)
            != talas::runtime::RealtimeTerminalRenderResult::Rendered
        || firstOutput.midiEventCount != 1
        || firstOutput.stageId == 0
        || firstEvents[0].bytes[0] != 0x91
        || firstEvents[0].bytes[1] != 69
        || firstEvents[0].bytes[2] != 127)
    {
        return finish("C41 opened example did not publish exact note-on");
    }
    if (!stageProject(settings, "C41 MIDI Note explicit restage"))
        return finish("C41 MIDI Note example did not accept restage");
    std::array<talas::runtime::RealtimeMidiOutputEvent, 2> restagedEvents{};
    talas::runtime::RealtimeTerminalOutputBlock restagedOutput{restagedEvents};
    if (projectBridge_.renderTerminalEvents(blockFrames, restagedOutput)
            != talas::runtime::RealtimeTerminalRenderResult::Rendered
        || restagedOutput.midiEventCount != 1
        || restagedOutput.stageId == firstOutput.stageId
        || restagedOutput.stageId != projectBridge_.activeStageId()
        || restagedEvents[0].bytes[0] != 0x91
        || restagedEvents[0].bytes[1] != 69
        || restagedEvents[0].bytes[2] != 127)
    {
        return finish("C41 restage did not restart exact note publication");
    }
    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    return runBoundedMidiFileEchoExampleSmoke();
}

std::optional<std::string>
WorkbenchComponent::runBoundedMidiFileEchoExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:bounded-midi-file-echo"};
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
                    "C42 MIDI File smoke restore")
                && error.empty())
            {
                error = "C42 MIDI File smoke could not restore prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error = "C42 MIDI File smoke could not restore prior project";
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
        return finish("C42 packaged MIDI File entry lacks authority");
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
    const auto* audition = soundLibraryAuditionActive_.get();
    if (audition == nullptr
        || auditionRendered != std::optional<bool>{true}
        || audition->terminalMidiEventCount.load(
               std::memory_order_acquire)
            != 1
        || audition->terminalStageId.load(std::memory_order_acquire) == 0
        || audition->terminalMidiEvents[0].frameOffset != 0
        || audition->terminalMidiEvents[0].byteCount != 3
        || audition->terminalMidiEvents[0].bytes[0] != 0x90
        || audition->terminalMidiEvents[0].bytes[1] != 0x3c
        || audition->terminalMidiEvents[0].bytes[2] != 0x40
        || !std::ranges::all_of(
            auditionLeft, [](float sample) { return sample == 0.0F; })
        || !std::ranges::all_of(
            auditionRight, [](float sample) { return sample == 0.0F; }))
    {
        return finish(
            "C42 private audition did not capture exact SMF note-on");
    }
    stopSoundLibraryAudition();

    state.selectedEntryId = std::string{stableId};
    workspaceSession_.soundLibrary = state;
    soundDefinitionsPanel_->setLibraryState(state);
    openSoundLibraryExampleNow(std::string{stableId});
    const auto& opened = projectBridge_.editor().document();
    if (projectPath().has_value()
        || opened.graph.nodes.size() != 1
        || opened.graph.nodes.front().typeId != "talas.midiFileEcho"
        || opened.assets.size() != 1
        || opened.assets.front().kind
            != talas::project::ProjectAssetKind::Binary)
    {
        return finish(
            "C42 Open Example did not install exact SMF terminal authority");
    }

    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "C42 packaged MIDI File example"))
        return finish("C42 MIDI File example did not stage");
    projectBridge_.setPlaying(true);
    std::array<talas::runtime::RealtimeMidiOutputEvent, 2> firstEvents{};
    talas::runtime::RealtimeTerminalOutputBlock firstOutput{firstEvents};
    if (projectBridge_.renderTerminalEvents(blockFrames, firstOutput)
            != talas::runtime::RealtimeTerminalRenderResult::Rendered
        || firstOutput.midiEventCount != 1
        || firstOutput.stageId == 0
        || firstEvents[0].bytes[0] != 0x90
        || firstEvents[0].bytes[1] != 0x3c
        || firstEvents[0].bytes[2] != 0x40)
    {
        return finish(
            "C42 opened example did not publish exact SMF note-on");
    }
    projectBridge_.stopTransport();
    if (!stageProject(settings, "C42 MIDI File explicit restage"))
        return finish("C42 MIDI File example did not accept restage");
    projectBridge_.setPlaying(true);
    std::array<talas::runtime::RealtimeMidiOutputEvent, 2> restagedEvents{};
    talas::runtime::RealtimeTerminalOutputBlock restagedOutput{restagedEvents};
    if (projectBridge_.renderTerminalEvents(
            blockFrames, restagedOutput)
            != talas::runtime::RealtimeTerminalRenderResult::Rendered
        || restagedOutput.midiEventCount != 1
        || restagedOutput.stageId == firstOutput.stageId
        || restagedOutput.stageId != projectBridge_.activeStageId()
        || restagedEvents[0].bytes[0] != 0x90
        || restagedEvents[0].bytes[1] != 0x3c
        || restagedEvents[0].bytes[2] != 0x40)
    {
        return finish(
            "C42 restage did not restart exact SMF publication");
    }
    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    return runBoundedGlobalControllerOutputExampleSmoke();
}

std::optional<std::string>
WorkbenchComponent::runBoundedGlobalControllerOutputExampleSmoke()
{
    constexpr std::string_view stableId{
        "example:bounded-global-controller-output"};
    constexpr std::string_view expectedName{"!Generated"};
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
                    "C43 global-controller smoke restore")
                && error.empty())
            {
                error =
                    "C43 global-controller smoke could not restore prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error =
                "C43 global-controller smoke could not restore prior project";
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
            "C43 packaged global-controller entry lacks authority");
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
    const auto* audition = soundLibraryAuditionActive_.get();
    if (audition == nullptr
        || auditionRendered != std::optional<bool>{true}
        || audition->terminalMidiEventCount.load(
               std::memory_order_acquire)
            != 0
        || audition->terminalGlobalControllerEventCount.load(
               std::memory_order_acquire)
            != 1
        || audition->terminalGlobalControllerNameLength.load(
               std::memory_order_acquire)
            != expectedName.size()
        || audition->terminalStageId.load(std::memory_order_acquire) == 0
        || audition->terminalGlobalControllerEvents[0].frameOffset != 0
        || audition->terminalGlobalControllerEvents[0].value != 0.75
        || audition->terminalGlobalControllerEvents[0].allowLiveOverride
        || std::string_view{
               audition->terminalGlobalControllerName.data(),
               expectedName.size()}
            != expectedName
        || !std::ranges::all_of(
            auditionLeft, [](float sample) { return sample == 0.0F; })
        || !std::ranges::all_of(
            auditionRight, [](float sample) { return sample == 0.0F; }))
    {
        return finish(
            "C43 private audition did not capture exact global event");
    }
    stopSoundLibraryAudition();

    state.selectedEntryId = std::string{stableId};
    workspaceSession_.soundLibrary = state;
    soundDefinitionsPanel_->setLibraryState(state);
    openSoundLibraryExampleNow(std::string{stableId});
    const auto& opened = projectBridge_.editor().document();
    if (projectPath().has_value()
        || opened.graph.nodes.size() != 1
        || opened.graph.nodes.front().typeId
            != "talas.soundToGlobalController"
        || !opened.assets.empty())
    {
        return finish(
            "C43 Open Example did not install exact terminal authority");
    }

    const auto renderOpened = [&](
        talas::runtime::RealtimeTerminalOutputBlock& output,
        std::span<talas::runtime::RealtimeGlobalControllerOutputEvent> events,
        std::span<char> name) {
        output.globalControllerEvents = events;
        output.globalControllerName = name;
        return projectBridge_.renderTerminalEvents(blockFrames, output);
    };
    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "C43 packaged global-controller example"))
        return finish("C43 global-controller example did not stage");
    projectBridge_.setPlaying(true);
    std::array<talas::runtime::RealtimeGlobalControllerOutputEvent, 2>
        firstEvents{};
    std::array<char, 16> firstName{};
    talas::runtime::RealtimeTerminalOutputBlock firstOutput{};
    if (renderOpened(firstOutput, firstEvents, firstName)
            != talas::runtime::RealtimeTerminalRenderResult::Rendered
        || firstOutput.midiEventCount != 0
        || firstOutput.globalControllerEventCount != 1
        || firstOutput.globalControllerNameLength != expectedName.size()
        || firstOutput.stageId == 0
        || firstOutput.stageId != projectBridge_.activeStageId()
        || firstEvents[0].frameOffset != 0
        || firstEvents[0].value != 0.75
        || std::string_view{firstName.data(), expectedName.size()}
            != expectedName)
    {
        return finish(
            "C43 opened example did not publish exact global event");
    }

    projectBridge_.stopTransport();
    if (!stageProject(settings, "C43 global-controller explicit restage"))
        return finish("C43 global-controller example did not accept restage");
    projectBridge_.setPlaying(true);
    std::array<talas::runtime::RealtimeGlobalControllerOutputEvent, 2>
        restagedEvents{};
    std::array<char, 16> restagedName{};
    talas::runtime::RealtimeTerminalOutputBlock restagedOutput{};
    if (renderOpened(restagedOutput, restagedEvents, restagedName)
            != talas::runtime::RealtimeTerminalRenderResult::Rendered
        || restagedOutput.globalControllerEventCount != 1
        || restagedOutput.globalControllerNameLength != expectedName.size()
        || restagedOutput.stageId == firstOutput.stageId
        || restagedOutput.stageId != projectBridge_.activeStageId()
        || restagedEvents[0].frameOffset != 0
        || restagedEvents[0].value != 0.75
        || std::string_view{restagedName.data(), expectedName.size()}
            != expectedName)
    {
        return finish(
            "C43 restage did not restart exact global publication");
    }
    projectBridge_.setPlaying(false);
    if (auto error = finish())
        return error;
    if (auto error = runTriggeredGlobalControllerProjectRouteSmoke())
        return error;
    return runThresholdExampleSmoke();
}

std::optional<std::string>
WorkbenchComponent::runTriggeredGlobalControllerProjectRouteSmoke()
{
    constexpr std::string_view stableId{
        "example:triggered-global-controller-override"};
    constexpr double sampleRate = 8'000.0;
    constexpr std::size_t blockFrames = 64;
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto previousDocumentStaged = currentDocumentStaged_;
    const auto previousTransportPlaying = transportPlaying_;
    const auto previousLibraryState = workspaceSession_.soundLibrary;
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        soundLibraryAuditionSmokeUsePreparedProfile_ = false;
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
            if (!stageProject(std::move(restoreSettings),
                    "Triggered controller route smoke restore")
                && error.empty())
                error = "Controller route smoke could not restore prior stage";
        }
        setPlaying(previousTransportPlaying);
        workspaceSession_.soundLibrary = previousLibraryState;
        soundDefinitionsPanel_->setLibraryState(previousLibraryState);
        if (!restored.ok() && error.empty())
            error = "Controller route smoke could not restore prior project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };

    talas::project::ProjectDocument target;
    target.metadata.title = "Generated Controller Target";
    target.virtualControls.push_back({
        "Generated", talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader, 0.0, 0.0, 1.0});
    talas::project::ProjectParameterExpression frequency;
    frequency.parameterId = "frequency";
    frequency.expression = "1 hz";
    talas::project::ProjectParameterExpression amplitude;
    amplitude.parameterId = "amplitude";
    amplitude.expression = "!Generated";
    target.graph.nodes.push_back({
        "osc", "talas.oscillator", {frequency, amplitude}});
    target.graph.nodes.push_back({"output", "talas.audioOutput", {}});
    target.graph.connections.push_back({
        "osc", "audioOut", "output", "audioIn"});
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "generated";
    bank.displayName = "Generated";
    bank.scope.kind = talas::project::SnapshotScopeKind::Project;
    const auto resolved = talas::project::resolveSnapshotScopeTargets(
        target, registry_, bank.scope);
    const auto initial = talas::project::initializeSnapshotScopeValues(
        resolved.targets);
    if (!resolved.ok() || !initial.ok())
        return finish("Controller route target Preset could not initialize");
    bank.snapshots.push_back({"captured", "Captured", initial.values});
    target.snapshotLibrary.banks.push_back(std::move(bank));
    if (!projectBridge_.editor().replaceDocument(target).ok())
        return finish("Controller route target project could not load");
    editBoundary_.markRecovered(target, std::nullopt);
    documentChanged();
    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (!stageProject(settings, "Triggered controller route target"))
        return finish("Controller route target could not stage: "
            + statusLabel_.getText().toStdString());
    setPlaying(true);
    std::array<float, blockFrames> left{};
    std::array<float, blockFrames> right{};
    std::array<float*, 2> channels{left.data(), right.data()};
    if (!projectBridge_.render(channels.data(), channels.size(), blockFrames)
        || projectBridge_.activeOutputChannelCount() == 0)
        return finish("Controller route target Audio stage did not activate");

    auto libraryState = soundDefinitionsPanel_->libraryState();
    libraryState.collection =
        talas::project::WorkspaceSoundLibraryCollection::All;
    libraryState.sourceFilter =
        talas::project::WorkspaceSoundLibrarySourceFilter::All;
    libraryState.compatibilityFilter =
        talas::project::WorkspaceSoundLibraryCompatibilityFilter::All;
    libraryState.healthFilter =
        talas::project::WorkspaceSoundLibraryHealthFilter::All;
    libraryState.category.reset();
    libraryState.origin.reset();
    libraryState.selectedEntryId = std::string{stableId};
    workspaceSession_.soundLibrary = libraryState;
    soundDefinitionsPanel_->setLibraryState(libraryState);
    if (!soundDefinitionsPanel_->controllerRouteAvailableForSmoke())
        return finish("Controller route action was unavailable for its tagged example");
    soundLibraryAuditionSmokeUsePreparedProfile_ = true;
    soundDefinitionsPanel_->triggerControllerRouteActionForSmoke();
    soundLibraryAuditionSmokeUsePreparedProfile_ = false;
    if (auto error = waitForSoundLibraryAuditionSmoke())
        return finish(*error);
    if (soundLibraryAuditionActive_ == nullptr
        || !soundLibraryAuditionActive_->routeControllerToProject)
        return finish("Controller example did not activate its project route: "
            + statusLabel_.getText().toStdString());

    talas::project::SnapshotTargetIdentity gain;
    gain.kind = talas::project::SnapshotTargetKind::EventValue;
    gain.eventName = "Generated";
    gain.domain = talas::expr::ValueDomain::Scalar;
    const auto performanceValue = [&]() -> std::optional<double> {
        const auto& values = projectBridge_.performanceState().currentValues;
        const auto found = std::ranges::find(
            values, gain, &talas::project::SnapshotValueEntry::target);
        return found == values.end()
            ? std::nullopt
            : std::optional<double>{found->value.number};
    };
    const auto renderRoute = [&] {
        return renderSoundLibraryAudition(
            0.0, channels.data(), channels.size(), blockFrames)
            == std::optional<bool>{true};
    };
    if (!renderRoute()
        || soundLibraryAuditionActive_->terminalGlobalControllerEventCount.load(
               std::memory_order_acquire) != 1
        || soundLibraryAuditionActive_->terminalGlobalControllerEvents[0].value
            != 0.4
        || !soundLibraryAuditionActive_->terminalGlobalControllerEvents[0]
                .allowLiveOverride)
        return finish("Triggered controller source did not publish its exact live-override event");
    processSoundLibraryControllerRoute();
    if (performanceValue() != std::optional<double>{0.4}
        || !renderRoute()
        || !std::ranges::any_of(left, [](float sample) {
               return std::abs(sample) > 1.0e-5F;
           }))
        return finish("Generated controller value did not reach audible target Sound");

    if (!virtualControlSurface_->setValueForSmoke("Generated", 0.75)
        || soundLibraryAuditionActive_ == nullptr
        || performanceValue() != std::optional<double>{0.75}
        || !projectBridge_.captureSnapshotPerformance(
            "generated", "captured").ok()
        || !projectBridge_.adoptSnapshotAuthoringEdit().ok())
        return finish("Manual VCS value or Preset capture failed during route");
    snapshotAuthoringChanged();
    if (!virtualControlSurface_->setValueForSmoke("Generated", 0.2)
        || !projectBridge_.recallSnapshot("generated", "captured").ok()
        || !renderRoute()
        || performanceValue() != std::optional<double>{0.75}
        || soundLibraryAuditionActive_ == nullptr)
        return finish("Preset recall did not supersede the manual VCS value while routed");

    soundDefinitionsPanel_->triggerControllerRouteActionForSmoke();
    if (soundLibraryAuditionActive_ != nullptr)
        return finish("Stop Control did not revoke the source route");
    soundLibraryAuditionSmokeUsePreparedProfile_ = true;
    soundDefinitionsPanel_->triggerControllerRouteActionForSmoke();
    soundLibraryAuditionSmokeUsePreparedProfile_ = false;
    if (auto error = waitForSoundLibraryAuditionSmoke())
        return finish(*error);
    if (soundLibraryAuditionActive_ == nullptr
        || !renderRoute())
        return finish("Later controller assertion could not restart");
    processSoundLibraryControllerRoute();
    if (performanceValue() != std::optional<double>{0.4}
        || !renderRoute()
        || !std::ranges::any_of(left, [](float sample) {
               return std::abs(sample) > 1.0e-5F;
           }))
        return finish("Later generated assertion did not supersede recalled Preset");

    soundDefinitionsPanel_->triggerControllerRouteActionForSmoke();
    soundLibraryAuditionSmokeUsePreparedProfile_ = true;
    soundDefinitionsPanel_->triggerControllerRouteActionForSmoke();
    soundLibraryAuditionSmokeUsePreparedProfile_ = false;
    if (auto error = waitForSoundLibraryAuditionSmoke())
        return finish(*error);
    if (soundLibraryAuditionActive_ == nullptr || !renderRoute())
        return finish("Stale-event route source could not restart");
    const auto generationBeforeRecall = projectBridge_.latestStageGeneration();
    if (!projectBridge_.recallSnapshot("generated", "captured").ok()
        || projectBridge_.latestStageGeneration()
            == generationBeforeRecall)
        return finish("Preset recall did not advance queued-event authority");
    processSoundLibraryControllerRoute();
    if (soundLibraryAuditionActive_ == nullptr
        || performanceValue() != std::optional<double>{0.75})
        return finish("Pre-recall generated event crossed the new Preset stage");

    soundDefinitionsPanel_->triggerControllerRouteActionForSmoke();
    soundLibraryAuditionSmokeUsePreparedProfile_ = true;
    soundDefinitionsPanel_->triggerControllerRouteActionForSmoke();
    soundLibraryAuditionSmokeUsePreparedProfile_ = false;
    if (auto error = waitForSoundLibraryAuditionSmoke())
        return finish(*error);
    if (soundLibraryAuditionActive_ == nullptr || !renderRoute()
        || !virtualControlSurface_->setValueForSmoke("Generated", 0.6)
        || soundLibraryAuditionActive_ == nullptr
        || performanceValue() != std::optional<double>{0.6})
        return finish("Manual VCS gesture did not follow a queued generated event");
    return finish();
}
}
