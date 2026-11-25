#include "runtime/PreparedUnaryControlProgram.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <stdexcept>

namespace
{
thread_local bool countAllocations = false;
thread_local std::size_t allocations = 0;
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}
void* operator new(std::size_t size)
{
    if (countAllocations) ++allocations;
    if (auto* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

int main()
{
    namespace family = talas::runtime::prepared_unary_control;
    require(!family::Program::prepare(static_cast<family::Kind>(255), 8), "unknown family member must reject preparation");
    for (auto kind : {family::Kind::AbsoluteValue, family::Kind::ArcTan})
    {
        auto program = family::Program::prepare(kind, 8);
        require(program.has_value(), "known unary member must prepare");
        require(program->contract().inputPort == "value" && program->contract().outputPort == "value",
            "unary family must retain canonical Control value ports");
        const std::array<double, 8> input{-1.0, -0.0, 0.0, 1.0, -std::sqrt(3.0), std::sqrt(3.0),
            -std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
        std::array<double, 8> output{};
        allocations = 0; countAllocations = true;
        const auto result = program->process(input, output);
        countAllocations = false;
        require(result == family::Error::None && allocations == 0, "prepared family process must allocate no objects");
        if (kind == family::Kind::AbsoluteValue)
        {
            const std::array<double, 8> expected{1.0, 0.0, 0.0, 1.0, std::sqrt(3.0), std::sqrt(3.0),
                std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
            require(output == expected && !std::signbit(output[1]), "absolute value must retain exact magnitude and positive zero");
        }
        else
        {
            const auto pi = std::numbers::pi_v<double>;
            const std::array<double, 8> expected{-pi / 4, -0.0, 0.0, pi / 4, -pi / 3, pi / 3, -pi / 2, pi / 2};
            for (std::size_t i = 0; i < output.size(); ++i)
                require(std::abs(output[i] - expected[i]) < 1e-15, "arctangent must match independent known angles");
            require(std::signbit(output[1]), "arctangent must retain negative zero");
        }
        for (double invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                -std::numeric_limits<double>::infinity()})
        {
            auto late = input; late.back() = invalid; output.fill(73.0); const auto before = output;
            allocations = 0; countAllocations = true;
            const auto failed = program->process(late, output);
            countAllocations = false;
            require(failed == family::Error::InputNonFinite && output == before && allocations == 0,
                "late invalid unary input must preserve all output without allocation");
        }
        output.fill(73.0); const auto before = output;
        require(program->process(std::span(input).first(7), output) == family::Error::SizeMismatch && output == before,
            "mismatched unary spans must preserve output");
        std::array<double, 9> tooLarge{};
        require(program->process(tooLarge, tooLarge) == family::Error::SizeMismatch, "prepared capacity is binding");
        require(program->process({}, {}) == family::Error::None, "zero-frame processing is inert");
        require(program->process(input, output) == family::Error::None, "failed blocks must leave stateless exact retry available");
        auto inPlace = input;
        require(program->process(inPlace, inPlace) == family::Error::None && inPlace == output,
            "exact in-place processing must preserve the out-of-place result");
        std::array<double, 9> overlap{1, 2, 3, 4, 5, 6, 7, 8, 9}; const auto overlapBefore = overlap;
        require(program->process(std::span(overlap).first(8), std::span(overlap).last(8)) == family::Error::OverlappingBuffers
            && overlap == overlapBefore, "forward overlap must reject before corrupting unread inputs");
        require(program->process(std::span(overlap).last(8), std::span(overlap).first(8)) == family::Error::OverlappingBuffers
            && overlap == overlapBefore, "reverse overlap must reject before output publication");

        talas::project::ProjectDocument document;
        document.graph.nodes = {{"out", "talas.audioOutput", {}}, {"vca", "talas.vca", {}},
            {"carrier", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}},
            {"source", "talas.constant", {{"value", kind == family::Kind::AbsoluteValue ? "-0.5" : "1"}}},
            // This family's ArcTan is the arctangent of one input in
            // radians, as in projects before format 48.
            {"math", std::string{program->contract().typeId},
                kind == family::Kind::ArcTan
                    ? std::vector<talas::project::ProjectParameterExpression>{
                        {"angle", std::nullopt, std::string{"legacyRadians"}}}
                    : std::vector<talas::project::ProjectParameterExpression>{}}};
        document.graph.connections = {{"source", "value", "math", "value"},
            {"carrier", "audioOut", "vca", "audioIn"}, {"math", "value", "vca", "gain"},
            {"vca", "audioOut", "out", "audioIn"}};
        auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), {8.0, 4});
        require(compiled.ok(), "unary family graph must retain existing compiler admission");
        const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
        require(offline.ok() && offline.mono.size() == 8, "unary family offline graph must render");
        talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
        require(realtime.ready(), "unary family realtime graph must prepare");
        std::array<double, 8> actual{}; std::size_t offset = 0;
        for (const std::size_t frames : {1, 3, 2, 2})
        {
            allocations = 0; countAllocations = true;
            const auto rendered = realtime.render(std::span(actual).subspan(offset, frames));
            countAllocations = false;
            require(rendered && allocations == 0, "irregular prepared unary callbacks must render without allocation");
            offset += frames;
        }
        const auto gain = kind == family::Kind::AbsoluteValue ? 0.5 : std::numbers::pi_v<double> / 4;
        for (std::size_t frame = 0; frame < actual.size(); ++frame)
        {
            const auto expected = gain * std::sin(2 * std::numbers::pi_v<double> * static_cast<double>(frame) / 8);
            require(std::abs(offline.mono[frame] - expected) < 1e-12 && std::abs(actual[frame] - expected) < 1e-12,
                "both unary execution paths must match independent carrier and gain with irregular continuation");
        }
        talas::runtime::RealtimeRenderSession reconstructed(talas::runtime::PreparedRuntimeGraph{
            compiled.graph->plan(), compiled.graph->renderSettings()});
        require(!reconstructed.ready(), "family preparation must not replace compiler-owned execution authority");

    }
    return 0;
}
