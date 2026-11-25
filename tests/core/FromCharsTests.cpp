// talas::fromChars on every platform: the grammar of each format, where
// parsing stops, rounding, range errors and what it leaves alone. On macOS
// a second run, with the realtime probe inserted, checks that it neither
// allocates nor locks (FromCharsParityTests compares it with libc++).
#include "talas/core/FromChars.h"

#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#if defined(__APPLE__)
#include "../support/RealtimeProbe.h"

#include <dlfcn.h>
#endif

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Float>
using BitsOf = std::conditional_t<sizeof(Float) == 8, std::uint64_t, std::uint32_t>;

template <typename Float>
constexpr Float untouched = std::bit_cast<Float>(static_cast<BitsOf<Float>>(0x4123456789abcdefULL));

const char* formatName(std::chars_format format)
{
    switch (format)
    {
    case std::chars_format::general: return "general";
    case std::chars_format::fixed: return "fixed";
    case std::chars_format::scientific: return "scientific";
    case std::chars_format::hex: return "hex";
    }
    return "?";
}

template <typename Float>
std::string describe(std::string_view text, std::chars_format format)
{
    std::string shown(text);
    if (shown.size() > 80)
        shown = shown.substr(0, 60) + "...(" + std::to_string(shown.size()) + " chars)";
    return std::string(sizeof(Float) == 8 ? "double " : "float ") + formatName(format) + " \"" + shown + "\"";
}

// Parsing text gives error, stops after used characters and, unless the
// text is invalid, writes expected (compared bit for bit).
template <typename Float>
void expect(
    std::string_view text,
    std::chars_format format,
    std::errc error,
    std::size_t used,
    Float expected = untouched<Float>)
{
    auto value = untouched<Float>;
    const auto result = talas::fromChars(text.data(), text.data() + text.size(), value, format);
    const auto name = describe<Float>(text, format);
    require(result.ec == error, name + " gives the wrong error");
    require(result.ptr == text.data() + used,
        name + " stops after " + std::to_string(result.ptr - text.data()) + " characters, not "
            + std::to_string(used));
    if (error == std::errc::invalid_argument)
        expected = untouched<Float>;
    std::ostringstream message;
    message << name << " gives " << std::hexfloat << value << " (bits " << std::hex
            << std::bit_cast<BitsOf<Float>>(value) << "), not " << std::hexfloat << expected;
    require(std::bit_cast<BitsOf<Float>>(value) == std::bit_cast<BitsOf<Float>>(expected), message.str());
}

template <typename Float>
void expectValue(std::string_view text, std::chars_format format, Float expected)
{
    expect<Float>(text, format, std::errc{}, text.size(), expected);
}

template <typename Float>
void expectInvalid(std::string_view text, std::chars_format format)
{
    expect<Float>(text, format, std::errc::invalid_argument, 0);
}

constexpr std::array allFormats{
    std::chars_format::general,
    std::chars_format::fixed,
    std::chars_format::scientific,
    std::chars_format::hex,
};

constexpr auto general = std::chars_format::general;
constexpr auto fixed = std::chars_format::fixed;
constexpr auto scientific = std::chars_format::scientific;
constexpr auto hex = std::chars_format::hex;
constexpr auto ok = std::errc{};
constexpr auto invalid = std::errc::invalid_argument;
constexpr auto outOfRange = std::errc::result_out_of_range;

void testWhatNeverParses()
{
    for (const auto format : allFormats)
    {
        for (const std::string_view text : {"", "-", "+1", " 1", "\t1", "--1", "-+1", ".", "-.", ".p1", "x", "+inf", " nan"})
        {
            expectInvalid<double>(text, format);
            expectInvalid<float>(text, format);
        }
    }
    double value = untouched<double>;
    const auto result = talas::fromChars(nullptr, nullptr, value);
    require(result.ec == invalid && result.ptr == nullptr && value == untouched<double>,
        "an empty range is invalid");
}

void testDecimalGrammar()
{
    expectValue<double>("1", general, 1.0);
    expectValue<double>("-0", general, -0.0);
    expectValue<double>("0.5", general, 0.5);
    expectValue<double>(".5", general, 0.5);
    expectValue<double>("-.5", general, -0.5);
    expectValue<double>("5.", general, 5.0);
    expectValue<double>("1e5", general, 1e5);
    expectValue<double>("1E+5", general, 1e5);
    expectValue<double>("1.5e-3", general, 1.5e-3);
    expectValue<double>("1.e2", general, 100.0);
    expectValue<double>("00012.50", general, 12.5);
    expectValue<double>("1e0000000000000000000000000000005", general, 1e5);
    // An 'e' without exponent digits is not part of the number.
    expect<double>("1e", general, ok, 1, 1.0);
    expect<double>("1e+", general, ok, 1, 1.0);
    expect<double>("1e-x", general, ok, 1, 1.0);
    expect<double>("1e+-5", general, ok, 1, 1.0);
    expect<double>("2.5e5e5", general, ok, 5, 2.5e5);
    expect<double>("1..2", general, ok, 2, 1.0);
    expect<double>("1.2.3", general, ok, 3, 1.2);
    expect<double>("1,5", general, ok, 1, 1.0);
    expect<double>("0x10", general, ok, 1, 0.0);
    expect<double>("1p3", general, ok, 1, 1.0);
    expect<double>("12abc", general, ok, 2, 12.0);
    expectInvalid<double>("e5", general);
    expectInvalid<double>("-e5", general);

    // fixed never reads an exponent.
    expect<double>("1e5", fixed, ok, 1, 1.0);
    expect<double>("2.5E-3", fixed, ok, 3, 2.5);
    expect<double>("1e400", fixed, ok, 1, 1.0);
    expect<double>("1.00000000000000000001e5", fixed, ok, 22, 1.0);
    expectValue<double>("123.25", fixed, 123.25);

    // scientific needs one.
    expectInvalid<double>("1", scientific);
    expectInvalid<double>("1.5", scientific);
    expectInvalid<double>("1e", scientific);
    expectInvalid<double>("1e+", scientific);
    expectValue<double>("1e5", scientific, 1e5);
    expectValue<double>("-2.5E-1", scientific, -0.25);
    expect<double>("1e5e5", scientific, ok, 3, 1e5);
}

void testHexadecimalGrammar()
{
    expectValue<double>("1", hex, 1.0);
    expectValue<double>("a", hex, 10.0);
    expectValue<double>("F.8", hex, 15.5);
    expectValue<double>(".8", hex, 0.5);
    expectValue<double>("1p3", hex, 8.0);
    expectValue<double>("1.8P-1", hex, 0.75);
    expectValue<double>("-e5", hex, -229.0);
    expectValue<double>("1e5", hex, 485.0);
    expect<double>("1.8p", hex, ok, 3, 1.5);
    expect<double>("1.8p+", hex, ok, 3, 1.5);
    expect<double>("1fp3x", hex, ok, 4, 248.0);
    // No "0x": the 'x' ends the number.
    expect<double>("0x1p3", hex, ok, 1, 0.0);
    expectInvalid<double>("p3", hex);
    expectInvalid<double>(".p1", hex);
    expectValue<float>("1.fffffep127", hex, std::numeric_limits<float>::max());
    expectValue<double>("1.fffffffffffffp1023", hex, std::numeric_limits<double>::max());
}

void testInfinityAndNan()
{
    constexpr auto infinity = std::numeric_limits<double>::infinity();
    const auto quietNan = std::bit_cast<double>(std::uint64_t{0x7ff8000000000000});
    const auto negativeQuietNan = std::bit_cast<double>(std::uint64_t{0xfff8000000000000});
    for (const auto format : allFormats)
    {
        expectValue<double>("inf", format, infinity);
        expectValue<double>("-INF", format, -infinity);
        expectValue<double>("InFiNiTy", format, infinity);
        expect<double>("infinit", format, ok, 3, infinity);
        expect<double>("infx", format, ok, 3, infinity);
        expectInvalid<double>("in", format);
        expectValue<double>("nan", format, quietNan);
        expectValue<double>("-NaN", format, negativeQuietNan);
        expectValue<double>("nan()", format, quietNan);
        expectValue<double>("nan(Abc_123)", format, quietNan);
        // nan(snan) is still the quiet NaN: no payload is read.
        expectValue<double>("nan(snan)", format, quietNan);
        expect<double>("nan(", format, ok, 3, quietNan);
        expect<double>("nan(12", format, ok, 3, quietNan);
        expect<double>("nan(1-2)", format, ok, 3, quietNan);
        expectInvalid<double>("na", format);
        expectValue<float>("-nan", format, std::bit_cast<float>(std::uint32_t{0xffc00000}));
        expectValue<float>("infinity", format, std::numeric_limits<float>::infinity());
    }
}

void testRounding()
{
    // 2^53 + 1 lies halfway between two doubles: ties go to the even one.
    expectValue<double>("9007199254740993", general, 9007199254740992.0);
    expectValue<double>("9007199254740995", general, 9007199254740996.0);
    expectValue<double>("9007199254740993.0000000000000000000000000001", general, 9007199254740994.0);
    expectValue<float>("16777217", general, 16777216.0F);
    expectValue<float>("16777217.000000000000001", general, 16777218.0F);
    expectValue<double>("0.1", general, 0.1);
    expectValue<float>("0.1", general, 0.1F);
    expectValue<double>("0.30000000000000004", general, 0.30000000000000004);
    expectValue<double>("2.2250738585072011e-308", general, 0x0.fffffffffffffp-1022);
    expectValue<double>("4.9406564584124654e-324", general, 0x1p-1074);
    expectValue<double>("1e-320", general, 0x0.00000000007e8p-1022);
    expectValue<float>("1e-45", general, 0x1p-149F);
    expectValue<float>("3.4028235e38", general, std::numeric_limits<float>::max());

    // Digits far past the 800th still decide the rounding.
    const std::string zeros(1000, '0');
    expectValue<double>("9007199254740993." + zeros, general, 9007199254740992.0);
    expectValue<double>("9007199254740993." + zeros + "1", general, 9007199254740994.0);
    expectValue<double>("9007199254740993" + zeros + "1e-1001", general, 9007199254740994.0);
    expectValue<double>("0." + zeros + "1e1001", general, 1.0);
    expectValue<double>("1" + zeros + "e-1000", general, 1.0);

    // Hexadecimal digits past the sixteenth, too.
    expectValue<double>("1.00000000000008", hex, 1.0);
    expectValue<double>("1.000000000000080000000001", hex, 1.0 + 0x1p-52);
    expectValue<double>("1.00000000000018", hex, 1.0 + 0x1p-51);
    expectValue<double>("0.000000000000000000000000000000001p132", hex, 1.0);
    expectValue<double>("1.8p-1074", hex, 0x1p-1073);
    expectValue<double>("1.0000001p-1075", hex, 0x1p-1074);
    expectValue<float>("1.000001p-150", hex, 0x1p-149F);
    expectValue<float>("1.ffffffp126", hex, 0x1p127F);
}

void testRange()
{
    constexpr auto infinity = std::numeric_limits<double>::infinity();
    // Out of range writes infinity or zero, signed as the text is.
    expect<double>("1e400", general, outOfRange, 5, infinity);
    expect<double>("-1e400", general, outOfRange, 6, -infinity);
    expect<double>("1.7976931348623159e308", general, outOfRange, 22, infinity);
    expect<double>("1e-400", general, outOfRange, 6, 0.0);
    expect<double>("-1e-400", scientific, outOfRange, 7, -0.0);
    expect<double>("2e-324", general, outOfRange, 6, 0.0);
    expect<double>("1e999999999999999999999999", general, outOfRange, 26, infinity);
    expect<double>("1e-999999999999999999999999", general, outOfRange, 27, 0.0);
    expect<float>("3.4028236e38", general, outOfRange, 12, std::numeric_limits<float>::infinity());
    expect<float>("1e-46", general, outOfRange, 5, 0.0F);
    expect<float>("-1e39", general, outOfRange, 5, -std::numeric_limits<float>::infinity());
    expect<double>("1p1024", hex, outOfRange, 6, infinity);
    expect<double>("1.fffffffffffff8p1023", hex, outOfRange, 21, infinity);
    expect<double>("1p-1075", hex, outOfRange, 7, 0.0);
    expect<double>("-1p-1080", hex, outOfRange, 8, -0.0);
    expect<float>("1p-150", hex, outOfRange, 6, 0.0F);
    // Zero is never out of range, nor is a subnormal.
    expectValue<double>("0e999999999999", general, 0.0);
    expectValue<double>("-0.000e-999999999", general, -0.0);
    expectValue<double>("0p99999", hex, 0.0);
    expectValue<double>("1e-310", general, 1e-310);
}

void testErrnoIsLeftAlone()
{
    for (const std::string_view text : {"1e400", "1e-400", "1.5", "nan"})
    {
        errno = 4321;
        double value = 0.0;
        (void) talas::fromChars(text.data(), text.data() + text.size(), value);
        require(errno == 4321, "parsing \"" + std::string(text) + "\" changes errno");
    }
}

#if defined(__APPLE__)
// Texts that reach every path: short and long decimals, cut digits,
// hexadecimal rounding, range errors and specials.
void parseEveryPath(std::string_view longDecimal)
{
    const std::array<std::string_view, 13> texts{
        "1.5", "0.1", "-2.5e-3", "123456789012345678901234567890", longDecimal, "1e400", "1e-400",
        "4.9406564584124654e-324", "1.fffffffffffff8p1023", "1.8p-1074", "-inf", "nan(1)", "12abc"};
    for (const auto text : texts)
    {
        for (const auto format : allFormats)
        {
            double value = 0.0;
            float single = 0.0F;
            (void) talas::fromChars(text.data(), text.data() + text.size(), value, format);
            (void) talas::fromChars(text.data(), text.data() + text.size(), single, format);
        }
    }
}

// Run before anything else parses, so the first conversion is probed too.
void testRealtimeSafety(bool requireProbe)
{
    const auto begin = reinterpret_cast<TalasRealtimeProbeBegin>(
        dlsym(RTLD_DEFAULT, TALAS_REALTIME_PROBE_BEGIN_SYMBOL));
    const auto end = reinterpret_cast<TalasRealtimeProbeEnd>(
        dlsym(RTLD_DEFAULT, TALAS_REALTIME_PROBE_END_SYMBOL));
    if (begin == nullptr || end == nullptr)
    {
        require(!requireProbe, "the realtime probe is not inserted");
        return;
    }
    const std::string longDecimal = "1." + std::string(900, '3') + "7e-30";
    TalasRealtimeProbeReport report{};
    begin();
    parseEveryPath(longDecimal);
    end(&report);
    std::uint64_t total = 0;
    for (const auto count : report.counts)
        total += count;
    require(total == 0,
        std::to_string(total) + " realtime-unsafe calls while parsing, first "
            + (report.firstFunction != nullptr ? report.firstFunction : "?"));
    std::cout << "fromChars made no allocation, lock, file I/O or sleep\n";
}
#endif
}

int main(int argc, char** argv)
{
    try
    {
#if defined(__APPLE__)
        testRealtimeSafety(argc > 1 && std::strcmp(argv[1], "--require-realtime-probe") == 0);
#else
        (void) argc;
        (void) argv;
#endif
        testWhatNeverParses();
        testDecimalGrammar();
        testHexadecimalGrammar();
        testInfinityAndNan();
        testRounding();
        testRange();
        testErrnoIsLeftAlone();
        std::cout << "fromChars tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
