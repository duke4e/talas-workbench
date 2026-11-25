// ADR-0030: a plugin's reading of its host's playhead, block by block, and
// the musical map it stages a project with at the host's tempo.

#include "talas/core/BuiltinNodes.h"
#include "talas/plugin/PluginHostTransport.h"
#include "talas/plugin/PluginRuntime.h"
#include "talas/project/ProjectJson.h"

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace
{
using namespace talas::plugin;

constexpr double sampleRate = 48000.0;
constexpr std::size_t blockFrames = 512;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

[[nodiscard]] bool near(double left, double right, double tolerance = 1.0e-9)
{
    return std::abs(left - right) <= tolerance;
}

[[nodiscard]] HostPlayheadReading playing(double bpm, double ppq)
{
    HostPlayheadReading reading;
    reading.hasPlayhead = true;
    reading.bpm = bpm;
    reading.ppqPosition = ppq;
    reading.playing = true;
    return reading;
}

[[nodiscard]] double blockQuarters(double bpm)
{
    return static_cast<double>(blockFrames) / sampleRate * bpm / 60.0;
}

void testQuarterNotesBecomeMusicalTicks()
{
    require(musicalTickForQuarters(0.0) == 0, "the song's start is tick 0");
    require(musicalTickForQuarters(1.0) == 960, "a quarter note is 960 ticks");
    require(musicalTickForQuarters(0.5) == 480 && musicalTickForQuarters(4.25) == 4080, "and fractions of one");
    require(musicalTickForQuarters(2.0 - 1.0e-7) == 1920, "a position a hair short of a tick rounds to it");
    require(musicalTickForQuarters(-1.5) == 0, "a count-in before the song is at its start");
    require(musicalTickForQuarters(std::nan("")) == 0, "and so is a position that is not a number");
}

void testMissingPlayheadFieldsKeepTheProjectsTransport()
{
    HostTransportFollower follower;
    HostPlayheadReading none;
    require(!follower.follow(none, blockFrames, sampleRate).followsHost, "without a playhead the project's transport runs");

    auto noTempo = playing(120.0, 4.0);
    noTempo.bpm.reset();
    require(!follower.follow(noTempo, blockFrames, sampleRate).followsHost,
        "a playhead without a tempo leaves the project's tempo and transport");
    for (const auto bpm : {0.0, -90.0, 70000.0, std::nan("")})
    {
        require(!follower.follow(playing(bpm, 4.0), blockFrames, sampleRate).followsHost,
            "a tempo the musical map cannot hold is not followed: " + std::to_string(bpm));
    }
    require(usableHostTempo(1.0) && usableHostTempo(60000.0) && !usableHostTempo(0.5), "1 to 60000 BPM are usable");

    // A tempo but no position: the song plays from its start at the tempo.
    auto noPosition = playing(120.0, 0.0);
    noPosition.ppqPosition.reset();
    const auto first = follower.follow(noPosition, blockFrames, sampleRate);
    require(first.followsHost && near(first.quarterPosition, 0.0) && first.relocated,
        "a host without a position starts the song at its start");
    const auto second = follower.follow(noPosition, blockFrames, sampleRate);
    require(near(second.quarterPosition, blockQuarters(120.0)) && !second.relocated,
        "and its position runs on at the host's tempo");
    noPosition.playing = false;
    const auto held = follower.follow(noPosition, blockFrames, sampleRate);
    const auto stillHeld = follower.follow(noPosition, blockFrames, sampleRate);
    require(near(held.quarterPosition, stillHeld.quarterPosition) && held.stopped && !stillHeld.relocated,
        "and holds while the host is stopped");

    // A playhead that answers no more: the project's transport again, and
    // the next answer is a fresh start.
    require(!follower.follow(none, blockFrames, sampleRate).followsHost, "a host that stops answering is not followed");
    require(follower.follow(playing(120.0, 8.0), blockFrames, sampleRate).relocated,
        "and following it again places the transport anew");
}

void testTempoChangesTakeEffectAtTheirBlock()
{
    HostTransportFollower follower;
    const auto first = follower.follow(playing(120.0, 0.0), blockFrames, sampleRate);
    require(first.followsHost && near(first.tempoBpm, 120.0) && first.relocated && first.started,
        "the first block followed places the transport and starts it");
    auto position = blockQuarters(120.0);
    const auto second = follower.follow(playing(90.0, position), blockFrames, sampleRate);
    require(near(second.tempoBpm, 90.0) && !second.relocated,
        "a tempo change takes effect at its block without moving the transport");
    position += blockQuarters(90.0);
    const auto third = follower.follow(playing(90.0, position), blockFrames, sampleRate);
    require(!third.relocated && near(third.quarterPosition, position),
        "the next block starts where the slower one ended");
    require(third.musicalTick == musicalTickForQuarters(position), "and its musical tick is its position's");

    // Blocks rendered in parts: each part is further on at the tempo; a jump
    // belongs to the first part alone.
    const auto start = follower.transportAt(0, true);
    const auto later = follower.transportAt(256, false);
    require(near(start.tempoBpm, 90.0) && near(start.quarterPosition, position) && start.playing && start.drivesSound,
        "the first part carries the block's transport");
    require(near(later.quarterPosition, position + 256.0 / sampleRate * 1.5) && !later.drivesSound,
        "a later part is that many frames further at the tempo");
}

void testStartStopAndJumps()
{
    HostTransportFollower follower;
    auto reading = playing(120.0, 16.0);
    reading.playing = false;
    const auto stopped = follower.follow(reading, blockFrames, sampleRate);
    require(stopped.followsHost && !stopped.playing && !stopped.started, "a stopped host is followed, stopped");
    const auto stillStopped = follower.follow(reading, blockFrames, sampleRate);
    require(!stillStopped.relocated, "a stopped transport stays where it is");
    require(near(follower.transportAt(256, false).quarterPosition, 16.0), "and does not run on within a block");

    reading.playing = true;
    const auto started = follower.follow(reading, blockFrames, sampleRate);
    require(started.started && started.playing && !started.relocated, "Play from where it stopped starts it there");
    reading.ppqPosition = 16.0 + blockQuarters(120.0);
    reading.playing = false;
    const auto halted = follower.follow(reading, blockFrames, sampleRate);
    require(halted.stopped && !halted.playing && !halted.relocated, "Stop stops it where it got to");

    // A locate while stopped, then Play from there.
    reading.ppqPosition = 32.0;
    require(follower.follow(reading, blockFrames, sampleRate).relocated, "a locate while stopped moves it");
    reading.playing = true;
    const auto fromLocate = follower.follow(reading, blockFrames, sampleRate);
    require(fromLocate.started && !fromLocate.relocated, "Play starts it at the located position");

    // A loop's return: the host's position jumps back.
    reading.looping = true;
    reading.loopStartPpq = 32.0;
    reading.loopEndPpq = 33.0;
    reading.ppqPosition = 32.0 + blockQuarters(120.0);
    require(!follower.follow(reading, blockFrames, sampleRate).relocated, "within the loop it plays on");
    reading.ppqPosition = 32.0;
    const auto looped = follower.follow(reading, blockFrames, sampleRate);
    require(looped.relocated && looped.looping && looped.playing, "the loop's return is a jump");

    // Positions rounded by the host are not jumps; a few milliseconds off are.
    const auto expected = 32.0 + blockQuarters(120.0);
    reading.ppqPosition = expected + 1.0e-6;
    require(!follower.follow(reading, blockFrames, sampleRate).relocated, "a rounded position is no jump");
    reading.ppqPosition = expected + 1.0e-6 + blockQuarters(120.0) + 0.05;
    require(follower.follow(reading, blockFrames, sampleRate).relocated, "a position 25 ms off is a jump");
}

void testTimeSignaturesPlaceBarsAndBeats()
{
    HostTransportFollower follower;
    auto reading = playing(120.0, 4.5);
    reading.timeSignatureNumerator = 3;
    reading.timeSignatureDenominator = 4;
    const auto waltz = follower.follow(reading, blockFrames, sampleRate);
    require(waltz.meterFromHost && waltz.meter == HostMeter{3, 4}, "the host's 3/4 is the meter");
    require(waltz.position == talas::project::MusicalPosition{2, 2, 480},
        "quarter 4.5 in 3/4 is bar 2, beat 2, half a beat in");

    const auto sixEight = hostMusicalPosition(3.25, std::nullopt, {6, 8});
    require(sixEight == talas::project::MusicalPosition{2, 1, 240},
        "in 6/8 a beat is an eighth: quarter 3.25 is bar 2, beat 1, tick 240");
    require(hostMusicalPosition(0.0, std::nullopt, {}) == talas::project::MusicalPosition{1, 1, 0},
        "the song's start is bar 1, beat 1");
    require(hostMusicalPosition(-2.0, std::nullopt, {}) == talas::project::MusicalPosition{1, 1, 0},
        "and so is a count-in");
    require(hostMusicalPosition(8.0 - 1.0e-7, std::nullopt, {}) == talas::project::MusicalPosition{3, 1, 0},
        "a position a hair before a bar line is on it");
    // After a change from 4/4 to 3/4 at quarter 8, the host's bar start
    // places the bar lines, so the beat is the host's; the bar is counted in
    // bars of the meter the host has now.
    const auto placed = hostMusicalPosition(12.0, 11.0, {3, 4});
    require(placed.beat == 2 && placed.tick == 0, "the host's last bar start places the beat");
    require(hostMusicalPosition(12.5, 12.0, {3, 4}) == talas::project::MusicalPosition{5, 1, 480},
        "without a meter change the bar counts from the song's start");

    reading.timeSignatureNumerator = 5;
    reading.timeSignatureDenominator = 3;
    const auto odd = follower.follow(reading, blockFrames, sampleRate);
    require(!odd.meterFromHost && odd.meter == HostMeter{4, 4}, "a meter the map cannot hold is not taken");
    require(!hostMeter(0, 4) && !hostMeter(65, 4) && !hostMeter(4, 128) && hostMeter(7, 8) == HostMeter{7, 8},
        "1 to 64 beats of a power-of-two note up to a 64th");
}

void testHostMusicalMaps()
{
    const auto map = hostMusicalTime(90.0, {3, 4});
    require(talas::project::isValidProjectMusicalTime(map), "a host's map is a valid musical map");
    require(map.tempo.size() == 1 && map.tempo.front().microsecondsPerQuarter == 666667,
        "90 BPM is 666667 microseconds a quarter from the start");
    require(map.meter.size() == 1 && map.meter.front().numerator == 3 && map.meter.front().denominator == 4,
        "with the host's meter");
    require(hostMusicalTime(120.0, {0, 0}).meter.front().numerator == 4, "a meter it cannot hold is 4/4");
    require(near(musicalTimeStartTempo(map), 60000000.0 / 666667.0) && musicalTimeStartMeter(map) == HostMeter{3, 4},
        "the map starts at its tempo and meter");
    require(sameMusicalMap(120.0, {4, 4}, 120.0000001, {4, 4}), "tempos equal to the microsecond make one map");
    require(!sameMusicalMap(120.0, {4, 4}, 121.0, {4, 4}) && !sameMusicalMap(120.0, {4, 4}, 120.0, {3, 4}),
        "another tempo or meter makes another");
}

[[nodiscard]] talas::project::ProjectDocument loadDemo(const talas::core::NodeRegistry& registry, std::string_view name)
{
    const auto path = std::filesystem::path{TALAS_SOURCE_DIR} / "examples" / "demos" / (std::string{name} + ".talas.json");
    std::ifstream stream(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    auto parsed = talas::project::parseProjectDocument(text, registry);
    require(parsed.ok(), "the demo " + std::string{name} + " parses");
    return std::move(parsed.document);
}

void testWhichProjectsFollowTheMusicalMap(const talas::core::NodeRegistry& registry)
{
    // The demos read !Tempo, which follows the host at every block: no
    // stage again.
    for (const auto* demo : {"stereo_delay", "arpeggiator", "sub_bass"})
    {
        const auto document = loadDemo(registry, demo);
        require(!projectPlaysATimeline(document) && !projectReadsTheMusicalMap(document, registry),
            std::string{demo} + " neither plays a Timeline nor reads the musical map");
    }
    auto document = loadDemo(registry, "sub_bass");
    document.timeline.eventValues.push_back({1.0, "Cutoff", talas::expr::ValueDomain::Scalar, 500.0});
    require(projectPlaysATimeline(document) && projectReadsTheMusicalMap(document, registry),
        "Timeline Event Values are a Timeline the host starts and stops");

    auto beats = loadDemo(registry, "stereo_delay");
    beats.graph.nodes[1].parameters.front().expression = "0.5 beats";
    require(!projectPlaysATimeline(beats) && projectReadsTheMusicalMap(beats, registry),
        "a field in beats reads the musical map's tempo");
    beats.graph.nodes[1].parameters.front().expression = "!beats * 1 s";
    require(!projectReadsTheMusicalMap(beats, registry), "an Event Value named beats is not the unit");
    beats.graph.nodes[1].parameters.front().expression = "!TimingClockDuration * 9 s";
    require(projectReadsTheMusicalMap(beats, registry), "the MIDI timing clock's stand-in reads it too");
}
}

int main()
{
    try
    {
        const auto registry = talas::core::makeBuiltinNodeRegistry();
        testQuarterNotesBecomeMusicalTicks();
        testMissingPlayheadFieldsKeepTheProjectsTransport();
        testTempoChangesTakeEffectAtTheirBlock();
        testStartStopAndJumps();
        testTimeSignaturesPlaceBarsAndBeats();
        testHostMusicalMaps();
        testWhichProjectsFollowTheMusicalMap(registry);
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "talas_plugin_host_transport_tests passed\n";
    return 0;
}
