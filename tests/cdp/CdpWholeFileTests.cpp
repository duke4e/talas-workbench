#include "talas/cdp/SpectralWholeFile.h"

#include "talas/cdp/PitchTrack.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// The whole-file CDP spectral processes (SpectralWholeFile.h) against CDP8's
// own `pvoc anal`, program and `pvoc synth` output, sample for sample, and
// against what CDP refuses.
namespace
{
using talas::cdp::OfflineSpectralField;
using talas::cdp::OfflineSpectralRequest;
using Fields = std::map<std::string, std::string, std::less<>>;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

constexpr double sampleRate = 48000.0;

// A fixture: "input N",
// "analysis N" and "output N" sections of C99 hex floats.
struct Fixture
{
    std::vector<std::vector<double>> inputs;
    std::vector<float> analysis;
    bool hasAnalysis = false;
    std::vector<double> output;
    bool hasOutput = false;
};

Fixture readFixture(const std::string& name)
{
    std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + name + ".txt");
    require(file.good(), "missing fixture " + name);
    Fixture fixture;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream header(line);
        std::string section;
        std::size_t count = 0;
        header >> section >> count;
        require(section == "input" || section == "analysis" || section == "output",
            name + ": unknown section " + section);
        std::vector<double> values;
        values.reserve(count);
        for (std::size_t n = 0; n < count; ++n)
        {
            require(static_cast<bool>(std::getline(file, line)), name + ": short section " + section);
            char* end = nullptr;
            values.push_back(std::strtod(line.c_str(), &end));
            require(end != line.c_str(), name + ": unreadable value " + line);
        }
        if (section == "input")
            fixture.inputs.push_back(std::move(values));
        else if (section == "analysis")
        {
            fixture.analysis.assign(values.begin(), values.end());
            fixture.hasAnalysis = true;
        }
        else
        {
            fixture.output = std::move(values);
            fixture.hasOutput = true;
        }
    }
    require(!fixture.inputs.empty() && fixture.hasOutput, name + ": incomplete fixture");
    return fixture;
}

struct Case
{
    std::string name;
    std::string process;
    int points;
    int overlap;
    Fields fields;
};

// Each fixture's request, as its header's CDP command line gives it.
const std::vector<Case>& cases()
{
    static const std::vector<Case> all{
        {"stretch-time-2-5-256-points", "stretchTime", 256, 3, {{"stretch", "2.5"}}},
        {"stretch-time-3-overlap-2", "stretchTime", 256, 2, {{"stretch", "3"}}},
        {"stretch-time-0-4-1024-points", "stretchTime", 1024, 3, {{"stretch", "0.4"}}},
        {"stretch-time-table-256-points", "stretchTime", 256, 3, {{"stretch", "0 0.5\n0.02 3"}}},
        {"stretch-time-table-past-end", "stretchTime", 256, 3, {{"stretch", "0.01 2\n0.03 0.7\n0.2 1.5"}}},
        {"stretch-time-table-negative-time", "stretchTime", 256, 3,
            {{"stretch", "-0.01 1\n0.01 2\n0.03 2\n0.04 0.8"}}},
        {"spec-cut-256-points", "specCut", 256, 3, {{"start", "0.005"}, {"end", "0.035"}}},
        {"spec-cut-later-buffer-256-points", "specCut", 256, 3, {{"start", "0.02"}, {"end", "0.045"}}},
        {"spec-cut-to-end-1024-points", "specCut", 1024, 3, {{"start", "0.03"}, {"end", "0.1"}}},
        {"spec-cut-overlap-1", "specCut", 128, 1, {{"start", "0.004"}, {"end", "0.03"}}},
        {"spec-grab-256-points", "specGrab", 256, 3, {{"time", "0.03"}}},
        {"spec-grab-past-end-1024-points", "specGrab", 1024, 3, {{"time", "0.5"}}},
        {"spec-magnify-256-points", "specMagnify", 256, 3, {{"time", "0.02"}, {"duration", "0.1"}}},
        {"spec-magnify-short-1024-points", "specMagnify", 1024, 3, {{"time", "0.04"}, {"duration", "0.03"}}},
        {"spec-magnify-overlap-4", "specMagnify", 512, 4, {{"time", "0.01"}, {"duration", "0.04"}}},
        {"focus-hold-256-points", "focusHold", 256, 3, {{"holds", "0.005 0.01\n0.03 0.02"}}},
        {"focus-hold-unsorted-1024-points", "focusHold", 1024, 3, {{"holds", "0.06 0.05 0.02 0.01 0.07 0"}}},
        {"combine-interleave-2-inputs-256-points", "combineInterleave", 256, 3, {{"leafSize", "3"}}},
        {"combine-interleave-3-inputs-1024-points", "combineInterleave", 1024, 3, {{"leafSize", "2"}}},
        {"blur-drunk-buffers-256-points", "blurDrunk", 256, 3,
            {{"range", "20"}, {"start", "0.03"}, {"duration", "0.1"}, {"seed", "1760000000"}}},
        {"blur-drunk-no-zero-steps-256-points", "blurDrunk", 256, 3,
            {{"range", "6"}, {"start", "0"}, {"duration", "0.1"}, {"noZeroSteps", "true"}, {"seed", "1760000100"}}},
        {"blur-drunk-1024-points", "blurDrunk", 1024, 3,
            {{"range", "5"}, {"start", "0.05"}, {"duration", "0.15"}, {"seed", "1760000200"}}},
        {"blur-shuffle-256-points", "blurShuffle", 256, 3, {{"domainImage", "abcd-dcbaab"}, {"groupSize", "1"}}},
        {"blur-shuffle-groups-1024-points", "blurShuffle", 1024, 3, {{"domainImage", "abc:cca"}, {"groupSize", "3"}}},
        {"blur-weave-256-points", "blurWeave", 256, 3, {{"weave", "3 -1 2"}}},
        {"blur-weave-backtrack-1024-points", "blurWeave", 1024, 3, {{"weave", "5\n-3\n0 2"}}},
    };
    return all;
}

OfflineSpectralRequest requestFor(const std::string& process, int points, int overlap, Fields fields)
{
    OfflineSpectralRequest request;
    request.process = process;
    request.points = points;
    request.overlap = overlap;
    request.fields = std::move(fields);
    return request;
}

void testFixturesMatchCdp()
{
    for (const auto& c : cases())
    {
        const auto fixture = readFixture(c.name);
        const auto request = requestFor(c.process, c.points, c.overlap, c.fields);
        const auto result = talas::cdp::renderOfflineSpectralProcess(request, fixture.inputs, sampleRate);
        require(result.ok(), c.name + ": refused: " + result.error);
        require(result.samples.size() == fixture.output.size(),
            c.name + ": " + std::to_string(result.samples.size()) + " samples, CDP wrote "
                + std::to_string(fixture.output.size()));
        for (std::size_t n = 0; n < fixture.output.size(); ++n)
        {
            if (result.samples[n] != fixture.output[n])
            {
                std::ostringstream message;
                message << c.name << ": sample " << n << " is " << std::hexfloat << result.samples[n] << ", CDP "
                        << fixture.output[n];
                throw std::runtime_error(message.str());
            }
        }
        if (fixture.hasAnalysis)
        {
            const auto analysis = talas::cdp::renderOfflineSpectralAnalysis(request, fixture.inputs, sampleRate);
            require(analysis.ok(), c.name + ": analysis refused: " + analysis.error);
            require(analysis.windows == fixture.analysis, c.name + ": the output .ana differs from CDP's");
            require(analysis.frameSize == static_cast<std::size_t>(c.points + c.points % 2 + 2),
                c.name + ": frame size");
        }
    }
}

std::vector<double> source(std::size_t frames)
{
    // wf_fixture.py's source.
    std::vector<double> values(frames);
    for (std::size_t n = 0; n < frames; ++n)
    {
        const double t = static_cast<double>(n) / sampleRate;
        const double s = 0.3 * std::sin(2 * std::numbers::pi * 441 * t) + 0.2 * std::sin(2 * std::numbers::pi * 1733 * t)
            + 0.15 * std::sin(2 * std::numbers::pi * (300 + 2000 * t) * t);
        values[n] = static_cast<float>(s);
    }
    return values;
}

void testRegistry()
{
    const auto& processes = talas::cdp::offlineSpectralProcesses();
    require(processes.size() == 9, "nine processes");
    std::set<std::string_view> ids;
    std::set<std::string> covered;
    for (const auto& c : cases())
        covered.insert(c.process);
    for (const auto& process : processes)
    {
        const std::string id{process.id};
        require(ids.insert(process.id).second, id + ": duplicate id");
        require(talas::cdp::findOfflineSpectralProcess(process.id) == &process, id + ": find");
        require(!process.cdpCommand.empty() && !process.label.empty(), id + ": command and label");
        require(process.minimumInputs >= 1 && process.minimumInputs <= process.maximumInputs, id + ": inputs");
        require(!process.fields.empty(), id + ": has fields");
        require(covered.contains(id), id + ": has fixtures");
        std::set<std::string_view> fieldIds;
        for (const auto& field : process.fields)
        {
            require(fieldIds.insert(field.id).second, id + ": duplicate field " + std::string{field.id});
            require(!field.label.empty() && !field.help.empty(), id + ": field label and help");
            if (field.kind == OfflineSpectralField::Kind::Flag)
                require(field.defaultValue == "true" || field.defaultValue == "false", id + ": flag default");
            if (field.kind == OfflineSpectralField::Kind::Choice)
                require(!field.choices.empty(), id + ": choices");
        }
    }
    require(talas::cdp::findOfflineSpectralProcess("noSuchProcess") == nullptr, "unknown process");
    require(processes[0].id == "stretchTime" && processes[0].cdpCommand == "stretch time", "stretch time first");
    require(talas::cdp::findOfflineSpectralProcess("combineInterleave")->minimumInputs == 2, "interleave takes two");
}

void testDefaultsRender()
{
    const auto input = source(16384);
    for (const auto& process : talas::cdp::offlineSpectralProcesses())
    {
        std::vector<std::vector<double>> inputs(process.minimumInputs, input);
        OfflineSpectralRequest request;
        request.process = std::string{process.id};
        const auto result = talas::cdp::renderOfflineSpectralProcess(request, inputs, sampleRate);
        require(result.ok(), std::string{process.id} + ": defaults refused: " + result.error);
        // spec grab's one window synthesises to nothing, as in CDP.
        require(process.id == "specGrab" ? result.samples.empty() : !result.samples.empty(),
            std::string{process.id} + ": defaults render");
        for (const auto sample : result.samples)
            require(std::isfinite(sample) && std::fabs(sample) <= 1.0, std::string{process.id} + ": sample range");
        // A field given its default's text renders alike.
        Fields fields;
        for (const auto& field : process.fields)
            fields.emplace(std::string{field.id}, std::string{field.defaultValue});
        request.fields = fields;
        const auto again = talas::cdp::renderOfflineSpectralProcess(request, inputs, sampleRate);
        require(again.ok() && again.samples == result.samples, std::string{process.id} + ": explicit defaults");
    }
}

void testEmptyHoldsCopyTheFile()
{
    // CDP8 run on an empty data file: focus hold writes its input.
    const auto input = source(4096);
    const auto request = requestFor("focusHold", 256, 3, {{"holds", "  \n"}});
    const std::vector<std::vector<double>> inputs{input};
    const auto analysis = talas::cdp::renderOfflineSpectralAnalysis(request, inputs, sampleRate);
    require(analysis.ok(), "empty holds: " + analysis.error);
    std::vector<float> anal;
    require(talas::cdp::analyseWholeSound(input, sampleRate, 256, 3,
                [&anal](std::span<const float> frame) { anal.insert(anal.end(), frame.begin(), frame.end()); }),
        "pvoc anal");
    require(analysis.windows == anal, "empty holds copy the file");
    const auto result = talas::cdp::renderOfflineSpectralProcess(request, inputs, sampleRate);
    require(result.ok() && result.samples == talas::cdp::synthesiseAnalysisFile(anal, 256, 3, sampleRate),
        "empty holds synthesise the copy");
}

void expectRefusal(const OfflineSpectralRequest& request, const std::vector<std::vector<double>>& inputs,
    const std::string& expected, double rate = sampleRate)
{
    const auto result = talas::cdp::renderOfflineSpectralProcess(request, inputs, rate);
    require(!result.ok(), request.process + ": expected a refusal containing \"" + expected + "\"");
    require(result.samples.empty(), request.process + ": a refusal has no samples");
    require(result.error.find(expected) != std::string::npos,
        request.process + ": refusal \"" + result.error + "\" lacks \"" + expected + "\"");
}

void testRefusals()
{
    const std::vector<std::vector<double>> one{source(2048)};
    const std::vector<std::vector<double>> two{source(2048), source(3000)};
    const auto at256 = [](const std::string& process, Fields fields) {
        return requestFor(process, 256, 3, std::move(fields));
    };
    // The request itself.
    expectRefusal(at256("noSuchProcess", {}), one, "no offline spectral process");
    expectRefusal(at256("stretchTime", {{"factor", "2"}}), one, "has no field \"factor\"");
    expectRefusal(at256("blurDrunk", {{"noZeroSteps", "yes"}}), one, "true or false");
    expectRefusal(at256("stretchTime", {}), {}, "takes 1 input");
    expectRefusal(at256("combineInterleave", {}), one, "takes 2 to 64 inputs");
    expectRefusal(at256("stretchTime", {}), {std::vector<double>{}}, "holds no samples");
    expectRefusal(at256("stretchTime", {}), {std::vector<double>{0.0, std::numeric_limits<double>::infinity()}},
        "not finite");
    expectRefusal(requestFor("stretchTime", 9000, 3, {}), one, "pvoc anal takes");
    expectRefusal(requestFor("stretchTime", 256, 5, {}), one, "pvoc anal takes");
    expectRefusal(at256("stretchTime", {}), one, "sample rate", 0.0);
    // stretch time: tklib1.c's range, readdata.c's breakpoint reading and
    // check_for_enough_tstretch_brkpnt_vals.
    expectRefusal(at256("stretchTime", {{"stretch", "0.00005"}}), one, "outside CDP's range");
    expectRefusal(at256("stretchTime", {{"stretch", "1e3"}}), one, "holds no data");
    expectRefusal(at256("stretchTime", {{"stretch", "0 2"}}), one, "two or more pairs");
    expectRefusal(at256("stretchTime", {{"stretch", "0 1\n0 2"}}), one, "not in increasing order");
    expectRefusal(at256("stretchTime", {{"stretch", "0 1\n1 20000"}}), one, "outside CDP's range");
    expectRefusal(at256("stretchTime", {{"stretch", "0 1\n1"}}), one, "not paired");
    // spec cut, grab and magnify.
    expectRefusal(at256("specCut", {{"start", "0.02"}, {"end", "0.02"}}), one, "Incompatible start and end");
    expectRefusal(at256("specCut", {{"start", "1"}}), one, "outside CDP's range");
    expectRefusal(at256("specCut", {{"end", "0.0001"}}), one, "outside CDP's range");
    expectRefusal(at256("specCut", {{"start", "abc"}}), one, "must be a number");
    expectRefusal(at256("specGrab", {{"time", "2"}}), one, "outside CDP's range");
    expectRefusal(at256("specMagnify", {{"duration", "0.0001"}}), one, "outside CDP's range");
    expectRefusal(at256("specMagnify", {{"time", "0.5"}}), one, "outside CDP's range");
    // focus hold's data file.
    expectRefusal(at256("focusHold", {{"holds", "10 1"}}), one, "Invalid freeze location");
    expectRefusal(at256("focusHold", {{"holds", "0.01 -1"}}), one, "Invalid freeze duration");
    expectRefusal(at256("focusHold", {{"holds", "0.01 0.1 0.02"}}), one, "not paired correctly");
    // combine interleave.
    expectRefusal(at256("combineInterleave", {{"leafSize", "0"}}), two, "outside CDP's range");
    expectRefusal(at256("combineInterleave", {{"leafSize", "73"}}), two, "outside CDP's range");
    // blur drunk: ranges, the seed, and a walk CDP8 stops on (seeded with
    // that time, CDP8 prints "ERROR: Step turned out ot be too large.").
    expectRefusal(at256("blurDrunk", {{"range", "37"}}), one, "outside CDP's range");
    expectRefusal(at256("blurDrunk", {{"range", "8"}, {"seed", "-1"}}), one, "seed must be");
    expectRefusal(at256("blurDrunk", {{"range", "8"}, {"seed", "4294967296"}}), one, "seed must be");
    expectRefusal(at256("blurDrunk", {{"range", "8"}, {"duration", "0"}}), one, "outside CDP's range");
    expectRefusal(at256("blurDrunk",
                      {{"range", "20"}, {"start", "0.03"}, {"duration", "0.1"}, {"seed", "1760000002"}}),
        one, "Step turned out ot be too large");
    // blur shuffle: get_domain_and_image, consistent_domain_and_image, and
    // a domain longer than the sound (CDP8: "Insufficient data in soundfile
    // to do this shuffle").
    expectRefusal(at256("blurShuffle", {{"domainImage", "abcd"}}), one, "no '-'");
    expectRefusal(at256("blurShuffle", {{"domainImage", "-ab"}}), one, "no domain");
    expectRefusal(at256("blurShuffle", {{"domainImage", "ab-"}}), one, "no image");
    expectRefusal(at256("blurShuffle", {{"domainImage", "ab-c"}}), one, "not in the domain");
    expectRefusal(at256("blurShuffle", {{"domainImage", "aa-a"}}), one, "repeats");
    expectRefusal(at256("blurShuffle", {{"domainImage", "a1-a"}}), one, "invalid character");
    expectRefusal(at256("blurShuffle", {{"domainImage", "abcdefghijklmnopqrstuvwxyz-a"}, {"groupSize", "3"}}),
        one, "Insufficient data");
    expectRefusal(at256("blurShuffle", {{"groupSize", "0"}}), one, "outside CDP's range");
    // blur weave: read_specweave_data, check_for_viable_weave, and a weave
    // whose backtrack CDP8 miscounts ("ERROR: Backtracking failure in
    // specweave()." from CDP8 itself).
    expectRefusal(at256("blurWeave", {{"weave", "4 0 -1 -2"}}), one, "Backtracking failure");
    expectRefusal(at256("blurWeave", {{"weave", "-1 2"}}), one, "before weave-start");
    expectRefusal(at256("blurWeave", {{"weave", "1 -1"}}), one, "must progress");
    expectRefusal(at256("blurWeave", {{"weave", "x"}}), one, "invalid character");
    expectRefusal(at256("blurWeave", {{"weave", "1.5"}}), one, "invalid character");
    expectRefusal(at256("blurWeave", {{"weave", "100"}}), one, "outside CDP's range");
    expectRefusal(at256("blurWeave", {{"weave", " \n"}}), one, "no steps");
}

void testSynthesisRefusesBadFiles()
{
    require(talas::cdp::synthesiseAnalysisFile({}, 256, 3, sampleRate).empty(), "no windows");
    const std::vector<float> partial(257, 0.0F);
    require(talas::cdp::synthesiseAnalysisFile(partial, 256, 3, sampleRate).empty(), "a partial window");
    const std::vector<float> silent(258 * 40, 0.0F);
    require(talas::cdp::synthesiseAnalysisFile(silent, 256, 3, sampleRate).size() == 31 * 32,
        "silent windows: pvoc synth writes the blocks up to its first silent one after the file");
    require(talas::cdp::synthesiseAnalysisFile(silent, 256, 3, 0.0).empty(), "no sample rate");
}
}

int main()
{
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"fixtures match CDP8", testFixturesMatchCdp},
        {"registry", testRegistry},
        {"defaults render", testDefaultsRender},
        {"empty holds copy the file", testEmptyHoldsCopyTheFile},
        {"refusals", testRefusals},
        {"synthesis refuses bad files", testSynthesisRefusesBadFiles},
    };
    int failures = 0;
    for (const auto& [name, test] : tests)
    {
        try
        {
            test();
            std::cout << "ok   " << name << '\n';
        }
        catch (const std::exception& error)
        {
            ++failures;
            std::cout << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
