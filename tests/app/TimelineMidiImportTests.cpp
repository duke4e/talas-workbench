#include "talas/app/TimelineMidiImport.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// A format 0 file at 480 ticks a quarter note: two eighth notes and their
// rests, 960 ticks, a second at the default 120 beats a minute; `tempo`
// adds a tempo of one quarter note a second at the start.
std::vector<std::uint8_t> twoNotes(bool tempo)
{
    std::vector<std::uint8_t> track;
    if (tempo)
        track.insert(track.end(), {0x00, 0xFF, 0x51, 0x03, 0x0F, 0x42, 0x40});
    track.insert(track.end(), {0x00, 0x90, 69, 100, 0x81, 0x70, 0x80, 69, 0, 0x81, 0x70, 0x90, 81, 90, 0x81, 0x70,
        0x80, 81, 0, 0x00, 0xFF, 0x2F, 0x00});
    std::vector<std::uint8_t> file{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0, 'M', 'T', 'r', 'k', 0, 0, 0,
        static_cast<std::uint8_t>(track.size())};
    file.insert(file.end(), track.begin(), track.end());
    return file;
}

void testFileLength()
{
    const auto plain = talas::app::standardMidiFileSeconds(twoNotes(false));
    require(plain.has_value() && std::abs(*plain - 0.75) < 1e-12,
        "three eighth notes of 240 ticks at 120 beats a minute should last 0.75 s");
    const auto slow = talas::app::standardMidiFileSeconds(twoNotes(true));
    require(slow.has_value() && std::abs(*slow - 1.5) < 1e-12, "a tempo of a quarter note a second should double it");
    auto broken = twoNotes(false);
    broken[0] = 'X';
    require(!talas::app::standardMidiFileSeconds(broken).has_value()
            && !talas::app::standardMidiFileSeconds(std::vector<std::uint8_t>(10, 0)).has_value(),
        "bytes that are not a Standard MIDI File have no length");
}

// A MIDI file imported into the Timeline is a Sound of a MIDIVoice playing
// it, on a new track for the file's length.
void testImportEdits()
{
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    talas::project::ProjectEditor editor(talas::project::ProjectDocument{}, registry);
    const auto sound = talas::app::midiFileTimelineSound(editor.document(), "/songs/omino.mid");
    const auto* voice = &sound.definition.graph.nodes[1];
    require(sound.definition.definitionId == "midiFile1" && sound.definition.displayName == "omino"
            && voice->typeId == "talas.midiVoice" && sound.resources.size() == 1
            && sound.resources.front().parameterId == "midiFile",
        "the imported Sound should be a MIDIVoice whose midiFile is the file");
    const auto added = editor.addSoundDefinition(sound.definition);
    require(added.ok(), "the imported Sound should be valid: " + added.message);
    const auto edits = talas::app::midiFileTimelineEdits(editor.document(), "midiFile1", "omino", 0.75);
    require(edits.tracks.size() == 2 && edits.tracks[0].kind == talas::project::ScoreTrackKind::Master
            && edits.clips.size() == 1 && edits.clips[0].duration.seconds == 0.75,
        "an empty Timeline should gain a Master, a track and a clip as long as the file");
    const auto applied = editor.applyScoreEdits(edits);
    require(applied.ok(), "the import's Timeline edits should apply: " + applied.message);
    const auto second = talas::app::midiFileTimelineEdits(editor.document(), "midiFile1", "omino", 0.75);
    require(second.tracks.size() == 1 && second.tracks[0].trackId == "midiTrack2"
            && second.tracks[0].outputTrackId == edits.tracks[0].trackId,
        "a second import should add a track to the existing Master");
}
}

int main()
{
    try
    {
        testFileLength();
        testImportEdits();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "timeline MIDI import tests passed\n";
    return 0;
}
