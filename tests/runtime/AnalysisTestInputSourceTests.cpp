#include "talas/runtime/AnalysisTestInputSource.h"

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::runtime::AnalysisTestInputSource prepare(
    talas::project::WorkspaceAnalysisTestSourceKind kind,
    std::vector<float> samples = {},
    bool loop = false)
{
    talas::runtime::AnalysisTestInputSource source;
    std::string diagnostic;
    const auto ok = talas::runtime::AnalysisTestInputSource::prepare(
        {
            .kind = kind,
            .sampleRate = 48'000.0,
            .sineFrequencyHz = 12'000.0,
            .frozenMonoSamples = std::move(samples),
            .loopFrozenAsset = loop,
        },
        source,
        diagnostic);
    require(ok, "source should prepare: " + diagnostic);
    return source;
}

void testClosedSourcesRenderExactPreparedSamples()
{
    std::array<float, 6> block{};
    auto silence = prepare(
        talas::project::WorkspaceAnalysisTestSourceKind::Silence);
    block.fill(1.0F);
    silence.render(block);
    require(block == std::array<float, 6>{},
        "Silence should clear every requested frame");

    auto impulse = prepare(
        talas::project::WorkspaceAnalysisTestSourceKind::Impulse);
    impulse.render(block);
    require(block[0] == 1.0F
            && block[1] == 0.0F
            && impulse.exhausted(),
        "Impulse should emit exactly one leading unit sample");
    block.fill(1.0F);
    impulse.render(block);
    require(block == std::array<float, 6>{},
        "Impulse tail should stay silent");

    auto sine = prepare(
        talas::project::WorkspaceAnalysisTestSourceKind::Sine);
    sine.render(block);
    require(std::abs(block[0]) < 1.0e-6F
            && std::abs(block[1] - 1.0F) < 1.0e-6F
            && std::abs(block[2]) < 1.0e-6F
            && std::abs(block[3] + 1.0F) < 1.0e-6F,
        "quarter-rate Sine should preserve exact phase progression");
}

void testFrozenAssetStopsOrLoopsWithoutCallbackAllocation()
{
    std::array<float, 6> block{};
    auto oneShot = prepare(
        talas::project::WorkspaceAnalysisTestSourceKind::FrozenAsset,
        {0.25F, -0.5F, 0.75F});
    oneShot.render(block);
    require(block == std::array<float, 6>{
                         0.25F, -0.5F, 0.75F, 0.0F, 0.0F, 0.0F}
            && oneShot.exhausted(),
        "one-shot frozen asset should zero-fill its bounded tail");

    auto loop = prepare(
        talas::project::WorkspaceAnalysisTestSourceKind::FrozenAsset,
        {0.25F, -0.5F},
        true);
    loop.render(block);
    require(block == std::array<float, 6>{
                         0.25F, -0.5F, 0.25F, -0.5F, 0.25F, -0.5F}
            && !loop.exhausted(),
        "looping frozen asset should wrap at the exact frozen boundary");
}

void testMalformedPreparationFailsWithoutReplacingDestination()
{
    auto retained = prepare(
        talas::project::WorkspaceAnalysisTestSourceKind::Silence);
    std::string diagnostic;
    require(!talas::runtime::AnalysisTestInputSource::prepare(
                {
                    .kind = static_cast<
                        talas::project::WorkspaceAnalysisTestSourceKind>(255),
                    .sampleRate = 48'000.0,
                },
                retained,
                diagnostic)
            && retained.kind()
                == talas::project::WorkspaceAnalysisTestSourceKind::Silence,
        "unknown source kind should fail without replacing the destination");
    require(!talas::runtime::AnalysisTestInputSource::prepare(
                {
                    .kind = talas::project::WorkspaceAnalysisTestSourceKind::Sine,
                    .sampleRate = 48'000.0,
                    .sineFrequencyHz = 24'000.0,
                },
                retained,
                diagnostic)
            && retained.kind()
                == talas::project::WorkspaceAnalysisTestSourceKind::Silence,
        "invalid Nyquist request should preserve the prior prepared source");

    require(!talas::runtime::AnalysisTestInputSource::prepare(
                {
                    .kind = talas::project::WorkspaceAnalysisTestSourceKind::FrozenAsset,
                    .sampleRate = 48'000.0,
                    .sineFrequencyHz = 440.0,
                    .frozenMonoSamples = {0.0F, std::nanf("")},
                },
                retained,
                diagnostic),
        "non-finite frozen asset should fail closed");
}
}

int main()
{
    testClosedSourcesRenderExactPreparedSamples();
    testFrozenAssetStopsOrLoopsWithoutCallbackAllocation();
    testMalformedPreparationFailsWithoutReplacingDestination();
    return 0;
}
