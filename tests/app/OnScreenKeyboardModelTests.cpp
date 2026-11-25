#include "talas/app/OnScreenKeyboardModel.h"

#include <algorithm>
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

using Note = talas::app::OnScreenKeyboardNote;

// The Fake Keyboard's pitch buttons send MIDI
// key events to the Sound, as a MIDI keyboard would.
void testKeysSendNoteEventsOnTheirChannel()
{
    talas::app::OnScreenKeyboardModel keyboard;
    require(keyboard.channel() == 1 && keyboard.velocity() == 100,
        "the keyboard should start on channel 1 at velocity 100");
    require(keyboard.press(60) == Note{1, 60, 100},
        "pressing a key should send its note-on");
    require(!keyboard.press(60).has_value(),
        "a held key should not send a second note-on");
    keyboard.setVelocity(127);
    require(keyboard.press(64) == Note{1, 64, 127},
        "a key should send the chosen velocity");
    require(keyboard.release(60) == Note{1, 60, 0},
        "releasing a key should send its note-off");
    require(!keyboard.release(60).has_value(),
        "a released key should not send another note-off");
    keyboard.setVelocity(0);
    require(keyboard.velocity() == 1,
        "velocity 0 would be a note-off, so the lowest velocity is 1");
    require(!keyboard.press(128).has_value() && !keyboard.press(-1).has_value(),
        "only MIDI notes 0..127 are sent");
}

void testNoNoteHangsWhenTheChannelChangesOrTheKeyboardCloses()
{
    talas::app::OnScreenKeyboardModel keyboard;
    (void)keyboard.press(60);
    (void)keyboard.press(67);
    require(keyboard.setChannel(3)
            == std::vector<Note>{{1, 60, 0}, {1, 67, 0}},
        "changing channel should release held notes on the old channel");
    require(keyboard.channel() == 3 && keyboard.heldNotes().empty(),
        "the new channel should start with no held notes");
    require(keyboard.press(62) == Note{3, 62, 100},
        "notes should then go out on the new channel");
    require(keyboard.releaseAll() == std::vector<Note>{{3, 62, 0}},
        "closing the keyboard should release what it holds");
    require(keyboard.setChannel(99).empty() && keyboard.channel() == 16,
        "channels are 1..16");
}

void testOctaveShiftsKeepTheKeysInMidiRange()
{
    talas::app::OnScreenKeyboardModel keyboard;
    require(keyboard.lowestVisibleNote() == 48, "the keyboard should start at C3");
    keyboard.shiftOctave(1);
    require(keyboard.lowestVisibleNote() == 60, "one octave up should start at C4");
    keyboard.shiftOctave(10);
    require(keyboard.lowestVisibleNote() + talas::app::OnScreenKeyboardModel::visibleKeyCount - 1 <= 127
            && keyboard.lowestVisibleNote() % 12 == 0,
        "the highest octave should keep every key within 127");
    keyboard.shiftOctave(-20);
    require(keyboard.lowestVisibleNote() == 0, "the lowest octave should start at 0");
}

// MIDI notes off sends a note-off (key up)
// for every note on every MIDI channel.
void testMidiNotesOffReleasesEveryNoteOnEveryChannel()
{
    const auto notes = talas::app::midiNotesOff();
    require(notes.size() == 16 * 128 && notes.front() == talas::app::OnScreenKeyboardNote{1, 0, 0}
            && notes.back() == talas::app::OnScreenKeyboardNote{16, 127, 0},
        "a note-off should go to every note on every channel");
    require(std::ranges::all_of(notes, [](const talas::app::OnScreenKeyboardNote& note) { return note.velocity == 0; })
            && std::ranges::count(notes, talas::app::OnScreenKeyboardNote{7, 61, 0}) == 1,
        "each note should be released once, with a key up");
}
}

int main()
{
    try
    {
        testKeysSendNoteEventsOnTheirChannel();
        testNoNoteHangsWhenTheChannelChangesOrTheKeyboardCloses();
        testOctaveShiftsKeepTheKeysInMidiRange();
        testMidiNotesOffReleasesEveryNoteOnEveryChannel();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
