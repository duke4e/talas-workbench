#include "talas/project/AlternateTunings.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
namespace tunings = talas::project::tunings;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool near(double value, double expected, double tolerance = 1.0e-9)
{
    return std::abs(value - expected) <= tolerance * std::max(1.0, std::abs(expected));
}

tunings::KeyFrequencies frequenciesOf(const tunings::Tuning& tuning)
{
    std::string error;
    const auto frequencies = tunings::keyFrequencies(tuning, error);
    require(frequencies.has_value(), "the tuning should give its key frequencies: " + error);
    return *frequencies;
}

// The tonic key plays the tonic frequency in every tuning;
// by default middle C at its equal-tempered frequency.
void testTheTonicIsAFixedPoint()
{
    tunings::Tuning tuning;
    require(tuning.tonicKey == 60 && near(tuning.tonicFrequency, 440.0 * std::exp2(-9.0 / 12.0)),
        "the default tonic should be middle C at its equal-tempered frequency");
    tuning.tonicKey = 57;
    tuning.tonicFrequency = 432.0;
    tuning.degreesPerOctave = 19;
    tuning.generatingInterval = 1.5;
    tuning.cents = {0, 90, 200, 290, 400, 500, 590, 700, 790, 900, 1000, 1090};
    for (const auto method : {tunings::Method::RatioScale, tunings::Method::CentsScale, tunings::Method::EqualTempered,
             tunings::Method::TwoInterval})
    {
        tuning.method = method;
        require(near(frequenciesOf(tuning)[57], 432.0),
            "the tonic key should play the tonic frequency in the " + std::string{tunings::methodName(method)}
                + " tuning");
    }
}

// The five ways to tune the keys.
void testTheFiveTuningMethods()
{
    tunings::Tuning tuning;
    const auto tonic = tuning.tonicFrequency;
    // Ratio Scale: twelve degrees, each a ratio to the tonic, repeating each
    // octave.
    tuning.method = tunings::Method::RatioScale;
    tuning.ratios[7] = {3, 2};
    tuning.ratios[4] = {5, 4};
    const auto ratios = frequenciesOf(tuning);
    require(near(ratios[67], tonic * 1.5) && near(ratios[79], tonic * 3.0) && near(ratios[55], tonic * 0.75)
            && near(ratios[72], tonic * 2.0) && near(ratios[64], tonic * 5.0 / 4.0),
        "a Ratio Scale should give each degree its ratio to the tonic, an octave apart each octave");
    // Cents Scale: twelve degrees, each so many cents above the tonic.
    tuning.method = tunings::Method::CentsScale;
    tuning.cents[4] = 386.3137;
    const auto cents = frequenciesOf(tuning);
    require(near(cents[64], tonic * std::exp2(386.3137 / 1200.0)) && near(cents[61], tonic * std::exp2(100.0 / 1200.0))
            && near(cents[52], tonic * std::exp2(386.3137 / 1200.0) / 2.0),
        "a Cents Scale should give each degree its cents above the tonic");
    // Equal Tempered: any number of equal steps to the octave, its only
    // parameter.
    tuning.method = tunings::Method::EqualTempered;
    tuning.degreesPerOctave = 19;
    const auto equal = frequenciesOf(tuning);
    require(near(equal[79], tonic * 2.0) && near(equal[61], tonic * std::exp2(1.0 / 19.0))
            && near(equal[41], tonic / 2.0),
        "an equal-tempered scale of 19 degrees should take 19 keys to the octave");
    // Two Interval: the powers of the generating interval, each halved
    // while above the octave, as scale degrees: 3/2 over five
    // degrees is the pentatonic 1, 9/8, 81/64, 3/2, 27/16.
    tuning.method = tunings::Method::TwoInterval;
    tuning.degreesPerOctave = 5;
    tuning.generatingInterval = 1.5;
    const auto two = frequenciesOf(tuning);
    require(near(two[61], tonic * 9.0 / 8.0) && near(two[62], tonic * 81.0 / 64.0) && near(two[63], tonic * 1.5)
            && near(two[64], tonic * 27.0 / 16.0) && near(two[65], tonic * 2.0) && near(two[59], tonic * 27.0 / 32.0),
        "a two-interval scale should raise its generating interval to higher powers within the octave");
    // Each degree's step is the generating interval or that interval over
    // the octave (multiplying by one of two intervals).
    for (int key = 61; key <= 65; ++key)
    {
        const auto step = two[static_cast<std::size_t>(key)] / two[static_cast<std::size_t>(key) - 1];
        require(near(step, 9.0 / 8.0) || near(step, 32.0 / 27.0),
            "a two-interval scale should step by one of two intervals");
    }
    // From Text File: completely arbitrary pitches, each from the file.
    tuning.method = tunings::Method::FromTextFile;
    for (std::size_t key = 0; key < tunings::keyCount; ++key)
        tuning.fileFrequencies[key] = 100.0 + static_cast<double>((key * 37) % 128);
    require(frequenciesOf(tuning) == tuning.fileFrequencies,
        "a tuning from a text file should take every pitch from the file");
    std::string error;
    tuning.method = tunings::Method::RatioScale;
    tuning.ratios[0] = {2, 1};
    require(!tunings::keyFrequencies(tuning, error).has_value(), "the first ratio should be the tonic's, 1/1");
    tuning.method = tunings::Method::TwoInterval;
    tuning.generatingInterval = 2.0;
    require(!tunings::keyFrequencies(tuning, error).has_value(),
        "an octave as the generating interval should repeat its degree and be refused");
}

// The central view lists an octave from the tonic, each
// key's frequency, its ratio to the tonic and its cents above the tonic.
void testTheOctaveView()
{
    tunings::Tuning tuning;
    tuning.method = tunings::Method::EqualTempered;
    tuning.degreesPerOctave = 24;
    const auto octave = tunings::octaveFromTonic(tuning, frequenciesOf(tuning));
    require(octave.size() == 24 && octave.front().key == 60 && near(octave.front().ratio, 1.0)
            && octave[1].key == 61 && near(octave[1].cents, 50.0) && near(octave[23].cents, 1150.0)
            && near(octave[12].ratio, std::sqrt(2.0)),
        "the view should list each degree of the octave with its ratio and cents");
}

// A saved tuning file lists all 128 keys'
// frequencies in hertz, ending with key 999; loading it into the method it
// was saved from takes its parameters, into another method parameters that
// approximate it, and From Text File reads its pitches.
void testTuningFiles()
{
    tunings::Tuning tuning;
    tuning.method = tunings::Method::RatioScale;
    tuning.ratios = tunings::exampleRatios(tunings::RatioExample::Just);
    const auto frequencies = frequenciesOf(tuning);
    const auto text = tunings::tuningText(tuning, frequencies);
    require(text.starts_with("0 ") && text.find("\n127 ") != std::string::npos && text.find("\n999\n") != std::string::npos
            && text.find("\n60 261.626 hz\n") != std::string::npos,
        "a tuning file should list the frequencies of the 128 key numbers in hertz, then 999");
    std::string error;
    const auto again = tunings::readTuningText(text, tunings::Method::RatioScale, 60, error);
    require(again.has_value() && again->ratios == tuning.ratios && again->tonicKey == 60,
        "a file loaded into its own method should give back its parameters: " + error);
    const auto asCents = tunings::readTuningText(text, tunings::Method::CentsScale, 60, error);
    require(asCents.has_value() && near(asCents->cents[7], 1200.0 * std::log2(1.5), 1.0e-5)
            && near(asCents->cents[4], 1200.0 * std::log2(1.25), 1.0e-5),
        "a file loaded into another method should approximate its tuning: " + error);
    const auto fromFile = tunings::readTuningText(text, tunings::Method::FromTextFile, 60, error);
    require(fromFile.has_value() && near(frequenciesOf(*fromFile)[67], 261.6255653005986 * 1.5, 1.0e-5),
        "From Text File should take every pitch from the file");
    tunings::Tuning equal;
    equal.degreesPerOctave = 31;
    const auto equalText = tunings::tuningText(equal, frequenciesOf(equal));
    const auto asEqual = tunings::readTuningText(
        equalText.substr(0, equalText.find("999\n") + 4), tunings::Method::EqualTempered, 60, error);
    require(asEqual.has_value() && asEqual->degreesPerOctave == 31,
        "the frequencies alone should approximate the number of equal steps to the octave");
    // A prepared file: a key number and a frequency in hertz a line.
    std::string prepared;
    for (int key = 0; key < 128; ++key)
        prepared += std::to_string(key) + "    " + std::to_string(8.17581 * std::exp2(key / 12.0)) + " hz\n";
    prepared += "999\n";
    const auto read = tunings::readTuningText(prepared, tunings::Method::FromTextFile, 60, error);
    require(read.has_value() && near(frequenciesOf(*read)[69], 8.17581 * std::exp2(69.0 / 12.0), 1.0e-6),
        "a prepared file of key numbers and frequencies should read");
    require(!tunings::readTuningText(prepared.substr(0, prepared.find("999")), tunings::Method::FromTextFile, 60, error)
                 .has_value()
            && !tunings::readTuningText("0 8 hz\n999\n", tunings::Method::FromTextFile, 60, error).has_value(),
        "a file should end with key 999 and list every key");
}
}

// The buttons that preset the
// ratios and cents to example scales.
void testExampleScales()
{
    auto tuning = tunings::Tuning{};
    // At first the ratio scale shows the ratios of an
    // equal-tempered scale.
    require(tuning.ratios == tunings::exampleRatios(tunings::RatioExample::EqualTempered),
        "the Ratio Scale should show equal-tempered ratios at first");
    tuning.method = tunings::Method::RatioScale;
    tuning.ratios = tunings::exampleRatios(tunings::RatioExample::EqualTempered);
    auto frequencies = frequenciesOf(tuning);
    require(near(frequencies[67], tuning.tonicFrequency * std::exp2(7.0 / 12.0), 1.0e-12),
        "the equal-tempered ratios give an equal-tempered scale");
    tuning.ratios = tunings::exampleRatios(tunings::RatioExample::Just);
    frequencies = frequenciesOf(tuning);
    require(near(frequencies[64] / frequencies[60], 5.0 / 4.0) && near(frequencies[67] / frequencies[60], 3.0 / 2.0)
            && near(frequencies[69] / frequencies[60], 5.0 / 3.0),
        "the Just button gives small integer ratios: C E G without beats, a sixth of 5/3");
    tuning.ratios = tunings::exampleRatios(tunings::RatioExample::Pythagorean);
    frequencies = frequenciesOf(tuning);
    require(near(frequencies[64] / frequencies[60], 81.0 / 64.0), "the Pythagorean third is 81/64");
    tuning.method = tunings::Method::CentsScale;
    tuning.cents = tunings::exampleCents(tunings::CentsExample::Meantone);
    frequencies = frequenciesOf(tuning);
    require(near(frequencies[64] / frequencies[60], 5.0 / 4.0, 1.0e-5), "quarter-comma meantone's thirds are pure");
    require(tunings::exampleCents(tunings::CentsExample::WerckmeisterIII)[7] == 696.090
            && tunings::exampleCents(tunings::CentsExample::EqualTempered)[7] == 700.0,
        "Werckmeister III and equal temperament in cents");
}

int main()
{
    try
    {
        testTheTonicIsAFixedPoint();
        testTheFiveTuningMethods();
        testTheOctaveView();
        testTuningFiles();
        testExampleScales();
    }
    catch (const std::exception& error)
    {
        std::cerr << "Alternate tunings tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Alternate tunings tests passed\n";
    return 0;
}
