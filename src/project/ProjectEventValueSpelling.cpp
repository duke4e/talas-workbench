#include "talas/project/ProjectEventValueSpelling.h"

#include "talas/expr/Expression.h"
#include "talas/project/MidiDefaultMap.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace talas::project
{
namespace
{
constexpr auto val = talas::expr::ExpressionLanguageVersion::ValV1;

[[nodiscard]] std::string folded(std::string_view name)
{
    std::string result{name};
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

// The built-in Event Values, in their own spelling, the transport's
// (ADR-0030) last.
constexpr std::array<std::string_view, 20> builtInNames{"Pitch", "KeyPitch", "KeyPitch12", "KeyNumber", "KeyVelocity",
    "KeyTimbre", "KeyDown", "LocalTime", "TimingClock", "TimingClockDuration", "MIDITimeCode", "PitchBend", "Random",
    "Random1000", "RealTime", "Time", "Tempo", "SongPosition", "BeatPhase", "TransportPlaying"};

// Each expression a document holds, as its graphs' parameters and its Sound
// definitions' defaults.
void forEachExpression(ProjectDocument& document, const std::function<void(std::string&)>& visit)
{
    const auto inGraph = [&](ProjectGraph& graph) {
        for (auto& node : graph.nodes)
        {
            for (auto& parameter : node.parameters)
            {
                if (parameter.expression.has_value())
                    visit(*parameter.expression);
            }
        }
    };
    inGraph(document.graph);
    for (auto& definition : document.soundDefinitions)
    {
        inGraph(definition.graph);
        for (auto& exposed : definition.exposedParameters)
        {
            if (exposed.defaultValue.expression.has_value())
                visit(*exposed.defaultValue.expression);
        }
    }
}

// Each name's spellings as the document first has them: its Virtual
// Controls, its Timeline, then its expressions.
void forEachSpelling(ProjectDocument& document, const std::function<void(std::string_view)>& seen)
{
    for (const auto& control : document.virtualControls)
        seen(control.eventName);
    for (const auto& event : document.timeline.eventValues)
        seen(event.eventName);
    forEachExpression(document, [&](std::string& expression) {
        if (const auto names = talas::expr::scanEventReferenceNames(expression, val))
        {
            for (const auto& name : *names)
                seen(name);
        }
    });
}

// Every Event Value of the document written as `spelling` gives it.
bool respell(ProjectDocument& document, const std::function<std::optional<std::string>(std::string_view)>& spelling)
{
    auto changed = false;
    const auto rename = [&](std::string& name) {
        if (auto written = spelling(name); written.has_value() && *written != name)
        {
            name = std::move(*written);
            changed = true;
        }
    };
    for (auto& control : document.virtualControls)
        rename(control.eventName);
    for (auto& event : document.timeline.eventValues)
        rename(event.eventName);
    forEachExpression(document, [&](std::string& expression) {
        const auto names = talas::expr::scanEventReferenceNames(expression, val);
        if (!names.has_value())
            return;
        std::vector<std::string> targets;
        targets.reserve(names->size());
        std::vector<talas::expr::EventReferenceRename> renames;
        for (const auto& name : *names)
        {
            auto written = spelling(name);
            if (written.has_value() && *written != name)
                targets.push_back(std::move(*written));
            else
                targets.emplace_back();
        }
        for (std::size_t index = 0; index < names->size(); ++index)
        {
            if (!targets[index].empty())
                renames.push_back({(*names)[index], targets[index]});
        }
        if (renames.empty())
            return;
        if (auto renamed = talas::expr::renameEventReferences(expression, val, renames))
        {
            expression = std::move(*renamed);
            changed = true;
        }
    });
    return changed;
}
}

std::optional<ProjectDocument> withEventValueSpellingsUnified(
    const ProjectDocument& document, std::span<const std::string_view> firstSeen)
{
    if (document.expressionLanguage != val)
        return std::nullopt;
    std::map<std::string, std::string, std::less<>> first;
    for (const auto name : builtInNames)
        first.emplace(folded(name), std::string{name});
    for (const auto& controller : defaultMidiMapControllers)
        first.emplace(folded(controller.eventName), std::string{controller.eventName});
    for (const auto name : firstSeen)
        first.emplace(folded(name), std::string{name});
    auto unified = document;
    forEachSpelling(unified, [&](std::string_view name) {
        // !CC07, a MIDI controller, is !cc07.
        const auto key = folded(name);
        const auto midi = key.size() > 2 && (key.starts_with("cc") || key.starts_with("bt") || key.starts_with("sw"))
            && std::ranges::all_of(key.substr(2), [](unsigned char character) { return std::isdigit(character) != 0; });
        first.emplace(key, midi ? key : std::string{name});
    });
    const auto changed = respell(unified, [&](std::string_view name) -> std::optional<std::string> {
        const auto found = first.find(folded(name));
        return found == first.end() ? std::nullopt : std::optional{found->second};
    });
    if (!changed)
        return std::nullopt;
    return unified;
}

bool keepEventValueSpellingsApart(ProjectDocument& document)
{
    if (document.expressionLanguage != val)
        return false;
    // Each folded name's spellings in the order the document has them.
    std::map<std::string, std::vector<std::string>, std::less<>> spellings;
    std::map<std::string, bool, std::less<>> taken;
    forEachSpelling(document, [&](std::string_view name) {
        auto& known = spellings[folded(name)];
        if (std::ranges::find(known, name) == known.end())
            known.emplace_back(name);
        taken[std::string{name}] = true;
    });
    std::map<std::string, std::string, std::less<>> apart;
    for (const auto& [key, written] : spellings)
    {
        for (std::size_t index = 1; index < written.size(); ++index)
        {
            auto number = index + 1;
            auto name = written[index] + "_" + std::to_string(number);
            while (taken.contains(name) || spellings.contains(folded(name)))
                name = written[index] + "_" + std::to_string(++number);
            taken[name] = true;
            apart.emplace(written[index], std::move(name));
        }
    }
    if (apart.empty())
        return false;
    return respell(document, [&](std::string_view name) -> std::optional<std::string> {
        const auto found = apart.find(name);
        return found == apart.end() ? std::nullopt : std::optional{found->second};
    });
}

std::optional<std::size_t> findEventValueSpellingTwin(const ProjectDocument& document)
{
    if (document.expressionLanguage != val)
        return std::nullopt;
    std::map<std::string, std::size_t, std::less<>> seen;
    for (std::size_t index = 0; index < document.virtualControls.size(); ++index)
    {
        if (!seen.emplace(folded(document.virtualControls[index].eventName), index).second)
            return index;
    }
    return std::nullopt;
}

std::optional<ProjectDocument> withKeyPitchBends(const ProjectDocument& document)
{
    if (document.keyPitchIgnoresPitchBend || document.expressionLanguage != val)
        return std::nullopt;
    auto bent = document;
    auto changed = false;
    forEachExpression(bent, [&](std::string& expression) {
        auto written = talas::expr::withKeyPitchBends(expression, val);
        // A field that no longer reads keeps what it was.
        if (!written.has_value()
            || (!talas::expr::parse(*written, val, document.expressionDefinitions).ok()
                && talas::expr::parse(expression, val, document.expressionDefinitions).ok()))
            return;
        expression = std::move(*written);
        changed = true;
    });
    if (!changed)
        return std::nullopt;
    bent.keyPitchIgnoresPitchBend = true;
    return bent;
}

std::optional<ProjectDocument> withRandomEventSources(const ProjectDocument& document)
{
    if (document.randomEventSourcesAreFaders || document.expressionLanguage != val)
        return std::nullopt;
    auto drawn = document;
    auto changed = false;
    forEachExpression(drawn, [&](std::string& expression) {
        auto written = talas::expr::withRandomEventSources(expression, val);
        if (!written.has_value()
            || (!talas::expr::parse(*written, val, document.expressionDefinitions).ok()
                && talas::expr::parse(expression, val, document.expressionDefinitions).ok()))
            return;
        expression = std::move(*written);
        changed = true;
    });
    if (!changed)
        return std::nullopt;
    drawn.randomEventSourcesAreFaders = true;
    return drawn;
}

std::optional<ProjectDocument> withSignSelectionsStartingAtZero(const ProjectDocument& document)
{
    if (document.signSelectionsSelectAtOnce || document.expressionLanguage != val)
        return std::nullopt;
    auto started = document;
    auto changed = false;
    forEachExpression(started, [&](std::string& expression) {
        auto written = talas::expr::withSignSelectionsStartingAtZero(expression);
        if (!written.has_value()
            || (!talas::expr::parse(*written, val, document.expressionDefinitions).ok()
                && talas::expr::parse(expression, val, document.expressionDefinitions).ok()))
            return;
        expression = std::move(*written);
        changed = true;
    });
    if (!changed)
        return std::nullopt;
    started.signSelectionsSelectAtOnce = true;
    return started;
}

std::optional<ProjectDocument> withTimeRunningOnInSkippedBranches(const ProjectDocument& document)
{
    if (!document.timeRunsOnInSkippedBranches || document.expressionLanguage != val)
        return std::nullopt;
    auto running = document;
    auto changed = false;
    forEachExpression(running, [&](std::string& expression) {
        if (auto written = talas::expr::withTimeRunningOnInSkippedBranches(expression, document.expressionDefinitions))
        {
            expression = std::move(*written);
            changed = true;
        }
    });
    if (!changed)
        return std::nullopt;
    // Its expressions now carry the choice.
    running.timeRunsOnInSkippedBranches = false;
    return running;
}

bool holdsTimeInSkippedBranches(const ProjectDocument& document)
{
    if (document.expressionLanguage != val)
        return false;
    auto holds = false;
    auto scanned = document;
    forEachExpression(scanned, [&](std::string& expression) {
        holds = holds
            || talas::expr::withTimeRunningOnInSkippedBranches(expression, document.expressionDefinitions).has_value();
    });
    return holds;
}

std::optional<ProjectDocument> withWholeSoundClocks(const ProjectDocument& document)
{
    if (document.timeEventSourcesAreFaders || document.expressionLanguage != val)
        return std::nullopt;
    auto clocked = document;
    auto changed = false;
    forEachExpression(clocked, [&](std::string& expression) {
        if (auto written = talas::expr::withWholeSoundClocks(expression))
        {
            expression = std::move(*written);
            changed = true;
        }
    });
    if (!changed)
        return std::nullopt;
    return clocked;
}

bool readsTimeEventSources(const ProjectDocument& document)
{
    if (document.expressionLanguage != val)
        return false;
    auto reads = false;
    auto scanned = document;
    forEachExpression(scanned, [&](std::string& expression) {
        if (const auto names = talas::expr::scanEventReferenceNames(expression, val))
        {
            reads = reads || std::ranges::any_of(*names, [](std::string_view name) {
                const auto key = folded(name);
                return key == "realtime" || key == "time";
            });
        }
    });
    return reads;
}

bool holdsTimeFunctions(const ProjectDocument& document)
{
    auto holds = false;
    auto scanned = document;
    forEachExpression(scanned, [&](std::string& expression) {
        if (holds)
            return;
        const auto parsed = talas::expr::parse(expression, document.expressionLanguage, document.expressionDefinitions);
        holds = parsed.expression.has_value() && parsed.expression->usesTime();
    });
    return holds;
}

bool holdsSignSelections(const ProjectDocument& document)
{
    if (document.expressionLanguage != val)
        return false;
    auto holds = false;
    auto scanned = document;
    forEachExpression(scanned, [&](std::string& expression) {
        holds = holds || talas::expr::withSignSelectionsStartingAtZero(expression).has_value();
    });
    return holds;
}

bool readsRandomEventSources(const ProjectDocument& document)
{
    if (document.expressionLanguage != val)
        return false;
    auto reads = false;
    auto scanned = document;
    forEachExpression(scanned, [&](std::string& expression) {
        if (const auto names = talas::expr::scanEventReferenceNames(expression, val))
        {
            reads = reads || std::ranges::any_of(*names, [](std::string_view name) {
                const auto key = folded(name);
                return key == "random" || key == "random1000";
            });
        }
    });
    return reads;
}

bool readsKeyPitch(const ProjectDocument& document)
{
    if (document.expressionLanguage != val)
        return false;
    auto reads = false;
    auto scanned = document;
    forEachExpression(scanned, [&](std::string& expression) {
        if (const auto names = talas::expr::scanEventReferenceNames(expression, val))
        {
            reads = reads || std::ranges::any_of(*names, [](std::string_view name) {
                const auto key = folded(name);
                return key == "keypitch" || key == "keypitch12";
            });
        }
    });
    return reads;
}
}
