#include "app/workbench/WorkbenchComponentInternal.h"

namespace talas::app::workbench
{
// Trk 2 routed to the submix rev, rev chosen
// from Trk 7's Input menu, the input itself placed on the empty track, the
// input taken away again, and a new submix from the Submix menu.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runTimelineInputSmoke()
{
#if defined(TALAS_WORKBENCH_TIMELINE_TEST_SUPPORT)
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "Timeline input smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    const auto seconds = [](double value) {
        return talas::project::ScoreTime{talas::project::ScoreTimeDomain::Seconds, value, 0};
    };
    const auto track = [](std::string id, talas::project::ScoreTrackKind kind, std::string output) {
        talas::project::ScoreTrack result;
        result.trackId = id;
        result.displayName = id;
        result.kind = kind;
        result.outputTrackId = std::move(output);
        result.overlapMode = talas::project::ScoreOverlapMode::Layer;
        return result;
    };
    talas::project::ProjectDocument fixture;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Vocal";
    definition.outputs.push_back({"audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    definition.graph.nodes.push_back({"osc", "talas.oscillator", {}});
    fixture.soundDefinitions.push_back(std::move(definition));
    fixture.timeline.score.tracks = {
        track("Trk 2", talas::project::ScoreTrackKind::Track, "rev"),
        track("rev", talas::project::ScoreTrackKind::Submix, "master"),
        track("Trk 7", talas::project::ScoreTrackKind::Track, "master"),
        track("master", talas::project::ScoreTrackKind::Master, {}),
    };
    fixture.timeline.score.tracks[0].trackId = "trk2";
    fixture.timeline.score.tracks[2].trackId = "trk7";
    fixture.timeline.score.clips = {
        {"vocal", "trk2", "voice", seconds(0.5), seconds(4.0), seconds(0.0), seconds(0.0), seconds(0.0), 1},
    };
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("Timeline input fixture was rejected: " + replaced.message);
    documentChanged();
    const auto& score = projectBridge_.editor().document().timeline.score;

    // The submixes the track may take, then the hardware's inputs.
    const auto trk7Inputs = timelinePanel_->trackInputMenuForSmoke("trk7");
    const auto trk2Inputs = timelinePanel_->trackInputMenuForSmoke("trk2");
    if (trk7Inputs.size() != 10 || trk7Inputs[0] != "No input" || trk7Inputs[1] != "rev" || trk7Inputs[2] != "Input: 1"
        || trk2Inputs.size() != 9 || trk2Inputs[1] != "Input: 1")
        return finish("the Input menu did not list rev for Trk 7 and leave it out for Trk 2, which feeds it");
    if (!timelinePanel_->setTrackInputForSmoke("trk7", "rev") || score.tracks[2].inputTrackId != "rev")
        return finish("choosing rev from Trk 7's Input menu did not make it the track's input");
    const auto input = std::ranges::find(score.clips, "trk7", &talas::project::ScoreClip::trackId);
    if (input == score.clips.end() || input->start != seconds(0.0) || input->duration != seconds(4.5))
        return finish("the input did not start at the beginning, as long as the longest submix track");
    if (timelinePanel_->setTrackInputForSmoke("trk2", "rev"))
        return finish("Trk 2 took the submix of its own output as its input");
    if (!timelinePanel_->setTrackInputForSmoke("trk7", "") || !score.tracks[2].inputTrackId.empty())
        return finish("No input did not take the input away");
    // Input: 1, the hardware's first audio input, then none again.
    if (!timelinePanel_->setTrackHardwareInputForSmoke("trk7", 1) || score.tracks[2].inputChannel != 1)
        return finish("Input: 1 did not become Trk 7's input");
    if (!timelinePanel_->setTrackInputForSmoke("trk7", "") || score.tracks[2].inputChannel != 0)
        return finish("No input did not take the hardware input away");

    // The Submix menu's new submix, named Vox, connected to Trk 7,
    // then disconnected.
    if (!timelinePanel_->addSubmixForSmoke("trk7", "Vox") || score.tracks[2].sendTrackIds != std::vector<std::string>{"Vox"})
        return finish("New Submix did not make a submix named Vox and connect Trk 7 to it");
    if (!timelinePanel_->setTrackSendForSmoke("trk7", "Vox", false) || !score.tracks[2].sendTrackIds.empty())
        return finish("the Submix menu did not disconnect Trk 7 from Vox");
    if (timelinePanel_->setTrackSendForSmoke("trk2", "rev", false))
        return finish("the submix Trk 2's output goes to was disconnected");
    // Trk 7 hard-wired to output 1, then back to its pan
    // controls.
    if (!timelinePanel_->toggleTrackOutputForSmoke("trk7", 1) || score.tracks[2].outputChannels != std::vector<std::uint32_t>{1})
        return finish("the audio output menu did not wire Trk 7 to output 1");
    if (!timelinePanel_->toggleTrackOutputForSmoke("trk7", 0) || !score.tracks[2].outputChannels.empty())
        return finish("Use Pan Controls did not switch Trk 7 back to panning");
    // No output, then the pan controls again.
    if (!timelinePanel_->setTrackNoOutputForSmoke("trk7") || !score.tracks[2].noOutput)
        return finish("No Output did not take Trk 7's output away");
    if (!timelinePanel_->toggleTrackOutputForSmoke("trk7", 0) || score.tracks[2].noOutput)
        return finish("Use Pan Controls did not give Trk 7 its output back");
    // Trk 7's Sounds take MIDI on channel 2, then on the
    // default channel again.
    if (!timelinePanel_->setTrackMidiChannelForSmoke("trk7", 2) || score.tracks[2].midiChannel != 2)
        return finish("the MIDI status control did not set Trk 7's MIDI channel");
    if (!timelinePanel_->setTrackMidiChannelForSmoke("trk7", 0) || score.tracks[2].midiChannel != 0)
        return finish("the MIDI status control did not return Trk 7 to the default channel");
    // Record to disk..., output 1 into a mono file of its own.
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("talas-timeline-record-smoke");
    (void) folder.deleteRecursively();
    (void) folder.createDirectory();
    talas::audio::TimelineRecordingOptions recording;
    recording.destination = folder.getChildFile("haiku.wav").getFullPathName().toStdString();
    recording.channels = {1};
    const auto recorded = timelinePanel_->recordTimelineForSmoke(recording);
    const auto written = folder.getChildFile("haiku 1.wav").existsAsFile() && !folder.getChildFile("haiku 2.wav").exists();
    (void) folder.deleteRecursively();
    if (!recorded.ok() || !written)
        return finish("Record to disk did not record output 1 of the Timeline: " + recorded.error);
    return finish();
#else
    return std::nullopt;
#endif
}
}
