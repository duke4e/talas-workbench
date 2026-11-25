#include "talas/runtime/MtcSmpteProtocol.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>

namespace
{
void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::array<talas::runtime::MtcQuarterFramePiece, 8> piecesFor(
    std::uint8_t hour,
    std::uint8_t minute,
    std::uint8_t second,
    std::uint8_t frame,
    std::uint8_t rateCode)
{
    return {{
        {0, static_cast<std::uint8_t>(frame & 0x0F)},
        {1, static_cast<std::uint8_t>((frame >> 4) & 0x01)},
        {2, static_cast<std::uint8_t>(second & 0x0F)},
        {3, static_cast<std::uint8_t>((second >> 4) & 0x03)},
        {4, static_cast<std::uint8_t>(minute & 0x0F)},
        {5, static_cast<std::uint8_t>((minute >> 4) & 0x03)},
        {6, static_cast<std::uint8_t>(hour & 0x0F)},
        {7, static_cast<std::uint8_t>(((hour >> 4) & 0x01)
            | ((rateCode & 0x03) << 1))},
    }};
}

void testClosedRatesAndLabels()
{
    using namespace talas::runtime;
    require(smpteRateRational(SmpteRate::Fps24)
                == SmpteRateRational{24, 1, 24, false}
            && smpteRateRational(SmpteRate::Fps25)
                == SmpteRateRational{25, 1, 25, false}
            && smpteRateRational(SmpteRate::Fps2997Drop)
                == SmpteRateRational{30'000, 1'001, 30, true}
            && smpteRateRational(SmpteRate::Fps30)
                == SmpteRateRational{30, 1, 30, false},
        "the SMPTE rate profile should remain closed and exact");
    require(smpteFramesPer24Hours(SmpteRate::Fps24) == 2'073'600
            && smpteFramesPer24Hours(SmpteRate::Fps25) == 2'160'000
            && smpteFramesPer24Hours(SmpteRate::Fps2997Drop) == 2'589'408
            && smpteFramesPer24Hours(SmpteRate::Fps30) == 2'592'000,
        "every accepted rate should have one exact 24-hour frame count");

    SmpteTimeCode value;
    require(makeSmpteTimeCode(
                SmpteRate::Fps2997Drop, 0, 0, 59, 29,
                SmpteDirection::Forward, value)
            && value.absoluteFrame == 1'799,
        "drop-frame should preserve the final label before minute one");
    require(!makeSmpteTimeCode(
                SmpteRate::Fps2997Drop, 0, 1, 0, 0,
                SmpteDirection::Forward, value)
            && !makeSmpteTimeCode(
                SmpteRate::Fps2997Drop, 0, 1, 0, 1,
                SmpteDirection::Forward, value),
        "drop-frame minute boundaries should reject skipped labels");
    require(makeSmpteTimeCode(
                SmpteRate::Fps2997Drop, 0, 1, 0, 2,
                SmpteDirection::Forward, value)
            && value.absoluteFrame == 1'800,
        "drop-frame minute one should resume at label 02");
    require(makeSmpteTimeCode(
                SmpteRate::Fps2997Drop, 0, 10, 0, 0,
                SmpteDirection::Forward, value)
            && value.absoluteFrame == 17'982,
        "every tenth drop-frame minute should retain labels 00 and 01");
    SmpteTimeCode minuteBoundary;
    require(smpteTimeCodeFromAbsoluteFrame(
                SmpteRate::Fps2997Drop, 1'800,
                SmpteDirection::Forward, minuteBoundary)
            && minuteBoundary.minute == 1 && minuteBoundary.second == 0
            && minuteBoundary.frame == 2,
        "absolute drop-frame conversion should skip minute-one labels 00 and 01");
    require(!makeSmpteTimeCode(
                SmpteRate::Fps24, 24, 0, 0, 0,
                SmpteDirection::Forward, value)
            && !makeSmpteTimeCode(
                SmpteRate::Fps24, 0, 0, 0, 24,
                SmpteDirection::Forward, value),
        "closed labels should reject out-of-range hours and frames");

    for (std::uint32_t totalMinute = 0; totalMinute < 24 * 60;
         ++totalMinute)
    {
        const auto hour = static_cast<std::uint8_t>(totalMinute / 60);
        const auto minute = static_cast<std::uint8_t>(totalMinute % 60);
        const auto firstFrame = static_cast<std::uint8_t>(
            minute % 10 == 0 ? 0 : 2);
        SmpteTimeCode boundary;
        require(makeSmpteTimeCode(
                    SmpteRate::Fps2997Drop,
                    hour,
                    minute,
                    0,
                    firstFrame,
                    SmpteDirection::Forward,
                    boundary),
            "every canonical drop-frame minute boundary should be valid");
        SmpteTimeCode reopened;
        require(smpteTimeCodeFromAbsoluteFrame(
                    SmpteRate::Fps2997Drop,
                    boundary.absoluteFrame,
                    SmpteDirection::Forward,
                    reopened)
                && reopened == boundary,
            "every drop-frame minute boundary should round-trip exactly");
    }
}

void testAbsoluteRoundTripAndWrap()
{
    using namespace talas::runtime;
    for (const auto rate : {SmpteRate::Fps24, SmpteRate::Fps25,
             SmpteRate::Fps2997Drop, SmpteRate::Fps30})
    {
        const auto limit = smpteFramesPer24Hours(rate);
        for (const auto frame : {std::uint64_t{0}, std::uint64_t{1},
                 limit / 10, limit - 2, limit - 1})
        {
            SmpteTimeCode value;
            require(smpteTimeCodeFromAbsoluteFrame(
                        rate, frame, SmpteDirection::Forward, value)
                    && value.absoluteFrame == frame
                    && isValidSmpteTimeCode(value),
                "absolute SMPTE frames should round-trip canonically");
        }
    }

    SmpteTimeCode end;
    require(makeSmpteTimeCode(
                SmpteRate::Fps30, 23, 59, 59, 29,
                SmpteDirection::Forward, end),
        "last non-drop label should be valid");
    SmpteTimeCode wrappedValue;
    bool wrapped = false;
    require(addSmpteFrames(end, 2, wrappedValue, wrapped)
            && wrapped && wrappedValue.hour == 0
            && wrappedValue.minute == 0 && wrappedValue.second == 0
            && wrappedValue.frame == 1,
        "frame addition should expose and canonicalize 24-hour wrap");
    require(addSmpteFrames(
                wrappedValue,
                smpteFramesPer24Hours(SmpteRate::Fps30),
                wrappedValue,
                wrapped)
            && wrapped && wrappedValue.frame == 1,
        "an exact 24-hour addition should remain an explicit wrap");
}

void testQuarterFrameDecodingAndCompensation()
{
    using namespace talas::runtime;
    MtcQuarterFramePiece piece;
    const std::array<std::uint8_t, 2> bytes{0xF1, 0x72};
    require(decodeMtcQuarterFramePiece(bytes, piece)
                == MtcDecodeResult::Accepted
            && piece == MtcQuarterFramePiece{7, 2},
        "one exact quarter-frame message should decode its type and nibble");
    const std::array<std::uint8_t, 1> shortBytes{0xF1};
    require(decodeMtcQuarterFramePiece(shortBytes, piece)
                == MtcDecodeResult::InvalidShape,
        "truncated quarter-frame messages should reject");
    for (const auto reserved : std::array{
             std::array<std::uint8_t, 2>{0xF1, 0x12},
             std::array<std::uint8_t, 2>{0xF1, 0x34},
             std::array<std::uint8_t, 2>{0xF1, 0x54},
             std::array<std::uint8_t, 2>{0xF1, 0x78}})
    {
        require(decodeMtcQuarterFramePiece(reserved, piece)
                    == MtcDecodeResult::InvalidPiece,
            "quarter-frame reserved bits should reject instead of being masked");
    }

    for (const auto fixture : std::array{
             std::array<std::uint8_t, 5>{1, 2, 3, 4, 0},
             std::array<std::uint8_t, 5>{1, 2, 3, 4, 1},
             std::array<std::uint8_t, 5>{1, 2, 3, 4, 2},
             std::array<std::uint8_t, 5>{1, 2, 3, 4, 3}})
    {
        const auto pieces = piecesFor(
            fixture[0], fixture[1], fixture[2], fixture[3], fixture[4]);
        SmpteTimeCode decoded;
        bool wrapped = false;
        require(decodeMtcQuarterFrameSet(
                    pieces, SmpteDirection::Forward, true,
                    decoded, wrapped)
                == MtcDecodeResult::Accepted
                && !wrapped && decoded.frame == fixture[3] + 2,
            "forward quarter-frame sets should apply exactly two frames of compensation");
    }

    auto reordered = piecesFor(1, 2, 3, 4, 0);
    reordered[3].type = 4;
    SmpteTimeCode ignored;
    bool wrapped = false;
    require(decodeMtcQuarterFrameSet(
                reordered, SmpteDirection::Forward, true,
                ignored, wrapped)
            == MtcDecodeResult::InvalidPiece,
        "reordered quarter-frame pieces should fail closed");
    auto reservedSet = piecesFor(1, 2, 3, 4, 0);
    reservedSet[5].value = 0x04;
    require(decodeMtcQuarterFrameSet(
                reservedSet, SmpteDirection::Forward, true,
                ignored, wrapped)
            == MtcDecodeResult::InvalidPiece,
        "direct quarter-frame sets should reject reserved bits too");
    const auto reverse = piecesFor(1, 2, 3, 4, 0);
    require(decodeMtcQuarterFrameSet(
                reverse, SmpteDirection::Reverse, true,
                ignored, wrapped)
            == MtcDecodeResult::UnsupportedDirection,
        "reverse MTC remains outside the accepted profile");

    const auto endOfDay = piecesFor(23, 59, 59, 29, 3);
    require(decodeMtcQuarterFrameSet(
                endOfDay, SmpteDirection::Forward, true,
                ignored, wrapped)
            == MtcDecodeResult::Accepted
            && wrapped && ignored.hour == 0 && ignored.minute == 0
            && ignored.second == 0 && ignored.frame == 1,
        "quarter-frame compensation should expose the 24-hour discontinuity");
}

void testFullFrameAndDropFrameDecoding()
{
    using namespace talas::runtime;
    const std::array<std::uint8_t, 10> message{
        0xF0, 0x7F, 0x10, 0x01, 0x01,
        static_cast<std::uint8_t>((2U << 5) | 1U),
        10, 0, 0, 0xF7};
    MtcFullFrameMessage decoded;
    require(decodeMtcFullFrame(message, decoded)
                == MtcDecodeResult::Accepted
            && decoded.deviceId == 0x10
            && decoded.timeCode.rate == SmpteRate::Fps2997Drop
            && decoded.timeCode.dropFrame
            && decoded.timeCode.absoluteFrame == 125'874,
        "full-frame MTC should decode exact device, rate, label, and authority frame");

    auto invalidLabel = message;
    invalidLabel[6] = 1;
    require(decodeMtcFullFrame(invalidLabel, decoded)
                == MtcDecodeResult::InvalidLabel,
        "full-frame drop-frame skipped labels should reject");
    auto nonRealtime = message;
    nonRealtime[1] = 0x7E;
    require(decodeMtcFullFrame(nonRealtime, decoded)
                == MtcDecodeResult::InvalidShape,
        "non-realtime universal SysEx should not broaden MTC authority");
}

void testExactSampleMapping()
{
    using namespace talas::runtime;
    for (const auto sampleRate : {44'100U, 48'000U, 96'000U})
    {
        SmpteTimeCode oneSecond;
        require(makeSmpteTimeCode(
                    SmpteRate::Fps24, 0, 0, 1, 0,
                    SmpteDirection::Forward, oneSecond),
            "one-second label should be valid");
        std::uint64_t sampleFrame = 0;
        require(smpteTimeCodeToSampleFrame(
                    oneSecond, sampleRate, sampleFrame)
                && sampleFrame == sampleRate,
            "integer-rate SMPTE seconds should map to exact sample frames");
    }

    SmpteTimeCode dropFrame;
    require(makeSmpteTimeCode(
                SmpteRate::Fps2997Drop, 0, 10, 0, 0,
                SmpteDirection::Forward, dropFrame),
        "ten-minute drop-frame boundary should be valid");
    std::uint64_t sampleFrame = 0;
    require(smpteTimeCodeToSampleFrame(dropFrame, 48'000, sampleFrame)
            && sampleFrame == 28'799'971,
        "29.97 DF mapping should use exact 30000/1001 rational rounding");
    require(!smpteTimeCodeToSampleFrame(dropFrame, 0, sampleFrame),
        "zero sample rate should fail closed");
}
}

int main()
{
    static_assert(std::is_trivially_copyable_v<talas::runtime::SmpteTimeCode>);
    static_assert(std::is_trivially_copyable_v<talas::runtime::MtcQuarterFramePiece>);
    static_assert(std::is_trivially_copyable_v<talas::runtime::MtcFullFrameMessage>);
    static_assert(talas::runtime::mtcQuarterFramePieceCount == 8);
    static_assert(talas::runtime::mtcForwardCompensationFrames == 2);
    static_assert(talas::runtime::mtcSignalTimeoutNanoseconds == 500'000'000ULL);
    static_assert(talas::runtime::smpteRoundToNearestDivisor == 2);
    testClosedRatesAndLabels();
    testAbsoluteRoundTripAndWrap();
    testQuarterFrameDecodingAndCompensation();
    testFullFrameAndDropFrameDecoding();
    testExactSampleMapping();
    std::cout << "MTC/SMPTE protocol tests passed\n";
    return 0;
}
