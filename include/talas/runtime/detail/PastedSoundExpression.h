#pragma once

#include "talas/expr/Expression.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::runtime::detail::pasted_sound
{
inline constexpr std::size_t maximumReferences =
    talas::expr::ExpressionLimits::maximumIdentifiers;
// A field's parts without a Sound that read Event Values or variables or call
// a function, evaluated live like any field: __kc_live_1 to __kc_live_8.
inline constexpr std::size_t maximumLiveParts = 8;

// The live part a skeleton identifier names, counted from 0.
[[nodiscard]] inline std::optional<std::size_t> livePartIndex(std::string_view name) noexcept
{
    constexpr std::string_view prefix = "__kc_live_";
    if (!name.starts_with(prefix) || name.size() != prefix.size() + 1)
        return std::nullopt;
    const auto digit = name.back();
    if (digit < '1' || digit > '8')
        return std::nullopt;
    return static_cast<std::size_t>(digit - '1');
}

// One pasted Sound in a field: the signal bindings it reads (two for M) and
// the frames between its updates.
struct Reference
{
    std::string name;
    std::array<std::size_t, 2> bindings{};
    std::size_t bindingCount = 0;
    std::uint64_t periodFrames = 1;
};

// A field with pasted Sounds, prepared from a Pasted Sound Expression's
// parameters (ADR-0020).
struct Program
{
    talas::expr::Expression expression;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    std::vector<Reference> references;
    std::size_t bindingCount = 0;
    std::uint64_t tickFrames = 1;
    // A field that applies a function keeping state or following time to
    // its Sounds, as `Threshold2 L nextRandom`,
    // keeps that state from one evaluation to the next.
    bool stateful = false;
    double sampleRate = 48000.0;
    // A project's choice (format 156): a Sound's output ranges over -1 to 1,
    // so each pasted Sound reads saturated at -1 and 1, as fixed-point signals
    // are.
    bool readsWithinUnity = false;
    // A project's choice (format 156): each pasted Sound is read as it stood
    // just before its tick, the last frame of the millisecond before, so that
    // a field lags the Sound it reads by up to a millisecond and a Sound
    // pasted into a Sound pasted into a field by a millisecond more.
    bool readsThePreviousMillisecond = false;
};

struct State
{
    std::uint64_t renderedFrames = 0;
    std::array<double, maximumReferences> held{};
    // Each reference's reading on the frame before, 0 before the first.
    std::array<double, maximumReferences> previous{};
    double value = 0.0;
    talas::expr::ExpressionState expression{};
    bool expressionPrepared = false;
};

[[nodiscard]] inline std::optional<talas::expr::ValueDomain> domainFromChoice(
    std::string_view choice) noexcept
{
    if (choice == "scalar")
        return talas::expr::ValueDomain::Scalar;
    if (choice == "hertz")
        return talas::expr::ValueDomain::Hertz;
    if (choice == "seconds")
        return talas::expr::ValueDomain::Seconds;
    if (choice == "noteNumber")
        return talas::expr::ValueDomain::NoteNumber;
    if (choice == "samples")
        return talas::expr::ValueDomain::Samples;
    return std::nullopt;
}

// The project's choices of how its fields read their pasted Sounds, as the
// Pasted Sound Expression's range and reads choices name them.
[[nodiscard]] inline std::optional<bool> readsWithinUnityFromChoice(std::string_view choice) noexcept
{
    if (choice == "unbounded")
        return false;
    if (choice == "unity")
        return true;
    return std::nullopt;
}

[[nodiscard]] inline std::optional<bool> readsThePreviousMillisecondFromChoice(std::string_view choice) noexcept
{
    if (choice == "atTheTick")
        return false;
    if (choice == "previousMillisecond")
        return true;
    return std::nullopt;
}

// A pasted Sound is updated once per millisecond, or every n ms with `L: n`.
[[nodiscard]] inline std::uint64_t periodFrames(
    std::optional<double> milliseconds,
    double sampleRate) noexcept
{
    const auto frames = std::round(milliseconds.value_or(1.0) * sampleRate / 1000.0);
    if (!std::isfinite(frames) || frames < 1.0)
        return 1;
    return frames >= 1.0e15 ? std::uint64_t{1'000'000'000'000'000} : static_cast<std::uint64_t>(frames);
}

// Parses the expression and its reference table ("name\tbinding[\tbinding]"
// per line); the expression's every identifier must be a listed Sound.
[[nodiscard]] inline std::optional<Program> prepare(
    std::string_view expression,
    std::string_view references,
    std::string_view domain,
    double sampleRate,
    std::size_t bindingCount,
    std::string_view range = "unbounded",
    std::string_view reads = "atTheTick")
{
    auto parsed = talas::expr::parse(expression, talas::expr::ExpressionLanguageVersion::ValV1);
    const auto valueDomain = domainFromChoice(domain);
    const auto withinUnity = readsWithinUnityFromChoice(range);
    const auto previousMillisecond = readsThePreviousMillisecondFromChoice(reads);
    if (!parsed.ok() || !parsed.expression.has_value() || !valueDomain.has_value()
        || !withinUnity.has_value() || !previousMillisecond.has_value()
        || !std::isfinite(sampleRate) || sampleRate <= 0.0
        || !parsed.expression->eventReferences().empty()
        || !parsed.expression->variableReferences().empty())
    {
        return std::nullopt;
    }
    const auto stateful = parsed.expression->usesTime() || parsed.expression->isStateful();
    Program program{std::move(*parsed.expression), *valueDomain, {}, bindingCount, 0, stateful, sampleRate,
        *withinUnity, *previousMillisecond};
    std::size_t next = 0;
    while (next < references.size())
    {
        const auto end = std::min(references.find('\n', next), references.size());
        const auto line = references.substr(next, end - next);
        next = end + 1;
        if (line.empty())
            continue;
        const auto tab = line.find('\t');
        if (tab == std::string_view::npos)
            return std::nullopt;
        Reference reference;
        reference.name = std::string(line.substr(0, tab));
        const auto sound = talas::expr::parseSoundReference(reference.name);
        if (!sound.has_value())
            return std::nullopt;
        auto rest = line.substr(tab + 1);
        while (!rest.empty())
        {
            const auto separator = std::min(rest.find('\t'), rest.size());
            std::size_t binding = 0;
            const auto digits = rest.substr(0, separator);
            for (const auto digit : digits)
            {
                if (digit < '0' || digit > '9')
                    return std::nullopt;
                binding = binding * 10 + static_cast<std::size_t>(digit - '0');
            }
            if (digits.empty() || binding >= bindingCount || reference.bindingCount == 2)
                return std::nullopt;
            reference.bindings[reference.bindingCount++] = binding;
            rest = separator < rest.size() ? rest.substr(separator + 1) : std::string_view{};
        }
        if (reference.bindingCount != (sound->channel == 'M' ? 2U : 1U))
            return std::nullopt;
        reference.periodFrames = periodFrames(sound->periodMilliseconds, sampleRate);
        program.references.push_back(std::move(reference));
    }
    if (program.references.empty() || program.references.size() > maximumReferences)
        return std::nullopt;
    std::size_t soundIdentifiers = 0;
    for (const auto& name : program.expression.identifiers())
    {
        if (livePartIndex(name).has_value())
            continue;
        if (std::ranges::find(program.references, name, &Reference::name) == program.references.end())
            return std::nullopt;
        ++soundIdentifiers;
    }
    if (soundIdentifiers != program.references.size())
        return std::nullopt;
    program.tickFrames = std::ranges::min_element(program.references, {}, &Reference::periodFrames)->periodFrames;
    return program;
}

// Advances the field over a block: each reference samples its signal on its
// own ticks and holds it, and the expression is evaluated on the shortest
// period in the parameter's domain. Fails without changing the state.
[[nodiscard]] inline bool advance(
    const Program& program,
    State& state,
    std::span<const std::span<const double>> signals,
    std::span<const talas::expr::ExpressionValue> liveParts,
    std::span<double> output) noexcept
{
    if (signals.size() != program.bindingCount)
        return false;
    for (const auto& signal : signals)
        if (signal.size() < output.size())
            return false;
    auto candidate = state;
    struct Resolver
    {
        const Program* program = nullptr;
        const State* state = nullptr;
        std::span<const talas::expr::ExpressionValue> liveParts;
    } resolver{&program, &candidate, liveParts};
    talas::expr::EvaluationContext context;
    context.sampleRate = program.sampleRate;
    context.user = &resolver;
    context.resolveIdentifier = [](void* user, std::string_view name, talas::expr::ExpressionValue& value) noexcept {
        const auto* resolver = static_cast<const Resolver*>(user);
        if (const auto live = livePartIndex(name))
        {
            if (*live >= resolver->liveParts.size())
                return false;
            value = resolver->liveParts[*live];
            return std::isfinite(value.value);
        }
        for (std::size_t index = 0; index < resolver->program->references.size(); ++index)
        {
            if (resolver->program->references[index].name == name)
            {
                value = {resolver->state->held[index], talas::expr::ValueDomain::Scalar};
                return true;
            }
        }
        return false;
    };
    if (program.stateful && !candidate.expressionPrepared)
    {
        if (!talas::expr::prepareExpressionState(program.expression, candidate.expression))
            return false;
        candidate.expressionPrepared = true;
    }
    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        const auto absolute = candidate.renderedFrames;
        if (absolute == std::numeric_limits<std::uint64_t>::max())
            return false;
        bool sampled = false;
        for (std::size_t index = 0; index < program.references.size(); ++index)
        {
            const auto& reference = program.references[index];
            const auto tick = absolute % reference.periodFrames == 0;
            if (!tick && !program.readsThePreviousMillisecond)
                continue;
            double sum = 0.0;
            for (std::size_t binding = 0; binding < reference.bindingCount; ++binding)
                sum += signals[reference.bindings[binding]][frame];
            auto value = sum / static_cast<double>(reference.bindingCount);
            if (program.readsWithinUnity && !std::isnan(value))
                value = std::clamp(value, -1.0, 1.0);
            // Read on its tick as it stood the frame before, or as
            // it stands on the tick.
            const auto read = program.readsThePreviousMillisecond ? candidate.previous[index] : value;
            candidate.previous[index] = value;
            if (!tick)
                continue;
            if (!std::isfinite(read))
                return false;
            candidate.held[index] = read;
            sampled = true;
        }
        // A field its Sounds leave undefined for a moment, such as the pitch
        // of a pasted SpectrumFundamental's 0 hz before its first frame
        //, holds its last value, 0 at the
        // start; the parameter saturates it at its range.
        if (sampled && absolute % program.tickFrames == 0)
        {
            talas::expr::ExpressionValue evaluated;
            context.elapsedSampleFrame = absolute;
            const auto ok = program.stateful
                ? talas::expr::tryEvaluateStateful(
                      program.expression, context, candidate.expression, {program.domain}, evaluated)
                : talas::expr::tryEvaluate(program.expression, context, {program.domain}, evaluated);
            if (ok && std::isfinite(evaluated.value))
                candidate.value = evaluated.value;
        }
        output[frame] = candidate.value;
        ++candidate.renderedFrames;
    }
    state = candidate;
    return true;
}
}
