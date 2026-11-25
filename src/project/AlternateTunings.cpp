#include "talas/project/AlternateTunings.h"

#include "talas/core/FromChars.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <map>

namespace talas::project::tunings
{
namespace
{
[[nodiscard]] bool positiveFinite(double value) noexcept
{
    return std::isfinite(value) && value > 0.0;
}

// The octave and degree of a key `steps` keys from the tonic, `degrees`
// keys to the octave.
[[nodiscard]] std::pair<int, int> octaveAndDegree(int steps, int degrees) noexcept
{
    const auto octave = steps >= 0 ? steps / degrees : -((-steps + degrees - 1) / degrees);
    return {octave, steps - octave * degrees};
}

[[nodiscard]] std::string shortest(double value)
{
    std::array<char, 64> buffer{};
    const auto [end, failed] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return failed == std::errc{} ? std::string(buffer.data(), end) : std::string{"0"};
}

[[nodiscard]] std::optional<double> number(std::string_view text)
{
    double value = 0.0;
    const auto [end, failed] = talas::fromChars(text.data(), text.data() + text.size(), value);
    if (failed != std::errc{} || end != text.data() + text.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

[[nodiscard]] std::vector<std::string_view> words(std::string_view line)
{
    std::vector<std::string_view> found;
    std::size_t offset = 0;
    while (offset < line.size())
    {
        while (offset < line.size() && (line[offset] == ' ' || line[offset] == '\t' || line[offset] == '\r'))
            ++offset;
        const auto start = offset;
        while (offset < line.size() && line[offset] != ' ' && line[offset] != '\t' && line[offset] != '\r')
            ++offset;
        if (offset > start)
            found.push_back(line.substr(start, offset - start));
    }
    return found;
}

[[nodiscard]] int degreesOf(const Tuning& tuning) noexcept
{
    return tuning.method == Method::EqualTempered || tuning.method == Method::TwoInterval
        ? tuning.degreesPerOctave
        : static_cast<int>(scaleDegrees);
}

// The scale degrees of a two-interval scale within the octave: the powers
// of the generating interval, each halved while it is 2 or more, in
// ascending order.
[[nodiscard]] std::optional<std::vector<double>> twoIntervalDegrees(const Tuning& tuning, std::string& error)
{
    if (!positiveFinite(tuning.generatingInterval) || tuning.generatingInterval <= 1.0)
    {
        error = "The generating interval is a ratio above 1";
        return std::nullopt;
    }
    std::vector<double> degrees;
    auto value = 1.0;
    for (int degree = 0; degree < tuning.degreesPerOctave; ++degree)
    {
        degrees.push_back(value);
        value *= tuning.generatingInterval;
        while (value >= 2.0)
            value /= 2.0;
    }
    std::ranges::sort(degrees);
    for (std::size_t degree = 1; degree < degrees.size(); ++degree)
    {
        if (degrees[degree] - degrees[degree - 1] < 1.0e-12)
        {
            error = "The generating interval repeats a scale degree before the octave has "
                + std::to_string(tuning.degreesPerOctave) + " degrees";
            return std::nullopt;
        }
    }
    return degrees;
}
}

std::string_view methodName(Method method) noexcept
{
    switch (method)
    {
    case Method::RatioScale:
        return "ratioScale";
    case Method::CentsScale:
        return "centsScale";
    case Method::EqualTempered:
        return "equalTempered";
    case Method::TwoInterval:
        return "twoInterval";
    case Method::FromTextFile:
        return "fromTextFile";
    }
    return "equalTempered";
}

std::optional<KeyFrequencies> keyFrequencies(const Tuning& tuning, std::string& error)
{
    if (tuning.tonicKey < 0 || tuning.tonicKey >= static_cast<int>(keyCount))
    {
        error = "The tonic key number is a MIDI key number from 0 to 127";
        return std::nullopt;
    }
    if (!positiveFinite(tuning.tonicFrequency))
    {
        error = "The tonic frequency is a frequency above 0 hz";
        return std::nullopt;
    }
    if ((tuning.method == Method::EqualTempered || tuning.method == Method::TwoInterval)
        && (tuning.degreesPerOctave < 1 || tuning.degreesPerOctave > maximumDegreesPerOctave))
    {
        error = "The scale degrees per octave are a whole number from 1 to 1200";
        return std::nullopt;
    }
    KeyFrequencies keys{};
    // Each octave from the tonic repeats the degrees' ratios an octave up.
    const auto byDegrees = [&](const std::vector<double>& degrees) {
        for (std::size_t key = 0; key < keyCount; ++key)
        {
            const auto [octave, degree] =
                octaveAndDegree(static_cast<int>(key) - tuning.tonicKey, static_cast<int>(degrees.size()));
            keys[key] = tuning.tonicFrequency * std::exp2(octave) * degrees[static_cast<std::size_t>(degree)];
        }
    };
    switch (tuning.method)
    {
    case Method::RatioScale:
    {
        std::vector<double> degrees;
        for (const auto& ratio : tuning.ratios)
        {
            if (!positiveFinite(ratio.numerator) || !positiveFinite(ratio.denominator))
            {
                error = "Each ratio's numerator and denominator are numbers above 0";
                return std::nullopt;
            }
            degrees.push_back(ratio.numerator / ratio.denominator);
        }
        if (degrees.front() != 1.0)
        {
            error = "The first scale degree is the tonic, 1/1";
            return std::nullopt;
        }
        byDegrees(degrees);
        break;
    }
    case Method::CentsScale:
    {
        std::vector<double> degrees;
        for (const auto cents : tuning.cents)
        {
            if (!std::isfinite(cents))
            {
                error = "Each scale degree's cents are a number";
                return std::nullopt;
            }
            degrees.push_back(std::exp2(cents / 1200.0));
        }
        if (tuning.cents.front() != 0.0)
        {
            error = "The first scale degree is the tonic, 0 cents";
            return std::nullopt;
        }
        byDegrees(degrees);
        break;
    }
    case Method::EqualTempered:
        for (std::size_t key = 0; key < keyCount; ++key)
        {
            keys[key] = tuning.tonicFrequency
                * std::exp2(static_cast<double>(static_cast<int>(key) - tuning.tonicKey) / tuning.degreesPerOctave);
        }
        break;
    case Method::TwoInterval:
    {
        const auto degrees = twoIntervalDegrees(tuning, error);
        if (!degrees)
            return std::nullopt;
        byDegrees(*degrees);
        break;
    }
    case Method::FromTextFile:
        keys = tuning.fileFrequencies;
        break;
    }
    if (!std::ranges::all_of(keys, positiveFinite))
    {
        error = "Every key's frequency is a frequency above 0 hz";
        return std::nullopt;
    }
    return keys;
}

std::vector<Degree> octaveFromTonic(const Tuning& tuning, const KeyFrequencies& frequencies)
{
    std::vector<Degree> octave;
    if (tuning.tonicKey < 0 || tuning.tonicKey >= static_cast<int>(keyCount))
        return octave;
    const auto tonic = frequencies[static_cast<std::size_t>(tuning.tonicKey)];
    for (int degree = 0; degree < degreesOf(tuning); ++degree)
    {
        const auto key = tuning.tonicKey + degree;
        if (key >= static_cast<int>(keyCount))
            break;
        const auto frequency = frequencies[static_cast<std::size_t>(key)];
        octave.push_back({key, frequency, frequency / tonic, 1200.0 * std::log2(frequency / tonic)});
    }
    return octave;
}

std::string tuningText(const Tuning& tuning, const KeyFrequencies& frequencies)
{
    std::string text;
    for (std::size_t key = 0; key < keyCount; ++key)
    {
        // Six significant digits.
        std::array<char, 64> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "%zu %.6g hz\n", key, frequencies[key]);
        text += buffer.data();
    }
    text += std::to_string(endOfKeys) + "\n";
    text += "method " + std::string{methodName(tuning.method)} + "\n";
    text += "tonic " + std::to_string(tuning.tonicKey) + " " + shortest(tuning.tonicFrequency) + "\n";
    switch (tuning.method)
    {
    case Method::RatioScale:
        text += "ratios";
        for (const auto& ratio : tuning.ratios)
            text += " " + shortest(ratio.numerator) + "/" + shortest(ratio.denominator);
        text += "\n";
        break;
    case Method::CentsScale:
        text += "cents";
        for (const auto cents : tuning.cents)
            text += " " + shortest(cents);
        text += "\n";
        break;
    case Method::EqualTempered:
        text += "degrees " + std::to_string(tuning.degreesPerOctave) + "\n";
        break;
    case Method::TwoInterval:
        text += "degrees " + std::to_string(tuning.degreesPerOctave) + "\n";
        text += "interval " + shortest(tuning.generatingInterval) + "\n";
        break;
    case Method::FromTextFile:
        break;
    }
    return text;
}

std::optional<Tuning> readTuningText(std::string_view text, Method method, int tonicKey, std::string& error)
{
    KeyFrequencies frequencies{};
    std::array<bool, keyCount> listed{};
    std::map<std::string, std::vector<std::string_view>, std::less<>> saved;
    auto ended = false;
    std::size_t lineNumber = 0;
    for (std::size_t offset = 0; offset <= text.size();)
    {
        const auto end = std::min(text.find('\n', offset), text.size());
        const auto line = text.substr(offset, end - offset);
        offset = end + 1;
        ++lineNumber;
        const auto parts = words(line);
        if (parts.empty())
            continue;
        if (ended)
        {
            saved[std::string{parts.front()}] = {parts.begin() + 1, parts.end()};
            continue;
        }
        const auto key = number(parts.front());
        if (key && *key == endOfKeys)
        {
            ended = true;
            continue;
        }
        const auto frequency = parts.size() >= 2 ? number(parts[1]) : std::nullopt;
        if (!key || *key < 0.0 || *key >= static_cast<double>(keyCount) || *key != std::floor(*key) || !frequency
            || !positiveFinite(*frequency) || parts.size() > 3 || (parts.size() == 3 && parts[2] != "hz"))
        {
            error = "Line " + std::to_string(lineNumber) + " is no key number and frequency in hertz";
            return std::nullopt;
        }
        frequencies[static_cast<std::size_t>(*key)] = *frequency;
        listed[static_cast<std::size_t>(*key)] = true;
    }
    if (!ended)
    {
        error = "A tuning file ends its keys with a line of key 999";
        return std::nullopt;
    }
    if (const auto missing = std::ranges::find(listed, false); missing != listed.end())
    {
        error = "The file lists no frequency for key " + std::to_string(missing - listed.begin());
        return std::nullopt;
    }

    Tuning tuning;
    tuning.method = method;
    tuning.tonicKey = tonicKey;
    if (tonicKey < 0 || tonicKey >= static_cast<int>(keyCount))
    {
        error = "The tonic key number is a MIDI key number from 0 to 127";
        return std::nullopt;
    }
    tuning.tonicFrequency = frequencies[static_cast<std::size_t>(tonicKey)];
    tuning.fileFrequencies = frequencies;
    const auto savedField = [&](std::string_view name) -> const std::vector<std::string_view>* {
        const auto found = saved.find(name);
        return found == saved.end() ? nullptr : &found->second;
    };
    if (const auto* tonic = savedField("tonic"); tonic != nullptr && tonic->size() == 2)
    {
        const auto key = number((*tonic)[0]);
        const auto frequency = number((*tonic)[1]);
        if (key && frequency && *key == std::floor(*key))
        {
            tuning.tonicKey = static_cast<int>(*key);
            tuning.tonicFrequency = *frequency;
        }
    }
    const auto* savedMethod = savedField("method");
    const auto sameMethod = savedMethod != nullptr && savedMethod->size() == 1
        && savedMethod->front() == methodName(method);
    const auto fromSaved = [&]() -> bool {
        switch (method)
        {
        case Method::RatioScale:
        {
            const auto* ratios = savedField("ratios");
            if (ratios == nullptr || ratios->size() != scaleDegrees)
                return false;
            for (std::size_t degree = 0; degree < scaleDegrees; ++degree)
            {
                const auto slash = (*ratios)[degree].find('/');
                const auto numerator = number((*ratios)[degree].substr(0, slash));
                const auto denominator =
                    slash == std::string_view::npos ? std::optional{1.0} : number((*ratios)[degree].substr(slash + 1));
                if (!numerator || !denominator)
                    return false;
                tuning.ratios[degree] = {*numerator, *denominator};
            }
            return true;
        }
        case Method::CentsScale:
        {
            const auto* cents = savedField("cents");
            if (cents == nullptr || cents->size() != scaleDegrees)
                return false;
            for (std::size_t degree = 0; degree < scaleDegrees; ++degree)
            {
                const auto value = number((*cents)[degree]);
                if (!value)
                    return false;
                tuning.cents[degree] = *value;
            }
            return true;
        }
        case Method::EqualTempered:
        case Method::TwoInterval:
        {
            const auto* degrees = savedField("degrees");
            const auto value = degrees != nullptr && degrees->size() == 1 ? number(degrees->front()) : std::nullopt;
            if (!value || *value != std::floor(*value))
                return false;
            tuning.degreesPerOctave = static_cast<int>(*value);
            if (method == Method::EqualTempered)
                return true;
            const auto* interval = savedField("interval");
            const auto generator = interval != nullptr && interval->size() == 1 ? number(interval->front()) : std::nullopt;
            if (!generator)
                return false;
            tuning.generatingInterval = *generator;
            return true;
        }
        case Method::FromTextFile:
            return true;
        }
        return false;
    };
    if (!sameMethod || !fromSaved())
    {
        // Parameters of this method approximating the file's frequencies,
        // read from the octave above the tonic (or the highest whole one).
        auto base = tuning.tonicKey;
        while (base + static_cast<int>(scaleDegrees) - 1 >= static_cast<int>(keyCount))
            base -= static_cast<int>(scaleDegrees);
        const auto ratioAbove = [&](int degree) {
            return frequencies[static_cast<std::size_t>(base + degree)] / frequencies[static_cast<std::size_t>(base)];
        };
        // The equal steps to the octave the whole keyboard averages.
        const auto steps = std::log2(frequencies.back() / frequencies.front());
        const auto degrees = steps > 0.0
            ? std::clamp(static_cast<int>(std::lround(static_cast<double>(keyCount - 1) / steps)), 1, maximumDegreesPerOctave)
            : 12;
        switch (method)
        {
        case Method::RatioScale:
            for (std::size_t degree = 0; degree < scaleDegrees; ++degree)
                tuning.ratios[degree] = {degree == 0 ? 1.0 : ratioAbove(static_cast<int>(degree)), 1.0};
            break;
        case Method::CentsScale:
            for (std::size_t degree = 0; degree < scaleDegrees; ++degree)
                tuning.cents[degree] = degree == 0 ? 0.0 : 1200.0 * std::log2(ratioAbove(static_cast<int>(degree)));
            break;
        case Method::EqualTempered:
            tuning.degreesPerOctave = degrees;
            break;
        case Method::TwoInterval:
        {
            // The degree nearest a fifth generates the scale.
            tuning.degreesPerOctave = degrees;
            auto fifth = static_cast<int>(std::lround(degrees * 7.0 / 12.0));
            fifth = std::clamp(fifth, 1, static_cast<int>(scaleDegrees) - 1);
            tuning.generatingInterval = ratioAbove(fifth);
            break;
        }
        case Method::FromTextFile:
            break;
        }
    }
    if (!keyFrequencies(tuning, error))
        return std::nullopt;
    return tuning;
}

std::array<Ratio, scaleDegrees> equalTemperedRatios() noexcept
{
    std::array<Ratio, scaleDegrees> ratios{};
    for (std::size_t degree = 0; degree < scaleDegrees; ++degree)
        ratios[degree] = {std::exp2(static_cast<double>(degree) / 12.0), 1.0};
    return ratios;
}

std::array<Ratio, scaleDegrees> exampleRatios(RatioExample example) noexcept
{
    switch (example)
    {
    case RatioExample::EqualTempered:
        return equalTemperedRatios();
    case RatioExample::Just:
        // A 5-limit just scale.
        return {Ratio{1, 1}, {16, 15}, {9, 8}, {6, 5}, {5, 4}, {4, 3}, {45, 32}, {3, 2}, {8, 5}, {5, 3}, {9, 5}, {15, 8}};
    case RatioExample::Pythagorean:
        return {Ratio{1, 1}, {256, 243}, {9, 8}, {32, 27}, {81, 64}, {4, 3}, {729, 512}, {3, 2}, {128, 81}, {27, 16},
            {16, 9}, {243, 128}};
    }
    return equalTemperedRatios();
}

std::array<double, scaleDegrees> exampleCents(CentsExample example) noexcept
{
    switch (example)
    {
    case CentsExample::EqualTempered:
        return Tuning{}.cents;
    case CentsExample::Meantone:
        return {0.0, 76.049, 193.157, 310.265, 386.314, 503.422, 579.471, 696.578, 772.627, 889.735, 1006.843, 1082.892};
    case CentsExample::WerckmeisterIII:
        return {0.0, 90.225, 192.180, 294.135, 390.225, 498.045, 588.270, 696.090, 792.180, 888.270, 996.090, 1092.180};
    }
    return Tuning{}.cents;
}
}
