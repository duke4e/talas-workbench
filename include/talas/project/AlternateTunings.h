#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The Design Alternate Tunings Tool: five
// ways to tune the 128 MIDI key numbers around a tonic key that plays the
// tonic frequency in every tuning, one octave of the scale as the Tool lists
// it, and the text file a tuning is saved in.
namespace talas::project::tunings
{
inline constexpr std::size_t keyCount = 128;
inline constexpr std::size_t scaleDegrees = 12;
inline constexpr int maximumDegreesPerOctave = 1200;
// The key number that ends the frequencies of a tuning file.
inline constexpr int endOfKeys = 999;

using KeyFrequencies = std::array<double, keyCount>;

enum class Method : std::uint8_t
{
    // Twelve scale degrees, each a ratio to the tonic.
    RatioScale,
    // Twelve scale degrees, each so many cents above the tonic.
    CentsScale,
    // Any number of equal steps to the octave.
    EqualTempered,
    // Any number of degrees, the powers of a generating interval brought
    // within the octave.
    TwoInterval,
    // Each key's frequency as a file gives it.
    FromTextFile,
};

struct Ratio
{
    double numerator = 1.0;
    double denominator = 1.0;

    bool operator==(const Ratio&) const = default;
};

// The ratios of an equal-tempered scale, which the
// Ratio Scale page shows until it is changed.
[[nodiscard]] std::array<Ratio, scaleDegrees> equalTemperedRatios() noexcept;

struct Tuning
{
    // The fixed point: this key plays this frequency in every tuning; by
    // default middle C at its equal-tempered frequency.
    int tonicKey = 60;
    double tonicFrequency = 261.6255653005986;
    Method method = Method::EqualTempered;
    // The scale degrees from the tonic up, the first the tonic itself (1/1
    // and 0 cents), so that the tonic keeps its frequency. The ratios and
    // the cents default to equal temperament.
    std::array<Ratio, scaleDegrees> ratios = equalTemperedRatios();
    std::array<double, scaleDegrees> cents{0, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1100};
    int degreesPerOctave = 12;
    double generatingInterval = 1.5;
    // From Text File: each key's frequency in hertz.
    KeyFrequencies fileFrequencies{};

    bool operator==(const Tuning&) const = default;
};

// The 128 key frequencies of the tuning, or nothing with the field at fault
// in error.
[[nodiscard]] std::optional<KeyFrequencies> keyFrequencies(const Tuning& tuning, std::string& error);

// A scale degree as the Tool's central view lists it: its key, its
// frequency, its ratio to the tonic and its cents above the tonic.
struct Degree
{
    int key = 0;
    double frequency = 0.0;
    double ratio = 1.0;
    double cents = 0.0;
};

// The octave from the tonic key up, as many keys as the tuning has degrees
// to the octave (12 for a file), those that are MIDI keys.
[[nodiscard]] std::vector<Degree> octaveFromTonic(const Tuning& tuning, const KeyFrequencies& frequencies);

// The text file of a tuning: a line "key frequency hz" for
// each key from 0 to 127, a line with key 999, and then the tuning's own
// parameters, which a file read for its frequencies alone ignores.
[[nodiscard]] std::string tuningText(const Tuning& tuning, const KeyFrequencies& frequencies);

// A tuning read from a text file for `method`: the parameters the
// file saved for that method where it saved them, otherwise parameters of
// that method approximating the file's frequencies, which must list every
// key before the line with key 999. The tonic key stays `tonicKey` and
// plays the file's frequency for it, unless the file saved a tonic.
[[nodiscard]] std::optional<Tuning> readTuningText(
    std::string_view text, Method method, int tonicKey, std::string& error);

[[nodiscard]] std::string_view methodName(Method method) noexcept;

// The buttons that preset the Ratio Scale's ratios and the Cents Scale's
// cents to example scales:
// equal temperament, its ratios the twelfth roots of two over one; a
// 5-limit just scale; the Pythagorean scale of pure fifths; quarter-comma
// meantone; Werckmeister III. Which examples these are is this project's
// choice.
enum class RatioExample : std::uint8_t
{
    EqualTempered,
    Just,
    Pythagorean,
};
enum class CentsExample : std::uint8_t
{
    EqualTempered,
    Meantone,
    WerckmeisterIII,
};
[[nodiscard]] std::array<Ratio, scaleDegrees> exampleRatios(RatioExample example) noexcept;
[[nodiscard]] std::array<double, scaleDegrees> exampleCents(CentsExample example) noexcept;
}
