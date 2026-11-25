#include "talas/cdp/DistortEnvelope.h"
#include "talas/cdp/DistortHarmonic.h"
#include "talas/cdp/DistortMultiply.h"
#include "talas/cdp/DistortOmit.h"
#include "talas/cdp/DistortOverload.h"
#include "talas/cdp/DistortReform.h"
#include "talas/cdp/DistortReverse.h"
#include "talas/cdp/DistortShuffle.h"
#include "talas/cdp/Echo.h"
#include "talas/cdp/FixedFilter.h"
#include "talas/cdp/LohiFilter.h"
#include "talas/cdp/LowerResolution.h"
#include "talas/cdp/Pan.h"
#include "talas/cdp/Phasing.h"
#include "talas/cdp/PhaseVocoder.h"
#include "talas/cdp/Reverb.h"
#include "talas/cdp/RingModulate.h"
#include "talas/cdp/SpectralBlur.h"
#include "talas/cdp/SpectralChorus.h"
#include "talas/cdp/SpectralFocus.h"
#include "talas/cdp/SpectralPitch.h"
#include "talas/cdp/SpectralScatter.h"
#include "talas/cdp/SpectralArpeg.h"
#include "talas/cdp/SpectralBands.h"
#include "talas/cdp/SpectralGlis.h"
#include "talas/cdp/SpectralCombine.h"
#include "talas/cdp/SpectralMorph.h"
#include "talas/cdp/SpectralTransposef.h"
#include "talas/cdp/SpectralFormants.h"
#include "talas/cdp/SpectralBlurBlur.h"
#include "talas/cdp/SpectralFocusPeaks.h"
#include "talas/cdp/SpectralPitchFile.h"
#include "talas/cdp/PitchTrack.h"
#include "talas/cdp/SpectralFreeze.h"
#include "talas/cdp/SpectralWholeSound.h"
#include "talas/cdp/VaryingBank2.h"
#include "talas/cdp/TremoloFine.h"
#include "talas/cdp/WavesetLocal.h"
#include "talas/cdp/RoomReverb.h"
#include "talas/cdp/Quantise.h"
#include "talas/cdp/LoudnessGain.h"
#include "talas/cdp/PhaseInvert.h"
#include "talas/cdp/DcShift.h"
#include "talas/cdp/TwoInput.h"
#include "talas/cdp/FastConvolution.h"
#include "talas/cdp/SilenceGate.h"
#include "talas/cdp/Deglitch.h"
#include "talas/cdp/NoiseCut.h"
#include "talas/cdp/AttackBoost.h"
#include "talas/cdp/SubmixPairs.h"
#include "talas/cdp/EnvelopeWarp.h"
#include "talas/cdp/PitchedDelay.h"
#include "talas/cdp/Bounce.h"
#include "talas/cdp/Rejoin.h"
#include "talas/cdp/Cascade.h"
#include "talas/cdp/Phasor.h"
#include "talas/cdp/Spin.h"
#include "talas/cdp/Brassage.h"
#include "talas/cdp/SpectralSimple.h"
#include "talas/cdp/StadiumEcho.h"
#include "talas/cdp/SweepingFilter.h"
#include "talas/cdp/TapDelay.h"
#include "talas/cdp/Tremolo.h"
#include "talas/cdp/UserBank.h"
#include "talas/cdp/VariableFilter.h"
#include "talas/cdp/VaryingBank.h"
#include "talas/cdp/VaryingDelay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

// The CDP ports (ADR-0018) against the arithmetic of the CDP8 functions they
// come from, and against streaming in blocks of any size.
namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::vector<double> ramp(std::size_t frames)
{
    std::vector<double> samples(frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
        samples[frame] = std::sin(0.037 * static_cast<double>(frame)) * 0.8;
    return samples;
}

// do_lowbit for a 32-bit float file, with lobit_pconsistency's step.
double cdpLowBit(double mean, int bits)
{
    const auto levels = bits == 16 ? 16.0 : std::ldexp(1.0, 16 - bits);
    const auto step = levels * 65536.0;
    auto value = mean * 2147483647.0;
    value /= step;
    value = std::round(value);
    value *= step;
    value /= 2147483647.0;
    return static_cast<float>(std::min(std::max(value, -1.0), 1.0));
}

void testLowerResolutionQuantizesLikeCdp()
{
    const auto input = ramp(512);
    for (const auto bits : {1, 4, 8, 12, 15, 16})
    {
        talas::cdp::LowerResolution lowBit;
        require(lowBit.prepare(bits, 1), "Lower Resolution should prepare");
        std::vector<double> output(input.size());
        require(lowBit.process(input, output), "Lower Resolution should process");
        for (std::size_t frame = 0; frame < input.size(); ++frame)
        {
            require(output[frame] == cdpLowBit(input[frame], bits),
                "Lower Resolution should quantize as CDP at "
                    + std::to_string(bits) + " bits");
        }
    }

    talas::cdp::LowerResolution twelve;
    talas::cdp::LowerResolution sixteen;
    require(twelve.prepare(12, 1) && sixteen.prepare(16, 1),
        "Lower Resolution should prepare 12 and 16 bits");
    std::vector<double> twelveOut(input.size());
    std::vector<double> sixteenOut(input.size());
    require(twelve.process(input, twelveOut) && sixteen.process(input, sixteenOut)
            && twelveOut == sixteenOut,
        "CDP8 quantizes 16 bits in steps of 16 levels, as 12 bits");
}

void testLowerResolutionHoldsEachGroupMean()
{
    const auto input = ramp(64);
    talas::cdp::LowerResolution lowBit;
    require(lowBit.prepare(15, 4) && lowBit.latencyFrames() == 3,
        "a division of 4 should lag by 3 frames");
    std::vector<double> output(input.size());
    require(lowBit.process(input, output), "Lower Resolution should process groups");
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        if (frame < 3)
        {
            require(output[frame] == 0.0, "no group is complete before frame 3");
            continue;
        }
        const auto group = (frame + 1) / 4 - 1;
        auto sum = 0.0;
        for (std::size_t member = 0; member < 4; ++member)
            sum += input[group * 4 + member];
        require(output[frame] == cdpLowBit(sum / 4, 15),
            "frame " + std::to_string(frame) + " should hold its group's quantized mean");
    }
}

void testLowerResolutionRejectsWithoutChange()
{
    talas::cdp::LowerResolution lowBit;
    require(!lowBit.prepare(0, 1) && !lowBit.prepare(17, 1)
            && !lowBit.prepare(8, 0) && !lowBit.prepare(8, 257),
        "Lower Resolution should reject values outside CDP's ranges");
    require(lowBit.prepare(8, 3), "Lower Resolution should prepare 8 bits");
    std::array<double, 2> first{0.25, 0.5};
    std::array<double, 2> firstOut{};
    require(lowBit.process(first, firstOut), "a finite block should process");
    const auto before = lowBit;
    std::array<double, 2> bad{0.1, std::numeric_limits<double>::infinity()};
    std::array<double, 2> badOut{7.0, 7.0};
    require(!lowBit.process(bad, badOut) && lowBit == before
            && badOut == std::array<double, 2>{7.0, 7.0},
        "a non-finite block should be rejected without changing state or output");
}

// ring_modulate at a constant frequency: the table index advances by
// frequency * 512 / rate, and each sample reads the table by truncating and
// interpolating.
void testRingModulateReadsCdpTable()
{
    constexpr double rate = 48000.0;
    constexpr double frequency = 440.0;
    const auto input = ramp(1000);
    talas::cdp::RingModulate ring;
    require(ring.prepare(rate), "Ring Modulate should prepare");
    std::vector<double> output(input.size());
    require(ring.process(input, std::nullopt, frequency, output),
        "Ring Modulate should process a constant frequency");

    std::array<double, 513> table{};
    for (std::size_t index = 0; index < 512; ++index)
        table[index] = std::sin(2.0 * 3.141592654 * static_cast<double>(index) / 512.0);
    table[512] = 0.0;
    const auto convertor = 512.0 / rate;
    const auto step = std::pow(10.0, std::log10(frequency)) * convertor;
    auto index = 0.0;
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        const auto whole = static_cast<std::size_t>(index);
        const auto fraction = index - static_cast<double>(whole);
        const auto increment = (table[whole + 1] - table[whole]) * fraction;
        const auto sine = table[whole] + increment;
        require(output[frame] == static_cast<float>(input[frame] * sine),
            "frame " + std::to_string(frame) + " should multiply by CDP's table sine");
        index += step;
        while (index >= 512.0)
            index -= 512.0;
    }
}

void testRingModulateRampsBetweenBlockReadings()
{
    constexpr double rate = 48000.0;
    std::vector<double> ones(384, 1.0);
    std::vector<double> frequency(384, 100.0);
    for (std::size_t frame = 128; frame < frequency.size(); ++frame)
        frequency[frame] = 1000.0;
    talas::cdp::RingModulate ring;
    talas::cdp::RingModulate steady;
    require(ring.prepare(rate) && steady.prepare(rate), "Ring Modulate should prepare");
    std::vector<double> output(ones.size());
    require(ring.process(ones, frequency, 100.0, output),
        "Ring Modulate should follow a connected frequency");

    // The phase advance per frame shows the frequency in use: flat at 100 Hz
    // through the first block, rising geometrically to 1000 Hz through the
    // second, flat at 1000 Hz through the third.
    std::vector<double> reference(ones.size());
    require(steady.process(ones, std::nullopt, 100.0, reference),
        "a steady ring should process");
    for (std::size_t frame = 0; frame < 128; ++frame)
        require(output[frame] == reference[frame], "the first block should stay at its first reading");
    require(output[200] != reference[200], "the second block should ramp to the new reading");
}

void testRingModulateStreamsInAnyBlocks()
{
    constexpr double rate = 44100.0;
    const auto input = ramp(3000);
    std::vector<double> frequency(input.size());
    for (std::size_t frame = 0; frame < frequency.size(); ++frame)
        frequency[frame] = 50.0 + static_cast<double>(frame);
    talas::cdp::RingModulate whole;
    require(whole.prepare(rate), "Ring Modulate should prepare");
    std::vector<double> expected(input.size());
    require(whole.process(input, frequency, 0.0, expected),
        "Ring Modulate should process one block");

    talas::cdp::RingModulate parts;
    require(parts.prepare(rate), "Ring Modulate should prepare again");
    std::vector<double> actual(input.size());
    constexpr std::array sizes{1, 127, 128, 129, 64, 5, 300};
    std::size_t offset = 0;
    for (std::size_t index = 0; offset < input.size(); ++index)
    {
        const auto frames = std::min<std::size_t>(sizes[index % sizes.size()], input.size() - offset);
        require(parts.process(
                    std::span{input}.subspan(offset, frames),
                    std::span<const double>{frequency}.subspan(offset, frames),
                    0.0,
                    std::span{actual}.subspan(offset, frames)),
            "Ring Modulate should process a partition");
        offset += frames;
    }
    require(actual == expected, "Ring Modulate should not depend on block sizes");
}

void testRingModulateRejectsWithoutChange()
{
    talas::cdp::RingModulate ring;
    require(!ring.prepare(0.0) && !ring.prepare(std::numeric_limits<double>::quiet_NaN()),
        "Ring Modulate should reject an invalid rate");
    require(ring.prepare(1000.0), "Ring Modulate should prepare");
    std::array<double, 4> input{0.1, 0.2, 0.3, 0.4};
    std::array<double, 4> output{};
    const auto before = ring;
    require(!ring.process(input, std::nullopt, 0.05, output) && ring == before,
        "a frequency below 0.1 Hz should be rejected without change");
    require(!ring.process(input, std::nullopt, 501.0, output) && ring == before,
        "a frequency above Nyquist should be rejected without change");
    input[2] = std::numeric_limits<double>::quiet_NaN();
    require(!ring.process(input, std::nullopt, 100.0, output) && ring == before,
        "a non-finite input should be rejected without change");
}

struct Reference
{
    std::vector<double> input;
    // One column per output channel.
    std::vector<std::vector<double>> outputs;
};

// Input and output channels of a CDP8 program run, recorded in
// tests/cdp/fixtures.
Reference reference(const std::string& name, std::size_t channels = 1)
{
    std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + name + ".txt");
    require(file.good(), "CDP reference " + name + " should be readable");
    Reference result;
    result.outputs.resize(channels);
    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line.front() == '#')
            continue;
        char* end = nullptr;
        result.input.push_back(std::strtod(line.c_str(), &end));
        for (auto& output : result.outputs)
            output.push_back(std::strtod(end, &end));
    }
    require(!result.input.empty(), "CDP reference " + name + " should hold samples");
    return result;
}

void testPortsMatchCdpPrograms()
{
    for (const auto& [name, bits, division] : {
             std::tuple{std::string{"modify-radical-4-bits-6-division-4"}, 6, 4},
             std::tuple{std::string{"modify-radical-4-bits-16-division-1"}, 16, 1}})
    {
        const auto cdp = reference(name);
        talas::cdp::LowerResolution lowBit;
        require(lowBit.prepare(bits, division), "Lower Resolution should prepare");
        std::vector<double> output(cdp.input.size());
        require(lowBit.process(cdp.input, output), "Lower Resolution should process");
        const auto lag = lowBit.latencyFrames();
        for (std::size_t frame = 0; frame + lag < output.size(); ++frame)
        {
            require(output[frame + lag] == cdp.outputs[0][frame],
                name + " should match CDP8 exactly at frame " + std::to_string(frame));
        }
    }

    const auto cdp = reference("modify-radical-5-440-hz");
    talas::cdp::RingModulate ring;
    require(ring.prepare(48000.0), "Ring Modulate should prepare");
    std::vector<double> output(cdp.input.size());
    require(ring.process(cdp.input, std::nullopt, 440.0, output) && output == cdp.outputs[0],
        "Ring Modulate should match CDP8's modify radical 5 exactly");

    const auto tremolo = reference("envel-tremolo-7.5-hz-depth-0.6-gain-0.9");
    talas::cdp::Tremolo tremoloPort;
    require(tremoloPort.prepare(48000.0), "Tremolo should prepare");
    std::vector<double> tremoloOutput(tremolo.input.size());
    require(tremoloPort.process(tremolo.input, 7.5, 0.6, 0.9, tremoloOutput)
            && tremoloOutput == tremolo.outputs[0],
        "Tremolo should match CDP8's envel tremolo exactly");

    for (const auto& [name, position, prescale] : {
             std::tuple{std::string{"modify-space-1-position-0.3-prescale-0.8"}, 0.3, 0.8},
             std::tuple{std::string{"modify-space-1-position-minus-2.5"}, -2.5, 0.7}})
    {
        const auto pan = reference(name, 2);
        talas::cdp::Pan panPort;
        std::vector<double> left(pan.input.size());
        std::vector<double> right(pan.input.size());
        require(panPort.process(pan.input, position, prescale, left, right)
                && left == pan.outputs[0] && right == pan.outputs[1],
            name + " should match CDP8's modify space exactly");
    }
}

// Three runs of CDP8's tapdelay: mono taps with feedback and Mix, stereo
// taps with negative feedback and a pan beyond the speakers, and a direct
// tap that takes the place of Mix. The port streams them in uneven blocks
// and replays them after reset.
void testTapDelayMatchesCdp()
{
    for (const auto& [name, taps, gain, feedback, mix, stereo] : {
             std::tuple{std::string{"tapdelay-mono-gain-0.4-feedback-0.5-mix-0.3"},
                 std::string{"0.0005 0.9\n0.001 0.5\n0.00125 0.25\n"}, 0.4, 0.5, 0.3, false},
             std::tuple{std::string{"tapdelay-stereo-gain-0.25-feedback-minus-0.3-mix-0"},
                 std::string{"0.0004 0.8 -0.5\n0.0009 0.6 1.5\n0.0011 1 0.25\n"}, 0.25, -0.3, 0.0, true},
             std::tuple{std::string{"tapdelay-direct-tap-gain-0.5-feedback-0-mix-0.6"},
                 std::string{"0 0.7 0.2\n0.0006 0.4 -1\n"}, 0.5, 0.0, 0.6, true}})
    {
        const auto cdp = reference(name, stereo ? 2 : 1);
        const auto list = talas::cdp::readDelayTaps(taps, 48000.0);
        require(list.has_value() && list->stereo == stereo, name + " taps should read");
        talas::cdp::TapDelay delay;
        require(delay.prepare(48000.0, *list, gain, feedback, mix) && delay.stereo() == stereo,
            name + " should prepare");
        const auto play = [&] {
            std::vector<double> left(cdp.input.size());
            std::vector<double> right(cdp.input.size());
            std::size_t offset = 0;
            for (std::size_t block = 1; offset < cdp.input.size(); block = block * 3 + 1)
            {
                const auto count = std::min(block, cdp.input.size() - offset);
                require(delay.process(std::span{cdp.input}.subspan(offset, count),
                            std::span{left}.subspan(offset, count),
                            std::span{right}.subspan(offset, count)),
                    name + " should process");
                offset += count;
            }
            return std::pair{left, right};
        };
        const auto [left, right] = play();
        require(left == cdp.outputs[0] && right == cdp.outputs[stereo ? 1 : 0],
            name + " should match CDP8's tapdelay exactly");
        delay.reset();
        require(play() == std::pair{left, right}, name + " should replay after reset");
    }
}

void testTapDelayReadsTapsAsCdp()
{
    const auto list = talas::cdp::readDelayTaps("; a comment\n\n0.01 0.5\n0.01 0.9\n0.02 1 0.5\n", 48000.0);
    require(list.has_value() && list->taps.size() == 2 && list->stereo && !list->directTap
            && list->taps[0] == talas::cdp::DelayTap{0.01, 0.5, 0.0}
            && list->taps[1] == talas::cdp::DelayTap{0.02, 1.0, 0.5},
        "tapdelay should skip comments and repeated times, and pan only stereo lines");
    const auto direct = talas::cdp::readDelayTaps("0.00001 0.5\n0.01 0.4", 48000.0);
    require(direct.has_value() && direct->directTap && direct->taps[0].time == 0.0,
        "a time under one sample should be the direct tap");
    for (const auto* text : {"0.02 0.5\n0.01 0.5", "0.01 1.5", "-0.01 0.5", "0.01", "", "; only a comment"})
    {
        require(!talas::cdp::readDelayTaps(text, 48000.0).has_value(),
            std::string{"tapdelay should refuse the taps \""} + text + "\"");
    }
}

void testTapDelayRejectsWithoutChange()
{
    const auto list = talas::cdp::readDelayTaps("0.001 0.8\n0.002 0.4", 48000.0);
    require(list.has_value(), "the taps should read");
    talas::cdp::TapDelay delay;
    require(!delay.prepare(48000.0, *list, 0.0, 0.0, 0.5)
            && !delay.prepare(48000.0, *list, 0.25, 1.5, 0.5)
            && !delay.prepare(48000.0, *list, 0.25, 0.0, 1.0)
            && !delay.prepared(),
        "tapdelay should refuse a TapGain of 0, Feedback beyond 1 and a Mix of 1");
    require(delay.prepare(48000.0, *list, 0.25, 0.5, 0.5), "the delay should prepare");
    const auto input = ramp(256);
    std::vector<double> left(256);
    std::vector<double> right(256);
    require(delay.process(std::span{input}.first(128), std::span{left}.first(128),
                std::span{right}.first(128)),
        "the delay should process");
    auto twin = delay;
    std::vector<double> bad(std::span{input}.subspan(128).begin(), std::span{input}.subspan(128).end());
    bad[3] = std::numeric_limits<double>::quiet_NaN();
    require(!delay.process(bad, std::span{left}.subspan(128), std::span{right}.subspan(128)),
        "the delay should refuse a non-finite input");
    std::vector<double> twinLeft(128);
    std::vector<double> twinRight(128);
    require(delay.process(std::span{input}.subspan(128), std::span{left}.subspan(128),
                std::span{right}.subspan(128))
            && twin.process(std::span{input}.subspan(128), twinLeft, twinRight)
            && std::ranges::equal(std::span{left}.subspan(128), twinLeft),
        "a refused block should leave the delay unchanged");
}

// Two runs of CDP8's modify revecho 1, the second with negative feedback
// and an inverted dry path. Their tails are the line played on with a
// silent input; the port streams them in uneven blocks and replays them
// after reset.
void testEchoMatchesCdp()
{
    for (const auto& [name, delay, mix, feedback, prescale, invert] : {
             std::tuple{std::string{"modify-revecho-1-delay-2-ms-mix-0.4-feedback-0.6"}, 2.0, 0.4, 0.6, 0.9, false},
             std::tuple{std::string{"modify-revecho-1-delay-0.7-ms-mix-0.8-feedback-minus-0.5-inverted"},
                 0.7, 0.8, -0.5, 1.0, true}})
    {
        const auto cdp = reference(name);
        talas::cdp::Echo echo;
        require(echo.prepare(48000.0, delay, mix, feedback, prescale, invert), name + " should prepare");
        const auto play = [&] {
            std::vector<double> output(cdp.input.size());
            std::size_t offset = 0;
            for (std::size_t block = 2; offset < cdp.input.size(); block = block * 2 + 3)
            {
                const auto count = std::min(block, cdp.input.size() - offset);
                require(echo.process(std::span{cdp.input}.subspan(offset, count),
                            std::span{output}.subspan(offset, count)),
                    name + " should process");
                offset += count;
            }
            return output;
        };
        const auto output = play();
        require(output == cdp.outputs[0], name + " should match CDP8's modify revecho exactly");
        echo.reset();
        require(play() == output, name + " should replay after reset");
    }
}

void testEchoRejectsWithoutChange()
{
    talas::cdp::Echo echo;
    require(!echo.prepare(48000.0, 0.01, 0.5, 0.0, 1.0, false)
            && !echo.prepare(48000.0, 20.0, 1.5, 0.0, 1.0, false)
            && !echo.prepare(48000.0, 20.0, 0.5, -1.0, 1.0, false)
            && !echo.prepare(48000.0, 20.0, 0.5, 0.0, 2.0, false)
            && !echo.prepare(48000.0, 10001.0, 0.5, 0.0, 1.0, false)
            && !echo.prepared(),
        "revecho should refuse a delay under a sample or over 10 s, Mix beyond 1, Feedback of -1 and a prescale above 1");
    require(echo.prepare(48000.0, 1.0, 0.5, 0.5, 1.0, false), "the echo should prepare");
    const auto input = ramp(128);
    std::vector<double> output(128);
    require(echo.process(std::span{input}.first(64), std::span{output}.first(64)), "the echo should process");
    auto twin = echo;
    auto bad = std::vector<double>(input.begin() + 64, input.end());
    bad[5] = std::numeric_limits<double>::infinity();
    std::vector<double> twinOutput(64);
    require(!echo.process(bad, std::span{output}.subspan(64))
            && echo.process(std::span{input}.subspan(64), std::span{output}.subspan(64))
            && twin.process(std::span{input}.subspan(64), twinOutput)
            && std::ranges::equal(std::span{output}.subspan(64), twinOutput),
        "a refused block should leave the echo unchanged");
}

// Two runs of CDP8's modify revecho 2: a sine sweep after a short wait,
// and CDP's random sweep, which ignores its -s seed, so Seed 1 reproduces it.
void testVaryingDelayMatchesCdp()
{
    talas::cdp::VaryingDelaySettings sine;
    sine.delayMilliseconds = 3.0;
    sine.mix = 0.5;
    sine.feedback = 0.4;
    sine.modulation = 0.6;
    sine.lfoFrequency = 40.0;
    sine.lfoPhase = 0.25;
    sine.lfoDelay = 0.001;
    sine.prescale = 0.8;
    talas::cdp::VaryingDelaySettings random;
    random.delayMilliseconds = 2.0;
    random.mix = 0.7;
    random.feedback = -0.3;
    random.modulation = 0.8;
    random.lfoFrequency = -30.0;
    random.lfoPhase = 0.5;
    for (const auto& [name, settings] : {
             std::pair{std::string{"modify-revecho-2-delay-3-ms-sine-lfo"}, sine},
             std::pair{std::string{"modify-revecho-2-delay-2-ms-random-lfo"}, random}})
    {
        const auto cdp = reference(name);
        talas::cdp::VaryingDelay delay;
        require(delay.prepare(48000.0, settings), name + " should prepare");
        const auto play = [&] {
            std::vector<double> output(cdp.input.size());
            std::size_t offset = 0;
            for (std::size_t block = 5; offset < cdp.input.size(); block = block * 2 + 1)
            {
                const auto count = std::min(block, cdp.input.size() - offset);
                require(delay.process(std::span{cdp.input}.subspan(offset, count),
                            std::span{output}.subspan(offset, count)),
                    name + " should process");
                offset += count;
            }
            return output;
        };
        const auto output = play();
        require(output == cdp.outputs[0], name + " should match CDP8's modify revecho 2 exactly");
        delay.reset();
        require(play() == output, name + " should replay after reset");
    }

    const auto cdp = reference("modify-revecho-2-delay-2-ms-random-lfo");
    random.seed = 2;
    talas::cdp::VaryingDelay seeded;
    std::vector<double> output(cdp.input.size());
    require(seeded.prepare(48000.0, random) && seeded.process(cdp.input, output)
            && output != cdp.outputs[0],
        "another Seed should give another random sweep");
}

void testVaryingDelayRejectsWithoutChange()
{
    talas::cdp::VaryingDelay delay;
    for (const auto& change : std::vector<void (*)(talas::cdp::VaryingDelaySettings&)>{
             [](talas::cdp::VaryingDelaySettings& s) { s.feedback = -1.0; },
             [](talas::cdp::VaryingDelaySettings& s) { s.modulation = 1.5; },
             [](talas::cdp::VaryingDelaySettings& s) { s.lfoFrequency = 51.0; },
             [](talas::cdp::VaryingDelaySettings& s) { s.lfoPhase = -0.1; },
             [](talas::cdp::VaryingDelaySettings& s) { s.seed = 0; },
             [](talas::cdp::VaryingDelaySettings& s) { s.delayMilliseconds = 0.001; }})
    {
        talas::cdp::VaryingDelaySettings settings;
        change(settings);
        require(!delay.prepare(48000.0, settings) && !delay.prepared(),
            "revecho 2 should refuse values outside its ranges");
    }
    talas::cdp::VaryingDelaySettings settings;
    settings.modulation = 0.5;
    settings.lfoFrequency = 3.0;
    require(delay.prepare(48000.0, settings), "the delay should prepare");
    const auto input = ramp(128);
    std::vector<double> output(128);
    require(delay.process(std::span{input}.first(64), std::span{output}.first(64)), "the delay should process");
    auto twin = delay;
    auto bad = std::vector<double>(input.begin() + 64, input.end());
    bad[9] = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> twinOutput(64);
    require(!delay.process(bad, std::span{output}.subspan(64))
            && delay.process(std::span{input}.subspan(64), std::span{output}.subspan(64))
            && twin.process(std::span{input}.subspan(64), twinOutput)
            && std::ranges::equal(std::span{output}.subspan(64), twinOutput),
        "a refused block should leave the delay unchanged");
}

// Two runs of CDP8's reverb at 16 kHz on an input that falls silent: a
// small room, and a large room with a predelay, a low cut, a high cut and
// a lowpass into the reverberator.
void testReverbMatchesCdp()
{
    talas::cdp::ReverbSettings small;
    small.reverbGain = 0.6;
    small.mix = 0.3;
    small.reverbTime = 0.5;
    small.damping = 0.5;
    talas::cdp::ReverbSettings large;
    large.reverbGain = 0.8;
    large.mix = 0.2;
    large.reverbTime = 2.0;
    large.damping = 0.2;
    large.lowpassHz = 3000.0;
    large.predelayMilliseconds = 10.0;
    large.lowCutHz = 200.0;
    large.highCutHz = 4000.0;
    for (const auto& [name, settings] : {
             std::pair{std::string{"reverb-small-room-16k"}, small},
             std::pair{std::string{"reverb-large-room-filtered-16k"}, large}})
    {
        const auto cdp = reference(name, 2);
        talas::cdp::Reverb reverb;
        require(reverb.prepare(16000.0, settings), name + " should prepare");
        const auto play = [&] {
            std::vector<double> left(cdp.input.size());
            std::vector<double> right(cdp.input.size());
            std::size_t offset = 0;
            for (std::size_t block = 3; offset < cdp.input.size(); block = block * 2 + 7)
            {
                const auto count = std::min(block, cdp.input.size() - offset);
                require(reverb.process(std::span{cdp.input}.subspan(offset, count),
                            std::span{left}.subspan(offset, count),
                            std::span{right}.subspan(offset, count)),
                    name + " should process");
                offset += count;
            }
            return std::pair{left, right};
        };
        const auto [left, right] = play();
        require(left == cdp.outputs[0] && right == cdp.outputs[1], name + " should match CDP8's reverb exactly");
        reverb.reset();
        require(play() == std::pair{left, right}, name + " should replay after reset");
    }
}

void testReverbRejectsWithoutChange()
{
    talas::cdp::Reverb reverb;
    for (const auto& change : std::vector<void (*)(talas::cdp::ReverbSettings&)>{
             [](talas::cdp::ReverbSettings& s) { s.reverbGain = 1.5; },
             [](talas::cdp::ReverbSettings& s) { s.mix = 1.0; },
             [](talas::cdp::ReverbSettings& s) { s.reverbTime = 0.0; },
             [](talas::cdp::ReverbSettings& s) { s.damping = -0.1; },
             [](talas::cdp::ReverbSettings& s) { s.lowpassHz = 20000.0; },
             [](talas::cdp::ReverbSettings& s) { s.lowCutHz = 500.0; s.highCutHz = 400.0; }})
    {
        talas::cdp::ReverbSettings settings;
        change(settings);
        require(!reverb.prepare(48000.0, settings) && !reverb.prepared(),
            "reverb should refuse values outside its ranges");
    }
    require(reverb.prepare(48000.0, {}), "the reverb should prepare");
    const auto input = ramp(128);
    std::vector<double> left(128);
    std::vector<double> right(128);
    require(reverb.process(std::span{input}.first(64), std::span{left}.first(64), std::span{right}.first(64)),
        "the reverb should process");
    auto twin = reverb;
    auto bad = std::vector<double>(input.begin() + 64, input.end());
    bad[2] = std::numeric_limits<double>::infinity();
    std::vector<double> twinLeft(64);
    std::vector<double> twinRight(64);
    require(!reverb.process(bad, std::span{left}.subspan(64), std::span{right}.subspan(64))
            && reverb.process(std::span{input}.subspan(64), std::span{left}.subspan(64), std::span{right}.subspan(64))
            && twin.process(std::span{input}.subspan(64), twinLeft, twinRight)
            && std::ranges::equal(std::span{left}.subspan(64), twinLeft),
        "a refused block should leave the reverb unchanged");
}

// CDP8's filter variable in each mode, and a band-pass loud enough to clip
// and lower its gain, the first 1200 frames of each run.
void testVariableFilterMatchesCdp()
{
    using Mode = talas::cdp::VariableFilter::Mode;
    for (const auto& [name, mode, acuity, gain, frequency] : {
             std::tuple{std::string{"filter-variable-1-notch-acuity-0.2-gain-0.8-1200-hz"}, Mode::Notch, 0.2, 0.8, 1200.0},
             std::tuple{std::string{"filter-variable-2-band-pass-acuity-0.2-gain-0.8-1200-hz"}, Mode::BandPass, 0.2, 0.8, 1200.0},
             std::tuple{std::string{"filter-variable-3-low-pass-acuity-0.2-gain-0.8-1200-hz"}, Mode::LowPass, 0.2, 0.8, 1200.0},
             std::tuple{std::string{"filter-variable-4-high-pass-acuity-0.2-gain-0.8-1200-hz"}, Mode::HighPass, 0.2, 0.8, 1200.0},
             std::tuple{std::string{"filter-variable-2-band-pass-clipping-gain-40"}, Mode::BandPass, 0.05, 40.0, 900.0}})
    {
        const auto cdp = reference(name);
        talas::cdp::VariableFilter filter;
        require(filter.prepare(48000.0, mode, acuity, gain, frequency), name + " should prepare");
        const auto play = [&] {
            std::vector<double> output(cdp.input.size());
            std::size_t offset = 0;
            for (std::size_t block = 7; offset < cdp.input.size(); block = block * 2 + 5)
            {
                const auto count = std::min(block, cdp.input.size() - offset);
                require(filter.process(std::span{cdp.input}.subspan(offset, count),
                            std::span{output}.subspan(offset, count)),
                    name + " should process");
                offset += count;
            }
            return output;
        };
        const auto output = play();
        require(output == cdp.outputs[0], name + " should match CDP8's filter variable exactly");
        filter.reset();
        require(play() == output, name + " should replay after reset");
    }
}

// CDP8's filter variable with breakpoint files for its acuity and frequency:
// it reads them at the start of each 128-frame block, a frequency at its
// breakpoint and its Q a block before it.
void testVariableFilterFollowsCdpBreakpoints()
{
    using Mode = talas::cdp::VariableFilter::Mode;
    const auto cdp = reference("filter-variable-2-band-pass-live-acuity-and-frequency");
    talas::cdp::VariableFilter filter;
    require(filter.prepare(48000.0, Mode::BandPass, 0.2, 0.8, 600.0), "the filter should prepare");
    std::vector<double> output(cdp.input.size());
    std::size_t offset = 0;
    const auto playTo = [&](std::size_t frame) {
        while (offset < frame)
        {
            const auto count = std::min<std::size_t>(100, frame - offset);
            require(filter.process(std::span{cdp.input}.subspan(offset, count), std::span{output}.subspan(offset, count)),
                "the filter should process");
            offset += count;
        }
    };
    playTo(512);
    require(filter.setControls(0.05, 600.0), "the filter should take a new acuity");
    playTo(768);
    require(filter.setControls(0.05, 2400.0), "the filter should take a new frequency");
    playTo(cdp.input.size());
    require(output == cdp.outputs[0], "the filter should follow CDP8's breakpoints exactly");
    require(!filter.setControls(0.00001, 600.0) && !filter.setControls(0.05, 30000.0),
        "the filter should refuse an acuity or frequency outside CDP's ranges");
    talas::cdp::VariableFilter unprepared;
    require(!unprepared.setControls(0.05, 600.0), "an unprepared filter should refuse controls");
}

// CDP8's filter sweeping with breakpoint files for its acuity and its low,
// high and sweep frequencies, read at the start of each 128-frame block.
void testSweepingFilterFollowsCdpBreakpoints()
{
    using Mode = talas::cdp::SweepingFilter::Mode;
    const auto cdp = reference("filter-sweeping-2-band-pass-live-acuity-and-frequencies");
    talas::cdp::SweepingFilter filter;
    require(filter.prepare(48000.0, Mode::BandPass, 0.1, 1.0, 300.0, 3000.0, 20.0, 0.25), "the filter should prepare");
    std::vector<double> output(cdp.input.size());
    std::size_t offset = 0;
    const auto playTo = [&](std::size_t frame) {
        while (offset < frame)
        {
            const auto count = std::min<std::size_t>(100, frame - offset);
            require(filter.process(std::span{cdp.input}.subspan(offset, count), std::span{output}.subspan(offset, count)),
                "the filter should process");
            offset += count;
        }
    };
    playTo(512);
    require(filter.setControls(0.03, 300.0, 3000.0, 20.0), "the filter should take a new acuity");
    playTo(768);
    require(filter.setControls(0.03, 500.0, 3000.0, 20.0), "the filter should take a new low frequency");
    playTo(1024);
    require(filter.setControls(0.03, 500.0, 2000.0, 20.0), "the filter should take a new high frequency");
    playTo(1280);
    require(filter.setControls(0.03, 500.0, 2000.0, 60.0), "the filter should take a new sweep frequency");
    playTo(cdp.input.size());
    require(output == cdp.outputs[0], "the filter should follow CDP8's breakpoints exactly");
    require(!filter.setControls(0.03, 500.0, 2000.0, 300.0) && !filter.setControls(0.03, 0.01, 2000.0, 60.0),
        "the filter should refuse values outside CDP's ranges");
}

// CDP8's userbank and varibank with a breakpoint file for their Q, read at
// the start of each 128-frame block as the filters are recomputed.
void testFilterBanksFollowCdpBreakpoints()
{
    {
        const auto cdp = reference("filter-userbank-1-live-q");
        talas::cdp::UserBank bank;
        require(bank.prepare(48000.0, {{440.0, 1.0}, {1200.0, 0.5}, {2600.0, 0.8}}, 20.0, 1.0, false),
            "the userbank should prepare");
        std::vector<double> output(cdp.input.size());
        require(bank.process(std::span{cdp.input}.first(700), std::span{output}.first(700))
                && bank.process(std::span{cdp.input}.subspan(700, 68), std::span{output}.subspan(700, 68))
                && bank.setQ(5.0)
                && bank.process(std::span{cdp.input}.subspan(768), std::span{output}.subspan(768)),
            "the userbank should process");
        require(output == cdp.outputs[0], "the userbank should follow CDP8's Q breakpoints exactly");
        require(!bank.setQ(0.0) && !bank.setQ(20000.0), "the userbank should refuse a Q outside CDP's range");
        bank.reset();
        std::vector<double> again(cdp.input.size());
        require(bank.process(cdp.input, again) && std::equal(again.begin(), again.begin() + 768, output.begin()),
            "a reset userbank should return to its first Q");
    }
    {
        const auto cdp = reference("filter-varibank-1-live-q");
        const auto data = talas::cdp::readVaryingFilterData(
            "0 300 1 1500 0.5\n0.03 500 1 1200 0.7\n", talas::cdp::FilterPitches::Hertz, 48000.0);
        talas::cdp::VaryingBank bank;
        require(data.has_value() && bank.prepare(48000.0, *data, 30.0, 1.0, 1, 0.0, false),
            "the varibank should prepare");
        std::vector<double> output(cdp.input.size());
        require(bank.process(std::span{cdp.input}.first(1024), std::span{output}.first(1024)) && bank.setQ(8.0)
                && bank.process(std::span{cdp.input}.subspan(1024), std::span{output}.subspan(1024)),
            "the varibank should process");
        require(output == cdp.outputs[0], "the varibank should follow CDP8's Q breakpoints exactly");
        require(!bank.setQ(0.0), "the varibank should refuse a Q outside CDP's range");
    }
}

// CDP8's filter phasing with a breakpoint file for its delay: its
// varidelay_allpass reads the delay at each frame, rounded to whole frames.
void testPhasingFollowsCdpBreakpoints()
{
    using Mode = talas::cdp::Phasing::Mode;
    for (const auto& [name, mode, first, second, frame] :
        {std::tuple{std::string{"filter-phasing-1-live-delay"}, Mode::PhaseShift, 2.0, 4.0, std::size_t{800}},
            std::tuple{std::string{"filter-phasing-2-phaser-live-delay"}, Mode::Phaser, 3.0, 1.5, std::size_t{1024}}})
    {
        const auto cdp = reference(name);
        talas::cdp::Phasing phasing;
        require(phasing.prepare(48000.0, mode, 0.6, first, 0.9, true), name + " should prepare");
        std::vector<double> output(cdp.input.size());
        require(phasing.process(std::span{cdp.input}.first(frame), std::span{output}.first(frame))
                && phasing.setDelay(second)
                && phasing.process(std::span{cdp.input}.subspan(frame), std::span{output}.subspan(frame)),
            name + " should process");
        require(output == cdp.outputs[0], name + " should follow CDP8's delay breakpoints exactly");
        require(!phasing.setDelay(20000.0) && !phasing.setDelay(0.001), name + " should refuse delays outside CDP's range");
    }
    talas::cdp::Phasing fixed;
    require(fixed.prepare(48000.0, Mode::PhaseShift, 0.6, 2.0, 0.9) && !fixed.setDelay(3.0),
        "a phasing prepared with a fixed delay should refuse a new one");
}

// CDP8's revecho mode 1 with breakpoint files for Mix and Feedback, which it
// plays as its domultiv, reading them every 128 frames.
void testEchoFollowsCdpBreakpoints()
{
    const auto cdp = reference("modify-revecho-1-live-mix-and-feedback");
    talas::cdp::Echo echo;
    require(echo.prepare(48000.0, 7.0, 0.4, 0.3, 0.8, false, true), "the echo should prepare");
    std::vector<double> output(cdp.input.size());
    require(echo.process(std::span{cdp.input}.first(640), std::span{output}.first(640)) && echo.setControls(0.8, 0.3)
            && echo.process(std::span{cdp.input}.subspan(640, 512), std::span{output}.subspan(640, 512))
            && echo.setControls(0.8, -0.5)
            && echo.process(std::span{cdp.input}.subspan(1152), std::span{output}.subspan(1152)),
        "the echo should process");
    require(output == cdp.outputs[0], "the echo should follow CDP8's breakpoints exactly");
    require(!echo.setControls(1.5, 0.0) && !echo.setControls(0.5, -1.0), "the echo should refuse values outside CDP's ranges");
    talas::cdp::Echo fixed;
    require(fixed.prepare(48000.0, 7.0, 0.4, 0.3, 0.8, false) && !fixed.setControls(0.5, 0.0),
        "an echo prepared with fixed fields should refuse new ones");
}

void testVariableFilterRejectsWithoutChange()
{
    using Mode = talas::cdp::VariableFilter::Mode;
    talas::cdp::VariableFilter filter;
    require(!filter.prepare(48000.0, Mode::LowPass, 0.00001, 1.0, 440.0)
            && !filter.prepare(48000.0, Mode::LowPass, 0.05, 20000.0, 440.0)
            && !filter.prepare(48000.0, Mode::LowPass, 0.05, 1.0, 30000.0)
            && !filter.prepare(48000.0, static_cast<Mode>(7), 0.05, 1.0, 440.0)
            && !filter.prepared(),
        "filter variable should refuse values outside its ranges");
    require(filter.prepare(48000.0, Mode::BandPass, 0.05, 1.0, 440.0), "the filter should prepare");
    const auto input = ramp(128);
    std::vector<double> output(128);
    require(filter.process(std::span{input}.first(64), std::span{output}.first(64)), "the filter should process");
    auto twin = filter;
    auto bad = std::vector<double>(input.begin() + 64, input.end());
    bad[7] = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> twinOutput(64);
    require(!filter.process(bad, std::span{output}.subspan(64))
            && filter.process(std::span{input}.subspan(64), std::span{output}.subspan(64))
            && twin.process(std::span{input}.subspan(64), twinOutput)
            && std::ranges::equal(std::span{output}.subspan(64), twinOutput),
        "a refused block should leave the filter unchanged");
}

// CDP8's filter fixed in each mode, the first 1200 frames of each run.
void testFixedFilterMatchesCdp()
{
    using Mode = talas::cdp::FixedFilter::Mode;
    for (const auto& [name, mode, boost, frequency, bandwidth, prescale] : {
             std::tuple{std::string{"filter-fixed-1-below-boost-9-db-800-hz"}, Mode::BelowFrequency, 9.0, 800.0, 0.0, 1.0},
             std::tuple{std::string{"filter-fixed-2-above-cut-minus-12-db-2000-hz-prescale-0.7"}, Mode::AboveFrequency,
                 -12.0, 2000.0, 0.0, 0.7},
             std::tuple{std::string{"filter-fixed-3-band-boost-4-db-1500-hz-300-hz-wide"}, Mode::Band, 4.0, 1500.0, 300.0, 1.0}})
    {
        const auto cdp = reference(name);
        talas::cdp::FixedFilter filter;
        require(filter.prepare(48000.0, mode, boost, frequency, bandwidth, prescale), name + " should prepare");
        const auto play = [&] {
            std::vector<double> output(cdp.input.size());
            std::size_t offset = 0;
            for (std::size_t block = 9; offset < cdp.input.size(); block = block * 2 + 1)
            {
                const auto count = std::min(block, cdp.input.size() - offset);
                require(filter.process(std::span{cdp.input}.subspan(offset, count),
                            std::span{output}.subspan(offset, count)),
                    name + " should process");
                offset += count;
            }
            return output;
        };
        const auto output = play();
        require(output == cdp.outputs[0], name + " should match CDP8's filter fixed exactly");
        filter.reset();
        require(play() == output, name + " should replay after reset");
    }
}

void testFixedFilterRejectsWithoutChange()
{
    using Mode = talas::cdp::FixedFilter::Mode;
    talas::cdp::FixedFilter filter;
    require(!filter.prepare(48000.0, Mode::Band, 40.0, 1000.0, 100.0, 1.0)
            && !filter.prepare(48000.0, Mode::Band, 3.0, 30000.0, 100.0, 1.0)
            && !filter.prepare(48000.0, Mode::Band, 3.0, 1000.0, 0.0, 1.0)
            && !filter.prepare(48000.0, Mode::BelowFrequency, 3.0, 1000.0, 0.0, 300.0)
            && !filter.prepared(),
        "filter fixed should refuse values outside its ranges");
    require(filter.prepare(48000.0, Mode::BelowFrequency, 3.0, 1000.0, 0.0, 1.0), "the filter should prepare");
    const auto input = ramp(128);
    std::vector<double> output(128);
    require(filter.process(std::span{input}.first(64), std::span{output}.first(64)), "the filter should process");
    auto twin = filter;
    auto bad = std::vector<double>(input.begin() + 64, input.end());
    bad[1] = std::numeric_limits<double>::infinity();
    std::vector<double> twinOutput(64);
    require(!filter.process(bad, std::span{output}.subspan(64))
            && filter.process(std::span{input}.subspan(64), std::span{output}.subspan(64))
            && twin.process(std::span{input}.subspan(64), twinOutput)
            && std::ranges::equal(std::span{output}.subspan(64), twinOutput),
        "a refused block should leave the filter unchanged");
}

// CDP8's filter phasing in each mode, the first 1200 frames of each run.
void testPhasingMatchesCdp()
{
    using Mode = talas::cdp::Phasing::Mode;
    for (const auto& [name, mode, gain, delay, prescale] : {
             std::tuple{std::string{"filter-phasing-1-phase-shift-gain-0.6-delay-1.5-ms"}, Mode::PhaseShift, 0.6, 1.5, 1.0},
             std::tuple{std::string{"filter-phasing-2-phaser-gain-minus-0.4-delay-0.4-ms-prescale-0.8"}, Mode::Phaser,
                 -0.4, 0.4, 0.8}})
    {
        const auto cdp = reference(name);
        talas::cdp::Phasing phasing;
        require(phasing.prepare(48000.0, mode, gain, delay, prescale), name + " should prepare");
        const auto play = [&] {
            std::vector<double> output(cdp.input.size());
            std::size_t offset = 0;
            for (std::size_t block = 9; offset < cdp.input.size(); block = block * 2 + 1)
            {
                const auto count = std::min(block, cdp.input.size() - offset);
                require(phasing.process(std::span{cdp.input}.subspan(offset, count),
                            std::span{output}.subspan(offset, count)),
                    name + " should process");
                offset += count;
            }
            return output;
        };
        const auto output = play();
        require(output == cdp.outputs[0], name + " should match CDP8's filter phasing exactly");
        phasing.reset();
        require(play() == output, name + " should replay after reset");
    }
}

void testPhasingRejectsWithoutChange()
{
    using Mode = talas::cdp::Phasing::Mode;
    talas::cdp::Phasing phasing;
    require(!phasing.prepare(48000.0, Mode::Phaser, 1.5, 10.0, 1.0)
            && !phasing.prepare(48000.0, Mode::Phaser, 0.5, 0.01, 1.0)
            && !phasing.prepare(48000.0, Mode::Phaser, 0.5, 20000.0, 1.0)
            && !phasing.prepare(48000.0, Mode::Phaser, 0.5, 10.0, -1.5)
            && !phasing.prepared(),
        "filter phasing should refuse values outside its ranges");
    require(phasing.prepare(48000.0, Mode::Phaser, 0.5, 1.0, 1.0), "the filter should prepare");
    const auto input = ramp(128);
    std::vector<double> output(128);
    require(phasing.process(std::span{input}.first(64), std::span{output}.first(64)), "the filter should process");
    auto twin = phasing;
    auto bad = std::vector<double>(input.begin() + 64, input.end());
    bad[1] = std::numeric_limits<double>::infinity();
    std::vector<double> twinOutput(64);
    require(!phasing.process(bad, std::span{output}.subspan(64))
            && phasing.process(std::span{input}.subspan(64), std::span{output}.subspan(64))
            && twin.process(std::span{input}.subspan(64), twinOutput)
            && std::ranges::equal(std::span{output}.subspan(64), twinOutput),
        "a refused block should leave the filter unchanged");
    talas::cdp::Phasing cancel;
    require(cancel.prepare(48000.0, Mode::Phaser, 1.0, 1.0, 1.0)
            && cancel.process(input, output)
            && std::ranges::all_of(output, [](double sample) { return sample == 0.0; }),
        "a phaser with a gain of 1 should cancel its input, as CDP's usage says");
}

// CDP8's filter lohi, low-pass and high-pass, at the order its attenuation
// gives, at its limit of 200 sections with its clipping, and at an
// attenuation of 0 dB; the first 1200 frames of each run.
void testLohiFilterMatchesCdp()
{
    for (const auto& [name, attenuation, passBand, stopBand, prescale] : {
             std::tuple{std::string{"filter-lohi-low-pass-minus-60-db-1000-hz-to-1500-hz"}, -60.0, 1000.0, 1500.0, 0.9},
             std::tuple{std::string{"filter-lohi-high-pass-minus-40-db-2000-hz-to-1200-hz"}, -40.0, 2000.0, 1200.0, 1.0},
             std::tuple{std::string{"filter-lohi-low-pass-200-sections-clipping-prescale-3"}, -96.0, 1000.0, 1001.0, 3.0},
             std::tuple{std::string{"filter-lohi-low-pass-0-db-order-2-3000-hz-to-6000-hz"}, 0.0, 3000.0, 6000.0, 1.0}})
    {
        const auto cdp = reference(name);
        talas::cdp::LohiFilter filter;
        require(filter.prepare(48000.0, attenuation, passBand, stopBand, prescale), name + " should prepare");
        const auto play = [&] {
            std::vector<double> output(cdp.input.size());
            std::size_t offset = 0;
            for (std::size_t block = 9; offset < cdp.input.size(); block = block * 2 + 1)
            {
                const auto count = std::min(block, cdp.input.size() - offset);
                require(filter.process(std::span{cdp.input}.subspan(offset, count),
                            std::span{output}.subspan(offset, count)),
                    name + " should process");
                offset += count;
            }
            return output;
        };
        const auto output = play();
        require(output == cdp.outputs[0], name + " should match CDP8's filter lohi exactly");
        filter.reset();
        require(play() == output, name + " should replay after reset");
    }
}

void testLohiFilterRejectsWithoutChange()
{
    talas::cdp::LohiFilter filter;
    require(!filter.prepare(48000.0, -100.0, 1000.0, 1500.0, 1.0)
            && !filter.prepare(48000.0, 6.0, 1000.0, 1500.0, 1.0)
            && !filter.prepare(48000.0, -60.0, 1000.0, 1000.0, 1.0)
            && !filter.prepare(48000.0, -60.0, 1000.0, 30000.0, 1.0)
            && !filter.prepare(48000.0, -60.0, 1000.0, 1500.0, 300.0)
            && !filter.prepared(),
        "filter lohi should refuse values outside its ranges and equal bands");
    require(filter.prepare(48000.0, -60.0, 1000.0, 1500.0, 1.0), "the filter should prepare");
    const auto input = ramp(128);
    std::vector<double> output(128);
    require(filter.process(std::span{input}.first(64), std::span{output}.first(64)), "the filter should process");
    auto twin = filter;
    auto bad = std::vector<double>(input.begin() + 64, input.end());
    bad[1] = std::numeric_limits<double>::infinity();
    std::vector<double> twinOutput(64);
    require(!filter.process(bad, std::span{output}.subspan(64))
            && filter.process(std::span{input}.subspan(64), std::span{output}.subspan(64))
            && twin.process(std::span{input}.subspan(64), twinOutput)
            && std::ranges::equal(std::span{output}.subspan(64), twinOutput),
        "a refused block should leave the filter unchanged");
}

// CDP8's filter sweeping in each mode, from several phases, with a high
// frequency below the low one, a sweep frequency of 0 and clipping; the
// first 1200 frames of each run.
void testSweepingFilterMatchesCdp()
{
    using Mode = talas::cdp::SweepingFilter::Mode;
    for (const auto& [name, mode, acuity, gain, low, high, sweep, phase] : {
             std::tuple{std::string{"filter-sweeping-2-band-pass-acuity-0.1-300-to-3000-hz-at-20-hz"}, Mode::BandPass,
                 0.1, 1.0, 300.0, 3000.0, 20.0, 0.25},
             std::tuple{std::string{"filter-sweeping-3-low-pass-acuity-0.3-200-to-2000-hz-at-7-hz-phase-0.6"}, Mode::LowPass,
                 0.3, 0.8, 200.0, 2000.0, 7.0, 0.6},
             std::tuple{std::string{"filter-sweeping-1-notch-acuity-0.5-high-below-low-at-0-hz"}, Mode::Notch,
                 0.5, 1.0, 4000.0, 500.0, 0.0, 0.0},
             std::tuple{std::string{"filter-sweeping-4-high-pass-clipping-gain-30-at-50-hz"}, Mode::HighPass,
                 0.05, 30.0, 1000.0, 5000.0, 50.0, 0.9}})
    {
        const auto cdp = reference(name);
        talas::cdp::SweepingFilter filter;
        require(filter.prepare(48000.0, mode, acuity, gain, low, high, sweep, phase), name + " should prepare");
        const auto play = [&] {
            std::vector<double> output(cdp.input.size());
            std::size_t offset = 0;
            for (std::size_t block = 9; offset < cdp.input.size(); block = block * 2 + 1)
            {
                const auto count = std::min(block, cdp.input.size() - offset);
                require(filter.process(std::span{cdp.input}.subspan(offset, count),
                            std::span{output}.subspan(offset, count)),
                    name + " should process");
                offset += count;
            }
            return output;
        };
        const auto output = play();
        require(output == cdp.outputs[0], name + " should match CDP8's filter sweeping exactly");
        filter.reset();
        require(play() == output, name + " should replay after reset");
    }
}

void testSweepingFilterRejectsWithoutChange()
{
    using Mode = talas::cdp::SweepingFilter::Mode;
    talas::cdp::SweepingFilter filter;
    require(!filter.prepare(48000.0, Mode::LowPass, 2.0, 1.0, 100.0, 4000.0, 0.5, 0.25)
            && !filter.prepare(48000.0, Mode::LowPass, 0.05, 0.0, 100.0, 4000.0, 0.5, 0.25)
            && !filter.prepare(48000.0, Mode::LowPass, 0.05, 1.0, 100.0, 30000.0, 0.5, 0.25)
            && !filter.prepare(48000.0, Mode::LowPass, 0.05, 1.0, 100.0, 4000.0, 300.0, 0.25)
            && !filter.prepare(48000.0, Mode::LowPass, 0.05, 1.0, 100.0, 4000.0, 0.5, 1.5)
            && !filter.prepared(),
        "filter sweeping should refuse values outside its ranges");
    require(filter.prepare(48000.0, Mode::LowPass, 0.05, 1.0, 100.0, 4000.0, 0.5, 0.25), "the filter should prepare");
    const auto input = ramp(128);
    std::vector<double> output(128);
    require(filter.process(std::span{input}.first(64), std::span{output}.first(64)), "the filter should process");
    auto twin = filter;
    auto bad = std::vector<double>(input.begin() + 64, input.end());
    bad[1] = std::numeric_limits<double>::infinity();
    std::vector<double> twinOutput(64);
    require(!filter.process(bad, std::span{output}.subspan(64))
            && filter.process(std::span{input}.subspan(64), std::span{output}.subspan(64))
            && twin.process(std::span{input}.subspan(64), twinOutput)
            && std::ranges::equal(std::span{output}.subspan(64), twinOutput),
        "a refused block should leave the filter unchanged");
}

// CDP8's filter userbank with bands in Hz (a comment, an empty line and a
// level in dB among them), as MIDI note numbers with double filtering, and
// clipping; the first 1200 frames of each run.
void testUserBankMatchesCdp()
{
    using Pitches = talas::cdp::FilterPitches;
    for (const auto& [name, pitches, bands, q, gain, doubled] : {
             std::tuple{std::string{"filter-userbank-1-three-bands-q-20"}, Pitches::Hertz,
                 std::string{"; three bands\n300 1\n  750 -6dB\n\n1800 0.5\n"}, 20.0, 1.0, false},
             std::tuple{std::string{"filter-userbank-2-midi-double-q-8-gain-0.8"}, Pitches::Midi,
                 std::string{"60 1\n67 0.7\n72.5 -3dB\n"}, 8.0, 0.8, true},
             std::tuple{std::string{"filter-userbank-1-clipping-q-50-gain-40"}, Pitches::Hertz,
                 std::string{"440 1\n880 1\n"}, 50.0, 40.0, false}})
    {
        const auto cdp = reference(name);
        const auto list = talas::cdp::readFilterBands(bands, pitches, 48000.0);
        require(list.has_value(), name + " should read its bands");
        talas::cdp::UserBank bank;
        require(bank.prepare(48000.0, *list, q, gain, doubled), name + " should prepare");
        const auto play = [&] {
            std::vector<double> output(cdp.input.size());
            std::size_t offset = 0;
            for (std::size_t block = 9; offset < cdp.input.size(); block = block * 2 + 1)
            {
                const auto count = std::min(block, cdp.input.size() - offset);
                require(bank.process(std::span{cdp.input}.subspan(offset, count),
                            std::span{output}.subspan(offset, count)),
                    name + " should process");
                offset += count;
            }
            return output;
        };
        const auto output = play();
        require(output == cdp.outputs[0], name + " should match CDP8's filter userbank exactly");
        bank.reset();
        require(play() == output, name + " should replay after reset");
    }
}

void testUserBankReadsAndRejectsAsCdp()
{
    using Pitches = talas::cdp::FilterPitches;
    const auto read = talas::cdp::readFilterBands("; bands\n  100 0dB\n+200 -20dB\n300 2.5\n", Pitches::Hertz, 48000.0);
    require(read.has_value() && read->size() == 3 && (*read)[0] == talas::cdp::FilterBand{100.0, 1.0}
            && (*read)[1].frequency == 200.0 && (*read)[1].amplitude == 1.0 / 10.0
            && (*read)[2] == talas::cdp::FilterBand{300.0, 2.5},
        "filter data should read as CDP reads it, levels in dB included");
    const auto midi = talas::cdp::readFilterBands("69 1", Pitches::Midi, 48000.0);
    require(midi.has_value() && (*midi)[0].frequency == 440.0, "note 69 should be 440 Hz");
    require(!talas::cdp::readFilterBands("", Pitches::Hertz, 48000.0)
            && !talas::cdp::readFilterBands("; only a comment", Pitches::Hertz, 48000.0)
            && !talas::cdp::readFilterBands("440", Pitches::Hertz, 48000.0)
            && !talas::cdp::readFilterBands("440 1 2", Pitches::Hertz, 48000.0)
            && !talas::cdp::readFilterBands("30000 1", Pitches::Hertz, 48000.0)
            && !talas::cdp::readFilterBands("128 1", Pitches::Midi, 48000.0)
            && !talas::cdp::readFilterBands("440 0.0001", Pitches::Hertz, 48000.0)
            && !talas::cdp::readFilterBands("440 nan", Pitches::Hertz, 48000.0),
        "filter data should refuse missing bands, a wrong count of values and values outside CDP's ranges");
    talas::cdp::UserBank bank;
    require(!bank.prepare(48000.0, *read, 0.0001, 1.0, false)
            && !bank.prepare(48000.0, *read, 20.0, 20000.0, false)
            && !bank.prepare(48000.0, {}, 20.0, 1.0, false)
            && !bank.prepared(),
        "filter userbank should refuse a Q or gain outside its ranges and no bands");
    require(bank.prepare(48000.0, *read, 20.0, 1.0, false), "the bank should prepare");
    const auto input = ramp(128);
    std::vector<double> output(128);
    require(bank.process(std::span{input}.first(64), std::span{output}.first(64)), "the bank should process");
    auto twin = bank;
    auto bad = std::vector<double>(input.begin() + 64, input.end());
    bad[1] = std::numeric_limits<double>::infinity();
    std::vector<double> twinOutput(64);
    require(!bank.process(bad, std::span{output}.subspan(64))
            && bank.process(std::span{input}.subspan(64), std::span{output}.subspan(64))
            && twin.process(std::span{input}.subspan(64), twinOutput)
            && std::ranges::equal(std::span{output}.subspan(64), twinOutput),
        "a refused block should leave the bank unchanged");
}

// CDP8's distort overload with noise, which reads CDP's unseeded rand(),
// and with its waveform at two clip levels; each run whole.
void testDistortOverloadMatchesCdp()
{
    using Mode = talas::cdp::DistortOverload::Mode;
    for (const auto& [name, mode, clipLevel, depth, frequency] : {
             std::tuple{std::string{"distort-overload-1-noise-clip-0.3-depth-0.7"}, Mode::Noise, 0.3, 0.7, 440.0},
             std::tuple{std::string{"distort-overload-2-waveform-clip-0.25-depth-0.5-1000-hz"}, Mode::Waveform,
                 0.25, 0.5, 1000.0},
             std::tuple{std::string{"distort-overload-2-waveform-clip-0.6-depth-1-50-hz"}, Mode::Waveform, 0.6, 1.0, 50.0}})
    {
        const auto cdp = reference(name);
        talas::cdp::DistortOverload overload;
        require(overload.prepare(48000.0, mode, clipLevel, depth, frequency, 1), name + " should prepare");
        const auto play = [&] {
            std::vector<double> output(cdp.input.size());
            std::size_t offset = 0;
            for (std::size_t block = 9; offset < cdp.input.size(); block = block * 2 + 1)
            {
                const auto count = std::min(block, cdp.input.size() - offset);
                require(overload.process(std::span{cdp.input}.subspan(offset, count),
                            std::span{output}.subspan(offset, count)),
                    name + " should process");
                offset += count;
            }
            return output;
        };
        const auto output = play();
        require(output == cdp.outputs[0], name + " should match CDP8's distort overload exactly");
        overload.reset();
        require(play() == output, name + " should replay after reset");
    }
}

void testDistortOverloadRejectsWithoutChange()
{
    using Mode = talas::cdp::DistortOverload::Mode;
    talas::cdp::DistortOverload overload;
    require(!overload.prepare(48000.0, Mode::Noise, 1.5, 0.5, 440.0, 1)
            && !overload.prepare(48000.0, Mode::Noise, 0.5, -0.1, 440.0, 1)
            && !overload.prepare(48000.0, Mode::Waveform, 0.5, 0.5, 30000.0, 1)
            && !overload.prepare(48000.0, Mode::Noise, 0.5, 0.5, 440.0, 0)
            && !overload.prepared(),
        "distort overload should refuse values outside its ranges");
    require(overload.prepare(48000.0, Mode::Noise, 0.01, 0.5, 440.0, 1), "the overload should prepare");
    const auto input = ramp(128);
    std::vector<double> output(128);
    require(overload.process(std::span{input}.first(64), std::span{output}.first(64)), "the overload should process");
    auto twin = overload;
    auto bad = std::vector<double>(input.begin() + 64, input.end());
    bad[1] = std::numeric_limits<double>::infinity();
    std::vector<double> twinOutput(64);
    require(!overload.process(bad, std::span{output}.subspan(64))
            && overload.process(std::span{input}.subspan(64), std::span{output}.subspan(64))
            && twin.process(std::span{input}.subspan(64), twinOutput)
            && std::ranges::equal(std::span{output}.subspan(64), twinOutput),
        "a refused block should leave the overload unchanged");
    talas::cdp::DistortOverload other;
    std::vector<double> otherOutput(128);
    require(other.prepare(48000.0, Mode::Noise, 0.01, 0.5, 440.0, 2) && other.process(input, otherOutput)
            && !std::ranges::equal(otherOutput, output),
        "another seed should give other noise");
}

// CDP8's filter varibank: two bands gliding over three lines; MIDI pitches
// whose data starts after 0, with three harmonics and double filtering; and
// clipping; the first 1200 frames of each run.
void testVaryingBankMatchesCdp()
{
    using Pitches = talas::cdp::FilterPitches;
    for (const auto& [name, pitches, data, q, gain, harmonics, rolloff, doubled] : {
             std::tuple{std::string{"filter-varibank-1-two-bands-gliding-q-20"}, Pitches::Hertz,
                 std::string{"0 300 1 900 0.5\n0.01 400 0.8 700 -6dB\n0.02 350 1 800 0.3\n"}, 20.0, 1.0, 1, -6.0,
                 false},
             std::tuple{std::string{"filter-varibank-2-midi-late-start-3-harmonics-double"}, Pitches::Midi,
                 std::string{"; starts after 0\n0.005 60 1 67 0.5\n0.015 62 0.7 65 -3dB\n"}, 8.0, 0.8, 3, -4.0,
                 true},
             std::tuple{std::string{"filter-varibank-1-clipping-gain-40"}, Pitches::Hertz,
                 std::string{"0 440 1\n0.012 880 1\n"}, 50.0, 40.0, 1, -6.0, false}})
    {
        const auto cdp = reference(name);
        const auto read = talas::cdp::readVaryingFilterData(data, pitches, 48000.0);
        require(read.has_value(), name + " should read its data");
        talas::cdp::VaryingBank bank;
        require(bank.prepare(48000.0, *read, q, gain, harmonics, rolloff, doubled), name + " should prepare");
        const auto play = [&] {
            std::vector<double> output(cdp.input.size());
            std::size_t offset = 0;
            for (std::size_t block = 9; offset < cdp.input.size(); block = block * 2 + 1)
            {
                const auto count = std::min(block, cdp.input.size() - offset);
                require(bank.process(std::span{cdp.input}.subspan(offset, count),
                            std::span{output}.subspan(offset, count)),
                    name + " should process");
                offset += count;
            }
            return output;
        };
        const auto output = play();
        require(output == cdp.outputs[0], name + " should match CDP8's filter varibank exactly");
        bank.reset();
        require(play() == output, name + " should replay after reset");
    }
}

void testVaryingBankReadsAndRejectsAsCdp()
{
    using Pitches = talas::cdp::FilterPitches;
    const auto read = talas::cdp::readVaryingFilterData("0.5 440 1 880 -6dB\n1 660 0.0001 990 2\n", Pitches::Hertz, 48000.0);
    require(read.has_value() && read->bandCount == 2 && read->times == std::vector<double>{0.0, 0.5, 1.0}
            && read->bands[0] == talas::cdp::FilterBand{440.0, 1.0} && read->bands[2] == read->bands[0]
            && read->bands[4] == talas::cdp::FilterBand{660.0, 0.001},
        "varibank data should copy a first line after 0 to time 0 and raise amplitudes to 0.001");
    require(!talas::cdp::readVaryingFilterData("", Pitches::Hertz, 48000.0)
            && !talas::cdp::readVaryingFilterData("0 440", Pitches::Hertz, 48000.0)
            && !talas::cdp::readVaryingFilterData("0 440 1 880", Pitches::Hertz, 48000.0)
            && !talas::cdp::readVaryingFilterData("0 440 1\n1 440", Pitches::Hertz, 48000.0)
            && !talas::cdp::readVaryingFilterData("0 440 1\n0 440 1", Pitches::Hertz, 48000.0)
            && !talas::cdp::readVaryingFilterData("0 30000 1", Pitches::Hertz, 48000.0)
            && !talas::cdp::readVaryingFilterData("0 128 1", Pitches::Midi, 48000.0),
        "varibank data should refuse unpaired, unequal, unordered or out-of-range lines");
    talas::cdp::VaryingBank bank;
    require(!bank.prepare(48000.0, *read, 0.0001, 1.0, 1, -6.0, false)
            && !bank.prepare(48000.0, *read, 20.0, 1.0, 0, -6.0, false)
            && !bank.prepare(48000.0, *read, 20.0, 1.0, 1, 3.0, false)
            && !bank.prepare(48000.0, *read, 20.0, 1.0, 40, -6.0, false)
            && !bank.prepared(),
        "filter varibank should refuse a Q, harmonics or rolloff out of range and harmonics past Nyquist");
    require(bank.prepare(48000.0, *read, 20.0, 1.0, 2, -6.0, false), "the bank should prepare");
    const auto input = ramp(128);
    std::vector<double> output(128);
    require(bank.process(std::span{input}.first(64), std::span{output}.first(64)), "the bank should process");
    auto twin = bank;
    auto bad = std::vector<double>(input.begin() + 64, input.end());
    bad[1] = std::numeric_limits<double>::infinity();
    std::vector<double> twinOutput(64);
    require(!bank.process(bad, std::span{output}.subspan(64))
            && bank.process(std::span{input}.subspan(64), std::span{output}.subspan(64))
            && twin.process(std::span{input}.subspan(64), twinOutput)
            && std::ranges::equal(std::span{output}.subspan(64), twinOutput),
        "a refused block should leave the bank unchanged");
}

// Three runs of CDP8's revecho stadium echo: its 23 echoes at a small size,
// 40 with a rolloff, and all 1000. The last two cut CDP's memory to 10 kB,
// so that the echoes cross its 853-frame input buffers, whose order CDP adds
// them in. The port streams them in uneven blocks and replays them after
// reset.
void testStadiumEchoMatchesCdp()
{
    for (const auto& [name, gain, rolloff, size, count, bufferFrames] : {
             std::tuple{std::string{"modify-revecho-3-stadium-size-0.004"}, 0.645654, 1.0, 0.004, 23,
                 talas::cdp::StadiumEcho::defaultBufferFrames},
             std::tuple{std::string{"modify-revecho-3-stadium-40-echoes-rolloff-0.5-small-buffer"},
                 0.9, 0.5, 0.002, 40, std::size_t{853}},
             std::tuple{std::string{"modify-revecho-3-stadium-1000-echoes-small-buffer"},
                 1.0, 1.0, 0.0001, 1000, std::size_t{853}}})
    {
        const auto cdp = reference(name, 2);
        talas::cdp::StadiumEcho echo;
        require(echo.prepare(48000.0, gain, rolloff, size, count, bufferFrames),
            name + " should prepare");
        const auto play = [&] {
            std::vector<double> left(cdp.input.size());
            std::vector<double> right(cdp.input.size());
            std::size_t offset = 0;
            for (std::size_t block = 1; offset < cdp.input.size(); block = block * 3 + 1)
            {
                const auto frames = std::min(block, cdp.input.size() - offset);
                require(echo.process(std::span{cdp.input}.subspan(offset, frames),
                            std::span{left}.subspan(offset, frames),
                            std::span{right}.subspan(offset, frames)),
                    name + " should process");
                offset += frames;
            }
            return std::pair{left, right};
        };
        const auto [left, right] = play();
        require(left == cdp.outputs[0] && right == cdp.outputs[1],
            name + " should match CDP8's stadium echo exactly");
        echo.reset();
        require(play() == std::pair{left, right}, name + " should replay after reset");
    }
}

void testStadiumEchoRejectsWithoutChange()
{
    talas::cdp::StadiumEcho echo;
    require(!echo.prepare(48000.0, 0.0, 1.0, 1.0, 23)
            && !echo.prepare(48000.0, 0.5, 1.5, 1.0, 23)
            && !echo.prepare(48000.0, 0.5, 1.0, 0.000001, 23)
            && !echo.prepare(48000.0, 0.5, 1.0, 1.0, 1)
            && !echo.prepare(48000.0, 0.5, 1.0, 1.0, 1001)
            && !echo.prepare(48000.0, 0.5, 1.0, 1.0, 1000)
            && !echo.prepare(48000.0, 0.5, 1.0, 1.0, 23, 0)
            && !echo.prepared(),
        "stadium echo should refuse a gain of 0, a rolloff over 1, a size under 0.2 s over the sample rate, 1 or 1001 echoes, a last echo past 10 s and no buffer");
    require(echo.prepare(48000.0, 0.5, 1.0, 0.002, 23), "the echo should prepare");
    const auto input = ramp(256);
    std::vector<double> left(256);
    std::vector<double> right(256);
    require(echo.process(std::span{input}.first(128), std::span{left}.first(128),
                std::span{right}.first(128)),
        "the echo should process");
    auto twin = echo;
    std::vector<double> bad(std::span{input}.subspan(128).begin(), std::span{input}.subspan(128).end());
    bad[3] = std::numeric_limits<double>::quiet_NaN();
    require(!echo.process(bad, std::span{left}.subspan(128), std::span{right}.subspan(128)),
        "the echo should refuse a non-finite input");
    std::vector<double> twinLeft(128);
    std::vector<double> twinRight(128);
    require(echo.process(std::span{input}.subspan(128), std::span{left}.subspan(128),
                std::span{right}.subspan(128))
            && twin.process(std::span{input}.subspan(128), twinLeft, twinRight)
            && std::ranges::equal(std::span{left}.subspan(128), twinLeft)
            && std::ranges::equal(std::span{right}.subspan(128), twinRight),
        "a refused block should leave the echo unchanged");
}

// CDP8's distort reform in each of its modes, and with CDP's memory cut to
// 32 kB, so that its 4096-frame buffers split silence and restart clicks.
// The port lags latencyFrames and streams in uneven blocks; CDP leaves its
// last half-cycle as it is, which the stream reshapes once it ends.
void testDistortReformMatchesCdp()
{
    using Mode = talas::cdp::DistortReform::Mode;
    for (const auto& [name, mode, exaggeration, bufferFrames, compared] : {
             std::tuple{std::string{"distort-reform-1-fixed-square"}, Mode::FixedSquare, 2.0, std::size_t{0}, std::size_t{1154}},
             std::tuple{std::string{"distort-reform-2-square"}, Mode::Square, 2.0, std::size_t{0}, std::size_t{1154}},
             std::tuple{std::string{"distort-reform-3-fixed-triangle"}, Mode::FixedTriangle, 2.0, std::size_t{0}, std::size_t{1154}},
             std::tuple{std::string{"distort-reform-4-triangle"}, Mode::Triangle, 2.0, std::size_t{0}, std::size_t{1154}},
             std::tuple{std::string{"distort-reform-5-invert"}, Mode::Invert, 2.0, std::size_t{0}, std::size_t{1154}},
             std::tuple{std::string{"distort-reform-6-click"}, Mode::Click, 2.0, std::size_t{0}, std::size_t{1154}},
             std::tuple{std::string{"distort-reform-7-sine"}, Mode::Sine, 2.0, std::size_t{0}, std::size_t{1154}},
             std::tuple{std::string{"distort-reform-8-exaggerate"}, Mode::Exaggerate, 2.0, std::size_t{0}, std::size_t{1154}},
             std::tuple{std::string{"distort-reform-8-exaggerate-0.5"}, Mode::Exaggerate, 0.5, std::size_t{0}, std::size_t{1154}},
             std::tuple{std::string{"distort-reform-1-fixed-square-4096-frame-buffers"}, Mode::FixedSquare, 2.0, std::size_t{4096}, std::size_t{4583}},
             std::tuple{std::string{"distort-reform-6-click-4096-frame-buffers"}, Mode::Click, 2.0, std::size_t{4096}, std::size_t{4583}}})
    {
        const auto cdp = reference(name);
        talas::cdp::DistortReform reform;
        require(reform.prepare(48000.0, mode, exaggeration, bufferFrames), name + " should prepare");
        const auto latency = talas::cdp::DistortReform::latencyFrames;
        std::vector<double> input(cdp.input);
        input.resize(cdp.input.size() + latency, 0.0);
        const auto play = [&] {
            std::vector<double> output(input.size());
            std::size_t offset = 0;
            for (std::size_t block = 1; offset < input.size(); block = block * 3 + 1)
            {
                const auto frames = std::min(block, input.size() - offset);
                require(reform.process(std::span{input}.subspan(offset, frames),
                            std::span{output}.subspan(offset, frames)),
                    name + " should process");
                offset += frames;
            }
            return output;
        };
        const auto output = play();
        require(std::all_of(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(latency),
                    [](double sample) { return sample == 0.0; })
                && std::equal(cdp.outputs[0].begin(),
                    cdp.outputs[0].begin() + static_cast<std::ptrdiff_t>(compared),
                    output.begin() + static_cast<std::ptrdiff_t>(latency)),
            name + " should match CDP8's distort reform exactly, latencyFrames late");
        reform.reset();
        require(play() == output, name + " should replay after reset");
    }
}

// A half-cycle longer than the latency is cut where the stream can wait no
// longer: a rising ramp in Square mode holds each cut piece at its peak.
void testDistortReformCutsLongHalfCycles()
{
    const auto latency = talas::cdp::DistortReform::latencyFrames;
    std::vector<double> input(3 * latency);
    for (std::size_t frame = 0; frame < input.size(); ++frame)
        input[frame] = static_cast<double>(static_cast<float>(0.25 + 0.5 * static_cast<double>(frame) / static_cast<double>(input.size())));
    talas::cdp::DistortReform reform;
    require(reform.prepare(48000.0, talas::cdp::DistortReform::Mode::Square, 2.0), "the reform should prepare");
    std::vector<double> output(input.size());
    require(reform.process(input, output), "the reform should process");
    require(std::all_of(output.begin() + static_cast<std::ptrdiff_t>(latency),
                output.begin() + static_cast<std::ptrdiff_t>(2 * latency),
                [&](double sample) { return sample == input[latency - 1]; }),
        "the first cut piece should hold its last, highest sample");
    require(output[2 * latency] == input[2 * latency - 1],
        "the next piece should hold its own peak");
}

void testDistortReformRejectsWithoutChange()
{
    using Mode = talas::cdp::DistortReform::Mode;
    talas::cdp::DistortReform reform;
    require(!reform.prepare(48000.0, Mode::Exaggerate, 0.0)
            && !reform.prepare(48000.0, Mode::Exaggerate, 41.0)
            && !reform.prepare(48000.0, Mode::Sine, 2.0, 4095)
            && !reform.prepare(0.0, Mode::Sine, 2.0)
            && !reform.prepare(48000.0, static_cast<Mode>(8), 2.0)
            && !reform.prepared(),
        "reform should refuse an exaggeration outside 0.000002 to 40, buffers under twice its latency, no sample rate and an unknown mode");
    require(talas::cdp::DistortReform::defaultBufferFrames(Mode::Sine) == 131072
            && talas::cdp::DistortReform::defaultBufferFrames(Mode::Exaggerate) == 87296,
        "reform should take CDP8's buffers in its default memory");
    require(reform.prepare(48000.0, Mode::Sine, 2.0), "the reform should prepare");
    const auto input = ramp(256);
    std::vector<double> output(256);
    require(reform.process(std::span{input}.first(128), std::span{output}.first(128)),
        "the reform should process");
    auto twin = reform;
    std::vector<double> bad(std::span{input}.subspan(128).begin(), std::span{input}.subspan(128).end());
    bad[3] = std::numeric_limits<double>::quiet_NaN();
    require(!reform.process(bad, std::span{output}.subspan(128)),
        "the reform should refuse a non-finite input");
    std::vector<double> twinOutput(128);
    require(reform.process(std::span{input}.subspan(128), std::span{output}.subspan(128))
            && twin.process(std::span{input}.subspan(128), twinOutput)
            && std::ranges::equal(std::span{output}.subspan(128), twinOutput),
        "a refused block should leave the reform unchanged");
}

// A port that lags latency frames, streamed in uneven blocks with the
// latency's silence after its input.
template <typename Port>
std::vector<double> playLagging(Port& port, std::vector<double> input, std::size_t latency,
    const std::string& name)
{
    input.resize(input.size() + latency, 0.0);
    std::vector<double> output(input.size());
    std::size_t offset = 0;
    for (std::size_t block = 1; offset < input.size(); block = block * 3 + 1)
    {
        const auto frames = std::min(block, input.size() - offset);
        require(port.process(std::span{input}.subspan(offset, frames),
                    std::span{output}.subspan(offset, frames)),
            name + " should process");
        offset += frames;
    }
    require(std::all_of(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(latency),
                [](double sample) { return sample == 0.0; }),
        name + " should start with its latency's silence");
    return std::vector<double>(output.begin() + static_cast<std::ptrdiff_t>(latency), output.end());
}

// The distortions' fields CDP8 takes as breakpoint files, which it reads,
// counts rounded, where each half-cycle or group starts, played in blocks of
// 64 frames that change their fields where the breakpoints step.
template <typename Port>
std::vector<double> playLive(Port& port, std::vector<double> input, std::size_t latency,
    const std::function<bool(std::size_t)>& set, const std::string& name)
{
    input.resize(input.size() + latency, 0.0);
    std::vector<double> output(input.size());
    for (std::size_t offset = 0; offset < input.size(); offset += 64)
    {
        const auto frames = std::min<std::size_t>(64, input.size() - offset);
        require(set(offset) && port.process(std::span{input}.subspan(offset, frames), std::span{output}.subspan(offset, frames)),
            name + " should process");
    }
    return std::vector<double>(output.begin() + static_cast<std::ptrdiff_t>(latency), output.end());
}

void testDistortionsFollowCdpBreakpoints()
{
    // The last group CDP leaves or envelopes as its file ends.
    constexpr std::size_t compared = 1984;
    const auto same = [](const std::vector<double>& played, const std::vector<double>& cdp) {
        return std::equal(cdp.begin(), cdp.begin() + static_cast<std::ptrdiff_t>(compared), played.begin());
    };
    {
        const auto cdp = reference("distort-multiply-live-factor");
        talas::cdp::DistortMultiply port;
        require(port.prepare(2), "multiply should prepare");
        const auto played = playLive(port, cdp.input, talas::cdp::DistortMultiply::latencyFrames,
            [&](std::size_t at) { return port.setFactor(at >= 1024 ? 4.6 : 2.0); }, "multiply");
        require(same(played, cdp.outputs[0]), "multiply should follow CDP8's factor breakpoints, rounded");
        require(!port.setFactor(16.6) && !port.setFactor(1.4), "multiply should refuse factors that round outside 2 to 16");
    }
    {
        const auto cdp = reference("distort-divide-live-factor-interpolated");
        talas::cdp::DistortDivide port;
        require(port.prepare(3, true), "divide should prepare");
        const auto played = playLive(port, cdp.input, talas::cdp::DistortDivide::latencyFrames,
            [&](std::size_t at) { return port.setFactor(at >= 1280 ? 2.0 : 3.0); }, "divide");
        require(same(played, cdp.outputs[0]), "divide should follow CDP8's factor breakpoints");
    }
    {
        const auto cdp = reference("distort-omit-live-a-of-5");
        talas::cdp::DistortOmit port;
        require(port.prepare(1, 5), "omit should prepare");
        const auto played = playLive(port, cdp.input, 0,
            [&](std::size_t at) { return port.setOmit(at >= 896 ? 3.0 : 1.0); }, "omit");
        require(same(played, cdp.outputs[0]), "omit should follow CDP8's Omit breakpoints");
        require(!port.setOmit(5.0) && !port.setOmit(0.4), "omit should refuse an Omit that does not round under OutOf");
    }
    {
        const auto cdp = reference("distort-reverse-live-cycles");
        talas::cdp::DistortReverse port;
        require(port.prepare(1), "reverse should prepare");
        const auto played = playLive(port, cdp.input, talas::cdp::DistortReverse::latencyFrames,
            [&](std::size_t at) { return port.setCycles(at >= 768 ? 4.0 : 1.0); }, "reverse");
        require(same(played, cdp.outputs[0]), "reverse should follow CDP8's cycle breakpoints");
    }
    {
        using Shape = talas::cdp::DistortEnvelope::Shape;
        const auto cdp = reference("distort-envel-1-live-cycles-trough-exponent");
        talas::cdp::DistortEnvelope port;
        require(port.prepare(Shape::Rising, 2, 0.2, 1.5, {}, true, true), "envel should prepare");
        const auto played = playLive(port, cdp.input, talas::cdp::DistortEnvelope::latencyFrames,
            [&](std::size_t at) {
                return port.setControls(at >= 704 ? 3.0 : 2.0, at >= 1088 ? 0.6 : 0.2, at >= 1280 ? 3.0 : 1.5);
            },
            "envel");
        require(same(played, cdp.outputs[0]), "envel should follow CDP8's breakpoints");
        require(!port.setControls(1001.0, 0.2, 1.5) && !port.setControls(2.0, 1.5, 1.5) && !port.setControls(2.0, 0.2, 60.0),
            "envel should refuse values outside CDP's ranges");
    }
    {
        // Shuffle reads its cycles, rounded, where each domain starts. CDP
        // drops its last domain, and the recorded input ends where CDP's
        // output does, so the domain from frame 1635 never ends here.
        const auto cdp = reference("distort-shuffle-live-cycles");
        const auto map = talas::cdp::readDistortShuffle("abc-bca");
        talas::cdp::DistortShuffle port;
        require(map.has_value() && port.prepare(*map, 1), "shuffle should prepare");
        const auto played = playLive(port, cdp.input, talas::cdp::DistortShuffle::latencyFrames,
            [&](std::size_t at) { return port.setCycles(at >= 832 ? 3.0 : 1.0); }, "shuffle");
        require(std::equal(cdp.outputs[0].begin(), cdp.outputs[0].begin() + 1635, played.begin()),
            "shuffle should follow CDP8's cycle breakpoints");
        require(!port.setCycles(0.4) && !port.setCycles(32767.6), "shuffle should refuse cycles that round outside 1 to 32767");
    }
    {
        // Overload reads its depth and frequency every 0.01 s, and the noise
        // no frequency.
        using Mode = talas::cdp::DistortOverload::Mode;
        const auto noise = reference("distort-overload-1-noise-live-depth");
        talas::cdp::DistortOverload port;
        require(port.prepare(48000.0, Mode::Noise, 0.3, 0.7, 440.0, 1), "overload should prepare");
        require(playLive(port, noise.input, 0,
                    [&](std::size_t at) { return port.setControls(at >= 704 ? 0.2 : 0.7, -1.0); }, "overload")
                == noise.outputs[0],
            "overload's noise should follow CDP8's depth breakpoints");
        const auto waveform = reference("distort-overload-2-waveform-live-depth-and-frequency");
        require(port.prepare(48000.0, Mode::Waveform, 0.25, 0.5, 1000.0, 1), "overload should prepare");
        require(playLive(port, waveform.input, 0,
                    [&](std::size_t at) {
                        return port.setControls(at >= 704 ? 1.0 : 0.5, at >= 1152 ? 300.0 : 1000.0);
                    },
                    "overload")
                == waveform.outputs[0],
            "overload's waveform should follow CDP8's depth and frequency breakpoints");
        require(!port.setControls(1.5, 300.0) && !port.setControls(0.5, 0.001) && !port.setControls(0.5, 24001.0),
            "overload should refuse values outside CDP's ranges");
    }
    {
        // Reform reads its exaggeration at each half-cycle's midpoint, here
        // with CDP's buffers small enough that the change falls before the
        // buffer where the file ends.
        using Mode = talas::cdp::DistortReform::Mode;
        const auto cdp = reference("distort-reform-8-live-exaggeration-5376-frame-buffers");
        talas::cdp::DistortReform port;
        require(port.prepare(48000.0, Mode::Exaggerate, 0.5, 5376), "reform should prepare");
        const auto played = playLive(port, cdp.input, talas::cdp::DistortReform::latencyFrames,
            [&](std::size_t at) { return port.setExaggeration(at >= 1024 ? 2.0 : 0.5); }, "reform");
        // CDP leaves its last half-cycle, the frames from 12237, as they are.
        require(std::equal(cdp.outputs[0].begin(), cdp.outputs[0].begin() + 12237, played.begin()),
            "reform should follow CDP8's exaggeration breakpoints");
        talas::cdp::DistortReform square;
        require(square.prepare(48000.0, Mode::Square, 2.0) && !square.setExaggeration(3.0),
            "reform should read no exaggeration outside Exaggerate");
    }
    {
        // Fractal reads its scale, rounded, and loudness where each group
        // starts.
        const auto cdp = reference("distort-fractal-live-scale-and-loudness");
        talas::cdp::DistortFractal port;
        require(port.prepare(48000.0, 2, 0.5, 0.8, true), "fractal should prepare");
        const auto played = playLive(port, cdp.input, talas::cdp::DistortFractal::latencyFrames,
            [&](std::size_t at) { return port.setControls(at >= 768 ? 3.0 : 2.0, at >= 1216 ? 1.5 : 0.5); }, "fractal");
        require(std::equal(cdp.outputs[0].begin(), cdp.outputs[0].begin() + 1952, played.begin()),
            "fractal should follow CDP8's scale and loudness breakpoints");
        require(!port.setControls(1.4, 0.5) && !port.setControls(24000.6, 0.5) && !port.setControls(2.0, 40000.0),
            "fractal should refuse values outside CDP's ranges");
    }
}

// CDP8's distort omit, reverse and envel on recorded references. Omit needs
// no latency; Reverse and Envelope lag latencyFrames, and match until the
// last group, which CDP leaves or envelopes as its file ends.
void testDistortOmitReverseAndEnvelopeMatchCdp()
{
    for (const auto& [name, omit, outOf] : {
             std::tuple{std::string{"distort-omit-1-of-2"}, 1, 2},
             std::tuple{std::string{"distort-omit-3-of-5-falling-start"}, 3, 5}})
    {
        const auto cdp = reference(name);
        talas::cdp::DistortOmit distortOmit;
        require(distortOmit.prepare(omit, outOf), name + " should prepare");
        const auto output = playLagging(distortOmit, cdp.input, 0, name);
        require(std::equal(cdp.outputs[0].begin(), cdp.outputs[0].end(), output.begin()),
            name + " should match CDP8's distort omit exactly");
        distortOmit.reset();
        require(playLagging(distortOmit, cdp.input, 0, name) == output, name + " should replay after reset");
    }
    for (const auto& [name, cycles, compared] : {
             std::tuple{std::string{"distort-reverse-1-cycle"}, 1, std::size_t{1092}},
             std::tuple{std::string{"distort-reverse-3-cycles-falling-start"}, 3, std::size_t{937}}})
    {
        const auto cdp = reference(name);
        talas::cdp::DistortReverse distortReverse;
        require(distortReverse.prepare(cycles), name + " should prepare");
        const auto latency = talas::cdp::DistortReverse::latencyFrames;
        const auto output = playLagging(distortReverse, cdp.input, latency, name);
        require(std::equal(cdp.outputs[0].begin(),
                    cdp.outputs[0].begin() + static_cast<std::ptrdiff_t>(compared), output.begin()),
            name + " should match CDP8's distort reverse exactly, latencyFrames late");
        distortReverse.reset();
        require(playLagging(distortReverse, cdp.input, latency, name) == output,
            name + " should replay after reset");
    }
    using Shape = talas::cdp::DistortEnvelope::Shape;
    const std::string user = "0 0\n0.3 1\n1 0.2\n";
    for (const auto& [name, shape, cycles, trough, exponent, compared] : {
             std::tuple{std::string{"distort-envel-1-rising-exponent-2"}, Shape::Rising, 1, 0.0, 2.0, std::size_t{1092}},
             std::tuple{std::string{"distort-envel-1-rising-3-cycles-trough-0.25-falling-start"}, Shape::Rising, 3, 0.25, 1.0, std::size_t{937}},
             std::tuple{std::string{"distort-envel-1-rising-exponent-1.5-trough-0.4"}, Shape::Rising, 1, 0.4, 1.5, std::size_t{1092}},
             std::tuple{std::string{"distort-envel-2-falling-2-cycles-trough-0.3"}, Shape::Falling, 2, 0.3, 1.0, std::size_t{937}},
             std::tuple{std::string{"distort-envel-2-falling-exponent-3-trough-0.1"}, Shape::Falling, 1, 0.1, 3.0, std::size_t{1092}},
             std::tuple{std::string{"distort-envel-3-troughed-0.2-exponent-0.5"}, Shape::Troughed, 1, 0.2, 0.5, std::size_t{1092}},
             std::tuple{std::string{"distort-envel-3-troughed-0.6-linear-2-cycles"}, Shape::Troughed, 2, 0.6, 1.0, std::size_t{937}},
             std::tuple{std::string{"distort-envel-4-user-2-cycles"}, Shape::User, 2, 0.0, 1.0, std::size_t{937}}})
    {
        const auto cdp = reference(name);
        const auto points = shape == Shape::User ? talas::cdp::readDistortEnvelope(user)
                                                 : std::optional<std::vector<double>>{std::vector<double>{}};
        talas::cdp::DistortEnvelope distortEnvelope;
        require(points.has_value() && distortEnvelope.prepare(shape, cycles, trough, exponent, *points),
            name + " should prepare");
        const auto latency = talas::cdp::DistortEnvelope::latencyFrames;
        const auto output = playLagging(distortEnvelope, cdp.input, latency, name);
        require(std::equal(cdp.outputs[0].begin(),
                    cdp.outputs[0].begin() + static_cast<std::ptrdiff_t>(compared), output.begin()),
            name + " should match CDP8's distort envel exactly, latencyFrames late");
        distortEnvelope.reset();
        require(playLagging(distortEnvelope, cdp.input, latency, name) == output,
            name + " should replay after reset");
    }
}

void testDistortEnvelopeReadsAsCdp()
{
    const auto envelope = talas::cdp::readDistortEnvelope("0 0\n0.3 1\n1 0.2\n");
    require(envelope.has_value()
            && *envelope == std::vector<double>{0.0, 0.0, static_cast<float>(0.3), 1.0, 1.0, 0.2},
        "distort envel should scale its times to end at 1 through CDP's float");
    const auto scaled = talas::cdp::readDistortEnvelope("; a comment\n0 0.5 2 1 ; stops here 3 0\n");
    require(scaled.has_value() && *scaled == std::vector<double>{0.0, 0.5, 1.0, 1.0},
        "distort envel should read pairs across a line and stop a line at its first word that is no number");
    for (const auto* text : {"0.5 0\n1 1", "0 0\n1", "0 1.5", "0 0\n0 1", "", "0 1e-3"})
    {
        require(!talas::cdp::readDistortEnvelope(text).has_value(),
            std::string{"distort envel should refuse the envelope \""} + text + "\"");
    }
}

void testDistortOmitReverseAndEnvelopeRejectWithoutChange()
{
    using Shape = talas::cdp::DistortEnvelope::Shape;
    talas::cdp::DistortOmit distortOmit;
    talas::cdp::DistortReverse distortReverse;
    talas::cdp::DistortEnvelope distortEnvelope;
    require(!distortOmit.prepare(2, 2) && !distortOmit.prepare(0, 2) && !distortOmit.prepare(1, 32770)
            && !distortReverse.prepare(0) && !distortReverse.prepare(32768)
            && !distortEnvelope.prepare(Shape::Rising, 0, 0.0, 1.0)
            && !distortEnvelope.prepare(Shape::Rising, 1, 1.5, 1.0)
            && !distortEnvelope.prepare(Shape::Falling, 1, 0.0, 0.01)
            && !distortEnvelope.prepare(Shape::User, 1, 0.0, 1.0)
            && !distortOmit.prepared() && !distortReverse.prepared() && !distortEnvelope.prepared(),
        "the distortions should refuse Omit not under OutOf, no cycles, a trough over 1, an exponent under 0.02 and a user shape without its envelope");
    require(distortEnvelope.prepare(Shape::Troughed, 2, 0.3, 2.0), "the envelope should prepare");
    const auto input = ramp(256);
    std::vector<double> output(256);
    require(distortEnvelope.process(std::span{input}.first(128), std::span{output}.first(128)),
        "the envelope should process");
    auto twin = distortEnvelope;
    std::vector<double> bad(std::span{input}.subspan(128).begin(), std::span{input}.subspan(128).end());
    bad[3] = std::numeric_limits<double>::quiet_NaN();
    require(!distortEnvelope.process(bad, std::span{output}.subspan(128)),
        "the envelope should refuse a non-finite input");
    std::vector<double> twinOutput(128);
    require(distortEnvelope.process(std::span{input}.subspan(128), std::span{output}.subspan(128))
            && twin.process(std::span{input}.subspan(128), twinOutput)
            && std::ranges::equal(std::span{output}.subspan(128), twinOutput),
        "a refused block should leave the envelope unchanged");
}

// CDP8's distort harmonic and fractal on recorded references, latencyFrames
// late, until the last cycle or group, which CDP leaves as it comes in.
void testDistortHarmonicAndFractalMatchCdp()
{
    for (const auto& [name, harmonics, prescale] : {
             std::tuple{std::string{"distort-harmonic-2-3-5"}, std::string{"2 0.5\n3 0.25\n5 0.1\n"}, 1.0},
             std::tuple{std::string{"distort-harmonic-4-7-prescale-0.5-falling-start"}, std::string{"\n4 1.5\n7 0.3\n"}, 0.5}})
    {
        const auto cdp = reference(name);
        const auto partials = talas::cdp::readDistortHarmonics(harmonics);
        talas::cdp::DistortHarmonic distortHarmonic;
        require(partials.has_value() && distortHarmonic.prepare(*partials, prescale), name + " should prepare");
        const auto latency = talas::cdp::DistortHarmonic::latencyFrames;
        const auto output = playLagging(distortHarmonic, cdp.input, latency, name);
        require(std::equal(cdp.outputs[0].begin(), cdp.outputs[0].begin() + 1092, output.begin()),
            name + " should match CDP8's distort harmonic exactly, latencyFrames late");
        distortHarmonic.reset();
        require(playLagging(distortHarmonic, cdp.input, latency, name) == output,
            name + " should replay after reset");
    }
    for (const auto& [name, scale, loudness, prescale] : {
             std::tuple{std::string{"distort-fractal-scale-2-loudness-0.5"}, 2, 0.5, 1.0},
             std::tuple{std::string{"distort-fractal-scale-3-loudness-1.5-prescale-0.8-falling-start"}, 3, 1.5, 0.8}})
    {
        const auto cdp = reference(name);
        talas::cdp::DistortFractal distortFractal;
        require(distortFractal.prepare(48000.0, scale, loudness, prescale), name + " should prepare");
        const auto latency = talas::cdp::DistortFractal::latencyFrames;
        const auto output = playLagging(distortFractal, cdp.input, latency, name);
        require(std::equal(cdp.outputs[0].begin(), cdp.outputs[0].begin() + 937, output.begin()),
            name + " should match CDP8's distort fractal exactly, latencyFrames late");
        distortFractal.reset();
        require(playLagging(distortFractal, cdp.input, latency, name) == output,
            name + " should replay after reset");
    }
}

void testDistortHarmonicReadsAndRejectsAsCdp()
{
    using Partial = talas::cdp::DistortHarmonicPartial;
    const auto harmonics = talas::cdp::readDistortHarmonics("2 0.5\n\n  \n3.5 1e-1 extra\n");
    require(harmonics.has_value()
            && *harmonics == std::vector<Partial>{{2, 0.5}, {3, 0.5}},
        "distort harmonic should skip blank lines and read each line as sscanf's %d%lf does");
    for (const auto* text : {"1 0.5", "1025 0.5", "2 33", "2 -0.5", "2", "x 1", "", "2 nan"})
    {
        require(!talas::cdp::readDistortHarmonics(text).has_value(),
            std::string{"distort harmonic should refuse the harmonics \""} + text + "\"");
    }
    talas::cdp::DistortHarmonic distortHarmonic;
    talas::cdp::DistortFractal distortFractal;
    const std::vector<Partial> partials{{2, 0.5}};
    require(!distortHarmonic.prepare(partials, 0.0) && !distortHarmonic.prepare(partials, 201.0)
            && !distortHarmonic.prepare({}, 1.0)
            && !distortFractal.prepare(48000.0, 1, 1.0, 1.0)
            && !distortFractal.prepare(48000.0, 24001, 1.0, 1.0)
            && !distortFractal.prepare(48000.0, 2, 0.0, 1.0)
            && !distortFractal.prepare(48000.0, 2, 1.0, 40000.0)
            && !distortHarmonic.prepared() && !distortFractal.prepared(),
        "distort harmonic and fractal should refuse a prescale out of range, no harmonics, a scale under 2 or over half a second and a loudness of 0");
    require(distortFractal.prepare(48000.0, 2, 0.7, 1.0), "the fractal should prepare");
    const auto input = ramp(256);
    std::vector<double> output(256);
    require(distortFractal.process(std::span{input}.first(128), std::span{output}.first(128)),
        "the fractal should process");
    auto twin = distortFractal;
    std::vector<double> bad(std::span{input}.subspan(128).begin(), std::span{input}.subspan(128).end());
    bad[3] = std::numeric_limits<double>::quiet_NaN();
    require(!distortFractal.process(bad, std::span{output}.subspan(128)),
        "the fractal should refuse a non-finite input");
    std::vector<double> twinOutput(128);
    require(distortFractal.process(std::span{input}.subspan(128), std::span{output}.subspan(128))
            && twin.process(std::span{input}.subspan(128), twinOutput)
            && std::ranges::equal(std::span{output}.subspan(128), twinOutput),
        "a refused block should leave the fractal unchanged");
}

// CDP8's distort multiply and divide on recorded references, latencyFrames
// late, as far as CDP writes (it drops its last half-cycle or group); with
// CDP's memory cut to 96 or 64 kB, so that its 4096-frame buffers split a
// silence and restart a half-cycle's scan.
void testDistortMultiplyAndDivideMatchCdp()
{
    for (const auto& [name, factor, interpolate, bufferFrames, written] : {
             std::tuple{std::string{"distort-multiply-2"}, 2, false, std::size_t{0}, std::size_t{1154}},
             std::tuple{std::string{"distort-multiply-3-falling-start"}, 3, false, std::size_t{0}, std::size_t{1154}},
             std::tuple{std::string{"distort-multiply-5-4096-frame-buffers"}, 5, false, std::size_t{4096}, std::size_t{4579}},
             std::tuple{std::string{"distort-divide-2"}, 2, false, std::size_t{0}, std::size_t{1092}},
             std::tuple{std::string{"distort-divide-3-interpolated-falling-start"}, 3, true, std::size_t{0}, std::size_t{1154}},
             std::tuple{std::string{"distort-divide-2-interpolated-4096-frame-buffers"}, 2, true, std::size_t{4096}, std::size_t{4583}}})
    {
        const auto cdp = reference(name);
        const auto divide = name.starts_with("distort-divide");
        talas::cdp::DistortMultiply distortMultiply;
        talas::cdp::DistortDivide distortDivide;
        require(divide
                ? distortDivide.prepare(factor, interpolate,
                    bufferFrames == 0 ? talas::cdp::DistortDivide::defaultBufferFrames : bufferFrames)
                : distortMultiply.prepare(factor,
                    bufferFrames == 0 ? talas::cdp::DistortMultiply::defaultBufferFrames : bufferFrames),
            name + " should prepare");
        const auto play = [&] {
            return divide
                ? playLagging(distortDivide, cdp.input, talas::cdp::DistortDivide::latencyFrames, name)
                : playLagging(distortMultiply, cdp.input, talas::cdp::DistortMultiply::latencyFrames, name);
        };
        const auto output = play();
        require(std::equal(cdp.outputs[0].begin(),
                    cdp.outputs[0].begin() + static_cast<std::ptrdiff_t>(written), output.begin()),
            name + " should match CDP8 exactly, latencyFrames late");
        distortMultiply.reset();
        distortDivide.reset();
        require(play() == output, name + " should replay after reset");
    }
    talas::cdp::DistortMultiply distortMultiply;
    talas::cdp::DistortDivide distortDivide;
    require(!distortMultiply.prepare(1) && !distortMultiply.prepare(17)
            && !distortMultiply.prepare(2, 4095) && !distortDivide.prepare(17, false)
            && !distortMultiply.prepared() && !distortDivide.prepared()
            && talas::cdp::DistortMultiply::defaultBufferFrames == 43520
            && talas::cdp::DistortDivide::defaultBufferFrames == 65536,
        "distort multiply and divide should refuse factors outside 2 to 16 and buffers under twice their latency, and take CDP8's buffers");
}

// CDP8's phase vocoder, `pvoc anal` then `pvoc synth`, latencyFrames late;
// `pvoc anal`'s frames; and `spec gain` and `spec gate` between them, with
// breakpoint files that step between two frames' times.
void testPhaseVocoderMatchesCdp()
{
    for (const auto& [name, points, overlap, latency] : {
             std::tuple{std::string{"pvoc-anal-synth-1024-points-overlap-3"}, 1024, 3, std::size_t{1152}},
             std::tuple{std::string{"pvoc-anal-synth-256-points-overlap-1"}, 256, 1, std::size_t{1152}},
             std::tuple{std::string{"pvoc-anal-synth-512-points-overlap-4"}, 512, 4, std::size_t{288}}})
    {
        const auto cdp = reference(name);
        talas::cdp::PhaseVocoder vocoder;
        require(vocoder.prepare(48000.0, points, overlap) && vocoder.latencyFrames() == latency
                && talas::cdp::PhaseVocoder::latencyFor(points, overlap) == latency,
            name + " should prepare with its latency");
        const auto output = playLagging(vocoder, cdp.input, latency, name);
        require(output == cdp.outputs[0], name + " should match CDP8's pvoc exactly, latencyFrames late");
        vocoder.reset();
        require(playLagging(vocoder, cdp.input, latency, name) == output, name + " should replay after reset");
    }
    {
        // Each analysis frame as `pvoc anal` writes it.
        std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/pvoc-anal-16-points-frames.txt");
        std::vector<std::vector<float>> frames;
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line.front() == '#')
                continue;
            std::istringstream values(line);
            std::vector<float> frame;
            std::string value;
            while (values >> value)
                frame.push_back(static_cast<float>(std::strtod(value.c_str(), nullptr)));
            frames.push_back(std::move(frame));
        }
        const auto source = reference("pvoc-anal-synth-1024-points-overlap-3").input;
        talas::cdp::PhaseVocoder vocoder;
        require(vocoder.prepare(48000.0, 16, 3) && vocoder.frameSize() == 18, "the 16-point vocoder should prepare");
        std::vector<std::vector<float>> analysed;
        std::vector<double> output(source.size());
        require(vocoder.process(source, output,
                    [&](std::span<float> frame) { analysed.emplace_back(frame.begin(), frame.end()); }),
            "the 16-point vocoder should process");
        require(frames.size() == 100 && analysed.size() >= frames.size()
                && std::equal(frames.begin(), frames.end(), analysed.begin()),
            "the phase vocoder should analyse each frame as CDP8's pvoc anal does");
    }
    {
        const auto cdp = reference("spec-gain-live-gain");
        talas::cdp::SpectralGain gain;
        require(gain.prepare(48000.0, 1024, 3, 0.5), "spec gain should prepare");
        const auto played = playLive(gain, cdp.input, gain.latencyFrames(),
            [&](std::size_t at) { return gain.setGain(at >= 1024 ? 2.0 : 0.5); }, "spec gain");
        require(played == cdp.outputs[0], "spec gain should follow CDP8's gain breakpoints at each frame");
        require(!gain.setGain(-0.1) && !gain.setGain(10001.0), "spec gain should refuse gains outside 0 to 10000");
    }
    {
        const auto cdp = reference("spec-gate-live-threshold");
        talas::cdp::SpectralGate gate;
        require(gate.prepare(48000.0, 256, 2, 0.002), "spec gate should prepare");
        const auto played = playLive(gate, cdp.input, gate.latencyFrames(),
            [&](std::size_t at) { return gate.setThreshold(at >= 1344 ? 0.01 : 0.002); }, "spec gate");
        require(played == cdp.outputs[0], "spec gate should follow CDP8's threshold breakpoints at each frame");
        require(!gate.setThreshold(-0.1) && !gate.setThreshold(1.1), "spec gate should refuse thresholds outside 0 to 1");
    }
    talas::cdp::PhaseVocoder refused;
    require(!refused.prepare(48000.0, 1, 3) && !refused.prepare(48000.0, 8193, 3) && !refused.prepare(48000.0, 1024, 0)
            && !refused.prepare(48000.0, 1024, 5) && !refused.prepare(0.0, 1024, 3)
            && !talas::cdp::PhaseVocoder::latencyFor(8194, 3).has_value(),
        "the phase vocoder should refuse points outside 2 to 8192, overlaps outside 1 to 4 and no sample rate");
}

// A sample beyond float's range (1e300 is finite as a double) makes CDP's
// float frames non-finite. The frames it overlaps are lost, but no running
// phase may keep it: before, every later frame was NaN, so a vocoder in a
// realtime Sound, fed one bad sample, failed every callback after its
// latency.
void testPhaseVocoderRecoversFromAnOverflowingSample()
{
    constexpr std::size_t frames = 64 * 128;
    std::vector<double> input(frames);
    std::vector<double> side(frames);
    double phase = 0.0;
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        input[frame] = 0.3 * (2.0 * phase - 1.0);
        phase += 130.81 / 48000.0;
        phase -= std::floor(phase);
        side[frame] = 0.25 * std::sin(2.0 * std::numbers::pi * 441.0 * static_cast<double>(frame) / 48000.0);
    }
    input[0] = 1.0e300;
    const auto recovered = [](const std::vector<double>& output, const std::string& name) {
        // Two windows of 1024 points after the bad sample's frames leave the
        // output, and still sounding.
        const auto tail = std::span<const double>{output}.subspan(4096);
        require(std::ranges::all_of(tail, [](double sample) { return std::isfinite(sample); }),
            name + " should be finite again once the frames the overflowing sample reaches are out");
        require(std::ranges::any_of(tail, [](double sample) { return std::abs(sample) > 0.01; }),
            name + " should still sound after the overflowing sample");
    };
    for (std::size_t block : {std::size_t{128}, frames})
    {
        talas::cdp::PhaseVocoder vocoder;
        require(vocoder.prepare(48000.0, 1024, 3), "the phase vocoder should prepare");
        std::vector<double> output(frames);
        for (std::size_t start = 0; start < frames; start += block)
        {
            require(vocoder.process(std::span<const double>{input}.subspan(start, block),
                        std::span<double>{output}.subspan(start, block)),
                "the phase vocoder should take a finite double");
        }
        recovered(output, "the phase vocoder in blocks of " + std::to_string(block));

        // The demo vocoder's fields.
        talas::cdp::FormantsVocode vocode;
        require(vocode.prepare(48000.0, 1024, 3, false, 6, false, 5.0, 24000.0, 0.4), "formants vocode should prepare");
        for (std::size_t start = 0; start < frames; start += block)
        {
            require(vocode.process(std::span<const double>{input}.subspan(start, block),
                        std::span<const double>{side}.subspan(start, block),
                        std::span<double>{output}.subspan(start, block)),
                "formants vocode should take a finite double");
        }
        recovered(output, "formants vocode in blocks of " + std::to_string(block));
    }
}

// CDP8's focus exag, fold and accu, blur avrg and suppress, and hilite
// trace between `pvoc anal` and `pvoc synth`, with breakpoint files that
// step between two frames' times; each played with its fields still differs.
void testSpectralProcessesMatchCdp()
{
    using Mode = talas::cdp::HiliteTrace::Mode;
    const auto check = [](const std::string& name, auto& port, const std::function<bool(std::size_t)>& set,
                           const std::function<bool(std::size_t)>& held) {
        const auto cdp = reference(name);
        const auto latency = port.latencyFrames();
        const auto played = playLive(port, cdp.input, latency, set, name);
        require(played == cdp.outputs[0], name + " should match CDP8 exactly, latencyFrames late");
        port.reset();
        require(playLive(port, cdp.input, latency, held, name) != cdp.outputs[0],
            name + " should differ with its fields held");
    };
    {
        talas::cdp::FocusExaggerate port;
        require(port.prepare(48000.0, 1024, 3, 2.0), "focus exag should prepare");
        check("focus-exag-live-exaggeration", port,
            [&](std::size_t at) { return port.setExaggeration(at >= 1024 ? 0.5 : 2.0); },
            [&](std::size_t) { return port.setExaggeration(2.0); });
        require(!port.setExaggeration(0.0005) && !port.setExaggeration(1001.0),
            "focus exag should refuse exaggerations outside 0.001 to 1000");
    }
    {
        talas::cdp::FocusFold port;
        require(port.prepare(48000.0, 1024, 3, 500.0, 1500.0, false, true, true), "focus fold should prepare");
        check("focus-fold-live-frequencies", port,
            [&](std::size_t at) { return port.setFrequencies(at >= 1024 ? 350.0 : 500.0, at >= 1344 ? 1100.0 : 1500.0); },
            [&](std::size_t) { return port.setFrequencies(500.0, 1500.0); });
        require(!port.setFrequencies(4.0, 1500.0) && !port.setFrequencies(500.0, 24001.0),
            "focus fold should refuse frequencies outside 5 Hz to half the sample rate");
        talas::cdp::FocusFold body;
        require(body.prepare(48000.0, 512, 3, 400.0, 1200.0, true, false, false), "focus fold -x should prepare");
        const auto cdp = reference("focus-fold-with-body");
        require(playLagging(body, cdp.input, body.latencyFrames(), "focus fold -x") == cdp.outputs[0],
            "focus fold -x should match CDP8 exactly, latencyFrames late");
    }
    {
        talas::cdp::FocusAccumulate port;
        require(port.prepare(48000.0, 1024, 3, 0.5, 0.0), "focus accu should prepare");
        check("focus-accu-live-decay-and-glis", port,
            [&](std::size_t at) { return port.setControls(at >= 1024 ? 0.95 : 0.5, at >= 1344 ? 1.5 : 0.0); },
            [&](std::size_t) { return port.setControls(0.5, 0.0); });
        const auto limit = talas::cdp::FocusAccumulate::maximumGlissando(48000.0, 1024, 3);
        require(!port.setControls(0.0005, 0.0) && !port.setControls(1.1, 0.0) && !port.setControls(0.5, limit * 1.01)
                && port.setControls(0.5, -limit),
            "focus accu should refuse decays outside 0.001 to 1 and glissandi beyond a sixteenth of an octave a frame");
    }
    {
        talas::cdp::BlurAverage port;
        require(port.prepare(48000.0, 1024, 3, 3.0), "blur avrg should prepare");
        check("blur-avrg-live-n", port, [&](std::size_t at) { return port.setN(at >= 1024 ? 10.0 : 3.0); },
            [&](std::size_t) { return port.setN(3.0); });
        require(!port.setN(1.4) && !port.setN(513.6) && port.setN(513.0), "blur avrg should take N from 2 to the 513 channels");
    }
    {
        talas::cdp::BlurSuppress port;
        require(port.prepare(48000.0, 1024, 3, 500.0), "blur suppress should prepare");
        check("blur-suppress-live-n", port, [&](std::size_t at) { return port.setN(at >= 1024 ? 8.0 : 500.0); },
            [&](std::size_t) { return port.setN(500.0); });
        require(!port.setN(0.4) && !port.setN(513.0) && port.setN(512.0),
            "blur suppress should take N from 1 to one less than the 513 channels");
    }
    {
        talas::cdp::HiliteTrace port;
        require(port.prepare(48000.0, 1024, 3, Mode::All, 12.0, 5.0, 24000.0, false, false, false),
            "hilite trace 1 should prepare");
        check("hilite-trace-1-live-n", port,
            [&](std::size_t at) { return port.setControls(at >= 1024 ? 400.0 : 12.0, 5.0, 24000.0); },
            [&](std::size_t) { return port.setControls(12.0, 5.0, 24000.0); });
        require(port.setControls(513.0, 5.0, 24000.0) && !port.setControls(514.0, 5.0, 24000.0),
            "hilite trace 1 should take N up to the 513 channels");
        talas::cdp::HiliteTrace band;
        require(band.prepare(48000.0, 1024, 3, Mode::Between, 6.0, 400.0, 2000.0, false, true, true),
            "hilite trace 4 should prepare");
        check("hilite-trace-4-live-n-and-band", band,
            [&](std::size_t at) { return band.setControls(at >= 1024 ? 40.0 : 6.0, at >= 1344 ? 900.0 : 400.0, 2000.0); },
            [&](std::size_t) { return band.setControls(6.0, 400.0, 2000.0); });
        require(!band.setControls(513.0, 400.0, 2000.0), "hilite trace 4 should take N below the 513 channels");
        talas::cdp::HiliteTrace retain;
        require(retain.prepare(48000.0, 256, 2, Mode::Above, 30.0, 1500.0, 24000.0, true, false, false),
            "hilite trace 2 -r should prepare");
        const auto cdp = reference("hilite-trace-2-retain");
        require(playLagging(retain, cdp.input, retain.latencyFrames(), "hilite trace 2 -r") == cdp.outputs[0],
            "hilite trace 2 -r should match CDP8 exactly, latencyFrames late");
    }
}

// CDP8's hilite filter in six of its twelve modes, hilite pluck and focus
// step between `pvoc anal` and `pvoc synth`.
void testHiliteFilterPluckAndStepMatchCdp()
{
    using Mode = talas::cdp::HiliteFilter::Mode;
    for (const auto& [name, points, overlap, mode, frequency, high, q, gain, liveFrequency, liveHigh, liveQ] : {
             std::tuple{std::string{"hilite-filter-1-live-frequency-and-q"}, 1024, 3, Mode::HighPass, 600.0, 0.0, 200.0,
                 1.0, true, false, true},
             std::tuple{std::string{"hilite-filter-4-lowpass-normalised"}, 512, 3, Mode::LowPassNormalised, 1500.0, 0.0,
                 300.0, 1.0, false, false, false},
             std::tuple{std::string{"hilite-filter-6-lowpass-gain"}, 1024, 3, Mode::LowPassGain, 1000.0, 0.0, 400.0, 2.5,
                 false, false, false},
             std::tuple{std::string{"hilite-filter-8-live-band"}, 1024, 3, Mode::BandPassNormalised, 400.0, 1500.0,
                 150.0, 1.0, true, true, false},
             std::tuple{std::string{"hilite-filter-10-notch-normalised"}, 256, 2, Mode::NotchNormalised, 400.0, 1800.0,
                 100.0, 1.0, false, false, false},
             std::tuple{std::string{"hilite-filter-12-notch-gain-live-q"}, 1024, 3, Mode::NotchGain, 300.0, 2000.0, 80.0,
                 0.5, false, false, true}})
    {
        const auto cdp = reference(name);
        talas::cdp::HiliteFilter port;
        require(port.prepare(48000.0, points, overlap, mode, frequency, high, q, gain, liveFrequency, liveHigh, liveQ),
            name + " should prepare");
        // The breakpoint files step between the frames at 896 and 1024, and
        // at 1280 and 1408.
        const auto played = playLive(port, cdp.input, port.latencyFrames(),
            [&](std::size_t at) {
                if (name == "hilite-filter-1-live-frequency-and-q")
                    return port.setControls(at >= 1024 ? 1200.0 : 600.0, 0.0, at >= 1344 ? 50.0 : 200.0);
                if (name == "hilite-filter-8-live-band")
                    return port.setControls(at >= 1024 ? 700.0 : 400.0, at >= 1344 ? 2500.0 : 1500.0, 150.0);
                if (name == "hilite-filter-12-notch-gain-live-q")
                    return port.setControls(300.0, 2000.0, at >= 1024 ? 400.0 : 80.0);
                return port.setControls(frequency, high, q);
            },
            name);
        require(played == cdp.outputs[0], name + " should match CDP8 exactly, latencyFrames late");
    }
    talas::cdp::HiliteFilter refused;
    require(!refused.prepare(48000.0, 1024, 3, Mode::HighPass, 9.0, 0.0, 20.0, 1.0, false, false, false)
            && !refused.prepare(48000.0, 1024, 3, Mode::BandPass, 400.0, 24001.0, 20.0, 1.0, false, false, false)
            && !refused.prepare(48000.0, 1024, 3, Mode::HighPass, 400.0, 0.0, 0.5, 1.0, false, false, false)
            && !refused.prepare(48000.0, 1024, 3, Mode::LowPassGain, 400.0, 0.0, 20.0, 10001.0, false, false, false),
        "hilite filter should refuse frequencies outside 10 Hz to half the sample rate, a Q under 1 and a gain over 10000");
    {
        const auto cdp = reference("hilite-pluck-live-gain");
        talas::cdp::HilitePluck port;
        require(port.prepare(48000.0, 1024, 3, 2.0), "hilite pluck should prepare");
        require(playLive(port, cdp.input, port.latencyFrames(),
                    [&](std::size_t at) { return port.setGain(at >= 1024 ? 6.0 : 2.0); }, "hilite pluck")
                == cdp.outputs[0],
            "hilite pluck should follow CDP8's gain breakpoints at each frame");
        require(!port.setGain(-0.1) && !port.setGain(1001.0), "hilite pluck should refuse gains outside 0 to 1000");
    }
    for (const auto& [name, points, overlap, step] : {
             std::tuple{std::string{"focus-step-0.02"}, 1024, 3, 0.02},
             std::tuple{std::string{"focus-step-0.005-256-points"}, 256, 2, 0.005}})
    {
        const auto cdp = reference(name);
        talas::cdp::FocusStep port;
        require(port.prepare(48000.0, points, overlap, step), name + " should prepare");
        require(playLagging(port, cdp.input, port.latencyFrames(), name) == cdp.outputs[0],
            name + " should match CDP8 exactly, latencyFrames late");
    }
    talas::cdp::FocusStep shortStep;
    require(!shortStep.prepare(48000.0, 1024, 3, 0.005) && !shortStep.prepare(48000.0, 1024, 3, 3601.0),
        "focus step should refuse steps under two frames or over an hour");
}

// CDP8's strange shift and repitch transpose between `pvoc anal` and `pvoc
// synth`, with breakpoint files that step between two frames' times.
void testStrangeShiftAndRepitchTransposeMatchCdp()
{
    using ShiftMode = talas::cdp::StrangeShift::Mode;
    using Units = talas::cdp::RepitchTranspose::Units;
    {
        talas::cdp::StrangeShift port;
        require(port.prepare(48000.0, 1024, 3, ShiftMode::All, 100.0, 0.0, 0.0), "strange shift 1 should prepare");
        const auto cdp = reference("strange-shift-1-live-shift");
        require(playLive(port, cdp.input, port.latencyFrames(),
                    [&](std::size_t at) { return port.setControls(at >= 1024 ? -150.0 : 100.0, 0.0, 0.0); },
                    "strange shift 1")
                == cdp.outputs[0],
            "strange shift should follow CDP8's shift breakpoints, up and down");
        require(!port.setControls(24001.0, 0.0, 0.0), "strange shift should refuse a shift beyond half the sample rate");
    }
    {
        talas::cdp::StrangeShift port;
        require(port.prepare(48000.0, 512, 3, ShiftMode::Between, 150.0, 500.0, 2000.0), "strange shift 4 should prepare");
        const auto cdp = reference("strange-shift-4-between");
        require(playLagging(port, cdp.input, port.latencyFrames(), "strange shift 4") == cdp.outputs[0],
            "strange shift 4 should match CDP8 exactly, latencyFrames late");
        require(!port.setControls(150.0, 1.0, 2000.0), "strange shift should refuse frequencies under a quarter of a channel");
    }
    {
        talas::cdp::StrangeShift port;
        require(port.prepare(48000.0, 1024, 3, ShiftMode::Below, 200.0, 800.0, 0.0), "strange shift 3 should prepare");
        const auto cdp = reference("strange-shift-3-live-divide");
        require(playLive(port, cdp.input, port.latencyFrames(),
                    [&](std::size_t at) { return port.setControls(200.0, at >= 1344 ? 1500.0 : 800.0, 0.0); },
                    "strange shift 3")
                == cdp.outputs[0],
            "strange shift should follow CDP8's frequency breakpoints");
    }
    for (const auto& [name, points, overlap, units, transposition, live, low, high, liveLow, fuller, changed, changeAt,
             changeLow] :
        {std::tuple{std::string{"repitch-transpose-1-live-ratio"}, 1024, 3, Units::Ratio, 1.5, true, 0.0, 0.0, false,
             false, 0.75, std::size_t{1024}, 0.0},
            std::tuple{std::string{"repitch-transpose-3-live-semitones"}, 1024, 3, Units::Semitones, 5.0, true, 0.0, 0.0,
                false, false, -7.0, std::size_t{1024}, 0.0},
            std::tuple{std::string{"repitch-transpose-3-semitones-fuller"}, 1024, 3, Units::Semitones, 5.0, false, 0.0,
                0.0, false, true, 5.0, std::size_t{0}, 0.0},
            std::tuple{std::string{"repitch-transpose-2-octaves-band"}, 256, 2, Units::Octaves, -0.5, false, 200.0,
                3000.0, false, false, -0.5, std::size_t{0}, 200.0},
            std::tuple{std::string{"repitch-transpose-1-live-low-frequency"}, 1024, 3, Units::Ratio, 1.25, false, 300.0,
                0.0, true, false, 1.25, std::size_t{1024}, 900.0}})
    {
        talas::cdp::RepitchTranspose port;
        require(port.prepare(48000.0, points, overlap, units, transposition, live, low, high, liveLow, false, fuller),
            name + " should prepare");
        const auto cdp = reference(name);
        require(playLive(port, cdp.input, port.latencyFrames(),
                    [&](std::size_t at) {
                        const auto later = at >= changeAt;
                        return port.setControls(later ? changed : transposition, later && liveLow ? changeLow : low, high);
                    },
                    name)
                == cdp.outputs[0],
            name + " should match CDP8 exactly, latencyFrames late");
    }
    talas::cdp::RepitchTranspose refused;
    require(!refused.prepare(48000.0, 1024, 3, Units::Octaves, 8.5, false, 0.0, 0.0, false, false, false)
            && !refused.prepare(48000.0, 1024, 3, Units::Ratio, 0.003, false, 0.0, 0.0, false, false, false)
            && !refused.prepare(48000.0, 1024, 3, Units::Ratio, 2.0, false, 0.0, 24001.0, false, false, false),
        "repitch transpose should refuse transpositions beyond eight octaves and band frequencies beyond half the sample rate");
}

// CDP8's strange waver, pitch transp, blur chorus and blur noise between
// `pvoc anal` and `pvoc synth`, with breakpoint files that step between two
// frames' times.
void testWaverTranspChorusAndNoiseMatchCdp()
{
    const auto matches = [](const std::string& name, auto& port, const std::function<bool(std::size_t)>& set) {
        const auto cdp = reference(name);
        require(playLive(port, cdp.input, port.latencyFrames(), set, name) == cdp.outputs[0],
            name + " should match CDP8 exactly, latencyFrames late");
        port.reset();
        require(playLive(port, cdp.input, port.latencyFrames(), set, name) == cdp.outputs[0],
            name + " should replay after reset");
    };
    {
        using Mode = talas::cdp::StrangeWaver::Mode;
        talas::cdp::StrangeWaver port;
        require(port.prepare(48000.0, 1024, 3, Mode::Standard, 40.0, 1.5, 300.0, 1.0), "strange waver 1 should prepare");
        matches("strange-waver-1-live-vibrato-and-stretch", port,
            [&](std::size_t at) { return port.setControls(at >= 1024 ? 20.0 : 40.0, at >= 1344 ? 2.5 : 1.5); });
        require(!port.setControls(200.0, 1.5) && !port.setControls(20.0, 0.5),
            "strange waver should refuse a vibrato over half the frame rate and a stretch under 1");
        talas::cdp::StrangeWaver specified;
        require(specified.prepare(48000.0, 512, 3, Mode::Specified, 25.0, 2.0, 500.0, 0.5),
            "strange waver 2 should prepare");
        matches("strange-waver-2-exponent", specified, [&](std::size_t) { return specified.setControls(25.0, 2.0); });
    }
    {
        using Mode = talas::cdp::PitchTransp::Mode;
        for (const auto& [name, points, mode, split, first, second, hasDepth, liveSplit, liveFirst, changeAt, newSplit,
                 newFirst, newDepth] :
            {std::tuple{std::string{"pitch-transp-1-live-split"}, 1024, Mode::OctaveUp, 600.0, 0.0, 0.0, false, true,
                 false, std::size_t{1024}, 1200.0, 0.0, 1.0},
                std::tuple{std::string{"pitch-transp-2-octave-down"}, 512, Mode::OctaveDown, 1500.0, 0.0, 0.0, false,
                    false, false, std::size_t{0}, 1500.0, 0.0, 1.0},
                std::tuple{std::string{"pitch-transp-3-octave-up-and-down"}, 1024, Mode::OctaveUpAndDown, 900.0, 0.0, 0.0,
                    false, false, false, std::size_t{0}, 900.0, 0.0, 1.0},
                std::tuple{std::string{"pitch-transp-4-live-semitones"}, 1024, Mode::ShiftUp, 700.0, 3.0, 0.0, false,
                    false, true, std::size_t{1024}, 700.0, -4.0, 1.0},
                std::tuple{std::string{"pitch-transp-5-shift-down"}, 1024, Mode::ShiftDown, 1600.0, 5.0, 0.0, false,
                    false, false, std::size_t{0}, 1600.0, 5.0, 1.0},
                std::tuple{std::string{"pitch-transp-6-live-depth"}, 1024, Mode::ShiftUpAndDown, 800.0, 4.0, -3.0, true,
                    false, false, std::size_t{1024}, 800.0, 4.0, 0.4}})
        {
            talas::cdp::PitchTransp port;
            require(port.prepare(48000.0, points, 3, mode, split, first, second, hasDepth, 1.0, liveSplit, liveFirst,
                        false),
                name + " should prepare");
            matches(name, port, [&](std::size_t at) {
                const auto later = at >= changeAt;
                return port.setControls(later ? newSplit : split, later ? newFirst : first, second,
                    later ? newDepth : 1.0);
            });
        }
        talas::cdp::PitchTransp refused;
        require(!refused.prepare(48000.0, 1024, 3, Mode::ShiftUp, 700.0, 3.0, 0.0, false, 1.0, true, false, false)
                && !refused.prepare(48000.0, 1024, 3, Mode::ShiftUpAndDown, 700.0, 3.0, 2.0, false, 1.0, false, true,
                    false)
                && !refused.prepare(48000.0, 1024, 3, Mode::ShiftUp, 20000.0, 12.0, 0.0, false, 1.0, false, false, false)
                && !refused.prepare(48000.0, 1024, 3, Mode::OctaveUp, 4.0, 0.0, 0.0, false, 1.0, false, false, false),
            "pitch transp should refuse a static transposition with a live split or the other one live, a shift the split cannot take and a split under 5 Hz");
    }
    {
        using Mode = talas::cdp::BlurChorus::Mode;
        for (const auto& [name, points, overlap, mode, amplitude, frequency, liveAmplitude, liveFrequency, changeAt,
                 newAmplitude, changeFrequencyAt, newFrequency] :
            {std::tuple{std::string{"blur-chorus-1-live-aspread"}, 1024, 3, Mode::Amplitude, 1.5, 1.1, false, false,
                 std::size_t{1024}, 6.0, std::size_t{0}, 1.1},
                std::tuple{std::string{"blur-chorus-1-256-points"}, 256, 2, Mode::Amplitude, 3.0, 1.1, false, false,
                    std::size_t{0}, 3.0, std::size_t{0}, 1.1},
                std::tuple{std::string{"blur-chorus-2"}, 1024, 3, Mode::Frequency, 2.0, 1.5, false, false, std::size_t{0},
                    2.0, std::size_t{0}, 1.5},
                std::tuple{std::string{"blur-chorus-3-512-points"}, 512, 3, Mode::FrequencyUp, 2.0, 2.0, false, false,
                    std::size_t{0}, 2.0, std::size_t{0}, 2.0},
                std::tuple{std::string{"blur-chorus-4-live-fspread"}, 1024, 3, Mode::FrequencyDown, 2.0, 1.3, false,
                    false, std::size_t{0}, 2.0, std::size_t{1024}, 2.5},
                std::tuple{std::string{"blur-chorus-5"}, 1024, 3, Mode::AmplitudeAndFrequency, 4.0, 1.5, false, false,
                    std::size_t{0}, 4.0, std::size_t{0}, 1.5},
                std::tuple{std::string{"blur-chorus-6-live-aspread"}, 1024, 3, Mode::AmplitudeAndFrequencyUp, 3.0, 1.2,
                    false, false, std::size_t{1024}, 20.0, std::size_t{0}, 1.2},
                std::tuple{std::string{"blur-chorus-7-live-fspread-512-points"}, 512, 3, Mode::AmplitudeAndFrequencyDown,
                    2.0, 1.5, false, true, std::size_t{0}, 2.0, std::size_t{1344}, 3.0},
                std::tuple{std::string{"blur-chorus-5-live-spreads"}, 1024, 3, Mode::AmplitudeAndFrequency, 2.0, 1.5,
                    true, true, std::size_t{1024}, 8.0, std::size_t{1344}, 3.0},
                std::tuple{std::string{"blur-chorus-7-live-spreads-512-points"}, 512, 3, Mode::AmplitudeAndFrequencyDown,
                    2.0, 1.5, true, true, std::size_t{1024}, 8.0, std::size_t{1344}, 3.0}})
        {
            talas::cdp::BlurChorus port;
            require(port.prepare(48000.0, points, overlap, mode, amplitude, frequency, liveAmplitude, liveFrequency),
                name + " should prepare");
            matches(name, port, [&](std::size_t at) {
                return port.setSpreads(at >= changeAt ? newAmplitude : amplitude,
                    at >= changeFrequencyAt ? newFrequency : frequency);
            });
        }
        talas::cdp::BlurChorus refused;
        require(!refused.prepare(48000.0, 1024, 3, Mode::Amplitude, 0.5, 1.1, false, false)
                && !refused.prepare(48000.0, 1024, 3, Mode::Frequency, 2.0, 5.0, false, false),
            "blur chorus should refuse an amplitude spread under 1 and a frequency spread over 4");
    }
    {
        talas::cdp::BlurNoise port;
        require(port.prepare(48000.0, 1024, 3, 0.3), "blur noise should prepare");
        matches("blur-noise-live-noise", port, [&](std::size_t at) { return port.setNoise(at >= 1024 ? 0.9 : 0.3); });
        talas::cdp::BlurNoise small;
        require(small.prepare(48000.0, 256, 2, 0.5), "blur noise at 256 points should prepare");
        matches("blur-noise-0.5-256-points", small, [&](std::size_t) { return small.setNoise(0.5); });
        require(!small.setNoise(1.1) && !small.setNoise(-0.1), "blur noise should refuse noise outside 0 to 1");
    }
}

// CDP8's stretch spectrum and blur scatter between `pvoc anal` and `pvoc
// synth`, with breakpoint files that step between two frames' times.
void testStretchSpectrumAndScatterMatchCdp()
{
    const auto matches = [](const std::string& name, auto& port, const std::function<bool(std::size_t)>& set) {
        const auto cdp = reference(name);
        require(playLive(port, cdp.input, port.latencyFrames(), set, name) == cdp.outputs[0],
            name + " should match CDP8 exactly, latencyFrames late");
        port.reset();
        require(playLive(port, cdp.input, port.latencyFrames(), set, name) == cdp.outputs[0],
            name + " should replay after reset");
    };
    {
        using Mode = talas::cdp::StretchSpectrum::Mode;
        talas::cdp::StretchSpectrum above;
        require(above.prepare(48000.0, 1024, 3, Mode::Above, 400.0, 1.8, 1.5, false, 1.0, false),
            "stretch spectrum 1 should prepare");
        matches("stretch-spectrum-1", above, [&](std::size_t) { return above.setDepth(1.0); });
        talas::cdp::StretchSpectrum shrink;
        require(shrink.prepare(48000.0, 1024, 3, Mode::Above, 600.0, 0.7, 2.0, false, 1.0, false),
            "stretch spectrum 1 shrinking should prepare");
        matches("stretch-spectrum-1-shrink", shrink, [&](std::size_t) { return shrink.setDepth(1.0); });
        talas::cdp::StretchSpectrum below;
        require(below.prepare(48000.0, 512, 3, Mode::Below, 2000.0, 0.6, 0.7, true, 1.0, true),
            "stretch spectrum 2 should prepare");
        matches("stretch-spectrum-2-live-depth", below,
            [&](std::size_t at) { return below.setDepth(at >= 1024 ? 0.3 : 1.0); });
        require(!below.setDepth(1.5), "stretch spectrum should refuse a depth over 1");
        talas::cdp::StretchSpectrum refused;
        require(!refused.prepare(48000.0, 1024, 3, Mode::Above, 400.0, 1.8, 60.0, false, 1.0, false)
                && !refused.prepare(48000.0, 1024, 3, Mode::Above, 4.0, 1.8, 1.0, false, 1.0, false),
            "stretch spectrum should refuse an exponent over 50 and a divide under 5 Hz");
    }
    {
        const auto width = *talas::cdp::BlurScatter::channelWidthFor(48000.0, 1024);
        for (const auto& [name, points, overlap, keep, block, randomise, normalise, newKeep, newBlock] :
            {std::tuple{std::string{"blur-scatter-1-live-keep"}, 1024, 3, 100.0, width, false, true, 30.0, width},
                std::tuple{std::string{"blur-scatter-2-live-blocksize"}, 1024, 3, 10.0, 117.1875, false, true, 10.0, 900.0},
                std::tuple{std::string{"blur-scatter-3-randomise-live-keep"}, 1024, 3, 30.0, 200.0, true, true, 8.0, 200.0},
                std::tuple{std::string{"blur-scatter-4-no-normalise"}, 1024, 3, 50.0, 300.0, false, false, 50.0, 300.0},
                std::tuple{std::string{"blur-scatter-5-live-keep-and-blocksize-256-points"}, 256, 2, 20.0, 400.0, false,
                    true, 6.0, 1000.0},
                std::tuple{std::string{"blur-scatter-6-randomise-no-normalise-512-points"}, 512, 4, 40.0,
                    *talas::cdp::BlurScatter::channelWidthFor(48000.0, 512), true, false, 40.0,
                    *talas::cdp::BlurScatter::channelWidthFor(48000.0, 512)}})
        {
            talas::cdp::BlurScatter port;
            require(port.prepare(48000.0, points, overlap, keep, block, randomise, normalise), name + " should prepare");
            matches(name, port, [&](std::size_t at) {
                return at >= 1024 ? port.setControls(newKeep, newBlock) : port.setControls(keep, block);
            });
        }
        talas::cdp::BlurScatter refused;
        require(refused.prepare(48000.0, 1024, 3, 10.0, width, false, true) && !refused.setControls(0.4, width)
                && !refused.setControls(10.0, width / 2.0) && !refused.setControls(10.0, 12001.0)
                && !refused.setControls(200.0, 300.0),
            "blur scatter should refuse no keep, a block under a channel or over a quarter of the sample rate, and a keep at the frame's block count");
    }
}

// CDP8's hilite arpeg, greq and band between `pvoc anal` and `pvoc synth`,
// with breakpoint files that step between two frames' times.
void testHiliteArpegGreqAndBandMatchCdp()
{
    const auto matches = [](const std::string& name, auto& port, const std::function<bool(std::size_t)>& set) {
        const auto cdp = reference(name);
        require(playLive(port, cdp.input, port.latencyFrames(), set, name) == cdp.outputs[0],
            name + " should match CDP8 exactly, latencyFrames late");
        port.reset();
        require(playLive(port, cdp.input, port.latencyFrames(), set, name) == cdp.outputs[0],
            name + " should replay after reset");
    };
    {
        using Mode = talas::cdp::HiliteArpeg::Mode;
        using Wave = talas::cdp::HiliteArpeg::Wave;
        // The rate, low and high frequencies, bandwidth, amplification and
        // sustain from a block on.
        struct Change
        {
            std::size_t at;
            std::size_t field;
            double value;
        };
        struct Arpeg
        {
            std::string name;
            int points;
            int overlap;
            Mode mode;
            Wave wave;
            std::array<double, 6> controls;
            double startPhase;
            double decay;
            bool track;
            bool keep;
            bool liveLow;
            bool liveHigh;
            bool liveAmplification;
            bool liveSustain;
            bool unused;
            std::vector<Change> changes;
        };
        const auto narrowest = [](int points, int overlap) {
            return talas::cdp::HiliteArpeg::minimumBandwidth(48000.0, points, overlap);
        };
        for (const auto& c : {
            Arpeg{"hilite-arpeg-1-sine-live-rate", 1024, 3, Mode::On, Wave::Sine, {20.0, 200.0, 2500.0, 300.0, 2.0, 3.0}, 0.0, 1.0, false, false, false, false, false, false,
                true, {Change{1024, 0, 40.0}}},
            Arpeg{"hilite-arpeg-2-downramp-track-keep", 1024, 3, Mode::Boost, Wave::DownRamp, {30.0, 300.0, 2000.0, 400.0, 2.0, 5.0}, 0.0, 1.0, true, true, false, false, false, false,
                true, {}},
            Arpeg{"hilite-arpeg-3-upramp-nonlinear-track", 1024, 3, Mode::BelowBoost, Wave::UpRamp, {25.0, 100.0, 3000.0, 200.0, 3.0, 3.0}, 0.0, 2.0, true, false, false, false, false, false,
                true, {}},
            Arpeg{"hilite-arpeg-4-saw-nonlinear-keep", 1024, 3, Mode::AboveBoost, Wave::Saw, {30.0, 200.0, 2500.0, 300.0, 2.0, 3.0}, 0.95, 0.5, false, true, false, false, false, false,
                true, {}},
            Arpeg{"hilite-arpeg-5-sine-live-amplification", 1024, 3, Mode::Below, Wave::Sine, {20.0, 200.0, 2500.0, narrowest(1024, 3), 1.5, 3.0}, 0.0, 1.0, false, false, false, false, true, false,
                true, {Change{1024, 4, 0.5}}},
            Arpeg{"hilite-arpeg-6-upramp-live-low", 1024, 3, Mode::Above, Wave::UpRamp, {35.0, 300.0, 3000.0, narrowest(1024, 3), 1.5, 3.0}, 0.0, 1.0, false, false, true, false, true, false,
                true, {Change{1024, 1, 150.0}}},
            Arpeg{"hilite-arpeg-7-saw", 1024, 3, Mode::OnceBelow, Wave::Saw, {30.0, 200.0, 2500.0, 300.0, 1.5, 3.0}, 0.0, 1.0, false, false, false, false, true, false,
                true, {}},
            Arpeg{"hilite-arpeg-7-upramp", 1024, 3, Mode::OnceBelow, Wave::UpRamp, {30.0, 200.0, 2500.0, 300.0, 1.5, 3.0}, 0.5, 1.0, false, false, false, false, true, false,
                true, {}},
            Arpeg{"hilite-arpeg-8-downramp", 1024, 3, Mode::OnceAbove, Wave::DownRamp, {30.0, 5.0, 2500.0, 400.0, 2.0, 3.0}, 0.3, 1.0, false, false, false, false, true, false,
                true, {}},
            Arpeg{"hilite-arpeg-8-sine", 1024, 3, Mode::OnceAbove, Wave::Sine, {30.0, 200.0, 2500.0, 300.0, 2.0, 3.0}, 0.7, 1.0, false, false, false, false, true, false,
                true, {}},
            Arpeg{"hilite-arpeg-1-live-sustain-and-amplification", 1024, 3, Mode::On, Wave::Sine, {30.0, 200.0, 2500.0, 300.0, 2.0, 3.0}, 0.0, 1.0, false, false, false, false, true, true,
                true, {Change{1024, 4, 3.0}, Change{1344, 5, 6.0}}},
            Arpeg{"hilite-arpeg-2-live-sustain-fixed-amplification", 1024, 3, Mode::Boost, Wave::Sine, {30.0, 200.0, 2500.0, 300.0, 10.0, 2.0}, 0.0, 1.0, false, false, false, false, false, true,
                true, {Change{1024, 5, 4.0}}},
            Arpeg{"hilite-arpeg-1-live-band-512-points", 512, 3, Mode::On, Wave::Saw, {25.0, 5.0, 2500.0, 400.0, 3.0, 3.0}, 0.0, 3.0, false, false, true, true, false, false,
                true, {Change{1024, 2, 1500.0}, Change{1344, 3, 200.0}}},
            Arpeg{"hilite-arpeg-1-downramp-table-end", 1024, 3, Mode::On, Wave::DownRamp, {0.0, 200.0, 2500.0, 300.0, 2.0, 3.0}, 0.9997, 1.0, false, false, false, false, false, false,
                true, {}},
            Arpeg{"hilite-arpeg-4-downramp-256-points", 256, 2, Mode::AboveBoost, Wave::DownRamp, {30.0, 200.0, 2500.0, 300.0, 2.0, 4.0}, 0.2, 1.0, false, false, false, false, false, false,
                true, {}},
            Arpeg{"hilite-arpeg-2-sine-keep", 1024, 3, Mode::Boost, Wave::Sine, {30.0, 200.0, 2500.0, 300.0, 2.0, 4.0}, 0.0, 1.0, false, true, false, false, false, false,
                true, {}},
            Arpeg{"hilite-arpeg-1-upramp-track", 1024, 3, Mode::On, Wave::UpRamp, {30.0, 200.0, 2500.0, 300.0, 3.0, 4.0}, 0.0, 1.0, true, false, false, false, false, false,
                true, {}},
            Arpeg{"hilite-arpeg-2-saw-nonlinear-track-keep", 1024, 3, Mode::Boost, Wave::Saw, {30.0, 200.0, 2500.0, 300.0, 3.0, 4.0}, 0.0, 2.0, true, true, false, false, false, false,
                true, {}},
            Arpeg{"hilite-arpeg-3-sine", 1024, 3, Mode::BelowBoost, Wave::Sine, {30.0, 200.0, 2500.0, 300.0, 2.0, 3.0}, 0.6, 1.0, false, false, false, false, false, false,
                true, {}},
            Arpeg{"hilite-arpeg-1-inverted-range", 1024, 3, Mode::On, Wave::Sine, {30.0, 2500.0, 200.0, 300.0, 2.0, 3.0}, 0.0, 1.0, false, false, false, false, false, false,
                true, {}},
             })
        {
            talas::cdp::HiliteArpeg port;
            const auto& [rate, low, high, band, amplification, sustain] = c.controls;
            require(port.prepare(48000.0, c.points, c.overlap, c.mode, c.wave, rate, c.startPhase, low, high, band,
                        amplification, c.decay, sustain, c.track, c.keep, c.liveLow, c.liveHigh, c.liveAmplification,
                        c.liveSustain),
                c.name + " should prepare");
            matches(c.name, port, [&](std::size_t at) {
                auto controls = c.controls;
                for (const auto& change : c.changes)
                    if (at >= change.at)
                        controls[change.field] = change.value;
                return port.setControls(controls[0], controls[1], controls[2], controls[3], controls[4], controls[5]);
            });
        }
        talas::cdp::HiliteArpeg refused;
        require(!refused.prepare(48000.0, 1024, 3, Mode::BelowBoost, Wave::DownRamp, 1.0, 0.0, 5.0, 24000.0,
                    narrowest(1024, 3), 10.0, 1.0, 3.0, false, false, false, false, false, false)
                && !refused.prepare(48000.0, 1024, 3, Mode::AboveBoost, Wave::Sine, 1.0, 0.4, 5.0, 24000.0,
                    narrowest(1024, 3), 10.0, 1.0, 3.0, false, false, false, false, false, false)
                && !refused.prepare(48000.0, 1024, 3, Mode::On, Wave::Sine, 1.0, 0.0, 5.0, 24000.0,
                    narrowest(1024, 3) / 2.0, 10.0, 1.0, 3.0, false, false, false, false, false, false)
                && !refused.prepare(48000.0, 1024, 3, Mode::On, Wave::Sine, 1.0, 0.0, 5.0, 24000.0,
                    narrowest(1024, 3), 10.0, 60.0, 3.0, false, false, false, false, false, false),
            "hilite arpeg should refuse a down ramp in mode 3, a sine starting before its middle in mode 4, a band under a channel and a decay over 50");
        require(refused.prepare(48000.0, 1024, 3, Mode::On, Wave::Sine, 1.0, 0.0, 5.0, 24000.0, narrowest(1024, 3),
                    10.0, 1.0, 3.0, false, false, false, false, false, false)
                && !refused.setControls(1000.0, 5.0, 24000.0, narrowest(1024, 3), 10.0, 3.0)
                && !refused.setControls(1.0, 5.0, 24000.0, narrowest(1024, 3), 10001.0, 3.0)
                && !refused.setControls(1.0, 5.0, 24000.0, narrowest(1024, 3), 10.0, 0.5),
            "hilite arpeg should refuse a rate over the frame rate, an amplification over 10000 and a sustain under a frame");
    }
    {
        using Mode = talas::cdp::HiliteGraphicEq::Mode;
        const std::string oneBandwidth = "1\n300\n441\n1733\n5000\n";
        const std::string bandwidths = "441 0.5\n1000 2\n1733 0.25\n6000 1\n";
        for (const auto& [name, mode, text, points, overlap, notch] : {
                 std::tuple{std::string{"hilite-greq-1"}, Mode::OneBandwidth, oneBandwidth, 1024, 3, false},
                 std::tuple{std::string{"hilite-greq-1-notch"}, Mode::OneBandwidth, oneBandwidth, 1024, 3, true},
                 std::tuple{std::string{"hilite-greq-2"}, Mode::Bandwidths, bandwidths, 1024, 3, false},
                 std::tuple{std::string{"hilite-greq-2-notch"}, Mode::Bandwidths, bandwidths, 1024, 3, true},
                 std::tuple{std::string{"hilite-greq-1-notch-256-points"}, Mode::OneBandwidth,
                     std::string{"; one bandwidth, then the centres\n0.5 441\n1733 2500 x 9000\n"}, 256, 2, true},
                 std::tuple{std::string{"hilite-greq-2-unsorted"}, Mode::Bandwidths,
                     std::string{"1733 0.5\n441 1\n3000 0.3\n800 0.75\n"}, 1024, 3, false},
             })
        {
            const auto bands = talas::cdp::readHiliteGraphicEq(text, mode);
            require(bands.has_value(), name + " should read its data");
            talas::cdp::HiliteGraphicEq port;
            require(port.prepare(48000.0, points, overlap, *bands, notch), name + " should prepare");
            matches(name, port, [](std::size_t) { return true; });
        }
        const auto unpaired = talas::cdp::readHiliteGraphicEq("441 0.5\n1000", Mode::Bandwidths);
        const auto tooHigh = talas::cdp::readHiliteGraphicEq("1 17000", Mode::OneBandwidth);
        talas::cdp::HiliteGraphicEq refused;
        require(!unpaired.has_value() && !talas::cdp::readHiliteGraphicEq("; nothing", Mode::OneBandwidth).has_value()
                && tooHigh.has_value() && !refused.prepare(48000.0, 1024, 3, *tooHigh, false),
            "hilite greq should refuse an unpaired band, no bands and a frequency over a third of the sample rate");
    }
    {
        const std::string bits = "100 400 1000 0.5\n500 1200 1100 0.2 3\n1500 2500 0010 1.5\n3000 4000 0011 +700\n"
                                 "4500 6000 1111 2 0.5 0.75\n9000 12000 0010 +-300\n";
        for (const auto& [name, text, points, overlap] : {
                 std::tuple{std::string{"hilite-band-bits"}, bits, 1024, 3},
                 std::tuple{std::string{"hilite-band-quirks"},
                     std::string{"2000 1000 0100 3\n\n200 600 1110x 0.5 2 0.5\n2500 3000 0000\n20000 30000 0011 1.01\n"},
                     1024, 3},
                 std::tuple{std::string{"hilite-band-bits-256-points"}, bits, 256, 2},
             })
        {
            const auto bands = talas::cdp::readHiliteBand(text);
            require(bands.has_value(), name + " should read its data");
            talas::cdp::HiliteBand port;
            require(port.prepare(48000.0, points, overlap, *bands), name + " should prepare");
            matches(name, port, [](std::size_t) { return true; });
        }
        const auto overlapping = talas::cdp::readHiliteBand("100 400 1000 2\n300 600 1000 2");
        talas::cdp::HiliteBand refused;
        require(!talas::cdp::readHiliteBand("100 400 0001").has_value()
                && !talas::cdp::readHiliteBand("100 400 1000").has_value() && overlapping.has_value()
                && !refused.prepare(48000.0, 1024, 3, *overlapping),
            "hilite band should refuse adding without a transposition, a missing amplitude and overlapping bands");
    }
}

// CDP8's blur spread and strange glis between `pvoc anal` and `pvoc synth`,
// on its formant envelope, with breakpoint files that step between two
// frames' times.
void testBlurSpreadAndGlisMatchCdp()
{
    const auto matches = [](const std::string& name, auto& port, const std::function<bool(std::size_t)>& set) {
        const auto cdp = reference(name);
        require(playLive(port, cdp.input, port.latencyFrames(), set, name) == cdp.outputs[0],
            name + " should match CDP8 exactly, latencyFrames late");
        port.reset();
        require(playLive(port, cdp.input, port.latencyFrames(), set, name) == cdp.outputs[0],
            name + " should replay after reset");
    };
    for (const auto& [name, points, overlap, pitchwise, bands, quick, spread, later, from] : {
                std::tuple{std::string{"blur-spread-1"}, 1024, 3, false, 4, false, 1.0, 1.0, std::size_t{1024}},
                std::tuple{std::string{"blur-spread-2-pitchwise-live-spread"}, 1024, 3, true, 2, false, 0.2, 0.9, std::size_t{1024}},
                std::tuple{std::string{"blur-spread-3-quicksearch"}, 1024, 3, false, 4, true, 0.6, 0.6, std::size_t{1024}},
                std::tuple{std::string{"blur-spread-4-pitchwise-quicksearch"}, 1024, 3, true, 12, true, 0.5, 0.5, std::size_t{1024}},
                std::tuple{std::string{"blur-spread-5-live-spread-512-points"}, 512, 3, false, 1, false, 0.9, 0.3, std::size_t{1344}},
                std::tuple{std::string{"blur-spread-6-quicksearch-live-spread"}, 1024, 3, false, 256, true, 0.1, 1.0, std::size_t{1024}},
                std::tuple{std::string{"blur-spread-7-pitchwise-256-points"}, 256, 3, true, 1, false, 0.75, 0.75, std::size_t{1024}},
                std::tuple{std::string{"blur-spread-8-pitchwise-0-bands"}, 1024, 3, true, 0, false, 0.8, 0.8, std::size_t{1024}},
                std::tuple{std::string{"blur-spread-9-pitchwise-76-points"}, 76, 3, true, 11, false, 0.7, 0.7, std::size_t{1024}},
         })
    {
        talas::cdp::BlurSpread port;
        require(port.prepare(48000.0, points, overlap, pitchwise, bands, quick, spread), name + " should prepare");
        matches(name, port, [&](std::size_t at) { return port.setSpread(at >= from ? later : spread); });
    }
    talas::cdp::BlurSpread refusedSpread;
    require(!refusedSpread.prepare(48000.0, 1024, 3, true, 13, false, 1.0)
            && !refusedSpread.prepare(48000.0, 1024, 3, false, 0, false, 1.0)
            && !refusedSpread.prepare(48000.0, 1024, 3, false, 257, false, 1.0)
            && refusedSpread.prepare(48000.0, 1024, 3, false, 4, false, 1.0) && !refusedSpread.setSpread(1.5),
        "blur spread should refuse 13 bands an octave, no channels a band or over half of them, and a spread over 1");
    using Mode = talas::cdp::StrangeGlis::Mode;
    for (const auto& [name, points, overlap, mode, pitchwise, bands, quick, rate, spacing, top, later] : {
                std::tuple{std::string{"strange-glis-1"}, 1024, 3, Mode::Shepard, false, 4, false, 1000.0, 50.0, 0.0, 1000.0},
                std::tuple{std::string{"strange-glis-1-pitchwise-down"}, 1024, 3, Mode::Shepard, true, 3, true, -1000.0, 50.0, 0.0, -1000.0},
                std::tuple{std::string{"strange-glis-1-live-rate"}, 1024, 3, Mode::Shepard, false, 4, false, 800.0, 50.0, 0.0, -1200.0},
                std::tuple{std::string{"strange-glis-1-512-points"}, 512, 3, Mode::Shepard, true, 6, false, 1500.0, 50.0, 15000.0, 1500.0},
                std::tuple{std::string{"strange-glis-1-zero-rate"}, 1024, 3, Mode::Shepard, false, 8, false, 0.0, 50.0, 0.0, 0.0},
                std::tuple{std::string{"strange-glis-1-top-5-hz"}, 1024, 3, Mode::Shepard, true, 4, true, 2000.0, 50.0, 5.0, 2000.0},
                std::tuple{std::string{"strange-glis-2"}, 1024, 3, Mode::Inharmonic, true, 2, false, 600.0, 46.875, 0.0, 600.0},
                std::tuple{std::string{"strange-glis-2-down-top-largest-step"}, 1024, 3, Mode::Inharmonic, false, 4, true, -900.0, 12000.0, 5000.0, -900.0},
                std::tuple{std::string{"strange-glis-2-live-rate"}, 1024, 3, Mode::Inharmonic, true, 4, false, -300.0, 46.875, 20000.0, 2500.0},
                std::tuple{std::string{"strange-glis-3"}, 1024, 3, Mode::Self, false, 4, false, 700.0, 50.0, 0.0, 700.0},
                std::tuple{std::string{"strange-glis-3-pitchwise-down-top"}, 1024, 3, Mode::Self, true, 4, true, -700.0, 50.0, 8000.0, -700.0},
                std::tuple{std::string{"strange-glis-3-live-rate"}, 1024, 3, Mode::Self, false, 2, false, -500.0, 50.0, 0.0, 900.0},
                std::tuple{std::string{"strange-glis-3-live-rate-quicksearch"}, 1024, 3, Mode::Self, true, 2, true, 2500.0, 50.0, 0.0, -2500.0},
                std::tuple{std::string{"strange-glis-3-pitchwise-76-points"}, 76, 3, Mode::Self, true, 11, false, 1200.0, 50.0, 0.0, 1200.0},
         })
    {
        talas::cdp::StrangeGlis port;
        require(port.prepare(48000.0, points, overlap, mode, pitchwise, bands, quick, rate, spacing, top > 0.0, top),
            name + " should prepare");
        matches(name, port, [&](std::size_t at) { return port.setGlisRate(at >= 1024 ? later : rate); });
    }
    talas::cdp::StrangeGlis refusedGlis;
    require(!refusedGlis.prepare(48000.0, 1024, 3, Mode::Inharmonic, false, 4, false, 100.0, 40.0, false, 0.0)
            && !refusedGlis.prepare(48000.0, 1024, 3, Mode::Inharmonic, false, 4, false, 100.0, 12001.0, false, 0.0)
            && !refusedGlis.prepare(48000.0, 1024, 3, Mode::Shepard, false, 4, false, 100.0, 50.0, true, 4.0)
            && refusedGlis.prepare(48000.0, 1024, 3, Mode::Self, false, 4, false, 100.0, 50.0, false, 0.0)
            && !refusedGlis.setGlisRate(3001.0) && refusedGlis.setGlisRate(-3000.0),
        "strange glis should refuse a spacing under a channel or over a quarter of the sample rate, a top under 5 Hz and a rate over 8 semitones a frame");
}

// Plays the input and side chain, then latency frames of silence, through a
// two-file port in 64-frame blocks, calling set before each block; returns
// what follows the latency, as long as the input.
template <typename Port>
std::vector<double> playPair(Port& port, std::vector<double> input, std::vector<double> side,
    const std::function<bool(std::size_t)>& set, const std::string& name)
{
    const auto latency = port.latencyFrames();
    const auto length = input.size();
    input.resize(length + latency, 0.0);
    side.resize(length + latency, 0.0);
    std::vector<double> output(input.size());
    for (std::size_t offset = 0; offset < input.size(); offset += 64)
    {
        const auto count = std::min<std::size_t>(64, input.size() - offset);
        require(set(offset), name + " should take its fields");
        require(port.process({input.data() + offset, count}, {side.data() + offset, count},
                    {output.data() + offset, count}),
            name + " should process");
    }
    return {output.begin() + static_cast<std::ptrdiff_t>(latency), output.end()};
}

// CDP8's combine sum, diff, max, mean and cross and formants vocode on two
// files between `pvoc anal` and `pvoc synth`, with breakpoint files that step
// between two frames' times; the references hold the input, the side chain
// (CDP's second file) and the output.
void testCombineAndVocodeMatchCdp()
{
    const auto matches = [](const std::string& name, auto& port, const std::function<bool(std::size_t)>& set) {
        const auto cdp = reference(name, 2);
        require(playPair(port, cdp.input, cdp.outputs[0], set, name) == cdp.outputs[1],
            name + " should match CDP8 exactly, latencyFrames late");
        port.reset();
        require(playPair(port, cdp.input, cdp.outputs[0], set, name) == cdp.outputs[1],
            name + " should replay after reset");
    };
    for (const auto& [name, points, crossover, live, later, from] : {
             std::tuple{std::string{"combine-sum-1"}, 1024, 1.0, false, 1.0, std::size_t{1024}},
             std::tuple{std::string{"combine-sum-2-crossover-0.4"}, 1024, 0.4, false, 0.4, std::size_t{1024}},
             std::tuple{std::string{"combine-sum-3-live-crossover"}, 1024, 0.3, true, 0.8, std::size_t{1024}},
             std::tuple{std::string{"combine-sum-4-live-crossover-from-1-512-points"}, 512, 1.0, true, 0.2, std::size_t{1344}},
         })
    {
        talas::cdp::CombineSum port;
        require(port.prepare(48000.0, points, 3, crossover, live), name + " should prepare");
        matches(name, port, [&](std::size_t at) { return port.setCrossover(at >= from ? later : crossover); });
    }
    for (const auto& [name, points, crossover, live, keepNegative, later, from] : {
             std::tuple{std::string{"combine-diff-1"}, 1024, 1.0, false, false, 1.0, std::size_t{1024}},
             std::tuple{std::string{"combine-diff-2-crossover-0.6-keep-negative"}, 1024, 0.6, false, true, 0.6, std::size_t{1024}},
             std::tuple{std::string{"combine-diff-3-live-crossover"}, 1024, 0.25, true, false, 0.9, std::size_t{1024}},
             std::tuple{std::string{"combine-diff-4-keep-negative"}, 1024, 1.0, false, true, 1.0, std::size_t{1024}},
             std::tuple{std::string{"combine-diff-5-live-crossover-keep-negative-256-points"}, 256, 0.7, true, true, 0.1, std::size_t{1024}},
         })
    {
        talas::cdp::CombineDiff port;
        require(port.prepare(48000.0, points, 3, crossover, live, keepNegative), name + " should prepare");
        matches(name, port, [&](std::size_t at) { return port.setCrossover(at >= from ? later : crossover); });
    }
    for (const auto& [name, points] : {std::pair{std::string{"combine-max-1"}, 1024},
             std::pair{std::string{"combine-max-2-512-points"}, 512}})
    {
        talas::cdp::CombineMax port;
        require(port.prepare(48000.0, points, 3), name + " should prepare");
        matches(name, port, [](std::size_t) { return true; });
    }
    {
        using Mode = talas::cdp::CombineMean::Mode;
        for (const auto& [name, points, mode, low, high, channels, zeroOutside] : {
             std::tuple{std::string{"combine-mean-1"}, 1024, Mode::MeanAmplitudeMeanPitch, 5.0, 24000.0, 513, false},
             std::tuple{std::string{"combine-mean-2"}, 1024, Mode::MeanAmplitudeMeanFrequency, 5.0, 24000.0, 513, false},
             std::tuple{std::string{"combine-mean-3"}, 1024, Mode::InputAmplitudeMeanPitch, 5.0, 24000.0, 513, false},
             std::tuple{std::string{"combine-mean-4"}, 1024, Mode::InputAmplitudeMeanFrequency, 5.0, 24000.0, 513, false},
             std::tuple{std::string{"combine-mean-5"}, 1024, Mode::SideAmplitudeMeanPitch, 5.0, 24000.0, 513, false},
             std::tuple{std::string{"combine-mean-6"}, 1024, Mode::SideAmplitudeMeanFrequency, 5.0, 24000.0, 513, false},
             std::tuple{std::string{"combine-mean-7"}, 1024, Mode::MaximumAmplitudeMeanPitch, 5.0, 24000.0, 513, false},
             std::tuple{std::string{"combine-mean-8"}, 1024, Mode::MaximumAmplitudeMeanFrequency, 5.0, 24000.0, 513, false},
             std::tuple{std::string{"combine-mean-1-range-and-channels"}, 1024, Mode::MeanAmplitudeMeanPitch, 300.0, 5000.0, 20, false},
             std::tuple{std::string{"combine-mean-2-zero-outside"}, 1024, Mode::MeanAmplitudeMeanFrequency, 500.0, 4000.0, 513, true},
             std::tuple{std::string{"combine-mean-7-high-below-low"}, 1024, Mode::MaximumAmplitudeMeanPitch, 6000.0, 800.0, 513, false},
             std::tuple{std::string{"combine-mean-4-256-points"}, 256, Mode::InputAmplitudeMeanFrequency, 5.0, 12000.0, 129, false},
             std::tuple{std::string{"combine-mean-6-zero-outside-512-points"}, 512, Mode::SideAmplitudeMeanFrequency, 100.0, 9000.0, 40, true},
             std::tuple{std::string{"combine-mean-5-range-512-points"}, 512, Mode::SideAmplitudeMeanPitch, 250.0, 18000.0, 257, false},
             std::tuple{std::string{"combine-mean-8-range"}, 1024, Mode::MaximumAmplitudeMeanFrequency, 1000.0, 3000.0, 513, false},
             std::tuple{std::string{"combine-mean-3-high-below-low-256-points"}, 256, Mode::InputAmplitudeMeanPitch, 9000.0, 400.0, 129, false},
             std::tuple{std::string{"combine-mean-2-top-channels"}, 1024, Mode::MeanAmplitudeMeanFrequency, 23900.0, 24000.0, 513, false},
             std::tuple{std::string{"combine-mean-1-top-channels"}, 1024, Mode::MeanAmplitudeMeanPitch, 23800.0, 24000.0, 513, false},
             })
        {
            talas::cdp::CombineMean port;
            require(port.prepare(48000.0, points, 3, mode, low, high, channels, zeroOutside), name + " should prepare");
            matches(name, port, [](std::size_t) { return true; });
        }
    }
    for (const auto& [name, points, interpolation, live, later, from] : {
             std::tuple{std::string{"combine-cross-1"}, 1024, 1.0, false, 1.0, std::size_t{1024}},
             std::tuple{std::string{"combine-cross-2-interp-0.35"}, 1024, 0.35, false, 0.35, std::size_t{1024}},
             std::tuple{std::string{"combine-cross-3-live-interp"}, 1024, 0.2, true, 0.75, std::size_t{1024}},
             std::tuple{std::string{"combine-cross-4-live-interp-512-points"}, 512, 0.9, true, 0.4, std::size_t{1344}},
             std::tuple{std::string{"combine-cross-5-interp-within-flterr"}, 1024, 0.999999, false, 0.999999, std::size_t{1024}},
         })
    {
        talas::cdp::CombineCross port;
        require(port.prepare(48000.0, points, 3, interpolation, live), name + " should prepare");
        matches(name, port, [&](std::size_t at) { return port.setInterpolation(at >= from ? later : interpolation); });
    }
    for (const auto& [name, points, pitchwise, bands, quick, low, high, gain] : {
             std::tuple{std::string{"formants-vocode-1"}, 1024, false, 4, false, 5.0, 24000.0, 1.0},
             std::tuple{std::string{"formants-vocode-2-pitchwise"}, 1024, true, 3, false, 5.0, 24000.0, 1.0},
             std::tuple{std::string{"formants-vocode-3-quicksearch-range-gain"}, 1024, false, 2, true, 200.0, 9000.0, 0.5},
             std::tuple{std::string{"formants-vocode-4-pitchwise-quicksearch-gain"}, 1024, true, 12, true, 5.0, 24000.0, 3.0},
             std::tuple{std::string{"formants-vocode-5-512-points"}, 512, true, 2, false, 100.0, 24000.0, 0.8},
             std::tuple{std::string{"formants-vocode-6-pitchwise-0-bands-256-points"}, 256, true, 0, false, 5.0, 24000.0, 1.0},
             std::tuple{std::string{"formants-vocode-7-high-under-10-hz"}, 1024, false, 8, false, 5.0, 9.0, 1.0},
             std::tuple{std::string{"formants-vocode-8-gain-within-flterr"}, 1024, false, 4, false, 12.0, 24000.0, 1.000001},
         })
    {
        talas::cdp::FormantsVocode port;
        require(port.prepare(48000.0, points, 3, pitchwise, bands, quick, low, high, gain), name + " should prepare");
        matches(name, port, [](std::size_t) { return true; });
    }
    talas::cdp::CombineSum sum;
    talas::cdp::CombineMean mean;
    talas::cdp::FormantsVocode vocode;
    std::vector<double> block(64, 0.0);
    std::vector<double> shorter(32, 0.0);
    require(!sum.prepare(48000.0, 1024, 3, 1.5, false) && sum.prepare(48000.0, 1024, 3, 1.0, false)
            && !sum.setCrossover(-0.1) && !sum.process(block, shorter, block)
            && !mean.prepare(48000.0, 1024, 3, talas::cdp::CombineMean::Mode::MeanAmplitudeMeanPitch, 4.0, 24000.0,
                513, false)
            && !vocode.prepare(48000.0, 1024, 3, false, 4, false, 5.0, 24000.0, 11.0),
        "combine and vocode should refuse a crossover outside 0 to 1, a side chain of another length, a mean under 5 Hz and a vocode gain over 10");
}

// CDP8's morph morph and bridge on two files between `pvoc anal` and `pvoc
// synth`, with the times from the start; the references hold the input, the
// side chain (CDP's second file) and the output.
void testMorphMatchesCdp()
{
    const auto matches = [](const std::string& name, auto& port) {
        const auto cdp = reference(name, 2);
        const auto set = [](std::size_t) { return true; };
        require(playPair(port, cdp.input, cdp.outputs[0], set, name) == cdp.outputs[1],
            name + " should match CDP8 exactly, latencyFrames late");
        port.reset();
        require(playPair(port, cdp.input, cdp.outputs[0], set, name) == cdp.outputs[1],
            name + " should replay after reset");
    };
    {
        using Mode = talas::cdp::MorphMorph::Mode;
        for (const auto& [name, points, overlap, mode, amplitudeStart, amplitudeEnd, frequencyStart, frequencyEnd,
                 amplitudeExponent, frequencyExponent, stagger] : {
             std::tuple{std::string{"morph-morph-1"}, 1024, 3, Mode::Linear, 0.005, 0.03, 0.01, 0.035, 1.0, 1.0, 0.0},
             std::tuple{std::string{"morph-morph-1-exponents-apart"}, 1024, 3, Mode::Linear, 0.008, 0.02, 0.022, 0.038, 2.5, 0.4, 0.0},
             std::tuple{std::string{"morph-morph-2"}, 1024, 3, Mode::Cosine, 0.005, 0.03, 0.01, 0.035, 1.0, 1.0, 0.0},
             std::tuple{std::string{"morph-morph-2-exponents-apart"}, 1024, 3, Mode::Cosine, 0.024, 0.04, 0.004, 0.02, 3.0, 0.3, 0.0},
             std::tuple{std::string{"morph-morph-1-stagger"}, 1024, 3, Mode::Linear, 0.008, 0.03, 0.01, 0.035, 1.5, 0.7, 0.004},
             std::tuple{std::string{"morph-morph-2-stagger"}, 1024, 3, Mode::Cosine, 0.012, 0.025, 0.015, 0.04, 0.6, 2.0, 0.01},
             std::tuple{std::string{"morph-morph-1-stagger-512-points"}, 512, 3, Mode::Linear, 0.005, 0.03, 0.004, 0.025, 2.0, 0.5, 0.003},
             std::tuple{std::string{"morph-morph-2-256-points"}, 256, 3, Mode::Cosine, 0.003, 0.02, 0.006, 0.025, 4.0, 0.25, 0.0},
             std::tuple{std::string{"morph-morph-1-overlap-4"}, 1024, 4, Mode::Linear, 0.006, 0.024, 0.002, 0.03, 0.8, 1.6, 0.002},
             std::tuple{std::string{"morph-morph-2-exponent-within-flterr"}, 1024, 3, Mode::Cosine, 0.005, 0.03, 0.01, 0.035, 0.020001, 49.999999, 0.0},
             std::tuple{std::string{"morph-morph-1-amplitude-in-one-frame"}, 1024, 3, Mode::Linear, 0.005, 0.0055, 0.008, 0.03, 1.0, 1.0, 0.0},
             std::tuple{std::string{"morph-morph-2-starts-at-stagger"}, 1024, 3, Mode::Cosine, 0.008, 0.03, 0.008, 0.03, 1.7, 0.6, 0.008},
             })
        {
            talas::cdp::MorphMorph port;
            require(port.prepare(48000.0, points, overlap, mode, amplitudeStart, amplitudeEnd, frequencyStart,
                        frequencyEnd, amplitudeExponent, frequencyExponent, stagger),
                name + " should prepare");
            matches(name, port);
        }
        talas::cdp::MorphMorph refused;
        require(!refused.prepare(48000.0, 1024, 3, Mode::Linear, 0.002, 0.03, 0.01, 0.035, 1.0, 1.0, 0.004)
                && !refused.prepare(48000.0, 1024, 3, Mode::Linear, 0.01, 0.01, 0.01, 0.035, 1.0, 1.0, 0.0)
                && !refused.prepare(48000.0, 1024, 3, Mode::Cosine, 0.01, 0.03, 0.01, 0.035, 60.0, 1.0, 0.0),
            "morph morph should refuse a start before the stagger, an end at its start and an exponent over 50");
    }
    {
        using Mode = talas::cdp::MorphBridge::Mode;
        for (const auto& [name, points, overlap, mode, offset, startSideFrequency, startSideAmplitude,
                 endSideFrequency, endSideAmplitude, start, end] : {
             std::tuple{std::string{"morph-bridge-1"}, 1024, 3, Mode::Direct, 0.0, 0.0, 0.0, 1.0, 1.0, 0.005, 0.035},
             std::tuple{std::string{"morph-bridge-1-fractions"}, 1024, 3, Mode::Direct, 0.0, 0.2, 0.3, 0.7, 0.9, 0.008, 0.03},
             std::tuple{std::string{"morph-bridge-1-ends-in-input"}, 1024, 3, Mode::Direct, 0.0, 0.5, 0.5, 0.0, 0.0, 0.006, 0.03},
             std::tuple{std::string{"morph-bridge-2"}, 1024, 3, Mode::Minimum, 0.0, 0.0, 0.25, 0.6, 0.8, 0.006, 0.03},
             std::tuple{std::string{"morph-bridge-3"}, 1024, 3, Mode::FollowInput, 0.0, 0.1, 0.4, 0.9, 0.6, 0.01, 0.032},
             std::tuple{std::string{"morph-bridge-4-from-start"}, 1024, 3, Mode::FollowSide, 0.0, 0.0, 0.5, 1.0, 0.2, 0.0, 0.03},
             std::tuple{std::string{"morph-bridge-5"}, 1024, 3, Mode::InputToSide, 0.0, 0.0, 0.0, 1.0, 1.0, 0.008, 0.035},
             std::tuple{std::string{"morph-bridge-5-side-to-input"}, 1024, 3, Mode::InputToSide, 0.0, 1.0, 1.0, 0.0, 0.0, 0.006, 0.03},
             std::tuple{std::string{"morph-bridge-6"}, 1024, 3, Mode::SideToInput, 0.0, 0.3, 0.2, 0.8, 0.7, 0.006, 0.03},
             std::tuple{std::string{"morph-bridge-1-offset"}, 1024, 3, Mode::Direct, 0.008, 0.0, 0.0, 1.0, 1.0, 0.0, 0.035},
             std::tuple{std::string{"morph-bridge-1-offset-ends-in-side"}, 1024, 3, Mode::Direct, 0.01, 0.0, 0.0, 1.0, 1.0, 0.012, 0.04},
             std::tuple{std::string{"morph-bridge-3-offset"}, 1024, 3, Mode::FollowInput, 0.005, 0.0, 0.0, 0.6, 0.8, 0.01, 0.03},
             std::tuple{std::string{"morph-bridge-3-offset-start-0"}, 1024, 3, Mode::FollowInput, 0.006, 0.0, 0.0, 0.3, 0.5, 0.0, 0.03},
             std::tuple{std::string{"morph-bridge-5-offset"}, 1024, 3, Mode::InputToSide, 0.006, 0.0, 0.0, 1.0, 1.0, 0.0, 0.033},
             std::tuple{std::string{"morph-bridge-2-offset-under-half-frame"}, 1024, 3, Mode::Minimum, 0.001, 0.4, 0.3, 1.0, 1.0, 0.005, 0.03},
             std::tuple{std::string{"morph-bridge-2-512-points"}, 512, 3, Mode::Minimum, 0.0, 0.2, 0.1, 0.9, 1.0, 0.004, 0.03},
             std::tuple{std::string{"morph-bridge-6-256-points"}, 256, 3, Mode::SideToInput, 0.0, 0.6, 0.4, 0.1, 0.2, 0.003, 0.025},
             std::tuple{std::string{"morph-bridge-1-offset-over-a-buffer-512-points"}, 512, 3, Mode::Direct, 0.03, 0.0, 0.0, 1.0, 1.0, 0.0, 0.04},
             std::tuple{std::string{"morph-bridge-5-offset-512-points"}, 512, 3, Mode::InputToSide, 0.02, 0.0, 0.0, 0.7, 0.5, 0.024, 0.045},
             std::tuple{std::string{"morph-bridge-4-overlap-2"}, 1024, 2, Mode::FollowSide, 0.0, 0.3, 0.6, 1.0, 0.1, 0.006, 0.03},
             std::tuple{std::string{"morph-bridge-1-offset-fractions-within-flterr"}, 1024, 3, Mode::Direct, 0.008, 1e-06, 1.9e-06, 1.0, 1.0, 0.0, 0.035},
             })
        {
            talas::cdp::MorphBridge port;
            require(port.prepare(48000.0, points, overlap, mode, offset, startSideFrequency, startSideAmplitude,
                        endSideFrequency, endSideAmplitude, start, end),
                name + " should prepare");
            matches(name, port);
        }
        talas::cdp::MorphBridge refused;
        require(!refused.prepare(48000.0, 1024, 3, Mode::FollowSide, 0.008, 0.0, 0.0, 1.0, 1.0, 0.0, 0.035)
                && !refused.prepare(48000.0, 1024, 3, Mode::Direct, 0.008, 0.0, 0.3, 1.0, 1.0, 0.0, 0.035)
                && !refused.prepare(48000.0, 1024, 3, Mode::Direct, 0.01, 0.0, 0.0, 1.0, 1.0, 0.005, 0.035)
                && !refused.prepare(48000.0, 1024, 3, Mode::Direct, 0.0, 0.0, 0.0, 1.0, 1.5, 0.005, 0.035),
            "morph bridge should refuse an offset that starts CDP8's output late, an offset after the start and a fraction over 1");
    }
}

// CDP8's repitch transposef between `pvoc anal` and `pvoc synth`, with
// breakpoint files that step between two frames' times.
void testRepitchTransposefMatchesCdp()
{
    using Units = talas::cdp::RepitchTransposeFormants::Units;
    struct Change
    {
        std::size_t at;
        std::size_t field;
        double value;
    };
    struct Transposef
    {
        std::string name;
        int points;
        int overlap;
        Units units;
        bool pitchwise;
        int bands;
        bool quickSearch;
        // The transposition and the low and high frequencies.
        std::array<double, 3> controls;
        bool liveTransposition;
        bool liveLow;
        bool liveHigh;
        bool fuller;
        std::vector<Change> changes;
    };
    for (const auto& c : {
            Transposef{"repitch-transposef-1", 1024, 3, Units::Ratio, false, 4, false, {1.5, 0.0, 0.0}, false, false, false, false, {}},
            Transposef{"repitch-transposef-1-live-ratio", 1024, 3, Units::Ratio, false, 4, false, {1.5, 0.0, 0.0}, true, false, false, false, {Change{1024, 0, 0.75}}},
            Transposef{"repitch-transposef-1-live-ratio-quicksearch-fuller", 1024, 3, Units::Ratio, false, 4, true, {0.8, 0.0, 0.0}, true, false, false, true, {Change{1024, 0, 1.6}}},
            Transposef{"repitch-transposef-2-octaves-pitchwise", 1024, 3, Units::Octaves, true, 6, false, {-0.5, 0.0, 0.0}, false, false, false, false, {}},
            Transposef{"repitch-transposef-2-octaves-band-256-points", 256, 2, Units::Octaves, false, 2, false, {0.5, 200.0, 3000.0}, false, false, false, false, {}},
            Transposef{"repitch-transposef-3-semitones-pitchwise-quicksearch-fuller", 1024, 3, Units::Semitones, true, 4, true, {5.0, 0.0, 0.0}, false, false, false, true, {}},
            Transposef{"repitch-transposef-3-live-semitones-pitchwise", 1024, 3, Units::Semitones, true, 4, false, {5.0, 0.0, 0.0}, true, false, false, false, {Change{1024, 0, -7.0}}},
            Transposef{"repitch-transposef-1-live-low-and-high", 1024, 3, Units::Ratio, false, 4, false, {1.25, 5.0, 4000.0}, false, true, true, false, {Change{1024, 1, 900.0}, Change{1344, 2, 2000.0}}},
            Transposef{"repitch-transposef-1-high-under-10-hz", 1024, 3, Units::Ratio, false, 4, false, {1.25, 300.0, 7.0}, false, false, false, false, {}},
            Transposef{"repitch-transposef-3-semitones-512-points", 512, 3, Units::Semitones, false, 8, false, {-3.0, 0.0, 0.0}, false, false, false, false, {}},
            Transposef{"repitch-transposef-1-pitchwise-0-bands", 1024, 3, Units::Ratio, true, 0, false, {2.0, 0.0, 0.0}, false, false, false, false, {}},
            Transposef{"repitch-transposef-3-semitones-9.57", 1024, 3, Units::Semitones, false, 4, false, {9.57, 0.0, 0.0}, false, false, false, false, {}},
            Transposef{"repitch-transposef-3-live-semitones-9.57-and-minus-11.98", 1024, 3, Units::Semitones, false, 4, false, {9.57, 0.0, 0.0}, true, false, false, false, {Change{1024, 0, -11.98}}},
         })
    {
        talas::cdp::RepitchTransposeFormants port;
        const auto& [transposition, low, high] = c.controls;
        require(port.prepare(48000.0, c.points, c.overlap, c.units, c.pitchwise, c.bands, c.quickSearch, transposition,
                    c.liveTransposition, low, high, c.liveLow, c.liveHigh, c.fuller),
            c.name + " should prepare");
        const auto set = [&](std::size_t at) {
            auto controls = c.controls;
            for (const auto& change : c.changes)
                if (at >= change.at)
                    controls[change.field] = change.value;
            return port.setControls(controls[0], controls[1], controls[2]);
        };
        const auto cdp = reference(c.name);
        require(playLive(port, cdp.input, port.latencyFrames(), set, c.name) == cdp.outputs[0],
            c.name + " should match CDP8 exactly, latencyFrames late");
        port.reset();
        require(playLive(port, cdp.input, port.latencyFrames(), set, c.name) == cdp.outputs[0],
            c.name + " should replay after reset");
    }
    talas::cdp::RepitchTransposeFormants refused;
    require(!refused.prepare(48000.0, 1024, 3, Units::Octaves, false, 4, false, 8.5, false, 0.0, 0.0, false, false,
                false)
            && !refused.prepare(48000.0, 1024, 3, Units::Ratio, false, 0, false, 1.5, false, 0.0, 0.0, false, false,
                false)
            && !refused.prepare(48000.0, 1024, 3, Units::Ratio, true, 13, false, 1.5, false, 0.0, 0.0, false, false,
                false)
            && refused.prepare(48000.0, 1024, 3, Units::Ratio, false, 4, false, 1.5, true, 0.0, 0.0, false, false,
                false)
            && !refused.setControls(0.003, 0.0, 0.0) && !refused.setControls(1.5, 0.0, 24001.0),
        "repitch transposef should refuse more than eight octaves, no channels a band, 13 bands an octave and a frequency over half the sample rate");
}

// CDP8's formants get of a second file then formants put on the first, and
// hilite vowels, between `pvoc anal` and `pvoc synth`.
void testFormantsPutAndVowelsMatchCdp()
{
    {
        using Mode = talas::cdp::FormantsPut::Mode;
        for (const auto& [name, points, overlap, pitchwise, bands, sideQuickSearch, mode, quickSearch, low, high,
                 gain] : {
             std::tuple{std::string{"formants-put-1-replace-f4"}, 1024, 3, false, 4, false, Mode::Replace, false, 5.0, 24000.0, 1.0},
             std::tuple{std::string{"formants-put-2-impose-f4"}, 1024, 3, false, 4, false, Mode::Impose, false, 5.0, 24000.0, 1.0},
             std::tuple{std::string{"formants-put-3-replace-p3"}, 1024, 3, true, 3, false, Mode::Replace, false, 5.0, 24000.0, 1.0},
             std::tuple{std::string{"formants-put-4-impose-p12"}, 1024, 3, true, 12, false, Mode::Impose, false, 5.0, 24000.0, 1.0},
             std::tuple{std::string{"formants-put-5-replace-f2-quicksearch"}, 1024, 3, false, 2, false, Mode::Replace, true, 5.0, 24000.0, 1.0},
             std::tuple{std::string{"formants-put-6-replace-get-quicksearch-p6"}, 1024, 3, true, 6, true, Mode::Replace, false, 5.0, 24000.0, 1.0},
             std::tuple{std::string{"formants-put-7-replace-range-gain"}, 1024, 3, false, 4, false, Mode::Replace, false, 300.0, 6000.0, 0.5},
             std::tuple{std::string{"formants-put-8-impose-range-gain-p4"}, 1024, 3, true, 4, false, Mode::Impose, false, 150.0, 9000.0, 3.0},
             std::tuple{std::string{"formants-put-9-low-under-10-hz-gain-within-flterr"}, 1024, 3, false, 8, false, Mode::Replace, false, 7.0, 20000.0, 1.000001},
             std::tuple{std::string{"formants-put-10-replace-quicksearch-512-points"}, 512, 3, false, 4, false, Mode::Replace, true, 100.0, 24000.0, 1.0},
             std::tuple{std::string{"formants-put-11-impose-get-quicksearch-256-points"}, 256, 3, false, 2, true, Mode::Impose, false, 5.0, 12000.0, 0.8},
             std::tuple{std::string{"formants-put-12-replace-p0-256-points"}, 256, 3, true, 0, false, Mode::Replace, false, 5.0, 24000.0, 1.0},
             std::tuple{std::string{"formants-put-13-impose-p2-overlap-4"}, 1024, 4, true, 2, false, Mode::Impose, false, 400.0, 24000.0, 1.0},
             std::tuple{std::string{"formants-put-14-replace-both-quicksearch-f3-overlap-4"}, 1024, 4, false, 3, true, Mode::Replace, true, 5.0, 15000.0, 2.0},
             std::tuple{std::string{"formants-put-15-replace-f1-shift-10"}, 1024, 3, false, 1, false, Mode::Replace, false, 9.0, 10000.0, 0.25},
             })
        {
            talas::cdp::FormantsPut port;
            require(port.prepare(48000.0, points, overlap, pitchwise, bands, sideQuickSearch, mode, quickSearch, low,
                        high, gain),
                name + " should prepare");
            const auto cdp = reference(name, 2);
            const auto set = [](std::size_t) { return true; };
            require(playPair(port, cdp.input, cdp.outputs[0], set, name) == cdp.outputs[1],
                name + " should match CDP8 exactly, latencyFrames late");
            port.reset();
            require(playPair(port, cdp.input, cdp.outputs[0], set, name) == cdp.outputs[1],
                name + " should replay after reset");
        }
        talas::cdp::FormantsPut refused;
        require(!refused.prepare(48000.0, 1024, 3, false, 4, false, Mode::Replace, false, 5.0, 24000.0, 41.0)
                && !refused.prepare(48000.0, 1024, 3, true, 13, false, Mode::Replace, false, 5.0, 24000.0, 1.0)
                && !refused.prepare(48000.0, 1024, 3, false, 4, false, Mode::Impose, false, 3000.0, 2000.0, 1.0),
            "formants put should refuse a gain over 40, 13 bands an octave and a high frequency under the low one");
    }
    for (const auto& [name, points, overlap, text, halfWidth, steepness, range, threshold] : {
             std::tuple{std::string{"hilite-vowels-1"}, 1024, 3, std::string{"0 a\n0.012 ee\n0.03 oo\n"}, 0.25, 3.0, 0.95, 0.5},
             std::tuple{std::string{"hilite-vowels-2-late-start-narrow"}, 1024, 3, std::string{"; a late start, pairs across lines\n0.008 i 0.02\nar 0.035 u\n"}, 0.1, 3.0, 0.95, 0.5},
             std::tuple{std::string{"hilite-vowels-3-one-vowel-wide-steep"}, 1024, 3, std::string{"0 x\n"}, 1.5, 8.0, 0.95, 0.5},
             std::tuple{std::string{"hilite-vowels-4-close-times-range-shallow"}, 1024, 3, std::string{"0 e\n0.007999 o\n0.0080005 n\n0.03 th\n"}, 0.25, 0.5, 0.3, 0.5},
             std::tuple{std::string{"hilite-vowels-5-threshold-low"}, 1024, 3, std::string{"0 a\n0.012 ee\n0.03 oo\n"}, 0.25, 3.0, 0.95, 0.05},
             std::tuple{std::string{"hilite-vowels-6-all-vowels-256-points"}, 256, 3, std::string{"0 ee\n0.002 i\n0.004 ai\n0.006 aii\n0.008 e\n0.01 a\n0.012 ar\n0.014 o\n0.016 or\n0.018 oa\n" "0.02 u\n0.022 uu\n0.024 ui\n0.026 oo\n0.028 xx\n0.03 x\n0.032 n\n0.034 m\n0.036 r\n0.038 th\n"}, 0.3, 2.0, 0.8, 0.7},
             std::tuple{std::string{"hilite-vowels-7-range-0-threshold-1-512-points"}, 512, 3, std::string{"0 a\n0.012 ee\n0.03 oo\n"}, 0.5, 1.0, 0.0, 1.0},
             std::tuple{std::string{"hilite-vowels-8-extremes"}, 1024, 3, std::string{"; a late start, pairs across lines\n0.008 i 0.02\nar 0.035 u\n"}, 10.0, 0.1, 1.0, 0.0},
             std::tuple{std::string{"hilite-vowels-9-vowel-name-overlap-4"}, 1024, 4, std::string{"oo"}, 0.25, 3.0, 0.95, 0.5},
             std::tuple{std::string{"hilite-vowels-10-threshold-0-steepness-10"}, 1024, 3, std::string{"0 e\n0.007999 o\n0.0080005 n\n0.03 th\n"}, 0.01, 10.0, 0.6, 0.0},
         })
    {
        const auto single = talas::cdp::hiliteVowel(text);
        const auto vowels = single.has_value()
            ? std::optional{std::vector<talas::cdp::HiliteVowels::Point>{{0.0, *single}}}
            : talas::cdp::readHiliteVowels(text);
        require(vowels.has_value(), name + " should read its vowels");
        talas::cdp::HiliteVowels port;
        require(port.prepare(48000.0, points, overlap, *vowels, halfWidth, steepness, range, threshold),
            name + " should prepare");
        const auto cdp = reference(name);
        const auto set = [](std::size_t) { return true; };
        require(playLive(port, cdp.input, port.latencyFrames(), set, name) == cdp.outputs[0],
            name + " should match CDP8 exactly, latencyFrames late");
        port.reset();
        require(playLive(port, cdp.input, port.latencyFrames(), set, name) == cdp.outputs[0],
            name + " should replay after reset");
    }
    const auto backwards = talas::cdp::readHiliteVowels("0 a\n0.02 e\n0.01 i\n");
    const auto unpaired = talas::cdp::readHiliteVowels("0 a\n0.02");
    const auto unknown = talas::cdp::readHiliteVowels("0 q\n");
    const auto good = talas::cdp::readHiliteVowels("0 a\n0.02 e\n");
    talas::cdp::HiliteVowels refused;
    require(!backwards.has_value() && !unpaired.has_value() && !unknown.has_value() && good.has_value()
            && !refused.prepare(48000.0, 1024, 3, *good, 11.0, 3.0, 0.95, 0.5)
            && !refused.prepare(48000.0, 1024, 3, *good, 0.25, 3.0, 1.5, 0.5),
        "hilite vowels should refuse falling times, an unpaired time, an unknown vowel, a half-width over 10 and a range over 1");
}

// CDP8's blur blur and hilite bltr between `pvoc anal` and `pvoc synth`, a
// maximum blur's frames late, as far as CDP's last blur, shortened at its
// file's end, cannot reach.
void testBlurBlurAndBltrMatchCdp()
{
    struct Blurred
    {
        std::string name;
        int points;
        int maximumBlur;
        // Each value from a block offset on.
        std::vector<std::pair<std::size_t, double>> blur;
        bool traced;
        std::vector<std::pair<std::size_t, double>> trace;
        std::size_t compared;
    };
    const auto at = [](const std::vector<std::pair<std::size_t, double>>& steps, std::size_t offset) {
        auto value = steps.front().second;
        for (const auto& [from, next] : steps)
            if (offset >= from)
                value = next;
        return value;
    };
    for (const auto& c : {
        Blurred{"blur-blur-1", 1024, 1, {std::pair<std::size_t, double>{0, 1.0}}, false, {std::pair<std::size_t, double>{0, 1.0}}, 2048},
        Blurred{"blur-blur-4", 1024, 4, {std::pair<std::size_t, double>{0, 4.0}}, false, {std::pair<std::size_t, double>{0, 1.0}}, 1664},
        Blurred{"blur-blur-7", 1024, 7, {std::pair<std::size_t, double>{0, 7.0}}, false, {std::pair<std::size_t, double>{0, 1.0}}, 1792},
        Blurred{"blur-blur-23-ends-on-last-frame", 1024, 23, {std::pair<std::size_t, double>{0, 23.0}}, false, {std::pair<std::size_t, double>{0, 1.0}}, 2048},
        Blurred{"blur-blur-live-blur", 1024, 9, {std::pair<std::size_t, double>{0, 3.0}, std::pair<std::size_t, double>{640, 7.0}, std::pair<std::size_t, double>{1344, 2.0}}, false, {std::pair<std::size_t, double>{0, 1.0}}, 2048},
        Blurred{"blur-blur-live-blur-through-1-512-points", 512, 8, {std::pair<std::size_t, double>{0, 5.0}, std::pair<std::size_t, double>{704, 1.0}, std::pair<std::size_t, double>{1152, 8.0}}, false, {std::pair<std::size_t, double>{0, 1.0}}, 1728},
        Blurred{"blur-blur-32-4096-frame-source", 1024, 32, {std::pair<std::size_t, double>{0, 32.0}}, false, {std::pair<std::size_t, double>{0, 1.0}}, 3200},
        Blurred{"hilite-bltr-6-8", 1024, 6, {std::pair<std::size_t, double>{0, 6.0}}, true, {std::pair<std::size_t, double>{0, 8.0}}, 1408},
        Blurred{"hilite-bltr-1-3", 1024, 1, {std::pair<std::size_t, double>{0, 1.0}}, true, {std::pair<std::size_t, double>{0, 3.0}}, 1920},
        Blurred{"hilite-bltr-live-blur-and-trace", 1024, 8, {std::pair<std::size_t, double>{0, 2.0}, std::pair<std::size_t, double>{704, 6.0}, std::pair<std::size_t, double>{1536, 3.0}}, true, {std::pair<std::size_t, double>{0, 5.0}, std::pair<std::size_t, double>{1024, 30.0}, std::pair<std::size_t, double>{1728, 2.0}}, 1792},
        Blurred{"hilite-bltr-live-blur-and-trace-2048-points", 2048, 5, {std::pair<std::size_t, double>{0, 2.0}, std::pair<std::size_t, double>{2048, 5.0}}, true, {std::pair<std::size_t, double>{0, 20.0}, std::pair<std::size_t, double>{1536, 4.0}}, 3840},
         })
    {
        const auto cdp = reference(c.name);
        const auto matches = [&](auto& port, const std::function<bool(std::size_t)>& set) {
            for (auto run = 0; run < 2; ++run)
            {
                const auto played = playLive(port, cdp.input, port.latencyFrames(), set, c.name);
                require(played.size() >= c.compared
                        && std::equal(played.begin(), played.begin() + static_cast<std::ptrdiff_t>(c.compared),
                            cdp.outputs[0].begin()),
                    c.name + (run == 0 ? " should match CDP8 exactly as far as its file's end leaves it"
                                       : " should replay after reset"));
                port.reset();
            }
        };
        require(talas::cdp::BlurBlur::latencyFor(c.points, 3, c.maximumBlur)
                == *talas::cdp::PhaseVocoder::latencyFor(c.points, 3)
                    + static_cast<std::size_t>(c.maximumBlur) * static_cast<std::size_t>(
                        *talas::cdp::PhaseVocoder::decimationFor(c.points, 3)),
            c.name + " should wait its maximum blur's frames");
        if (!c.traced)
        {
            talas::cdp::BlurBlur port;
            require(port.prepare(48000.0, c.points, 3, c.blur.front().second, c.maximumBlur), c.name + " should prepare");
            matches(port, [&](std::size_t offset) { return port.setBlur(at(c.blur, offset)); });
        }
        else
        {
            talas::cdp::HiliteBlurTrace port;
            require(port.prepare(48000.0, c.points, 3, c.blur.front().second, c.trace.front().second, c.maximumBlur),
                c.name + " should prepare");
            matches(port, [&](std::size_t offset) { return port.setControls(at(c.blur, offset), at(c.trace, offset)); });
        }
    }
    talas::cdp::BlurBlur refused;
    talas::cdp::HiliteBlurTrace refusedTrace;
    require(!refused.prepare(48000.0, 1024, 3, 5.0, 4) && !refused.prepare(48000.0, 1024, 3, 1.0, 1025)
            && refused.prepare(48000.0, 1024, 3, 4.0, 8) && !refused.setBlur(9.0) && !refused.setBlur(0.4)
            && !refusedTrace.prepare(48000.0, 1024, 3, 4.0, 514.0, 8),
        "blur blur should refuse a blur over its maximum or under 1 and a maximum over 1024, and bltr a trace over the channels");
}

// CDP8's focus focus between `pvoc anal` and `pvoc synth`, with breakpoint
// files that step between two frames' times; with -s, its stability's frames
// late, as far as CDP's end of file leaves it.
void testFocusFocusMatchesCdp()
{
    using Steps = std::vector<std::pair<std::size_t, double>>;
    struct Focused
    {
        std::string name;
        int points;
        int overlap;
        bool pitchwise;
        int bands;
        bool quickSearch;
        int peaks;
        Steps bandwidth;
        Steps low;
        Steps high;
        bool liveLow;
        bool liveHigh;
        bool stable;
        double stability;
        std::size_t compared;
    };
    const auto at = [](const Steps& steps, std::size_t offset) {
        auto value = steps.front().second;
        for (const auto& [from, next] : steps)
            if (offset >= from)
                value = next;
        return value;
    };
    for (const auto& c : {
        Focused{"focus-focus-1-f4-4-peaks", 1024, 3, false, 4, false, 4, {std::pair<std::size_t, double>{0, 0.5}}, {std::pair<std::size_t, double>{0, 5.0}}, {std::pair<std::size_t, double>{0, 24000.0}}, false, false, false, 9.0, 2048},
        Focused{"focus-focus-2-pitchwise", 1024, 3, true, 2, false, 6, {std::pair<std::size_t, double>{0, 1.0}}, {std::pair<std::size_t, double>{0, 5.0}}, {std::pair<std::size_t, double>{0, 24000.0}}, false, false, false, 9.0, 2048},
        Focused{"focus-focus-3-quicksearch", 1024, 3, false, 8, true, 3, {std::pair<std::size_t, double>{0, 0.25}}, {std::pair<std::size_t, double>{0, 5.0}}, {std::pair<std::size_t, double>{0, 24000.0}}, false, false, false, 9.0, 2048},
        Focused{"focus-focus-4-pitchwise-quicksearch-16-peaks", 1024, 3, true, 6, true, 16, {std::pair<std::size_t, double>{0, 0.15}}, {std::pair<std::size_t, double>{0, 5.0}}, {std::pair<std::size_t, double>{0, 24000.0}}, false, false, false, 9.0, 2048},
        Focused{"focus-focus-5-one-peak-3-octaves", 1024, 3, false, 2, false, 1, {std::pair<std::size_t, double>{0, 3.0}}, {std::pair<std::size_t, double>{0, 5.0}}, {std::pair<std::size_t, double>{0, 24000.0}}, false, false, false, 9.0, 2048},
        Focused{"focus-focus-6-largest-bandwidth", 1024, 3, false, 4, false, 2, {std::pair<std::size_t, double>{0, 10.0}}, {std::pair<std::size_t, double>{0, 5.0}}, {std::pair<std::size_t, double>{0, 24000.0}}, false, false, false, 9.0, 2048},
        Focused{"focus-focus-7-bottom-top", 1024, 3, false, 4, false, 5, {std::pair<std::size_t, double>{0, 0.4}}, {std::pair<std::size_t, double>{0, 300.0}}, {std::pair<std::size_t, double>{0, 2500.0}}, false, false, false, 9.0, 2048},
        Focused{"focus-focus-8-top-under-bottom", 1024, 3, false, 4, false, 5, {std::pair<std::size_t, double>{0, 0.4}}, {std::pair<std::size_t, double>{0, 3000.0}}, {std::pair<std::size_t, double>{0, 200.0}}, false, false, false, 9.0, 2048},
        Focused{"focus-focus-9-live-bottom-top", 1024, 3, false, 4, false, 5, {std::pair<std::size_t, double>{0, 0.4}}, {std::pair<std::size_t, double>{0, 100.0}, std::pair<std::size_t, double>{640, 800.0}, std::pair<std::size_t, double>{1344, 200.0}}, {std::pair<std::size_t, double>{0, 4000.0}, std::pair<std::size_t, double>{1024, 1500.0}}, true, true, false, 9.0, 2048},
        Focused{"focus-focus-10-pitchwise-live-bandwidth", 1024, 3, true, 3, false, 4, {std::pair<std::size_t, double>{0, 0.3}, std::pair<std::size_t, double>{704, 2.0}, std::pair<std::size_t, double>{1408, 0.12}}, {std::pair<std::size_t, double>{0, 5.0}}, {std::pair<std::size_t, double>{0, 24000.0}}, false, false, false, 9.0, 2048},
        Focused{"focus-focus-11-live-band-without-peaks", 1024, 3, false, 4, false, 4, {std::pair<std::size_t, double>{0, 0.5}}, {std::pair<std::size_t, double>{0, 100.0}, std::pair<std::size_t, double>{704, 1600.0}, std::pair<std::size_t, double>{1536, 100.0}}, {std::pair<std::size_t, double>{0, 4000.0}, std::pair<std::size_t, double>{704, 1700.0}, std::pair<std::size_t, double>{1536, 4000.0}}, true, true, false, 9.0, 2048},
        Focused{"focus-focus-12-512-points", 512, 3, false, 2, false, 4, {std::pair<std::size_t, double>{0, 0.6}}, {std::pair<std::size_t, double>{0, 5.0}}, {std::pair<std::size_t, double>{0, 24000.0}}, false, false, false, 9.0, 2048},
        Focused{"focus-focus-13-stability-9-is-the-default", 1024, 3, false, 4, false, 4, {std::pair<std::size_t, double>{0, 0.5}}, {std::pair<std::size_t, double>{0, 5.0}}, {std::pair<std::size_t, double>{0, 24000.0}}, false, false, true, 9.0, 2048},
        Focused{"focus-focus-14-stability-2", 1024, 3, false, 4, false, 4, {std::pair<std::size_t, double>{0, 0.5}}, {std::pair<std::size_t, double>{0, 5.0}}, {std::pair<std::size_t, double>{0, 24000.0}}, false, false, true, 2.0, 2048},
        Focused{"focus-focus-15-stability-3", 1024, 3, false, 4, false, 4, {std::pair<std::size_t, double>{0, 0.5}}, {std::pair<std::size_t, double>{0, 5.0}}, {std::pair<std::size_t, double>{0, 24000.0}}, false, false, true, 3.0, 1920},
        Focused{"focus-focus-16-stability-4", 1024, 3, false, 4, false, 4, {std::pair<std::size_t, double>{0, 0.5}}, {std::pair<std::size_t, double>{0, 5.0}}, {std::pair<std::size_t, double>{0, 24000.0}}, false, false, true, 4.0, 1920},
        Focused{"focus-focus-17-stability-9.4", 1024, 3, false, 4, false, 4, {std::pair<std::size_t, double>{0, 0.5}}, {std::pair<std::size_t, double>{0, 5.0}}, {std::pair<std::size_t, double>{0, 24000.0}}, false, false, true, 9.4, 1536},
        Focused{"focus-focus-18-pitchwise-16-peaks-stability-6", 1024, 3, true, 1, false, 16, {std::pair<std::size_t, double>{0, 2.0}}, {std::pair<std::size_t, double>{0, 5.0}}, {std::pair<std::size_t, double>{0, 24000.0}}, false, false, true, 6.0, 1792},
        Focused{"focus-focus-19-stability-31-8192-frame-source", 1024, 3, false, 4, false, 6, {std::pair<std::size_t, double>{0, 0.5}}, {std::pair<std::size_t, double>{0, 5.0}}, {std::pair<std::size_t, double>{0, 24000.0}}, false, false, true, 31.0, 6272},
        Focused{"focus-focus-20-stability-5-live-bandwidth-bottom-top", 1024, 3, false, 4, false, 5, {std::pair<std::size_t, double>{0, 0.25}, std::pair<std::size_t, double>{960, 1.2}}, {std::pair<std::size_t, double>{0, 200.0}, std::pair<std::size_t, double>{512, 1000.0}}, {std::pair<std::size_t, double>{0, 3000.0}, std::pair<std::size_t, double>{1216, 6000.0}}, true, true, true, 5.0, 1792},
        Focused{"focus-focus-21-stability-3-live-band-without-peaks", 1024, 3, false, 4, false, 4, {std::pair<std::size_t, double>{0, 0.5}}, {std::pair<std::size_t, double>{0, 100.0}, std::pair<std::size_t, double>{704, 1600.0}, std::pair<std::size_t, double>{1536, 100.0}}, {std::pair<std::size_t, double>{0, 4000.0}, std::pair<std::size_t, double>{704, 1700.0}, std::pair<std::size_t, double>{1536, 4000.0}}, true, true, true, 3.0, 1920},
        Focused{"focus-focus-22-2048-points-overlap-2-stability-5", 2048, 2, true, 4, true, 5, {std::pair<std::size_t, double>{0, 0.4}}, {std::pair<std::size_t, double>{0, 5.0}}, {std::pair<std::size_t, double>{0, 24000.0}}, false, false, true, 5.0, 1024},
        Focused{"focus-focus-23-256-points-overlap-4-stability-3", 256, 4, false, 3, false, 3, {std::pair<std::size_t, double>{0, 1.5}}, {std::pair<std::size_t, double>{0, 5.0}}, {std::pair<std::size_t, double>{0, 24000.0}}, false, false, true, 3.0, 2032},
         })
    {
        talas::cdp::FocusFocus port;
        require(port.prepare(48000.0, c.points, c.overlap, c.pitchwise, c.bands, c.quickSearch, c.peaks,
                    c.bandwidth.front().second, c.low.front().second, c.high.front().second, c.liveLow, c.liveHigh,
                    c.stable, c.stability),
            c.name + " should prepare");
        require(port.latencyFrames()
                == *talas::cdp::FocusFocus::latencyFor(c.points, c.overlap, c.stable, c.stability),
            c.name + " should wait as its latency helper says");
        const auto set = [&](std::size_t offset) {
            return port.setBandwidth(at(c.bandwidth, offset))
                && port.setFrequencies(at(c.low, offset), at(c.high, offset));
        };
        const auto cdp = reference(c.name);
        for (auto run = 0; run < 2; ++run)
        {
            const auto played = playLive(port, cdp.input, port.latencyFrames(), set, c.name);
            require(played.size() >= c.compared
                    && std::equal(played.begin(), played.begin() + static_cast<std::ptrdiff_t>(c.compared),
                        cdp.outputs[0].begin()),
                c.name + (run == 0 ? " should match CDP8 exactly as far as its file's end leaves it"
                                   : " should replay after reset"));
            port.reset();
        }
    }
    talas::cdp::FocusFocus refused;
    require(!refused.prepare(48000.0, 1024, 3, false, 4, false, 17, 0.5, 5.0, 24000.0, false, false, false, 9.0)
            && !refused.prepare(48000.0, 1024, 3, false, 4, false, 4, 0.05, 5.0, 24000.0, false, false, false, 9.0)
            && !refused.prepare(48000.0, 1024, 3, false, 4, false, 4, 0.5, 5.0, 24000.0, false, false, true, 1.0)
            && refused.prepare(48000.0, 1024, 3, false, 4, false, 4, 0.5, 5.0, 24000.0, true, true, false, 9.0)
            && !refused.setFrequencies(800.0, 800.0) && !refused.setBandwidth(11.0),
        "focus focus should refuse 17 peaks, a bandwidth under 1/12 octave, a stability under 2, a zero-width band and a bandwidth over its limit");
}

// CDP8's pitch tune, pick, chord and chordf between `pvoc anal` and `pvoc
// synth`, reading their data files, with breakpoint files that step between
// two frames' times.
void testPitchTunePickAndChordsMatchCdp()
{
    const auto matches = [](const std::string& name, auto& port, const std::function<bool(std::size_t)>& set) {
        const auto cdp = reference(name);
        require(playLive(port, cdp.input, port.latencyFrames(), set, name) == cdp.outputs[0],
            name + " should match CDP8 exactly, latencyFrames late");
        port.reset();
        require(playLive(port, cdp.input, port.latencyFrames(), set, name) == cdp.outputs[0],
            name + " should replay after reset");
    };
    const auto none = [](std::size_t) { return true; };
    const auto at = [](std::size_t offset, std::size_t from, double before, double after) {
        return offset >= from ? after : before;
    };
    using TuneMode = talas::cdp::PitchTune::Mode;
    const auto tune = [&](const std::string& name, int points, std::string text, TuneMode mode, double focus,
                          double clarity, double trace, bool liveTrace, double bassCut, bool liveBassCut) {
        const auto pitches = talas::cdp::readPitchTemplate(text, mode);
        require(pitches.has_value(), name + " should read its template");
        talas::cdp::PitchTune port;
        require(port.prepare(48000.0, points, 3, *pitches, focus, clarity, trace, liveTrace, bassCut, liveBassCut),
            name + " should prepare");
        return port;
    };
    {
        auto port = tune("pitch-tune-1", 1024, "1310 1960\n", TuneMode::Frequencies, 1, 1, 0, false, 0, false);
        matches("pitch-tune-1", port, none);
    }
    {
        auto port = tune("pitch-tune-2-midi-focus-0.6-clarity-0.4-512-points", 512, "84.5\n91.2\n", TuneMode::Midi,
            0.6, 0.4, 0, false, 0, false);
        matches("pitch-tune-2-midi-focus-0.6-clarity-0.4-512-points", port, none);
    }
    {
        auto port = tune("pitch-tune-1-trace-6-bass-cut-900", 1024, "1310\n1960\n", TuneMode::Frequencies, 0.8,
            0.5, 6, false, 900, false);
        matches("pitch-tune-1-trace-6-bass-cut-900", port, none);
    }
    {
        auto port = tune("pitch-tune-2-static-trace-16-128-points", 128, "86\n", TuneMode::Midi, 1, 0.25, 16, false,
            0, false);
        matches("pitch-tune-2-static-trace-16-128-points", port, none);
    }
    {
        auto port = tune("pitch-tune-1-live-focus-clarity-trace-bass-cut-256-points", 256, "650\n",
            TuneMode::Frequencies, 1, 0.2, 3, true, 9.5, true);
        matches("pitch-tune-1-live-focus-clarity-trace-bass-cut-256-points", port, [&](std::size_t offset) {
            return port.setControls(at(offset, 512, 1, 0.3), at(offset, 768, 0.2, 0.9), at(offset, 1024, 3, 12),
                at(offset, 1280, 9.5, 2000));
        });
    }
    using PickMode = talas::cdp::PitchPick::Mode;
    for (const auto& [name, points, mode, fundamental, stepSize, clarity] : {
             std::tuple{std::string{"pitch-pick-1"}, 1024, PickMode::Harmonics, 1310.0, 0.0, 1.0},
             std::tuple{std::string{"pitch-pick-2-octaves-clarity-0.7-512-points"}, 512, PickMode::Octaves, 163.75,
                 0.0, 0.7},
             std::tuple{std::string{"pitch-pick-3-odd-harmonics-128-points"}, 128, PickMode::OddHarmonics, 655.0, 0.0,
                 1.0},
             std::tuple{std::string{"pitch-pick-5-displaced-minus-350-256-points"}, 256, PickMode::DisplacedHarmonics,
                 1310.0, -350.0, 0.9},
         })
    {
        talas::cdp::PitchPick port;
        require(port.prepare(48000.0, points, 3, mode, fundamental, stepSize, clarity), name + " should prepare");
        matches(name, port, none);
    }
    {
        talas::cdp::PitchPick port;
        require(port.prepare(48000.0, 1024, 3, PickMode::Linear, 400.0, 650.0, 0.4), "pick 4 should prepare");
        matches("pitch-pick-4-linear-live-clarity", port,
            [&](std::size_t offset) { return port.setClarity(at(offset, 1024, 0.4, 1.0)); });
    }
    const auto chordText = [](std::string text) {
        const auto transpositions = talas::cdp::readChordTranspositions(text);
        require(transpositions.has_value(), "chord transpositions should read");
        return *transpositions;
    };
    for (const auto& [name, points, text, bottom, top, lessBody] : {
             std::tuple{std::string{"pitch-chord"}, 1024, std::string{"-12 0 7\n"}, 5.0, 24000.0, false},
             std::tuple{std::string{"pitch-chord-less-body-bottom-300-top-5000-512-points"}, 512,
                 std::string{"-5\n4\n"}, 300.0, 5000.0, true},
             std::tuple{std::string{"pitch-chord-top-3000-alone"}, 1024, std::string{"3 -7\n"}, 5.0, 3000.0, false},
             std::tuple{std::string{"pitch-chord-bottom-2000-alone-128-points"}, 128, std::string{"12\n"}, 2000.0,
                 24000.0, false},
         })
    {
        talas::cdp::PitchChord port;
        require(port.prepare(48000.0, points, 3, chordText(text), bottom, false, top, false, lessBody),
            name + " should prepare");
        matches(name, port, none);
    }
    {
        talas::cdp::PitchChord port;
        require(port.prepare(48000.0, 256, 3, chordText("-12\n0\n7.5\n"), 800.0, true, 6000.0, true, false),
            "live chord should prepare");
        matches("pitch-chord-live-bottom-and-top-256-points", port, [&](std::size_t offset) {
            return port.setBand(at(offset, 768, 800, 3000), at(offset, 1280, 6000, 2000));
        });
    }
    for (const auto& [name, points, pitchwise, bands, quick, text, bottom, top, lessBody] : {
             std::tuple{std::string{"pitch-chordf"}, 1024, false, 4, false, std::string{"-12 0 7\n"}, 5.0, 24000.0,
                 false},
             std::tuple{std::string{"pitch-chordf-pitchwise-quicksearch-less-body-bottom-7-top-8000-512-points"}, 512,
                 true, 2, true, std::string{"-5\n4\n"}, 7.0, 8000.0, true},
             std::tuple{std::string{"pitch-chordf-top-4000-alone-256-points"}, 256, true, 1, false,
                 std::string{"12 19\n"}, 5.0, 4000.0, false},
         })
    {
        talas::cdp::PitchChordFormants port;
        require(port.prepare(48000.0, points, 3, pitchwise, bands, quick, chordText(text), bottom, false, top, false,
                    lessBody),
            name + " should prepare");
        matches(name, port, none);
    }
    {
        talas::cdp::PitchChordFormants port;
        require(port.prepare(48000.0, 1024, 3, false, 6, false, chordText("7\n-3\n"), 6.0, true, 9000.0, true, false),
            "live chordf should prepare");
        matches("pitch-chordf-live-bottom-and-top", port, [&](std::size_t offset) {
            return port.setBand(at(offset, 1024, 6, 1200), at(offset, 1536, 9000, 4000));
        });
    }
    talas::cdp::PitchPick refusedPick;
    talas::cdp::PitchTune refusedTune;
    require(!refusedPick.prepare(48000.0, 1024, 3, PickMode::Harmonics, 5.0, 0.0, 1.0)
            && !refusedPick.prepare(48000.0, 1024, 3, PickMode::Harmonics, 440.0, 0.0, 1.5)
            && !talas::cdp::readChordTranspositions("").has_value()
            && !talas::cdp::readChordTranspositions("120\n").has_value()
            && !refusedTune.prepare(48000.0, 1024, 3, std::vector<double>{30000.0}, 1, 1, 0, false, 0, false),
        "pitch pick, chord and tune should refuse a fundamental under 10 Hz, a clarity over 1, no or too wide transpositions and a pitch over half the sample rate");
}

// CDP8's repitch getpitch over a whole sound, and pitch altharms, octmove
// and spec bare reading the pitch it finds, between `pvoc anal` and `pvoc
// synth`.
void testGetPitchAndPitchFileProcessesMatchCdp()
{
    // The pitched source getpitch's references were made from.
    const auto source = reference("pitch-altharms-1-256-points").input;
    struct Options
    {
        double tuning = talas::cdp::GetPitchSettings::defaultTuningRange;
        int windows = talas::cdp::GetPitchSettings::defaultMinimumPitchedWindows;
        double noise = talas::cdp::GetPitchSettings::defaultSignalToNoise;
        int harmonics = talas::cdp::GetPitchSettings::defaultHarmonicsToConfirm;
        double lowest = talas::cdp::GetPitchSettings::minimumPitch;
        double highest = 0.0;
        bool alternative = false;
        bool keepUnpitched = false;
    };
    const auto settings = [](const Options& options) {
        talas::cdp::GetPitchSettings pitch;
        pitch.tuningRange = options.tuning;
        pitch.minimumPitchedWindows = options.windows;
        pitch.signalToNoise = options.noise;
        pitch.harmonicsToConfirm = options.harmonics;
        pitch.lowestPitch = options.lowest;
        if (options.highest > 0.0)
            pitch.highestPitch = options.highest;
        pitch.alternativeMethod = options.alternative;
        pitch.keepUnpitched = options.keepUnpitched;
        return pitch;
    };
    const auto keep = [] {
        Options options;
        options.keepUnpitched = true;
        return options;
    }();
    Options tuned;
    tuned.tuning = 0.5;
    tuned.windows = 4;
    tuned.noise = 60.0;
    tuned.harmonics = 3;
    tuned.lowest = 200.0;
    tuned.highest = 1500.0;
    Options sparse;
    sparse.windows = 0;
    sparse.noise = 40.0;
    sparse.harmonics = 2;
    sparse.keepUnpitched = true;
    Options alternative;
    alternative.tuning = 2.0;
    alternative.windows = 3;
    alternative.noise = 70.0;
    alternative.harmonics = 6;
    alternative.lowest = 200.0;
    alternative.highest = 900.0;
    alternative.alternative = true;
    alternative.keepUnpitched = true;
    for (const auto& [name, points, overlap, options] : {
             std::tuple{std::string{"repitch-getpitch-1-256-points"}, 256, 3, Options{}},
             std::tuple{std::string{"repitch-getpitch-1-256-points-keep-unpitched"}, 256, 3, keep},
             std::tuple{std::string{"repitch-getpitch-1-256-points-options"}, 256, 3, tuned},
             std::tuple{std::string{"repitch-getpitch-1-256-points-g0-s40-n2-keep-unpitched"}, 256, 3, sparse},
             std::tuple{std::string{"repitch-getpitch-1-256-points-overlap-4"}, 256, 4, Options{}},
             std::tuple{std::string{"repitch-getpitch-1-512-points"}, 512, 3, Options{}},
             std::tuple{std::string{"repitch-getpitch-1-512-points-alternative-keep-unpitched"}, 512, 3, alternative},
             std::tuple{std::string{"repitch-getpitch-1-1024-points"}, 1024, 3, Options{}},
         })
    {
        const auto pitches = talas::cdp::getPitch(source, 48000.0, points, overlap, settings(options));
        const auto cdp = reference(name).input;
        require(pitches.has_value() && pitches->size() == cdp.size()
                && std::equal(pitches->begin(), pitches->end(), cdp.begin(),
                    [](float ours, double theirs) { return ours == static_cast<float>(theirs); }),
            name + " should find CDP8's pitches exactly");
    }
    require(!talas::cdp::getPitch(source, 48000.0, 1024, 3, [] {
        talas::cdp::GetPitchSettings pitch;
        pitch.lowestPitch = 3000.0;
        pitch.highestPitch = 2000.0;
        return pitch;
    }()).has_value(),
        "getpitch should refuse a highest pitch under its lowest");
    const auto matches = [](const std::string& name, auto& port, const std::function<bool(std::size_t)>& set) {
        const auto cdp = reference(name);
        require(playLive(port, cdp.input, port.latencyFrames(), set, name) == cdp.outputs[0],
            name + " should match CDP8 exactly, latencyFrames late");
        port.reset();
        require(playLive(port, cdp.input, port.latencyFrames(), set, name) == cdp.outputs[0],
            name + " should replay after reset");
    };
    const auto none = [](std::size_t) { return true; };
    using AltMode = talas::cdp::PitchAltHarmonics::Mode;
    for (const auto& [name, points, mode, lessBody, options] : {
             std::tuple{std::string{"pitch-altharms-1-256-points"}, 256, AltMode::DeleteOdd, false, Options{}},
             std::tuple{std::string{"pitch-altharms-2-256-points-less-body"}, 256, AltMode::DeleteEven, true, Options{}},
             std::tuple{std::string{"pitch-altharms-1-512-points-keep-unpitched"}, 512, AltMode::DeleteOdd, false, keep},
         })
    {
        const auto pitches = talas::cdp::getPitch(reference(name).input, 48000.0, points, 3, settings(options));
        require(pitches.has_value(), name + " should find its pitch");
        talas::cdp::PitchAltHarmonics port;
        require(port.prepare(48000.0, points, 3, *pitches, mode, lessBody), name + " should prepare");
        matches(name, port, none);
    }
    using OctMode = talas::cdp::PitchOctaveMove::Mode;
    for (const auto& [name, points, mode, transposition, options] : {
             std::tuple{std::string{"pitch-octmove-1-256-points"}, 256, OctMode::Up, 2, Options{}},
             std::tuple{std::string{"pitch-octmove-2-512-points-ratio-3"}, 512, OctMode::Down, 3, Options{}},
         })
    {
        const auto pitches = talas::cdp::getPitch(reference(name).input, 48000.0, points, 3, settings(options));
        require(pitches.has_value(), name + " should find its pitch");
        talas::cdp::PitchOctaveMove port;
        require(port.prepare(48000.0, points, 3, *pitches, mode, transposition, 0.0), name + " should prepare");
        matches(name, port, none);
    }
    {
        const std::string name = "pitch-octmove-3-256-points-live-bass-boost-keep-unpitched";
        const auto pitches = talas::cdp::getPitch(reference(name).input, 48000.0, 256, 3, settings(keep));
        require(pitches.has_value(), name + " should find its pitch");
        talas::cdp::PitchOctaveMove port;
        require(port.prepare(48000.0, 256, 3, *pitches, OctMode::DownBassBoost, 2, 0.0), name + " should prepare");
        matches(name, port, [&](std::size_t offset) { return port.setBassBoost(offset >= 4096 ? 50.0 : 0.0); });
    }
    // Spec bare from CDP's own pitch files.
    const auto pitchFile = [](const std::string& name) {
        const auto values = reference(name).input;
        return std::vector<float>(values.begin(), values.end());
    };
    for (const auto& [name, file, points, lessBody] : {
             std::tuple{std::string{"spec-bare-128-points"}, std::string{"pitchfile-128-points"}, 128, false},
             std::tuple{std::string{"spec-bare-less-body-128-points"}, std::string{"pitchfile-128-points"}, 128, true},
             std::tuple{std::string{"spec-bare-unpitched-marked-128-points"},
                 std::string{"pitchfile-128-points-unpitched-marked"}, 128, false},
             std::tuple{std::string{"spec-bare"}, std::string{"pitchfile-1024-points"}, 1024, false},
             std::tuple{std::string{"spec-bare-less-body-256-points"}, std::string{"pitchfile-256-points"}, 256, true},
         })
    {
        talas::cdp::SpecBare port;
        require(port.prepare(48000.0, points, 3, pitchFile(file), lessBody), name + " should prepare");
        matches(name, port, none);
    }
}

// CDP8's spec clean and strange invert, which read a whole analysis file
// first (spec clean's noise and good-signal files, analyses of samples;
// strange invert's own input), and focus freeze, which reads ahead as far
// as its data says, between `pvoc anal` and `pvoc synth`.
void testWholeSoundAndFreezeProcessesMatchCdp()
{
    const auto plays = [](const std::string& name, auto& port, const std::vector<double>& input,
                           const std::vector<double>& cdp) {
        require(playLive(port, input, port.latencyFrames(), [](std::size_t) { return true; }, name) == cdp,
            name + " should match CDP8 exactly, latencyFrames late");
        port.reset();
        require(playLive(port, input, port.latencyFrames(), [](std::size_t) { return true; }, name) == cdp,
            name + " should replay after reset");
    };
    using CleanMode = talas::cdp::SpecClean::Mode;
    for (const auto& [name, points, mode, value, gain] : {
             std::tuple{std::string{"spec-clean-1-from-time-256-points"}, 256, CleanMode::FromTime, 0.005, 2.0},
             std::tuple{std::string{"spec-clean-2-anywhere-gain-4"}, 1024, CleanMode::Anywhere, 0.0, 4.0},
             std::tuple{std::string{"spec-clean-3-filtering-2000-hz-256-points"}, 256, CleanMode::Filtering, 2000.0, 2.0},
         })
    {
        const auto cdp = reference(name, 2);
        talas::cdp::SpecClean port;
        require(port.prepare(48000.0, points, 3, mode, cdp.outputs[0], {}, value, gain), name + " should prepare");
        plays(name, port, cdp.input, cdp.outputs[1]);
    }
    {
        const std::string name = "spec-clean-4-comparing-256-points";
        const auto cdp = reference(name, 3);
        talas::cdp::SpecClean port;
        require(port.prepare(48000.0, 256, 3, CleanMode::Comparing, cdp.outputs[0], cdp.outputs[1], 0.0, 2.0),
            name + " should prepare");
        plays(name, port, cdp.input, cdp.outputs[2]);
        // A good signal that takes every channel above its level leaves
        // nothing to clean, which CDP refuses.
        require(!port.prepare(48000.0, 256, 3, CleanMode::Comparing, std::vector<double>(64, 0.0),
                    std::vector<double>(64, 0.5), 0.0, 1.0),
            "spec clean should refuse a good signal that leaves no channel to clean");
    }
    {
        const auto cdp = reference("spec-clean-1-from-time-256-points", 2);
        talas::cdp::SpecClean port;
        require(!port.prepare(48000.0, 256, 3, CleanMode::Anywhere, cdp.outputs[0], {}, 0.0, 41.0)
                && !port.prepare(48000.0, 256, 3, CleanMode::Filtering, cdp.outputs[0], {}, 4.0, 2.0)
                && !port.prepare(48000.0, 256, 3, CleanMode::Filtering, cdp.outputs[0], {}, 23999.0, 2.0)
                && !port.prepare(48000.0, 256, 3, CleanMode::FromTime, cdp.outputs[0], {}, -1.0, 2.0)
                && !port.prepare(48000.0, 256, 3, CleanMode::FromTime, {}, {}, 0.0, 2.0)
                && !port.prepared(),
            "spec clean should refuse a gain over 40, frequencies outside 5 Hz to half the rate less half a "
            "channel, a negative skip time and no noise");
    }
    using InvertMode = talas::cdp::StrangeInvert::Mode;
    for (const auto& [name, points, mode] : {
             std::tuple{std::string{"strange-invert-1"}, 1024, InvertMode::Normal},
             std::tuple{std::string{"strange-invert-2-keep-amplitude-256-points"}, 256, InvertMode::KeepAmplitude},
         })
    {
        const auto cdp = reference(name);
        talas::cdp::StrangeInvert port;
        require(port.prepare(48000.0, points, 3, mode, cdp.input), name + " should prepare");
        plays(name, port, cdp.input, cdp.outputs[0]);
    }
    using FreezeMode = talas::cdp::FocusFreeze::Mode;
    for (const auto& [name, points, mode, data, lookahead] : {
             std::tuple{std::string{"focus-freeze-1-amplitudes-256-points"}, 256, FreezeMode::Amplitudes,
                 std::string{"a0.004 0.02 b0.03 a0.05"}, 15},
             std::tuple{std::string{"focus-freeze-2-frequencies-before-90-windows-256-points"}, 256,
                 FreezeMode::Frequencies, std::string{"b0.06"}, 90},
             std::tuple{std::string{"focus-freeze-3-both-8192-frame-source"}, 1024, FreezeMode::Both,
                 std::string{"0.01 b0.05\na0.1"}, 15},
             // CDP8's rationalise_frztimes loses the a0.012 freeze.
             std::tuple{std::string{"focus-freeze-3-both-cdp-drops-a-freeze-256-points"}, 256, FreezeMode::Both,
                 std::string{"0.004 0.008 a0.012"}, 0},
         })
    {
        const auto cdp = reference(name);
        talas::cdp::FocusFreeze port;
        require(port.prepare(48000.0, points, 3, mode, data), name + " should prepare");
        const auto spacing = static_cast<std::size_t>(*talas::cdp::PhaseVocoder::decimationFor(points, 3));
        require(port.latencyFrames() == *talas::cdp::PhaseVocoder::latencyFor(points, 3) + lookahead * spacing
                && talas::cdp::FocusFreeze::latencyFor(48000.0, points, 3, data) == port.latencyFrames(),
            name + " should wait for the windows its data reads ahead");
        plays(name, port, cdp.input, cdp.outputs[0]);
    }
    {
        talas::cdp::FocusFreeze port;
        require(!port.prepare(48000.0, 1024, 3, FreezeMode::Both, "0.1 0.2")
                && !port.prepare(48000.0, 1024, 3, FreezeMode::Both, "")
                && !port.prepare(48000.0, 1024, 3, FreezeMode::Both, "a0.2 b0.4")
                && !port.prepare(48000.0, 1024, 3, FreezeMode::Both, "a0.2 b0.1")
                && !port.prepare(48000.0, 1024, 3, FreezeMode::Both, "a0.2 a0.2001")
                && !port.prepare(48000.0, 1024, 3, FreezeMode::Both, "x0.2")
                && !port.prepare(48000.0, 1024, 3, FreezeMode::Both, "b 0.2") && !port.prepared(),
            "focus freeze should refuse data without a or b, a before b, times out of order or in one window, "
            "and unknown flags, as CDP does");
        // A b time further ahead than a stream waits is cut to the most it
        // waits.
        require(talas::cdp::FocusFreeze::latencyFor(48000.0, 1024, 3, "b100")
                == *talas::cdp::PhaseVocoder::latencyFor(1024, 3) + std::size_t{1024} * 128,
            "focus freeze should wait at most 1024 windows");
    }
}

// CDP8's filter varibank2 and its newer tremolo on recorded references, with
// breakpoint files that step at the blocks where CDP reads them.
void testVaryingBank2AndTremoloFineMatchCdp()
{
    using Pitches = talas::cdp::FilterPitches;
    const auto at = [](std::size_t offset, std::vector<std::pair<std::size_t, double>> steps) {
        auto value = steps.front().second;
        for (const auto& [from, next] : steps)
            if (offset >= from)
                value = next;
        return value;
    };
    const auto matches = [](const std::string& name, auto& port, const std::function<bool(std::size_t)>& set) {
        const auto cdp = reference(name);
        require(playLive(port, cdp.input, port.latencyFrames(), set, name) == cdp.outputs[0],
            name + " should match CDP8 exactly");
        port.reset();
        require(playLive(port, cdp.input, port.latencyFrames(), set, name) == cdp.outputs[0],
            name + " should replay after reset");
    };
    struct Bank
    {
        std::string name;
        std::string text;
        Pitches pitches;
        double q;
        double gain;
        bool doubleFiltering;
        std::size_t bufferFrames;
        std::vector<std::pair<std::size_t, double>> qSteps;
    };
    for (const auto& c : {
             Bank{"filter-varibank2-1-gliding-pitches-and-partials", "; two pitches gliding\n0 300 1 1500 0.5\n\n0.015 420 0.8 1300 -6dB\n0.03 380 1 1650 0.3\n# partials\n0 1 1 2 0.5 3 0.25\n0.02 1 1 2.5 0.3 3.5 0.4\n0.04 1 0.6 2 0.7 4 0.2\n", Pitches::Hertz, 20.0, 1.0, false,
                 talas::cdp::VaryingBank2::defaultBufferFrames, {{0, 20.0}}},
             Bank{"filter-varibank2-2-midi-double", "0 60 1 67 0.5\n0.02 62 0.7 65 -3dB\n#\n0 1 1 1.5 0.5\n0.01 1 1 2 0.5\n0.03 1 0.5 3 0.8\n", Pitches::Midi, 8.0, 0.8, true,
                 talas::cdp::VaryingBank2::defaultBufferFrames, {{0, 8.0}}},
             Bank{"filter-varibank2-1-live-q", "; two pitches gliding\n0 300 1 1500 0.5\n\n0.015 420 0.8 1300 -6dB\n0.03 380 1 1650 0.3\n# partials\n0 1 1 2 0.5 3 0.25\n0.02 1 1 2.5 0.3 3.5 0.4\n0.04 1 0.6 2 0.7 4 0.2\n", Pitches::Hertz, 20.0, 1.0, false,
                 talas::cdp::VaryingBank2::defaultBufferFrames, {{0, 30.0}, {1024, 8.0}, {1664, 50.0}}},
             Bank{"filter-varibank2-1-first-partial-1.5", "0 200 1\n0.03 300 0\n#\n0 1.5 1 2 0.5 3 0\n0.025 2 0.8 3 0.5 4 0.25\n", Pitches::Hertz, 30.0, 1.0, false,
                 talas::cdp::VaryingBank2::defaultBufferFrames, {{0, 30.0}}},
             Bank{"filter-varibank2-1-clipping-gain-40", "0 440 1\n0.012 880 1\n#\n0 1 1\n", Pitches::Hertz, 50.0, 40.0, false,
                 talas::cdp::VaryingBank2::defaultBufferFrames, {{0, 50.0}}},
             Bank{"filter-varibank2-1-live-q-2560-frame-buffers", "0 250 1 900 0.6\n0.05 350 1 700 0.4\n0.1 300 0.5 1000 1\n#\n0 1 1 2 0.5\n0.06 1 1 3 0.3\n0.12 1 0.8 2.5 0.6\n", Pitches::Hertz, 20.0, 1.0, false, 2560,
                 {{0, 15.0}, {2560, 25.0}, {5376, 6.0}}},
             Bank{"filter-varibank2-1-partial-past-nyquist-after-test", "0 7000 1\n0.01 9000 1\n#\n0 1 1 3 1\n0.005 1 1 3 1\n", Pitches::Hertz, 25.0, 1.0, false,
                 talas::cdp::VaryingBank2::defaultBufferFrames, {{0, 25.0}}},
         })
    {
        const auto bands = talas::cdp::readVaryingBank2Data(c.text, c.pitches, 48000.0);
        require(bands.has_value(), c.name + " should read its data");
        talas::cdp::VaryingBank2 port;
        require(port.prepare(48000.0, *bands, c.q, c.gain, c.doubleFiltering, c.bufferFrames), c.name + " should prepare");
        matches(c.name, port, [&](std::size_t offset) { return port.setQ(at(offset, c.qSteps)); });
    }
    require(!talas::cdp::readVaryingBank2Data("0 440 1\n0.1 500 1\n", Pitches::Hertz, 48000.0).has_value()
            && !talas::cdp::readVaryingBank2Data("0.1 440 1\n#\n0 1 1\n", Pitches::Hertz, 48000.0).has_value(),
        "varibank2 should refuse data without a # line and times that do not start at 0");
    using Mode = talas::cdp::TremoloFine::Mode;
    struct Tremolo
    {
        std::string name;
        Mode mode;
        int fineness;
        bool liveFrequency;
        std::vector<std::pair<std::size_t, double>> frequency;
        std::vector<std::pair<std::size_t, double>> depth;
        std::vector<std::pair<std::size_t, double>> gain;
    };
    for (const auto& c : {
             Tremolo{"tremolo-new-1-75-hz-depth-0.6-gain-0.9", Mode::Linear, 1, false, {{0, 75.0}}, {{0, 0.6}},
                 {{0, 0.9}}},
             Tremolo{"tremolo-new-1-fineness-8-140-hz-depth-1-gain-1", Mode::Linear, 8, false, {{0, 140.0}},
                 {{0, 1.0}}, {{0, 1.0}}},
             Tremolo{"tremolo-new-1-fineness-100-31.25-hz-depth-0.35-gain-0.99", Mode::Linear, 100, false,
                 {{0, 31.25}}, {{0, 0.35}}, {{0, 0.99}}},
             Tremolo{"tremolo-new-1-live-frequency-depth-gain-fineness-3", Mode::Linear, 3, true,
                 {{0, 60.0}, {768, 0.0}, {1280, 210.5}, {2560, 500.0}}, {{0, 1.0}, {1024, 0.25}, {3072, 0.0}},
                 {{0, 0.8}, {1536, 1.0}, {3584, 0.3}}},
             Tremolo{"tremolo-new-2-fixed-90-hz-depth-0.8-gain-0.7-fineness-2", Mode::Logarithmic, 2, false,
                 {{0, 90.0}}, {{0, 0.8}}, {{0, 0.7}}},
             Tremolo{"tremolo-new-2-live-frequency-depth-gain-fineness-4", Mode::Logarithmic, 4, true,
                 {{0, 7.0}, {1024, 31.0}, {3072, 113.7}, {5120, 260.0}}, {{0, 0.9}, {2048, 0.4}, {6144, 1.0}},
                 {{0, 0.85}, {4096, 0.6}}},
             // CDP raises the 5 Hz to one over its 2048-frame file's duration.
             Tremolo{"tremolo-new-2-live-frequency-below-file-floor", Mode::Logarithmic, 1, true,
                 {{0, 1.0 / (2048.0 / 48000.0)}, {1024, 70.0}}, {{0, 1.0}}, {{0, 1.0}}},
         })
    {
        talas::cdp::TremoloFine port;
        require(port.prepare(48000.0, c.mode, c.frequency.front().second, c.depth.front().second,
                    c.gain.front().second, c.fineness, c.liveFrequency),
            c.name + " should prepare");
        matches(c.name, port, [&](std::size_t offset) {
            return port.setControls(at(offset, c.frequency), at(offset, c.depth), at(offset, c.gain));
        });
    }
    talas::cdp::TremoloFine refused;
    require(!refused.prepare(48000.0, Mode::Linear, 600.0, 0.5, 1.0, 1, false)
            && !refused.prepare(48000.0, Mode::Linear, 10.0, 0.5, 1.0, 101, false)
            && !refused.prepare(48000.0, Mode::Logarithmic, 0.0, 0.5, 1.0, 1, true),
        "the newer tremolo should refuse a frequency over 500 Hz, a fineness over 100 and a live logarithmic 0 Hz");
}

// CDP8's distmore double, clip clip 2 and quirk quirk 1 on recorded
// references, latencyFrames() late, as far as CDP writes (distmore and quirk
// drop their last partial waveset or half-waveset; clip leaves its last
// half-waveset unclipped, which the stream clips once it is cut) and outside
// the (half-)wavesets longer than the 2047-frame wait, which the stream cuts.
void testWavesetDistortionsMatchCdp()
{
    enum class Kind
    {
        Double,
        Clip,
        Quirk,
    };
    struct Waveset
    {
        std::string name;
        Kind kind;
        double parameter;
        std::size_t compared;
        // The first and last frames of each (half-)waveset the stream cuts.
        std::vector<std::pair<std::size_t, std::size_t>> cut;
    };
    for (const auto& c : {
             Waveset{"distmore-double-1", Kind::Double, 1.0, 2022, {}},
             Waveset{"distmore-double-1-zero-runs", Kind::Double, 1.0, 4071, {}},
             Waveset{"distmore-double-1-negative-zero-runs", Kind::Double, 1.0, 4071, {}},
             Waveset{"distmore-double-1-long-cycles", Kind::Double, 1.0, 23957,
                 {{6201, 8600}, {8601, 11000}, {11001, 18199}}},
             Waveset{"clip-2-fraction-0.5", Kind::Clip, 0.5, 2033, {}},
             Waveset{"clip-2-fraction-0.13-zero-runs", Kind::Clip, 0.13, 4085, {}},
             Waveset{"clip-2-fraction-0.8-negative-zero-runs", Kind::Clip, 0.8, 4085, {}},
             // A fraction of 1 clips nothing, so CDP's unclipped last
             // half-waveset matches too.
             Waveset{"clip-2-fraction-1", Kind::Clip, 1.0, 2048, {}},
             Waveset{"clip-2-fraction-0.3-long-cycles", Kind::Clip, 0.3, 23957, {{11001, 15800}, {15801, 18199}}},
             Waveset{"quirk-1-exponent-0.5", Kind::Quirk, 0.5, 2033, {}},
             Waveset{"quirk-1-exponent-3-zero-runs", Kind::Quirk, 3.0, 4085, {}},
             Waveset{"quirk-1-exponent-0.01-negative-zero-runs", Kind::Quirk, 0.01, 4085, {}},
             Waveset{"quirk-1-exponent-100", Kind::Quirk, 100.0, 2033, {}},
             Waveset{"quirk-1-exponent-0.25-long-cycles", Kind::Quirk, 0.25, 23957, {{11000, 15800}, {15801, 18199}}},
         })
    {
        const auto cdp = reference(c.name);
        const auto matches = [&](auto& port) {
            require(port.latencyFrames() == 2047, c.name + " should wait 2047 frames");
            const auto output = playLagging(port, cdp.input, port.latencyFrames(), c.name);
            for (std::size_t frame = 0; frame < c.compared; ++frame)
            {
                const auto cut = std::ranges::any_of(c.cut, [&](const auto& range) {
                    return frame >= range.first && frame <= range.second;
                });
                require(cut || output[frame] == cdp.outputs[0][frame],
                    c.name + " should match CDP8 exactly, latencyFrames late, at frame " + std::to_string(frame));
            }
            port.reset();
            require(playLagging(port, cdp.input, port.latencyFrames(), c.name) == output,
                c.name + " should replay after reset");
        };
        switch (c.kind)
        {
        case Kind::Double:
        {
            talas::cdp::DistmoreDouble port;
            require(port.prepare(static_cast<int>(c.parameter)), c.name + " should prepare");
            matches(port);
            break;
        }
        case Kind::Clip:
        {
            talas::cdp::ClipWaveset port;
            require(port.prepare(c.parameter), c.name + " should prepare");
            matches(port);
            break;
        }
        case Kind::Quirk:
        {
            talas::cdp::QuirkWaveset port;
            require(port.prepare(c.parameter), c.name + " should prepare");
            matches(port);
            break;
        }
        }
    }
}

void testWavesetDistortionsRejectWithoutChange()
{
    talas::cdp::DistmoreDouble distmoreDouble;
    talas::cdp::ClipWaveset clipWaveset;
    talas::cdp::QuirkWaveset quirkWaveset;
    const auto input = ramp(256);
    std::vector<double> output(256);
    require(!distmoreDouble.prepare(2) && !distmoreDouble.prepare(0)
            && !clipWaveset.prepare(0.0) && !clipWaveset.prepare(1.01)
            && !clipWaveset.prepare(std::numeric_limits<double>::quiet_NaN())
            && !quirkWaveset.prepare(0.009) && !quirkWaveset.prepare(100.1)
            && !distmoreDouble.prepared() && !clipWaveset.prepared() && !quirkWaveset.prepared()
            && !distmoreDouble.process(input, output),
        "the waveset distortions should refuse a multiplier but 1, a fraction outside 0 (exclusive) to 1, an exponent outside 0.01 to 100 and to play unprepared");
    require(clipWaveset.prepare(0.5)
            && !clipWaveset.process(std::span{input}.first(128), std::span{output}.first(127))
            && clipWaveset.process(std::span{input}.first(128), std::span{output}.first(128)),
        "clip should refuse spans of different sizes");
    auto twin = clipWaveset;
    std::vector<double> bad(std::span{input}.subspan(128).begin(), std::span{input}.subspan(128).end());
    bad[3] = std::numeric_limits<double>::quiet_NaN();
    require(!clipWaveset.process(bad, std::span{output}.subspan(128)),
        "clip should refuse a non-finite input");
    std::vector<double> twinOutput(128);
    require(clipWaveset.process(std::span{input}.subspan(128), std::span{output}.subspan(128))
            && twin.process(std::span{input}.subspan(128), twinOutput)
            && std::ranges::equal(std::span{output}.subspan(128), twinOutput),
        "a refused block should leave clip unchanged");
}

// CDP8's rmverb on recorded references at 16 and 44.1 kHz, each room with
// its options, streamed in uneven blocks; a mono output writes both.
void testRoomReverbMatchesCdp()
{
    using talas::cdp::RoomReflection;
    using talas::cdp::RoomSize;
    // readtaps on the hand-written file at 16 kHz: its remark and empty line
    // skipped, 0.00001 s, under a sample, taken as 0, the repeats of 0.0179
    // and 0.0241 dropped (the second despite its amplitude of 5), the words
    // after 0.0302 ignored and +0.0377 read.
    const auto reflections = talas::cdp::readRoomReflections(
        "; hand-written early reflections for rmverb -e\n0.00001 0.9\n0.0123 0.6\n   0.0179  0.45\n\n0.0179 0.3\n0.0241 0.5\n0.0241 5.0\n0.0302 0.35 and a remark\n+0.0377 0.2\n0.0466 0.25",
        16000.0);
    require(reflections.has_value()
            && *reflections
                == std::vector<RoomReflection>{{0.0, 0.9}, {0.0123, 0.6}, {0.0179, 0.45}, {0.0241, 0.5},
                    {0.0302, 0.35}, {0.0377, 0.2}, {0.0466, 0.25}},
        "rmverb's reflections should read as its readtaps reads them");
    struct Room
    {
        std::string name;
        double sampleRate;
        std::size_t channels;
        talas::cdp::RoomReverbSettings settings;
    };
    for (const auto& c : {
             Room{"rmverb-small-room-16k", 16000.0, 2,
                 {.room = RoomSize::small, .reverbGain = 0.5, .mix = 0.5, .feedback = 0.5, .absorbHz = 4200.0}},
             Room{"rmverb-medium-room-double-damping-low-cut-16k", 16000.0, 2,
                 {.room = RoomSize::medium, .reverbGain = 0.6, .mix = 0.4, .feedback = 0.7, .absorbHz = 2500.0,
                     .lowCutHz = 150.0, .doubleDamping = true}},
             Room{"rmverb-large-room-predelay-30-high-cut-lowpass-16k", 16000.0, 2,
                 {.room = RoomSize::large, .reverbGain = 0.8, .mix = 0.3, .feedback = 0.6, .absorbHz = 2600.0,
                     .lowpassHz = 3000.0, .predelayMilliseconds = 30.0, .highCutHz = 5000.0}},
             Room{"rmverb-small-room-double-damping-no-absorb-cuts-all-wet-16k", 16000.0, 2,
                 {.room = RoomSize::small, .reverbGain = 1.0, .mix = 0.0, .feedback = 0.9, .absorbHz = 0.0,
                     .lowCutHz = 200.0, .highCutHz = 3000.0, .doubleDamping = true}},
             Room{"rmverb-medium-room-mono-out-lowpass-16k", 16000.0, 1,
                 {.room = RoomSize::medium, .reverbGain = 0.5, .mix = 0.5, .feedback = 0.5, .absorbHz = 2500.0,
                     .lowpassHz = 2000.0, .monoOutput = true}},
             Room{"rmverb-large-room-double-damping-no-absorb-feedback-1-16k", 16000.0, 2,
                 {.room = RoomSize::large, .reverbGain = 0.7, .mix = 0.6, .feedback = 1.0, .absorbHz = 0.0,
                     .doubleDamping = true}},
             Room{"rmverb-small-room-predelay-0-no-feedback-float-16k", 16000.0, 2,
                 {.room = RoomSize::small, .reverbGain = 0.5, .mix = 0.5, .feedback = 0.0, .absorbHz = 4200.0,
                     .predelayMilliseconds = 0.0}},
             Room{"rmverb-medium-room-reflections-file-predelay-15-16k", 16000.0, 2,
                 {.room = RoomSize::medium, .reverbGain = 0.9, .mix = 0.3, .feedback = 0.8, .absorbHz = 3000.0,
                     .lowpassHz = 4000.0, .predelayMilliseconds = 15.0,
                     .reflections = reflections.value_or(std::vector<RoomReflection>{})}},
             Room{"rmverb-large-room-cuts-predelay-5-44k", 44100.0, 2,
                 {.room = RoomSize::large, .reverbGain = 0.9, .mix = 0.25, .feedback = 0.8, .absorbHz = 4000.0,
                     .lowpassHz = 6000.0, .predelayMilliseconds = 5.0, .lowCutHz = 100.0, .highCutHz = 8000.0}},
         })
    {
        const auto cdp = reference(c.name, c.channels);
        talas::cdp::RoomReverb reverb;
        require(reverb.prepare(c.sampleRate, c.settings), c.name + " should prepare");
        const auto play = [&] {
            std::vector<double> left(cdp.input.size());
            std::vector<double> right(cdp.input.size());
            std::size_t offset = 0;
            for (std::size_t block = 3; offset < cdp.input.size(); block = block * 2 + 7)
            {
                const auto count = std::min(block, cdp.input.size() - offset);
                require(reverb.process(std::span{cdp.input}.subspan(offset, count),
                            std::span{left}.subspan(offset, count),
                            std::span{right}.subspan(offset, count)),
                    c.name + " should process");
                offset += count;
            }
            return std::pair{left, right};
        };
        const auto [left, right] = play();
        require(left == cdp.outputs[0] && right == cdp.outputs[c.channels - 1],
            c.name + " should match CDP8's rmverb exactly");
        reverb.reset();
        require(play() == std::pair{left, right}, c.name + " should replay after reset");
    }
}

void testRoomReverbRejectsWithoutChange()
{
    talas::cdp::RoomReverb reverb;
    for (const auto& change : std::vector<void (*)(talas::cdp::RoomReverbSettings&)>{
             [](talas::cdp::RoomReverbSettings& s) { s.room = static_cast<talas::cdp::RoomSize>(4); },
             [](talas::cdp::RoomReverbSettings& s) { s.reverbGain = 0.0; },
             [](talas::cdp::RoomReverbSettings& s) { s.reverbGain = 1.5; },
             [](talas::cdp::RoomReverbSettings& s) { s.mix = 1.01; },
             [](talas::cdp::RoomReverbSettings& s) { s.feedback = -0.1; },
             [](talas::cdp::RoomReverbSettings& s) { s.absorbHz = 0.35 * 16000.0 + 1.0; },
             [](talas::cdp::RoomReverbSettings& s) { s.lowpassHz = -1.0; },
             [](talas::cdp::RoomReverbSettings& s) { s.lowCutHz = 500.0; s.highCutHz = 400.0; },
             [](talas::cdp::RoomReverbSettings& s) { s.predelayMilliseconds = 10001.0; },
             // The first reflection within a sample of the direct sound.
             [](talas::cdp::RoomReverbSettings& s) { s.predelayMilliseconds = 0.01; },
             [](talas::cdp::RoomReverbSettings& s) { s.reflections = {{0.02, 0.5}, {0.01, 0.5}}; },
             [](talas::cdp::RoomReverbSettings& s) { s.reflections = {{0.02, 0.0}}; },
             [](talas::cdp::RoomReverbSettings& s) { s.reflections = {{0.00001, 0.5}}; }})
    {
        talas::cdp::RoomReverbSettings settings;
        change(settings);
        require(!reverb.prepare(16000.0, settings) && !reverb.prepared(),
            "rmverb should refuse values outside CDP's ranges");
    }
    talas::cdp::RoomReverbSettings large;
    large.room = talas::cdp::RoomSize::large;
    large.absorbHz = 0.0;
    require(!reverb.prepare(300.0, large), "rmverb's large room should need its delays");
    for (const auto* text : {"0.01 0.5\n   \n", "0.01 0\n", "; nothing\n\n", "0.02 0.5\n0.01 0.5\n", "0.01\n"})
    {
        require(!talas::cdp::readRoomReflections(text, 16000.0).has_value(),
            std::string{"rmverb should refuse the reflections \""} + text + "\"");
    }
    require(reverb.prepare(16000.0, {}), "rmverb should prepare");
    const auto input = ramp(128);
    std::vector<double> left(128);
    std::vector<double> right(128);
    require(!reverb.process(std::span{input}.first(64), std::span{left}.first(64), std::span{right}.first(63))
            && reverb.process(std::span{input}.first(64), std::span{left}.first(64), std::span{right}.first(64)),
        "rmverb should refuse spans of different sizes");
    auto twin = reverb;
    auto bad = std::vector<double>(input.begin() + 64, input.end());
    bad[2] = std::numeric_limits<double>::infinity();
    std::vector<double> twinLeft(64);
    std::vector<double> twinRight(64);
    require(!reverb.process(bad, std::span{left}.subspan(64), std::span{right}.subspan(64))
            && reverb.process(std::span{input}.subspan(64), std::span{left}.subspan(64), std::span{right}.subspan(64))
            && twin.process(std::span{input}.subspan(64), twinLeft, twinRight)
            && std::ranges::equal(std::span{left}.subspan(64), twinLeft)
            && std::ranges::equal(std::span{right}.subspan(64), twinRight),
        "a refused block should leave rmverb unchanged");
}

// The same samples to the last bit: a zero's sign counts.
bool sameBits(const std::vector<double>& played, const std::vector<double>& cdp)
{
    return played.size() == cdp.size()
        && std::ranges::equal(played, cdp, [](double a, double b) {
               return a == b && std::signbit(a) == std::signbit(b);
           });
}

// CDP8's modify radical 7, modify loudness 1, 2 and 6, phase phase 1 and
// housekeep extract 4 on recorded references, bit for bit, in 64-frame blocks
// whose live fields change where CDP's breakpoint files step (each step falls
// between two frames, and CDP reads its gain at every frame).
void testSampleProcessesMatchCdp()
{
    const auto matches = [](const std::string& name, auto& port, const std::function<bool(std::size_t)>& set) {
        const auto cdp = reference(name);
        require(sameBits(playLive(port, cdp.input, 0, set, name), cdp.outputs[0]),
            name + " should match CDP8 bit for bit");
        port.reset();
        require(sameBits(playLive(port, cdp.input, 0, set, name), cdp.outputs[0]),
            name + " should replay after reset");
    };
    const auto at = [](std::size_t offset, const std::vector<std::pair<std::size_t, double>>& steps) {
        auto value = steps.front().second;
        for (const auto& [from, next] : steps)
            if (offset >= from)
                value = next;
        return value;
    };
    const auto fixed = [](std::size_t) { return true; };
    for (const int bits : {16, 8, 3, 1})
    {
        const auto name = "modify-radical-7-quantise-bits-" + std::to_string(bits);
        talas::cdp::Quantise port;
        require(port.prepare(bits) && port.prepared() && port.latencyFrames() == 0, name + " should prepare");
        matches(name, port, fixed);
    }
    // A resolution set at a block start plays from there as CDP8 plays it.
    {
        const auto sixteen = reference("modify-radical-7-quantise-bits-16");
        const auto three = reference("modify-radical-7-quantise-bits-3");
        talas::cdp::Quantise port;
        require(port.prepare(16), "quantise should prepare at 16 bits");
        const auto played = playLive(port, sixteen.input, 0,
            [&](std::size_t offset) { return port.setBitResolution(offset < 1024 ? 16 : 3); }, "quantise");
        std::vector<double> expected(sixteen.outputs[0].begin(), sixteen.outputs[0].begin() + 1024);
        expected.insert(expected.end(), three.outputs[0].begin() + 1024, three.outputs[0].end());
        require(sameBits(played, expected),
            "quantise should play a resolution set at a block start from there as CDP8 plays it");
    }
    using Gain = talas::cdp::LoudnessGain::Mode;
    struct Loudness
    {
        std::string name;
        Gain mode;
        bool clip;
        bool live;
        std::vector<std::pair<std::size_t, double>> gain;
    };
    for (const auto& c : {
             Loudness{"modify-loudness-1-gain-2.5", Gain::Gain, true, false, {{0, 2.5}}},
             Loudness{"modify-loudness-2-db-gain-7.5", Gain::Decibels, true, false, {{0, 7.5}}},
             Loudness{"modify-loudness-1-live-gain", Gain::Gain, true, true,
                 {{0, 0.5}, {576, 3.0}, {1024, 0.0}, {1280, 1.25}, {1664, 32767.0}}},
             // CDP_NOCLIP_FLOATS.
             Loudness{"modify-loudness-1-live-gain-no-clip", Gain::Gain, false, true,
                 {{0, 2.0}, {704, 0.25}, {1344, 7.5}}},
             // -96 dB is silence and within 0.000002 of 0 dB is exactly 1.
             Loudness{"modify-loudness-2-live-db-gain-no-clip", Gain::Decibels, false, true,
                 {{0, -6.0}, {512, 33.3}, {768, -96.0}, {1024, 0.000001}, {1280, -53.714}, {1408, -33.3},
                     {1536, 96.0}, {1792, 10.292}, {1920, -95.5}}},
         })
    {
        talas::cdp::LoudnessGain port;
        require(port.prepare(c.mode, c.gain.front().second, c.clip, c.live) && port.latencyFrames() == 0,
            c.name + " should prepare");
        matches(c.name, port, [&](std::size_t offset) { return !c.live || port.setGain(at(offset, c.gain)); });
    }
    using Inversion = talas::cdp::PhaseInvert::Mode;
    for (const auto& [name, mode] : {std::pair{std::string{"modify-loudness-6-invert-phase"}, Inversion::Loudness},
             std::pair{std::string{"phase-phase-1-invert"}, Inversion::Phase}})
    {
        talas::cdp::PhaseInvert port;
        require(port.prepare(mode) && port.latencyFrames() == 0, name + " should prepare");
        matches(name, port, fixed);
    }
    for (const auto& [name, shift] : {std::pair{std::string{"housekeep-extract-4-rectify-shift-0.3"}, 0.3},
             std::pair{std::string{"housekeep-extract-4-rectify-shift-minus-0.25"}, -0.25}})
    {
        talas::cdp::DcShift port;
        require(port.prepare(shift) && port.latencyFrames() == 0, name + " should prepare");
        matches(name, port, fixed);
    }
}

// CDP8's modify radical 6, subtract subtract, submix balance and submix
// crossfade on recorded references of two files, bit for bit, in 64-frame
// blocks whose live balance changes where CDP's breakpoint file steps; the
// references hold the input, the side chain (CDP's second file) and the
// output.
void testTwoInputProcessesMatchCdp()
{
    const auto matches = [](const std::string& name, auto& port, const std::function<bool(std::size_t)>& set) {
        const auto cdp = reference(name, 2);
        require(sameBits(playPair(port, cdp.input, cdp.outputs[0], set, name), cdp.outputs[1]),
            name + " should match CDP8 bit for bit");
        port.reset();
        require(sameBits(playPair(port, cdp.input, cdp.outputs[0], set, name), cdp.outputs[1]),
            name + " should replay after reset");
    };
    const auto fixed = [](std::size_t) { return true; };
    {
        talas::cdp::CrossModulate port;
        require(port.prepare() && port.latencyFrames() == 0, "cross modulation should prepare");
        matches("modify-radical-6-cross-modulation", port, fixed);
    }
    {
        talas::cdp::Subtract port;
        require(port.prepare() && port.latencyFrames() == 0, "subtract should prepare");
        matches("subtract-channel-1", port, fixed);
    }
    for (const auto& [name, balance] : {std::pair{std::string{"submix-balance-0.3"}, 0.3},
             std::pair{std::string{"submix-balance-default"}, talas::cdp::MixBalance::defaultBalance}})
    {
        talas::cdp::MixBalance port;
        require(port.prepare(balance) && port.latencyFrames() == 0, name + " should prepare");
        matches(name, port, fixed);
    }
    {
        talas::cdp::MixBalance port;
        require(port.prepare(0.25), "the live balance should prepare");
        matches("submix-balance-live-balance", port, [&](std::size_t offset) {
            return port.setBalance(offset < 1024 ? 0.25 : offset < 2048 ? 0.9 : offset < 3072 ? 0.0 : 1.0);
        });
    }
    using Contour = talas::cdp::MixCrossfade::Mode;
    struct Crossfade
    {
        std::string name;
        Contour mode;
        double begin;
        double end;
        double stagger;
        double powerFactor;
    };
    for (const auto& c : {
             Crossfade{"submix-crossfade-1-begin-0.01235-end-0.0567", Contour::Linear, 0.01235, 0.0567, 0.0, 1.0},
             Crossfade{"submix-crossfade-2-begin-0.01235-end-0.0567", Contour::Cosine, 0.01235, 0.0567, 0.0, 1.0},
             Crossfade{"submix-crossfade-2-powfac-3", Contour::Cosine, 0.01, 0.07, 0.0, 3.0},
             Crossfade{"submix-crossfade-2-stagger-0.00502-powfac-0.4", Contour::Cosine, 0.02, 0.06, 0.00502, 0.4},
             Crossfade{"submix-crossfade-1-end-0.04", Contour::Linear, 0.0, 0.04, 0.0, 1.0},
         })
    {
        talas::cdp::MixCrossfade port;
        require(port.prepare(48000.0, c.mode, c.begin, c.end, c.stagger, c.powerFactor) && port.latencyFrames() == 0,
            c.name + " should prepare");
        matches(c.name, port, fixed);
    }
}

void testSampleAndTwoInputProcessesRejectWithoutChange()
{
    using Gain = talas::cdp::LoudnessGain::Mode;
    using Contour = talas::cdp::MixCrossfade::Mode;
    talas::cdp::Quantise quantise;
    talas::cdp::LoudnessGain loudness;
    talas::cdp::PhaseInvert invert;
    talas::cdp::DcShift dcShift;
    talas::cdp::CrossModulate crossModulate;
    talas::cdp::Subtract subtract;
    talas::cdp::MixBalance balance;
    talas::cdp::MixCrossfade crossfade;
    const auto input = ramp(256);
    std::vector<double> output(256);
    require(!quantise.prepare(0) && !quantise.prepare(17) && !quantise.setBitResolution(8)
            && !loudness.prepare(Gain::Gain, 0.0) && !loudness.prepare(Gain::Gain, 0.000002)
            && !loudness.prepare(Gain::Gain, 32767.5) && !loudness.prepare(Gain::Decibels, -96.0)
            && !loudness.prepare(Gain::Decibels, 96.5) && !loudness.setGain(1.0)
            && !invert.prepare(static_cast<talas::cdp::PhaseInvert::Mode>(2))
            && !dcShift.prepare(0.0) && !dcShift.prepare(0.000002) && !dcShift.prepare(-1.5)
            && !dcShift.prepare(std::numeric_limits<double>::quiet_NaN())
            && !balance.prepare(1.5) && !balance.setBalance(0.5)
            && !crossfade.prepare(48000.0, Contour::Linear, 0.02, 0.02)
            && !crossfade.prepare(48000.0, Contour::Linear, 0.004, 0.02, 0.005)
            && !crossfade.prepare(48000.0, Contour::Cosine, 0.01, 0.02, 0.0, 8.5)
            && !crossfade.prepare(48000.0, Contour::Linear, 11.0, 12.0, 10.5)
            && !quantise.prepared() && !loudness.prepared() && !invert.prepared() && !dcShift.prepared()
            && !crossModulate.prepared() && !subtract.prepared() && !balance.prepared() && !crossfade.prepared()
            && !quantise.process(input, output) && !loudness.process(input, output)
            && !invert.process(input, output) && !dcShift.process(input, output)
            && !crossModulate.process(input, input, output) && !subtract.process(input, input, output)
            && !balance.process(input, input, output) && !crossfade.process(input, input, output),
        "the sample processes should refuse a bit resolution outside 1 to 16, a fixed gain of silence or beyond CDP's range, an unknown inversion, a shift of 0 or beyond 1, a balance beyond 1, a crossfade of no length, before its stagger, with a power factor over 8 or a stagger over 10 s, a setter before prepare and to play unprepared");
    // A live gain may reach silence, which a fixed gain may not, and only a
    // live gain takes the setter.
    require(loudness.prepare(Gain::Gain, 0.0, true, true) && loudness.setGain(0.0) && !loudness.setGain(-0.1)
            && !loudness.setGain(32767.5) && loudness.prepare(Gain::Decibels, -96.0, false, true)
            && loudness.setGain(96.0) && !loudness.setGain(96.5) && loudness.prepare(Gain::Gain, 2.0)
            && !loudness.setGain(1.0),
        "modify loudness should take a live gain of silence and refuse the setter for a fixed gain");
    require(quantise.prepare(8) && quantise.setBitResolution(1) && quantise.setBitResolution(16)
            && !quantise.setBitResolution(0) && !quantise.setBitResolution(17)
            && balance.prepare() && balance.setBalance(0.0) && balance.setBalance(1.0) && !balance.setBalance(-0.01)
            && invert.prepare(talas::cdp::PhaseInvert::Mode::Loudness) && dcShift.prepare(-0.3)
            && crossModulate.prepare() && subtract.prepare()
            && crossfade.prepare(48000.0, Contour::Cosine, 0.001, 0.003, 0.0005, 2.0),
        "the sample processes should prepare within CDP's ranges");
    auto bad = input;
    bad[3] = std::numeric_limits<double>::quiet_NaN();
    const std::span<const double> in{input};
    const std::span<const double> notFinite{bad};
    const std::span<double> out{output};
    const auto refusesMono = [&](auto& port) {
        const auto before = port;
        return port.process(in.first(64), out.first(64)) && !port.process(in.first(8), out.first(7))
            && !port.process(notFinite, out) && port == before;
    };
    const auto refusesPair = [&](auto& port) {
        const auto before = port;
        return !port.process(in.first(8), in.first(8), out.first(7))
            && !port.process(in.first(8), in.first(7), out.first(8)) && !port.process(notFinite, in, out)
            && !port.process(in, notFinite, out) && port == before;
    };
    require(refusesMono(quantise) && refusesMono(loudness) && refusesMono(invert) && refusesMono(dcShift),
        "the one-input processes should refuse spans of different sizes and a non-finite input without change");
    require(crossfade.process(in.first(64), in.first(64), out.first(64)) && refusesPair(crossModulate)
            && refusesPair(subtract) && refusesPair(balance) && refusesPair(crossfade),
        "the two-input processes should refuse spans of different sizes and a non-finite input or side without change");
}

// A port's output in blocks of the given frames.
template <typename Port>
std::vector<double> playBlocks(Port& port, const std::vector<double>& input, std::size_t block, const std::string& name)
{
    std::vector<double> output(input.size());
    for (std::size_t offset = 0; offset < input.size(); offset += block)
    {
        const auto frames = std::min(block, input.size() - offset);
        require(port.process(std::span{input}.subspan(offset, frames), std::span{output}.subspan(offset, frames)),
            name + " should process");
    }
    return output;
}

// Whether a port's output, starting with its latency's +0 frames, matches
// CDP's frames to the last bit latency frames late, over all but CDP's last
// latency frames (where CDP ends its file as a stream cannot).
bool lateBits(const std::vector<double>& played, const std::vector<double>& cdp, std::size_t latency)
{
    const auto same = [](double a, double b) { return a == b && std::signbit(a) == std::signbit(b); };
    if (played.size() < cdp.size() || cdp.size() < latency)
        return false;
    for (std::size_t frame = 0; frame < latency; ++frame)
    {
        if (!same(played[frame], 0.0))
            return false;
    }
    for (std::size_t frame = 0; frame + latency < cdp.size(); ++frame)
    {
        if (!same(played[frame + latency], cdp[frame]))
            return false;
    }
    return true;
}

// A port refuses to play before it is prepared, and spans of different
// sizes or a non-finite input without change.
template <typename Port>
void requireProcessRefusals(Port& port, const std::string& name)
{
    {
        Port unprepared;
        const std::vector<double> in{0.25, -0.5};
        std::vector<double> out(2, 7.0);
        require(!unprepared.prepared() && !unprepared.process(in, out) && out == std::vector<double>(2, 7.0),
            name + " should refuse to play before it is prepared");
    }
    const auto before = port;
    const std::vector<double> in{0.25, -0.5, 0.75};
    std::vector<double> out(2, 7.0);
    require(!port.process(in, out) && out == std::vector<double>(2, 7.0),
        name + " should refuse spans of different sizes");
    for (const auto bad : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity()})
    {
        const std::vector<double> badIn{0.25, bad, 0.5};
        std::vector<double> badOut(3, 7.0);
        require(!port.process(badIn, badOut) && badOut == std::vector<double>(3, 7.0),
            name + " should refuse a non-finite input");
    }
    require(port == before, name + " should be unchanged by refused calls");
}

// A fastconv impulse response in tests/cdp/fixtures, its frames interleaved
// as CDP's soundfile holds them, and its channels: a stereo one's second
// column is its second channel.
std::pair<std::vector<double>, std::size_t> fastConvolutionImpulse(const std::string& name)
{
    const auto ir = reference("fastconv-ir-" + name);
    if (name.find("stereo") == std::string::npos)
        return {ir.input, 1};
    std::vector<double> frames;
    for (std::size_t frame = 0; frame < ir.input.size(); ++frame)
    {
        frames.push_back(ir.input[frame]);
        frames.push_back(ir.outputs[0][frame]);
    }
    return {frames, 2};
}

// CDP8's fastconv on recorded references, each output channel bit for bit,
// latencyFrames (the impulse response's frames less one) late: in 64-frame
// blocks, in uneven blocks after reset, and played on, where the stream's
// tail is the FFTs' rounding noise and then silence. Only in CDP's tail,
// which CDP writes without the dry part the stream adds as +0, may a zero's
// sign differ.
void testFastConvolutionMatchesCdp()
{
    struct Case
    {
        std::string name;
        std::string impulse;
        double dry;
        double scale;
    };
    for (const auto& c : {
             Case{"fastconv-37-frame-lowpass", "37-frame-lowpass", 0.0, 1.0},
             Case{"fastconv-256-frame-noise-dry-0.5", "256-frame-noise", 0.5, 1.0},
             Case{"fastconv-1000-frame-reverb-scale-0.5-dry-0.3", "1000-frame-reverb", 0.3, 0.5},
             Case{"fastconv-300-frame-stereo-impulse-dry-0.4", "300-frame-stereo", 0.4, 1.0},
             Case{"fastconv-100-frame-noise-scale-2-dry-0.25", "100-frame-noise", 0.25, 2.0},
             Case{"fastconv-5-frame-dry-0.8", "5-frame", 0.8, 1.0},
             Case{"fastconv-2-frame", "2-frame", 0.0, 1.0},
             Case{"fastconv-1-frame-dry-0.2", "1-frame", 0.2, 1.0},
             Case{"fastconv-37-frame-lowpass-dry-1-silent", "37-frame-lowpass", 1.0, 1.0},
             Case{"fastconv-silent-impulse-dry-0.5", "16-frame-silent", 0.5, 1.0},
         })
    {
        const auto [impulse, channels] = fastConvolutionImpulse(c.impulse);
        const auto frames = impulse.size() / channels;
        const auto cdp = reference(c.name, channels);
        // CDP's blocks end at its input's frames rounded up to a block; its
        // tail of the impulse response's frames follows.
        const auto inputFrames = cdp.input.size() - frames;
        const auto tailStart = (inputFrames + frames - 1) / frames * frames;
        std::size_t fftLength = 1;
        while (fftLength < 2 * frames)
            fftLength <<= 1;
        for (std::size_t channel = 0; channel < channels; ++channel)
        {
            const auto name = c.name + " channel " + std::to_string(channel + 1);
            talas::cdp::FastConvolutionSettings settings;
            settings.dry = c.dry;
            settings.scale = c.scale;
            settings.impulseChannels = channels;
            settings.channel = channel;
            settings.inputChannels = 1;
            talas::cdp::FastConvolution port;
            require(port.prepare(48000.0, impulse, 48000.0, settings) && port.latencyFrames() == frames - 1
                    && port.fftLength() == fftLength,
                name + " should prepare, lagging its impulse response's frames less one, with fastconv's FFT size");
            const auto& expected = cdp.outputs[channel];
            const auto matches = [&](const std::vector<double>& played) {
                if (played.size() < expected.size())
                    return false;
                for (std::size_t frame = 0; frame < expected.size(); ++frame)
                {
                    if (played[frame] != expected[frame]
                        || (frame < tailStart && std::signbit(played[frame]) != std::signbit(expected[frame])))
                    {
                        return false;
                    }
                }
                return true;
            };
            const auto latency = port.latencyFrames();
            require(matches(playLive(port, cdp.input, latency, [](std::size_t) { return true; }, name)),
                name + " should match CDP8's fastconv bit for bit in 64-frame blocks, latencyFrames late");
            port.reset();
            require(matches(playLagging(port, cdp.input, latency, name)),
                name + " should match CDP8 in uneven blocks after reset");
            port.reset();
            auto longer = cdp.input;
            longer.resize(cdp.input.size() + 3 * frames + 5, 0.0);
            const auto tail = playLagging(port, longer, latency, name);
            auto quiet = matches(tail);
            for (std::size_t frame = expected.size(); frame < tail.size(); ++frame)
                quiet = quiet && (frame < tailStart + frames ? std::abs(tail[frame]) < 1.0e-12 : tail[frame] == 0.0);
            require(quiet, name + " should play only the FFTs' rounding noise past CDP's end, then silence");
        }
    }
}

void testFastConvolutionRejectsWithoutChange()
{
    const auto impulse = fastConvolutionImpulse("37-frame-lowpass").first;
    const auto stereo = fastConvolutionImpulse("300-frame-stereo").first;
    const auto lowpass = reference("fastconv-37-frame-lowpass-dry-1-silent");
    const auto fields = [](double dry, double scale, std::size_t impulseChannels, std::size_t channel,
                            std::size_t inputChannels) {
        talas::cdp::FastConvolutionSettings settings;
        settings.dry = dry;
        settings.scale = scale;
        settings.impulseChannels = impulseChannels;
        settings.channel = channel;
        settings.inputChannels = inputChannels;
        return settings;
    };
    talas::cdp::FastConvolution port;
    const std::vector<double> in{0.1, 0.2, 0.3, 0.4};
    std::vector<double> out(4);
    require(!port.prepared() && !port.process(in, out), "an unprepared fast convolution should refuse to play");
    const auto base = fields(0.3, 1.0, 1, 0, 1);
    require(port.prepare(48000.0, impulse, 48000.0, base), "the lowpass should prepare");
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto inf = std::numeric_limits<double>::infinity();
    const std::vector<double> overlong(talas::cdp::FastConvolution::maximumImpulseFrames + 1, 0.0);
    auto nonFinite = impulse;
    nonFinite[3] = inf;
    struct Refusal
    {
        const char* what;
        double rate;
        std::span<const double> impulse;
        double impulseRate;
        talas::cdp::FastConvolutionSettings settings;
    };
    const Refusal refusals[] = {
        {"a zero sample rate", 0.0, impulse, 0.0, base},
        {"a NaN sample rate", nan, impulse, nan, base},
        {"differing sample rates", 48000.0, impulse, 44100.0, base},
        {"an empty impulse response", 48000.0, {}, 48000.0, base},
        {"a non-finite impulse sample", 48000.0, nonFinite, 48000.0, base},
        {"an impulse response over the maximum", 48000.0, overlong, 48000.0, base},
        {"a dry below 0", 48000.0, impulse, 48000.0, fields(-0.01, 1.0, 1, 0, 1)},
        {"a dry above 1", 48000.0, impulse, 48000.0, fields(1.01, 1.0, 1, 0, 1)},
        {"a NaN dry", 48000.0, impulse, 48000.0, fields(nan, 1.0, 1, 0, 1)},
        {"a zero scale", 48000.0, impulse, 48000.0, fields(0.0, 0.0, 1, 0, 1)},
        {"a negative scale", 48000.0, impulse, 48000.0, fields(0.0, -1.0, 1, 0, 1)},
        {"an infinite scale", 48000.0, impulse, 48000.0, fields(0.0, inf, 1, 0, 1)},
        {"zero impulse channels", 48000.0, impulse, 48000.0, fields(0.0, 1.0, 0, 0, 1)},
        {"partial frames", 48000.0, std::span<const double>{impulse}.first(37), 48000.0, fields(0.0, 1.0, 2, 0, 1)},
        {"a channel outside the impulse response", 48000.0, stereo, 48000.0, fields(0.0, 1.0, 2, 2, 1)},
        {"a mono channel past the first", 48000.0, impulse, 48000.0, fields(0.0, 1.0, 1, 1, 1)},
        {"three input channels for a stereo impulse", 48000.0, stereo, 48000.0, fields(0.0, 1.0, 2, 0, 3)},
        {"zero input channels", 48000.0, impulse, 48000.0, fields(0.0, 1.0, 1, 0, 0)},
    };
    for (const auto& r : refusals)
    {
        require(!port.prepare(r.rate, r.impulse, r.impulseRate, r.settings) && port.latencyFrames() == 36
                && port.fftLength() == 128,
            std::string{"fast convolution should refuse "} + r.what + " without change");
    }
    // Still the dry-0.3 lowpass: it plays as a fresh one.
    talas::cdp::FastConvolution fresh;
    require(fresh.prepare(48000.0, impulse, 48000.0, base), "a fresh lowpass should prepare");
    std::vector<double> a(lowpass.input.size());
    std::vector<double> b(lowpass.input.size());
    require(port.process(lowpass.input, a) && fresh.process(lowpass.input, b) && a == b,
        "a refused prepare should leave fast convolution as it was");
    std::vector<double> shorter(3);
    const std::vector<double> notANumber{0.1, nan, 0.2, 0.3};
    const std::vector<double> infinite{0.1, 0.2, inf, 0.3};
    require(!port.process(in, shorter) && !port.process(notANumber, out) && !port.process(infinite, out),
        "fast convolution should refuse spans of different sizes and a non-finite input");
    require(port.process(lowpass.input, a) && fresh.process(lowpass.input, b) && a == b,
        "a refused block should leave fast convolution unchanged");
    // CDP's channel cases: a stereo input with a stereo impulse, and any
    // number of channels with a mono one.
    require(port.prepare(48000.0, stereo, 48000.0, fields(0.0, 1.0, 2, 1, 2))
            && port.prepare(48000.0, impulse, 48000.0, fields(0.0, 1.0, 1, 0, 5)),
        "fast convolution should take a stereo input with a stereo impulse and five channels with a mono one");
    std::vector<double> longest(talas::cdp::FastConvolution::maximumImpulseFrames, 0.0);
    longest[0] = 1.0;
    require(port.prepare(48000.0, longest, 48000.0) && port.latencyFrames() == longest.size() - 1
            && port.fftLength() == 2 * longest.size(),
        "fast convolution should take the longest impulse response");
    std::vector<double> click(longest.size() + 4, 0.0);
    click[0] = 0.5;
    std::vector<double> clickOut(click.size());
    require(port.process(click, clickOut) && clickOut[longest.size() - 1] != 0.0
            && std::isfinite(clickOut[longest.size() - 1]),
        "the longest impulse response should convolve a click after its latency");
}

// CDP8's gate gate 1, housekeep deglitch, sfedit noisecut and envel attack 1
// to 3 on recorded references, bit for bit, latencyFrames late over all but
// CDP's last latencyFrames frames, in blocks of 64, 37, 1 and 1000 frames and
// whole, each after reset. Where a reference was made with CDP's buffers
// shortened, the port takes the same buffer length; CDP's default buffers
// must not match there, nor noisecut's other keep-noise.
void testRestorationProcessesMatchCdp()
{
    const auto matches = [](const std::string& name, auto& port, std::size_t latency) {
        const auto cdp = reference(name);
        require(port.latencyFrames() == latency,
            name + ": latency " + std::to_string(port.latencyFrames()) + ", expected " + std::to_string(latency));
        for (const auto block : {std::size_t{64}, std::size_t{37}, std::size_t{1}, std::size_t{1000}, cdp.input.size()})
        {
            port.reset();
            require(lateBits(playBlocks(port, cdp.input, block, name), cdp.outputs[0], latency),
                name + " should match CDP8 bit for bit, latencyFrames late, in " + std::to_string(block)
                    + "-frame blocks");
        }
        return cdp;
    };
    const auto differs = [](const std::string& name, auto& other, const Reference& cdp, std::size_t latency) {
        return !lateBits(playBlocks(other, cdp.input, 64, name), cdp.outputs[0], latency);
    };

    using talas::cdp::SilenceGate;
    for (const auto& [name, decibels, buffer] : {
             std::tuple{std::string{"gate-1-gate-minus-40-db-1280-frame-buffers"}, -40.0, std::size_t{1280}},
             std::tuple{std::string{"gate-1-gate-minus-40-db"}, -40.0, SilenceGate::defaultBufferFrames},
             std::tuple{std::string{"gate-1-gate-minus-30-db-long-start-1280-frame-buffers"}, -30.0, std::size_t{1280}},
             std::tuple{std::string{"gate-1-gate-0-db-unit-samples"}, 0.0, SilenceGate::defaultBufferFrames},
         })
    {
        SilenceGate gate;
        require(gate.prepare(48000.0, decibels, buffer) && gate.prepared(), name + " should prepare");
        const auto cdp = matches(name, gate, 96);
        // The run that starts the long-start reference leaves no buffer end
        // for CDP's search to skip from.
        if (buffer != SilenceGate::defaultBufferFrames && name.find("long-start") == std::string::npos)
        {
            SilenceGate other;
            require(other.prepare(48000.0, decibels) && differs(name, other, cdp, 96),
                name + " should differ with CDP's default buffers");
        }
    }

    struct Glitches
    {
        std::string name;
        double glitch;
        double silence;
        double threshold;
        double splice;
        double window;
        std::size_t latency;
    };
    for (const auto& c : {
             Glitches{"housekeep-deglitch-glitch-2-sil-10-thresh-0.01-splice-1-window-1", 2.0, 10.0, 0.01, 1.0, 1.0,
                 96 + 480 - 1 + 48},
             Glitches{"housekeep-deglitch-glitch-1.5-sil-0.5-thresh-0.005-splice-0.25-window-3", 1.5, 0.5, 0.005, 0.25,
                 3.0, 144 + 24 - 1 + 12},
             Glitches{"housekeep-deglitch-glitch-2-sil-10-thresh-0.01-splice-1-window-1-start", 2.0, 10.0, 0.01, 1.0,
                 1.0, 96 + 480 - 1 + 48},
             Glitches{"housekeep-deglitch-glitch-1.5-sil-0.5-thresh-0.005-splice-0.25-window-3-start", 1.5, 0.5, 0.005,
                 0.25, 3.0, 144 + 24 - 1 + 12},
             Glitches{"housekeep-deglitch-glitch-2-sil-10-thresh-0-splice-0-window-1-digital-silence", 2.0, 10.0, 0.0,
                 0.0, 1.0, 96 + 480 - 1 + 1},
         })
    {
        talas::cdp::Deglitch deglitch;
        require(deglitch.prepare(48000.0, c.glitch, c.silence, c.threshold, c.splice, c.window) && deglitch.prepared(),
            c.name + " should prepare");
        matches(c.name, deglitch, c.latency);
    }

    struct Cut
    {
        std::string name;
        double splice;
        double frequency;
        double maxNoise;
        double minTone;
        bool keepNoise;
        bool lastWavesetNoise;
        std::size_t buffer;
        std::size_t latency;
    };
    const std::string p = "sfedit-noisecut-splice-2-noisefrq-6000-maxnoise-5-mintone-15";
    const auto noiseBuffer = talas::cdp::NoiseCut::defaultBufferFrames;
    for (const auto& c : {
             Cut{p + "-noise-first", 2.0, 6000.0, 5.0, 15.0, false, false, noiseBuffer, 1008},
             Cut{p + "-2560-frame-buffers", 2.0, 6000.0, 5.0, 15.0, false, false, 2560, 1008},
             Cut{p + "-keep-noise", 2.0, 6000.0, 5.0, 15.0, true, false, noiseBuffer, 1008},
             Cut{p + "-tone-first", 2.0, 6000.0, 5.0, 15.0, false, false, noiseBuffer, 1008},
             Cut{p + "-noise-end", 2.0, 6000.0, 5.0, 15.0, false, true, noiseBuffer, 1008},
             Cut{"sfedit-noisecut-splice-1-noisefrq-2000-maxnoise-2-mintone-5-noise-first", 1.0, 2000.0, 2.0, 5.0,
                 false, false, noiseBuffer, 144 + 240},
             Cut{"sfedit-noisecut-splice-0.5-noisefrq-6000-maxnoise-0-mintone-2-2560-frame-buffers", 0.5, 6000.0, 0.0,
                 2.0, false, false, 2560, 72 + 96},
             Cut{"sfedit-noisecut-splice-1-noisefrq-6000-maxnoise-0-mintone-0.5-pairs", 1.0, 6000.0, 0.0, 0.5, false,
                 false, noiseBuffer, 144 + 24},
             Cut{p + "-noise-first-noise-end", 2.0, 6000.0, 5.0, 15.0, false, true, noiseBuffer, 1008},
             Cut{p + "-overrun", 2.0, 6000.0, 5.0, 15.0, false, true, noiseBuffer, 1008},
         })
    {
        talas::cdp::NoiseCut cut;
        require(cut.prepare(48000.0, c.splice, c.frequency, c.maxNoise, c.minTone, c.keepNoise, c.lastWavesetNoise,
                    c.buffer)
                && cut.prepared(),
            c.name + " should prepare");
        const auto cdp = matches(c.name, cut, c.latency);
        talas::cdp::NoiseCut other;
        require(other.prepare(48000.0, c.splice, c.frequency, c.maxNoise, c.minTone, !c.keepNoise,
                    c.lastWavesetNoise, c.buffer)
                && differs(c.name, other, cdp, c.latency),
            c.name + " should differ with the other keep-noise");
        // Where CDP's second search keeps a switch its first did not, the
        // first search's end state shows.
        if (c.name.ends_with("-overrun"))
        {
            talas::cdp::NoiseCut otherEnd;
            require(otherEnd.prepare(48000.0, c.splice, c.frequency, c.maxNoise, c.minTone, c.keepNoise,
                        !c.lastWavesetNoise, c.buffer)
                    && differs(c.name, otherEnd, cdp, c.latency),
                c.name + " should differ with the other last waveset");
        }
        if (c.buffer != noiseBuffer)
        {
            talas::cdp::NoiseCut defaults;
            require(defaults.prepare(48000.0, c.splice, c.frequency, c.maxNoise, c.minTone, c.keepNoise,
                        c.lastWavesetNoise)
                    && differs(c.name, defaults, cdp, c.latency),
                c.name + " should differ with CDP's default buffers");
        }
    }

    using Mode = talas::cdp::AttackBoost::Mode;
    using Envelope = talas::cdp::AttackBoost::Envelope;
    struct Attack
    {
        std::string name;
        Mode mode;
        double level;
        double gain;
        double onset;
        double decay;
        Envelope envelope;
        std::size_t buffer;
        std::size_t latency;
    };
    const auto envelBuffer = talas::cdp::AttackBoost::defaultBufferFrames;
    for (const auto& c : {
             Attack{"envel-attack-1-gate-0.1-gain-2-onset-20-decay-50", Mode::Gate, 0.1, 2.0, 20.0, 50.0,
                 Envelope::Exponential, envelBuffer, 961 + 64},
             Attack{"envel-attack-1-gate-0.1-gain-2-onset-20-decay-50-linear", Mode::Gate, 0.1, 2.0, 20.0, 50.0,
                 Envelope::Linear, envelBuffer, 961 + 64},
             Attack{"envel-attack-3-time-0.1-gain-2.5-onset-10-decay-40", Mode::ExactTime, 0.1, 2.5, 10.0, 40.0,
                 Envelope::Exponential, envelBuffer, 0},
             Attack{"envel-attack-3-time-0.1-gain-2.5-onset-10-decay-40-linear", Mode::ExactTime, 0.1, 2.5, 10.0, 40.0,
                 Envelope::Linear, envelBuffer, 0},
             Attack{"envel-attack-2-time-0.3-gain-2-onset-20-decay-50", Mode::ApproximateTime, 0.3, 2.0, 20.0, 50.0,
                 Envelope::Exponential, envelBuffer, 961 + 19200},
             Attack{"envel-attack-2-time-0.3-gain-2-onset-20-decay-50-linear", Mode::ApproximateTime, 0.3, 2.0, 20.0,
                 50.0, Envelope::Linear, envelBuffer, 961 + 19200},
             Attack{"envel-attack-3-time-0.0999167-gain-1.00017-onset-10-decay-48-exponential-2304-frame-buffers",
                 Mode::ExactTime, 0.0999167, 1.00017, 10.0, 48.0, Envelope::Exponential, 2304, 0},
         })
    {
        talas::cdp::AttackBoost attack;
        require(attack.prepare(48000.0, c.mode, c.level, c.gain, c.onset, c.decay, c.envelope, c.buffer)
                && attack.prepared(),
            c.name + " should prepare");
        const auto cdp = matches(c.name, attack, c.latency);
        // It acts once: played on without a reset, its input passes as it
        // is, latencyFrames late.
        attack.reset();
        static_cast<void>(playBlocks(attack, cdp.input, 64, c.name));
        const auto again = playBlocks(attack, cdp.input, 64, c.name);
        auto unchanged = true;
        for (std::size_t frame = c.latency; frame + 2 * c.latency + 1 < cdp.input.size(); ++frame)
        {
            unchanged = unchanged && again[frame] == cdp.input[frame - c.latency]
                && std::signbit(again[frame]) == std::signbit(cdp.input[frame - c.latency]);
        }
        require(unchanged, c.name + " should act once until reset");
        if (c.buffer != envelBuffer)
        {
            talas::cdp::AttackBoost defaults;
            require(defaults.prepare(48000.0, c.mode, c.level, c.gain, c.onset, c.decay, c.envelope)
                    && differs(c.name, defaults, cdp, c.latency),
                c.name + " should differ with CDP's default buffers");
        }
    }
    // A gate never passed leaves the input as it is, latencyFrames late.
    {
        const auto cdp = reference("envel-attack-1-gate-0.1-gain-2-onset-20-decay-50");
        talas::cdp::AttackBoost quiet;
        require(quiet.prepare(48000.0, Mode::Gate, 0.9, 2.0, 20.0, 50.0), "a high gate should prepare");
        require(lateBits(playBlocks(quiet, cdp.input, 64, "a high gate"), cdp.input, quiet.latencyFrames()),
            "a gate never passed should leave the input as it is");
    }
}

void testRestorationProcessesRejectWithoutChange()
{
    const auto nan = std::numeric_limits<double>::quiet_NaN();

    talas::cdp::SilenceGate gate;
    require(gate.prepare(48000.0, -40.0, 1280), "the gate should prepare");
    requireProcessRefusals(gate, "the gate");
    const auto gateBefore = gate;
    require(!gate.prepare(48000.0, -96.5, 1280) && !gate.prepare(48000.0, 0.5, 1280)
            && !gate.prepare(48000.0, -94.0, 1280) && !gate.prepare(48000.0, nan, 1280)
            && !gate.prepare(0.0, -40.0, 1280) && !gate.prepare(400.0, -40.0, 1280)
            && !gate.prepare(48000.0, -40.0, 0) && gate == gateBefore,
        "the gate should refuse a level outside -96 to 0 dB or at most one over the sample rate (-94 dB), a NaN, a splice under a frame and no buffer without change");
    require(gate.prepare(48000.0, -93.0) && gate.prepare(44100.0, 0.0) && gate.latencyFrames() == 88,
        "the gate should take -93 and 0 dB, with 44-frame splices at 44.1 kHz");

    talas::cdp::Deglitch deglitch;
    require(deglitch.prepare(48000.0, 2.0, 10.0, 0.01, 1.0, 1.0), "deglitch should prepare");
    requireProcessRefusals(deglitch, "deglitch");
    const auto deglitchBefore = deglitch;
    require(!deglitch.prepare(48000.0, 0.02, 10.0, 0.01, 1.0, 1.0)
            && !deglitch.prepare(48000.0, 1000.5, 10.0, 0.01, 1.0, 1.0)
            && !deglitch.prepare(48000.0, 2.0, 0.29, 0.01, 0.1, 1.0)
            && !deglitch.prepare(48000.0, 2.0, 1000.5, 0.01, 1.0, 1.0)
            && !deglitch.prepare(48000.0, 2.0, 10.0, -0.001, 1.0, 1.0)
            && !deglitch.prepare(48000.0, 2.0, 10.0, 1.001, 1.0, 1.0)
            && !deglitch.prepare(48000.0, 2.0, 10.0, 0.01, -0.1, 1.0)
            && !deglitch.prepare(48000.0, 2.0, 100.0, 0.01, 50.5, 1.0)
            && !deglitch.prepare(48000.0, 2.0, 10.0, 0.01, 1.0, 0.02)
            && !deglitch.prepare(48000.0, 2.0, 10.0, 0.01, 1.0, 4.01)
            && !deglitch.prepare(48000.0, 2.0, 10.0, 0.01, 5.02, 1.0)
            && !deglitch.prepare(48000.0, 2.0, 10.0, nan, 1.0, 1.0)
            && !deglitch.prepare(0.0, 2.0, 10.0, 0.01, 1.0, 1.0) && deglitch == deglitchBefore,
        "deglitch should refuse a glitch under a frame or over 1000 ms, silence outside 0.3 to 1000 ms, a threshold outside 0 to 1, a splice outside 0 to 50 ms or over half the silence, a window under a frame or over 4 ms, a NaN and no sample rate without change");
    require(deglitch.prepare(48000.0, 1000.0, 1000.0, 1.0, 50.0, 4.0)
            && deglitch.latencyFrames() == 48000 + 48000 - 1 + 2400
            && deglitch.prepare(48000.0, 0.0208334, 0.3, 0.0, 0.0, 0.0208334)
            && deglitch.latencyFrames() == 1 + 14 - 1 + 1 && deglitch.prepare(48000.0, 2.0, 10.0, 0.01, 5.0, 1.0),
        "deglitch should take CDP's largest and smallest values (a splice of 0 ms is a frame) and a splice of half the silence");

    using talas::cdp::NoiseCut;
    NoiseCut cut;
    require(cut.prepare(48000.0, 2.0, 6000.0, 5.0, 15.0), "noisecut should prepare");
    requireProcessRefusals(cut, "noisecut");
    const auto cutBefore = cut;
    const auto big = NoiseCut::defaultBufferFrames;
    require(!cut.prepare(48000.0, -0.1, 6000.0, 5.0, 15.0, false, false, big)
            && !cut.prepare(48000.0, 50.1, 6000.0, 5.0, 15.0, false, false, big)
            && !cut.prepare(48000.0, 2.0, 999.0, 5.0, 15.0, false, false, big)
            && !cut.prepare(48000.0, 2.0, 24001.0, 5.0, 15.0, false, false, big)
            && !cut.prepare(48000.0, 2.0, 6000.0, -1.0, 15.0, false, false, big)
            && !cut.prepare(48000.0, 2.0, 6000.0, 50.1, 15.0, false, false, big)
            && !cut.prepare(48000.0, 2.0, 6000.0, 5.0, -1.0, false, false, big)
            && !cut.prepare(48000.0, 2.0, 6000.0, 5.0, 1000.1, false, false, big)
            && !cut.prepare(48000.0, 2.0, 6000.0, 5.0, 15.0, false, false, 0)
            && !cut.prepare(48000.0, 2.0, 6000.0, 5.0, 15.0, false, false, 96)
            && !cut.prepare(48000.0, 2.0, nan, 5.0, 15.0, false, false, big)
            && !cut.prepare(-48000.0, 2.0, 6000.0, 5.0, 15.0, false, false, big) && cut == cutBefore,
        "noisecut should refuse a splice outside 0 to 50 ms or as long as its buffer, a noise frequency outside 1000 Hz to half the sample rate, a max noise outside 0 to 50 ms, a min tone outside 0 to 1000 ms, no buffer, a NaN and a negative sample rate without change");
    require(cut.prepare(48000.0, 50.0, 24000.0, 50.0, 1000.0) && cut.latencyFrames() == 3 * 2400 + 48000
            && cut.prepare(48000.0, 0.0, 1000.0, 0.0, 0.0) && cut.latencyFrames() == 47
            && cut.prepare(48000.0, 2.0, 6000.0, 5.0, 15.0, false, false, 97),
        "noisecut should take CDP's largest values, wait a noise waveset less a frame with no splice or minimum lengths, and take a splice a frame shorter than its buffer");

    using Mode = talas::cdp::AttackBoost::Mode;
    using Envelope = talas::cdp::AttackBoost::Envelope;
    talas::cdp::AttackBoost attack;
    require(attack.prepare(48000.0, Mode::Gate, 0.1, 2.0, 20.0, 50.0), "envel attack should prepare");
    requireProcessRefusals(attack, "envel attack");
    const auto attackBefore = attack;
    require(!attack.prepare(48000.0, Mode::Gate, 1.01, 2.0, 20.0, 50.0)
            && !attack.prepare(48000.0, Mode::Gate, -0.01, 2.0, 20.0, 50.0)
            && !attack.prepare(48000.0, Mode::ExactTime, -0.1, 2.0, 20.0, 50.0)
            && !attack.prepare(48000.0, Mode::ApproximateTime, 3600.5, 2.0, 20.0, 50.0)
            && !attack.prepare(48000.0, Mode::Gate, 0.1, 0.99, 20.0, 50.0)
            && !attack.prepare(48000.0, Mode::Gate, 0.1, 32768.0, 20.0, 50.0)
            && !attack.prepare(48000.0, Mode::Gate, 0.1, 2.0, 4.9, 50.0)
            && !attack.prepare(48000.0, Mode::Gate, 0.1, 2.0, 1000.5, 50.0)
            && !attack.prepare(48000.0, Mode::Gate, 0.1, 2.0, 20.0, 4.9)
            && !attack.prepare(48000.0, Mode::Gate, 0.1, 2.0, 20.0, 3600001.0)
            && !attack.prepare(48000.0, Mode::Gate, nan, 2.0, 20.0, 50.0)
            && !attack.prepare(0.0, Mode::Gate, 0.1, 2.0, 20.0, 50.0)
            && !attack.prepare(48000.0, static_cast<Mode>(3), 0.1, 2.0, 20.0, 50.0)
            && !attack.prepare(48000.0, Mode::Gate, 0.1, 2.0, 20.0, 50.0, Envelope::Linear, 0)
            && attack == attackBefore,
        "envel attack should refuse a gate outside 0 to 1, a time outside 0 to 3600 s, a gain outside 1 to 32767, an onset outside 5 to 1000 ms, a decay outside 5 ms to an hour, a NaN, no sample rate, mode 4 and no buffer without change");
    require(attack.prepare(48000.0, Mode::ApproximateTime, 3600.0, 32767.0, 1000.0, 3600000.0, Envelope::Exponential)
            && attack.latencyFrames() == 48001 + 19200,
        "envel attack should take its largest values");
}

// A two-file port's output, its input and side chain played in blocks of
// the given frames, set called with each block's first frame before it.
template <typename Port>
std::vector<double> playPairBlocks(Port& port, const std::vector<double>& input, const std::vector<double>& side,
    std::size_t block, const std::function<bool(std::size_t)>& set, const std::string& name)
{
    std::vector<double> output(input.size());
    for (std::size_t offset = 0; offset < input.size(); offset += block)
    {
        const auto frames = std::min(block, input.size() - offset);
        require(set(offset), name + " should take its fields");
        require(port.process(std::span{input}.subspan(offset, frames), std::span{side}.subspan(offset, frames),
                    std::span{output}.subspan(offset, frames)),
            name + " should process");
    }
    return output;
}

// CDP8's submix merge, faders, interleave and inbetween and standnew repair
// on recorded references of two files, bit for bit: in 64-frame blocks, with
// faders' gain set at each block's start where CDP's breakpoint file steps,
// replayed after reset, and in blocks of 1, 7, 256 and 333 frames and whole
// (the live gain only in blocks that divide 1024, where it steps). The
// references hold the input, the side chain (CDP's second file) and each
// output channel.
void testSubmixPairsMatchCdp()
{
    const auto fixed = [](std::size_t) { return true; };
    const auto matches = [](const std::string& name, auto& port, const std::function<bool(std::size_t)>& set,
                             bool live) {
        const auto cdp = reference(name, 2);
        require(cdp.input.size() == 4096, name + " should hold 4096 frames");
        require(sameBits(playPair(port, cdp.input, cdp.outputs[0], set, name), cdp.outputs[1]),
            name + " should match CDP8 bit for bit in 64-frame blocks");
        port.reset();
        require(sameBits(playPair(port, cdp.input, cdp.outputs[0], set, name), cdp.outputs[1]),
            name + " should replay after reset");
        for (const auto block : {std::size_t{1}, std::size_t{7}, std::size_t{256}, std::size_t{333}, cdp.input.size()})
        {
            if (live && 1024 % block != 0)
                continue;
            port.reset();
            require(sameBits(playPairBlocks(port, cdp.input, cdp.outputs[0], block, set, name), cdp.outputs[1]),
                name + " should match CDP8 in " + std::to_string(block) + "-frame blocks");
        }
    };

    struct Merge
    {
        std::string name;
        double stagger;
        double skew;
        double skip;
    };
    for (const auto& c : {
             Merge{"submix-merge-default", 0.0, talas::cdp::MixMerge::defaultSkew, 0.0},
             Merge{"submix-merge-skew-0.2", 0.0, 0.2, 0.0},
             Merge{"submix-merge-stagger-0.0123-skew-0.5", 0.0123, 0.5, 0.0},
             Merge{"submix-merge-stagger-0.03-skip-0.011-skew-1.4", 0.03, 1.4, 0.011},
             Merge{"submix-merge-stagger-0.016-skip-0.016-skew-4", 0.016, 4.0, 0.016},
             // 0.0306 s is 1468.8 frames, 1280 in sectors, as 0.03 s is,
             // and 0.0107 s 513.6, 512, as 0.011 s is: they play alike.
             Merge{"submix-merge-stagger-0.03-skip-0.011-skew-1.4", 0.0306, 1.4, 0.0107},
         })
    {
        talas::cdp::MixMerge port;
        require(port.prepare(48000.0, c.stagger, c.skew, c.skip) && port.prepared() && port.latencyFrames() == 0,
            c.name + " should prepare");
        matches(c.name, port, fixed, false);
    }

    using Line = talas::cdp::MixFaders::Line;
    struct Faders
    {
        std::string name;
        std::vector<Line> balance;
        double gain;
    };
    for (const auto& c : {
             Faders{"submix-faders-ramps-gain-0.8",
                 {{0.0, 1.0, 0.0}, {0.0123, 1.0, 0.0}, {0.03331, 0.2, 0.8}, {0.06, 3.0, 1.0}, {0.07, 0.0, 2.0}}, 0.8},
             // CDP checks each time only against the first: a line its count
             // has passed ends its ramp at once.
             Faders{"submix-faders-unordered-times",
                 {{0.0, 1.0, 0.0}, {0.05, 0.0, 1.0}, {0.02, 1.0, 1.0}, {0.07, 1.0, 3.0}}, 1.3},
             Faders{"submix-faders-one-line", {{0.0, 2.0, 3.0}}, talas::cdp::MixFaders::defaultGain},
         })
    {
        talas::cdp::MixFaders port;
        require(port.prepare(48000.0, c.balance, c.gain) && port.prepared() && port.latencyFrames() == 0,
            c.name + " should prepare");
        matches(c.name, port, fixed, false);
    }
    {
        // CDP reads its gain breakpoint file at every frame: 1, 0.5, 2.5 and
        // 0 from frames 0, 1024, 2048 and 3072.
        talas::cdp::MixFaders port;
        const std::vector<Line> balance{{0.0, 1.0, 1.0}, {0.05, 0.3, 0.7}};
        require(port.prepare(48000.0, balance, 1.0), "the live faders should prepare");
        matches(
            "submix-faders-live-gain", port,
            [&](std::size_t offset) {
                return port.setGain(offset < 1024 ? 1.0 : offset < 2048 ? 0.5 : offset < 3072 ? 2.5 : 0.0);
            },
            true);
    }

    for (const std::string name : {"submix-interleave", "repair-2-channels"})
    {
        const auto cdp = reference(name, 3);
        require(cdp.input.size() == 4096, name + " should hold 4096 frames");
        talas::cdp::MixInterleave port;
        require(port.prepare() && port.prepared() && port.latencyFrames() == 0, name + " should prepare");
        for (const auto block : {std::size_t{64}, std::size_t{64}, std::size_t{1}, std::size_t{7}, std::size_t{333},
                 cdp.input.size()})
        {
            std::vector<double> left(cdp.input.size());
            std::vector<double> right(cdp.input.size());
            for (std::size_t offset = 0; offset < cdp.input.size(); offset += block)
            {
                const auto frames = std::min(block, cdp.input.size() - offset);
                require(port.process(std::span{cdp.input}.subspan(offset, frames),
                            std::span{cdp.outputs[0]}.subspan(offset, frames), std::span{left}.subspan(offset, frames),
                            std::span{right}.subspan(offset, frames)),
                    name + " should process");
            }
            require(sameBits(left, cdp.outputs[1]) && sameBits(right, cdp.outputs[2]),
                name + " should match CDP8 bit for bit on both channels in " + std::to_string(block)
                    + "-frame blocks, after reset");
            port.reset();
        }
    }

    {
        talas::cdp::MixInbetween port;
        require(port.prepareCount(11, 5) && port.prepared() && port.ratio() == 5.0 * (1.0 / 12.0)
                && port.latencyFrames() == 0,
            "inbetween's fifth of 11 files should prepare at CDP's ratio, 5 times 1/12");
        matches("submix-inbetween-1-count-11-file-5", port, fixed, false);
    }
    {
        talas::cdp::MixInbetween port;
        require(port.prepare(0.3) && port.ratio() == 0.3, "inbetween's ratio of 0.3 should prepare");
        matches("submix-inbetween-2-ratio-0.3", port, fixed, false);
    }

    // Merge's side chain is silent until the stagger and then plays the
    // stagger less the skip late, its delay line running on across calls.
    {
        talas::cdp::MixMerge port;
        // 0.012 s is 576 frames, 512 in sectors; 0.0054 s 259, 256.
        require(port.prepare(48000.0, 0.012, 1.0, 0.0054), "a staggered merge should prepare");
        std::vector<double> ramp(1000);
        for (std::size_t n = 0; n < ramp.size(); ++n)
            ramp[n] = static_cast<double>(n + 1) / 1000.0;
        const std::vector<double> silence(1000, 0.0);
        std::vector<double> output(1000);
        const auto half = [](double side) {
            return static_cast<double>(
                static_cast<float>(std::fma(static_cast<double>(static_cast<float>(side)), 0.5, 0.0)));
        };
        require(port.process(silence, ramp, output) && output[511] == 0.0 && output[512] == half(ramp[256]),
            "merge should silence its side chain until the stagger and then play it 256 frames late");
        require(port.process(silence, ramp, output) && port.process(silence, ramp, output)
                && output[10] == half(ramp[1000 - 246]),
            "merge's delay line should run on across calls");
    }
    // Faders holds the last line's levels after it.
    {
        talas::cdp::MixFaders port;
        require(port.prepare(48000.0, std::vector<Line>{{0.0, 1.0, 0.0}, {0.001, 1.0, 3.0}}, 2.0),
            "faders should prepare a short ramp");
        const std::vector<double> input(1000, 0.5);
        const std::vector<double> side(1000, 0.25);
        std::vector<double> output(1000);
        auto played = true;
        for (int pass = 0; pass < 50; ++pass)
            played = played && port.process(input, side, output);
        const auto held = static_cast<double>(static_cast<float>(std::fma(0.25, 0.75, std::fma(0.5, 0.25, 0.0)) * 2.0));
        require(played && output[999] == held, "faders should hold the last line's levels after it");
    }
}

// A two-file port refuses to play before it is prepared, and spans of
// different sizes or a non-finite input or side chain without change.
template <typename Port>
void requirePairProcessRefusals(Port& port, const std::string& name)
{
    const std::vector<double> input{0.25, -0.5, 0.75};
    const std::vector<double> side{-0.125, 0.5, 0.0625};
    {
        Port unprepared;
        std::vector<double> output(3, 7.0);
        require(!unprepared.prepared() && !unprepared.process(input, side, output)
                && output == std::vector<double>(3, 7.0),
            name + " should refuse to play before it is prepared");
    }
    const auto before = port;
    std::vector<double> shortOutput(2, 7.0);
    std::vector<double> output(3, 7.0);
    require(!port.process(input, side, shortOutput) && !port.process(input, std::span{side}.first(2), output)
            && !port.process(std::span{input}.first(2), side, output),
        name + " should refuse spans of different sizes");
    for (const auto bad : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity()})
    {
        auto badInput = input;
        badInput[1] = bad;
        auto badSide = side;
        badSide[2] = bad;
        require(!port.process(badInput, side, output) && !port.process(input, badSide, output),
            name + " should refuse a non-finite input or side chain");
    }
    require(port == before && output == std::vector<double>(3, 7.0) && shortOutput == std::vector<double>(2, 7.0),
        name + " should be unchanged by refused calls");
}

void testSubmixPairsRejectWithoutChange()
{
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto inf = std::numeric_limits<double>::infinity();
    using Line = talas::cdp::MixFaders::Line;

    talas::cdp::MixMerge merge;
    require(merge.prepare(48000.0, 0.02, 2.0, 0.006), "merge should prepare");
    requirePairProcessRefusals(merge, "merge");
    const auto mergeBefore = merge;
    require(!merge.prepare(0.0) && !merge.prepare(-48000.0) && !merge.prepare(nan)
            && !merge.prepare(48000.0, -0.001) && !merge.prepare(48000.0, 10.001) && !merge.prepare(48000.0, nan)
            && !merge.prepare(48000.0, 0.0, 1.0 / 32767.0 - 0.0000021) && !merge.prepare(48000.0, 0.0, 32767.0000021)
            && !merge.prepare(48000.0, 0.0, 0.0) && !merge.prepare(48000.0, 0.0, nan)
            && !merge.prepare(48000.0, 0.0, inf) && !merge.prepare(48000.0, 0.0, 1.0, -0.001)
            && !merge.prepare(48000.0, 0.0, 1.0, nan)
            // 0.0107 s is 513.6 frames, 512 in sectors: past a stagger of
            // 0.0054 s, 256 frames in sectors.
            && !merge.prepare(48000.0, 0.0054, 1.0, 0.0107)
            && !merge.prepare(1.0e12, 0.01) && merge == mergeBefore,
        "merge should refuse no sample rate, a stagger outside 0 to 10 s, a skew outside 1/32767 to 32767, a negative skip, a skip past the stagger in sectors, more frames than CDP's int and a NaN without change");
    require(merge.prepare(48000.0, 10.0, 32767.0, 10.0) && merge.prepare(48000.0, 0.0, 1.0 / 32767.0)
            && merge.prepare(48000.0, -0.0000019, 32767.0000019, -0.0000019)
            && merge.prepare(48000.0, 0.0054, 1.0, 0.0106),
        "merge should take its range's ends, values within 0.000002 of them, and a skip in the stagger's sector");

    talas::cdp::MixFaders faders;
    const std::vector<Line> good{{0.0, 1.0, 2.0}, {0.5, 3.0, 4.0}};
    require(faders.prepare(48000.0, good, 2.0), "faders should prepare");
    requirePairProcessRefusals(faders, "faders");
    const auto fadersBefore = faders;
    const auto refuses = [&](const std::vector<Line>& balance, double rate, double gain) {
        return !faders.prepare(rate, balance, gain);
    };
    require(refuses({}, 48000.0, 1.0) && refuses({{0.001, 1.0, 1.0}}, 48000.0, 1.0)
            && refuses({{0.0, 1.0, 1.0}, {0.5 / 48000.0, 1.0, 0.0}}, 48000.0, 1.0)
            && refuses({{0.0, 1.0, -0.1}}, 48000.0, 1.0) && refuses({{0.0, 1.0, 1.0}, {0.1, 0.0, 0.0}}, 48000.0, 1.0)
            && refuses({{0.0, nan, 1.0}}, 48000.0, 1.0) && refuses({{0.0, 1.0, 1.0}, {inf, 1.0, 1.0}}, 48000.0, 1.0)
            && refuses({{0.0, 1.0, 1.0}, {4294967296.0 / 48000.0, 1.0, 1.0}}, 48000.0, 1.0)
            && refuses(good, 0.0, 1.0) && refuses(good, nan, 1.0) && refuses(good, 48000.0, -0.0000021)
            && refuses(good, 48000.0, 256.0000021) && refuses(good, 48000.0, nan)
            && !faders.setGain(-0.0000021) && !faders.setGain(256.0000021) && !faders.setGain(nan)
            && !faders.setGain(inf) && faders == fadersBefore,
        "faders should refuse no balance, a first time other than 0, a time within a frame of the first, a negative level, levels that sum to 0, a NaN, a time beyond 2^32 - 1 frames, no sample rate and a gain outside 0 to 256 without change");
    require(faders.setGain(-0.0000019) && faders.setGain(256.0000019), "faders' gain should snap to its range's ends");
    faders.reset();
    require(faders == fadersBefore, "faders' reset should return to prepare's gain");
    require(faders.prepare(48000.0, std::vector<Line>{{0.0, 1.0, 1.0}, {1.0 / 48000.0, 1.0, 0.0}})
            && faders.prepare(48000.0, std::vector<Line>{{0.0, 1.0, 1.0}, {0.1, 1.0, 0.0}, {1.0 / 48000.0, 0.0, 1.0}})
            && faders.prepare(48000.0, std::vector<Line>{{0.0, 0.0, 1.0}, {0.1, 1.0, 0.0}}, 0.0)
            && faders.prepare(48000.0, good, 256.0),
        "faders should take a time a frame on, unordered times, a zero level, a gain of 0 and of 256");

    talas::cdp::MixInterleave interleave;
    const std::vector<double> input{0.25, -0.5, 0.75};
    const std::vector<double> side{-0.125, 0.5, 0.0625};
    std::vector<double> left(3, 7.0);
    std::vector<double> right(3, 7.0);
    std::vector<double> shortRight(2, 7.0);
    require(!interleave.prepared() && !interleave.process(input, side, left, right),
        "interleave should refuse to play before it is prepared");
    require(interleave.prepare() && interleave.prepared() && interleave.latencyFrames() == 0,
        "interleave should prepare");
    auto badInput = input;
    badInput[0] = nan;
    auto badSide = side;
    badSide[1] = -inf;
    require(!interleave.process(input, side, left, shortRight)
            && !interleave.process(input, std::span{side}.first(2), left, right)
            && !interleave.process(badInput, side, left, right) && !interleave.process(input, badSide, left, right)
            && left == std::vector<double>(3, 7.0) && right == std::vector<double>(3, 7.0),
        "interleave should refuse spans of different sizes and a non-finite input or side chain without change");

    talas::cdp::MixInbetween inbetween;
    require(inbetween.prepare(0.4), "inbetween should prepare");
    requirePairProcessRefusals(inbetween, "inbetween");
    const auto inbetweenBefore = inbetween;
    require(!inbetween.prepare(-0.0000001) && !inbetween.prepare(1.0000001) && !inbetween.prepare(nan)
            && !inbetween.prepareCount(0, 1) && !inbetween.prepareCount(1000, 1) && !inbetween.prepareCount(3, 0)
            && !inbetween.prepareCount(3, 4) && inbetween == inbetweenBefore,
        "inbetween should refuse a ratio outside 0 to 1 and counts and files outside 1 <= file <= count <= 999 without change");
    require(inbetween.prepare(0.0) && inbetween.prepare(1.0) && inbetween.prepareCount(999, 999)
            && inbetween.ratio() == 999.0 * (1.0 / 1000.0),
        "inbetween should take ratios of 0 and 1 and the last of 999 files");
}

// A port's output in blocks of the given frames, its input (and a side
// chain, for a port that takes one) followed by its latency's silence, set
// called with each block's first frame before it: what follows the latency.
template <typename Port>
std::vector<double> playEnvelope(Port& port, std::vector<double> input, std::vector<double> side, std::size_t block,
    const std::function<bool(std::size_t)>& set, const std::string& name)
{
    const auto latency = port.latencyFrames();
    const auto length = input.size();
    input.resize(length + latency, 0.0);
    side.resize(length + latency, 0.0);
    std::vector<double> output(input.size());
    for (std::size_t offset = 0; offset < input.size(); offset += block)
    {
        const auto frames = std::min(block, input.size() - offset);
        require(set(offset), name + " should take its fields");
        const auto in = std::span<const double>{input}.subspan(offset, frames);
        const auto out = std::span<double>{output}.subspan(offset, frames);
        auto processed = false;
        if constexpr (requires { port.process(in, out); })
            processed = port.process(in, out);
        else
            processed = port.process(in, std::span<const double>{side}.subspan(offset, frames), out);
        require(processed, name + " should process");
    }
    require(std::all_of(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(latency),
                [](double sample) { return sample == 0.0; }),
        name + " should start with its latency's silence");
    return {output.begin() + static_cast<std::ptrdiff_t>(latency), output.end()};
}

// The envel references' breakpoint files, as their record script wrote
// them: the first parameter's values from frames 0, 1280, 3072, 5376 and
// 6656, the second's from 0, 768, 2304, 4608 and 6144, each step falling just
// before a 256-frame window starts.
struct EnvelopeSteps
{
    std::array<double, 5> first{};
    std::array<double, 5> second{};

    [[nodiscard]] std::pair<double, double> at(std::size_t frame) const
    {
        constexpr std::array<std::size_t, 5> firstFrames{0, 1280, 3072, 5376, 6656};
        constexpr std::array<std::size_t, 5> secondFrames{0, 768, 2304, 4608, 6144};
        std::size_t a = 0;
        std::size_t b = 0;
        for (std::size_t step = 0; step < 5; ++step)
        {
            if (firstFrames[step] <= frame)
                a = step;
            if (secondFrames[step] <= frame)
                b = step;
        }
        return {first[a], second[b]};
    }
};

// CDP8's envel warp (modes 3, 4, 5, 7, 8, 9, 10, 11, 12 and 15), envel impose
// 1 and envel replace 1, 3 and 4 on recorded references with CDP's 5.333333
// ms windows (256 frames at 48 kHz), bit for bit, latencyFrames late: in
// blocks of 64, 1, 7, 256 and 333 frames and whole, each after reset, the
// live references' fields set at each block's start where CDP's breakpoint
// files step (in blocks that divide 256, as a window starts there). The
// references hold the input, the side chain for impose and replace 1, and
// the output; where CDP8 rescales a whole file the reference holds its output
// before the rescale, clipped at -1 and 1 as CDP8 clips each sample.
void testEnvelopeProcessesMatchCdp()
{
    using Warp = talas::cdp::EnvelopeWarp;
    using M = Warp::Mode;
    constexpr double window = 5.333333;
    const auto matches = [](const std::string& name, auto& port, std::size_t latency, bool two,
                             const std::function<bool(std::size_t)>& set, bool live) {
        const auto cdp = reference(name, two ? 2 : 1);
        require(cdp.input.size() == 8192, name + " should hold 8192 frames");
        require(port.latencyFrames() == latency,
            name + ": latency " + std::to_string(port.latencyFrames()) + ", expected " + std::to_string(latency));
        const auto side = two ? cdp.outputs[0] : std::vector<double>{};
        for (const auto block : {std::size_t{64}, std::size_t{1}, std::size_t{7}, std::size_t{256}, std::size_t{333},
                 cdp.input.size()})
        {
            if (live && 256 % block != 0)
                continue;
            port.reset();
            require(sameBits(playEnvelope(port, cdp.input, side, block, set, name), cdp.outputs.back()),
                name + " should match CDP8 bit for bit, latencyFrames late, in " + std::to_string(block)
                    + "-frame blocks");
        }
    };
    const auto fixed = [](std::size_t) { return true; };

    struct Fixed
    {
        std::string name;
        M mode;
        double first;
        double second;
        int smoothing;
        std::size_t latency;
    };
    const std::vector<Fixed> warps{
        {"envel-warp-3-exaggerate-2", M::Exaggerate, 2.0, 0.0, 0, 511},
        {"envel-warp-4-attenuate-0.5", M::Attenuate, 0.5, 0.0, 0, 511},
        {"envel-warp-5-lift-0.2", M::Lift, 0.2, 0.0, 0, 511},
        // CDP8 rescales this one.
        {"envel-warp-5-lift-0.6-overload", M::Lift, 0.6, 0.0, 0, 511},
        {"envel-warp-7-flatten-5", M::Flatten, 5.0, 0.0, 0, 2 * 256 + 511},
        {"envel-warp-7-flatten-6-shifted", M::Flatten, 6.0, 0.0, 0, 2 * 256 + 511},
        {"envel-warp-8-gate-0.05", M::Gate, 0.05, 0.0, 0, 511},
        {"envel-warp-8-gate-0.05-smoothing-4", M::Gate, 0.05, 0.0, 4, 3 * 256 + 511},
        {"envel-warp-8-gate-0.05-smoothing-4-shifted", M::Gate, 0.05, 0.0, 4, 3 * 256 + 511},
        {"envel-warp-9-invert-0.25-0.5", M::Invert, 0.25, 0.5, 0, 511},
        {"envel-warp-10-limit-0.3-threshold-0.1", M::Limit, 0.3, 0.1, 0, 511},
        {"envel-warp-11-corrugate-3-5", M::Corrugate, 3.0, 5.0, 0, 4 * 256 + 511},
        {"envel-warp-11-corrugate-5-3-ripple", M::Corrugate, 5.0, 3.0, 0, 5 * 256 + 511},
        {"envel-warp-11-corrugate-4-7-ripple", M::Corrugate, 4.0, 7.0, 0, 6 * 256 + 511},
        {"envel-warp-12-expand-0.01-0.2", M::Expand, 0.01, 0.2, 0, 511},
        {"envel-warp-12-expand-0.01-0.2-smoothing-3", M::Expand, 0.01, 0.2, 3, 2 * 256 + 511},
        {"envel-warp-15-ducked-0.1-0.3", M::Ducked, 0.1, 0.3, 0, 511},
    };
    for (const auto& c : warps)
    {
        Warp port;
        require(port.prepare(48000.0, c.mode, window, c.first, c.second, c.smoothing) && port.prepared(),
            c.name + " should prepare");
        matches(c.name, port, c.latency, false, fixed, false);
    }
    {
        // CDP's buffer of 2560 frames, where its power-of-ten interpolation
        // stops sooner; and 22.05 kHz, where 5 ms is a 128-frame window.
        Warp port;
        require(port.prepare(48000.0, M::Lift, window, 0.2, 0.0, 0, 0, 0, 2560), "a 2560-frame buffer should prepare");
        matches("envel-warp-5-lift-0.2-buffer-2560", port, 511, false, fixed, false);
        require(port.prepare(22050.0, M::Lift, 5.0, 0.2) && port.windowFrames() == 128, "22.05 kHz should prepare");
        matches("envel-warp-5-lift-0.2-rate-22050", port, 255, false, fixed, false);
    }

    struct Live
    {
        std::string name;
        M mode;
        int smoothing;
        int maximumFirst;
        int maximumSecond;
        EnvelopeSteps steps;
        std::size_t latency;
    };
    const std::vector<Live> lives{
        {"envel-warp-3-live-exaggerate", M::Exaggerate, 0, 0, 0, {{0.5, 3.0, 0.6, 1.7, 8.0}, {}}, 511},
        {"envel-warp-4-live-attenuate", M::Attenuate, 0, 0, 0, {{1.0, 0.3, 0.0, 0.75, 0.5}, {}}, 511},
        {"envel-warp-5-live-lift", M::Lift, 0, 0, 0, {{0.5, 0.4, 0.0, 0.25, 0.1}, {}}, 511},
        // Flatten up to 7 reads ahead 3 windows.
        {"envel-warp-7-live-flatten", M::Flatten, 0, 7, 0, {{2.0, 7.0, 3.0, 6.0, 4.0}, {}}, 3 * 256 + 511},
        {"envel-warp-8-live-gate-smoothing-3", M::Gate, 3, 0, 0, {{0.02, 0.1, 0.005, 0.2, 0.05}, {}}, 2 * 256 + 511},
        {"envel-warp-9-live-invert", M::Invert, 0, 0, 0,
            {{0.25, 0.3, 0.27, 0.35, 0.25}, {0.4, 0.5, 0.45, 0.6, 0.4}}, 511},
        {"envel-warp-10-live-limit", M::Limit, 0, 0, 0, {{0.5, 0.2, 0.35, 0.15, 0.4}, {0.1, 0.05, 0.12, 0.15, 0.02}},
            511},
        // Trough delay up to 5 and peak separation up to 8 read ahead
        // 4 + 2 + 2 windows.
        {"envel-warp-11-live-corrugate-ripple", M::Corrugate, 0, 5, 8, {{1.0, 5.0, 2.0, 4.0, 3.0}, {2.0, 8.0, 3.0, 6.0, 5.0}},
            8 * 256 + 511},
        {"envel-warp-12-live-expand-smoothing-2", M::Expand, 2, 0, 0,
            {{0.005, 0.03, 0.002, 0.05, 0.01}, {0.1, 0.3, 0.06, 0.2, 0.15}}, 256 + 511},
        {"envel-warp-15-live-ducked", M::Ducked, 0, 0, 0, {{0.1, 0.02, 0.3, 0.05, 0.2}, {0.2, 0.1, 0.35, 0.05, 0.25}},
            511},
    };
    for (const auto& c : lives)
    {
        Warp port;
        require(port.prepare(48000.0, c.mode, window, c.steps.first[0], c.steps.second[0], c.smoothing, c.maximumFirst,
                    c.maximumSecond),
            c.name + " should prepare");
        matches(
            c.name, port, c.latency, false,
            [&](std::size_t frame) {
                const auto [first, second] = c.steps.at(frame);
                return port.setControls(first, second);
            },
            true);
    }

    using I = talas::cdp::EnvelopeImpose::Mode;
    struct Side
    {
        std::string name;
        double rate;
        I mode;
        double windowMs;
        std::size_t latency;
    };
    const std::vector<Side> sides{
        {"envel-impose-1", 48000.0, I::Impose, window, 511},
        {"envel-impose-1-window-10ms", 48000.0, I::Impose, 10.0, 1023},
        {"envel-replace-1", 48000.0, I::Replace, window, 511},
        {"envel-replace-1-rate-22050", 22050.0, I::Replace, 5.0, 255},
    };
    for (const auto& c : sides)
    {
        talas::cdp::EnvelopeImpose port;
        require(port.prepare(c.rate, c.mode, c.windowMs) && port.prepared(), c.name + " should prepare");
        matches(c.name, port, c.latency, true, fixed, false);
    }

    using T = talas::cdp::EnvelopeTarget::Mode;
    {
        talas::cdp::EnvelopeTarget port;
        require(port.prepare(48000.0, T::Level, window, 0.3), "a target of 0.3 should prepare");
        matches("envel-replace-3-target-0.3", port, 511, false, fixed, false);
    }
    for (const auto& [name, mode, steps] : {
             std::tuple{std::string{"envel-replace-3-live-target"}, T::Level,
                 std::array<double, 5>{0.3, 0.4, 0.05, 0.25, 0.2}},
             std::tuple{std::string{"envel-replace-4-live-decibels"}, T::Decibels,
                 std::array<double, 5>{-14.0, -10.0, -96.0, -12.0, -20.0}},
         })
    {
        talas::cdp::EnvelopeTarget port;
        const EnvelopeSteps targets{steps, {}};
        require(port.prepare(48000.0, mode, window, steps[0]), name + " should prepare");
        matches(
            name, port, 511, false, [&](std::size_t frame) { return port.setTarget(targets.at(frame).first); }, true);
    }
}

void testEnvelopeProcessesRejectWithoutChange()
{
    using Warp = talas::cdp::EnvelopeWarp;
    using M = Warp::Mode;
    using talas::cdp::EnvelopeWindows;
    constexpr double window = 5.333333;
    const auto nan = std::numeric_limits<double>::quiet_NaN();

    Warp warp;
    require(!warp.setControls(0.5), "envel warp should refuse its controls before it is prepared");
    require(warp.prepare(48000.0, M::Lift, window, 0.2), "envel warp should prepare");
    requireProcessRefusals(warp, "envel warp");
    const auto warpBefore = warp;
    require(!warp.prepare(0.0, M::Lift, window, 0.2) && !warp.prepare(nan, M::Lift, window, 0.2)
            && !warp.prepare(48000.0, M::Lift, 4.99, 0.2) && !warp.prepare(48000.0, M::Lift, 10000.01, 0.2)
            && !warp.prepare(48000.0, M::Lift, window, 1.01) && !warp.prepare(48000.0, M::Lift, window, -0.01)
            && !warp.prepare(48000.0, M::Exaggerate, window, 0.009) && !warp.prepare(48000.0, M::Exaggerate, window, 100.5)
            && !warp.prepare(48000.0, M::Attenuate, window, 1.5) && !warp.prepare(48000.0, M::Flatten, window, 1.0)
            && !warp.prepare(48000.0, M::Flatten, window, 5001.0)
            && !warp.prepare(48000.0, M::Flatten, window, 4.0, 0.0, 0, 6000)
            && !warp.prepare(48000.0, M::Gate, window, 0.1, 0.0, -1)
            && !warp.prepare(48000.0, M::Gate, window, 0.1, 0.0, 32768)
            && !warp.prepare(48000.0, M::Gate, window, 0.1, 0.0, 4096)
            && !warp.prepare(48000.0, M::Invert, window, 0.3, 0.3)
            && !warp.prepare(48000.0, M::Invert, window, 0.3 - 0.000001, 0.3)
            && !warp.prepare(48000.0, M::Limit, window, 0.2, 0.3) && !warp.prepare(48000.0, M::Expand, window, 0.3, 0.2)
            && !warp.prepare(48000.0, M::Ducked, window, 0.3, 1.2) && !warp.prepare(48000.0, M::Corrugate, window, 0.4, 4.0)
            && !warp.prepare(48000.0, M::Corrugate, window, 3.0, 1.0)
            && !warp.prepare(48000.0, M::Lift, window, 0.2, 0.0, 0, 0, 0, 0)
            && !warp.prepare(48000.0, M::Flatten, 1000.0, 100.0) && warp == warpBefore,
        "envel warp should refuse no sample rate, a window outside 5 to 10000 ms, values outside its modes' ranges, a flatten maximum over 5000, smoothing outside 0 to 32767 or past the latency limit, an invert gate within 0.000002 of its mirror, a limit under its threshold, an expand gate over its threshold, a trough delay under 1, a peak separation under 2, no buffer and a latency over 2^20 frames without change");
    require(warp.prepare(48000.0, M::Invert, window, 0.3 - 0.000002, 0.3) && warp.prepare(48000.0, M::Limit, window, 0.3, 0.3)
            && warp.prepare(48000.0, M::Lift, 10000.0, 0.2) && warp.latencyFrames() == 959999,
        "envel warp should take an invert gate 0.000002 under its mirror, a limit at its threshold and a 10000 ms window");
    require(EnvelopeWindows::windowFrames(48000.0, 5.0) == 256 && EnvelopeWindows::windowFrames(48000.0, 10.0) == 512
            && EnvelopeWindows::windowFrames(48000.0, 8.0) == 512 && EnvelopeWindows::windowFrames(48000.0, 7.9) == 256
            && EnvelopeWindows::windowFrames(22050.0, 5.0) == 128 && EnvelopeWindows::windowFrames(8000.0, 5.0) == 32
            && EnvelopeWindows::windowFrames(8000.0, 6.0) == 64 && EnvelopeWindows::windowFrames(48000.0, 4.0) == 0,
        "CDP's windows should round to multiples of 256 frames, or power-of-two fractions of 256");

    require(warp.prepare(48000.0, M::Invert, window, 0.02, 0.3), "an invert should prepare");
    const auto invertBefore = warp;
    require(!warp.setControls(0.3, 0.3) && !warp.setControls(nan, 0.3) && !warp.setControls(0.1, 1.1)
            && warp == invertBefore,
        "envel warp should refuse a gate at its mirror, a NaN and a mirror over 1 without change");
    require(warp.prepare(48000.0, M::Flatten, window, 3.0, 0.0, 0, 6) && !warp.setControls(7.0) && !warp.setControls(1.0)
            && warp.setControls(6.4) && !warp.setControls(6.6),
        "a flatten prepared to 6 should take up to 6 (6.4 rounded) and refuse 7 and 1");
    require(warp.prepare(48000.0, M::Corrugate, window, 3.0, 5.0, 0, 4, 9) && warp.setControls(4.0, 9.0)
            && !warp.setControls(5.0, 9.0) && !warp.setControls(4.0, 10.0),
        "a corrugate should take its maxima and refuse a trough delay or peak separation over them");

    talas::cdp::EnvelopeImpose impose;
    require(!impose.prepare(48000.0, talas::cdp::EnvelopeImpose::Mode::Impose, 3.0),
        "envel impose should refuse a 3 ms window");
    require(impose.prepare(48000.0, talas::cdp::EnvelopeImpose::Mode::Replace, window) && impose.latencyFrames() == 511,
        "envel replace 1 should prepare, two windows less a frame late");
    requirePairProcessRefusals(impose, "envel replace 1");

    using T = talas::cdp::EnvelopeTarget::Mode;
    std::vector<double> input(4096);
    for (std::size_t n = 0; n < input.size(); ++n)
        input[n] = 0.3 * std::sin(0.05 * static_cast<double>(n)) * (n % 1000 < 600 ? 1.0 : 0.01);
    for (const auto& [decibels, level] : {std::pair{-0.000001, 1.0}, std::pair{0.0, 1.0}, std::pair{-96.0, 0.0}})
    {
        talas::cdp::EnvelopeTarget inDecibels;
        talas::cdp::EnvelopeTarget asLevel;
        std::vector<double> a(input.size());
        std::vector<double> b(input.size());
        require(inDecibels.prepare(48000.0, T::Decibels, window, decibels) && asLevel.prepare(48000.0, T::Level, window, level)
                && inDecibels.process(input, a) && asLevel.process(input, b) && a == b,
            "a target of " + std::to_string(decibels) + " dB should play as a level of " + std::to_string(level));
    }
    talas::cdp::EnvelopeTarget target;
    require(!target.setTarget(0.5), "envel replace should refuse a target before it is prepared");
    require(!target.prepare(48000.0, T::Level, window, 1.01) && !target.prepare(48000.0, T::Decibels, window, 0.1)
            && !target.prepare(48000.0, T::Decibels, window, -96.5),
        "envel replace should refuse a level over 1 and decibels outside -96 to 0");
    require(target.prepare(48000.0, T::Decibels, window, -6.0), "envel replace 4 should prepare");
    requireProcessRefusals(target, "envel replace 4");
    const auto targetBefore = target;
    require(!target.setTarget(-97.0) && !target.setTarget(nan) && target == targetBefore,
        "envel replace should refuse a live target under -96 dB and a NaN without change");
}

// A CDP8 reference's columns: its inputs, then each output channel.
std::vector<std::vector<double>> referenceColumns(const std::string& name)
{
    std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + name + ".txt");
    require(file.good(), "CDP reference " + name + " should be readable");
    std::vector<std::vector<double>> columns;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line.front() == '#')
            continue;
        const char* position = line.c_str();
        for (std::size_t column = 0;; ++column)
        {
            char* end = nullptr;
            const auto value = std::strtod(position, &end);
            if (end == position)
                break;
            if (columns.size() <= column)
                columns.emplace_back();
            columns[column].push_back(value);
            position = end;
        }
    }
    require(!columns.empty() && !columns[0].empty(), "CDP reference " + name + " should hold samples");
    return columns;
}

// A breakpoint file a CDP8 reference's header records ("# name.brk: value
// at frame f / ..."), its times frame / rate, read between its points as
// CDP's read_value_from_brktable reads it.
struct Breakpoints
{
    std::vector<double> times;
    std::vector<double> values;

    [[nodiscard]] double at(double time) const
    {
        if (time <= times.front())
            return values.front();
        if (time >= times.back())
            return values.back();
        std::size_t point = 0;
        while (times[point] < time)
            ++point;
        auto value = (time - times[point - 1]) / (times[point] - times[point - 1]);
        value *= (values[point] - values[point - 1]);
        value += values[point - 1];
        return value;
    }

    [[nodiscard]] double lowest() const { return *std::min_element(values.begin(), values.end()); }
};

Breakpoints referenceBreakpoints(const std::string& name, const std::string& table, double rate)
{
    std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + name + ".txt");
    require(file.good(), "CDP reference " + name + " should be readable");
    const auto label = "# " + table + ".brk: ";
    std::string line;
    while (std::getline(file, line))
    {
        if (line.rfind(label, 0) != 0)
            continue;
        Breakpoints result;
        std::istringstream items(line.substr(label.size()));
        std::string item;
        while (std::getline(items, item, '/'))
        {
            char* end = nullptr;
            const auto value = std::strtod(item.c_str(), &end);
            const auto frame = item.find("frame");
            require(end != item.c_str() && frame != std::string::npos,
                "CDP reference " + name + "'s " + table + " breakpoints should be readable");
            result.times.push_back(std::strtod(item.c_str() + frame + 5, nullptr) / rate);
            result.values.push_back(value);
        }
        require(!result.times.empty(), "CDP reference " + name + " should hold " + table + " breakpoints");
        return result;
    }
    require(false, "CDP reference " + name + " should record its " + table + " breakpoints");
    return {};
}

// CDP8's output before its whole-file rescale, clipped at -1 and 1 as the
// stream clips it; the reference passes 1 there.
std::vector<double> clippedSamples(const std::vector<double>& samples, const std::string& name)
{
    require(std::any_of(samples.begin(), samples.end(), [](double sample) { return std::abs(sample) > 1.0; }),
        name + "'s unscaled output should pass 1");
    auto clipped = samples;
    for (auto& sample : clipped)
        sample = std::clamp(sample, -1.0, 1.0);
    return clipped;
}

// An interleaving port's output channels in blocks of the given frames, its
// input (and right input, for a port that takes two) followed by its
// latency's silence, set(frame, frames) called before each block: each
// channel from the latency on.
template <typename Port>
std::vector<std::vector<double>> playChannels(Port& port, std::vector<double> left, std::vector<double> right,
    std::size_t block, const std::function<bool(std::size_t, std::size_t)>& set, const std::string& name)
{
    const auto latency = port.latencyFrames();
    const auto channels = port.outputChannels();
    const auto length = left.size();
    left.resize(length + latency, 0.0);
    right.resize(length + latency, 0.0);
    std::vector<double> output(left.size() * channels);
    for (std::size_t offset = 0; offset < left.size(); offset += block)
    {
        const auto frames = std::min(block, left.size() - offset);
        require(set(offset, frames), name + " should take its fields");
        const auto in = std::span<const double>{left}.subspan(offset, frames);
        const auto out = std::span<double>{output}.subspan(offset * channels, frames * channels);
        auto processed = false;
        if constexpr (requires { port.process(in, out); })
            processed = port.process(in, out);
        else
            processed = port.process(in, std::span<const double>{right}.subspan(offset, frames), out);
        require(processed, name + " should process");
    }
    std::vector<std::vector<double>> played(channels);
    for (std::size_t channel = 0; channel < channels; ++channel)
    {
        for (std::size_t frame = latency; frame < left.size(); ++frame)
            played[channel].push_back(output[frame * channels + channel]);
    }
    return played;
}

// The same bits over the first frames frames.
bool sameLeadingBits(const std::vector<double>& played, const std::vector<double>& cdp, std::size_t frames)
{
    return played.size() >= frames && cdp.size() >= frames
        && sameBits({played.begin(), played.begin() + static_cast<std::ptrdiff_t>(frames)},
            {cdp.begin(), cdp.begin() + static_cast<std::ptrdiff_t>(frames)});
}

// CDP8's newdelay, bounce -s0 and rejoin on recorded references, bit for bit:
// in blocks of 64, 1, 7 and 333 frames and whole, each after reset, a live
// pitch set at each block's start where CDP's breakpoint file steps (just
// before a 64-frame block), and a glide's set before every frame. Where CDP8
// rescales its whole file, the reference holds its output before the
// rescale, which the port clips at -1 and 1.
void testPitchedDelayBounceAndRejoinMatchCdp()
{
    const auto fixed = [](std::size_t) { return true; };
    const auto blocks = [](std::size_t whole) {
        return std::vector<std::size_t>{64, 1, 7, 333, whole};
    };

    struct Delay
    {
        std::string name;
        double rate;
        double pitch;
        double mix;
        double feedback;
        std::size_t channels;
        bool overload;
    };
    const std::vector<Delay> delays{
        {"newdelay-60-mix-0.5-feedback-0.7", 48000.0, 60.0, 0.5, 0.7, 1, false},
        {"newdelay-45.5-mix-0.8-feedback-minus-0.6", 48000.0, 45.5, 0.8, -0.6, 1, false},
        {"newdelay-84-mix-1-feedback-0.95", 48000.0, 84.0, 1.0, 0.95, 1, false},
        {"newdelay-138.2-mix-0.3-feedback-0.6", 48000.0, 138.2, 0.3, 0.6, 1, false},
        {"newdelay-70-mix-0.6-feedback-0.9-rate-44100", 44100.0, 70.0, 0.6, 0.9, 1, false},
        // CDP delays each channel of a stereo file alike.
        {"newdelay-stereo-55-mix-0.5-feedback-0.8", 48000.0, 55.0, 0.5, 0.8, 2, false},
        {"newdelay-64-mix-0.5-feedback-0.8-overload", 48000.0, 64.0, 0.5, 0.8, 1, true},
    };
    for (const auto& c : delays)
    {
        const auto columns = referenceColumns(c.name);
        require(columns.size() == 2 * c.channels, c.name + " should hold its inputs and outputs");
        for (std::size_t channel = 0; channel < c.channels; ++channel)
        {
            const auto& cdp = columns[c.channels + channel];
            const auto expected = c.overload ? clippedSamples(cdp, c.name) : cdp;
            talas::cdp::PitchedDelay port;
            require(port.prepare(c.rate, c.pitch, c.mix, c.feedback) && port.prepared()
                    && static_cast<long>(port.delayFrames()) == talas::cdp::PitchedDelay::delayForPitch(c.rate, c.pitch),
                c.name + " should prepare a period of its pitch");
            for (const auto block : blocks(columns[0].size()))
            {
                port.reset();
                require(sameBits(playEnvelope(port, columns[channel], {}, block, fixed, c.name), expected),
                    c.name + " channel " + std::to_string(channel + 1) + " should match CDP8 bit for bit in "
                        + std::to_string(block) + "-frame blocks");
            }
        }
    }
    for (const auto& [name, mix, feedback] : {
             std::tuple{std::string{"newdelay-live-pitch"}, 0.5, 0.75},
             std::tuple{std::string{"newdelay-glide"}, 0.6, 0.8},
         })
    {
        const auto columns = referenceColumns(name);
        const auto pitches = referenceBreakpoints(name, "pitch", 48000.0);
        const auto glide = name.ends_with("glide");
        // CDP's time for a frame: the frame times 1 / 48000.
        const auto convertor = 1.0 / 48000.0;
        const auto pitchAt = [&](std::size_t frame) { return pitches.at(static_cast<double>(frame) * convertor); };
        talas::cdp::PitchedDelay port;
        require(port.prepare(48000.0, pitchAt(0), mix, feedback, pitches.lowest()), name + " should prepare");
        const auto set = [&](std::size_t frame) { return port.setPitch(pitchAt(frame)); };
        for (const auto block : glide ? std::vector<std::size_t>{1} : std::vector<std::size_t>{64, 1, 32})
        {
            port.reset();
            require(sameBits(playEnvelope(port, columns[0], {}, block, set, name), columns[1]),
                name + " should match CDP8 bit for bit with its pitch set before " + std::to_string(block)
                    + "-frame blocks");
        }
        if (glide)
        {
            port.reset();
            require(!sameBits(playEnvelope(port, columns[0], {}, 64, set, name), columns[1]),
                "a glide held over 64-frame blocks should differ from CDP8's");
        }
    }

    struct Bounces
    {
        std::string name;
        double rate;
        int count;
        double gap;
        double shorten;
        double end;
        double warp;
        std::size_t copies;
        bool overload;
    };
    const std::vector<Bounces> bounces{
        {"bounce-5-gap-0.05-shorten-0.8-end-0.1-warp-1", 48000.0, 5, 0.05, 0.8, 0.1, 1.0, 5, false},
        {"bounce-12-gap-0.04-shorten-0.9-end-0.05-warp-2-clicks", 48000.0, 12, 0.04, 0.9, 0.05, 2.0, 12, false},
        // The gaps fall under half CDP's splice after 65 copies.
        {"bounce-100-gap-0.04-shorten-0.95-end-0-warp-0.5-curtailed", 48000.0, 100, 0.04, 0.95, 0.0, 0.5, 65, false},
        {"bounce-3-gap-0.07-shorten-1-end-1-warp-3-clicks", 48000.0, 3, 0.07, 1.0, 1.0, 3.0, 3, false},
        {"bounce-8-gap-0.04-shorten-0.7-end-0.2-warp-1.5-rate-44100", 44100.0, 8, 0.04, 0.7, 0.2, 1.5, 8, false},
        {"bounce-6-gap-0.04-shorten-0.85-end-0.3-warp-1-overload", 48000.0, 6, 0.04, 0.85, 0.3, 1.0, 6, true},
    };
    for (const auto& c : bounces)
    {
        const auto columns = referenceColumns(c.name);
        require(columns.size() == 2, c.name + " should hold its input and output");
        const auto expected = c.overload ? clippedSamples(columns[1], c.name) : columns[1];
        talas::cdp::Bounce port;
        require(port.prepare(c.rate, c.count, c.gap, c.shorten, c.end, c.warp) && port.bounces() == c.copies,
            c.name + " should prepare " + std::to_string(c.copies) + " copies");
        for (const auto block : blocks(columns[0].size()))
        {
            port.reset();
            require(sameBits(playEnvelope(port, columns[0], {}, block, fixed, c.name), expected),
                c.name + " should match CDP8 bit for bit in " + std::to_string(block) + "-frame blocks");
        }
    }

    for (const auto& [name, gain, overload] : {
             std::tuple{std::string{"rejoin-1-gain-1"}, 1.0, false},
             std::tuple{std::string{"rejoin-1-gain-0.6"}, 0.6, false},
             std::tuple{std::string{"rejoin-2-gain-0.35"}, 0.35, false},
             std::tuple{std::string{"rejoin-1-gain-0.9-rate-44100"}, 0.9, false},
             std::tuple{std::string{"rejoin-1-gain-1-overload"}, 1.0, true},
         })
    {
        const auto columns = referenceColumns(name);
        require(columns.size() == 3, name + " should hold its two inputs and output");
        const auto expected = overload ? clippedSamples(columns[2], name) : columns[2];
        talas::cdp::Rejoin port;
        require(port.prepare(gain) && port.prepared(), name + " should prepare");
        for (const auto block : blocks(columns[0].size()))
        {
            require(sameBits(playEnvelope(port, columns[0], columns[1], block, fixed, name), expected),
                name + " should match CDP8 bit for bit in " + std::to_string(block) + "-frame blocks");
        }
        // CDP adds the other file to its longest: swapped, the fused sum
        // rounds otherwise.
        if (name == "rejoin-1-gain-0.6")
        {
            require(!sameBits(playEnvelope(port, columns[1], columns[0], 64, fixed, name), expected),
                "rejoin's inputs swapped should differ from CDP8's");
        }
    }
}

void testPitchedDelayBounceAndRejoinRejectWithoutChange()
{
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    using talas::cdp::PitchedDelay;
    const auto lowest = PitchedDelay::lowestPitch();
    const auto highest = PitchedDelay::highestPitch(48000.0);
    require(lowest < -76.2 && lowest > -76.3 && highest > 138.2 && highest < 138.3,
        "newdelay's pitches should run from 0.1 Hz's to the Nyquist frequency's");
    PitchedDelay delay;
    require(!delay.setPitch(60.0), "newdelay should refuse a pitch before it is prepared");
    require(delay.prepare(48000.0, 60.0, 0.5, 0.5, 40.0), "newdelay should prepare");
    requireProcessRefusals(delay, "newdelay");
    const auto delayBefore = delay;
    require(!delay.prepare(0.0, 60.0, 0.5, 0.5) && !delay.prepare(nan, 60.0, 0.5, 0.5)
            && !delay.prepare(48000.0, nan, 0.5, 0.5) && !delay.prepare(48000.0, highest + 0.00001, 0.5, 0.5)
            && !delay.prepare(48000.0, lowest - 0.00001, 0.5, 0.5) && !delay.prepare(48000.0, 60.0, 0.0009, 0.5)
            && !delay.prepare(48000.0, 60.0, 1.00001, 0.5) && !delay.prepare(48000.0, 60.0, 0.5, -1.00001)
            && !delay.prepare(48000.0, 60.0, 0.5, 1.00001) && !delay.prepare(48000.0, 60.0, 0.5, 0.5, 200.0)
            && !delay.setPitch(39.9) && !delay.setPitch(nan) && !delay.setPitch(highest + 0.1) && delay == delayBefore,
        "newdelay should refuse no sample rate, a pitch outside 0.1 Hz's to the Nyquist frequency's, a mix outside 0.001 to 1, a feedback outside -1 to 1, a lowest pitch past the Nyquist frequency's and a live pitch under its lowest without change");
    require(delay.setPitch(40.0) && delay.delayFrames() == 582 && delay.setPitch(96.0),
        "newdelay should take its lowest pitch, 582 frames at 48 kHz, and a higher one");
    delay.reset();
    require(static_cast<long>(delay.delayFrames()) == PitchedDelay::delayForPitch(48000.0, 60.0),
        "newdelay's reset should return to prepare's pitch");
    require(delay.prepare(48000.0, highest + 0.000001, 0.5, 0.5) && delay.delayFrames() == 2
            && delay.prepare(48000.0, lowest - 0.000001, 0.0009999, -1.000001) && delay.delayFrames() == 480000,
        "newdelay should take its ranges' ends within 0.000002, 2 frames at the top at 48 kHz and 480000 at 0.1 Hz");
    PitchedDelay above;
    require(above.prepare(48000.0, 60.0, 0.5, 0.5, 70.0) && !above.setPitch(59.0),
        "a lowest pitch above the pitch should be the pitch");

    talas::cdp::Bounce bounce;
    require(bounce.prepare(48000.0, 4, 0.05, 0.5, 0.25, 1.0) && bounce.bounces() == 4 && bounce.delayFrames(0) == 2400
            && bounce.delayFrames(1) == 3600 && bounce.delayFrames(2) == 4200 && bounce.delayFrames(3) == 4500
            && bounce.level(0) == 0.75 * 0.75 + 0.25 && bounce.level(3) == 0.25,
        "bounce should prepare gaps of 2400, 1200, 600 and 300 frames at levels from 0.8125 to 0.25");
    requireProcessRefusals(bounce, "bounce");
    const auto bounceBefore = bounce;
    require(!bounce.prepare(0.0, 5, 0.05, 0.8, 0.1, 1.0) && !bounce.prepare(48000.0, 0, 0.05, 0.8, 0.1, 1.0)
            && !bounce.prepare(48000.0, 101, 0.05, 0.8, 0.1, 1.0) && !bounce.prepare(48000.0, 5, 0.0399, 0.8, 0.1, 1.0)
            && !bounce.prepare(48000.0, 5, 10.001, 0.8, 0.1, 1.0) && !bounce.prepare(48000.0, 5, 0.05, 0.0999, 0.1, 1.0)
            && !bounce.prepare(48000.0, 5, 0.05, 1.001, 0.1, 1.0) && !bounce.prepare(48000.0, 5, 0.05, 0.8, -0.001, 1.0)
            && !bounce.prepare(48000.0, 5, 0.05, 0.8, 1.001, 1.0) && !bounce.prepare(48000.0, 5, 0.05, 0.8, 0.1, 0.0099)
            && !bounce.prepare(48000.0, 5, 0.05, 0.8, 0.1, 100.01) && !bounce.prepare(48000.0, 5, 0.05, 0.8, nan, 1.0)
            && !bounce.prepare(48000.0, 10, 10.0, 1.0, 0.1, 1.0) && bounce == bounceBefore,
        "bounce should refuse no sample rate, values outside its ranges, a NaN and copies past 2^22 frames without change");
    require(bounce.prepare(48000.0, 8, 10.0, 1.0, 0.1, 1.0) && bounce.delayFrames(7) == 3840000
            && bounce.prepare(48000.0, 5, 0.039999, 0.099999, 0.5, 100.000001) && bounce.delayFrames(0) == 1920
            && bounce.bounces() == 2 && bounce.delayFrames(1) == 2112,
        "bounce should take 8 copies 10 s apart and its ranges' ends within 0.000002, stopping where a gap falls under 72 frames");

    talas::cdp::Rejoin rejoin;
    require(rejoin.prepare(0.5) && rejoin.latencyFrames() == 0, "rejoin should prepare");
    requirePairProcessRefusals(rejoin, "rejoin");
    const auto rejoinBefore = rejoin;
    require(!rejoin.prepare(0.0) && !rejoin.prepare(0.0000019) && !rejoin.prepare(1.0001) && !rejoin.prepare(-0.5)
            && !rejoin.prepare(nan) && rejoin == rejoinBefore,
        "rejoin should refuse a gain within 0.000002 of 0, over 1, negative or a NaN without change");
    require(rejoin.prepare(1.0000019) && rejoin.prepare(0.0000021),
        "rejoin should take a gain within 0.000002 of 1 and one just past 0.000002");
}

// CDP8's cascade modes 1 to 5 on recorded references, each output channel
// bit for bit, latencyFrames late, in blocks of 64, 1, 7 and 333 frames and
// whole, each after reset, before the place of the clip CDP8 shortens where
// its file ends.
void testCascadeMatchesCdp()
{
    using M = talas::cdp::Cascade::Mode;
    const auto settings = [](M mode, double clip, int echoes, double clipMax = 0.0, int echoesMax = 0,
                              double rand = 0.0, int seed = 1, bool linear = false) {
        talas::cdp::Cascade::Settings fields;
        fields.mode = mode;
        fields.clipSize = clip;
        fields.echoes = echoes;
        fields.clipMax = clipMax;
        fields.echoesMax = echoesMax;
        fields.rand = rand;
        fields.seed = seed;
        fields.linear = linear;
        return fields;
    };
    // Where CDP shortens its last clip: the place of the clip the file ends
    // in, else the file's end.
    const auto compared = [](const talas::cdp::Cascade::Settings& fields, double rate, const std::vector<double>& input) {
        talas::cdp::Cascade port;
        require(port.prepare(rate, fields), "cascade should prepare");
        std::vector<double> out(port.outputChannels());
        for (std::size_t frame = 0; frame < input.size(); ++frame)
        {
            if (port.readStart() + port.clipFrames() > input.size())
                return port.clipStart();
            require(port.process(std::span<const double>{input}.subspan(frame, 1), out), "cascade should process");
        }
        return input.size();
    };
    struct Case
    {
        std::string name;
        double rate;
        talas::cdp::Cascade::Settings fields;
        bool overload;
    };
    const std::vector<Case> cases{
        {"cascade-1-clip-0.0213333-echoes-3", 48000.0, settings(M::Echoes, 0.021333333, 3), false},
        {"cascade-1-clip-0.0106667-echoes-8-linear", 48000.0, settings(M::Echoes, 0.010666667, 8, 0, 0, 0, 1, true),
            false},
        {"cascade-2-clip-0.0426667-echoes-4", 48000.0, settings(M::PanRight, 0.042666667, 4), false},
        {"cascade-3-clip-0.0213333-echoes-5", 48000.0, settings(M::Alternate, 0.021333333, 5), false},
        {"cascade-4-clip-0.0106667-echoes-9", 48000.0, settings(M::Rotate, 0.010666667, 9), false},
        {"cascade-5-clip-0.0213333-echoes-4", 48000.0, settings(M::RotateBack, 0.021333333, 4), false},
        {"cascade-1-clip-0.0213333-echoes-6-rand-0.5-seed-3", 48000.0, settings(M::Echoes, 0.021333333, 6, 0, 0, 0.5, 3),
            false},
        {"cascade-4-clip-0.0106667-echoes-5-rand-0.7-seed-5", 48000.0, settings(M::Rotate, 0.010666667, 5, 0, 0, 0.7, 5),
            false},
        {"cascade-2-clip-0.0125-to-0.03125-echoes-2-to-6", 48000.0, settings(M::PanRight, 0.0125, 2, 0.03125, 6), false},
        {"cascade-5-clip-0.0225-echoes-3-rate-44100", 44100.0, settings(M::RotateBack, 0.0225, 3), false},
        {"cascade-1-clip-0.0213333-echoes-12-overload", 48000.0, settings(M::Echoes, 0.021333333, 12), true},
    };
    const auto none = [](std::size_t, std::size_t) { return true; };
    for (const auto& c : cases)
    {
        const auto columns = referenceColumns(c.name);
        talas::cdp::Cascade port;
        require(port.prepare(c.rate, c.fields), c.name + " should prepare");
        const auto channels = port.outputChannels();
        require(columns.size() == 1 + channels
                && channels == (c.fields.mode == M::Echoes ? 1u : c.fields.mode == M::PanRight
                                        || c.fields.mode == M::Alternate ? 2u : 8u),
            c.name + " should hold its input and each output channel");
        const auto upTo = compared(c.fields, c.rate, columns[0]);
        require(upTo > columns[0].size() / 2, c.name + " should compare most of its frames");
        for (const auto block : {std::size_t{64}, std::size_t{1}, std::size_t{7}, std::size_t{333}, columns[0].size()})
        {
            port.reset();
            const auto played = playChannels(port, columns[0], {}, block, none, c.name);
            for (std::size_t channel = 0; channel < channels; ++channel)
            {
                const auto expected = c.overload && channel == 0 ? clippedSamples(columns[1], c.name)
                                                                 : columns[1 + channel];
                require(sameLeadingBits(played[channel], expected, upTo),
                    c.name + " channel " + std::to_string(channel + 1) + " should match CDP8 bit for bit in "
                        + std::to_string(block) + "-frame blocks");
            }
        }
    }
}

void testCascadeRejectsWithoutChange()
{
    using M = talas::cdp::Cascade::Mode;
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto settings = [](M mode, double clip, int echoes, double clipMax = 0.0, int echoesMax = 0,
                              double rand = 0.0, int seed = 1) {
        talas::cdp::Cascade::Settings fields;
        fields.mode = mode;
        fields.clipSize = clip;
        fields.echoes = echoes;
        fields.clipMax = clipMax;
        fields.echoesMax = echoesMax;
        fields.rand = rand;
        fields.seed = seed;
        return fields;
    };
    talas::cdp::Cascade port;
    std::vector<double> in(16, 0.1);
    std::vector<double> out(16, 0.0);
    require(!port.prepared() && !port.process(in, out), "cascade should refuse to play before it is prepared");
    require(port.prepare(48000.0, settings(M::Rotate, 0.02, 3, 0.03, 5, 0.5, 2)) && port.outputChannels() == 8
            && port.latencyFrames() == 1440 + 480 - 1,
        "cascade with draws should prepare eight channels, the longest clip and two splices less a frame late");
    const auto before = port;
    std::vector<double> out8(128);
    auto bad = in;
    bad[3] = nan;
    require(!port.process(in, out) && !port.process(bad, out8) && port == before,
        "cascade should refuse an output not eight times its input and a NaN without change");
    require(!port.prepare(0.0, settings(M::Echoes, 0.02, 3)) && !port.prepare(nan, settings(M::Echoes, 0.02, 3))
            && !port.prepare(48000.0, settings(static_cast<M>(9), 0.02, 3))
            && !port.prepare(48000.0, settings(M::Echoes, 0.0049, 3))
            && !port.prepare(48000.0, settings(M::Echoes, 60.01, 3)) && !port.prepare(48000.0, settings(M::Echoes, 0.02, 0))
            && !port.prepare(48000.0, settings(M::Echoes, 0.02, 65))
            && !port.prepare(48000.0, settings(M::Echoes, 0.02, 3, 0.004))
            && !port.prepare(48000.0, settings(M::Echoes, 0.02, 3, 0.0, 65))
            && !port.prepare(48000.0, settings(M::Echoes, 0.02, 3, 0.0, 0, 1.01))
            && !port.prepare(48000.0, settings(M::Echoes, 0.02, 3, 0.0, 0, 0.5, 0))
            && !port.prepare(48000.0, settings(M::Echoes, 0.02, 3, 0.0, 0, 0.0, 65))
            && !port.prepare(48000.0, settings(M::Echoes, 2.0, 64)) && port == before,
        "cascade should refuse no sample rate, an unknown mode, a clip outside 0.005 to 60 s, echoes outside 1 to 64, a clip maximum under 0.005 s, a rand over 1, seed 0 with draws, seed 65 and an echo set past 2^22 frames without change");
    require(port.prepare(48000.0, settings(M::Echoes, 0.02, 3, 0.0, 0, 0.0, 0))
            && port.prepare(48000.0, settings(M::Echoes, 0.0049999, 3)) && port.clipFrames() == 240
            && port.latencyFrames() == 719,
        "cascade should take seed 0 with nothing drawn and a clip within 0.000002 of 0.005 s");
    port.reset();
    std::vector<double> mono(16);
    require(port.process(in, mono), "cascade's echoes should play one channel");
}

// CDP8's phasor on recorded references, each output channel bit for bit,
// latencyFrames late, in blocks of 64, 1, 7 and 333 frames and whole, each
// after reset; a live Phasfrq and Shift set before the block each packet
// starts in, at CDP's packet times (blocks in which two packets start are
// not played).
void testPhasorMatchesCdp()
{
    using talas::cdp::Phasor;
    struct Case
    {
        std::string name;
        double rate;
        int streams;
        double frequency;
        double shift;
        int channels;
        double offset;
        bool surround;
        bool overload;
        bool liveFrequency;
        bool liveShift;
    };
    const std::vector<Case> cases{
        {"phasor-2-streams-10hz-shift-1-2-channels", 48000.0, 2, 10.0, 1.0, 2, 0.0, false, false, false, false},
        {"phasor-3-streams-5hz-shift-2-3-channels", 48000.0, 3, 5.0, 2.0, 3, 0.0, false, false, false, false},
        {"phasor-4-streams-20hz-shift-3-2-channels-offset-10", 48000.0, 4, 20.0, 3.0, 2, 10.0, false, false, false,
            false},
        {"phasor-5-streams-7hz-shift-0.5-4-channels-surround", 48000.0, 5, 7.0, 0.5, 4, 0.0, true, false, false, false},
        {"phasor-8-streams-2.5hz-shift-12-8-channels", 48000.0, 8, 2.5, 12.0, 8, 0.0, false, false, false, false},
        {"phasor-2-streams-100hz-shift-12-1-channel", 48000.0, 2, 100.0, 12.0, 1, 0.0, false, false, false, false},
        {"phasor-3-streams-10hz-shift-0-3-channels", 48000.0, 3, 10.0, 0.0, 3, 0.0, false, false, false, false},
        {"phasor-6-streams-37hz-shift-5-3-channels-offset-250-rate-44100", 44100.0, 6, 37.0, 5.0, 3, 250.0, false,
            false, false, false},
        {"phasor-2-streams-100hz-shift-0.5-2-channels-rate-8000", 8000.0, 2, 100.0, 0.5, 2, 0.0, false, false, false,
            false},
        {"phasor-3-streams-10hz-live-shift-3-channels", 48000.0, 3, 10.0, 0.0, 3, 0.0, false, false, false, true},
        {"phasor-4-streams-live-phasfrq-and-shift-2-channels", 48000.0, 4, 0.0, 0.0, 2, 0.0, false, false, true, true},
        {"phasor-3-streams-9.999hz-shift-4-3-channels-offset-7.31", 48000.0, 3, 9.999, 4.0, 3, 7.31, false, false,
            false, false},
        {"phasor-8-streams-10hz-shift-1-1-channel-overload", 48000.0, 8, 10.0, 1.0, 1, 0.0, false, true, false, false},
    };
    struct Packet
    {
        std::size_t start;
        double frequency;
        double shift;
    };
    for (const auto& c : cases)
    {
        const auto columns = referenceColumns(c.name);
        const auto channels = static_cast<std::size_t>(c.channels);
        require(columns.size() == 1 + channels, c.name + " should hold its input and each output channel");
        const auto frames = columns[0].size();
        const auto live = c.liveFrequency || c.liveShift;
        const auto frequencies = c.liveFrequency ? std::optional{referenceBreakpoints(c.name, "phasfrq", c.rate)}
                                                 : std::optional<Breakpoints>{};
        const auto shifts = c.liveShift ? std::optional{referenceBreakpoints(c.name, "shift", c.rate)}
                                        : std::optional<Breakpoints>{};
        // CDP's packets: Phasfrq and Shift read at a time that advances by
        // 1 / Phasfrq a packet, the packet by P frames.
        std::vector<Packet> packets;
        {
            double time = 0.0;
            std::size_t start = 0;
            while (live && start < frames + (std::size_t{1} << 16))
            {
                const auto frequency = frequencies ? frequencies->at(time) : c.frequency;
                const auto shift = shifts ? shifts->at(time) : c.shift;
                packets.push_back({start, frequency, shift});
                start += Phasor::packetFrames(c.rate, frequency);
                time += 1.0 / frequency;
            }
        }
        auto first = c.frequency;
        auto firstShift = c.shift;
        auto lowest = std::numeric_limits<double>::quiet_NaN();
        if (live)
        {
            // CDP sizes its buffer by the lowest Phasfrq it reads within its
            // file.
            first = packets.front().frequency;
            firstShift = packets.front().shift;
            lowest = first;
            for (const auto& packet : packets)
            {
                if (packet.start < frames)
                    lowest = std::min(lowest, packet.frequency);
            }
        }
        Phasor port;
        require(port.prepare(c.rate, c.streams, first, firstShift, c.channels, c.offset, c.surround, lowest)
                && port.outputChannels() == channels
                && port.latencyFrames() == Phasor::packetFrames(c.rate, live ? lowest : c.frequency) + 1,
            c.name + " should prepare, the packet of its lowest frequency and a frame late");
        std::size_t next = 0;
        const auto set = [&](std::size_t offset, std::size_t count) {
            auto taken = true;
            while (next < packets.size() && packets[next].start < offset + count)
            {
                taken = taken && port.setControls(packets[next].frequency, packets[next].shift);
                ++next;
            }
            return taken;
        };
        std::size_t played = 0;
        for (const auto block : {std::size_t{64}, std::size_t{1}, std::size_t{7}, std::size_t{333}, frames})
        {
            // A block may hold one packet's start at most.
            auto playable = true;
            for (std::size_t index = 0; live && index + 1 < packets.size(); ++index)
            {
                if (packets[index + 1].start < frames + port.latencyFrames()
                    && packets[index].start / block == packets[index + 1].start / block)
                    playable = false;
            }
            if (!playable)
                continue;
            ++played;
            port.reset();
            next = 0;
            const auto output = playChannels(port, columns[0], {}, block, set, c.name);
            for (std::size_t channel = 0; channel < channels; ++channel)
            {
                const auto expected = c.overload ? clippedSamples(columns[1 + channel], c.name) : columns[1 + channel];
                require(sameBits(output[channel], expected),
                    c.name + " channel " + std::to_string(channel + 1) + " should match CDP8 bit for bit in "
                        + std::to_string(block) + "-frame blocks");
            }
        }
        require(played >= 2, c.name + " should play in two block sizes or more");
    }
}

void testPhasorRejectsWithoutChange()
{
    using talas::cdp::Phasor;
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    Phasor port;
    std::vector<double> in(16, 0.1);
    std::vector<double> out(32, 0.0);
    require(!port.prepared() && !port.process(in, out) && !port.setControls(10.0, 1.0),
        "phasor should refuse to play or take controls before it is prepared");
    require(!port.prepare(0.0, 2, 10.0, 1.0, 2) && !port.prepare(nan, 2, 10.0, 1.0, 2)
            && !port.prepare(48000.0, 1, 10.0, 1.0, 1) && !port.prepare(48000.0, 9, 10.0, 1.0, 2)
            && !port.prepare(48000.0, 2, 10.0, 1.0, 3) && !port.prepare(48000.0, 2, 10.0, 1.0, 0)
            && !port.prepare(48000.0, 4, 10.0, 1.0, 2, 0.0, true) && !port.prepare(48000.0, 2, 0.0099, 1.0, 2)
            && !port.prepare(48000.0, 2, 100.001, 1.0, 2) && !port.prepare(48000.0, 2, 10.0, -0.001, 2)
            && !port.prepare(48000.0, 2, 10.0, 12.001, 2) && !port.prepare(48000.0, 2, 10.0, 1.0, 2, 500.001)
            && !port.prepare(48000.0, 2, 10.0, 1.0, 2, -0.001) && !port.prepare(48000.0, 2, nan, 1.0, 2)
            && !port.prepare(48000.0, 2, 0.01, 1.0, 2) && !port.prepare(48000.0, 2, 10.0, 1.0, 2, 0.0, false, 0.04)
            && !port.prepared(),
        "phasor should refuse no sample rate, streams outside 2 to 8, more channels than streams, surround on two channels, values outside its ranges, a NaN and a packet over 2^20 frames");
    require(port.prepare(48000.0, 2, 100.000001, 12.000001, 2, 500.000001) && port.latencyFrames() == 481
            && port.prepare(8000.0, 2, 0.0100001, 0.0, 1) && port.latencyFrames() == 800001
            && Phasor::packetFrames(48000.0, 7.0) == 6860 && Phasor::packetFrames(44100.0, 37.0) == 1192,
        "phasor should take its ranges' ends within 0.000002, packets of sr / Phasfrq raised to a multiple of 4");
    require(port.prepare(48000.0, 3, 20.0, 1.0, 3, 0.0, false, 10.0) && port.latencyFrames() == 4801,
        "phasor should wait for the packet of its lowest frequency");
    const auto before = port;
    std::vector<double> out3(48, 0.0);
    auto bad = in;
    bad[3] = nan;
    require(!port.setControls(9.99, 1.0) && !port.setControls(20.0, 12.5) && !port.setControls(nan, 1.0)
            && !port.process(in, out) && !port.process(bad, out3) && port == before,
        "phasor should refuse a frequency under its lowest, a shift over 12, a NaN, an output not three times its input and a NaN input without change");
    require(port.setControls(10.0, 12.0) && port.process(in, out3), "phasor should take its lowest frequency");
    port.reset();
    Phasor fresh;
    require(fresh.prepare(48000.0, 3, 20.0, 1.0, 3, 0.0, false, 10.0) && port == fresh,
        "phasor's reset should return to prepare's state");
    require(fresh.prepare(48000.0, 3, 20.0, 1.0, 3, 0.0, false, 30.0) && fresh.latencyFrames() == 2401
            && !fresh.setControls(19.0, 1.0),
        "a lowest frequency above the frequency should be the frequency");
}

// CDP8's spin stereo 1 to 3 without doppler on recorded references, each
// output channel bit for bit, in blocks of 64, 1, 7 and 333 frames and whole,
// each after reset; a live rate set at each block's start where CDP's
// breakpoint file steps (just before a 64-frame block), a glide's before
// every frame. Mode 1 starts with the half-cycle flag CDP8's level pass ends
// with, found by playing the file once.
void testSpinMatchesCdp()
{
    using talas::cdp::Spin;
    using M = Spin::Mode;
    struct Case
    {
        std::string name;
        double sampleRate;
        M mode;
        double rate;
        double boost;
        double attenuation;
        int channels;
        int centre;
        double centreMinimum;
        double centreMaximum;
        bool live;
        bool overload;
    };
    const std::vector<Case> cases{
        {"spin-stereo-1-rate-2-boost-0", 48000.0, M::Stereo, 2.0, 0.0, 0.0, 8, 1, 0.0, 0.5, false, false},
        {"spin-stereo-1-rate-5", 48000.0, M::Stereo, 5.0, 2.0, 0.0, 8, 1, 0.0, 0.5, false, false},
        {"spin-stereo-1-rate-minus-3-boost-1-atten-0.5", 48000.0, M::Stereo, -3.0, 1.0, 0.5, 8, 1, 0.0, 0.5, false,
            false},
        {"spin-stereo-2-rate-2-8-channels-centre-3-boost-1-atten-0.3", 48000.0, M::Wide, 2.0, 1.0, 0.3, 8, 3, 0.1, 0.7,
            false, false},
        {"spin-stereo-2-rate-7-4-channels-centre-1", 48000.0, M::Wide, 7.0, 0.5, 0.0, 4, 1, 0.0, 0.5, false, false},
        {"spin-stereo-3-rate-2-8-channels-centre-3-boost-1-cmin-0.2", 48000.0, M::WideBareCentre, 2.0, 1.0, 0.25, 8, 3,
            0.2, 0.5, false, false},
        {"spin-stereo-3-rate-minus-9-6-channels-centre-6-boost-1.2", 48000.0, M::WideBareCentre, -9.0, 1.2, 0.0, 6, 6,
            0.0, 0.5, false, false},
        {"spin-stereo-1-rate-11-boost-1.5-rate-44100", 44100.0, M::Stereo, 11.0, 1.5, 0.0, 8, 1, 0.0, 0.5, false,
            false},
        {"spin-stereo-1-live-rate", 48000.0, M::Stereo, 0.0, 1.0, 0.0, 8, 1, 0.0, 0.5, true, false},
        {"spin-stereo-1-glide", 48000.0, M::Stereo, 0.0, 1.0, 0.0, 8, 1, 0.0, 0.5, true, false},
        {"spin-stereo-3-live-rate-8-channels-centre-3", 48000.0, M::WideBareCentre, 0.0, 1.0, 0.0, 8, 3, 0.1, 0.5, true,
            false},
        {"spin-stereo-1-rate-3-boost-4-overload", 48000.0, M::Stereo, 3.0, 4.0, 0.0, 8, 1, 0.0, 0.5, false, true},
    };
    for (const auto& c : cases)
    {
        const auto columns = referenceColumns(c.name);
        const std::size_t channels = c.mode == M::Stereo ? 2 : static_cast<std::size_t>(c.channels);
        require(columns.size() == 2 + channels, c.name + " should hold its two inputs and each output channel");
        const auto glide = c.name.ends_with("glide");
        const auto rates = c.live ? std::optional{referenceBreakpoints(c.name, "rate", c.sampleRate)}
                                  : std::optional<Breakpoints>{};
        // CDP's time for a frame: frame / sr in mode 1, floor((frame
        // channels + left channel) / 2) / sr in modes 2 and 3.
        std::size_t leftChannel = 0;
        if (c.mode != M::Stereo)
        {
            auto left = c.centre - 1;
            if (left < 1)
                left += c.channels;
            leftChannel = static_cast<std::size_t>(left - 1);
        }
        const auto rateAt = [&](std::size_t frame) {
            const auto samples = c.mode == M::Stereo ? frame : (frame * channels + leftChannel) / 2;
            return rates->at(static_cast<double>(samples) / c.sampleRate);
        };
        auto flipped = false;
        if (c.mode == M::Stereo)
        {
            Spin once;
            require(once.prepare(c.sampleRate, c.mode, c.live ? rateAt(0) : c.rate, c.boost, c.attenuation),
                c.name + " should prepare");
            std::vector<double> out(2);
            for (std::size_t frame = 0; frame < columns[0].size(); ++frame)
            {
                require((!c.live || once.setRate(rateAt(frame)))
                        && once.process(std::span<const double>{columns[0]}.subspan(frame, 1),
                            std::span<const double>{columns[1]}.subspan(frame, 1), out),
                    c.name + " should play once");
            }
            flipped = once.halfCycleFlag();
        }
        Spin port;
        require(port.prepare(c.sampleRate, c.mode, c.live ? rateAt(0) : c.rate, c.boost, c.attenuation, c.channels,
                    c.centre, c.centreMinimum, c.centreMaximum, flipped)
                && port.outputChannels() == channels && port.latencyFrames() == (c.mode == M::Stereo ? 0u : 1u),
            c.name + " should prepare");
        const auto set = [&](std::size_t frame, std::size_t) { return !c.live || port.setRate(rateAt(frame)); };
        const auto blocks = glide ? std::vector<std::size_t>{1}
            : c.live              ? std::vector<std::size_t>{64, 1, 32}
                                  : std::vector<std::size_t>{64, 1, 7, 333, columns[0].size()};
        for (const auto block : blocks)
        {
            port.reset();
            const auto output = playChannels(port, columns[0], columns[1], block, set, c.name);
            for (std::size_t channel = 0; channel < channels; ++channel)
            {
                const auto expected = c.overload ? clippedSamples(columns[2 + channel], c.name) : columns[2 + channel];
                require(sameBits(output[channel], expected),
                    c.name + " channel " + std::to_string(channel + 1) + " should match CDP8 bit for bit in "
                        + std::to_string(block) + "-frame blocks");
            }
        }
        if (glide)
        {
            port.reset();
            require(!sameBits(playChannels(port, columns[0], columns[1], 64, set, c.name)[0], columns[2]),
                "a glide held over 64-frame blocks should differ from CDP8's");
        }
    }
}

void testSpinRejectsWithoutChange()
{
    using talas::cdp::Spin;
    using M = Spin::Mode;
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    Spin port;
    std::vector<double> left(16, 0.1);
    std::vector<double> right(16, 0.1);
    std::vector<double> out(32, 0.0);
    std::vector<double> out8(128, 0.0);
    std::vector<double> shorter(15, 0.0);
    require(!port.prepared() && !port.process(left, right, out) && !port.setRate(1.0),
        "spin should refuse to play or take a rate before it is prepared");
    require(!port.prepare(0.0, M::Stereo, 1.0) && !port.prepare(nan, M::Stereo, 1.0)
            && !port.prepare(48000.0, static_cast<M>(7), 1.0) && !port.prepare(48000.0, M::Stereo, 100.001)
            && !port.prepare(48000.0, M::Stereo, -100.001) && !port.prepare(48000.0, M::Stereo, nan)
            && !port.prepare(48000.0, M::Stereo, 1.0, 16.001) && !port.prepare(48000.0, M::Stereo, 1.0, -0.001)
            && !port.prepare(48000.0, M::Stereo, 1.0, 2.0, 1.001) && !port.prepare(48000.0, M::Wide, 1.0, 2.0, 0.0, 3)
            && !port.prepare(48000.0, M::Wide, 1.0, 2.0, 0.0, 17) && !port.prepare(48000.0, M::Wide, 1.0, 2.0, 0.0, 8, 0)
            && !port.prepare(48000.0, M::Wide, 1.0, 2.0, 0.0, 8, 9)
            && !port.prepare(48000.0, M::Wide, 1.0, 2.0, 0.0, 8, 3, 0.6, 0.5)
            && !port.prepare(48000.0, M::Wide, 1.0, 2.0, 0.0, 8, 3, 0.0, 1.001)
            && !port.prepare(48000.0, M::WideBareCentre, 1.0, 2.0, 0.0, 8, 3, 1.001) && !port.prepared(),
        "spin should refuse no sample rate, an unknown mode, values outside its ranges, channels outside 4 to 16, a centre outside them and a centre minimum over its maximum");
    require(port.prepare(48000.0, M::WideBareCentre, 1.0, 2.0, 0.0, 8, 3, 0.6, 0.5)
            && port.prepare(48000.0, M::Stereo, 100.000001, 16.000001, 1.000001, 2, 9) && port.outputChannels() == 2,
        "spin's mode 3 should ignore the centre maximum, mode 1 the channels, and take its ranges' ends within 0.000002");
    require(port.prepare(48000.0, M::Wide, -2.0, 1.0, 0.0, 8, 3) && port.outputChannels() == 8
            && port.latencyFrames() == 1,
        "spin's mode 2 should prepare eight channels a frame late");
    const auto before = port;
    auto bad = right;
    bad[3] = nan;
    require(!port.setRate(100.5) && !port.process(left, right, out) && !port.process(left, shorter, out8)
            && !port.process(left, bad, out8) && port == before,
        "spin should refuse a rate over 100, an output not eight times its inputs, inputs of different sizes and a NaN without change");
    require(port.setRate(5.0) && port.process(left, right, out8), "spin should take a rate");
    port.reset();
    require(port == before, "spin's reset should return to prepare's state");
    require(port.prepare(48000.0, M::Stereo, 1.0) && port.latencyFrames() == 0
            && port.prepare(48000.0, M::Stereo, 1.0, 2.0, 0.0, 8, 1, 0.0, 0.5, true) && port.halfCycleFlag()
            && port.prepare(48000.0, M::Wide, 1.0, 2.0, 0.0, 8, 1, 0.0, 0.5, true) && !port.halfCycleFlag(),
        "spin's mode 1 should wait for nothing and take the half-cycle flag, which mode 2 clears");
}

// The seed a CDP8 reference's header records ("drand48 seeded with N"):
// CDP seeds drand48 from the time.
std::uint32_t referenceSeed(const std::string& name)
{
    std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + name + ".txt");
    require(file.good(), "CDP reference " + name + " should be readable");
    std::string line;
    while (std::getline(file, line) && !line.empty() && line.front() == '#')
    {
        const auto at = line.find("drand48 seeded with ");
        if (at != std::string::npos)
            return static_cast<std::uint32_t>(std::strtoul(line.c_str() + at + 20, nullptr, 10));
    }
    require(false, "CDP reference " + name + " should record its seed");
    return 0;
}

// CDP8's modify brassage modes 1, 3, 4 and 5 on recorded references, mono
// and stereo in, each output channel bit for bit, latencyFrames late, in
// blocks of 64, 1, 7 and 333 frames and whole, each after reset. Live fields
// (CDP's breakpoint files, read at each grain's start) are set as each call
// starts, a block split where CDP's value changes (its steps fall just before
// 512-frame blocks); CDP sizes its buffers from its tables' extremes, the
// port from its bounds. Where CDP8 renormalises its file, the reference holds
// its output before the rescale, which the port clips at -1 and 1.
void testBrassageMatchesCdp()
{
    using talas::cdp::Brassage;
    using M = Brassage::Mode;
    const auto settings = [](M mode, int channels, int memory, double pitch, double density, double amp,
                              double grainsize, double range) {
        Brassage::Settings fields;
        fields.mode = mode;
        fields.channels = channels;
        fields.memoryKilobytes = memory;
        fields.pitch = pitch;
        fields.density = density;
        fields.amp = amp;
        fields.grainsizeMs = grainsize;
        fields.rangeMs = range;
        return fields;
    };
    const auto pitchshift = [&](double pitch, int channels = 1, int memory = 1024) {
        return settings(M::Pitchshift, channels, memory, pitch, 8.0, 1.0, 50.0, 0.0);
    };
    const auto reverb = [&](double density, double pitch, double amp, double range = 0.0, int channels = 1,
                            int memory = 1024) {
        return settings(M::Reverb, channels, memory, pitch, density, amp, 50.0, range);
    };
    const auto scramble = [&](double grainsize, double range = 0.0, int channels = 1) {
        return settings(M::Scramble, channels, 1024, 0.0, 8.0, 1.0, grainsize, range);
    };
    const auto granulate = [&](double density, int channels = 1) {
        return settings(M::Granulate, channels, 1024, 0.0, density, 1.0, 50.0, 0.0);
    };
    struct Case
    {
        std::string name;
        double rate;
        Brassage::Settings settings;
        std::size_t latency;
        bool overload;
    };
    const std::vector<Case> cases{
        {"modify-brassage-1-pitch-3", 48000.0, pitchshift(3.0), 2854, false},
        {"modify-brassage-1-pitch-minus-7.5-tone", 48000.0, pitchshift(-7.5), 1557, false},
        {"modify-brassage-1-pitch-12-rate-44100", 44100.0, pitchshift(12.0), 4409, false},
        {"modify-brassage-1-pitch-5-stereo", 48000.0, pitchshift(5.0, 2), 3204, false},
        {"modify-brassage-1-pitch-7-memory-10", 48000.0, pitchshift(7.0, 1, 10), 3596, false},
        {"modify-brassage-1-pitch-minus-4-stereo-memory-10", 48000.0, pitchshift(-4.0, 2, 10), 1906, false},
        {"modify-brassage-1-live-pitch-memory-10", 48000.0, pitchshift(0.0, 1, 10), 4799, false},
        {"modify-brassage-1-pitch-2-overload", 48000.0, pitchshift(2.0), 2694, true},
        {"modify-brassage-3-density-8-pitch-0.2-amp-0.3", 48000.0, reverb(8.0, 0.2, 0.3), 2428, false},
        {"modify-brassage-3-density-12-pitch-minus-0.33-amp-0.2-range-50-memory-10", 48000.0,
            reverb(12.0, -0.33, 0.2, 50.0, 1, 10), 2355, false},
        {"modify-brassage-3-stereo-density-6-pitch-0.1-amp-0.25", 48000.0, reverb(6.0, 0.1, 0.25, 0.0, 2), 2414,
            false},
        {"modify-brassage-3-stereo-density-10-pitch-0.3-amp-0.2-range-50-memory-10", 48000.0,
            reverb(10.0, 0.3, 0.2, 50.0, 2, 10), 2442, false},
        {"modify-brassage-3-live-memory-10", 48000.0, reverb(8.0, 0.0, 1.0, 0.0, 1, 10), 2447, false},
        {"modify-brassage-3-density-16-pitch-0.1-amp-1-overload", 48000.0, reverb(16.0, 0.1, 1.0), 2414, true},
        {"modify-brassage-4-grainsize-50", 48000.0, scramble(50.0), 2399, false},
        {"modify-brassage-4-grainsize-20-range-100", 48000.0, scramble(20.0, 100.0), 959, false},
        {"modify-brassage-4-grainsize-7-range-40-tone", 48000.0, scramble(7.0, 40.0), 335, false},
        {"modify-brassage-4-stereo-grainsize-30-range-50", 48000.0, scramble(30.0, 50.0, 2), 1439, false},
        {"modify-brassage-4-live-grainsize-range", 48000.0, scramble(50.0), 3839, false},
        {"modify-brassage-5-density-1", 48000.0, granulate(1.0), 2399, false},
        {"modify-brassage-5-density-0.4-clicks", 48000.0, granulate(0.4), 2399, false},
        {"modify-brassage-5-stereo-density-2", 48000.0, granulate(2.0, 2), 2399, false},
        {"modify-brassage-5-density-0.7-rate-44100", 44100.0, granulate(0.7), 2204, false},
        {"modify-brassage-5-live-density", 48000.0, granulate(1.0), 2399, false},
    };
    // The fields CDP's breakpoint files hold, in Brassage's setter order.
    const std::array<std::string, 5> tables{"pitch", "density", "amp", "grainsize", "range"};
    for (const auto& c : cases)
    {
        const auto columns = referenceColumns(c.name);
        auto fields = c.settings;
        fields.seed = referenceSeed(c.name);
        const auto inputs = static_cast<std::size_t>(fields.channels);
        const auto outputs = fields.mode == M::Reverb ? std::size_t{2} : inputs;
        require(columns.size() == inputs + outputs, c.name + " should hold its inputs and each output channel");
        std::array<std::optional<Breakpoints>, 5> live;
        {
            std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + c.name + ".txt");
            std::string line;
            while (std::getline(file, line) && !line.empty() && line.front() == '#')
            {
                for (std::size_t field = 0; field < tables.size(); ++field)
                {
                    if (line.rfind("# " + tables[field] + ".brk: ", 0) == 0)
                        live[field] = referenceBreakpoints(c.name, tables[field], c.rate);
                }
            }
        }
        // CDP starts from its tables' first values and sizes its buffers by
        // their extremes.
        const auto highest = [](const Breakpoints& points) {
            return *std::max_element(points.values.begin(), points.values.end());
        };
        if (live[0])
        {
            fields.pitch = live[0]->at(0.0);
            fields.highestPitch = highest(*live[0]);
        }
        if (live[1])
        {
            fields.density = live[1]->at(0.0);
            fields.lowestDensity = live[1]->lowest();
        }
        if (live[2])
            fields.amp = live[2]->at(0.0);
        if (live[3])
        {
            fields.grainsizeMs = live[3]->at(0.0);
            fields.longestGrainsizeMs = highest(*live[3]);
        }
        if (live[4])
        {
            fields.rangeMs = live[4]->at(0.0);
            fields.longestRangeMs = highest(*live[4]);
        }
        const auto anyLive = std::any_of(live.begin(), live.end(), [](const auto& points) { return points.has_value(); });
        // CDP reads its fields at frame f's time, f times 1 / rate.
        const auto valuesAt = [&](std::size_t frame) {
            const auto time = static_cast<double>(frame) * (1.0 / c.rate);
            std::array<double, 5> values{fields.pitch, fields.density, fields.amp, fields.grainsizeMs, fields.rangeMs};
            for (std::size_t field = 0; field < live.size(); ++field)
            {
                if (live[field])
                    values[field] = live[field]->at(time);
            }
            return values;
        };
        Brassage port;
        require(port.prepare(c.rate, fields) && port.inputChannels() == inputs && port.outputChannels() == outputs
                && port.latencyFrames() == c.latency,
            c.name + " should prepare, " + std::to_string(c.latency) + " frames late");
        const auto frames = columns[0].size();
        const auto total = frames + c.latency;
        std::vector<double> in(total * inputs, 0.0);
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            for (std::size_t channel = 0; channel < inputs; ++channel)
                in[frame * inputs + channel] = columns[channel][frame];
        }
        std::vector<std::size_t> steps;
        for (std::size_t frame = 1; anyLive && frame < total; ++frame)
        {
            if (valuesAt(frame) != valuesAt(frame - 1))
                steps.push_back(frame);
        }
        for (const auto block : {std::size_t{64}, std::size_t{1}, std::size_t{7}, std::size_t{333}, total})
        {
            port.reset();
            std::vector<double> out(total * outputs, 0.0);
            std::size_t nextStep = 0;
            for (std::size_t from = 0; from < total;)
            {
                while (nextStep < steps.size() && steps[nextStep] <= from)
                    ++nextStep;
                const auto end = std::min(total, (from / block + 1) * block);
                const auto to = nextStep < steps.size() ? std::min(end, steps[nextStep]) : end;
                if (anyLive)
                {
                    const auto values = valuesAt(from);
                    require((!live[0] || port.setPitch(values[0])) && (!live[1] || port.setDensity(values[1]))
                            && (!live[2] || port.setAmp(values[2])) && (!live[3] || port.setGrainsize(values[3]))
                            && (!live[4] || port.setRange(values[4])),
                        c.name + " should take its live fields");
                }
                require(port.process(std::span<const double>{in}.subspan(from * inputs, (to - from) * inputs),
                            std::span<double>{out}.subspan(from * outputs, (to - from) * outputs)),
                    c.name + " should process");
                from = to;
            }
            for (std::size_t channel = 0; channel < outputs; ++channel)
            {
                std::vector<double> played;
                for (std::size_t frame = c.latency; frame < total; ++frame)
                    played.push_back(out[frame * outputs + channel]);
                const auto& cdp = columns[inputs + channel];
                require(sameBits(played, c.overload ? clippedSamples(cdp, c.name) : cdp),
                    c.name + " channel " + std::to_string(channel + 1) + " should match CDP8 bit for bit in "
                        + std::to_string(block) + "-frame blocks");
            }
        }
    }
}

void testBrassageRejectsWithoutChange()
{
    using talas::cdp::Brassage;
    using M = Brassage::Mode;
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto fields = [](M mode, double pitch, double density, double amp, double grainsize, double range,
                            int channels = 1, int memory = 1024) {
        Brassage::Settings settings;
        settings.mode = mode;
        settings.pitch = pitch;
        settings.density = density;
        settings.amp = amp;
        settings.grainsizeMs = grainsize;
        settings.rangeMs = range;
        settings.channels = channels;
        settings.memoryKilobytes = memory;
        return settings;
    };
    Brassage port;
    require(!port.setPitch(1.0), "brassage should refuse a field before it is prepared");
    const auto top = Brassage::maximumPitchshift(48000.0);
    require(std::abs(top - 134.7458) < 0.0001, "brassage's pitchshift should reach about 134.7458 semitones at 48 kHz");
    auto lowest = fields(M::Granulate, 0.0, 1.0, 1.0, 50.0, 0.0);
    lowest.lowestDensity = 0.0001;
    require(!port.prepare(0.0, fields(M::Pitchshift, 1.0, 2.0, 1.0, 50.0, 0.0))
            && !port.prepare(nan, fields(M::Pitchshift, 1.0, 2.0, 1.0, 50.0, 0.0))
            && !port.prepare(48000.0, fields(M::Pitchshift, 1.0, 2.0, 1.0, 50.0, 0.0, 0))
            && !port.prepare(48000.0, fields(M::Pitchshift, 1.0, 2.0, 1.0, 50.0, 0.0, 3))
            && !port.prepare(48000.0, fields(M::Pitchshift, 1.0, 2.0, 1.0, 50.0, 0.0, 1, 9))
            && !port.prepare(48000.0, fields(M::Pitchshift, 1.0, 2.0, 1.0, 50.0, 0.0, 1, 20481))
            && !port.prepare(48000.0, fields(M::Pitchshift, top + 0.00001, 2.0, 1.0, 50.0, 0.0))
            && !port.prepare(48000.0, fields(M::Pitchshift, nan, 2.0, 1.0, 50.0, 0.0))
            && !port.prepare(48000.0, fields(M::Pitchshift, 130.0, 2.0, 1.0, 50.0, 0.0))
            && !port.prepare(48000.0, fields(M::Reverb, 0.0, 1.999, 1.0, 50.0, 0.0))
            && !port.prepare(48000.0, fields(M::Reverb, 0.0, 16384.001, 1.0, 50.0, 0.0))
            && !port.prepare(48000.0, fields(M::Reverb, 0.0, 4801.0, 1.0, 50.0, 0.0))
            && !port.prepare(48000.0, fields(M::Reverb, 0.34, 8.0, 1.0, 50.0, 0.0))
            && !port.prepare(48000.0, fields(M::Reverb, 0.0, 8.0, 1.01, 50.0, 0.0))
            && !port.prepare(48000.0, fields(M::Reverb, 0.0, 8.0, -0.01, 50.0, 0.0))
            && !port.prepare(48000.0, fields(M::Reverb, 0.0, 8.0, 1.0, 50.0, 50.01))
            && !port.prepare(48000.0, fields(M::Scramble, 0.0, 2.0, 1.0, 1.99, 0.0))
            && !port.prepare(48000.0, fields(M::Scramble, 0.0, 2.0, 1.0, 50.0, -1.0))
            && !port.prepare(48000.0, fields(M::Scramble, 0.0, 2.0, 1.0, 100000.0, 0.0))
            && !port.prepare(48000.0, fields(M::Granulate, 0.0, 2.01, 1.0, 50.0, 0.0))
            && !port.prepare(48000.0, fields(M::Granulate, 0.0, 0.0000019, 1.0, 50.0, 0.0))
            && !port.prepare(48000.0, fields(M::Granulate, 0.0, 0.0001, 1.0, 50.0, 0.0))
            && !port.prepare(48000.0, lowest) && !port.prepared(),
        "brassage should refuse no sample rate, channels outside 1 and 2, memory outside 10 to 20480 kB, fields outside its mode's ranges, a NaN, a reverb grain step of 0 and spans past 2^22 frames");
    require(port.prepare(48000.0, fields(M::Pitchshift, -134.7458, 2.0, 1.0, 50.0, 0.0))
            && port.prepare(48000.0, fields(M::Reverb, 0.330001, 2.0 - 0.000001, 1.000001, 50.0, 50.000001))
            && port.prepare(48000.0, fields(M::Granulate, 0.0, 0.001, 1.0, 50.0, 0.0))
            && port.prepare(48000.0, fields(M::Pitchshift, 128.0, 2.0, 1.0, 50.0, 0.0))
            && port.prepare(48000.0, fields(M::Reverb, 0.0, 4800.0, 1.0, 50.0, 0.0)),
        "brassage should take its ranges' ends within 0.000002, 128 semitones and a reverb step of a frame");

    auto bounded = fields(M::Pitchshift, 2.0, 2.0, 1.0, 50.0, 0.0);
    bounded.highestPitch = 5.0;
    require(port.prepare(48000.0, bounded) && port.inputChannels() == 1 && port.outputChannels() == 1
            && port.latencyFrames() == static_cast<std::size_t>(std::floor(2399.0 * std::exp2(5.0 * 0.08333333333))) + 2,
        "brassage's pitchshift should wait floor((G - 1) 2^(highest pitch / 12)) + 2 frames");
    requireProcessRefusals(port, "brassage");
    const auto before = port;
    require(!port.setPitch(5.1) && !port.setPitch(nan) && !port.setDensity(1.0) && !port.setAmp(0.5)
            && !port.setGrainsize(20.0) && !port.setRange(10.0) && port == before,
        "brassage's pitchshift should refuse a pitch over its highest, a NaN and the fields it does not take without change");
    std::vector<double> in(8, 0.1);
    std::vector<double> out(8, 0.0);
    require(port.setPitch(-3.0) && port.process(in, out), "brassage should take a lower pitch and play");
    port.reset();
    Brassage fresh;
    require(fresh.prepare(48000.0, bounded) && port == fresh, "brassage's reset should return to prepare's state");

    auto reverb = fields(M::Reverb, 0.1, 8.0, 0.5, 50.0, 20.0, 2);
    reverb.lowestDensity = 4.0;
    reverb.longestRangeMs = 30.0;
    require(port.prepare(44100.0, reverb) && port.inputChannels() == 2 && port.outputChannels() == 2
            && !port.setDensity(3.9) && port.setDensity(4.0) && !port.setRange(30.5) && port.setRange(30.0)
            && port.setAmp(0.0) && !port.setGrainsize(20.0),
        "brassage's reverb should take a stereo input, a density down to its lowest and a range up to its longest");
    auto scramble = fields(M::Scramble, 0.0, 2.0, 1.0, 20.0, 10.0);
    scramble.longestGrainsizeMs = 80.0;
    require(port.prepare(48000.0, scramble) && port.latencyFrames() == 80 * 48 - 1 && !port.setGrainsize(80.1)
            && port.setGrainsize(2.0) && !port.setRange(10.5) && !port.setPitch(0.0),
        "brassage's scramble should wait for its longest grain less a frame, a fixed range its own bound");
    require(port.prepare(48000.0, fields(M::Granulate, 0.0, 1.0, 1.0, 50.0, 0.0, 2)) && port.latencyFrames() == 2399
            && !port.setDensity(0.5) && port.setDensity(2.0),
        "brassage's granulate should wait 2399 frames at 48 kHz, a fixed density its own lowest");
}

// CDP8's distort shuffle on recorded references, latencyFrames late, as far
// as CDP writes (it drops its last domain).
void testDistortShuffleMatchesCdp()
{
    for (const auto& [name, shuffle, cycles] : {
             std::tuple{std::string{"distort-shuffle-abc-cab"}, std::string{"abc-cab"}, 1},
             std::tuple{std::string{"distort-shuffle-ab-ba-2-cycles-falling-start"}, std::string{"ab-ba"}, 2},
             std::tuple{std::string{"distort-shuffle-abcd-dbca"}, std::string{"abcd:dbca"}, 1}})
    {
        const auto cdp = reference(name);
        const auto map = talas::cdp::readDistortShuffle(shuffle);
        talas::cdp::DistortShuffle distortShuffle;
        require(map.has_value() && distortShuffle.prepare(*map, cycles), name + " should prepare");
        const auto latency = talas::cdp::DistortShuffle::latencyFrames;
        const auto output = playLagging(distortShuffle, cdp.input, latency, name);
        require(std::equal(cdp.outputs[0].begin(), cdp.outputs[0].begin() + 937, output.begin()),
            name + " should match CDP8's distort shuffle exactly, latencyFrames late");
        distortShuffle.reset();
        require(playLagging(distortShuffle, cdp.input, latency, name) == output,
            name + " should replay after reset");
    }
    require(talas::cdp::readDistortShuffle("abc-cab") == std::vector<int>{2, 0, 1},
        "distort shuffle should map each image letter to its place in the domain");
    for (const auto* text : {"ab-aab", "ab-a", "aab-ab", "ab-bc", "a1-1a", "abba", "-ab", "ab-"})
    {
        require(!talas::cdp::readDistortShuffle(text).has_value(),
            std::string{"distort shuffle should refuse \""} + text + "\"");
    }
    talas::cdp::DistortShuffle distortShuffle;
    const std::vector<int> repeated{0, 0};
    require(!distortShuffle.prepare(repeated, 1) && !distortShuffle.prepare(std::vector<int>{1, 0}, 0)
            && !distortShuffle.prepared(),
        "distort shuffle should refuse a map that is no permutation and no cycles");
}

void testTremoloAndPanRejectWithoutChange()
{
    talas::cdp::Tremolo tremolo;
    require(tremolo.prepare(44100.0), "Tremolo should prepare");
    std::array<double, 3> input{0.1, 0.2, 0.3};
    std::array<double, 3> output{};
    require(tremolo.process(input, 5.0, 0.5, 1.0, output), "Tremolo should process");
    const auto tremoloBefore = tremolo;
    require(!tremolo.process(input, 501.0, 0.5, 1.0, output)
            && !tremolo.process(input, 5.0, 1.5, 1.0, output)
            && !tremolo.process(input, 5.0, 0.5, -0.1, output)
            && tremolo == tremoloBefore,
        "Tremolo should reject values outside CDP's ranges without change");

    talas::cdp::Pan pan;
    std::array<double, 3> left{};
    std::array<double, 3> right{};
    require(pan.process(input, 0.5, 0.7, left, right), "Pan should process");
    const auto panBefore = pan;
    input[1] = std::numeric_limits<double>::infinity();
    require(!pan.process(input, 0.5, 0.7, left, right)
            && !pan.process(std::array<double, 1>{0.0}, 257.0, 0.7,
                std::span{left}.first(1), std::span{right}.first(1))
            && !pan.process(std::array<double, 1>{0.0}, 0.0, 3.0,
                std::span{left}.first(1), std::span{right}.first(1))
            && pan == panBefore,
        "Pan should reject non-finite input and values outside CDP's ranges without change");
}
}

int main()
{
    try
    {
        testLowerResolutionQuantizesLikeCdp();
        testLowerResolutionHoldsEachGroupMean();
        testLowerResolutionRejectsWithoutChange();
        testRingModulateReadsCdpTable();
        testRingModulateRampsBetweenBlockReadings();
        testRingModulateStreamsInAnyBlocks();
        testRingModulateRejectsWithoutChange();
        testPortsMatchCdpPrograms();
        testTremoloAndPanRejectWithoutChange();
        testTapDelayMatchesCdp();
        testTapDelayReadsTapsAsCdp();
        testTapDelayRejectsWithoutChange();
        testEchoMatchesCdp();
        testEchoRejectsWithoutChange();
        testVaryingDelayMatchesCdp();
        testVaryingDelayRejectsWithoutChange();
        testReverbMatchesCdp();
        testReverbRejectsWithoutChange();
        testVariableFilterMatchesCdp();
        testVariableFilterRejectsWithoutChange();
        testVariableFilterFollowsCdpBreakpoints();
        testSweepingFilterFollowsCdpBreakpoints();
        testFilterBanksFollowCdpBreakpoints();
        testPhasingFollowsCdpBreakpoints();
        testDistortionsFollowCdpBreakpoints();
        testPhaseVocoderMatchesCdp();
        testPhaseVocoderRecoversFromAnOverflowingSample();
        testSpectralProcessesMatchCdp();
        testHiliteFilterPluckAndStepMatchCdp();
        testStrangeShiftAndRepitchTransposeMatchCdp();
        testWaverTranspChorusAndNoiseMatchCdp();
        testStretchSpectrumAndScatterMatchCdp();
        testHiliteArpegGreqAndBandMatchCdp();
        testBlurSpreadAndGlisMatchCdp();
        testCombineAndVocodeMatchCdp();
        testMorphMatchesCdp();
        testRepitchTransposefMatchesCdp();
        testFormantsPutAndVowelsMatchCdp();
        testBlurBlurAndBltrMatchCdp();
        testFocusFocusMatchesCdp();
        testPitchTunePickAndChordsMatchCdp();
        testGetPitchAndPitchFileProcessesMatchCdp();
        testWholeSoundAndFreezeProcessesMatchCdp();
        testVaryingBank2AndTremoloFineMatchCdp();
        testWavesetDistortionsMatchCdp();
        testWavesetDistortionsRejectWithoutChange();
        testRoomReverbMatchesCdp();
        testRoomReverbRejectsWithoutChange();
        testSampleProcessesMatchCdp();
        testTwoInputProcessesMatchCdp();
        testSampleAndTwoInputProcessesRejectWithoutChange();
        testFastConvolutionMatchesCdp();
        testFastConvolutionRejectsWithoutChange();
        testRestorationProcessesMatchCdp();
        testRestorationProcessesRejectWithoutChange();
        testSubmixPairsMatchCdp();
        testSubmixPairsRejectWithoutChange();
        testEnvelopeProcessesMatchCdp();
        testEnvelopeProcessesRejectWithoutChange();
        testPitchedDelayBounceAndRejoinMatchCdp();
        testPitchedDelayBounceAndRejoinRejectWithoutChange();
        testCascadeMatchesCdp();
        testCascadeRejectsWithoutChange();
        testPhasorMatchesCdp();
        testPhasorRejectsWithoutChange();
        testSpinMatchesCdp();
        testSpinRejectsWithoutChange();
        testBrassageMatchesCdp();
        testBrassageRejectsWithoutChange();
        testEchoFollowsCdpBreakpoints();
        testFixedFilterMatchesCdp();
        testFixedFilterRejectsWithoutChange();
        testPhasingMatchesCdp();
        testPhasingRejectsWithoutChange();
        testLohiFilterMatchesCdp();
        testLohiFilterRejectsWithoutChange();
        testSweepingFilterMatchesCdp();
        testSweepingFilterRejectsWithoutChange();
        testUserBankMatchesCdp();
        testUserBankReadsAndRejectsAsCdp();
        testDistortOverloadMatchesCdp();
        testDistortOverloadRejectsWithoutChange();
        testVaryingBankMatchesCdp();
        testVaryingBankReadsAndRejectsAsCdp();
        testStadiumEchoMatchesCdp();
        testStadiumEchoRejectsWithoutChange();
        testDistortReformMatchesCdp();
        testDistortReformCutsLongHalfCycles();
        testDistortReformRejectsWithoutChange();
        testDistortOmitReverseAndEnvelopeMatchCdp();
        testDistortEnvelopeReadsAsCdp();
        testDistortOmitReverseAndEnvelopeRejectWithoutChange();
        testDistortHarmonicAndFractalMatchCdp();
        testDistortHarmonicReadsAndRejectsAsCdp();
        testDistortMultiplyAndDivideMatchCdp();
        testDistortShuffleMatchesCdp();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
