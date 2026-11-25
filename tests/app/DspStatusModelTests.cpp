#include "talas/app/DspStatusModel.h"

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

// The time since the Sound started, as
// hours:minutes:seconds, reset each time it starts and held once stopped;
// the time code display as hours:minutes:seconds.frames.
void testElapsedTimeAndTimeCode()
{
    talas::app::DspStatusClock clock;
    clock.start(100.0);
    require(clock.running() && talas::app::dspStatusElapsedText(clock.elapsed(3825.5)) == "1:02:05",
        "the time since the Sound started should read hours:minutes:seconds");
    clock.stop(110.0);
    require(!clock.running() && clock.elapsed(500.0) == 10.0,
        "the time display should stop when the Sound is stopped");
    clock.start(600.0);
    require(clock.elapsed(600.0) == 0.0, "the time should reset each time the Sound plays");
    require(talas::app::dspStatusTimeCodeText(1, 2, 3, 4) == "01:02:03.04",
        "time code should read hours:minutes:seconds.frames");
    // The processor's usage as Sounds start and end.
    require(talas::app::dspStatusProcessorText(0.374) == "Processor 37 %" && talas::app::dspStatusProcessorText(-1.0) == "Processor 0 %",
        "DSP Status should show the processor's usage");
    require(talas::app::dspStatusProcessorText(1.7) == "Processor 100 %  Out of Real Time"
            && talas::app::dspStatusProcessorText(1.0) == "Processor 100 %  Out of Real Time"
            && talas::app::dspStatusProcessorText(0.99) == "Processor 99 %",
        "a processor that cannot keep up should say Out of Real Time");
}

// The level bars marked every 10 dB, and
// a peak indicator that shows >0< where the signal may have clipped until
// it is reset.
void testLevelsAndPeakIndicators()
{
    require(talas::app::dspStatusMeterMarksDb() == std::vector<double>{0.0, -10.0, -20.0, -30.0, -40.0, -50.0, -60.0},
        "the marks alongside the bars should be steps of 10 dB");
    require(talas::app::dspStatusMeterFraction(1.0) == 1.0 && talas::app::dspStatusMeterFraction(0.0) == 0.0
            && std::abs(talas::app::dspStatusMeterFraction(0.1) - 2.0 / 3.0) < 1.0e-12,
        "a bar should run from its floor to 0 dB");
    talas::app::DspPeakIndicator peak;
    peak.observe(0.5);
    peak.observe(0.25);
    require(!peak.clipped() && peak.text() == "-6.0", "the peak indicator should hold the highest level");
    peak.observe(1.0);
    peak.observe(0.1);
    require(peak.clipped() && peak.text() == ">0<", "the peak indicator should show >0< if the signal may have clipped");
    peak.reset();
    require(!peak.clipped() && peak.text() == "-inf", "clicking should reset the peak indicator");
}
}

// Show Eight Channels in DSP Status shows all eight
// input and output meters, else two.
// The DSP Status window's Sample Rate menu holds
// the seven standard rates, those the device offers.
void testTheSampleRateMenu()
{
    require(talas::app::talasSampleRates() == std::vector<double>{48000.0, 44100.0, 32000.0, 24000.0, 22050.0, 12000.0, 11025.0},
        "seven rates, the compact disc's, the DAT's and extended-play DAT's");
    const std::vector<double> offered{44100.0, 48000.0, 96000.0, 22050.0};
    require(talas::app::dspStatusSampleRateChoices(offered) == std::vector<double>{48000.0, 44100.0, 22050.0},
        "the menu should list the seven rates the device offers, highest first");
}

void testEightChannelMeters()
{
    require(talas::app::dspStatusMeterCount(false) == 2 && talas::app::dspStatusMeterCount(true) == 8
            && talas::app::dspStatusMaximumChannels == 8,
        "the DSP Status should show eight meters a side when asked, else two");
}

// Record to disk counts a Sound with live input in,
// a beep a beat at the tempo given, and records from the beat after; zero
// beats is no count-off.
void testRecordingCountsOff()
{
    const auto four = talas::app::recordingCountOff(4, 120.0);
    require(four.beepSeconds == std::vector<double>{0.0, 0.5, 1.0, 1.5} && four.recordingStartSeconds == 2.0,
        "four beats at 120 should beep every half second and record on the beat after");
    const auto none = talas::app::recordingCountOff(0, 120.0);
    require(none.beepSeconds.empty() && none.recordingStartSeconds == 0.0,
        "zero beats should be no count-off");
    talas::app::LoadedSoundInputs live;
    live.audioInput = true;
    talas::app::LoadedSoundInputs keyboard;
    keyboard.midiChannels = {"1"};
    require(talas::app::takesLiveInput(live) && talas::app::takesLiveInput(keyboard) && !talas::app::takesLiveInput({}),
        "a Sound with audio or MIDI input takes live input; the count-off is asked for it alone");
}

// The memory bar is cyan for light use, green and
// yellow for moderate use, red for heavy use.
void testMemoryUseColours()
{
    using talas::app::MemoryUseColour;
    require(talas::app::memoryUseColour(0.1) == MemoryUseColour::Cyan && talas::app::memoryUseColour(0.3) == MemoryUseColour::Green
            && talas::app::memoryUseColour(0.6) == MemoryUseColour::Yellow && talas::app::memoryUseColour(0.9) == MemoryUseColour::Red,
        "cyan for light usage, green and yellow for moderate usage, red for heavy usage");
    require(talas::app::memoryUseColour(-1.0) == MemoryUseColour::Cyan && talas::app::memoryUseColour(2.0) == MemoryUseColour::Red,
        "a share out of range is light or heavy");
    // The open window recycles by itself under heavy use.
    require(talas::app::shouldRecycleAutomatically(0.9, 31.0) && !talas::app::shouldRecycleAutomatically(0.9, 5.0)
            && !talas::app::shouldRecycleAutomatically(0.6, 300.0),
        "heavy use should recycle automatically, at most every half minute, moderate use not at all");
}

// The bottom of the status window tells of the
// Sound last loaded: the global map defining its Event Values, whether it
// expects input from the A/D, the MIDI channels it expects keyboard input
// on, and the Event Values that control its parameters.
void testTheLoadedSoundsInputs()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"voice", "talas.midiVoice", {{"channel", "2"}}});
    document.virtualControls.push_back({"Cutoff", talas::expr::ValueDomain::Hertz});
    document.virtualControls.push_back({"Amp", talas::expr::ValueDomain::Scalar});
    talas::project::ProjectSoundDefinition effect;
    effect.definitionId = "effect";
    effect.graph.nodes.push_back({"in", "talas.genericSource", {}});
    document.soundDefinitions.push_back(effect);
    const auto inputs = talas::app::loadedSoundInputs(document);
    require(inputs.globalMap == "NONE SELECTED (Default)" && inputs.audioInput && inputs.midiChannels == std::vector<std::string>{"2"}
            && inputs.eventValues == std::vector<std::string>{"Amp", "Cutoff"},
        "the window should name the global map, the A/D input, the MIDI channel and the Event Values");
    require(talas::app::loadedSoundInputLines(inputs)
            == std::vector<std::string>{"Global map: NONE SELECTED (Default)", "Input from the A/D", "MIDI keyboard on channel 2",
                "Event Values: !Amp, !Cutoff"},
        "the window's lines");
    talas::project::ProjectDocument quiet;
    quiet.globalMap = "!Volume is: `MIDIController7.";
    quiet.graph.nodes.push_back({"disk", "talas.genericSource", {{"source", {}, {}, {}, {}, std::string{"Disk"}}}});
    const auto none = talas::app::loadedSoundInputs(quiet);
    require(none.globalMap == "the project's own" && !none.audioInput && none.midiChannels.empty() && none.eventValues.empty(),
        "a Sound reading the disk, on the project's own map, with no MIDI or Event Values");
    // A Sound playing from or recording to the disk.
    talas::project::ProjectDocument recorder;
    recorder.graph.nodes.push_back({"take", "talas.diskRecorder", {}});
    require(none.usesDisk && talas::app::loadedSoundInputs(recorder).usesDisk && !inputs.usesDisk,
        "a Sound reading or recording the disk uses the disk; one taking live input does not");
}

// Show MIDI Messages lists the keyboard's and
// the controllers' messages as they arrive, the latest lines kept.
void testShowMidiMessages()
{
    using Bytes = std::vector<std::uint8_t>;
    const auto described = [](Bytes bytes) { return talas::app::midiMessageDescription(bytes); };
    require(described({0x90, 60, 100}) == "Key down 60 velocity 100 channel 1" && described({0x81, 60, 0}) == "Key up 60 channel 2"
            && described({0x90, 64, 0}) == "Key up 64 channel 1",
        "a keyboard message should name its key, velocity and channel");
    require(described({0xB3, 7, 64}) == "Controller 7 value 64 channel 4" && described({0xE0, 0, 64}) == "Pitch bend 8192 channel 1"
            && described({0xC0, 5}) == "Program 5 channel 1" && described({0xF8}) == "F8",
        "a controller should name its number, value and channel; others show their bytes");
    talas::app::MidiMessageLog log;
    for (std::size_t index = 0; index < talas::app::MidiMessageLog::capacity + 5; ++index)
        log.add("line " + std::to_string(index));
    require(log.lines().size() == talas::app::MidiMessageLog::capacity && log.lines().front() == "line 5"
            && log.text().ends_with("line 204\n"),
        "the window should keep its latest lines, the oldest leaving first");
}

int main()
{
    try
    {
        testElapsedTimeAndTimeCode();
        testLevelsAndPeakIndicators();
        testTheLoadedSoundsInputs();
        testMemoryUseColours();
        testRecordingCountsOff();
        testEightChannelMeters();
        testTheSampleRateMenu();
        testShowMidiMessages();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "DSP status model tests passed\n";
    return 0;
}
