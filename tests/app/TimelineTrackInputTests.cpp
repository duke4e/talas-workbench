#include "talas/app/AutomationLaneEditing.h"
#include "talas/app/TimelineTrackInput.h"
#include "talas/project/ProjectScore.h"

#include <iostream>
#include <stdexcept>
#include <string>

// The track input source control's menu, the
// submix colours, and choosing an input.
namespace
{
using talas::project::ScoreTrackKind;

void require(bool value, std::string_view message)
{
    if (!value)
        throw std::runtime_error(std::string(message));
}

talas::project::ScoreTime seconds(double value)
{
    return {talas::project::ScoreTimeDomain::Seconds, value, 0};
}

talas::project::ScoreTrack track(std::string id, ScoreTrackKind kind, std::string output)
{
    return {id, id + " name", kind, talas::project::ScoreTimeDomain::Seconds, std::move(output), 2,
        talas::project::ScoreOverlapMode::Layer};
}

talas::project::ProjectDocument mix()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice";
    voice.displayName = "Voice";
    voice.graph.nodes = {{"osc", "talas.oscillator", {}}};
    voice.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    document.soundDefinitions.push_back(voice);
    document.timeline.score.tracks = {
        track("trk1", ScoreTrackKind::Track, "vox"),
        track("trk2", ScoreTrackKind::Track, "vox"),
        track("vox", ScoreTrackKind::Submix, "master"),
        track("drums", ScoreTrackKind::Submix, "master"),
        track("trk3", ScoreTrackKind::Track, "master"),
        track("master", ScoreTrackKind::Master, {}),
    };
    document.timeline.score.clips = {
        {"a", "trk1", "voice", seconds(1.0), seconds(2.0), seconds(0), seconds(0), seconds(0), 1},
        {"b", "trk2", "voice", seconds(0.5), seconds(1.5), seconds(0), seconds(0), seconds(0), 3},
    };
    return document;
}

void testTheInputMenu()
{
    const auto document = mix();
    const auto& score = document.timeline.score;
    require(talas::app::timelineSubmixColourIndex(score, "vox") == 0 && talas::app::timelineSubmixColourIndex(score, "drums") == 1
            && !talas::app::timelineSubmixColourIndex(score, "trk1").has_value(),
        "each submix is a block of its own colour");
    const auto choices = talas::app::timelineTrackInputChoices(score, "trk3");
    require(choices.size() == 2 && choices[0].trackId == "vox" && choices[0].displayName == "vox name"
            && choices[1].colourIndex == 1,
        "the last grouping lists the names of all current submixes");
    const auto fromVox = talas::app::timelineTrackInputChoices(score, "trk1");
    require(fromVox.size() == 1 && fromVox[0].trackId == "drums",
        "a submix a track's output goes to is absent from its list of inputs");
}

void testChoosingAnInput()
{
    const auto document = mix();
    std::string error;
    const auto chosen = talas::app::timelineWithTrackInput(document, "trk3", "vox", error);
    require(chosen.has_value() && chosen->timeline.score.tracks[4].inputTrackId == "vox", "Trk 3 takes vox as its input");
    const auto routeError = talas::project::projectScoreValidationError(chosen->timeline.score,
        std::vector<std::string_view>{"voice", chosen->soundDefinitions.back().definitionId});
    require(!routeError.has_value(), "the chosen input is a valid route: " + (routeError ? routeError->entityId + " " + routeError->message : std::string{}));
    // From the beginning of the Timeline, as long as its longest
    // submix track: Trk 2's three repeats end at 5 seconds.
    const auto& input = chosen->timeline.score.clips.back();
    require(input.trackId == "trk3" && input.start == seconds(0.0) && input.duration == seconds(5.0),
        "the input starts at the beginning, as long as the longest submix track");
    const auto& sound = chosen->soundDefinitions.back();
    require(sound.displayName == "vox name" && sound.graph.nodes.front().typeId == "talas.audioInput" && sound.outputs.size() == 2,
        "the input is a Sound of the track's stereo audio input");

    auto occupied = document;
    occupied.timeline.score.clips.push_back(
        {"c", "trk3", "voice", seconds(0.0), seconds(1.0), seconds(0), seconds(0), seconds(0), 1});
    const auto kept = talas::app::timelineWithTrackInput(occupied, "trk3", "vox", error);
    require(kept.has_value() && kept->timeline.score.clips.size() == 3 && kept->soundDefinitions.size() == 1,
        "a track with Sounds keeps them and gets no input Sound");
    const auto none = talas::app::timelineWithTrackInput(*chosen, "trk3", "", error);
    require(none.has_value() && none->timeline.score.tracks[4].inputTrackId.empty(), "No input takes the input away");
    require(!talas::app::timelineWithTrackInput(document, "trk1", "vox", error).has_value(),
        "a track cannot take the submix its own output goes to");
    require(!talas::app::timelineWithTrackInput(document, "vox", "drums", error).has_value(),
        "a submix has no input of its own");
}

void testTheSubmixMenu()
{
    auto document = mix();
    auto& score = document.timeline.score;
    const auto menu = talas::app::timelineSubmixMenu(score, "trk1");
    require(menu.size() == 2 && menu[0].trackId == "vox" && menu[0].connected && menu[0].output
            && menu[1].trackId == "drums" && !menu[1].connected,
        "the Submix menu connects or disconnects the track from each existing submix");
    std::string error;
    const auto sent = talas::app::timelineWithTrackSend(document, "trk1", "drums", true, error);
    require(sent.has_value() && sent->timeline.score.tracks[0].sendTrackIds == std::vector<std::string>{"drums"},
        "connecting the track to a second submix");
    require(talas::app::timelineSubmixStatus(sent->timeline.score, "trk1") == std::vector<std::size_t>{0, 1},
        "for each submix the track is routed to, the Submix status shows a block of colour");
    require(talas::app::timelineSubmixMenu(sent->timeline.score, "trk3").size() == 2,
        "the submix's name appears in the Submix menu of each of the other tracks");
    const auto unsent = talas::app::timelineWithTrackSend(*sent, "trk1", "drums", false, error);
    require(unsent.has_value() && unsent->timeline.score.tracks[0].sendTrackIds.empty(), "disconnecting the track from it");
    require(!talas::app::timelineWithTrackSend(document, "trk1", "vox", false, error).has_value(),
        "the submix the track's output goes to stays connected");

    auto many = document;
    for (int index = 0; index < 20; ++index)
    {
        auto submix = track("s" + std::to_string(index), ScoreTrackKind::Submix, "master");
        many.timeline.score.tracks.insert(many.timeline.score.tracks.end() - 1, submix);
        many.timeline.score.tracks[0].sendTrackIds.push_back(submix.trackId);
    }
    require(talas::app::timelineSubmixStatus(many.timeline.score, "trk1").size() == 16,
        "only 16 submixes show as blocks of colour");

    // The track's Trk_To_Submix control, for each submix.
    const auto targets = talas::app::automationTargets(*sent, talas::project::ScoreAutomationScope::Track, "trk1");
    require(std::ranges::any_of(targets, [](const auto& target) {
        return target.eventName == "To_vox" && target.label == "trk1 name_To_vox name";
    }) && std::ranges::any_of(targets, [](const auto& target) { return target.eventName == "To_drums"; }),
        "a level control for each submix, named with the track's name, To and the submix's");

    // A new submix, with a random letter for a name unless one is typed.
    const auto letter = talas::app::timelineNewSubmixName(score, 7);
    require(letter.size() == 1 && letter[0] >= 'A' && letter[0] <= 'Z', "a random letter is supplied");
    const auto made = talas::app::timelineWithNewSubmix(document, "trk3", "Vox", error);
    require(made.has_value(), "create a new submix and connect to that: " + error);
    const auto& tracks = made->timeline.score.tracks;
    const auto created = std::ranges::find(tracks, "Vox", &talas::project::ScoreTrack::trackId);
    require(created != tracks.end() && created->kind == talas::project::ScoreTrackKind::Submix && created->displayName == "Vox"
            && tracks[4].sendTrackIds == std::vector<std::string>{"Vox"},
        "route the output of the track to a new submix named Vox");
    require(!talas::project::projectScoreValidationError(made->timeline.score, std::vector<std::string_view>{"voice"}).has_value(),
        "the new submix is a valid route");
}

// The audio output menu and the Output channel
// status.
void testTheOutputMenu()
{
    const auto document = mix();
    require(talas::app::timelineOutputStatus(document.timeline.score, "trk3").empty(),
        "a track uses its pan controls until it is wired to output channels");
    std::string error;
    const auto wired = talas::app::timelineWithTrackOutputs(document, "trk3", {2, 1, 2}, error);
    require(wired.has_value() && wired->timeline.score.tracks[4].outputChannels == std::vector<std::uint32_t>{1, 2},
        "any combination of hardware outputs");
    require(talas::app::timelineOutputStatus(wired->timeline.score, "trk3") == std::vector<bool>{true, true},
        "the output status shows a green square for each of the output channels");
    const auto one = talas::app::timelineWithTrackOutputs(document, "trk3", {2}, error);
    require(one.has_value() && talas::app::timelineOutputStatus(one->timeline.score, "trk3") == std::vector<bool>{false, true},
        "the Panning status is replaced with an Output channel status");
    const auto panned = talas::app::timelineWithTrackOutputs(*wired, "trk3", {}, error);
    require(panned.has_value() && panned->timeline.score.tracks[4].outputChannels.empty(),
        "Use Pan Controls switches back to panning");
    require(!talas::app::timelineWithTrackOutputs(document, "trk3", {3}, error).has_value(),
        "a track is wired to channels the Master has");
    require(!talas::project::projectScoreValidationError(one->timeline.score, std::vector<std::string_view>{"voice"}).has_value(),
        "a wired track is a valid route");
    const auto none = talas::app::timelineWithTrackNoOutput(*wired, "trk3", error);
    require(none.has_value() && none->timeline.score.tracks[4].noOutput && none->timeline.score.tracks[4].outputChannels.empty()
            && talas::app::timelineOutputStatus(none->timeline.score, "trk3") == std::vector<bool>{false, false},
        "you can also opt for no output, every output light off");
    const auto again = talas::app::timelineWithTrackOutputs(*none, "trk3", {}, error);
    require(again.has_value() && !again->timeline.score.tracks[4].noOutput, "Use Pan Controls gives the output back");
    require(!talas::app::timelineWithTrackNoOutput(document, "vox", error).has_value(), "only a Sound track has no output");
}

void testTheHardwareInput()
{
    const auto document = mix();
    const auto& score = document.timeline.score;
    require(talas::app::timelineTrackInputLabel(score, "trk3") == "--",
        "the double-dash indicates that there is no input to this track");
    std::string error;
    const auto submixed = talas::app::timelineWithTrackInput(document, "trk3", "drums", error);
    require(submixed.has_value() && talas::app::timelineTrackInputLabel(submixed->timeline.score, "trk3") == "drums name",
        "the name of the input is displayed within square brackets in place of the double-dashes");
    const auto hardware = talas::app::timelineWithTrackHardwareInput(*submixed, "trk3", 1, error);
    require(hardware.has_value() && hardware->timeline.score.tracks[4].inputChannel == 1
            && hardware->timeline.score.tracks[4].inputTrackId.empty()
            && talas::app::timelineTrackInputLabel(hardware->timeline.score, "trk3") == "Input: 1",
        "switch the audio input source from the submix to Input: 1");
    const auto back = talas::app::timelineWithTrackInput(*hardware, "trk3", "drums", error);
    require(back.has_value() && back->timeline.score.tracks[4].inputChannel == 0
            && back->timeline.score.tracks[4].inputTrackId == "drums",
        "switch the input back to the submix");
    const auto none = talas::app::timelineWithTrackInput(*hardware, "trk3", "", error);
    require(none.has_value() && none->timeline.score.tracks[4].inputChannel == 0, "No input takes the hardware input away");
    require(!talas::app::timelineWithTrackHardwareInput(document, "trk3", 9, error).has_value()
            && !talas::app::timelineWithTrackHardwareInput(document, "trk3", 0, error).has_value()
            && !talas::app::timelineWithTrackHardwareInput(document, "vox", 1, error).has_value(),
        "a Sound track takes one of the hardware's eight audio inputs");
    require(!talas::project::projectScoreValidationError(hardware->timeline.score, std::vector<std::string_view>{"voice"}).has_value(),
        "a track on a hardware input is a valid route");
}

// The Sounds a control can affect, whose spans
// its editor shows darker; and the MIDI status red while a Sound's
// parameters are being recorded.
void testTheControlsAffectedSounds()
{
    auto document = mix();
    auto& score = document.timeline.score;
    const auto lane = [](std::string id, talas::project::ScoreAutomationScope scope, std::string owner) {
        talas::project::ScoreAutomationLane result;
        result.laneId = std::move(id);
        result.scope = scope;
        result.ownerId = std::move(owner);
        result.eventName = "Rate";
        return result;
    };
    score.automation = {lane("own", talas::project::ScoreAutomationScope::Clip, "a"),
        lane("track", talas::project::ScoreAutomationScope::Track, "trk2"),
        lane("submix", talas::project::ScoreAutomationScope::Track, "vox"),
        lane("master", talas::project::ScoreAutomationScope::Master, "master")};
    using Clips = std::vector<std::string>;
    require(talas::app::automationLaneAffectedClips(score, "own") == Clips{"a"},
        "a parameter can't affect a Sound before it begins or after it ends: its own Sound's span");
    require(talas::app::automationLaneAffectedClips(score, "track") == Clips{"b"},
        "a track control shows the spans of the Sounds it could affect");
    require(talas::app::automationLaneAffectedClips(score, "submix") == Clips{"a", "b"},
        "a submix's control reaches the Sounds whose output goes through it");
    require(talas::app::automationLaneAffectedClips(score, "master") == Clips{"a", "b"}, "a master control reaches every Sound");
    require(talas::app::automationLaneAffectedClips(score, "none").empty(), "no lane, no Sounds");

    require(!talas::app::timelineClipInputStatus(document, "b").recording, "nothing is being recorded");
    score.automation[1].state = talas::project::ScoreAutomationState::Record;
    require(talas::app::timelineClipInputStatus(document, "b").recording && !talas::app::timelineClipInputStatus(document, "a").recording,
        "the MIDI status turns red over a Sound whose parameters are being recorded");
}

void testTheMidiStatus()
{
    auto document = mix();
    std::string error;
    const auto channel = talas::app::timelineWithTrackMidiChannel(document, "trk1", 4, error);
    require(channel.has_value() && channel->timeline.score.tracks[0].midiChannel == 4,
        "the MIDI status control changes the track's MIDI input channel");
    const auto back = talas::app::timelineWithTrackMidiChannel(*channel, "trk1", 0, error);
    require(back.has_value() && back->timeline.score.tracks[0].midiChannel == 0, "back to the default channel");
    require(!talas::app::timelineWithTrackMidiChannel(document, "trk1", 17, error).has_value()
            && !talas::app::timelineWithTrackMidiChannel(document, "master", 2, error).has_value(),
        "a MIDI channel is 1 to 16, for a track with Sounds");

    require(talas::app::timelineClipInputStatus(document, "a") == talas::app::TimelineInputStatus{},
        "an oscillator expects neither audio nor MIDI input");
    talas::project::ProjectSoundDefinition keys;
    keys.definitionId = "keys";
    keys.displayName = "Keys";
    talas::project::ProjectNode osc{"osc", "talas.oscillator", {}};
    talas::project::ProjectParameterExpression frequency;
    frequency.parameterId = "frequency";
    frequency.expression = "!KeyPitch";
    osc.parameters = {frequency};
    keys.graph.nodes = {osc};
    talas::project::ProjectSoundDefinition mic;
    mic.definitionId = "mic";
    mic.displayName = "Mic";
    mic.graph.nodes = {{"in", "talas.audioInput", {}}};
    talas::project::ProjectSoundDefinition both;
    both.definitionId = "both";
    both.displayName = "Both";
    talas::project::ProjectNode keysInside;
    keysInside.instanceId = "k";
    keysInside.soundDefinitionId = "keys";
    talas::project::ProjectNode micInside;
    micInside.instanceId = "m";
    micInside.soundDefinitionId = "mic";
    both.graph.nodes = {keysInside, micInside};
    document.soundDefinitions.insert(document.soundDefinitions.end(), {keys, mic, both});
    document.timeline.score.clips[0].soundDefinitionId = "keys";
    document.timeline.score.clips[1].soundDefinitionId = "mic";
    require(talas::app::timelineClipInputStatus(document, "a") == talas::app::TimelineInputStatus{false, true},
        "the MIDI indicator turns green for a Sound that expects MIDI input");
    require(talas::app::timelineClipInputStatus(document, "b") == talas::app::TimelineInputStatus{true, false},
        "the microphone turns green for a Sound that expects audio input");
    document.timeline.score.clips[1].soundDefinitionId = "both";
    require(talas::app::timelineClipInputStatus(document, "b") == talas::app::TimelineInputStatus{true, true},
        "the Sounds inside it count");
}
}

int main()
{
    try
    {
        testTheInputMenu();
        testChoosingAnInput();
        testTheSubmixMenu();
        testTheOutputMenu();
        testTheMidiStatus();
        testTheControlsAffectedSounds();
        testTheHardwareInput();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
