#include "talas/core/ResourceSnapshot.h"
#include "talas/project/ProjectDiskCache.h"
#include "talas/runtime/PreparedGraphSummary.h"
#include "talas/runtime/Runtime.h"

#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::runtime::ParameterValue numeric(std::string id, double value)
{
    talas::runtime::ParameterValue result;
    result.id = std::move(id);
    result.value = value;
    return result;
}

talas::runtime::ParameterValue choice(std::string id, std::string value)
{
    auto result = numeric(std::move(id), 0.0);
    result.choice = std::move(value);
    return result;
}

talas::runtime::ParameterValue boolean(std::string id, bool value)
{
    auto result = numeric(std::move(id), 0.0);
    result.boolean = value;
    return result;
}

talas::runtime::ParameterValue resource(std::string id, std::string value)
{
    auto result = numeric(std::move(id), 0.0);
    result.resource = std::move(value);
    return result;
}

talas::runtime::ParameterValue text(std::string id, std::string value)
{
    auto result = numeric(std::move(id), 0.0);
    result.text = std::move(value);
    return result;
}

talas::runtime::CompileStep step(
    std::string nodeId,
    std::string typeId,
    std::string path = {})
{
    talas::runtime::CompileStep result;
    result.nodeId = std::move(nodeId);
    result.typeId = std::move(typeId);
    result.structuralSourcePath = std::move(path);
    return result;
}

void setExactLatency(
    talas::runtime::CompileStep& target,
    std::size_t intrinsicFrames,
    std::size_t cumulativeInputFrames = 0)
{
    target.resolvedLatencyFrames = intrinsicFrames;
    target.cumulativeInputLatencyFrames = cumulativeInputFrames;
    target.cumulativeOutputLatencyFrames =
        cumulativeInputFrames + intrinsicFrames;
}

talas::runtime::PreparedRuntimeGraph graphWithSettings(
    talas::runtime::RuntimePlan plan,
    talas::runtime::RenderSettings settings)
{
    if (plan.processorPlacement.empty())
    {
        const auto placement =
            talas::runtime::buildProcessorPlacementPlan(plan);
        if (placement.ok())
            plan.processorPlacement = *placement.plan;
    }
    return talas::runtime::PreparedRuntimeGraph{
        std::move(plan),
        std::move(settings),
    };
}

void testSummaryPublishesOutputsLatencyResourcesAndStreams()
{
    talas::runtime::RuntimePlan plan;
    auto matrix = step("matrix", "talas.matrix4", "voice/matrix");
    matrix.scopeId = 7;
    matrix.inputs = {
        {"input1", "dry", "audioOut", 6, 7},
        {"input2", "wet", "audioOut", 5, 7},
    };
    matrix.inputLatencyFrames = {16, 64};
    matrix.cumulativeInputLatencyFrames = 64;
    matrix.cumulativeOutputLatencyFrames = 64;
    plan.steps.push_back(matrix);

    auto player = step("player", "talas.diskPlayer", "voice/player");
    player.scopeId = 7;
    player.parameters = {resource("fileName", "audio/voice")};
    plan.steps.push_back(player);

    auto cache = step("cache", "talas.diskCache", "voice/cache");
    cache.scopeId = 7;
    cache.parameters = {
        resource("fileName", "audio/voice"),
        boolean("record", false),
    };
    plan.steps.push_back(cache);
    auto dry = step("dry", "talas.summaryFixture", "voice/dry");
    dry.scopeId = 7;
    plan.steps.push_back(std::move(dry));
    auto wet = step("wet", "talas.summaryFixture", "voice/wet");
    wet.scopeId = 7;
    plan.steps.push_back(std::move(wet));

    for (std::size_t channel = 0; channel < 4; ++channel)
    {
        talas::runtime::BufferSpec buffer;
        buffer.nodeId = "matrix";
        buffer.portId = "output" + std::to_string(channel + 1);
        buffer.type = talas::core::PortType::Audio;
        buffer.frames = 256;
        buffer.scopeId = 7;
        plan.buffers.push_back(std::move(buffer));
        plan.outputBufferIndices.push_back(channel);
    }
    plan.buffers.push_back({"dry", "audioOut", talas::core::PortType::Audio, 256, 7});
    plan.buffers.push_back({"wet", "audioOut", talas::core::PortType::Audio, 256, 7});
    plan.buffers.push_back({"dry", "audioOut", talas::core::PortType::Audio, 256, 7});
    plan.outputBufferIndex = 0;
    plan.criticalPathLatencyFrames = 64;
    plan.totalOutputLatencyFrames = 64;
    plan.latencyCompensations.push_back({0, 0, 4, 6, 48});
    plan.latencyWarnings.push_back({
        talas::runtime::LatencyPlanWarningKind::UncompensatedFeedback,
        0,
        "matrix",
        "Feedback latency is unknown",
    });

    auto snapshot = std::make_shared<talas::core::ResourceSnapshot>();
    snapshot->id = "audio/voice";
    snapshot->kind = talas::core::ResourceSnapshotKind::Audio;
    snapshot->generation = 9;
    snapshot->contentHash = std::string(64, 'a');
    snapshot->channelCount = 2;
    snapshot->sampleRate = 48000.0;
    snapshot->frameCount = 96000;
    snapshot->encoding = "pcm16-wav";

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 256;
    settings.resourceSnapshots.push_back(std::move(snapshot));

    const auto result = talas::runtime::makePreparedGraphSummary(
        graphWithSettings(std::move(plan), std::move(settings)));
    require(result.ok(), "prepared M3 summary should accept exact plan authority: " + result.error);
    const auto& summary = *result.summary;
    require(summary.sampleRate == 48000.0 && summary.maxBlockFrames == 256,
        "summary should retain the prepared render configuration");
    require(summary.outputLayout == talas::runtime::PreparedOutputLayout::Discrete
            && summary.outputs.size() == 4,
        "four ordered buffers should publish a discrete-four layout");
    require(summary.outputs[2].projectOrdinal == 3
            && summary.outputs[2].nodeId == "matrix"
            && summary.outputs[2].portId == "output3"
            && summary.outputs[2].scopeId == 7
            && summary.outputs[2].structuralSourcePath == "voice/matrix",
        "output records should retain exact logical and structural identity");
    require(summary.latency.criticalPathFrames == 64
            && summary.latency.terminalOutputFrames == 64
            && summary.latency.compensations.size() == 1,
        "summary should retain critical, terminal, and compensation authority");
    const auto& compensation = summary.latency.compensations.front();
    require(compensation.nodeId == "matrix"
            && compensation.inputPortId == "input1"
            && compensation.uncompensatedFrames == 16
            && compensation.insertedFrames == 48
            && compensation.compensatedFrames == 64,
        "compensation record should expose exact before/delay/after frames");
    require(summary.latency.warnings.size() == 1
            && summary.latency.warnings.front().structuralSourcePath == "voice/matrix",
        "latency warnings should be structurally scoped");
    require(summary.resources.size() == 1
            && summary.resources.front().health == talas::runtime::PreparedResourceHealth::Verified
            && summary.resources.front().generation == 9
            && summary.resources.front().contentHash == std::string(64, 'a')
            && summary.resources.front().channelCount == 2
            && summary.resources.front().frameCount == 96000,
        "resource summary should publish generation, hash health, and media shape");
    require(summary.streams.size() == 2
            && summary.streams[0].role == talas::runtime::PreparedStreamRole::Playback
            && summary.streams[1].role == talas::runtime::PreparedStreamRole::CachePrewarm
            && summary.streams[0].resourceId == "audio/voice"
            && !summary.streams[0].limitationIds.empty()
            && !summary.streams[1].limitationIds.empty(),
        "DiskPlayer and DiskCache should expose scoped static contracts and limitations");
}

std::vector<talas::runtime::ParameterValue> nativeStftParameters(
    std::size_t frameSize,
    std::size_t hopSize,
    std::string window)
{
    return {
        numeric("frameSize", static_cast<double>(frameSize)),
        numeric("hopSize", static_cast<double>(hopSize)),
        choice("window", std::move(window)),
    };
}

std::vector<talas::runtime::ParameterValue> liveSpectralParameters()
{
    return {
        numeric("lowestAnalyzedFreq", 750.0),
        numeric("ampScale", 1.0),
        numeric("freqScale", 1.0),
        text("response", ""),
        boolean("harmonic", false),
        numeric("unpitchedOnly", 0.0),
        numeric("unpitchedThreshold", 0.0),
        numeric("trackedHarmonic", 1.0),
        numeric("lowFreq", 20.0),
        numeric("highFreq", 12000.0),
        numeric("initFreq", 440.0),
        boolean("fundamentalOnly", false),
    };
}

void testSummaryDistinguishesEveryM3SpectralContract()
{
    talas::runtime::RuntimePlan plan;

    auto audioSource = step(
        "audioSource", "talas.oscillator", "voice/audioSource");
    setExactLatency(audioSource, 0);
    plan.steps.push_back(std::move(audioSource));

    auto forward = step("forward", "talas.forwardFftV1", "voice/forward");
    forward.parameters = nativeStftParameters(64, 16, "hann");
    forward.preparedSpectralFrameSize = 64;
    forward.preparedSpectralHopSize = 16;
    forward.inputs.push_back({
        "audioIn", "audioSource", "audioOut", 0, 0});
    forward.inputLatencyFrames = {0};
    setExactLatency(forward, 63);
    plan.steps.push_back(std::move(forward));

    auto inverse = step("inverse", "talas.inverseFftV1", "voice/inverse");
    inverse.parameters = nativeStftParameters(64, 16, "hann");
    inverse.preparedSpectralFrameSize = 64;
    inverse.preparedSpectralHopSize = 16;
    inverse.inputs.push_back({
        "spectrumIn", "forward", "spectrumOut", 0, 0});
    inverse.inputLatencyFrames = {63};
    setExactLatency(inverse, 0, 63);
    plan.steps.push_back(std::move(inverse));

    auto packedForward = step("packedF", "talas.fft", "packedF");
    packedForward.parameters = {numeric("length", 128), boolean("inverse", false), choice("packing", "legacyPacked")};
    packedForward.inputs.push_back({
        "audioIn", "audioSource", "audioOut", 0, 0});
    packedForward.inputLatencyFrames = {0};
    setExactLatency(packedForward, 127);
    plan.steps.push_back(std::move(packedForward));

    auto packedInverse = step("packedI", "talas.fft", "packedI");
    packedInverse.parameters = {numeric("length", 128), boolean("inverse", true), choice("packing", "legacyPacked")};
    packedInverse.inputs.push_back({
        "audioIn", "packedF", "audioOut", 0, 0});
    packedInverse.inputLatencyFrames = {127};
    setExactLatency(packedInverse, 127, 127);
    plan.steps.push_back(std::move(packedInverse));

    auto liveSource = step(
        "liveSource", "talas.delay", "voice/liveSource");
    setExactLatency(liveSource, 8);
    plan.steps.push_back(std::move(liveSource));

    auto live = step("live", "talas.liveSpectralAnalysis", "voice/live");
    live.parameters = liveSpectralParameters();
    live.preparedSpectralFrameSize = 64;
    live.preparedSpectralHopSize = 16;
    live.inputs.push_back({
        "audioIn", "liveSource", "audioOut", 0, 0});
    live.inputLatencyFrames = {8};
    setExactLatency(live, 63, 8);
    plan.steps.push_back(std::move(live));

    auto hold = step("hold", "talas.partialSpectrumLegacyHoldV1", "voice/hold");
    hold.inputs.push_back({"spectrumIn", "live", "spectrumOut", 0, 0});
    hold.inputLatencyFrames = {71};
    setExactLatency(hold, 0, 71);
    plan.steps.push_back(std::move(hold));

    // A stereo-packed FFT on two channels, the right arriving later.
    auto stereoForward = step("stereoF", "talas.fft", "stereoF");
    stereoForward.parameters = {numeric("length", 16), boolean("inverse", false), choice("packing", "talasStereo")};
    stereoForward.inputs.push_back({"audioIn", "audioSource", "audioOut", 0, 0});
    stereoForward.inputs.push_back({"rightIn", "liveSource", "audioOut", 0, 0});
    stereoForward.inputLatencyFrames = {0, 8};
    setExactLatency(stereoForward, 15, 8);
    plan.steps.push_back(std::move(stereoForward));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 256;
    const auto result = talas::runtime::makePreparedGraphSummary(
        graphWithSettings(std::move(plan), settings));
    require(result.ok(), "all M3 spectral contracts should summarize: " + result.error);
    const auto& spectral = result.summary->spectralNodes;
    require(spectral.size() == 7, "summary should include native pair, packed pair, live frames, hold, and stereo FFT");
    require(spectral[0].format == talas::runtime::PreparedSpectralFormat::ComplexSpectrumV1
            && spectral[0].role == talas::runtime::PreparedSpectralRole::Forward
            && spectral[0].frameSize == 64
            && spectral[0].hopSize == 16
            && spectral[0].window == talas::dsp::SpectralWindowKind::Hann
            && spectral[0].intrinsicLatencyFrames == 63
            && spectral[0].cumulativeOutputLatencyFrames == 63
            && std::abs(spectral[0].cadenceHz - 3000.0) < 1.0e-12,
        "native forward STFT should expose complex N/H/window/cadence truth");
    require(spectral[1].format == talas::runtime::PreparedSpectralFormat::ComplexSpectrumV1
            && spectral[1].role == talas::runtime::PreparedSpectralRole::Inverse
            && spectral[1].intrinsicLatencyFrames == 0
            && spectral[1].cumulativeOutputLatencyFrames == 63,
        "native inverse STFT should retain complex inverse identity");
    require(spectral[2].format == talas::runtime::PreparedSpectralFormat::PackedRealAudioV1
            && spectral[2].role == talas::runtime::PreparedSpectralRole::Forward
            && spectral[2].frameSize == 128
            && spectral[2].hopSize == 128
            && spectral[2].window == talas::dsp::SpectralWindowKind::Rectangular
            && spectral[2].intrinsicLatencyFrames == 127
            && !spectral[2].limitationIds.empty(),
        "historical forward FFT must stay explicitly packed Audio");
    require(spectral[3].format == talas::runtime::PreparedSpectralFormat::PackedRealAudioV1
            && spectral[3].role == talas::runtime::PreparedSpectralRole::Inverse
            && spectral[3].intrinsicLatencyFrames == 127,
        "historical inverse FFT must stay explicitly packed Audio");
    require(spectral[4].format == talas::runtime::PreparedSpectralFormat::PartialSpectrumV1
            && spectral[4].role == talas::runtime::PreparedSpectralRole::Analysis
            && spectral[4].frameSize == 64
            && spectral[4].hopSize == 16
            && spectral[4].intrinsicLatencyFrames == 63
            && spectral[4].cumulativeOutputLatencyFrames == 71
            && !spectral[4].limitationIds.empty(),
        "LiveSpectralAnalysis should expose partial frame timing and subset limitations");
    require(spectral[5].format == talas::runtime::PreparedSpectralFormat::LegacyPartialTriples
            && spectral[5].role == talas::runtime::PreparedSpectralRole::CompatibilityHold
            && spectral[5].frameSize == 64
            && spectral[5].hopSize == 16
            && spectral[5].window == talas::dsp::SpectralWindowKind::Hann
            && spectral[5].limitationIds.empty(),
        "legacy hold should inherit exact source timing and see every frame (ADR-0016)");
    require(spectral[6].format == talas::runtime::PreparedSpectralFormat::TalasStereoAudioV1
            && spectral[6].role == talas::runtime::PreparedSpectralRole::Forward
            && spectral[6].frameSize == 16
            && spectral[6].intrinsicLatencyFrames == 15
            && spectral[6].cumulativeOutputLatencyFrames == 23
            && !spectral[6].limitationIds.empty()
            && spectral[6].limitationIds.front() == "packed-stereo-audio-layout",
        "a stereo FFT should report its stereo layout, its frames starting from the later channel");
}

void testSummaryRejectsTamperedPreparedAuthority()
{
    talas::runtime::RuntimePlan plan;
    auto source = step("source", "talas.oscillator", "source");
    setExactLatency(source, 0);
    plan.steps.push_back(std::move(source));
    auto forward = step("forward", "talas.forwardFftV1", "forward");
    forward.parameters = nativeStftParameters(64, 16, "hann");
    forward.preparedSpectralFrameSize = 128;
    forward.preparedSpectralHopSize = 16;
    forward.inputs.push_back({
        "audioIn", "source", "audioOut", 0, 0});
    forward.inputLatencyFrames = {0};
    setExactLatency(forward, 63);
    plan.steps.push_back(std::move(forward));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 256;
    auto result = talas::runtime::makePreparedGraphSummary(
        graphWithSettings(plan, settings));
    require(!result.ok()
            && result.error.find("forward") != std::string::npos,
        "summary must reject inconsistent prepared spectral authority with node identity");

    const auto requireRejectedLatency = [&settings](
        talas::runtime::CompileStep tampered) {
        talas::runtime::RuntimePlan tamperedPlan;
        const auto nodeId = tampered.nodeId;
        auto source = step(
            "tamperSource", "talas.oscillator", "tamperSource");
        setExactLatency(source, 0);
        tamperedPlan.steps.push_back(std::move(source));
        if (tampered.typeId == "talas.inverseFftV1")
        {
            auto nativeForward = step(
                "tamperForward",
                "talas.forwardFftV1",
                "tamperForward");
            nativeForward.parameters =
                nativeStftParameters(64, 16, "hann");
            nativeForward.preparedSpectralFrameSize = 64;
            nativeForward.preparedSpectralHopSize = 16;
            nativeForward.inputs.push_back({
                "audioIn", "tamperSource", "audioOut", 0, 0});
            nativeForward.inputLatencyFrames = {0};
            setExactLatency(nativeForward, 63);
            tamperedPlan.steps.push_back(
                std::move(nativeForward));
            tampered.inputs.push_back({
                "spectrumIn",
                "tamperForward",
                "spectrumOut",
                0,
                0,
            });
            tampered.inputLatencyFrames = {63};
        }
        else
        {
            tampered.inputs.push_back({
                "audioIn", "tamperSource", "audioOut", 0, 0});
            tampered.inputLatencyFrames = {0};
        }
        tamperedPlan.steps.push_back(std::move(tampered));
        const auto tamperedResult =
            talas::runtime::makePreparedGraphSummary(
                graphWithSettings(
                    std::move(tamperedPlan),
                    settings));
        require(!tamperedResult.ok()
                && tamperedResult.error.find(nodeId)
                    != std::string::npos,
            "summary must reject tampered intrinsic latency for '"
                + nodeId + "'");
    };

    auto wrongForward = step(
        "wrongForward", "talas.forwardFftV1", "wrongForward");
    wrongForward.parameters = nativeStftParameters(64, 16, "hann");
    wrongForward.preparedSpectralFrameSize = 64;
    wrongForward.preparedSpectralHopSize = 16;
    setExactLatency(wrongForward, 0);
    requireRejectedLatency(std::move(wrongForward));

    auto wrongInverse = step(
        "wrongInverse", "talas.inverseFftV1", "wrongInverse");
    wrongInverse.parameters = nativeStftParameters(64, 16, "hann");
    wrongInverse.preparedSpectralFrameSize = 64;
    wrongInverse.preparedSpectralHopSize = 16;
    setExactLatency(wrongInverse, 63, 63);
    requireRejectedLatency(std::move(wrongInverse));

    auto wrongPacked = step("wrongPacked", "talas.fft", "wrongPacked");
    wrongPacked.parameters = {
        numeric("length", 128),
        boolean("inverse", true),
        choice("packing", "legacyPacked"),
    };
    setExactLatency(wrongPacked, 0);
    requireRejectedLatency(std::move(wrongPacked));

    auto wrongLive = step(
        "wrongLive", "talas.liveSpectralAnalysis", "wrongLive");
    wrongLive.parameters = liveSpectralParameters();
    wrongLive.preparedSpectralFrameSize = 64;
    wrongLive.preparedSpectralHopSize = 16;
    setExactLatency(wrongLive, 0);
    requireRejectedLatency(std::move(wrongLive));

    talas::runtime::RuntimePlan wrongCumulativePlan;
    auto cumulativeSource = step(
        "cumulativeSource", "talas.oscillator", "cumulativeSource");
    setExactLatency(cumulativeSource, 0);
    wrongCumulativePlan.steps.push_back(
        std::move(cumulativeSource));
    auto wrongCumulative = step(
        "wrongCumulative", "talas.forwardFftV1", "wrongCumulative");
    wrongCumulative.parameters =
        nativeStftParameters(64, 16, "hann");
    wrongCumulative.preparedSpectralFrameSize = 64;
    wrongCumulative.preparedSpectralHopSize = 16;
    wrongCumulative.inputs.push_back({
        "audioIn", "cumulativeSource", "audioOut", 0, 0});
    wrongCumulative.inputLatencyFrames = {0};
    setExactLatency(wrongCumulative, 63, 8);
    wrongCumulativePlan.steps.push_back(
        std::move(wrongCumulative));
    const auto wrongCumulativeResult =
        talas::runtime::makePreparedGraphSummary(
            graphWithSettings(
                std::move(wrongCumulativePlan), settings));
    require(!wrongCumulativeResult.ok()
            && wrongCumulativeResult.error.find("wrongCumulative")
                != std::string::npos,
        "summary must bind cumulative latency to the exact connected source");

    talas::runtime::RuntimePlan wrongHoldPlan;
    auto holdAudioSource = step(
        "holdAudioSource", "talas.oscillator", "holdAudioSource");
    setExactLatency(holdAudioSource, 0);
    wrongHoldPlan.steps.push_back(std::move(holdAudioSource));
    auto holdSource = step(
        "holdSource", "talas.liveSpectralAnalysis", "holdSource");
    holdSource.parameters = liveSpectralParameters();
    holdSource.preparedSpectralFrameSize = 64;
    holdSource.preparedSpectralHopSize = 16;
    holdSource.inputs.push_back({
        "audioIn", "holdAudioSource", "audioOut", 0, 0});
    holdSource.inputLatencyFrames = {0};
    setExactLatency(holdSource, 63);
    wrongHoldPlan.steps.push_back(std::move(holdSource));
    auto wrongHold = step(
        "wrongHold", "talas.partialSpectrumLegacyHoldV1", "wrongHold");
    wrongHold.inputs.push_back({
        "spectrumIn", "holdSource", "spectrumOut", 0, 0});
    wrongHold.inputLatencyFrames = {63};
    setExactLatency(wrongHold, 0, 70);
    wrongHoldPlan.steps.push_back(std::move(wrongHold));
    const auto wrongHoldResult =
        talas::runtime::makePreparedGraphSummary(
            graphWithSettings(std::move(wrongHoldPlan), settings));
    require(!wrongHoldResult.ok()
            && wrongHoldResult.error.find("wrongHold")
                != std::string::npos,
        "summary must reject tampered legacy-hold latency authority");

    plan.steps.clear();
    auto invalid = std::make_shared<talas::core::ResourceSnapshot>();
    invalid->id = "broken";
    invalid->kind = talas::core::ResourceSnapshotKind::Audio;
    invalid->generation = 0;
    invalid->contentHash = "not-a-hash";
    settings.resourceSnapshots = {invalid};
    result = talas::runtime::makePreparedGraphSummary(
        graphWithSettings(std::move(plan), std::move(settings)));
    require(!result.ok()
            && result.error.find("broken") != std::string::npos,
        "summary must reject unverified resource identity rather than omit it");
}

void testSummaryPublishesCompiledMidiMapperAuthority()
{
    talas::runtime::RuntimePlan plan;
    auto blank = step("blank", "talas.midiMapper", "root/blank");
    blank.parameters = {
        text("map", ""),
        numeric("channel", 0),
        numeric("lowPitch", 36),
        numeric("highPitch", 84),
        numeric("polyphony", 4),
    };
    plan.steps.push_back(std::move(blank));

    auto velocity = step(
        "velocity", "talas.midiMapper", "sound/mapper");
    velocity.scopeId = 7;
    velocity.parameters = {
        text("map", "velocity"),
        numeric("channel", 0),
        numeric("lowPitch", 24),
        numeric("highPitch", 96),
        numeric("polyphony", 8),
    };
    plan.steps.push_back(std::move(velocity));

    talas::runtime::PreparedMidiMapperConfiguration configuration;
    configuration.mode =
        talas::runtime::PreparedMidiMapperMode::Velocity;
    configuration.channel = 0;
    configuration.lowPitch = 24;
    configuration.highPitch = 96;
    configuration.polyphony = 8;
    plan.midiMappers.push_back({1, 7, configuration, "velocity"});

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 256;
    const auto result = talas::runtime::makePreparedGraphSummary(
        graphWithSettings(std::move(plan), std::move(settings)));
    require(result.ok(),
        "prepared summary should accept compiled MIDI Mapper authority: "
            + result.error);
    require(result.summary->midiMappers.size() == 2,
        "summary should publish blank and mapped MIDI Mapper modes");
    const auto& blankSummary = result.summary->midiMappers[0];
    const auto& velocitySummary = result.summary->midiMappers[1];
    require(blankSummary.nodeId == "blank"
            && blankSummary.structuralSourcePath == "root/blank"
            && blankSummary.configuration.mode
                == talas::runtime::PreparedMidiMapperMode::Passthrough
            && talas::runtime::preparedMidiMapperModeName(
                   blankSummary.configuration.mode)
                == "blank passthrough"
            && blankSummary.configuration.channel == 0
            && blankSummary.configuration.lowPitch == 36
            && blankSummary.configuration.highPitch == 84
            && blankSummary.configuration.polyphony == 4,
        "blank mapper should retain exact structural identity and configured authority");
    require(velocitySummary.nodeId == "velocity"
            && velocitySummary.scopeId == 7
            && velocitySummary.structuralSourcePath == "sound/mapper"
            && velocitySummary.configuration.mode
                == talas::runtime::PreparedMidiMapperMode::Velocity
            && velocitySummary.configuration.channel == 0
            && velocitySummary.configuration.lowPitch == 24
            && velocitySummary.configuration.highPitch == 96
            && velocitySummary.configuration.polyphony == 8
            && talas::runtime::preparedMidiMapperModeName(
                   velocitySummary.configuration.mode)
                == "velocity",
        "mapped summary should expose exact compiled mapper configuration");
}

// A MIDIMapper's LowPitch or HighPitch given as a frequency is the
// nearest note, as the renderers take it; 300 hz is note 62.04.
void testSummaryRoundsMidiMapperPitchBoundsGivenAsFrequencies()
{
    talas::runtime::RuntimePlan plan;
    auto mapper = step("mapper", "talas.midiMapper", "root/mapper");
    mapper.parameters = {
        text("map", ""),
        numeric("channel", 0),
        numeric("lowPitch", 69.0 + 12.0 * std::log2(300.0 / 440.0)),
        numeric("highPitch", 84),
        numeric("polyphony", 4),
    };
    plan.steps.push_back(std::move(mapper));
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 256;
    const auto result = talas::runtime::makePreparedGraphSummary(
        graphWithSettings(std::move(plan), std::move(settings)));
    require(result.ok() && result.summary->midiMappers.size() == 1
            && result.summary->midiMappers[0].configuration.lowPitch == 62
            && result.summary->midiMappers[0].configuration.highPitch == 84,
        "a pitch bound given as a frequency should stage as the nearest note: "
            + result.error);
}

void testSummaryPublishesCapturedDiskCachePlaybackTruth()
{
    talas::runtime::RuntimePlan plan;
    auto cache = step("cache", "talas.diskCache", "voice/cache");
    cache.parameters = {
        resource("fileName", "Audio/Cache"),
        boolean("record", false),
    };
    cache.diskCachePlayback = talas::runtime::PreparedDiskCachePlayback{
        "Audio/Cache",
        "DiskCache/cache-g3.wav",
        3,
        std::string(64, 'b'),
        1,
        48000.0,
        24000,
        "pcm16-wav",
        std::string(64, 'a'),
        talas::project::makeDiskCacheCaptureProvenance(
            std::string(64, 'a')),
    };
    plan.steps.push_back(std::move(cache));

    auto snapshot = std::make_shared<talas::core::ResourceSnapshot>();
    snapshot->id = "Audio/Cache";
    snapshot->kind = talas::core::ResourceSnapshotKind::Audio;
    snapshot->generation = 3;
    snapshot->contentHash = std::string(64, 'b');
    snapshot->channelCount = 1;
    snapshot->sampleRate = 48000.0;
    snapshot->frameCount = 24000;
    snapshot->encoding = "pcm16-wav";
    snapshot->projectAssetPath = "DiskCache/cache-g3.wav";
    snapshot->provenance = talas::project::makeDiskCacheCaptureProvenance(
        std::string(64, 'a'));
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 256;
    settings.resourceSnapshots.push_back(std::move(snapshot));

    auto forgedPlan = plan;
    auto forgedSettings = settings;
    ++forgedPlan.steps.front()
          .diskCachePlayback->generation;
    auto downgradedPlan = plan;
    auto downgradedSettings = settings;
    downgradedPlan.steps.front().diskCachePlayback.reset();

    const auto result = talas::runtime::makePreparedGraphSummary(
        graphWithSettings(std::move(plan), std::move(settings)));
    require(
        result.ok()
            && result.summary->streams.size() == 1
            && result.summary->streams.front().role
                == talas::runtime::PreparedStreamRole::Playback
            && result.summary->streams.front().limitationIds
                == std::vector<std::string>{
                    "stream-mono-or-stereo-wav-or-aiff-only",
                    "disk-cache-captured-pcm16-sequential-playback",
                    "disk-cache-record-explicit-operation-only",
                },
        "prepared summary should distinguish captured playback from legacy cache prewarm truth: "
            + result.error);

    const auto forged = talas::runtime::makePreparedGraphSummary(
        graphWithSettings(
            std::move(forgedPlan),
            std::move(forgedSettings)));
    require(
        !forged.ok()
            && forged.error.find("forged or stale")
                != std::string::npos,
        "prepared summary must reject a mismatched DiskCache playback generation instead of publishing optimistic product truth");

    const auto downgraded = talas::runtime::makePreparedGraphSummary(
        graphWithSettings(
            std::move(downgradedPlan),
            std::move(downgradedSettings)));
    require(
        !downgraded.ok()
            && downgraded.error.find("forged or stale")
                != std::string::npos,
        "prepared summary must reject removal of captured DiskCache playback authority");
}

void testSummaryPublishesAndValidatesProcessorPlacement()
{
    talas::runtime::RuntimePlan plan;
    plan.steps.push_back(step("osc", "talas.oscillator", "voice/osc"));
    auto inner = step(
        "inner", "talas.forcedProcessorAssignment", "voice/inner");
    inner.parameters = {numeric("processor", 3.0)};
    inner.inputs = {{"audioIn", "osc", "audioOut", 0, 0}};
    plan.steps.push_back(std::move(inner));
    auto outer = step(
        "outer", "talas.forcedProcessorAssignment", "voice/outer");
    outer.parameters = {numeric("processor", 7.0)};
    outer.inputs = {{"audioIn", "inner", "audioOut", 0, 0}};
    plan.steps.push_back(std::move(outer));
    auto output = step("out", "talas.audioOutput", "voice/out");
    output.inputs = {{"audioIn", "outer", "audioOut", 0, 0}};
    plan.steps.push_back(std::move(output));
    plan.buffers.push_back({
        "outer", "audioOut", talas::core::PortType::Audio, 256, 0});
    plan.outputBufferIndex = 0;
    plan.outputBufferIndices = {0};
    const auto placement =
        talas::runtime::buildProcessorPlacementPlan(plan);
    require(placement.ok(), "summary placement fixture should build");
    plan.processorPlacement = *placement.plan;

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 256;
    auto forgedPlan = plan;
    forgedPlan.processorPlacement.transfers.front()
        .targetProcessorId = 99;

    const auto result = talas::runtime::makePreparedGraphSummary(
        graphWithSettings(plan, settings));
    require(
        result.ok(),
        "summary should publish exact processor placement: "
            + result.error);
    require(
        result.summary->processorPlacements.size() == 3
            && result.summary->processorPlacements[0].processorId == 0
            && result.summary->processorPlacements[1].processorId == 3
            && result.summary->processorPlacements[2].processorId == 7,
        "summary should retain canonical host/inner/outer domain order");
    const auto& innerDomain =
        result.summary->processorPlacements[1];
    require(
        innerDomain.steps.size() == 2
            && innerDomain.steps[0].nodeId == "osc"
            && innerDomain.steps[0].structuralSourcePath == "voice/osc"
            && innerDomain.boundaries.size() == 1
            && innerDomain.boundaries.front().nodeId == "inner",
        "summary should retain exact placed step and boundary identities");
    require(
        result.summary->processorTransfers.size() == 2
            && result.summary->processorTransfers[0].source.nodeId == "inner"
            && result.summary->processorTransfers[0].sourcePortId == "audioOut"
            && result.summary->processorTransfers[0].target.nodeId == "outer"
            && result.summary->processorTransfers[0].targetPortId == "audioIn"
            && result.summary->processorTransfers[0].sourceProcessorId == 3
            && result.summary->processorTransfers[0].targetProcessorId == 7,
        "summary should retain exact cross-domain connection authority");

    const auto forged = talas::runtime::makePreparedGraphSummary(
        graphWithSettings(
            std::move(forgedPlan),
            std::move(settings)));
    require(
        !forged.ok()
            && forged.error.find("processor placement")
                != std::string::npos,
        "summary should reject forged placement instead of publishing it");
}

void testSummaryPublishesAndValidatesDiskRecorderCapture()
{
    talas::runtime::RuntimePlan plan;
    auto source = step("osc", "talas.oscillator", "voice/osc");
    source.scopeId = 7;
    plan.steps.push_back(std::move(source));
    auto recorder = step(
        "recorder", "talas.diskRecorder", "voice/recorder");
    recorder.scopeId = 7;
    recorder.inputs = {{"audioIn", "osc", "audioOut", 0, 7}};
    recorder.parameters = {
        resource("fileName", "Recordings/Capture"),
        text("format", "WAV"),
        text("wordSize", "16"),
        choice("channels", "mono"),
        numeric("trigger", 1.0),
        numeric("captureDuration", 0.25),
        boolean("gated", false),
    };
    const auto triggerExpression = talas::expr::parse("1");
    const auto durationExpression = talas::expr::parse("0.25 s");
    require(triggerExpression.ok() && durationExpression.ok(),
        "recorder parameter fixture expressions should parse");
    recorder.parameters[4].expression = *triggerExpression.expression;
    recorder.parameters[5].domain = talas::expr::ValueDomain::Seconds;
    recorder.parameters[5].expression = *durationExpression.expression;
    plan.steps.push_back(std::move(recorder));
    plan.buffers.push_back({
        "osc", "audioOut", talas::core::PortType::Audio, 256, 7});
    plan.outputBufferIndex = 0;
    plan.outputBufferIndices = {0};
    plan.diskRecorderCapture = talas::runtime::PreparedDiskRecorderCapturePlan{
        1,
        0,
        std::nullopt,
        std::nullopt,
        "Recordings/Capture",
        "WAV",
        "16",
        "mono",
        1.0,
        0.25,
        false,
        talas::runtime::DiskRecorderWriteSupport::Writable,
    };
    const auto placement =
        talas::runtime::buildProcessorPlacementPlan(plan);
    require(placement.ok(), "disk recorder summary placement should build");
    plan.processorPlacement = *placement.plan;

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 256;
    auto forgedPlan = plan;
    forgedPlan.diskRecorderCapture->captureBufferIndex = 1;
    auto ambiguousPlan = plan;
    auto extraTerminal = step(
        "output", "talas.audioOutput", "voice/output");
    extraTerminal.scopeId = 7;
    extraTerminal.inputs = {{"audioIn", "osc", "audioOut", 0, 7}};
    ambiguousPlan.steps.push_back(std::move(extraTerminal));
    const auto ambiguousValidation =
        talas::runtime::validateDiskRecorderCapturePlan(ambiguousPlan);
    require(!ambiguousValidation.ok()
            && ambiguousValidation.error.find("exact and only terminal")
                != std::string::npos,
        "recorder authority must reject an additional terminal output");
    auto extraParameterPlan = plan;
    extraParameterPlan.steps[1].parameters.push_back(
        numeric("forgedExtra", 1.0));
    require(!talas::runtime::validateDiskRecorderCapturePlan(
                 extraParameterPlan).ok(),
        "recorder authority must reject parameters outside its closed schema");
    auto extraInputPlan = plan;
    extraInputPlan.steps[1].inputs.push_back(
        {"forgedExtra", "osc", "audioOut", 0, 7});
    require(!talas::runtime::validateDiskRecorderCapturePlan(
                 extraInputPlan).ok(),
        "recorder authority must reject inputs outside its closed schema");
    auto mixedParameterPlan = plan;
    mixedParameterPlan.steps[1].parameters[1].resource = "forged";
    require(!talas::runtime::validateDiskRecorderCapturePlan(
                 mixedParameterPlan).ok(),
        "recorder authority must reject contradictory parameter value kinds");

    const auto requireRejectedRecorderBuffer = [&settings](
        talas::runtime::RuntimePlan forged,
        std::string_view label) {
        const auto validation =
            talas::runtime::validateDiskRecorderCapturePlan(forged);
        require(!validation.ok()
                && validation.error.find("input buffer binding")
                    != std::string::npos,
            std::string{label}
                + " must fail the recorder validator at the binding boundary");
        const auto summary = talas::runtime::makePreparedGraphSummary(
            graphWithSettings(std::move(forged), settings));
        require(!summary.ok()
                && summary.error.find("Disk Recorder capture")
                    != std::string::npos,
            std::string{label}
                + " must fail before prepared recorder authority is summarized");
    };
    auto wrongBufferType = plan;
    wrongBufferType.buffers[0].type = talas::core::PortType::Control;
    requireRejectedRecorderBuffer(
        std::move(wrongBufferType), "wrong recorder buffer type");
    auto wrongBufferNode = plan;
    wrongBufferNode.buffers[0].nodeId = "forged";
    requireRejectedRecorderBuffer(
        std::move(wrongBufferNode), "wrong recorder buffer node");
    auto wrongBufferPort = plan;
    wrongBufferPort.buffers[0].portId = "forged";
    requireRejectedRecorderBuffer(
        std::move(wrongBufferPort), "wrong recorder buffer port");
    auto wrongBufferScope = plan;
    wrongBufferScope.buffers[0].scopeId = 99;
    requireRejectedRecorderBuffer(
        std::move(wrongBufferScope), "wrong recorder buffer scope");
    auto duplicateBufferIdentity = plan;
    duplicateBufferIdentity.buffers.push_back(
        duplicateBufferIdentity.buffers.front());
    duplicateBufferIdentity.steps[1].inputs.front().bufferIndex = 1;
    duplicateBufferIdentity.outputBufferIndex = 1;
    duplicateBufferIdentity.outputBufferIndices = {1};
    duplicateBufferIdentity.diskRecorderCapture->captureBufferIndex = 1;
    requireRejectedRecorderBuffer(
        std::move(duplicateBufferIdentity),
        "duplicate recorder buffer identity");

    const auto result = talas::runtime::makePreparedGraphSummary(
        graphWithSettings(plan, settings));
    require(result.ok(),
        "summary should publish exact disk recorder capture: "
            + result.error);
    require(result.summary->diskRecorderCapture.has_value(),
        "summary should expose one terminal disk recorder capture");
    const auto& capture = *result.summary->diskRecorderCapture;
    require(capture.nodeId == "recorder"
            && capture.scopeId == 7
            && capture.structuralSourcePath == "voice/recorder"
            && capture.destinationNameHint == "Recordings/Capture"
            && capture.writeSupport
                == talas::runtime::DiskRecorderWriteSupport::Writable
            && capture.captureBufferIndex == 0,
        "summary should retain exact recorder identity, hint, support, and buffer authority");

    const auto forged = talas::runtime::makePreparedGraphSummary(
        graphWithSettings(std::move(forgedPlan), std::move(settings)));
    require(!forged.ok()
            && forged.error.find("Disk Recorder capture")
                != std::string::npos,
        "summary should reject forged disk recorder capture authority");
    talas::runtime::RenderSettings extraSettings;
    extraSettings.sampleRate = 48000.0;
    extraSettings.maxBlockFrames = 256;
    const auto forgedExtra = talas::runtime::makePreparedGraphSummary(
        graphWithSettings(
            std::move(extraParameterPlan),
            std::move(extraSettings)));
    require(!forgedExtra.ok()
            && forgedExtra.error.find("Disk Recorder capture")
                != std::string::npos,
        "summary should reject a recorder profile with extra schema fields");
    talas::runtime::RenderSettings mixedSettings;
    mixedSettings.sampleRate = 48000.0;
    mixedSettings.maxBlockFrames = 256;
    const auto forgedMixed = talas::runtime::makePreparedGraphSummary(
        graphWithSettings(
            std::move(mixedParameterPlan),
            std::move(mixedSettings)));
    require(!forgedMixed.ok()
            && forgedMixed.error.find("Disk Recorder capture")
                != std::string::npos,
        "summary should reject contradictory recorder parameter value kinds");
}
}

int main()
{
    testSummaryPublishesOutputsLatencyResourcesAndStreams();
    testSummaryDistinguishesEveryM3SpectralContract();
    testSummaryRejectsTamperedPreparedAuthority();
    testSummaryPublishesCompiledMidiMapperAuthority();
    testSummaryRoundsMidiMapperPitchBoundsGivenAsFrequencies();
    testSummaryPublishesCapturedDiskCachePlaybackTruth();
    testSummaryPublishesAndValidatesProcessorPlacement();
    testSummaryPublishesAndValidatesDiskRecorderCapture();
    return 0;
}
