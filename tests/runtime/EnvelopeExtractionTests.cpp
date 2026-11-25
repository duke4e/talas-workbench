#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/EnvelopeExtraction.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

// A Sound's amplitude or frequency envelope
// extracted to control a parameter.
namespace
{
void require(bool value, std::string_view message)
{
    if (!value)
        throw std::runtime_error(std::string(message));
}

const auto& registry()
{
    static const auto r = talas::core::makeBuiltinNodeRegistry();
    return r;
}

talas::project::ProjectDocument tone(std::string amplitude)
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition sound;
    sound.definitionId = "honey";
    sound.displayName = "karsten honey pie";
    talas::project::ProjectNode osc{"osc", "talas.oscillator", {}};
    talas::project::ProjectParameterExpression frequency;
    frequency.parameterId = "frequency";
    frequency.expression = "440 hz";
    talas::project::ProjectParameterExpression level;
    level.parameterId = "amplitude";
    level.expression = std::move(amplitude);
    osc.parameters = {frequency, level};
    sound.graph.nodes = {osc};
    sound.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    document.soundDefinitions = {sound};
    return document;
}

double meanOf(const std::vector<double>& values, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (auto index = from; index < to; ++index)
        sum += values[index];
    return sum / static_cast<double>(to - from);
}

void testTheAmplitudeEnvelope()
{
    talas::runtime::EnvelopeExtractionRequest request;
    request.soundDefinitionId = "honey";
    request.durationSeconds = 0.5;
    request.sampleRate = 8000.0;
    const auto loud = talas::runtime::extractEnvelope(tone("0.8"), registry(), std::nullopt, request);
    const auto soft = talas::runtime::extractEnvelope(tone("0.2"), registry(), std::nullopt, request);
    require(loud.ok() && soft.ok() && loud.values.size() == 4000, "the amplitude envelope extracts: " + loud.error + soft.error);
    const auto loudLevel = meanOf(loud.values, 2000, 4000);
    const auto softLevel = meanOf(soft.values, 2000, 4000);
    require(loudLevel > 0.1 && std::abs(loudLevel / softLevel - 4.0) < 0.2,
        "the amplitude envelope follows the Sound's level");
}

void testTheFrequencyEnvelope()
{
    talas::runtime::EnvelopeExtractionRequest request;
    request.soundDefinitionId = "honey";
    request.kind = talas::runtime::EnvelopeKind::Frequency;
    request.durationSeconds = 0.5;
    request.sampleRate = 8000.0;
    request.minimumHz = 100.0;
    request.maximumHz = 1000.0;
    const auto extracted = talas::runtime::extractEnvelope(tone("0.5"), registry(), std::nullopt, request);
    require(extracted.ok(), "the frequency envelope extracts: " + extracted.error);
    require(std::abs(meanOf(extracted.values, 2000, 4000) - 440.0) < 10.0,
        "the controller follows the melody of the Sound, in hertz");
    request.maximumHz = 5000.0;
    require(!talas::runtime::extractEnvelope(tone("0.5"), registry(), std::nullopt, request).ok(),
        "the range of frequencies to expect lies below half the sample rate");
    request.maximumHz = 1000.0;
    request.durationSeconds = 0.0;
    require(!talas::runtime::extractEnvelope(tone("0.5"), registry(), std::nullopt, request).ok(),
        "the extraction should ask how much of the envelope to record");
}

void testTheSoundsDuration()
{
    auto document = tone("0.5");
    require(!talas::runtime::envelopeSoundDuration(document, "honey").has_value(), "a synthetic Sound has no duration of its own");
    talas::project::ProjectAsset sample;
    sample.id = "pie";
    sample.kind = talas::project::ProjectAssetKind::Audio;
    sample.sampleRate = 44100.0;
    sample.frameCount = 280476;
    document.assets = {sample};
    talas::project::ProjectParameterExpression file;
    file.parameterId = "fileName";
    file.resource = "pie";
    document.soundDefinitions.front().graph.nodes.push_back({"player", "talas.diskPlayer", {file}});
    const auto duration = talas::runtime::envelopeSoundDuration(document, "honey");
    require(duration.has_value() && std::abs(*duration - 6.36) < 0.001,
        "the default duration corresponds to the duration of the sample");
}
}

int main()
{
    try
    {
        testTheAmplitudeEnvelope();
        testTheFrequencyEnvelope();
        testTheSoundsDuration();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
