#include "talas/audio/PartialTrackAudioAnalysis.h"

#include "talas/audio/AudioFilePublication.h"
#include "talas/dsp/LiveSpectralAnalysis.h"
#include "talas/project/SosSpectrumFile.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <new>
#include <utility>
#include <vector>

namespace talas::audio
{
namespace
{
[[nodiscard]] talas::project::PartialTrackAnalysisResult failure(
    talas::project::PartialTrackAnalysisErrorCode code,
    std::string message)
{
    talas::project::PartialTrackAnalysisResult result;
    result.error = talas::project::PartialTrackAnalysisError{
        code, 0, std::move(message)};
    return result;
}
}

talas::project::PartialTrackAnalysisResult analyzeMonoAudioPartialTracks(
    std::span<const double> samples,
    double sampleRate,
    talas::project::PartialTrackAnalysisOptions options,
    std::function<bool()> shouldCancel)
{
    return analyzeMonoAudioPartialTracks(
        samples,
        sampleRate,
        PartialTrackAudioAnalysisSettings{},
        options,
        std::move(shouldCancel));
}

talas::project::PartialTrackAnalysisResult analyzeMonoAudioPartialTracks(
    std::span<const double> samples,
    double sampleRate,
    const PartialTrackAudioAnalysisSettings& settings,
    talas::project::PartialTrackAnalysisOptions options,
    std::function<bool()> shouldCancel)
{
    if (samples.empty()
        || samples.size()
            > talas::project::maximumPartialTrackSourceFrames)
    {
        return failure(
            talas::project::PartialTrackAnalysisErrorCode::InvalidInput,
            "Offline partial analysis requires one bounded non-empty mono source");
    }
    const auto analysisLow = settings.lowestFrequencyHz > 0.0
        ? settings.lowestFrequencyHz
        : std::max(20.0, sampleRate / 2048.0);
    talas::dsp::LiveSpectralAnalysisConfigurationInput input{
        sampleRate,
        samples.size(),
        analysisLow,
        settings.level,
        1.0,
        analysisLow,
        sampleRate * 0.5,
        analysisLow,
        false};
    input.responseBins = settings.responseBins;
    input.bandTracks = settings.kind != PartialTrackAnalysisKind::StrongestPeaks;
    if (settings.kind == PartialTrackAnalysisKind::QuasiHarmonic)
    {
        // The harmonics of a fundamental followed over the two octaves above
        // the lowest setting, which is the highest one below the fundamental
        //, from the middle of the first.
        input.harmonic = true;
        input.highFrequencyHz = std::min(sampleRate * 0.49, analysisLow * 4.0);
        input.initialFrequencyHz = std::min(input.highFrequencyHz, analysisLow * 1.5);
    }
    const auto configuration =
        talas::dsp::deriveLiveSpectralAnalysisConfiguration(input);
    if (!configuration)
    {
        return failure(
            talas::project::PartialTrackAnalysisErrorCode::InvalidInput,
            "Offline partial analysis configuration is unsupported");
    }
    if (shouldCancel && shouldCancel())
    {
        return failure(
            talas::project::PartialTrackAnalysisErrorCode::Cancelled,
            "Offline partial analysis cancelled");
    }
    try
    {
        talas::dsp::PreparedLiveSpectralAnalysis analyzer(
            configuration.configuration);
        talas::dsp::PreparedPartialSpectrumSequence sequence(
            configuration.configuration.frameCapacity,
            talas::dsp::maximumLiveSpectralPartials);
        const auto status = analyzer.process(
            samples,
            {},
            {},
            {1, 0, 0},
            sequence);
        if (status != talas::dsp::LiveSpectralAnalysisStatus::Ok)
        {
            return failure(
                talas::project::PartialTrackAnalysisErrorCode::AnalysisFailed,
                "Offline prepared partial analysis failed");
        }
        std::vector<talas::project::UntrackedPartialFrame> frames;
        frames.reserve(sequence.frames().size());
        for (const auto& source : sequence.frames())
        {
            talas::project::UntrackedPartialFrame frame{
                source.sourceTimestamp.transportEpoch,
                source.sourceTimestamp.absoluteFrame,
                {}};
            frame.peaks.reserve(source.partials.size());
            for (const auto& point : source.partials)
            {
                // A band with nothing in it this frame is no peak.
                if (input.bandTracks
                    && (!(point.amplitude > 0.0) || !(point.frequencyHz > 0.0)
                    || !(point.frequencyHz <= sampleRate * 0.5)
                    || !std::isfinite(point.bandwidthHz) || point.bandwidthHz < 0.0))
                {
                    continue;
                }
                frame.peaks.push_back({
                    0,
                    point.amplitude,
                    point.frequencyHz,
                    point.bandwidthHz});
            }
            frames.push_back(std::move(frame));
        }
        return talas::project::trackPartialSpectrumFrames(
            sampleRate,
            samples.size(),
            frames,
            options,
            std::move(shouldCancel));
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            talas::project::PartialTrackAnalysisErrorCode::AllocationFailed,
            "Offline partial analysis allocation failed");
    }
}

bool publishPartialTrackAssetNoReplace(
    const std::filesystem::path& destination,
    const talas::project::PartialTrackAsset& asset,
    std::string& error)
{
    // A Sum-of-Sines spectrum file for an .spc, Talas's own for a
    // .talaspartial.
    const auto encoded = talas::project::encodeSpectrumFileFor(destination, asset);
    if (!encoded.ok())
    {
        error = encoded.error.has_value()
            ? encoded.error->message
            : "Unable to encode partial-track asset";
        return false;
    }
    auto publication = AudioFilePublication::prepare(
        destination,
        ExistingAudioFilePolicy::Reject,
        error);
    if (publication == nullptr)
        return false;
    {
        std::ofstream output(
            publication->temporaryPath(),
            std::ios::binary | std::ios::trunc);
        if (!output.good())
        {
            error = "Unable to open private partial-track staging file";
            return false;
        }
        output.write(
            reinterpret_cast<const char*>(encoded.bytes.data()),
            static_cast<std::streamsize>(encoded.bytes.size()));
        output.flush();
        if (!output.good())
        {
            error = "Unable to write private partial-track staging file";
            return false;
        }
    }
    return publication->publish(error);
}
}
