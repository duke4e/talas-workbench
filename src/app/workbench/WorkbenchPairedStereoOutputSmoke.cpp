#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::runPairedStereoOutputExampleSmoke()
{
    struct Profile
    {
        std::string_view stableId;
        std::string_view title;
        std::string_view typeId;
        std::size_t channelCount = 0;
        std::size_t oscillatorCount = 0;
    };
    constexpr std::array profiles{
        Profile{
            "example:paired-stereo-output-4",
            "Paired Stereo Output 4",
            "talas.stereoInOutput4",
            4,
            2},
        Profile{
            "example:paired-stereo-output-8",
            "Paired Stereo Output 8",
            "talas.stereoInOutput8",
            8,
            4},
    };
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
                    "C28 paired terminal smoke restore")
                && error.empty())
            {
                error = "C28 paired terminal smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error = "C28 paired terminal smoke could not restore the prior project";
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

    for (const auto& profile : profiles)
    {
        refreshSoundLibraryProjection();
        const auto* entry = soundLibraryEntry(profile.stableId);
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
                "C28 paired terminal entry omitted open or audition authority");
        }

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
        state.selectedEntryId = std::string{profile.stableId};
        workspaceSession_.soundLibrary = state;
        soundDefinitionsPanel_->setLibraryState(state);
        if (soundDefinitionsPanel_->selectedEntryId()
            != profile.stableId)
        {
            return finish(
                "C28 paired terminal entry could not be selected");
        }

        startSoundLibraryAuditionWithProfile(
            std::string{profile.stableId},
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
            || soundLibraryAuditionActive_->entry.stableId
                != profile.stableId
            || auditionRendered != std::optional<bool>{true}
            || !finiteAndAudible(auditionLeft)
            || auditionLeft != auditionRight)
        {
            return finish(
                "C28 paired terminal audition did not preserve its first stereo pair");
        }
        stopSoundLibraryAudition();

        state.selectedEntryId = std::string{profile.stableId};
        workspaceSession_.soundLibrary = state;
        soundDefinitionsPanel_->setLibraryState(state);
        openSoundLibraryExampleNow(std::string{profile.stableId});
        const auto& opened = projectBridge_.editor().document();
        const auto terminalCount = std::ranges::count(
            opened.graph.nodes,
            profile.typeId,
            &talas::project::ProjectNode::typeId);
        const auto oscillatorCount = std::ranges::count(
            opened.graph.nodes,
            std::string_view{"talas.oscillator"},
            &talas::project::ProjectNode::typeId);
        if (projectPath().has_value()
            || !statusLabel_.getText().contains(
                juce::String{"Opened an untitled copy of original example "}
                    + juce::String{
                        profile.title.data(), profile.title.size()})
            || terminalCount != 1
            || static_cast<std::size_t>(oscillatorCount)
                != profile.oscillatorCount
            || !opened.assets.empty())
        {
            return finish(
                "C28 Open Example action did not install the exact paired terminal project");
        }

        talas::runtime::RenderSettings settings{sampleRate, blockFrames};
        if (!stageProject(settings, "C28 packaged paired terminal example"))
        {
            return finish(
                "C28 paired terminal example did not stage: "
                + statusLabel_.getText().toStdString());
        }
        std::array<std::array<float, blockFrames>, 8> first{};
        std::array<float*, 8> firstChannels{};
        for (std::size_t channel = 0;
             channel < profile.channelCount;
             ++channel)
        {
            firstChannels[channel] = first[channel].data();
        }
        projectBridge_.setPlaying(true);
        if (!projectBridge_.render(
                {},
                0.0,
                firstChannels.data(),
                profile.channelCount,
                blockFrames))
        {
            return finish(
                "C28 opened paired terminal example did not play");
        }
        for (std::size_t pair = 0;
             pair < profile.channelCount / 2;
             ++pair)
        {
            if (!finiteAndAudible(first[pair * 2])
                || first[pair * 2] != first[(pair * 2) + 1])
            {
                return finish(
                    "C28 opened paired terminal example reordered or deduplicated a pair");
            }
        }

        if (!stageProject(settings, "C28 paired terminal explicit restage"))
        {
            return finish(
                "C28 paired terminal example did not accept restage");
        }
        std::array<std::array<float, blockFrames>, 8> restaged{};
        std::array<float*, 8> restagedChannels{};
        for (std::size_t channel = 0;
             channel < profile.channelCount;
             ++channel)
        {
            restagedChannels[channel] = restaged[channel].data();
        }
        if (!projectBridge_.render(
                {},
                1.0,
                restagedChannels.data(),
                profile.channelCount,
                blockFrames))
        {
            return finish(
                "C28 restaged paired terminal example did not render");
        }
        for (std::size_t channel = 0;
             channel < profile.channelCount;
             ++channel)
        {
            if (first[channel] != restaged[channel])
            {
                return finish(
                    "C28 paired terminal restage did not restart exact source state");
            }
        }
        projectBridge_.setPlaying(false);
    }

    if (auto error = finish())
        return error;
    return runSyntheticSpectrumFromSoundsExampleSmoke();
}
}
