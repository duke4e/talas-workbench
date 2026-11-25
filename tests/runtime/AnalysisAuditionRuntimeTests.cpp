#include "talas/app/AnalysisAuditionModel.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/AnalysisTestInputSource.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectParameterExpression expression(
    std::string id,
    std::string value)
{
    return {
        .parameterId = std::move(id),
        .expression = std::move(value),
    };
}

talas::project::ProjectDocument document()
{
    talas::project::ProjectDocument result;
    result.graph.nodes = {
        {"osc", "talas.oscillator",
            {expression("frequency", "220 hz"),
             expression("amplitude", "0.2")}},
        {"filter", "talas.filter", {}},
        {"out", "talas.audioOutput", {}},
    };
    result.graph.connections = {
        {"osc", "audioOut", "filter", "audioIn"},
        {"filter", "audioOut", "out", "audioIn"},
    };
    return result;
}

talas::runtime::RenderSettings settings()
{
    return {
        .sampleRate = 48'000.0,
        .maxBlockFrames = 64,
    };
}

void testTemporaryRuntimeLeavesMainGraphRetainedAndResumable()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto source = document();
    const auto sourceBytes = talas::project::serializeProjectDocument(source);
    talas::runtime::ProjectRuntimeBridge mainBridge(source, registry);
    require(mainBridge.compileAndStage(settings()).ok(),
        "main Analysis fixture should stage");
    mainBridge.setPlaying(true);

    std::array<float, 64> mainOutput{};
    std::array<float*, 1> mainChannels{mainOutput.data()};
    require(mainBridge.render(
                mainChannels.data(), mainChannels.size(), mainOutput.size()),
        "main Analysis fixture should adopt and render");
    const auto retainedMainPosition = mainBridge.transportFramePosition();

    talas::project::WorkspaceAnalysisTraceState trace;
    trace.traceId = "filter-audio";
    trace.nodeId = "filter";
    trace.portId = "audioOut";
    trace.kind = talas::project::WorkspaceAnalysisTraceKind::Audio;
    talas::project::WorkspaceAnalysisAuditionState request;
    request.traceId = trace.traceId;
    request.destinationInputPortId = "audioIn";
    request.source = talas::project::WorkspaceAnalysisTestSourceKind::Sine;
    request.sineFrequencyHz = 1'000.0;
    const auto prepared = talas::app::prepareAnalysisAudition(
        source, registry, trace, request);
    require(prepared.ok(), "temporary Analysis overlay should prepare");

    talas::runtime::AnalysisTestInputSource sourceRuntime;
    std::string diagnostic;
    require(talas::runtime::AnalysisTestInputSource::prepare(
                {
                    .kind = request.source,
                    .sampleRate = settings().sampleRate,
                    .sineFrequencyHz = request.sineFrequencyHz,
                },
                sourceRuntime,
                diagnostic),
        "temporary sine should prepare: " + diagnostic);

    std::unique_ptr<talas::runtime::ProjectRuntimeBridge> auditionBridge;
    std::string workerFailure;
    std::jthread preparationWorker([&] {
        try
        {
            auto candidate =
                std::make_unique<talas::runtime::ProjectRuntimeBridge>(
                    *prepared.overlayDocument, registry);
            const auto staged = candidate->compileAndStageDocument(
                *prepared.overlayDocument, settings());
            if (!staged.ok())
            {
                workerFailure = staged.message.empty()
                    ? "temporary Analysis overlay did not stage"
                    : staged.message;
                return;
            }
            candidate->setPlaying(true);
            auditionBridge = std::move(candidate);
        }
        catch (const std::exception& error)
        {
            workerFailure = error.what();
        }
    });
    preparationWorker.join();
    require(auditionBridge != nullptr,
        "temporary Analysis overlay should stage on a default worker stack: "
            + workerFailure);

    std::array<float, 64> input{};
    std::array<float, 64> auditionOutput{};
    std::array<const float*, 1> inputChannels{input.data()};
    std::array<float*, 1> auditionChannels{auditionOutput.data()};
    sourceRuntime.render(input);
    require(auditionBridge->renderOrdered(
                {inputChannels.data(), inputChannels.size()},
                0.0,
                auditionChannels.data(),
                auditionChannels.size(),
                auditionOutput.size()),
        "temporary Analysis graph should render its prepared source");
    require(std::ranges::any_of(auditionOutput, [](float sample) {
                return std::isfinite(sample) && std::abs(sample) > 1.0e-6F;
            }),
        "temporary Analysis graph should produce finite audible output");
    require(mainBridge.transportFramePosition() == retainedMainPosition,
        "audition render must not advance the retained main graph");
    require(talas::project::serializeProjectDocument(mainBridge.editor().document())
            == sourceBytes,
        "audition render must not mutate the editable project");

    mainOutput.fill(0.0F);
    require(mainBridge.render(
                mainChannels.data(), mainChannels.size(), mainOutput.size()),
        "main graph should resume without recompilation after audition");
    require(mainBridge.transportFramePosition()
            == retainedMainPosition + mainOutput.size(),
        "main graph should resume from its exact retained cursor");
}
}

int main()
{
    testTemporaryRuntimeLeavesMainGraphRetainedAndResumable();
    return 0;
}
