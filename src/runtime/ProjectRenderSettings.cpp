#include "talas/runtime/ProjectRenderSettings.h"

#include "talas/project/ProjectEventValueDiscovery.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <ranges>
#include <string_view>

namespace talas::runtime
{
namespace
{
// A Virtual Control starts its Event Value from the value the project saved,
// unless the caller binds that Event Value itself: a value given for this
// render (the CLI's --event, a plugin's parameter) is the one the Sound
// starts from, as it is for an Event Value without a control. The caller's
// binding must be in the control's domain.
[[nodiscard]] std::string applyPersistedVirtualControls(
    RenderSettings& settings,
    const talas::project::ProjectDocument& document)
{
    settings.eventValues.reserve(settings.eventValues.size() + document.virtualControls.size());
    for (const auto& control : document.virtualControls)
    {
        const auto given = std::ranges::find_if(
            settings.eventValues,
            [&](const EventValueBinding& binding) {
                return binding.name == control.eventName;
            });
        if (given == settings.eventValues.end())
        {
            settings.eventValues.push_back({control.eventName, control.value, control.domain});
        }
        else if (given->domain != control.domain)
        {
            return "Event Value binding has the wrong domain for its Virtual Control ("
                + control.eventName + ")";
        }
    }
    return {};
}

[[nodiscard]] std::string applyDiscoveredEventValues(
    RenderSettings& settings,
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    const auto discovery =
        talas::project::discoverProjectEventValueControls(document, registry);
    if (!discovery.ok())
    {
        const auto& diagnostic = discovery.diagnostics.front();
        auto message = "Event Value discovery failed: " + diagnostic.message;
        if (!diagnostic.eventName.empty())
            message += " (" + diagnostic.eventName + ")";
        return message;
    }

    for (const auto& control : discovery.missingControls)
    {
        const auto existing = std::ranges::find_if(
            settings.eventValues,
            [&](const EventValueBinding& binding) {
                return binding.name == control.eventName;
            });
        if (existing == settings.eventValues.end())
        {
            settings.eventValues.push_back({
                control.eventName,
                control.value,
                control.domain,
            });
        }
        else if (existing->domain != control.domain)
        {
            return "Event Value binding has the wrong discovered domain ("
                + control.eventName + ")";
        }
    }
    return {};
}

bool applyPersistedTimeline(RenderSettings& settings, const talas::project::ProjectDocument& document)
{
    settings.transportLoop.reset();
    const auto hasTimelineWork = document.timeline.loop.enabled
        || !document.timeline.eventValues.empty();
    if (!hasTimelineWork)
        return true;
    if (!std::isfinite(settings.sampleRate)
        || settings.sampleRate <= 0.0)
        return false;

    if (document.timeline.loop.enabled)
    {
        const auto& loop = document.timeline.loop;
        const auto exactStart = static_cast<long double>(loop.startSeconds)
            * static_cast<long double>(settings.sampleRate);
        const auto exactEnd = static_cast<long double>(loop.endSeconds)
            * static_cast<long double>(settings.sampleRate);
        const auto roundedStart = std::floor(exactStart + 0.5L);
        const auto roundedEnd = std::floor(exactEnd + 0.5L);
        if (!std::isfinite(loop.startSeconds)
            || !std::isfinite(loop.endSeconds)
            || loop.startSeconds < 0.0
            || loop.endSeconds <= loop.startSeconds
            || roundedStart
                > static_cast<long double>(
                    std::numeric_limits<std::uint64_t>::max())
            || roundedEnd
                > static_cast<long double>(
                    std::numeric_limits<std::uint64_t>::max())
            || roundedEnd <= roundedStart)
        {
            return false;
        }
        settings.transportLoop = TransportLoopRegion{
            static_cast<std::uint64_t>(roundedStart),
            static_cast<std::uint64_t>(roundedEnd),
        };
    }

    settings.eventValues.reserve(
        settings.eventValues.size() + document.timeline.eventValues.size());
    settings.eventValueChanges.reserve(
        settings.eventValueChanges.size() + document.timeline.eventValues.size());
    for (const auto& event : document.timeline.eventValues)
    {
        const auto binding = std::ranges::find_if(
            settings.eventValues,
            [&](const EventValueBinding& candidate) { return candidate.name == event.eventName; });
        if (binding == settings.eventValues.end())
        {
            settings.eventValues.push_back({event.eventName, 0.0, event.domain});
        }
        else if (binding->domain != event.domain)
        {
            return false;
        }

        const auto exactFrame = static_cast<long double>(event.timeSeconds)
            * static_cast<long double>(settings.sampleRate);
        const auto roundedFrame = std::floor(exactFrame + 0.5L);
        if (!std::isfinite(event.timeSeconds) || event.timeSeconds < 0.0
            || roundedFrame > static_cast<long double>(std::numeric_limits<std::size_t>::max()))
        {
            return false;
        }
        settings.eventValueChanges.push_back({
            static_cast<std::size_t>(roundedFrame),
            event.eventName,
            event.value,
            event.domain,
            event.interpolation,
        });
    }

    std::stable_sort(
        settings.eventValueChanges.begin(),
        settings.eventValueChanges.end(),
        [](const EventValueChange& left, const EventValueChange& right) {
            return left.frame < right.frame;
        });
    for (std::size_t index = 1; index < settings.eventValueChanges.size(); ++index)
    {
        const auto& previous = settings.eventValueChanges[index - 1];
        const auto& current = settings.eventValueChanges[index];
        if (previous.frame == current.frame && previous.name == current.name)
            return false;
    }
    return true;
}

void applyReferencedBuiltinMidiEventValues(
    RenderSettings& settings,
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    struct BuiltinDefault
    {
        std::string_view name;
        double value;
        talas::expr::ValueDomain domain;
        bool referenced = false;
    };
    std::array<BuiltinDefault, 3> defaults{{
        {"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber},
        {"KeyVelocity", 0.0, talas::expr::ValueDomain::Scalar},
        {"KeyDown", 0.0, talas::expr::ValueDomain::Scalar},
    }};

    for (auto& builtin : defaults)
    {
        builtin.referenced = talas::project::projectReferencesEventValue(
            document,
            registry,
            builtin.name);
    }

    for (const auto& builtin : defaults)
    {
        if (!builtin.referenced)
            continue;
        const auto existing = std::ranges::find_if(
            settings.eventValues,
            [&](const EventValueBinding& binding) { return binding.name == builtin.name; });
        if (existing == settings.eventValues.end())
            settings.eventValues.push_back({std::string(builtin.name), builtin.value, builtin.domain});
    }
}
}

ProjectRenderSettingsResult prepareProjectRenderSettings(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    RenderSettings settings)
{
    settings.expressionLanguage = document.expressionLanguage;
    settings.expressionDefinitions = document.expressionDefinitions;
    settings.rampsStartAtFirstValue = document.rampsStartAtFirstValue;
    settings.rampsHoldUntriggered = document.rampsHoldUntriggered;
    settings.smalltalkNumbersAreFloats = document.smalltalkNumbersAreFloats;
    settings.timeFunctionsCountFromTheWholeSound = document.timeFunctionsCountFromTheWholeSound;
    settings.timeEventsIgnoreTimeControls = document.timeEventsIgnoreTimeControls;
    settings.inverseOfZeroIsInfinite = document.inverseOfZeroIsInfinite;
    settings.graphicalEnvelopeLoopsPlayOut = document.graphicalEnvelopeLoopsPlayOut;
    settings.banksReadLogSpectraAsHertz = document.banksReadLogSpectraAsHertz;
    settings.randomExpressionsShareSeeds = document.randomExpressionsShareSeeds;
    settings.musicalTime = document.timeline.musicalTime;
    if (!settings.musicalTime.tempo.empty())
    {
        settings.tempoBpm = 60000000.0
            / static_cast<double>(
                settings.musicalTime.tempo.front()
                    .microsecondsPerQuarter);
    }
    if (auto error = applyPersistedVirtualControls(settings, document);
        !error.empty())
    {
        return {std::nullopt, std::move(error)};
    }
    if (auto error =
            applyDiscoveredEventValues(settings, document, registry);
        !error.empty())
    {
        return {std::nullopt, std::move(error)};
    }
    applyReferencedBuiltinMidiEventValues(settings, document, registry);
    if (!applyPersistedTimeline(settings, document))
    {
        return {
            std::nullopt,
            "Project Timeline is invalid for the selected sample rate or conflicts with Event Value bindings",
        };
    }
    return {std::move(settings), {}};
}
}
