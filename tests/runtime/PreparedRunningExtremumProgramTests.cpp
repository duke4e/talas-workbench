#include "runtime/PreparedRunningExtremumProgram.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"

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

namespace
{
void testGraph(bool minimum)
{
    namespace runtime = talas::runtime;
    const double sign = minimum ? -1.0 : 1.0;
    talas::project::ProjectDocument document;
    document.graph.nodes = {{"out", "talas.audioOutput", {}}, {"vca", "talas.vca", {}},
        {"carrier", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}},
        {"source", "talas.constant", {{"value", minimum ? "-0.25" : "0.25"}}},
        {"running", minimum ? "talas.runningMin" : "talas.runningMax", {}},
        {"divide", "talas.product", {}}, {"divisor", "talas.constant", {{"value", "1"}}}};
    document.graph.connections = {{"source", "value", "running", "value"},
        {"running", "value", "divide", "left"}, {"divisor", "value", "divide", "right"},
        {"divide", "value", "vca", "gain"}, {"carrier", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "out", "audioIn"}};
    auto settings = runtime::RenderSettings{16.0, 4};
    auto compiled = runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "literal extremum graph must retain compiler admission");
    runtime::OfflineRenderSession offline(*compiled.graph);
    runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(realtime.ready(), "extremum realtime graph must prepare");
    std::size_t offset = 0;
    for (std::size_t frames : {1, 3, 2, 2})
    {
        const auto expectedBlock = offline.render(frames);
        require(expectedBlock.ok() && expectedBlock.mono.size() == frames, "ordinary offline continuation must render");
        std::array<double, 4> actual{};
        allocations = 0; countAllocations = true;
        const auto rendered = realtime.render(std::span(actual).first(frames));
        countAllocations = false;
        require(rendered && allocations == 0, "prepared callbacks must retain allocation-free processing");
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            const auto expected = sign * 0.25 * std::sin(2 * std::numbers::pi_v<double> * (offset + frame) / 16);
            require(std::abs(expectedBlock.mono[frame] - expected) < 1e-12 && std::abs(actual[frame] - expected) < 1e-12,
                "both adapters must match independent carrier and extremum across irregular blocks");
        }
        offset += frames;
    }
    offline.reset(); realtime.resetTransport();
    const auto restarted = offline.render(4); std::array<double, 4> restartedRealtime{};
    require(restarted.ok() && realtime.render(restartedRealtime), "both adapter resets must restore rendering");
    for (std::size_t frame = 0; frame < 4; ++frame)
        require(std::abs(restarted.mono[frame] - sign * 0.25 * std::sin(2 * std::numbers::pi_v<double> * frame / 16)) < 1e-12
            && restarted.mono[frame] == restartedRealtime[frame], "both resets must restore the exact fresh prefix");
    runtime::RealtimeRenderSession reconstructed(runtime::PreparedRuntimeGraph{
        compiled.graph->plan(), compiled.graph->renderSettings()});
    require(!reconstructed.ready(), "family configuration cannot replace compiler-owned authority");

    document.graph.nodes[3].parameters = {{"value", "!ExtremumValue"}};
    document.graph.nodes[6].parameters = {{"value", "1 / !Divisor"}};
    settings.eventValues = {{"ExtremumValue", sign * 0.25, talas::expr::ValueDomain::Scalar},
        {"Divisor", 1.0, talas::expr::ValueDomain::Scalar}};
    compiled = runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "combined-input extremum graph must compile");
    runtime::OfflineRenderSession transaction(*compiled.graph);
    std::array<runtime::OfflineMidiVoiceEvent, 3> events{};
    for (std::size_t index = 0; index < events.size(); ++index)
    {
        events[index].kind = runtime::OfflineMidiVoiceEventKind::EventValueAssignment;
        events[index].frame = index == 0 ? 4 : index == 1 ? 8 : 10;
        events[index].eventValueName = index == 2 ? "Divisor" : "ExtremumValue";
        events[index].value = index == 0 ? sign * 0.5 : index == 1 ? sign * 0.75 : 0.0;
        events[index].batchId = index + 1;
        events[index].sequence = index + 1;
    }
    require(transaction.scheduleMidiVoiceEvents(events), "combined-input controls must schedule");
    const auto prefix = transaction.render(4);
    require(prefix.ok() && prefix.mono.size() == 4, "transaction prefix must commit");
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const auto failed = transaction.render(8);
        require(!failed.ok() && failed.mono.empty(), "late division failure must discard the entire public call");
    }
    const auto retry = transaction.render(4);
    require(retry.ok() && retry.mono.size() == 4, "shorter retry must remain available after repeated whole-call failure");
    for (std::size_t frame = 0; frame < 4; ++frame)
        require(std::abs(retry.mono[frame] - sign * 0.5 * std::sin(2 * std::numbers::pi_v<double> * (frame + 4) / 16)) < 1e-12,
            "whole-call rollback must discard the later extremum, rewind controls and retain the committed prefix");
    const auto tail = transaction.render(2);
    require(tail.ok() && tail.mono.size() == 2, "retry must retain the next scheduled extremum change");
    for (std::size_t frame = 0; frame < 2; ++frame)
        require(std::abs(tail.mono[frame] - sign * 0.75 * std::sin(2 * std::numbers::pi_v<double> * (frame + 8) / 16)) < 1e-12,
            "retried transaction must reach the independent next extremum");
    require(!transaction.render(1).ok(), "the scheduled failing boundary must also be retained");
}
}

int main()
{
    namespace family = talas::runtime::prepared_running_extremum;
    require(!family::Program::prepare(static_cast<family::Kind>(255), 8), "unknown kind must not prepare");
    for (auto kind : {family::Kind::Minimum, family::Kind::Maximum})
    {
        const bool minimum = kind == family::Kind::Minimum;
        const auto program = *family::Program::prepare(kind, 8);
        const auto ports = program.contract().inputs();
        require(ports.size() == (minimum ? 3 : 2) && ports[0] == "value" && ports[1] == "reset"
            && (!minimum || ports[2] == "maximumAmplitude"), "exact member-specific inputs must be enumerated");
        family::State state;
        const std::array<double, 8> input{-4, -2, -7, -3, 5, 2, -8, -1};
        const std::array<double, 8> reset{0, 0, -1, 0, 0, 1, 0, 0};
        const std::array<double, 8> amplitude{9, 9, -9, 9, 9, 3, 9, 9};
        const auto nan = std::numeric_limits<double>::quiet_NaN();
        std::array<double, 8> output{};
        allocations = 0; countAllocations = true;
        const auto result = program.process(state, input, {reset, nan}, {amplitude, nan}, output);
        countAllocations = false;
        const std::array<double, 8> expected = minimum
            ? std::array<double, 8>{-4, -4, -9, -9, -9, 2, -8, -8}
            : std::array<double, 8>{-4, -2, 0, 0, 5, 2, 2, 2};
        require(result == family::Error::None && allocations == 0 && output == expected,
            "selected controls, asymmetric seeds and cumulative extrema must match independent expectations");
        const auto checkpoint = program.checkpoint(state);
        for (double invalid : {nan, std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()})
        {
            for (int port = 0; port < (minimum ? 3 : 2); ++port)
            {
                auto values = input; auto resets = reset; auto amplitudes = amplitude;
                (port == 0 ? values : port == 1 ? resets : amplitudes).back() = invalid;
                output.fill(73); const auto before = output;
                allocations = 0; countAllocations = true;
                const auto failed = program.process(state, values, {resets, 0}, {amplitudes, 1}, output);
                countAllocations = false;
                const auto error = port == 0 ? family::Error::ValueNonFinite
                    : port == 1 ? family::Error::ResetNonFinite : family::Error::MaximumAmplitudeNonFinite;
                require(failed == error && output == before && state == checkpoint && allocations == 0,
                    "late nonfinite input must preserve all output and committed state without allocation");
            }
        }
        output.fill(73); const auto before = output;
        require(program.process(state, input, {{}, 0}, {{}, nan}, output)
                == (minimum ? family::Error::MaximumAmplitudeNonFinite : family::Error::None),
            "Min validates selected maximumAmplitude at inactive reset; Max ignores it");
        if (minimum) require(output == before && state == checkpoint, "invalid inactive-reset amplitude must be atomic");
        program.restore(state, checkpoint);
        require(program.process(state, std::span(input).first(7), {reset, 0}, {amplitude, 1}, output)
                == family::Error::SizeMismatch && state == checkpoint, "mismatched block sizes must preserve state");
        require(program.process(state, input, {std::span(reset).first(7), 0}, {amplitude, 1}, output)
                == family::Error::SizeMismatch, "connected control size is binding");
        std::array<double, 9> large{};
        require(program.process(state, large, {{}, 0}, {{}, 1}, large) == family::Error::SizeMismatch,
            "prepared capacity is binding");
        require(program.process(state, {}, {{}, nan}, {{}, nan}, {}) == family::Error::None && state == checkpoint,
            "empty processing must preserve state and skip unselected frames");
        const auto empty = *family::Program::prepare(kind, 0);
        require(empty.process(state, {}, {}, {}, {}) == family::Error::None, "zero-capacity preparation is valid");
        program.reset(state);
        require(!state.initialized && state.extremum == 0 && !std::signbit(state.extremum), "reset restores fresh state");
        for (std::size_t offset = 0; offset < 8; offset += 2)
            require(program.process(state, std::span(input).subspan(offset, 2),
                {std::span(reset).subspan(offset, 2), 0}, {std::span(amplitude).subspan(offset, 2), 1},
                std::span(output).subspan(offset, 2)) == family::Error::None, "partitioned processing must succeed");
        require(output == expected, "partitioned state continuation must retain exact output");
        program.restore(state, checkpoint);
        const std::array<double, 2> next{4, -6}; std::array<double, 2> first{}, retry{};
        require(program.process(state, next, {{}, 0}, {{}, 1}, first) == family::Error::None, "continuation must render");
        program.restore(state, checkpoint);
        require(program.process(state, next, {{}, 0}, {{}, 1}, retry) == family::Error::None && retry == first,
            "checkpoint restore must reproduce exact continuation");
        for (int alias = 0; alias < (minimum ? 3 : 2); ++alias)
        {
            program.reset(state);
            auto values = input; auto resets = reset; auto amplitudes = amplitude;
            auto& destination = alias == 0 ? values : alias == 1 ? resets : amplitudes;
            require(program.process(state, values, {resets, 0}, {amplitudes, 1}, destination) == family::Error::None
                && destination == expected, "exact in-place input or controls must preserve results");
        }
        for (int port = 0; port < (minimum ? 3 : 2); ++port)
        {
            for (bool reverse : {false, true})
            {
                std::array<double, 9> overlap{1, 2, 3, 4, 5, 6, 7, 8, 9}; const auto saved = overlap;
                auto source = reverse ? std::span(overlap).last(8) : std::span(overlap).first(8);
                auto destination = reverse ? std::span(overlap).first(8) : std::span(overlap).last(8);
                const auto savedState = state;
                require(program.process(state, port == 0 ? source : std::span<const double>(input),
                    {port == 1 ? source : std::span<const double>(reset), 0},
                    {port == 2 ? source : std::span<const double>(amplitude), 1}, destination)
                        == family::Error::OverlappingBuffers && overlap == saved && state == savedState,
                    "partial overlap must reject before state or output mutation");
            }
        }
        for (double zero : {-0.0, 0.0})
        {
            program.reset(state); const std::array<double, 2> zeros{zero, -zero}; std::array<double, 2> actual{};
            require(program.process(state, zeros, {{}, -0.0}, {{}, 1}, actual) == family::Error::None
                && std::signbit(actual[0]) == std::signbit(zero) && std::signbit(actual[1]) == std::signbit(zero),
                "equal signed-zero ties retain stored sign and negative-zero reset is inactive");
            require(program.process(state, zeros, {{}, -1}, {{}, -0.0}, actual) == family::Error::None
                && std::signbit(actual[0]) == minimum && std::signbit(actual[1]) == minimum,
                "reset seed controls the sign of equal zero extrema");
        }
    }
    testGraph(true);
    testGraph(false);
    return 0;
}
