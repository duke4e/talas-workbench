// talas::fromChars against libc++'s floating-point std::from_chars, which
// needs macOS 26: this file alone is compiled for macOS 26, so the test runs
// only there. Every comparison checks ec, ptr and the value's bits, for
// double and float in all four formats.
//
// One libc++ defect is left out. In fixed format libc++ stops before an
// exponent, as it should, but when its fast path fails its slower one
// rereads the text and applies that exponent to the value:
// "1.00000000000000000001e5" gives 100000 with ptr before the 'e'. Where the
// text goes on with an exponent, fixed format is compared with libc++
// reading only the prefix it consumed, and the test counts how often libc++
// differed. talas::fromChars reads only what it consumes.
#include "talas/core/FromChars.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace
{
constexpr std::array formats{
    std::chars_format::general,
    std::chars_format::fixed,
    std::chars_format::scientific,
    std::chars_format::hex,
};

std::uint64_t comparisons = 0;
std::uint64_t mismatches = 0;

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

const char* errorName(std::errc error)
{
    if (error == std::errc{})
        return "ok";
    if (error == std::errc::invalid_argument)
        return "invalid_argument";
    if (error == std::errc::result_out_of_range)
        return "result_out_of_range";
    return "other";
}

template <typename Float>
using BitsOf = std::conditional_t<sizeof(Float) == 8, std::uint64_t, std::uint32_t>;

struct Parsed
{
    std::from_chars_result result;
    std::uint64_t bits = 0;
};

template <typename Float>
Parsed parseWith(bool standard, const char* first, const char* last, std::chars_format format)
{
    // An unusual value, so an untouched value is told from a written one.
    auto value = std::bit_cast<Float>(static_cast<BitsOf<Float>>(0x4123456789abcdefULL));
    const auto result = standard ? std::from_chars(first, last, value, format)
                                 : talas::fromChars(first, last, value, format);
    return {result, std::bit_cast<BitsOf<Float>>(value)};
}

bool same(const Parsed& a, const Parsed& b)
{
    return a.result.ec == b.result.ec && a.result.ptr == b.result.ptr && a.bits == b.bits;
}

std::uint64_t fixedExponentDefects = 0;

// [first, last) may be a prefix of a longer text, so a parser that reads
// past last answers differently.
template <typename Float>
void compare(const char* first, const char* last, std::chars_format format)
{
    auto wanted = parseWith<Float>(true, first, last, format);
    const auto got = parseWith<Float>(false, first, last, format);
    ++comparisons;
    const auto* stop = wanted.result.ptr;
    if (format == std::chars_format::fixed && stop != last && (*stop == 'e' || *stop == 'E'))
    {
        const auto prefix = parseWith<Float>(true, first, stop, format);
        if (!same(prefix, wanted))
            ++fixedExponentDefects;
        wanted = prefix;
    }
    if (same(wanted, got))
        return;
    if (++mismatches > 25)
        return;
    std::string shown(first, last);
    if (shown.size() > 160)
        shown = shown.substr(0, 120) + "...(" + std::to_string(shown.size()) + " chars)..."
            + shown.substr(shown.size() - 30);
    std::cerr << "MISMATCH " << (sizeof(Float) == 8 ? "double" : "float") << ' ' << formatName(format)
              << " \"" << shown << "\": std " << errorName(wanted.result.ec) << " used "
              << (wanted.result.ptr - first) << " bits " << std::hex << wanted.bits << std::dec
              << "; talas " << errorName(got.result.ec) << " used " << (got.result.ptr - first) << " bits "
              << std::hex << got.bits << std::dec << '\n';
}

void compareAll(std::string_view text)
{
    const auto* first = text.data();
    const auto* last = first + text.size();
    for (const auto format : formats)
    {
        compare<double>(first, last, format);
        compare<float>(first, last, format);
    }
}

void compareEveryPrefix(std::string_view text)
{
    for (std::size_t length = 0; length <= text.size(); ++length)
        compareAll(text.substr(0, length));
}

void edgeCases()
{
    const char* const cases[] = {
        "", "-", "+", "+1", " 1", "\t1", "1", "-1", "--1", "-+1", "0", "-0", "00", "-00.000",
        ".", "-.", ".5", "5.", "-.5", "..5", "5..", "1.2.3", "1,5", "1_000",
        "1e", "1e+", "1e-", "1E5", "1e+5", "1e-5", "1e5x", "1e5e5", "1.e5", ".e5", "e5", "-e5",
        "1e+-5", "1e-+5", "1e--5", "1e 5", "1ee5", "1E+0005", "1e000000000000000000000000000000003",
        "0x10", "0X1p3", "0x", "-0x1", "0x1.8p1", "x1", "1p3", "p", "1.8p", "1.8p+", "1.8p+3", "1.8P-3",
        ".p1", "f.8", "a", "A.Bp1", "abc.defp-10", "1.fffffep127", "1.ffffffp127",
        "1.fffffffffffffp1023", "1.fffffffffffff8p1023", "1.fffffffffffff7ffffp1023",
        "1p-1074", "1p-1075", "1.0000001p-1075", "1.8p-1075", "0.8p-1074", "1p-1076",
        "1p-149", "1p-150", "1.000001p-150", "1.8p-150", "1p-126", "1p-127", "1p-1022", "1p-1023",
        "fffffffffffffffffffffffffffffffffp0", "0.00000000000000000000000000000001p128",
        "1p2147483647", "1p-2147483648", "1p9223372036854775807", "1p-9223372036854775808",
        "inf", "-inf", "INF", "Inf", "iNf", "infinity", "INFINITY", "InFiNiTy", "infinit", "infin",
        "infx", "in", "i", "-infinity", "infinityx", "infinity5", "-in",
        "nan", "-nan", "NaN", "NAN", "nA", "n", "nan()", "nan(123)", "nan(abc_9)", "nan(", "nan(1",
        "nan(snan)", "nan(ind)", "nan(-)", "nan(0x7)", "nan(1.5)", "nan( )", "nan())", "nan(()",
        "-nan(ABC_xyz_019)", "nanx", "nan5", "nan(\xc3\xa9)",
        "1e400", "-1e400", "1e-400", "-1e-400", "1e-320", "1e-310", "4e-324", "2e-324", "3e-324",
        "2.4703282292062327e-324", "2.4703282292062328e-324", "2.47032822920623272e-324",
        "4.9406564584124654e-324", "2.2250738585072011e-308", "2.2250738585072014e-308",
        "2.2250738585072012e-308", "1e-45", "1e-46", "7e-46", "7.1e-46", "1.4e-45",
        "1.17549435e-38", "1.1754942e-38", "3.4028236e38", "3.4028235e38", "3.40282357e38",
        "3.40282347e38", "3.4028235677973366e38", "3.4028235677973367e38",
        "1.7976931348623157e308", "1.7976931348623158e308", "1.7976931348623159e308",
        "179769313486231580793728971405303415079934132710037826936173778980444968292764750946649017977587207096330286416692887910946555547851940402630657488671505820681908902000708383676273854845817711531764475730270069855571366959622842914819860834936475292719074168444365510704342711559699508093042880177904174497791",
        "179769313486231580793728971405303415079934132710037826936173778980444968292764750946649017977587207096330286416692887910946555547851940402630657488671505820681908902000708383676273854845817711531764475730270069855571366959622842914819860834936475292719074168444365510704342711559699508093042880177904174497792",
        "0e999999999999999999", "-0e-999999999999999999", "1e-999999999999999999",
        "1e999999999999999999", "1e2147483647", "1e2147483648", "1e-2147483648", "1e-2147483649",
        "1e4294967296", "1e-4294967296", "1e9223372036854775807", "1e9223372036854775808",
        "1e-9223372036854775808", "1e-9223372036854775809", "00000000000000000000001e-20",
        "0.1", "0.2", "0.3", "0.30000000000000004", "9007199254740993", "9007199254740992.5",
        "9007199254740993.0000000000000000000000000000000000001", "16777217", "16777217.0000001",
        "123456789012345678901234567890", "1.5e-3", "2.5", "3.5", "-2.5e-1",
    };
    for (const auto* text : cases)
        compareEveryPrefix(text);

    // A number's digits moving its exponent: each is 1, 0.1 or out of range.
    const std::string zeros(1000, '0');
    for (const auto& text : {
             "0." + zeros + "1e1001",
             "0." + zeros + "1e1000",
             "1" + zeros + "e-1000",
             "1" + zeros + "e-1001",
             zeros + "1e-1",
             "0." + zeros + "1e-2147483648",
             "1" + zeros + "e2147483647",
             "0." + zeros + zeros + "1e2001",
             "1" + zeros + zeros + zeros + "e-3000",
             "1." + zeros + "1",
             "1." + zeros + "0",
             zeros + "." + zeros,
             "-" + zeros + "." + zeros + "e99999999",
             "0." + zeros + "1p4004",
             "1" + zeros + "p-4000",
         })
        compareAll(text);
}

// Short texts from the characters numbers are made of, with every prefix.
void randomFragments(std::mt19937_64& random)
{
    constexpr std::string_view alphabet =
        "0123456789012345678901234567890123456789.....eeeEEppPP++---xXabcdefABCDEF"
        "infinityINFINITYnanNAN()()__ \t,/:9";
    std::uniform_int_distribution<std::size_t> letter(0, alphabet.size() - 1);
    std::uniform_int_distribution<int> length(0, 24);
    std::string text;
    for (int sample = 0; sample < 40000; ++sample)
    {
        text.clear();
        const auto count = length(random);
        for (int index = 0; index < count; ++index)
            text += alphabet[letter(random)];
        compareEveryPrefix(text);
    }
}

std::string randomDigits(std::mt19937_64& random, int count, bool nonzeroFirst)
{
    std::uniform_int_distribution<int> digit(0, 9);
    std::string digits;
    for (int index = 0; index < count; ++index)
    {
        auto value = digit(random);
        if (index == 0 && nonzeroFirst && value == 0)
            value = 1;
        digits += static_cast<char>('0' + value);
    }
    return digits;
}

// Well-formed decimal numbers of every length and magnitude.
void randomDecimals(std::mt19937_64& random)
{
    std::uniform_int_distribution<int> percent(0, 99);
    std::uniform_int_distribution<int> shortCount(1, 20);
    std::uniform_int_distribution<int> mediumCount(21, 120);
    std::uniform_int_distribution<int> longCount(700, 1100);
    std::uniform_int_distribution<int> zeroCount(0, 6);
    std::uniform_int_distribution<int> doubleExponent(-360, 330);
    std::uniform_int_distribution<int> floatExponent(-60, 50);
    std::uniform_int_distribution<int> wideExponent(-1500, 1500);
    for (int sample = 0; sample < 120000; ++sample)
    {
        const auto kind = percent(random);
        int count = shortCount(random);
        if (kind >= 80 && kind < 97)
            count = mediumCount(random);
        else if (kind >= 97)
            count = longCount(random);
        auto digits = std::string(static_cast<std::size_t>(zeroCount(random)), '0')
            + randomDigits(random, count, false);
        std::string text = percent(random) < 30 ? "-" : "";
        const auto point = percent(random);
        if (point < 25)
        {
            text += digits;
        }
        else
        {
            std::uniform_int_distribution<std::size_t> at(0, digits.size());
            const auto split = at(random);
            text += digits.substr(0, split) + "." + digits.substr(split);
        }
        const auto exponentKind = percent(random);
        if (exponentKind < 85)
        {
            int exponent = 0;
            if (exponentKind < 45)
                exponent = doubleExponent(random);
            else if (exponentKind < 75)
                exponent = floatExponent(random);
            else
                exponent = wideExponent(random);
            text += percent(random) < 50 ? 'e' : 'E';
            if (exponent >= 0 && percent(random) < 30)
                text += '+';
            text += std::to_string(exponent);
        }
        compareAll(text);
    }
}

// Every finite value written back: shortest, at fixed precisions, in
// scientific and fixed notation and in hexadecimal.
template <typename Float>
void roundTrips(std::mt19937_64& random)
{
    std::array<char, 2048> buffer{};
    std::uniform_int_distribution<int> precision(0, 40);
    for (int sample = 0; sample < 25000; ++sample)
    {
        auto bits = static_cast<BitsOf<Float>>(random());
        auto value = std::bit_cast<Float>(bits);
        if (!std::isfinite(value))
            continue;
        const auto write = [&](auto... arguments)
        {
            const auto [end, error] =
                std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, arguments...);
            if (error == std::errc{})
                compareAll(std::string_view(buffer.data(), static_cast<std::size_t>(end - buffer.data())));
        };
        write();
        write(std::chars_format::scientific);
        write(std::chars_format::fixed);
        write(std::chars_format::hex);
        write(std::chars_format::general, precision(random));
        write(std::chars_format::scientific, precision(random));
        write(std::chars_format::hex, precision(random) / 3);
    }
}

// a + b, both unsigned decimal integers.
std::string addDecimal(const std::string& a, const std::string& b)
{
    std::string sum;
    int carry = 0;
    for (std::size_t index = 0; index < std::max(a.size(), b.size()) || carry != 0; ++index)
    {
        int digit = carry;
        if (index < a.size())
            digit += a[a.size() - 1 - index] - '0';
        if (index < b.size())
            digit += b[b.size() - 1 - index] - '0';
        sum.insert(sum.begin(), static_cast<char>('0' + digit % 10));
        carry = digit / 10;
    }
    return sum;
}

// The exact decimal value of a double, as an integer and a power of ten.
struct ExactDecimal
{
    std::string digits;
    int exponent = 0;
};

ExactDecimal exactDecimal(double value)
{
    // Every double is a multiple of 2^-1074, which has 1074 decimal places.
    std::array<char, 2048> buffer{};
    const auto [end, error] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, 1075);
    std::string text(buffer.data(), end);
    const auto point = text.find('.');
    ExactDecimal exact;
    exact.digits = text.substr(0, point) + text.substr(point + 1);
    exact.exponent = -static_cast<int>(text.size() - point - 1);
    (void) error;
    return exact;
}

std::string decimalText(std::string digits, int exponent, bool negative)
{
    const auto firstNonzero = digits.find_first_not_of('0');
    digits = firstNonzero == std::string::npos ? "0" : digits.substr(firstNonzero);
    return (negative ? "-" : "") + digits + "e" + std::to_string(exponent);
}

// The midpoints between neighbouring values, exactly, a hair above and
// below them and with digits far past the 800th.
template <typename Float>
void midpoints(std::mt19937_64& random)
{
    std::uniform_int_distribution<int> percent(0, 99);
    const auto test = [](Float low, bool negative)
    {
        const auto high = std::nextafter(low, std::numeric_limits<Float>::infinity());
        const auto a = exactDecimal(static_cast<double>(low));
        auto b = exactDecimal(std::isinf(high) ? 0.0 : static_cast<double>(high));
        if (std::isinf(high))
        {
            // Past the largest value its step continues: the midpoint is the
            // overflow threshold.
            const auto step = low - std::nextafter(low, Float{0});
            b.digits = addDecimal(a.digits, exactDecimal(static_cast<double>(step)).digits);
        }
        // (a + b) / 2 = 5 (a + b) / 10.
        const auto sum = addDecimal(a.digits, b.digits);
        auto midpoint = addDecimal(addDecimal(addDecimal(sum, sum), addDecimal(sum, sum)), sum);
        const auto exponent = a.exponent - 1;
        compareAll(decimalText(midpoint, exponent, negative));
        compareAll(decimalText(midpoint + "1", exponent - 1, negative));
        compareAll(decimalText(midpoint + std::string(1200, '0') + "1", exponent - 1201, negative));
        compareAll(decimalText(midpoint + std::string(1200, '0'), exponent - 1200, negative));
        // Just below: the last nonzero digit one less, then nines.
        auto below = midpoint;
        const auto lastNonzero = below.find_last_not_of('0');
        if (lastNonzero != std::string::npos)
        {
            --below[lastNonzero];
            below = below.substr(0, lastNonzero + 1) + std::string(below.size() - lastNonzero - 1, '9');
            compareAll(decimalText(below + "9999", exponent - 4, negative));
            compareAll(decimalText(below + std::string(900, '9'), exponent - 900, negative));
        }
    };
    const auto denormMin = std::numeric_limits<Float>::denorm_min();
    for (const auto low : {Float{0}, denormMin, 2 * denormMin, 3 * denormMin,
             std::numeric_limits<Float>::min(), std::nextafter(std::numeric_limits<Float>::min(), Float{0}),
             std::numeric_limits<Float>::max(), std::nextafter(std::numeric_limits<Float>::max(), Float{0}),
             Float{1}, static_cast<Float>(0.1)})
    {
        test(low, false);
        test(low, true);
    }
    for (int sample = 0; sample < 1500; ++sample)
    {
        auto bits = static_cast<BitsOf<Float>>(random());
        bits &= ~(BitsOf<Float>{1} << (sizeof(Float) * 8 - 1));
        if (percent(random) < 25)
        {
            // Subnormal and smallest normal values.
            bits &= (BitsOf<Float>{1} << (std::numeric_limits<Float>::digits + 1)) - 1;
        }
        const auto low = std::bit_cast<Float>(bits);
        if (!std::isfinite(low))
            continue;
        test(low, percent(random) < 20);
    }
}

std::string hexDigits(std::uint64_t value)
{
    std::array<char, 32> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 16);
    (void) error;
    return std::string(buffer.data(), end);
}

// Hexadecimal significands longer than sixteen digits, and midpoints with
// their neighbours, across the exponent range.
template <typename Float>
void hexadecimals(std::mt19937_64& random)
{
    constexpr auto digits = std::numeric_limits<Float>::digits;
    std::uniform_int_distribution<int> percent(0, 99);
    std::uniform_int_distribution<int> extraDigits(1, 30);
    std::uniform_int_distribution<int> exponent(
        std::numeric_limits<Float>::min_exponent - digits - 70, std::numeric_limits<Float>::max_exponent + 10);
    std::uniform_int_distribution<int> hexDigit(0, 15);
    constexpr std::string_view hexAlphabet = "0123456789abcdefABCDEF";
    for (int sample = 0; sample < 30000; ++sample)
    {
        const auto power = exponent(random);
        // A midpoint, 2m + 1 over one more bit than the type has.
        const auto significand =
            (random() & ((std::uint64_t{1} << digits) - 1)) | (std::uint64_t{1} << (digits - 1));
        const auto odd = significand * 2 + 1;
        const auto text = hexDigits(odd);
        const auto suffix = "p" + std::to_string(power);
        compareAll(text + suffix);
        const auto zeros = static_cast<std::size_t>(extraDigits(random));
        const auto shifted = "p" + std::to_string(power - 4 * static_cast<int>(zeros + 1));
        compareAll(text + std::string(zeros, '0') + "1" + shifted);
        compareAll(text + std::string(zeros, '0') + "0" + shifted);
        compareAll(hexDigits(odd - 1) + std::string(zeros + 1, 'f') + shifted);
        // Any long significand, with a point somewhere.
        std::string longText = percent(random) < 30 ? "-" : "";
        const auto count = extraDigits(random) + (percent(random) < 20 ? 40 : 0);
        std::uniform_int_distribution<int> pointAt(0, count);
        const auto point = pointAt(random);
        for (int index = 0; index < count; ++index)
        {
            if (index == point)
                longText += '.';
            longText += hexAlphabet[static_cast<std::size_t>(hexDigit(random) + (percent(random) < 30 ? 6 : 0))
                % hexAlphabet.size()];
        }
        if (percent(random) < 85)
            longText += (percent(random) < 50 ? "p" : "P") + std::to_string(power);
        compareAll(longText);
    }
}

void specialValues(std::mt19937_64& random)
{
    std::uniform_int_distribution<int> percent(0, 99);
    std::uniform_int_distribution<int> length(0, 9);
    constexpr std::string_view nanCharacters = "aZz09_9xX-.()+ ";
    std::uniform_int_distribution<std::size_t> nanCharacter(0, nanCharacters.size() - 1);
    for (int sample = 0; sample < 4000; ++sample)
    {
        std::string text = percent(random) < 40 ? "-" : "";
        std::string word = percent(random) < 50 ? "infinity" : "nan";
        for (auto& character : word)
        {
            if (percent(random) < 50)
                character = static_cast<char>(character - 'a' + 'A');
        }
        text += word;
        if (word.size() == 3)
        {
            text += '(';
            const auto count = length(random);
            for (int index = 0; index < count; ++index)
                text += nanCharacters[nanCharacter(random)];
            if (percent(random) < 70)
                text += ')';
        }
        text += randomDigits(random, length(random) / 3, false);
        compareEveryPrefix(text);
    }
}
}

int main()
{
    std::mt19937_64 random(20261125);
    edgeCases();
    randomFragments(random);
    randomDecimals(random);
    roundTrips<double>(random);
    roundTrips<float>(random);
    midpoints<double>(random);
    midpoints<float>(random);
    hexadecimals<double>(random);
    hexadecimals<float>(random);
    specialValues(random);
    if (mismatches != 0)
    {
        std::cerr << "FAIL: " << mismatches << " of " << comparisons
                  << " comparisons differ from std::from_chars\n";
        return 1;
    }
    std::cout << "fromChars matches std::from_chars in " << comparisons << " comparisons ("
              << fixedExponentDefects << " fixed-format texts compared without libc++'s exponent defect)\n";
    return 0;
}
