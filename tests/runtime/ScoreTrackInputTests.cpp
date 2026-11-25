#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectScore.h"
#include "talas/project/ProjectScoreClipFingerprint.h"
#include "talas/runtime/PreparedScore.h"
#include "talas/runtime/ProjectRuntimeBridge.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"
#include "talas/runtime/ScoreClipMaterialization.h"

#include <array>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>

// A Timeline track takes a submix of
// other tracks as its audio input, and the Sound on it processes them.
namespace
{
using talas::project::ScoreTrackKind;

void require(bool value, std::string_view message)
{
    if (!value)
        throw std::runtime_error(std::string(message));
}

const auto& registry()
{
    static const auto r = talas::core::makeBuiltinNodeRegistry();
    return r;
}

talas::project::ScoreTime seconds(double value)
{
    return {talas::project::ScoreTimeDomain::Seconds, value, 0};
}

talas::project::ScoreTrack track(std::string id, ScoreTrackKind kind, std::string output, std::string input = {})
{
    talas::project::ScoreTrack result{id, id, kind, talas::project::ScoreTimeDomain::Seconds, std::move(output), 1,
        talas::project::ScoreOverlapMode::Layer};
    result.inputTrackId = std::move(input);
    return result;
}

talas::project::ScoreClip clip(std::string id, std::string trackId, std::string sound, double start)
{
    return {std::move(id), std::move(trackId), std::move(sound), seconds(start), seconds(0.016), seconds(0), seconds(0),
        seconds(0), 1};
}

double tone(std::size_t frame)
{
    return 0.25 * std::sin(2 * std::numbers::pi * 7 * static_cast<double>(frame) / 1000);
}

// Trk 2 to the submix rev, rev to Master, and Trk 7, taking rev as its
// input, to Master: the Haiku example's reverb send, with a gain standing in
// for the reverb.
talas::project::ProjectDocument haiku(double effectStart = 0.0)
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition source;
    source.definitionId = "tone";
    source.displayName = "Tone";
    source.graph.nodes = {{"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "0.25"}}}};
    source.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    document.soundDefinitions.push_back(source);
    talas::project::ProjectSoundDefinition effect;
    effect.definitionId = "effect";
    effect.displayName = "Effect";
    effect.graph.nodes = {{"input", "talas.audioInput", {}}, {"gain", "talas.vca", {{"gain", "0.5"}}}};
    effect.graph.connections = {{"input", "audioOut", "gain", "audioIn"}};
    effect.outputs = {{"audioOut", talas::core::PortType::Audio, "gain", "audioOut"}};
    document.soundDefinitions.push_back(effect);
    document.timeline.score.tracks = {
        track("trk2", ScoreTrackKind::Track, "rev"),
        track("rev", ScoreTrackKind::Submix, "master"),
        track("trk7", ScoreTrackKind::Track, "master", "rev"),
        track("master", ScoreTrackKind::Master, {}),
    };
    document.timeline.score.clips = {clip("vocal", "trk2", "tone", 0.0), clip("reverb", "trk7", "effect", effectStart)};
    return document;
}

std::vector<std::string_view> soundIds(const talas::project::ProjectDocument& document)
{
    std::vector<std::string_view> ids;
    for (const auto& definition : document.soundDefinitions)
        ids.push_back(definition.definitionId);
    return ids;
}

std::optional<talas::project::ScoreValidationError> validation(const talas::project::ProjectDocument& document)
{
    const auto ids = soundIds(document);
    return talas::project::projectScoreValidationError(document.timeline.score, ids);
}

talas::runtime::RenderSettings settings(const talas::project::ProjectDocument& document)
{
    talas::runtime::RenderSettings result;
    result.sampleRate = 1000;
    result.maxBlockFrames = 8;
    const auto prepared = talas::runtime::prepareProjectScore(
        document, talas::runtime::preparedScoreBindingForDocument(document, 1, 1, 1000, 1));
    require(prepared.ok(), prepared.error);
    result.preparedScore = prepared.schedule;
    return result;
}

void testTheInputChoices()
{
    auto document = haiku();
    require(!validation(document).has_value(), "Trk 7 takes the submix rev as its input");
    const auto& score = document.timeline.score;
    require(talas::project::scoreTrackInputFeeders(score, "trk7") == std::vector<std::string>{"trk2"},
        "the tracks routed to rev feed Trk 7's input");
    require(talas::project::scoreTrackInputChoices(score, "trk7") == std::vector<std::string>{"rev"},
        "the input menu lists the submixes");
    require(talas::project::scoreTrackInputChoices(score, "trk2").empty(),
        "a submix a track's output goes to is absent from its inputs");

    auto self = document;
    self.timeline.score.tracks[0].inputTrackId = "rev";
    const auto selfError = validation(self);
    require(selfError.has_value() && selfError->code == talas::project::ScoreValidationCode::RouteCycle,
        "a track cannot take the submix of its own output as input");

    // Trk 7 to a second submix taken by Trk 2: Trk 2's output would come back.
    auto loop = document;
    loop.timeline.score.tracks.push_back(track("aux", ScoreTrackKind::Submix, "master"));
    loop.timeline.score.tracks[2].outputTrackId = "aux";
    require(!validation(loop).has_value(), "a second submix fed by Trk 7");
    require(talas::project::scoreTrackInputChoices(loop.timeline.score, "trk2").empty(),
        "a submix fed through the tracks a track feeds is absent too");
    require(talas::project::scoreTrackFeeds(loop.timeline.score, "trk2", "trk7"), "Trk 2 feeds Trk 7");
    loop.timeline.score.tracks[0].inputTrackId = "aux";
    require(validation(loop).has_value(), "an input that brings a track's output back is rejected");

    auto toTrack = document;
    toTrack.timeline.score.tracks[2].inputTrackId = "trk2";
    require(validation(toTrack).has_value(), "an input is a submix, not a Sound track");
    auto onSubmix = document;
    onSubmix.timeline.score.tracks[1].inputTrackId = "rev";
    require(validation(onSubmix).has_value(), "only a Sound track has an audio input");
}

void testTheInputIsSaved()
{
    const auto document = haiku();
    const auto json = talas::project::serializeProjectDocument(document, registry());
    require(json.find("\"inputTrackId\": \"rev\"") != std::string::npos, "the input is written with the track");
    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok() && loaded.document.timeline.score.tracks[2].inputTrackId == "rev"
            && loaded.document.timeline.score.tracks[0].inputTrackId.empty(),
        "the input loads back");
    auto older = json;
    const auto version = older.find("\"version\": 156");
    require(version != std::string::npos, "the fixture is written at the current format");
    older.replace(version, std::string_view{"\"version\": 156"}.size(), "\"version\": 126");
    require(!talas::project::parseProjectDocument(older).ok(), "format 126 has no track input");
}

void testTheInputTrackProcessesTheSubmix()
{
    for (const auto effectStart : {0.0, 0.004})
    {
        const auto document = haiku(effectStart);
        const auto configured = settings(document);
        const auto materialized = talas::runtime::materializeProjectScoreClips(document, registry(), configured);
        require(materialized.ok() && materialized.clips.size() == 2, "materialize the Haiku clips: " + materialized.error);
        const auto& wet = *materialized.clips[1]->channels[0];
        const auto offset = static_cast<std::size_t>(effectStart * 1000 + 0.5);
        for (std::size_t frame = 0; frame < wet.size(); ++frame)
        {
            const auto expected = frame + offset < 16 ? 0.5 * tone(frame + offset) : 0.0;
            require(std::abs(wet[frame] - expected) < 1e-9,
                "EuverbMono on Trk 7 processes rev at frame " + std::to_string(frame));
        }
    }
}

void testTheTimelinePlaysDryAndProcessed()
{
    const auto document = haiku();
    const auto configured = settings(document);
    auto compiled = talas::runtime::compileProject(document, registry(), configured);
    require(compiled.ok(), "compile the Haiku Timeline");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), session.error());
    std::array<double, 8> output{};
    double* outputs[] = {output.data()};
    for (std::size_t block = 0; block < 2; ++block)
    {
        require(session.render({}, outputs, 1, 8), "render the Haiku Timeline");
        for (std::size_t index = 0; index < 8; ++index)
        {
            const auto frame = block * 8 + index;
            require(std::abs(output[index] - 1.5 * tone(frame)) < 1e-9,
                "the Timeline plays Trk 2 through rev and processed on Trk 7 at frame " + std::to_string(frame));
        }
    }
}

void testAGroupOfTracksIsProcessedTogether()
{
    // Each track of the group sent to the same submix, the submix
    // the input to a new track.
    auto document = haiku();
    document.soundDefinitions.push_back(document.soundDefinitions[0]);
    document.soundDefinitions.back().definitionId = "octave";
    document.soundDefinitions.back().graph.nodes[0].parameters[0].expression = "14 hz";
    document.timeline.score.tracks.insert(document.timeline.score.tracks.begin() + 1, track("trk3", ScoreTrackKind::Track, "rev"));
    document.timeline.score.clips.push_back(clip("harmony", "trk3", "octave", 0.0));
    const auto materialized = talas::runtime::materializeProjectScoreClips(document, registry(), settings(document));
    require(materialized.ok() && materialized.clips.size() == 3, "materialize the group: " + materialized.error);
    const auto& wet = *materialized.clips[1]->channels[0];
    for (std::size_t frame = 0; frame < wet.size(); ++frame)
    {
        const auto octave = 0.25 * std::sin(2 * std::numbers::pi * 14 * static_cast<double>(frame) / 1000);
        require(std::abs(wet[frame] - 0.5 * (tone(frame) + octave)) < 1e-9,
            "the processing on the new track applies to the whole group at frame " + std::to_string(frame));
    }
}

void testAMarkedSoundTakesItsInputTracks()
{
    const auto document = haiku();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    talas::runtime::RenderSettings configured;
    configured.sampleRate = 1000;
    configured.maxBlockFrames = 8;
    configured.playedScoreClipIds = std::vector<std::string>{"reverb"};
    const auto staged = bridge.compileAndStage(configured);
    require(staged.ok(), "stage the marked EuverbMono: " + staged.message);
    bridge.setPlaying(true);
    for (std::size_t block = 0; block < 2; ++block)
    {
        std::array<float, 8> output{};
        float* outputs[] = {output.data()};
        require(bridge.render(outputs, 1, output.size()), "render the marked Sound");
        for (std::size_t index = 0; index < 8; ++index)
        {
            const auto frame = block * 8 + index;
            require(std::abs(output[index] - 0.5 * tone(frame)) < 1e-6,
                "the submix is heard through the marked Sound, without the dry tracks, at frame "
                    + std::to_string(frame));
        }
    }
    const auto played = talas::project::scoreWithPlayedClips(document.timeline.score, {"reverb"});
    require(played.clips.size() == 2 && played.tracks[0].muted && !played.tracks[2].muted,
        "the Sounds on the marked Sound's input tracks compile with it");
    const auto both = talas::project::scoreWithPlayedClips(document.timeline.score, {"reverb", "vocal"});
    require(!both.tracks[0].muted, "a submixed Sound marked too is heard dry");
}

// Trk 1 heard on its own output and sent to vox, which Trk 3 takes
// as its input.
talas::project::ProjectDocument vox()
{
    auto document = haiku();
    auto& tracks = document.timeline.score.tracks;
    tracks[0].outputTrackId = "master";
    tracks[0].sendTrackIds = {"rev"};
    return document;
}

void testSendsFeedTheInput()
{
    auto document = vox();
    require(!validation(document).has_value(), "a track's output goes to Master and is sent to a submix too");
    require(talas::project::scoreTrackInputFeeders(document.timeline.score, "trk7") == std::vector<std::string>{"trk2"},
        "the track sent to rev feeds Trk 7");
    require(talas::project::scoreTrackInputChoices(document.timeline.score, "trk2").empty(),
        "a submix the track sends to is absent from its inputs");

    const auto materialized = talas::runtime::materializeProjectScoreClips(document, registry(), settings(document));
    require(materialized.ok(), "materialize the send: " + materialized.error);
    for (std::size_t frame = 0; frame < 16; ++frame)
        require(std::abs((*materialized.clips[1]->channels[0])[frame] - 0.5 * tone(frame)) < 1e-9,
            "the submix a track sends to is the input of another track");

    // Trk2_To_rev at half sends half of Trk 2.
    talas::project::ScoreAutomationLane level;
    level.laneId = "send";
    level.ownerId = "trk2";
    level.eventName = talas::project::scoreSendLevelName("rev");
    level.points = {{seconds(0.0), 0.5, talas::core::AutomationInterpolation::Step}};
    document.timeline.score.automation.push_back(level);
    require(!validation(document).has_value() && level.eventName == "To_rev", "the send level is a track lane");
    auto compiled = talas::runtime::compileProject(document, registry(), settings(document));
    require(compiled.ok(), "compile the send");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), session.error());
    std::array<double, 8> output{};
    double* outputs[] = {output.data()};
    for (std::size_t block = 0; block < 2; ++block)
    {
        require(session.render({}, outputs, 1, 8), "render the send");
        for (std::size_t index = 0; index < 8; ++index)
        {
            const auto frame = block * 8 + index;
            require(std::abs(output[index] - 1.25 * tone(frame)) < 1e-9,
                "Trk 2 dry at its level and sent at its send level, at frame " + std::to_string(frame));
        }
    }
}

// Trk 2 with No output, heard only through the submix rev it sends
// to, as Trk 7's input.
void testATrackWithNoOutput()
{
    auto document = vox();
    document.timeline.score.tracks[0].noOutput = true;
    require(!validation(document).has_value(), "you can also opt for no output");
    require(talas::project::scoreTrackInputFeeders(document.timeline.score, "trk7") == std::vector<std::string>{"trk2"},
        "the track output is routed to submixes instead");
    auto compiled = talas::runtime::compileProject(document, registry(), settings(document));
    require(compiled.ok(), "compile the track with no output");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), session.error());
    std::array<double, 8> output{};
    double* outputs[] = {output.data()};
    for (std::size_t block = 0; block < 2; ++block)
    {
        require(session.render({}, outputs, 1, 8), "render the track with no output");
        for (std::size_t index = 0; index < 8; ++index)
        {
            const auto frame = block * 8 + index;
            require(std::abs(output[index] - 0.5 * tone(frame)) < 1e-9,
                "only the submix Trk 2 sends to is heard, through Trk 7, at frame " + std::to_string(frame));
        }
    }
    // Its output route to a submix feeds nothing either.
    auto routed = haiku();
    routed.timeline.score.tracks[0].noOutput = true;
    require(talas::project::scoreTrackInputFeeders(routed.timeline.score, "trk7").empty(),
        "a track with no output does not reach the submix its output goes to");
    auto onSubmix = haiku();
    onSubmix.timeline.score.tracks[1].noOutput = true;
    require(validation(onSubmix).has_value(), "only a Sound track has no output");
    const auto loaded = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document, registry()));
    require(loaded.ok() && loaded.document.timeline.score.tracks[0].noOutput, "No output is saved");
}

// Trk 2 sent to rev, which Trk 7 takes: (Trk2Out * Trk2_To_rev)
// feeds rev before Trk 2's Level, and Trk 7's Level scales what it makes.
void testASendIsTakenBeforeTheLevel()
{
    auto document = vox();
    const auto level = [](std::string owner, double value) {
        talas::project::ScoreAutomationLane lane;
        lane.laneId = "level_" + owner;
        lane.ownerId = std::move(owner);
        lane.eventName = "Level";
        lane.points = {{seconds(0.0), value, talas::core::AutomationInterpolation::Step}};
        return lane;
    };
    document.timeline.score.automation = {level("trk2", 0.0), level("trk7", 0.5)};
    auto compiled = talas::runtime::compileProject(document, registry(), settings(document));
    require(compiled.ok(), "compile the levelled send");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), session.error());
    std::array<double, 8> output{};
    double* outputs[] = {output.data()};
    for (std::size_t block = 0; block < 2; ++block)
    {
        require(session.render({}, outputs, 1, 8), "render the levelled send");
        for (std::size_t index = 0; index < 8; ++index)
        {
            const auto frame = block * 8 + index;
            require(std::abs(output[index] - 0.25 * tone(frame)) < 1e-9,
                "Level_Trk2 has no effect on rev, Level_Trk7 does, at frame " + std::to_string(frame));
        }
    }
}

void testSendsAreChecked()
{
    auto onRoute = haiku();
    onRoute.timeline.score.tracks[0].sendTrackIds = {"rev"};
    require(validation(onRoute).has_value(), "a track already routed to rev does not send there too");
    auto twice = vox();
    twice.timeline.score.tracks[0].sendTrackIds = {"rev", "rev"};
    require(validation(twice).has_value(), "a track sends to a submix once");
    auto toTrack = vox();
    toTrack.timeline.score.tracks[0].sendTrackIds = {"trk7"};
    require(validation(toTrack).has_value(), "a track sends to submixes only");
    auto loop = vox();
    loop.timeline.score.tracks[2].sendTrackIds = {"rev"};
    const auto loopError = validation(loop);
    require(loopError.has_value() && loopError->code == talas::project::ScoreValidationCode::RouteCycle,
        "Trk 7 cannot send to the submix it takes as input");

    const auto document = vox();
    const auto json = talas::project::serializeProjectDocument(document, registry());
    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok() && loaded.document.timeline.score.tracks[0].sendTrackIds == std::vector<std::string>{"rev"},
        "the sends are saved with the track");
    auto older = json;
    const auto version = older.find("\"version\": 156");
    older.replace(version, std::string_view{"\"version\": 156"}.size(), "\"version\": 127");
    require(!talas::project::parseProjectDocument(older).ok(), "format 127 has no sends");
}

// EuverbMono, its input Mixer206 in the Sound, on Trk 7 takes
// rev in place of Mixer206; on a track with no input it keeps Mixer206.
void testTheTrackInputReplacesTheSoundsInput()
{
    auto document = haiku();
    talas::project::ProjectSoundDefinition euverb;
    euverb.definitionId = "euverb";
    euverb.displayName = "EuverbMono";
    euverb.graph.nodes = {{"gain", "talas.vca", {{"gain", "0.5"}}}};
    euverb.inputs = {{"sourceIn", talas::core::PortType::Audio, "gain", "audioIn"}};
    euverb.outputs = {{"audioOut", talas::core::PortType::Audio, "gain", "audioOut"}};
    document.soundDefinitions.push_back(euverb);
    auto mixer = document.soundDefinitions[0];
    mixer.definitionId = "mixer206";
    mixer.graph.nodes[0].parameters[0].expression = "30 hz";
    document.soundDefinitions.push_back(mixer);
    talas::project::ProjectNode mixerNode;
    mixerNode.instanceId = "Mixer206";
    mixerNode.soundDefinitionId = "mixer206";
    talas::project::ProjectNode effectNode;
    effectNode.instanceId = "EuverbMono";
    effectNode.soundDefinitionId = "euverb";
    document.graph.nodes = {mixerNode, effectNode};
    document.graph.connections = {{"Mixer206", "audioOut", "EuverbMono", "sourceIn"}};
    document.timeline.score.clips[1].soundDefinitionId = "euverb";
    document.timeline.score.clips[1].sourceInstanceId = "EuverbMono";
    document.timeline.score.tracks.insert(document.timeline.score.tracks.end() - 1, track("trk8", ScoreTrackKind::Track, "master"));
    auto dry = clip("dry", "trk8", "euverb", 0.0);
    dry.sourceInstanceId = "EuverbMono";
    document.timeline.score.clips.push_back(dry);
    auto unbound = clip("unbound", "trk7", "euverb", 0.016);
    document.timeline.score.clips.push_back(unbound);
    document.timeline.score.clips[0].repeatCount = 2;
    const auto before = document;
    const auto materialized = talas::runtime::materializeProjectScoreClips(document, registry(), settings(document));
    require(materialized.ok() && materialized.clips.size() == 4, "materialize EuverbMono on both tracks: " + materialized.error);
    for (std::size_t frame = 0; frame < 16; ++frame)
    {
        const auto mixed = 0.25 * std::sin(2 * std::numbers::pi * 30 * static_cast<double>(frame) / 1000);
        require(std::abs((*materialized.clips[1]->channels[0])[frame] - 0.5 * tone(frame)) < 1e-9,
            "on a track with an input, the track's input replaces Mixer206");
        require(std::abs((*materialized.clips[2]->channels[0])[frame] - 0.5 * mixed) < 1e-9,
            "on a track with no input, the Sound keeps its own input");
        // The vocal's second repeat starts its Sound again.
        require(std::abs((*materialized.clips[3]->channels[0])[frame] - 0.5 * tone(frame)) < 1e-9,
            "a Sound with no input of its own in the project takes the track's");
    }
    require(document.soundDefinitions == before.soundDefinitions && document.graph == before.graph,
        "the track input does not change the Sounds in a permanent way");
    std::string error;
    require(!talas::project::projectScoreClipSourceFingerprint(document, registry(), "unbound", 1000, error).empty(),
        "the Sound taking the track's input has a cache fingerprint: " + error);
}

// Trk 7's input switched from the submix to Input: 2, the
// hardware's second audio input, in place of the Sound's own input and of
// a replaceable input fed in the project.
void testAHardwareInputFeedsTheTrack()
{
    for (const auto replaceable : {false, true})
    {
        auto document = haiku();
        auto& tracks = document.timeline.score.tracks;
        tracks[2].inputTrackId.clear();
        tracks[2].inputChannel = 2;
        document.timeline.score.clips.erase(document.timeline.score.clips.begin());
        if (replaceable)
        {
            talas::project::ProjectSoundDefinition euverb;
            euverb.definitionId = "euverb";
            euverb.displayName = "EuverbMono";
            euverb.graph.nodes = {{"gain", "talas.vca", {{"gain", "0.5"}}}};
            euverb.inputs = {{"sourceIn", talas::core::PortType::Audio, "gain", "audioIn"}};
            euverb.outputs = {{"audioOut", talas::core::PortType::Audio, "gain", "audioOut"}};
            document.soundDefinitions.push_back(euverb);
            talas::project::ProjectNode toneNode;
            toneNode.instanceId = "Tone";
            toneNode.soundDefinitionId = "tone";
            talas::project::ProjectNode effectNode;
            effectNode.instanceId = "EuverbMono";
            effectNode.soundDefinitionId = "euverb";
            document.graph.nodes = {toneNode, effectNode};
            document.graph.connections = {{"Tone", "audioOut", "EuverbMono", "sourceIn"}};
            document.timeline.score.clips[0].soundDefinitionId = "euverb";
            document.timeline.score.clips[0].sourceInstanceId = "EuverbMono";
        }
        require(!validation(document).has_value(), "a Sound track takes one of the hardware's inputs");
        talas::runtime::ProjectRuntimeBridge bridge(document, registry());
        talas::runtime::RenderSettings configured;
        configured.sampleRate = 1000;
        configured.maxBlockFrames = 8;
        const auto staged = bridge.compileAndStage(configured);
        require(staged.ok(), "stage the track on Input: 2: " + staged.message
            + (staged.diagnostics.empty() ? std::string{} : " / " + staged.diagnostics.front().message));
        bridge.setPlaying(true);
        std::array<float, 8> first{}, second{}, output{};
        first[2] = 1.0F;
        second[5] = -0.75F;
        const float* inputs[] = {first.data(), second.data()};
        float* outputs[] = {output.data()};
        require(bridge.render({inputs, 2}, outputs, 1, output.size()), "render the hardware input");
        for (std::size_t frame = 0; frame < output.size(); ++frame)
            require(std::abs(output[frame] - (frame == 5 ? -0.375F : 0.0F)) < 1e-6F,
                std::string{"the track's Sound processes Input: 2"} + (replaceable ? " in place of its input" : "")
                    + " at frame " + std::to_string(frame));
    }
    auto both = haiku();
    both.timeline.score.tracks[2].inputChannel = 1;
    require(validation(both).has_value(), "a track takes a submix or a hardware input, not both");
    auto json = haiku();
    json.timeline.score.tracks[2].inputTrackId.clear();
    json.timeline.score.tracks[2].inputChannel = 4;
    const auto loaded = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(json, registry()));
    require(loaded.ok() && loaded.document.timeline.score.tracks[2].inputChannel == 4, "the hardware input is saved");
}

void testFeedingSoundsChangeTheCache()
{
    auto document = haiku();
    document.soundDefinitions.push_back(document.soundDefinitions[0]);
    document.soundDefinitions.back().definitionId = "other";
    document.timeline.score.tracks.insert(document.timeline.score.tracks.begin(), track("trk9", ScoreTrackKind::Track, "master"));
    document.timeline.score.clips.push_back(clip("unrelated", "trk9", "other", 0.0));
    std::string error;
    const auto print = [&](const talas::project::ProjectDocument& candidate) {
        return talas::project::projectScoreClipSourceFingerprint(candidate, registry(), "reverb", 1000, error);
    };
    const auto before = print(document);
    require(!before.empty(), "fingerprint the processed clip: " + error);
    auto changedFeeder = document;
    changedFeeder.soundDefinitions[0].graph.nodes[0].parameters[1].expression = "0.5";
    require(print(changedFeeder) != before, "a change to a Sound feeding the submix invalidates the cache");
    auto changedOther = document;
    changedOther.soundDefinitions.back().graph.nodes[0].parameters[1].expression = "0.5";
    require(print(changedOther) == before, "a Sound on a track outside the submix leaves the cache");
}
}

int main()
{
    try
    {
        testTheInputChoices();
        testTheInputIsSaved();
        testTheInputTrackProcessesTheSubmix();
        testTheTimelinePlaysDryAndProcessed();
        testAGroupOfTracksIsProcessedTogether();
        testAMarkedSoundTakesItsInputTracks();
        testSendsFeedTheInput();
        testSendsAreChecked();
        testTheTrackInputReplacesTheSoundsInput();
        testFeedingSoundsChangeTheCache();
        testAHardwareInputFeedsTheTrack();
        testATrackWithNoOutput();
        testASendIsTakenBeforeTheLevel();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
