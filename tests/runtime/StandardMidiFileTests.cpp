#include "../../src/runtime/StandardMidiFile.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"

#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace
{
using Bytes = std::vector<std::uint8_t>;
using Events = std::vector<talas::runtime::MidiEvent>;
constexpr auto allFrames = std::numeric_limits<std::size_t>::max();

void require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

void append32(Bytes& bytes, std::uint32_t value)
{
    for (const auto shift : {24, 16, 8, 0})
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void chunk(Bytes& bytes, std::string_view type, const Bytes& body)
{
    require(type.size() == 4, "fixture chunk type");
    bytes.insert(bytes.end(), type.begin(), type.end());
    append32(bytes, static_cast<std::uint32_t>(body.size()));
    bytes.insert(bytes.end(), body.begin(), body.end());
}

Bytes file(std::uint16_t format, std::uint16_t division,
           const std::vector<Bytes>& tracks, bool extensions = false)
{
    Bytes bytes;
    Bytes header{static_cast<std::uint8_t>(format >> 8),
                 static_cast<std::uint8_t>(format),
                 static_cast<std::uint8_t>(tracks.size() >> 8),
                 static_cast<std::uint8_t>(tracks.size()),
                 static_cast<std::uint8_t>(division >> 8),
                 static_cast<std::uint8_t>(division)};
    if (extensions) header.insert(header.end(), {0xaa, 0xbb, 0xcc});
    chunk(bytes, "MThd", header);
    for (const auto& track : tracks)
    {
        // Unknown chunks are not tracks and need not have an even length.
        if (extensions) chunk(bytes, "Xtra", {0x90, 0x80, 0xff});
        chunk(bytes, "MTrk", track);
    }
    if (extensions) chunk(bytes, "Tail", {});
    return bytes;
}

const Bytes notes{0, 0x90, 60, 100, 10, 60, 0, 0, 0xff, 0x2f, 0};
const Bytes emptyTrack{0, 0xff, 0x2f, 0};

Events decode(const Bytes& bytes, double rate = 1000, std::size_t frames = allFrames,
              std::uint8_t low = 1, std::uint8_t high = 16, std::size_t budget = 256)
{
    std::string error;
    auto result = talas::runtime::parseStandardMidiFileChannelEvents(
        bytes, rate, frames, low, high, budget, error);
    require(result.has_value(), "decode failed: " + error);
    require(error.empty(), "successful decoding must not retain an error");
    return std::move(*result);
}

void expect(const Events& actual, const Events& expected)
{
    require(actual.size() == expected.size(), "event count");
    for (std::size_t index = 0; index < actual.size(); ++index)
    {
        require(actual[index].frame == expected[index].frame,
                "event frame at index " + std::to_string(index));
        require(actual[index].bytes == expected[index].bytes,
                "event bytes at index " + std::to_string(index));
    }
}

void rejects(const Bytes& bytes, std::string_view reason,
             double rate = 1000, std::uint8_t low = 1, std::uint8_t high = 16)
{
    std::string error;
    const auto result = talas::runtime::parseStandardMidiFileChannelEvents(
        bytes, rate, allFrames, low, high, 256, error);
    require(!result.has_value(), "invalid input was accepted: " + std::string(reason));
    require(error.find(reason) != std::string::npos,
            "wrong diagnostic: " + error + " (expected " + std::string(reason) + ")");
}

void testDefaultTempoAndRunningStatus()
{
    expect(decode(file(0, 100, {notes})),
           {{0, {0x90, 60, 100}}, {50, {0x90, 60, 0}}});
    expect(decode(file(0, 100, {{0, 0xc0, 5, 0, 6, 0, 0xd0, 40,
                               0, 41, 0, 0xff, 0x2f, 0}})),
           {{0, {0xc0, 5}}, {0, {0xc0, 6}}, {0, {0xd0, 40}}, {0, {0xd0, 41}}});
}

void testTempoMapsAndStableOrder()
{
    const Bytes tempo{0, 0xff, 0x51, 3, 7, 0xa1, 0x20,
                      10, 0xff, 0x51, 3, 15, 0x42, 0x40, 0, 0xff, 0x2f, 0};
    const Bytes track{10, 0x90, 60, 100, 10, 60, 0, 0, 0xff, 0x2f, 0};
    const Bytes simultaneous{10, 0x91, 64, 80, 10, 64, 0, 0, 0xff, 0x2f, 0};
    expect(decode(file(1, 100, {tempo, track, simultaneous})),
           {{50, {0x90, 60, 100}}, {50, {0x91, 64, 80}},
            {150, {0x90, 60, 0}}, {150, {0x91, 64, 0}}});
    auto ownTempo = tempo;
    ownTempo.insert(ownTempo.end() - 4, track.begin(), track.end() - 4);
    // The second track retains default tempo; format 2 is the existing Echo
    // projection, not a choice of instrument playback pattern.
    expect(decode(file(2, 100, {ownTempo, simultaneous})),
           {{50, {0x91, 64, 80}}, {100, {0x91, 64, 0}},
            {150, {0x90, 60, 100}}, {250, {0x90, 60, 0}}});
}

void testHeaderAndAlienChunkExtensions()
{
    for (const std::uint16_t division : {100, 0xe728})
    {
        const auto canonical = file(1, division, {notes, notes});
        const auto extended = file(1, division, {notes, notes}, true);
        expect(decode(extended), decode(canonical));
    }
}

void testExtendedTempoUsesKnownPrefix()
{
    // Extensions after the three known tempo bytes must not change timing.
    const Bytes track{0, 0xff, 0x51, 5, 15, 0x42, 0x40, 0xaa, 0xbb,
                      0, 0x90, 60, 100, 10, 60, 0, 0, 0xff, 0x2f, 0};
    expect(decode(file(0, 100, {track})),
           {{0, {0x90, 60, 100}}, {100, {0x90, 60, 0}}});
}

void testChannelAndExclusiveHorizon()
{
    const Bytes track{0, 0x90, 60, 100, 0, 0x9f, 127, 127,
                      10, 0x90, 60, 0, 0, 0x9f, 127, 0, 0, 0xff, 0x2f, 0};
    const auto bytes = file(0, 100, {track});
    expect(decode(bytes, 1000, 50, 16, 16, 1), {{0, {0x9f, 127, 127}}});
    expect(decode(bytes, 1000, 51, 16, 16, 2),
           {{0, {0x9f, 127, 127}}, {50, {0x9f, 127, 0}}});
    require(decode(bytes, 1000, 0, 1, 16, 0).empty(), "zero horizon");
    std::string error;
    require(!talas::runtime::parseStandardMidiFileChannelEvents(
                bytes, 1000, 51, 16, 16, 1, error), "event budget must reject entire result");
    require(error.find("event limit") != std::string::npos, "budget diagnostic");
}

void testSmpteRates()
{
    for (const auto code : {24, 25, 29, 30})
    {
        const auto division = static_cast<std::uint16_t>(((256 - code) << 8) | 40);
        const auto events = decode(file(0, division, {notes}), 48000);
        // Preserve the existing -29 interpretation; no new drop-frame policy.
        const auto rate = code == 29 ? 29.97 : static_cast<double>(code);
        expect(events, {{0, {0x90, 60, 100}},
                        {static_cast<std::size_t>(std::round(48000 * 10 / (rate * 40))),
                         {0x90, 60, 0}}});
    }
}

void testInvalidPreparationArguments()
{
    const auto bytes = file(0, 100, {emptyTrack});
    for (const auto rate : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN()})
        rejects(bytes, "sample rate", rate);
    for (const auto channels : {std::pair{0, 16}, {1, 17}, {16, 1}})
        rejects(bytes, "channel", 1000,
                static_cast<std::uint8_t>(channels.first),
                static_cast<std::uint8_t>(channels.second));
}

void testHeaderBoundsAndFormatZero()
{
    auto giant = file(0, 100, {notes});
    for (std::size_t index = 4; index < 8; ++index) giant[index] = 0xff;
    rejects(giant, "MThd length");
    rejects(file(0, 100, {notes, notes}), "format 0");
    rejects(file(3, 100, {notes}), "format");
    rejects(file(0, 0, {notes}), "division");
    rejects(file(0, 0xe700, {notes}), "division");
}

void testInvalidTempoAndMetaType()
{
    rejects(file(0, 100, {{0, 0xff, 0x51, 2, 7, 0xa1, 0, 0xff, 0x2f, 0}}),
            "tempo");
    rejects(file(0, 100, {{0, 0xff, 0x51, 4, 0, 0, 0, 1, 0, 0xff, 0x2f, 0}}),
            "tempo");
    for (const std::uint16_t division : {100, 0xe728})
        rejects(file(0, division, {{0, 0xff, 0x80, 0, 0, 0xff, 0x2f, 0}}),
                "meta event type");
}

void testRunningStatusCancellationAndTruncation()
{
    for (const Bytes& boundary : {Bytes{0, 0xff, 1, 1, 65}, Bytes{0, 0xf0, 1, 0xf7},
                                  Bytes{0, 0xf7, 0}})
    {
        Bytes track{0, 0x90, 60, 100};
        track.insert(track.end(), boundary.begin(), boundary.end());
        track.insert(track.end(), {0, 60, 0, 0, 0xff, 0x2f, 0});
        rejects(file(0, 100, {track}), "running status");
    }
    rejects(file(0, 100, {{0, 0x90, 60}}), "truncated");
    rejects(file(0, 100, {{0x81, 0x80, 0x80, 0x80, 0}}), "delta time");
    rejects(file(0, 100, {{0, 0xff, 1, 10, 1}}), "length");
    rejects(file(0, 100, {{0, 0xf0, 10, 1}}), "length");
    auto truncated = file(0, 100, {notes});
    truncated.pop_back();
    rejects(truncated, "MTrk length");
}

void testFailureThenSuccessfulRetry()
{
    std::string error;
    require(!talas::runtime::parseStandardMidiFileChannelEvents(
                {}, 1000, allFrames, 1, 16, 256, error), "bad file");
    require(!error.empty(), "bad file diagnostic");
    const auto events = talas::runtime::parseStandardMidiFileChannelEvents(
        file(0, 100, {notes}), 1000, allFrames, 1, 16, 256, error);
    require(events.has_value() && events->size() == 2, "successful retry");
    require(error.empty(), "successful retry must clear the previous failure");
}

void testChunkValidationAndEveryTruncatedPrefix()
{
    const auto valid = file(0, 100, {notes});
    for (std::size_t count = 0; count < valid.size(); ++count)
    {
        std::string error;
        require(!talas::runtime::parseStandardMidiFileChannelEvents(
            Bytes(valid.begin(), valid.begin() + count), 1000, allFrames, 1, 16, 256, error),
            "every truncated prefix must fail without partial events");
        require(!error.empty(), "truncated prefix diagnostic");
    }
    auto duplicate = valid;
    chunk(duplicate, "MTrk", emptyTrack);
    rejects(duplicate, "more MTrk");
    auto duplicateHeader = valid;
    chunk(duplicateHeader, "MThd", {0, 0, 0, 1, 0, 100});
    rejects(duplicateHeader, "duplicate MThd");
    auto tail = valid;
    tail.push_back('X');
    rejects(tail, "chunk header");
    tail = valid;
    chunk(tail, "Xtra", {1});
    tail.pop_back();
    rejects(tail, "chunk length");
}

// Many tempo changes and events, with exact frames from a plain rescan of
// the tempo map; offline Echo renders it in one-frame blocks.
void testDenseTempoMapKeepsExactFramesAndRendersInLinearTime()
{
    constexpr std::size_t count = 1500;
    constexpr std::uint16_t division = 96;
    Bytes tempoTrack;
    Bytes noteTrack;
    struct Tempo { std::uint64_t tick; double microseconds; };
    std::vector<Tempo> tempos;
    for (std::size_t index = 0; index < count; ++index)
    {
        const std::uint32_t microseconds = index % 3 == 0 ? 400000U
            : index % 3 == 1 ? 523411U : 611111U;
        tempoTrack.insert(tempoTrack.end(), {7, 0xff, 0x51, 3,
            static_cast<std::uint8_t>(microseconds >> 16),
            static_cast<std::uint8_t>(microseconds >> 8),
            static_cast<std::uint8_t>(microseconds)});
        tempos.push_back({(index + 1) * 7, static_cast<double>(microseconds)});
        noteTrack.insert(noteTrack.end(), {5, 0x90, 60, 100});
    }
    tempoTrack.insert(tempoTrack.end(), {0, 0xff, 0x2f, 0});
    noteTrack.insert(noteTrack.end(), {0, 0xff, 0x2f, 0});
    const auto bytes = file(1, division, {tempoTrack, noteTrack});

    constexpr double rate = 1000.0;
    Events expected;
    for (std::size_t index = 0; index < count; ++index)
    {
        const std::uint64_t target = (index + 1) * 5;
        double seconds = 0.0;
        double tempo = 500000.0;
        std::uint64_t cursor = 0;
        for (const auto& change : tempos)
        {
            if (change.tick > target) break;
            if (change.tick > cursor)
            {
                seconds += (static_cast<double>(change.tick - cursor) * tempo)
                    / (division * 1000000.0);
                cursor = change.tick;
            }
            tempo = change.microseconds;
        }
        if (target > cursor)
            seconds += (static_cast<double>(target - cursor) * tempo)
                / (division * 1000000.0);
        expected.push_back({static_cast<std::size_t>(std::round(seconds * rate)),
                            {0x90, 60, 100}});
    }
    expect(decode(bytes, rate, allFrames, 1, 16, count), expected);

    talas::project::ProjectDocument document;
    talas::project::ProjectParameterExpression resource;
    resource.parameterId = "fileName";
    resource.resource = "Sequences/Dense.mid";
    document.graph.nodes.push_back({"terminal", "talas.midiFileEcho",
        {{"lowChannel", "1"}, {"highChannel", "16"}, resource}});
    const auto frames = expected.back().frame + 1U;
    talas::runtime::RenderSettings settings{rate, 1};
    settings.binaryResources.push_back({*resource.resource, bytes});
    auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "dense MIDI Echo graph must compile");
    const auto started = std::chrono::steady_clock::now();
    const auto offline = talas::runtime::renderOffline(*compiled.graph, frames);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    require(offline.ok(), "dense offline MIDI Echo must render");
    expect(offline.midiEvents, expected);
    require(elapsed < std::chrono::seconds(5),
            "one-frame blocks must not reparse the MIDI file per block");
}

void testEchoUsesSharedDecoderAndCapacityRetry()
{
    const Bytes tempo{0, 0xff, 0x51, 4, 15, 0x42, 0x40, 0xaa,
                      0, 0xff, 0x2f, 0};
    const Bytes second{5, 0x91, 64, 80, 10, 64, 0, 0, 0xff, 0x2f, 0};
    const auto bytes = file(1, 100, {tempo, notes, second}, true);
    const Events expected{{0, {0x90, 60, 100}}, {50, {0x91, 64, 80}},
                           {100, {0x90, 60, 0}}, {150, {0x91, 64, 0}}};
    talas::project::ProjectDocument document;
    talas::project::ProjectParameterExpression resource;
    resource.parameterId = "fileName";
    resource.resource = "Sequences/Extended.mid";
    document.graph.nodes.push_back({"terminal", "talas.midiFileEcho",
        {{"lowChannel", "1"}, {"highChannel", "16"}, resource}});
    talas::runtime::RenderSettings settings{1000, 151};
    settings.binaryResources.push_back({*resource.resource, bytes});
    auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "extended MIDI Echo graph must compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 151);
    require(offline.ok(), "offline extended MIDI Echo must render");
    expect(offline.midiEvents, expected);

    for (const auto blockSize : {std::size_t{1}, std::size_t{17}, std::size_t{151}})
    {
        talas::runtime::RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), session.error());
        std::array<talas::runtime::RealtimeMidiOutputEvent, 4> storage{};
        talas::runtime::RealtimeTerminalOutputBlock output{storage};
        Events actual;
        for (std::size_t frame = 0; frame < 151; frame += blockSize)
        {
            const auto count = std::min(blockSize, 151 - frame);
            require(session.renderTerminalEvents(count, output)
                    == talas::runtime::RealtimeTerminalRenderResult::Rendered,
                    "partitioned realtime Echo");
            for (std::size_t index = 0; index < output.midiEventCount; ++index)
                actual.push_back({frame + storage[index].frameOffset,
                    Bytes(storage[index].bytes.begin(),
                          storage[index].bytes.begin() + storage[index].byteCount)});
        }
        expect(actual, expected);
        session.resetTransport();
        // The failed callback must publish no prefix and consume no event.
        output.midiEvents = std::span(storage).first(3);
        output.midiEventCount = 2;
        storage[0].frameOffset = 777;
        require(session.renderTerminalEvents(151, output)
                == talas::runtime::RealtimeTerminalRenderResult::CapacityExceeded,
                "insufficient terminal capacity must reject atomically");
        require(output.midiEventCount == 2 && storage[0].frameOffset == 777,
                "failed publication must preserve caller storage and metadata");
        output.midiEvents = storage;
        require(session.renderTerminalEvents(151, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered,
                "capacity correction must retry the same MIDI events");
        actual.clear();
        for (std::size_t index = 0; index < output.midiEventCount; ++index)
            actual.push_back({storage[index].frameOffset,
                Bytes(storage[index].bytes.begin(),
                      storage[index].bytes.begin() + storage[index].byteCount)});
        expect(actual, expected);
    }
}
}

int main(int argc, char** argv)
{
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests{
        {"default-tempo-running-status", testDefaultTempoAndRunningStatus},
        {"tempo-maps-stable-order", testTempoMapsAndStableOrder},
        {"header-alien-extensions", testHeaderAndAlienChunkExtensions},
        {"extended-tempo", testExtendedTempoUsesKnownPrefix},
        {"channel-horizon-budget", testChannelAndExclusiveHorizon},
        {"smpte-rates", testSmpteRates},
        {"invalid-preparation", testInvalidPreparationArguments},
        {"header-bounds-format-zero", testHeaderBoundsAndFormatZero},
        {"invalid-tempo-meta", testInvalidTempoAndMetaType},
        {"running-status-truncation", testRunningStatusCancellationAndTruncation},
        {"failure-success-retry", testFailureThenSuccessfulRetry},
        {"chunk-validation-prefixes", testChunkValidationAndEveryTruncatedPrefix},
        {"echo-integration-retry", testEchoUsesSharedDecoderAndCapacityRetry},
        {"dense-tempo-linear-render", testDenseTempoMapKeepsExactFramesAndRendersInLinearTime},
    };
    std::size_t failures = 0;
    std::size_t selected = 0;
    for (const auto& [name, run] : tests)
    {
        if (argc > 1 && std::string_view(argv[1]) != name) continue;
        ++selected;
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error)
        {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 && selected != 0 ? 0 : 1;
}
