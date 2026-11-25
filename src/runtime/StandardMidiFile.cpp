#include "StandardMidiFile.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace talas::runtime
{
namespace
{
[[nodiscard]] std::optional<std::uint16_t> readBigEndian16(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 2)
        return std::nullopt;
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8)
        | static_cast<std::uint16_t>(bytes[offset + 1]));
}

[[nodiscard]] std::optional<std::uint32_t> readBigEndian32(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4)
        return std::nullopt;
    return (static_cast<std::uint32_t>(bytes[offset]) << 24)
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8)
        | static_cast<std::uint32_t>(bytes[offset + 3]);
}

[[nodiscard]] std::optional<std::uint32_t> readVariableLengthQuantity(
    const std::vector<std::uint8_t>& bytes,
    std::size_t& offset,
    std::size_t limit)
{
    std::uint32_t value = 0;
    for (int byteCount = 0; byteCount < 4; ++byteCount)
    {
        if (offset >= limit)
            return std::nullopt;
        const auto byte = bytes[offset++];
        value = (value << 7) | static_cast<std::uint32_t>(byte & 0x7fU);
        if ((byte & 0x80U) == 0)
            return value;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::vector<MidiEvent>> parseStandardMidiFileEvents(
    const std::vector<std::uint8_t>& bytes,
    double sampleRate,
    std::size_t frameCount,
    std::uint8_t lowChannel,
    std::uint8_t highChannel,
    std::size_t maxEventCount,
    std::string& error,
    bool quarterNotes = false,
    bool timingClock = false)
{
    error.clear();
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
    {
        error = "MIDI file requires a finite positive sample rate";
        return std::nullopt;
    }
    if (lowChannel < 1 || highChannel > 16 || lowChannel > highChannel)
    {
        error = "MIDI file requires channels from 1 through 16 in ascending order";
        return std::nullopt;
    }
    if (bytes.size() < 14
        || bytes[0] != 'M' || bytes[1] != 'T' || bytes[2] != 'h' || bytes[3] != 'd')
    {
        error = "MIDI file is missing an MThd header";
        return std::nullopt;
    }

    const auto headerLength = readBigEndian32(bytes, 4);
    if (!headerLength.has_value() || *headerLength < 6 || *headerLength > bytes.size() - 8)
    {
        error = "MIDI file has an invalid MThd length";
        return std::nullopt;
    }

    const auto format = readBigEndian16(bytes, 8);
    const auto trackCount = readBigEndian16(bytes, 10);
    const auto division = readBigEndian16(bytes, 12);
    if (!format.has_value() || !trackCount.has_value() || !division.has_value() || *trackCount == 0)
    {
        error = "MIDI file has an incomplete MThd header";
        return std::nullopt;
    }
    if (*format > 2)
    {
        error = "MIDIFileEcho only supports SMF format 0, 1, and 2";
        return std::nullopt;
    }
    if (*format == 0 && *trackCount != 1)
    {
        error = "MIDI file format 0 requires exactly one track";
        return std::nullopt;
    }

    // Validate chunk framing once for both timing and channel-event passes.
    // SMF permits unknown chunks, extended headers and odd payload lengths.
    struct TrackRange
    {
        std::size_t begin;
        std::size_t end;
    };
    std::vector<TrackRange> tracks;
    tracks.reserve(*trackCount);
    std::size_t chunkOffset = std::size_t{8} + *headerLength;
    while (chunkOffset < bytes.size())
    {
        if (bytes.size() - chunkOffset < 8)
        {
            error = "MIDI file has a truncated chunk header";
            return std::nullopt;
        }
        const auto isTrack = bytes[chunkOffset] == 'M'
            && bytes[chunkOffset + 1] == 'T' && bytes[chunkOffset + 2] == 'r'
            && bytes[chunkOffset + 3] == 'k';
        const auto isHeader = bytes[chunkOffset] == 'M'
            && bytes[chunkOffset + 1] == 'T' && bytes[chunkOffset + 2] == 'h'
            && bytes[chunkOffset + 3] == 'd';
        const auto length = readBigEndian32(bytes, chunkOffset + 4);
        chunkOffset += 8;
        if (!length.has_value() || *length > bytes.size() - chunkOffset)
        {
            error = isTrack ? "MIDI file has an invalid MTrk length"
                            : "MIDI file has an invalid chunk length";
            return std::nullopt;
        }
        if (isHeader)
        {
            error = "MIDI file contains a duplicate MThd header";
            return std::nullopt;
        }
        if (isTrack)
        {
            if (tracks.size() == *trackCount)
            {
                error = "MIDI file contains more MTrk chunks than declared";
                return std::nullopt;
            }
            tracks.push_back({chunkOffset, chunkOffset + *length});
        }
        chunkOffset += *length;
    }
    if (tracks.size() != *trackCount)
    {
        error = "MIDI file is missing an MTrk chunk";
        return std::nullopt;
    }
    std::vector<MidiEvent> events;
    const bool usesPpqTiming = (*division & 0x8000U) == 0;
    double ticksPerQuarter = 0.0;
    double ticksPerSecond = 0.0;
    struct TempoEvent
    {
        std::uint64_t tick = 0;
        double microsecondsPerQuarter = 500000.0;
    };
    std::vector<TempoEvent> globalTempoEvents;
    std::vector<std::vector<TempoEvent>> tempoEventsByTrack(*trackCount);
    if (usesPpqTiming)
    {
        if (*division == 0)
        {
            error = "MIDI file has an invalid PPQ timing division";
            return std::nullopt;
        }
        ticksPerQuarter = static_cast<double>(*division);
    }
    else
    {
        const auto smpteCode = static_cast<std::int8_t>((*division >> 8U) & 0xffU);
        const auto ticksPerFrame = static_cast<std::uint8_t>(*division & 0xffU);
        const auto framesPerSecondCode = -static_cast<int>(smpteCode);
        double framesPerSecond = 0.0;
        switch (framesPerSecondCode)
        {
        case 24:
        case 25:
        case 30:
            framesPerSecond = static_cast<double>(framesPerSecondCode);
            break;
        case 29:
            framesPerSecond = 29.97;
            break;
        default:
            error = "MIDIFileEcho only supports SMPTE frame rates 24, 25, 29, and 30";
            return std::nullopt;
        }

        if (ticksPerFrame == 0)
        {
            error = "MIDI file has an invalid SMPTE ticks-per-frame division";
            return std::nullopt;
        }
        ticksPerSecond = framesPerSecond * static_cast<double>(ticksPerFrame);
    }

    if (usesPpqTiming)
    {
        for (std::uint16_t trackIndex = 0; trackIndex < *trackCount; ++trackIndex)
        {
            auto tempoOffset = tracks[trackIndex].begin;
            const auto trackEnd = tracks[trackIndex].end;
            std::uint64_t tick = 0;
            std::uint8_t runningStatus = 0;
            std::vector<TempoEvent> trackTempoEvents;

            while (tempoOffset < trackEnd)
            {
                const auto deltaTicks = readVariableLengthQuantity(bytes, tempoOffset, trackEnd);
                if (!deltaTicks.has_value())
                {
                    error = "MIDI file has an invalid delta time";
                    return std::nullopt;
                }
                tick += *deltaTicks;

                if (tempoOffset >= trackEnd)
                {
                    error = "MIDI file event is truncated";
                    return std::nullopt;
                }

                auto status = bytes[tempoOffset++];
                bool hasFirstDataByte = false;
                if (status < 0x80U)
                {
                    if (runningStatus == 0)
                    {
                        error = "MIDI file uses running status before a status byte";
                        return std::nullopt;
                    }
                    hasFirstDataByte = true;
                    status = runningStatus;
                }
                else if (status < 0xf0U)
                {
                    runningStatus = status;
                }
                else
                {
                    runningStatus = 0;
                }

                if (status == 0xffU)
                {
                    if (tempoOffset >= trackEnd)
                    {
                        error = "MIDI meta event is truncated";
                        return std::nullopt;
                    }
                    const auto metaType = bytes[tempoOffset++];
                    if (metaType >= 0x80U)
                    {
                        error = "MIDI file has an invalid meta event type";
                        return std::nullopt;
                    }
                    const auto length = readVariableLengthQuantity(bytes, tempoOffset, trackEnd);
                    if (!length.has_value() || *length > trackEnd - tempoOffset)
                    {
                        error = "MIDI meta event has an invalid length";
                        return std::nullopt;
                    }
                    if (metaType == 0x51U)
                    {
                        if (*length < 3)
                        {
                            error = "MIDI tempo event has an invalid length";
                            return std::nullopt;
                        }
                        const auto tempo =
                            static_cast<double>((static_cast<std::uint32_t>(bytes[tempoOffset]) << 16)
                                | (static_cast<std::uint32_t>(bytes[tempoOffset + 1]) << 8)
                                | static_cast<std::uint32_t>(bytes[tempoOffset + 2]));
                        if (tempo <= 0.0)
                        {
                            error = "MIDI tempo event has an invalid tempo";
                            return std::nullopt;
                        }
                        trackTempoEvents.push_back({tick, tempo});
                    }
                    tempoOffset += *length;
                    if (metaType == 0x2fU)
                        break;
                    continue;
                }

                if (status == 0xf0U || status == 0xf7U)
                {
                    const auto length = readVariableLengthQuantity(bytes, tempoOffset, trackEnd);
                    if (!length.has_value() || *length > trackEnd - tempoOffset)
                    {
                        error = "MIDI SysEx event has an invalid length";
                        return std::nullopt;
                    }
                    tempoOffset += *length;
                    continue;
                }

                if (status < 0x80U || status > 0xefU)
                {
                    error = "MIDI file contains an unsupported event status";
                    return std::nullopt;
                }

                const auto dataLength = ((status & 0xf0U) == 0xc0U || (status & 0xf0U) == 0xd0U) ? std::size_t{1} : std::size_t{2};
                std::size_t remainingDataLength = dataLength;
                if (hasFirstDataByte)
                    --remainingDataLength;
                while (remainingDataLength > 0)
                {
                    if (tempoOffset >= trackEnd || bytes[tempoOffset] >= 0x80U)
                    {
                        error = "MIDI channel event data is truncated";
                        return std::nullopt;
                    }
                    ++tempoOffset;
                    --remainingDataLength;
                }
            }

            if (*format == 2)
            {
                std::stable_sort(trackTempoEvents.begin(), trackTempoEvents.end(), [](const TempoEvent& lhs, const TempoEvent& rhs) {
                    return lhs.tick < rhs.tick;
                });
                tempoEventsByTrack[static_cast<std::size_t>(trackIndex)] = std::move(trackTempoEvents);
            }
            else
            {
                globalTempoEvents.insert(
                    globalTempoEvents.end(),
                    std::make_move_iterator(trackTempoEvents.begin()),
                    std::make_move_iterator(trackTempoEvents.end()));
            }
        }

        std::stable_sort(globalTempoEvents.begin(), globalTempoEvents.end(), [](const TempoEvent& lhs, const TempoEvent& rhs) {
            return lhs.tick < rhs.tick;
        });
    }

    // The timing state after the first k tempo events, accumulated in the
    // same order as a scan from the start, so each event needs one binary
    // search instead of a rescan of the whole tempo map.
    struct TempoMap
    {
        std::vector<std::uint64_t> ticks;
        std::vector<double> seconds{0.0};
        std::vector<std::uint64_t> cursorTicks{0};
        std::vector<double> microsecondsPerQuarter{500000.0};
    };
    const auto makeTempoMap = [&](const std::vector<TempoEvent>& tempoEvents) {
        TempoMap map;
        map.ticks.reserve(tempoEvents.size());
        map.seconds.reserve(tempoEvents.size() + 1U);
        map.cursorTicks.reserve(tempoEvents.size() + 1U);
        map.microsecondsPerQuarter.reserve(tempoEvents.size() + 1U);
        double seconds = 0.0;
        double tempoMicrosecondsPerQuarter = 500000.0;
        std::uint64_t cursorTick = 0;
        for (const auto& tempoEvent : tempoEvents)
        {
            if (tempoEvent.tick > cursorTick)
            {
                seconds += (static_cast<double>(tempoEvent.tick - cursorTick) * tempoMicrosecondsPerQuarter)
                    / (ticksPerQuarter * 1000000.0);
                cursorTick = tempoEvent.tick;
            }
            tempoMicrosecondsPerQuarter = tempoEvent.microsecondsPerQuarter;
            map.ticks.push_back(tempoEvent.tick);
            map.seconds.push_back(seconds);
            map.cursorTicks.push_back(cursorTick);
            map.microsecondsPerQuarter.push_back(tempoMicrosecondsPerQuarter);
        }
        return map;
    };
    const auto globalTempoMap = makeTempoMap(globalTempoEvents);
    std::vector<TempoMap> tempoMapsByTrack;
    if (*format == 2)
    {
        tempoMapsByTrack.reserve(tempoEventsByTrack.size());
        for (const auto& trackTempoEvents : tempoEventsByTrack)
            tempoMapsByTrack.push_back(makeTempoMap(trackTempoEvents));
    }
    const auto secondsFromPpqTick = [&](std::uint64_t targetTick, const TempoMap& map) {
        const auto processed = static_cast<std::size_t>(
            std::upper_bound(map.ticks.begin(), map.ticks.end(), targetTick)
            - map.ticks.begin());
        auto seconds = map.seconds[processed];
        const auto cursorTick = map.cursorTicks[processed];
        if (targetTick > cursorTick)
        {
            seconds += (static_cast<double>(targetTick - cursorTick) * map.microsecondsPerQuarter[processed])
                / (ticksPerQuarter * 1000000.0);
        }
        return seconds;
    };

    // The frame of a track's tick, or nothing (with the error) outside
    // the supported range.
    const auto frameAt = [&](std::uint64_t tick, std::uint16_t trackIndex) -> std::optional<std::size_t> {
        const auto& tempoMap = (*format == 2)
            ? tempoMapsByTrack[static_cast<std::size_t>(trackIndex)]
            : globalTempoMap;
        const auto seconds = !usesPpqTiming ? static_cast<double>(tick) / ticksPerSecond
            : quarterNotes                  ? static_cast<double>(tick) / ticksPerQuarter
                                            : secondsFromPpqTick(tick, tempoMap);
        const auto roundedFrame = std::round(seconds * sampleRate);
        if (!std::isfinite(roundedFrame)
            || roundedFrame < 0.0
            || roundedFrame
                >= static_cast<double>(
                    std::numeric_limits<std::size_t>::max()))
        {
            error = "MIDIFileEcho event frame is outside the supported range";
            return std::nullopt;
        }
        return static_cast<std::size_t>(roundedFrame);
    };

    for (std::uint16_t trackIndex = 0; trackIndex < *trackCount; ++trackIndex)
    {
        auto offset = tracks[trackIndex].begin;
        const auto trackEnd = tracks[trackIndex].end;
        std::uint64_t tick = 0;
        std::uint8_t runningStatus = 0;

        while (offset < trackEnd)
        {
            const auto deltaTicks = readVariableLengthQuantity(bytes, offset, trackEnd);
            if (!deltaTicks.has_value())
            {
                error = "MIDI file has an invalid delta time";
                return std::nullopt;
            }
            tick += *deltaTicks;

            if (offset >= trackEnd)
            {
                error = "MIDI file event is truncated";
                return std::nullopt;
            }

            auto status = bytes[offset++];
            std::optional<std::uint8_t> firstDataByte;
            if (status < 0x80U)
            {
                if (runningStatus == 0)
                {
                    error = "MIDI file uses running status before a status byte";
                    return std::nullopt;
                }
                firstDataByte = status;
                status = runningStatus;
            }
            else if (status < 0xf0U)
            {
                runningStatus = status;
            }
            else
            {
                runningStatus = 0;
            }

            if (status == 0xffU)
            {
                if (offset >= trackEnd)
                {
                    error = "MIDI meta event is truncated";
                    return std::nullopt;
                }
                const auto metaType = bytes[offset++];
                if (metaType >= 0x80U)
                {
                    error = "MIDI file has an invalid meta event type";
                    return std::nullopt;
                }
                const auto length = readVariableLengthQuantity(bytes, offset, trackEnd);
                if (!length.has_value() || *length > trackEnd - offset)
                {
                    error = "MIDI meta event has an invalid length";
                    return std::nullopt;
                }
                if (metaType == 0x51U && (*length < 3
                        || (bytes[offset] == 0 && bytes[offset + 1] == 0
                            && bytes[offset + 2] == 0)))
                {
                    error = "MIDI tempo event has an invalid tempo or length";
                    return std::nullopt;
                }
                offset += *length;
                if (metaType == 0x2fU)
                    break;
                continue;
            }

            if (status == 0xf0U || status == 0xf7U)
            {
                const auto length = readVariableLengthQuantity(bytes, offset, trackEnd);
                if (!length.has_value() || *length > trackEnd - offset)
                {
                    error = "MIDI SysEx event has an invalid length";
                    return std::nullopt;
                }
                // An escaped event of the timing
                // clock's real-time messages (F8, Start FA, Continue FB,
                // Stop FC), each a one-byte event.
                const auto clockMessages = timingClock && status == 0xf7U && *length > 0
                    && std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                        bytes.begin() + static_cast<std::ptrdiff_t>(offset + *length), [](std::uint8_t byte) {
                            return byte == 0xf8U || byte == 0xfaU || byte == 0xfbU || byte == 0xfcU;
                        });
                if (clockMessages)
                {
                    const auto frame = frameAt(tick, trackIndex);
                    if (!frame.has_value())
                        return std::nullopt;
                    for (std::size_t index = 0; index < *length && *frame < frameCount; ++index)
                    {
                        if (events.size() >= maxEventCount)
                        {
                            error = "MIDI file exceeds the prepared timing clock limit";
                            return std::nullopt;
                        }
                        events.push_back(MidiEvent{*frame, {bytes[offset + index]}});
                    }
                }
                offset += *length;
                continue;
            }

            if (status < 0x80U || status > 0xefU)
            {
                error = "MIDI file contains an unsupported event status";
                return std::nullopt;
            }

            const auto dataLength = ((status & 0xf0U) == 0xc0U || (status & 0xf0U) == 0xd0U) ? std::size_t{1} : std::size_t{2};
            std::vector<std::uint8_t> message;
            message.reserve(1 + dataLength);
            message.push_back(status);
            if (firstDataByte.has_value())
                message.push_back(*firstDataByte);
            while (message.size() < 1 + dataLength)
            {
                if (offset >= trackEnd || bytes[offset] >= 0x80U)
                {
                    error = "MIDI channel event data is truncated";
                    return std::nullopt;
                }
                message.push_back(bytes[offset++]);
            }

            const auto channel = static_cast<std::uint8_t>((status & 0x0fU) + 1U);
            if (timingClock || channel < lowChannel || channel > highChannel)
                continue;

            const auto frameValue = frameAt(tick, trackIndex);
            if (!frameValue.has_value())
                return std::nullopt;
            const auto frame = *frameValue;
            if (frame < frameCount)
            {
                if (events.size() >= maxEventCount)
                {
                    error = "MIDIFileEcho exceeds the prepared event limit";
                    return std::nullopt;
                }
                events.push_back(MidiEvent{frame, std::move(message)});
            }
        }

    }

    std::stable_sort(events.begin(), events.end(), [](const MidiEvent& lhs, const MidiEvent& rhs) {
        return lhs.frame < rhs.frame;
    });
    return events;
}

}

std::optional<std::vector<MidiEvent>> parseStandardMidiFileChannelEvents(
    const std::vector<std::uint8_t>& bytes,
    double sampleRate,
    std::size_t frameCount,
    std::uint8_t lowChannel,
    std::uint8_t highChannel,
    std::size_t maxEventCount,
    std::string& error)
{
    return parseStandardMidiFileEvents(
        bytes,
        sampleRate,
        frameCount,
        lowChannel,
        highChannel,
        maxEventCount,
        error);
}

std::optional<std::vector<std::size_t>> parseStandardMidiFileTimingClockFrames(
    const std::vector<std::uint8_t>& bytes,
    double sampleRate,
    std::size_t maxEventCount,
    std::string& error)
{
    const auto messages = parseStandardMidiFileEvents(bytes, sampleRate, std::numeric_limits<std::size_t>::max(), 1, 16,
        maxEventCount, error, false, true);
    if (!messages.has_value())
        return std::nullopt;
    std::vector<std::size_t> frames;
    auto running = true;
    for (const auto& message : *messages)
    {
        const auto byte = message.bytes.front();
        if (byte == 0xfaU || byte == 0xfbU)
            running = true;
        else if (byte == 0xfcU)
            running = false;
        else if (running)
            frames.push_back(message.frame);
    }
    return frames;
}

std::optional<std::vector<MidiEvent>> parseStandardMidiFileChannelEventsByBeat(
    const std::vector<std::uint8_t>& bytes,
    double framesPerBeat,
    std::uint8_t lowChannel,
    std::uint8_t highChannel,
    std::size_t maxEventCount,
    std::string& error)
{
    return parseStandardMidiFileEvents(bytes, framesPerBeat, std::numeric_limits<std::size_t>::max(), lowChannel,
        highChannel, maxEventCount, error, true);
}

}
