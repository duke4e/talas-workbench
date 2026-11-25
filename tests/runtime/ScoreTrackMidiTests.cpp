#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectScore.h"
#include "talas/project/ProjectScoreClipFingerprint.h"
#include "talas/runtime/PreparedScore.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

// A Timeline track's MIDI input channel,
// on which the Sounds in the track expect to receive MIDI, and the Notes
// that are always live: the Sound under the time cursor plays from a MIDI
// keyboard.
namespace
{
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

talas::project::ProjectParameterExpression expression(std::string id, std::string text)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(text);
    return parameter;
}

// A MIDIVoice on the default channel (0) whose voice is its key's
// KeyDown, so the Sound sounds 1 while a key is down.
talas::project::ProjectDocument keyboard(std::uint8_t trackChannel = 0, std::string voiceChannel = "0")
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition keys;
    keys.definitionId = "keys";
    keys.displayName = "Keys";
    talas::project::ProjectParameterExpression source;
    source.parameterId = "source";
    source.text = "";
    talas::project::ProjectParameterExpression script;
    script.parameterId = "script";
    script.text = "";
    keys.graph.nodes = {
        {"midi", "talas.midiVoice", {expression("left", "1"), expression("right", "1"),
            expression("channel", std::move(voiceChannel)), source, expression("polyphony", "1"),
            expression("lowPitch", "0 nn"), expression("highPitch", "127 nn"), script}},
        {"level", "talas.valToSound", {expression("value", "!KeyDown")}},
    };
    keys.graph.connections = {{"level", "audioOut", "midi", "audioIn"}};
    keys.outputs = {{"audioOut", talas::core::PortType::Audio, "midi", "leftOut"}};
    document.soundDefinitions = {keys};
    talas::project::ScoreTrack track{"track", "Track", talas::project::ScoreTrackKind::Track,
        talas::project::ScoreTimeDomain::Seconds, "master", 1, talas::project::ScoreOverlapMode::Layer};
    track.midiChannel = trackChannel;
    document.timeline.score.tracks = {track,
        {"master", "Master", talas::project::ScoreTrackKind::Master, talas::project::ScoreTimeDomain::Seconds, {}, 1,
            talas::project::ScoreOverlapMode::Layer}};
    document.timeline.score.clips = {{"clip", "track", "keys", seconds(0), seconds(1), seconds(0), seconds(0),
        seconds(0), 1}};
    return document;
}

struct Player
{
    explicit Player(const talas::project::ProjectDocument& document)
        : bridge(document, registry())
    {
        talas::runtime::RenderSettings configured;
        configured.sampleRate = 1000;
        configured.maxBlockFrames = 8;
        const auto staged = bridge.compileAndStage(configured);
        require(staged.ok(), "stage the Timeline: " + staged.message
            + (staged.diagnostics.empty() ? std::string{} : " / " + staged.diagnostics.front().message));
        bridge.setPlaying(true);
    }

    std::array<float, 8> block()
    {
        std::array<float, 8> output{};
        float* outputs[] = {output.data()};
        require(bridge.render(outputs, 1, output.size()), "render the Timeline");
        return output;
    }

    talas::runtime::ProjectRuntimeBridge bridge;
    double clock = 1.0;
};

bool silent(const std::array<float, 8>& output)
{
    for (const auto sample : output)
    {
        if (sample != 0.0F)
            return false;
    }
    return true;
}

bool sounding(const std::array<float, 8>& output)
{
    return std::abs(output.back() - 1.0F) < 1e-6F;
}

void testTheSoundUnderTheCursorPlaysFromTheKeyboard()
{
    Player player(keyboard());
    require(silent(player.block()), "no key, no sound");
    require(player.bridge.enqueueMidiNoteOn(1, 60, 100, player.clock), "a key down on channel 1");
    auto heard = player.block();
    for (std::size_t attempt = 0; attempt < 3 && !sounding(heard); ++attempt)
        heard = player.block();
    require(sounding(heard), "Notes is always live, the track's Sound plays from a MIDI keyboard");
    require(player.bridge.enqueueMidiNoteOff(1, 60, player.clock + 1), "the key up");
    auto released = player.block();
    for (std::size_t attempt = 0; attempt < 3 && !silent(released); ++attempt)
        released = player.block();
    require(silent(released), "the key up releases the Sound");
}

// A Sound reading the keyboard's !KeyDown without a MIDIVoice of its own.
void testAKeyReadingSoundPlays()
{
    auto document = keyboard(2);
    auto& keys = document.soundDefinitions[0];
    keys.graph.nodes = {{"level", "talas.valToSound", {expression("value", "!KeyDown")}}};
    keys.graph.connections.clear();
    keys.outputs = {{"audioOut", talas::core::PortType::Audio, "level", "audioOut"}};
    Player player(document);
    require(silent(player.block()), "no key, no sound");
    require(player.bridge.enqueueMidiNoteOn(1, 60, 100, player.clock), "a key down on channel 1");
    for (std::size_t block = 0; block < 4; ++block)
        require(silent(player.block()), "a Sound reading the keyboard ignores other channels");
    require(player.bridge.enqueueMidiNoteOn(2, 60, 100, player.clock + 1), "a key down on channel 2");
    auto heard = player.block();
    for (std::size_t attempt = 0; attempt < 3 && !sounding(heard); ++attempt)
        heard = player.block();
    require(sounding(heard), "a Sound reading the keyboard plays on its track's channel");
    require(player.bridge.enqueueMidiNoteOn(2, 64, 100, player.clock + 2)
            && player.bridge.enqueueMidiNoteOff(2, 60, player.clock + 3),
        "a second key down, then the first one up");
    for (std::size_t block = 0; block < 4; ++block)
        require(sounding(player.block()), "the keyboard follows the last key down");
    require(player.bridge.enqueueMidiNoteOff(2, 64, player.clock + 4), "the last key up");
    auto released = player.block();
    for (std::size_t attempt = 0; attempt < 3 && !silent(released); ++attempt)
        released = player.block();
    require(silent(released), "the last key up releases it");
    // Without a MIDIVoice the Sound is one voice:
    // two keys held at once play it once, not a voice each.
    require(player.bridge.enqueueMidiNoteOn(2, 60, 100, player.clock + 5)
            && player.bridge.enqueueMidiNoteOn(2, 67, 100, player.clock + 6),
        "two keys down at once");
    auto both = player.block();
    for (std::size_t attempt = 0; attempt < 3 && !sounding(both); ++attempt)
        both = player.block();
    for (std::size_t block = 0; block < 4; ++block, both = player.block())
        require(sounding(both), "two keys held should play the one voice once");
}

// A key held before the Sound starts is not its key.
void testOnlyTheSoundUnderTheCursor()
{
    auto document = keyboard();
    document.timeline.score.clips[0].start = seconds(0.032);
    Player player(document);
    require(player.bridge.enqueueMidiNoteOn(1, 60, 100, player.clock), "a key down before the Sound");
    for (std::size_t block = 0; block < 6; ++block)
        require(silent(player.block()), "the key down reaches only the Sound under the cursor");
    require(player.bridge.enqueueMidiNoteOff(1, 60, player.clock + 1)
            && player.bridge.enqueueMidiNoteOn(1, 62, 100, player.clock + 2),
        "a key down under the Sound");
    auto heard = player.block();
    for (std::size_t attempt = 0; attempt < 3 && !sounding(heard); ++attempt)
        heard = player.block();
    require(sounding(heard), "the Sound under the cursor plays");
}

void testTheTracksMidiChannel()
{
    {
        Player player(keyboard(3));
        (void) player.block();
        require(player.bridge.enqueueMidiNoteOn(1, 60, 100, player.clock), "a key down on channel 1");
        for (std::size_t attempt = 0; attempt < 4; ++attempt)
            require(silent(player.block()), "a track on channel 3 ignores channel 1");
    }
    Player player(keyboard(3));
    (void) player.block();
    require(player.bridge.enqueueMidiNoteOn(3, 60, 100, player.clock), "a key down on channel 3");
    auto heard = player.block();
    for (std::size_t attempt = 0; attempt < 3 && !sounding(heard); ++attempt)
        heard = player.block();
    require(sounding(heard), "the Sounds in the track receive MIDI on the track's channel");

    Player fixed(keyboard(3, "1"));
    (void) fixed.block();
    require(fixed.bridge.enqueueMidiNoteOn(1, 60, 100, fixed.clock), "a key down on channel 1");
    heard = fixed.block();
    for (std::size_t attempt = 0; attempt < 3 && !sounding(heard); ++attempt)
        heard = fixed.block();
    require(sounding(heard), "a MIDIVoice with a channel of its own keeps it");
}

void testTheChannelIsSaved()
{
    const auto document = keyboard(5);
    const auto json = talas::project::serializeProjectDocument(document, registry());
    require(json.find("\"midiChannel\": 5") != std::string::npos, "the channel is written with the track");
    const auto reloaded = talas::project::parseProjectDocument(json);
    require(reloaded.ok() && reloaded.document.timeline.score.tracks[0].midiChannel == 5,
        "the track's MIDI input channel is saved");
    require(talas::project::serializeProjectDocument(keyboard(), registry()).find("midiChannel") == std::string::npos,
        "a track on the default channel writes none");
    auto invalid = document;
    invalid.timeline.score.tracks[0].midiChannel = 17;
    const std::vector<std::string_view> ids{"keys"};
    require(talas::project::projectScoreValidationError(invalid.timeline.score, ids).has_value(),
        "a MIDI channel is 1 to 16");
}

void testTheChannelChangesTheCache()
{
    std::string error;
    const auto first = talas::project::projectScoreClipSourceFingerprint(keyboard(), registry(), "clip", 1000, error);
    const auto second = talas::project::projectScoreClipSourceFingerprint(keyboard(2), registry(), "clip", 1000, error);
    require(!first.empty() && !second.empty() && first != second,
        "a Sound's cache follows the channel it expects MIDI on: " + error);
}
}

int main()
{
    try
    {
        testTheSoundUnderTheCursorPlaysFromTheKeyboard();
        testOnlyTheSoundUnderTheCursor();
        testAKeyReadingSoundPlays();
        testTheTracksMidiChannel();
        testTheChannelIsSaved();
        testTheChannelChangesTheCache();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
