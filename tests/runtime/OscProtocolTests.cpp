#include "talas/runtime/OscProtocol.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using Bytes = std::vector<std::uint8_t>;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void appendBig32(Bytes& target, std::uint32_t value)
{
    target.push_back(static_cast<std::uint8_t>(value >> 24));
    target.push_back(static_cast<std::uint8_t>(value >> 16));
    target.push_back(static_cast<std::uint8_t>(value >> 8));
    target.push_back(static_cast<std::uint8_t>(value));
}

void appendBig64(Bytes& target, std::uint64_t value)
{
    appendBig32(target, static_cast<std::uint32_t>(value >> 32));
    appendBig32(target, static_cast<std::uint32_t>(value));
}

void appendOscString(Bytes& target, std::string_view value)
{
    target.insert(target.end(), value.begin(), value.end());
    target.push_back(0);
    while (target.size() % 4 != 0)
        target.push_back(0);
}

Bytes message(
    std::string_view address,
    char type,
    std::uint64_t payloadBits = 0)
{
    Bytes output;
    appendOscString(output, address);
    const std::array<char, 2> types{',', type};
    appendOscString(output, {types.data(), types.size()});
    if (type == 'i' || type == 'f')
        appendBig32(output, static_cast<std::uint32_t>(payloadBits));
    else if (type == 'd')
        appendBig64(output, payloadBits);
    return output;
}

Bytes bundle(
    const std::vector<Bytes>& elements,
    std::uint64_t timetag = 1)
{
    Bytes output{'#', 'b', 'u', 'n', 'd', 'l', 'e', 0};
    appendBig64(output, timetag);
    for (const auto& element : elements)
    {
        appendBig32(output, static_cast<std::uint32_t>(element.size()));
        output.insert(output.end(), element.begin(), element.end());
    }
    return output;
}

talas::runtime::OscPacketParseResult parse(
    const Bytes& bytes,
    std::span<talas::runtime::OscControlMessage> output)
{
    return talas::runtime::parseOscControlPacket(bytes, output);
}

void testClosedScalarMessages()
{
    std::array<talas::runtime::OscControlMessage, 4> output{};
    const auto integer = parse(message("/int", 'i', 1), output);
    require(integer.accepted() && integer.messageCount == 1
                && output[0].addressView() == "/int"
                && output[0].normalizedValue == 1.0,
            "big-endian int32 one should parse exactly");

    const auto floating = parse(
        message("/float", 'f', std::bit_cast<std::uint32_t>(0.25F)),
        output);
    require(floating.accepted()
                && output[0].normalizedValue == 0.25,
            "big-endian float32 should parse exactly");

    const auto doubled = parse(
        message("/double", 'd', std::bit_cast<std::uint64_t>(0.75)),
        output);
    require(doubled.accepted()
                && output[0].normalizedValue == 0.75,
            "big-endian float64 should parse exactly");
    require(parse(message("/true", 'T'), output).accepted()
                && output[0].normalizedValue == 1.0,
            "OSC true should normalize to one");
    require(parse(message("/false", 'F'), output).accepted()
                && output[0].normalizedValue == 0.0,
            "OSC false should normalize to zero");
}

void testMessagesFailClosed()
{
    std::array<talas::runtime::OscControlMessage, 2> output{};
    require(parse({}, output).status == talas::runtime::OscPacketStatus::Empty,
            "empty packet should reject distinctly");
    require(parse(message("/wild*", 'T'), output).status
                == talas::runtime::OscPacketStatus::InvalidAddress,
            "pattern addresses should reject");
    require(parse(message("/string", 's'), output).status
                == talas::runtime::OscPacketStatus::UnsupportedType,
            "unsupported OSC types should reject");
    require(parse(message("/int", 'i', 2), output).status
                == talas::runtime::OscPacketStatus::InvalidValue,
            "integer values outside normalized range should reject");
    require(parse(
                message("/nan", 'f', std::bit_cast<std::uint32_t>(
                    std::numeric_limits<float>::quiet_NaN())), output).status
                == talas::runtime::OscPacketStatus::InvalidValue,
            "nonfinite numeric values should reject");

    auto trailing = message("/true", 'T');
    appendBig32(trailing, 0);
    require(parse(trailing, output).status
                == talas::runtime::OscPacketStatus::Malformed,
            "trailing payload should reject");
    auto badPadding = message("/a", 'T');
    badPadding[3] = 1;
    require(parse(badPadding, output).status
                == talas::runtime::OscPacketStatus::Malformed,
            "nonzero OSC string padding should reject");
    auto truncated = message("/double", 'd', 0);
    truncated.pop_back();
    require(parse(truncated, output).status
                == talas::runtime::OscPacketStatus::Malformed,
            "truncated argument should reject");

    Bytes oversized(talas::runtime::maximumOscUdpPacketBytes + 1);
    require(parse(oversized, output).status
                == talas::runtime::OscPacketStatus::PacketTooLarge,
            "oversized UDP payload should reject before parsing");
}

void testImmediateBundlesAndLimits()
{
    std::array<talas::runtime::OscControlMessage, 128> output{};
    const auto accepted = parse(
        bundle({message("/a", 'f', std::bit_cast<std::uint32_t>(0.5F)),
                bundle({message("/b", 'T')})}),
        output);
    require(accepted.accepted() && accepted.messageCount == 2
                && output[0].addressView() == "/a"
                && output[1].addressView() == "/b"
                && output[0].timetag == talas::runtime::oscImmediateTimetag
                && output[1].timetag == talas::runtime::oscImmediateTimetag,
            "nested immediate bundle should preserve exact message order");

    constexpr auto timed = std::uint64_t{3'900'000'000} << 32;
    const auto timedBundle = parse(
        bundle({message("/a", 'T'),
                bundle({message("/b", 'F')})}, timed),
        output);
    require(timedBundle.accepted() && timedBundle.messageCount == 2
                && output[0].timetag == timed
                && output[1].timetag == timed,
            "immediate nested bundles should inherit the enclosing NTP timetag");
    const auto later = timed + 1;
    require(parse(
                bundle({bundle({message("/later", 'T')}, later)}, timed),
                output).accepted()
                && output[0].timetag == later,
            "nested bundles may advance their effective timetag");
    require(parse(
                bundle({bundle({message("/past", 'T')}, timed - 1)}, timed),
                output).status
                == talas::runtime::OscPacketStatus::RegressingTimetag,
            "nested bundles may not regress before the enclosing timetag");

    const auto atomicReject = parse(
        bundle({message("/accepted", 'T'), message("/bad*", 'T')}),
        output);
    require(!atomicReject.accepted() && atomicReject.messageCount == 0
                && output[0].addressSize == 0,
            "one malformed bundle member should reject the whole packet");

    Bytes tooDeep = message("/deep", 'T');
    for (int depth = 0; depth < 5; ++depth)
        tooDeep = bundle({tooDeep});
    require(parse(tooDeep, output).status
                == talas::runtime::OscPacketStatus::LimitExceeded,
            "bundle nesting beyond four should reject");

    std::vector<Bytes> tooMany;
    tooMany.reserve(129);
    for (int index = 0; index < 129; ++index)
        tooMany.push_back(message("/v" + std::to_string(index), 'F'));
    require(parse(bundle(tooMany), output).status
                == talas::runtime::OscPacketStatus::LimitExceeded,
            "more than 128 packet elements should reject");

    std::array<talas::runtime::OscControlMessage, 1> shortOutput{};
    require(parse(bundle({message("/a", 'T'), message("/b", 'F')}),
                  shortOutput).status
                == talas::runtime::OscPacketStatus::LimitExceeded,
            "insufficient fixed output capacity should reject");
}

void testNtpClockConversion()
{
    constexpr std::int64_t steady = 10'000'000'000;
    const auto anchor = talas::runtime::makeOscNtpClockAnchor(0, steady);
    require(anchor.valid()
                && anchor.timetag
                    == talas::runtime::oscNtpUnixEpochOffsetSeconds
                        * talas::runtime::oscNtpFractionUnitsPerSecond,
            "Unix epoch should map to the exact NTP epoch offset");
    require(!talas::runtime::makeOscNtpClockAnchor(-1, steady).valid()
                && !talas::runtime::makeOscNtpClockAnchor(0, 0).valid(),
            "negative wall time and nonpositive steady anchors should reject");
    constexpr auto lastCurrentEraUnixSecond =
        static_cast<std::int64_t>(
            std::numeric_limits<std::uint32_t>::max()
            - talas::runtime::oscNtpUnixEpochOffsetSeconds);
    require(talas::runtime::makeOscNtpClockAnchor(
                lastCurrentEraUnixSecond * 1'000'000'000,
                steady).valid()
            && !talas::runtime::makeOscNtpClockAnchor(
                (lastCurrentEraUnixSecond + 1) * 1'000'000'000,
                steady).valid(),
        "the closed current NTP era should reject rollover ambiguity");

    const auto immediate = talas::runtime::convertOscNtpTimetag(
        talas::runtime::oscImmediateTimetag, anchor);
    require(immediate.status == talas::runtime::OscNtpConversionStatus::Immediate
                && immediate.steadyDeadlineNanoseconds == steady,
            "OSC immediate should bind to the packet steady anchor");
    const auto now = talas::runtime::convertOscNtpTimetag(
        anchor.timetag, anchor);
    require(now.status == talas::runtime::OscNtpConversionStatus::Scheduled
                && now.steadyDeadlineNanoseconds == steady,
            "an exact anchor timetag should schedule at the anchor");

    const auto futureBoundary = talas::runtime::convertOscNtpTimetag(
        anchor.timetag + talas::runtime::maximumOscFutureTimetagDelta,
        anchor);
    require(futureBoundary.accepted()
                && futureBoundary.steadyDeadlineNanoseconds
                    == steady + 2'000'000'000,
            "the exact two-second future boundary should be accepted");
    require(talas::runtime::convertOscNtpTimetag(
                anchor.timetag
                    + talas::runtime::maximumOscFutureTimetagDelta + 1,
                anchor).status
                == talas::runtime::OscNtpConversionStatus::TooFar,
            "one NTP unit beyond the future horizon should reject");

    const auto lateBoundary = talas::runtime::convertOscNtpTimetag(
        anchor.timetag - talas::runtime::maximumOscLateTimetagDelta,
        anchor);
    require(lateBoundary.status == talas::runtime::OscNtpConversionStatus::Late
                && lateBoundary.steadyDeadlineNanoseconds
                    == steady - 100'000'000,
            "the greatest NTP delta not exceeding 100 ms should be accepted");
    require(talas::runtime::convertOscNtpTimetag(
                anchor.timetag
                    - talas::runtime::maximumOscLateTimetagDelta - 1,
                anchor).status
                == talas::runtime::OscNtpConversionStatus::TooLate,
            "one NTP unit beyond the late horizon should reject");

    constexpr auto oneSample48k =
        (talas::runtime::oscNtpFractionUnitsPerSecond + 24'000) / 48'000;
    const auto sample = talas::runtime::convertOscNtpTimetag(
        anchor.timetag + oneSample48k, anchor);
    require(sample.accepted()
                && sample.steadyDeadlineNanoseconds == steady + 20'833,
            "one 48 kHz sample should convert to the nearest nanosecond");
}

void testCanonicalFloat64Encoding()
{
    std::array<std::uint8_t,
               talas::runtime::maximumOscControlMessageBytes>
        packet{};
    std::array<talas::runtime::OscControlMessage, 1> decoded{};
    for (const auto value : {0.0, 0.5, 1.0})
    {
        const auto encoded = talas::runtime::encodeOscControlMessage(
            "/feedback/gain", value, packet);
        require(encoded.encoded() && encoded.packetSize == 28,
                "canonical OSC feedback packet should have exact padding and size");
        require(packet[16] == ',' && packet[17] == 'd'
                    && packet[18] == 0 && packet[19] == 0,
                "feedback encoder should emit only the canonical ,d typetag");
        const auto parsed = talas::runtime::parseOscControlPacket(
            std::span<const std::uint8_t>{
                packet.data(), encoded.packetSize},
            decoded);
        require(parsed.accepted()
                    && decoded[0].addressView() == "/feedback/gain"
                    && decoded[0].normalizedValue == value,
                "encoded feedback should round-trip through the accepted ingress profile");
    }

    std::array<std::uint8_t, 8> tooSmall{};
    require(talas::runtime::encodeOscControlMessage(
                "/feedback/gain", 0.5, tooSmall).status
                == talas::runtime::OscPacketEncodeStatus::OutputTooSmall,
            "short output should fail before writing a partial packet");
    require(talas::runtime::encodeOscControlMessage(
                "/wild*", 0.5, packet).status
                == talas::runtime::OscPacketEncodeStatus::InvalidAddress,
            "encoder should share the exact address authority");
    require(talas::runtime::encodeOscControlMessage(
                "/feedback/gain",
                std::numeric_limits<double>::infinity(),
                packet).status
                == talas::runtime::OscPacketEncodeStatus::InvalidValue,
            "encoder should reject nonfinite output");

    const auto minimum = talas::runtime::encodeOscControlMessage(
        "/a", -0.0, packet);
    require(minimum.encoded() && minimum.packetSize == 16,
            "minimum exact address should use one four-byte OSC string slot");
    require(talas::runtime::parseOscControlPacket(
                std::span<const std::uint8_t>{
                    packet.data(), minimum.packetSize},
                decoded).accepted()
                && std::signbit(decoded.front().normalizedValue),
            "canonical float64 encoding should preserve negative zero exactly");

    const auto maximumAddress = "/" + std::string(
        talas::project::maximumOscAddressBytes - 1, 'a');
    const auto maximum = talas::runtime::encodeOscControlMessage(
        maximumAddress, 1.0, packet);
    require(maximum.encoded()
                && maximum.packetSize
                    == talas::runtime::maximumOscControlMessageBytes,
            "maximum exact address should fill the fixed packet buffer exactly");
}
}

int main()
{
    testClosedScalarMessages();
    testMessagesFailClosed();
    testImmediateBundlesAndLimits();
    testNtpClockConversion();
    testCanonicalFloat64Encoding();
    return 0;
}
