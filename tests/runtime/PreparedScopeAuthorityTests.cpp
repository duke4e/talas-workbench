#include "talas/core/BuiltinNodes.h"
#include "runtime/PreparedMidiVoiceHierarchy.h"
#include "runtime/PreparedScopeAuthority.h"
#include "talas/runtime/RealtimeRenderSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace
{
thread_local bool countAllocations = false;
thread_local std::size_t allocations = 0;
void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

talas::project::ProjectDocument fixture(int family, bool nested, bool midi)
{
    using namespace talas::project;
    ProjectDocument document;
    ProjectGraph body;
    body.nodes = {{"osc", "talas.oscillator",
        {{"frequency", "73 hz"}, {"amplitude", midi ? "!KeyDown * !KeyVelocity * !ScopeGain" : "1"}}}};
    if (family < 2)
    {
        body.nodes.push_back({"constant", "talas.constant", {{"value", family == 0 ? "-0.5" : "1"}}});
        body.nodes.push_back({"math", family == 0 ? "talas.absoluteValue" : "talas.arcTan", {}});
        body.nodes.push_back({"result", "talas.vca", {}});
        body.connections = {{"constant", "value", "math", "value"},
            {"osc", "audioOut", "result", "audioIn"}, {"math", "value", "result", "gain"}};
    }
    else
    {
        body.nodes.push_back({"result", "talas.filter", {{"frequency", "170 hz"}, {"q", "0.707"}}});
        body.connections = {{"osc", "audioOut", "result", "audioIn"}};
    }
    std::string source = "result";
    if (nested)
    {
        ProjectSoundDefinition definition;
        definition.definitionId = "scopeTone";
        definition.displayName = "Scope Tone";
        definition.graph = std::move(body);
        definition.outputs = {{"audioOut", talas::core::PortType::Audio, "result", "audioOut"}};
        document.soundDefinitions.push_back(std::move(definition));
        ProjectNode cell;
        cell.instanceId = "cell";
        cell.soundDefinitionId = "scopeTone";
        document.graph.nodes.push_back(std::move(cell));
        source = "cell";
    }
    else document.graph = std::move(body);
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    if (midi)
    {
        document.graph.nodes.push_back({"midi", "talas.midiVoice",
            {{"polyphony", "2"}, {"left", "1"}, {"right", "1"}, {"channel", "1"},
             {"lowPitch", "0 nn"}, {"highPitch", "127 nn"}}});
        auto& parameters = document.graph.nodes.back().parameters;
        for (const auto* id : {"source", "script"})
        {
            ProjectParameterExpression parameter;
            parameter.parameterId = id;
            parameter.text = "";
            parameters.push_back(std::move(parameter));
        }
        ProjectParameterExpression file;
        file.parameterId = "midiFile"; file.resource = "";
        parameters.push_back(std::move(file));
        ProjectParameterExpression shared;
        shared.parameterId = "shared"; shared.boolean = false;
        parameters.push_back(std::move(shared));
        document.graph.connections.push_back({source, "audioOut", "midi", "audioIn"});
        document.graph.connections.push_back({"midi", "leftOut", "out", "audioIn"});
        document.graph.connections.push_back({"midi", "rightOut", "out", "rightIn"});
    }
    else document.graph.connections.push_back({source, "audioOut", "out", "audioIn"});
    return document;
}

void queue(talas::runtime::RealtimeRenderSession& session, bool midi)
{
    if (!midi) return;
    require(session.scheduleMidiNote({talas::runtime::RealtimeMidiNoteKind::NoteOn, 1, 60, 127}, 0)
        == talas::runtime::RealtimeMidiDispatchResult::Handled, "first voice note must schedule");
    require(session.scheduleMidiNote({talas::runtime::RealtimeMidiNoteKind::NoteOn, 1, 64, 127}, 8)
        == talas::runtime::RealtimeMidiDispatchResult::Handled, "second voice note must schedule");
}

std::array<double, 64> render(talas::runtime::RealtimeRenderSession& session, bool irregular, bool midi)
{
    std::array<double, 64> output{};
    const std::array<std::size_t, 6> partitions{16, 1, 11, 7, 13, 16};
    std::size_t offset = 0, ordinal = 0;
    while (offset < output.size())
    {
        if (midi && offset == 16)
            require(session.scheduleEventValue("ScopeGain", 0.5, talas::expr::ValueDomain::Scalar, 7),
                "global event must schedule across distinct voice scopes");
        const auto frames = irregular ? partitions[ordinal++] : 16;
        allocations = 0; countAllocations = true;
        const bool ok = session.render(std::span(output).subspan(offset, frames));
        countAllocations = false;
        require(ok && allocations == 0, "scoped callbacks must render without allocation");
        offset += frames;
    }
    return output;
}

talas::project::ProjectDocument nestedOwnerEventValueFixture(bool localEventValue)
{
    using namespace talas::project;
    const auto expression = [](std::string id, std::string value) {
        ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.expression = std::move(value);
        return parameter;
    };
    const auto text = [](std::string id, std::string value) {
        ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.text = std::move(value);
        return parameter;
    };
    const auto resource = [](std::string id, std::string value) {
        ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.resource = std::move(value);
        return parameter;
    };
    const auto boolean = [](std::string id, bool value) {
        ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.boolean = value;
        return parameter;
    };
    const auto voiceOwner = [&](std::string id) {
        return ProjectNode{
            std::move(id),
            "talas.midiVoice",
            {expression("polyphony", "2"), expression("left", "1"),
             expression("right", "1"), expression("channel", "1"),
             expression("lowPitch", "0 nn"), expression("highPitch", "127 nn"),
             text("source", ""), text("script", ""), resource("midiFile", ""),
             boolean("shared", false)},
            {},
        };
    };

    ProjectDocument document;
    ProjectSoundDefinition reusable;
    reusable.definitionId = "reusableParent";
    reusable.displayName = "Reusable Parent";
    reusable.graph.nodes.push_back({
        "tone", "talas.oscillator",
        {expression("frequency", "73 hz"),
         expression("amplitude", localEventValue ? "!LocalGain" : "0.5")},
        {},
    });
    reusable.graph.nodes.push_back(voiceOwner("inner"));
    reusable.graph.connections.push_back(
        {"tone", "audioOut", "inner", "audioIn"});
    reusable.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "inner", "leftOut"});
    if (localEventValue)
    {
        ProjectSoundExposedParameter gain;
        gain.parameterId = "gain";
        gain.displayName = "Gain";
        gain.defaultValue = expression("gain", "0.5");
        gain.bindings.push_back({
            ProjectSoundParameterTarget::EventValue,
            {}, {}, "LocalGain", talas::expr::ValueDomain::Scalar});
        reusable.exposedParameters.push_back(std::move(gain));
    }
    document.soundDefinitions.push_back(std::move(reusable));

    ProjectNode firstSource;
    firstSource.instanceId = "sourceA";
    firstSource.soundDefinitionId = "reusableParent";
    ProjectNode secondSource;
    secondSource.instanceId = "sourceB";
    secondSource.soundDefinitionId = "reusableParent";
    if (localEventValue)
    {
        firstSource.parameters.push_back(expression("gain", "0.25"));
        secondSource.parameters.push_back(expression("gain", "0.5"));
    }
    document.graph.nodes = {
        std::move(firstSource), std::move(secondSource),
        voiceOwner("parentA"), voiceOwner("parentB"),
        {"mix", "talas.mixer", {}, {}}, {"out", "talas.audioOutput", {}, {}},
    };
    document.graph.connections = {
        {"sourceA", "audioOut", "parentA", "audioIn"},
        {"sourceB", "audioOut", "parentB", "audioIn"},
        {"parentA", "leftOut", "mix", "in1"},
        {"parentB", "leftOut", "mix", "in2"},
        {"mix", "audioOut", "out", "audioIn"},
    };
    return document;
}

void testNestedSoundEventValuesAreProjected()
{
    using namespace talas::runtime;
    RenderSettings settings{1000.0, 16};
    settings.eventValues = {
        {"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber},
        {"KeyVelocity", 0.0}, {"KeyDown", 0.0},
        {"LocalGain", 0.75, talas::expr::ValueDomain::Scalar},
    };
    const auto eventValue = compileProject(
        nestedOwnerEventValueFixture(true),
        talas::core::makeBuiltinNodeRegistry(), settings);
    if (!eventValue.ok())
        for (const auto& diagnostic : eventValue.diagnostics)
            std::cerr << diagnostic.message << '\n';
    require(eventValue.ok(), "nested Sound-local EventValues must compile with independent projections");
    std::size_t checked = 0;
    for (const auto& step : eventValue.graph->plan().steps)
    {
        if (step.typeId != "talas.oscillator") continue;
        const auto amplitude = std::ranges::find(step.parameters, "amplitude", &ParameterValue::id);
        const auto expected = step.structuralSourcePath.starts_with("sourceA/") ? 0.25 : 0.5;
        require(amplitude != step.parameters.end() && amplitude->value == expected
                && step.eventValueScopeId != globalRuntimeScope,
            "each nested oscillator must resolve its own Sound value instead of the same-name global fallback");
        ++checked;
    }
    require(checked == 8, "two reusable parents with 2x2 slots must specialize eight local readers");
    const auto global = std::ranges::find_if(eventValue.graph->renderSettings().eventValues,
        [](const auto& value) { return value.scopeId == globalRuntimeScope && value.name == "LocalGain"; });
    require(global != eventValue.graph->renderSettings().eventValues.end() && global->value == 0.75,
        "projected local values must preserve the independent global value");

    const auto plain = compileProject(
        nestedOwnerEventValueFixture(false),
        talas::core::makeBuiltinNodeRegistry(), settings);
    if (!plain.ok())
        for (const auto& diagnostic : plain.diagnostics)
            std::cerr << diagnostic.message << '\n';
    require(plain.ok(),
        "the equivalent nested owner graph with a literal Sound parameter must still prepare");
}

void testNestedVoiceCountProjectionDependencies()
{
    using namespace talas::project;
    using namespace talas::runtime;
    auto document = nestedOwnerEventValueFixture(true);
    auto& sound = document.soundDefinitions.front();
    auto& child = sound.graph.nodes[1];
    std::ranges::find(child.parameters, "polyphony", &ProjectParameterExpression::parameterId)
        ->expression = "!VoiceCount";
    for (const auto& [name, source] : {std::pair{"VoiceCount", "!CountBase + 1"}, {"CountBase", "2"}})
    {
        ProjectSoundExposedParameter parameter;
        parameter.parameterId = name; parameter.displayName = name;
        parameter.defaultValue = {name, source};
        parameter.bindings.push_back({ProjectSoundParameterTarget::EventValue,
            {}, {}, name, talas::expr::ValueDomain::Scalar});
        sound.exposedParameters.push_back(std::move(parameter));
    }
    document.graph.nodes[1].parameters.push_back({"CountBase", "3"});
    RenderSettings settings{1000.0, 16};
    settings.eventValues = {{"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber},
        {"KeyVelocity", 0.0}, {"KeyDown", 0.0}, {"LocalGain", 0.75},
        {"VoiceCount", 9.0}, {"CountBase", 7.0}};
    const auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    if (!compiled.ok())
        for (const auto& diagnostic : compiled.diagnostics) std::cerr << diagnostic.message << '\n';
    require(compiled.ok(), "nested static polyphony resolves its local transitive EventValue dependencies");
    std::size_t children = 0;
    for (const auto& owner : compiled.graph->plan().midiVoices)
    {
        if (owner.voiceAncestors.empty()) continue;
        const auto expected = owner.voiceAncestors.front().ownerNodeId == "parentA" ? 3U : 4U;
        require(owner.voices.size() == expected,
            "each reusable parent's static polyphony must use its local dependency chain instead of globals: "
                + owner.voiceAncestors.front().ownerNodeId + " has " + std::to_string(owner.voices.size()));
        ++children;
    }
    require(children == 4, "both voices of both parents retain their own configured child owner");
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV4;
    sound.exposedParameters[1].defaultValue.expression = "@missing";
    require(!compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings).ok(),
        "an unresolved local voice-count dependency cannot use a global namesake as fallback");
    sound.exposedParameters[1].defaultValue.expression = "!VoiceCount + 1";
    const auto cyclic = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(!cyclic.ok() && std::ranges::any_of(cyclic.diagnostics, [](const auto& diagnostic) {
            return diagnostic.message.find("dependency cycle") != std::string::npos;
        }), "cyclic local voice-count projections must reject before allocating voice instances");
}

void testRuntimeScopes(bool golden)
{
    for (int family = 0; family != 3; ++family)
    for (bool nested : {false, true})
    for (bool midi : {false, true})
    {
        talas::runtime::RenderSettings settings{1000.0, 16};
        settings.eventValues = {{"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber},
            {"KeyVelocity", 0.0}, {"KeyDown", 0.0}, {"ScopeGain", 1.0}};
        const auto compiled = talas::runtime::compileProject(fixture(family, nested, midi),
            talas::core::makeBuiltinNodeRegistry(), settings);
        if (!compiled.ok())
            for (const auto& diagnostic : compiled.diagnostics) std::cerr << diagnostic.message << '\n';
        require(compiled.ok(), "scoped fixture must compile");
        talas::runtime::RealtimeRenderSession candidate(*compiled.graph), reference(*compiled.graph);
        if (!candidate.ready())
        {
            for (const auto& step : compiled.graph->plan().steps)
            {
                std::cerr << step.typeId << " scopes " << step.scopeId << '/' << step.structuralScopeId << '/' << step.eventValueScopeId << '\n';
                for (const auto& parameter : step.parameters)
                    std::cerr << " param " << parameter.id << " scope " << parameter.variableScopeId << '\n';
            }
            for (const auto& scope : compiled.graph->renderSettings().preparedVariables.scopes)
                std::cerr << " scope " << scope.scopeOrdinal << " parent " << scope.parentScopeOrdinal << '\n';
        }
        require(candidate.ready() && reference.ready(), "scoped fixture must prepare: " + std::string(candidate.error()));
        queue(candidate, midi); queue(reference, midi);
        const auto output = render(candidate, true, midi);
        require(output == render(reference, false, midi), "scope identity must preserve exact partitioned PCM");
        require(std::ranges::any_of(output, [](double v) { return std::abs(v) > 1e-5; }),
            "scoped fixture must produce audible evidence");
        if (!midi)
        {
            const auto offline = talas::runtime::renderOffline(*compiled.graph, output.size());
            require(offline.ok() && std::ranges::equal(output, offline.mono),
                "local and nested scopes must retain exact independent native PCM");
        }
        if (family < 2)
        {
            // ArcTan of a mono 1 is the angle of 1 over 1, a quarter of a
            // half turn.
            const auto gain = family == 0 ? 0.5 : 0.25;
            for (std::size_t frame = 0; frame < output.size(); ++frame)
            {
                const auto tone = [](std::size_t age) {
                    return std::sin(2.0 * std::numbers::pi_v<double> * 73.0 * static_cast<double>(age) / 1000.0);
                };
                // Prepared oscillators run from the transport origin in each
                // voice. MIDI retrigger resets expression/variable state, while
                // the five-millisecond attack controls oscillator audibility.
                const auto expected = !midi ? gain * tone(frame)
                    : gain * (frame < 23 ? 1.0 : 0.5)
                        * ((frame >= 5 ? tone(frame) : 0.0) + (frame >= 13 ? tone(frame) : 0.0));
                require(std::abs(output[frame] - expected) < 1e-12,
                    "independent voice attack, global event scope and unary gain oracle must match at "
                        + std::to_string(frame) + ": " + std::to_string(output[frame]) + " vs " + std::to_string(expected));
            }
        }
        candidate.resetTransport();
        reference.resetTransport();
        const auto resetOutput = render(candidate, true, midi);
        require(resetOutput == render(reference, false, midi),
            "transport reset must preserve exact held-voice continuation across partitions");
        if (!midi) require(resetOutput == output, "non-voice DSP reset must match its initial render");
        if (midi && family < 2)
        {
            const auto gain = family == 0 ? 0.5 : 0.25;
            for (std::size_t frame = 0; frame < resetOutput.size(); ++frame)
            {
                const auto expected = 2.0 * gain * (frame < 23 ? 1.0 : 0.5)
                    * std::sin(2.0 * std::numbers::pi_v<double> * 73.0 * static_cast<double>(frame) / 1000.0);
                require(std::abs(resetOutput[frame] - expected) < 1e-12,
                    "transport reset must reset DSP phase while preserving both held voices");
            }
        }
        if (golden)
        {
            std::cout << "family=" << family << " nested=" << nested << " midi=" << midi << '\n';
            for (double value : output) std::cout << std::hexfloat << value << '\n';
            for (double value : resetOutput) std::cout << std::hexfloat << value << '\n';
        }
        else std::cout << "family=" << family << " nested=" << nested << " midi=" << midi << " passed\n";
    }
}
talas::runtime::RuntimePlan nestedVoiceHierarchyPlan()
{
    using namespace talas::runtime;
    RuntimePlan plan;
    plan.steps.resize(9);
    const std::array<RuntimeScopeId, 9> stepScopes{
        10, 11, 12, 1, 13, 14, 15, 2, 50};
    for (std::size_t index = 0; index < stepScopes.size(); ++index)
        plan.steps[index].scopeId = stepScopes[index];

    MidiVoicePlan outer;
    outer.ownerStepIndex = 8;
    outer.configuration.polyphony = 2;
    outer.ownerNodeId = "outer";
    outer.ownerDefinitionId = "OuterDefinition";
    outer.ownerInstancePath = {"parent"};
    for (std::uint16_t ordinal = 0; ordinal < 2; ++ordinal)
    {
        VoiceInstancePlan voice;
        voice.scopeId = static_cast<RuntimeScopeId>(ordinal + 1);
        voice.firstStepIndex = ordinal == 0 ? 0 : 4;
        voice.stepCount = 4;
        voice.voiceOrdinal = ordinal;
        outer.voices.push_back(std::move(voice));
    }
    plan.midiVoices.push_back(std::move(outer));

    const auto addChild = [&](std::uint16_t parentOrdinal,
                              RuntimeScopeId parentScope,
                              std::size_t ownerStepIndex,
                              RuntimeScopeId firstChildScope,
                              std::size_t firstChildStep) {
        MidiVoicePlan child;
        child.ownerStepIndex = ownerStepIndex;
        child.configuration.polyphony = 3;
        child.ownerNodeId = "inner";
        child.ownerDefinitionId = "InnerDefinition";
        child.ownerInstancePath = {"parent", "inner"};
        child.parentVoiceScopeId = parentScope;
        child.voiceAncestors.push_back({
            "outer", "OuterDefinition", {"parent"}, parentOrdinal});
        for (std::uint16_t ordinal = 0; ordinal < 3; ++ordinal)
        {
            VoiceInstancePlan voice;
            voice.scopeId = static_cast<RuntimeScopeId>(firstChildScope + ordinal);
            voice.firstStepIndex = firstChildStep + ordinal;
            voice.stepCount = 1;
            voice.voiceOrdinal = ordinal;
            child.voices.push_back(std::move(voice));
        }
        plan.midiVoices.push_back(std::move(child));
    };
    addChild(0, 1, 3, 10, 0);
    addChild(1, 2, 7, 13, 4);
    return plan;
}

void testPreparedVoiceHierarchyLayouts()
{
    namespace hierarchy = talas::runtime::prepared_voice_hierarchy;
    using namespace talas::runtime;
    const auto plan = nestedVoiceHierarchyPlan();
    const auto layout = hierarchy::prepare(plan);
    require(layout.ok(), "valid 2x3 nested MIDI Voice layout must prepare: " + layout.error);
    require(layout.hierarchical
            && layout.firstVoiceStateByRegion
                == std::vector<std::size_t>{0, 2, 5}
            && layout.parentVoiceStateByRegion
                == std::vector<std::size_t>{hierarchy::noIndex, 0, 1}
            && layout.regionByOwnerStep
                == std::vector<std::size_t>{hierarchy::noIndex,
                    hierarchy::noIndex, hierarchy::noIndex, 1,
                    hierarchy::noIndex, hierarchy::noIndex,
                    hierarchy::noIndex, 2, 0}
            && layout.voiceStateByStep
                == std::vector<std::size_t>{2, 3, 4, 0, 5, 6, 7, 1,
                    hierarchy::noIndex},
        "2x3 layout must flatten states and assign direct step owners deterministically");

    RuntimePlan flat;
    flat.steps.resize(4);
    flat.steps[0].scopeId = 41;
    flat.steps[1].scopeId = 42;
    flat.steps[2].scopeId = 43;
    flat.steps[3].scopeId = 40;
    MidiVoicePlan flatRegion;
    flatRegion.ownerStepIndex = 3;
    flatRegion.configuration.polyphony = 2;
    flatRegion.ownerNodeId = "flat";
    for (std::uint16_t ordinal = 0; ordinal < 2; ++ordinal)
    {
        VoiceInstancePlan voice;
        voice.scopeId = static_cast<RuntimeScopeId>(41 + ordinal);
        voice.firstStepIndex = ordinal;
        voice.stepCount = 1;
        voice.voiceOrdinal = ordinal;
        flatRegion.voices.push_back(std::move(voice));
    }
    flat.midiVoices.push_back(std::move(flatRegion));
    const auto flatLayout = hierarchy::prepare(flat);
    require(flatLayout.ok() && !flatLayout.hierarchical,
        "valid flat MIDI Voice layouts must remain accepted");

    const auto reject = [&](auto mutate, std::string_view reason) {
        auto forged = plan;
        mutate(forged);
        const auto result = hierarchy::prepare(forged);
        require(!result.ok(), std::string(reason) + " must be rejected");
    };
    reject([](auto& forged) {
        forged.midiVoices[1].voiceAncestors.front().voiceOrdinal = 1;
    }, "mismatched parent ancestry");
    reject([](auto& forged) {
        forged.midiVoices[1].parentVoiceScopeId = 999;
    }, "missing parent scope");
    reject([](auto& forged) {
        forged.steps[4].scopeId = forged.midiVoices[1].voices[0].scopeId;
    }, "nested owner step with the wrong direct parent scope");
    reject([](auto& forged) {
        forged.midiVoices[1].voices[0].stepCount = 2;
    }, "partial sibling range overlap");
    reject([](auto& forged) {
        forged.midiVoices[2].voices[0].scopeId =
            forged.midiVoices[1].voices[0].scopeId;
    }, "duplicate voice scope");
    reject([](auto& forged) {
        forged.midiVoices[2].ownerStepIndex =
            forged.midiVoices[1].ownerStepIndex;
    }, "duplicate owner step");
    reject([](auto& forged) {
        forged.midiVoices[1].voices[0].stepCount = 0;
    }, "empty voice range");
    reject([](auto& forged) {
        forged.midiVoices[0].voices[1].firstStepIndex = 0;
    }, "voice ranges out of local ordinal order");
    reject([](auto& forged) {
        forged.midiVoices[0].voices[1].stepCount = 5;
    }, "voice range extending past its owner step");
    reject([](auto& forged) {
        forged.midiVoices[1].voices[2].stepCount = 2;
    }, "child voice range extending past its owner step");
    reject([](auto& forged) {
        forged.steps.back().scopeId = forged.midiVoices[1].voices[0].scopeId;
    }, "known voice scope outside its range");
    reject([](auto& forged) {
        forged.steps[2].scopeId = 99;
    }, "unowned source step inside a voice range");
    reject([](auto& forged) {
        forged.midiVoices[0].ownerStepIndex = 1;
    }, "root owner step inside a voice range");
    reject([](auto& forged) {
        forged.midiVoices[0].voiceAncestors.push_back(
            {"wrong", "wrong", {}, 0});
    }, "root ancestry");

    RuntimePlan overBudget;
    overBudget.midiVoices.resize(65);
    for (auto& region : overBudget.midiVoices)
    {
        region.configuration.polyphony = 128;
        region.voices.resize(128);
    }
    require(!hierarchy::prepare(overBudget).ok(),
        "total prepared voice scopes must respect the hierarchy budget");
}

void testProjectedVoiceScopeAuthority()
{
    using namespace talas::runtime;
    RuntimePlan plan;
    plan.steps.resize(3);
    plan.steps[0].scopeId = 5;
    plan.steps[0].structuralScopeId = 1;
    plan.steps[0].eventValueScopeId = 6;
    plan.steps[1].scopeId = 9;
    plan.steps[1].eventValueScopeId = 10;
    plan.steps[2].scopeId = globalRuntimeScope;
    ParameterValue parameter;
    parameter.variableScopeId = 8;
    plan.steps[0].parameters.push_back(std::move(parameter));

    MidiVoicePlan region;
    region.ownerStepIndex = 2;
    region.configuration.polyphony = 2;
    for (std::uint16_t ordinal = 0; ordinal < 2; ++ordinal)
    {
        VoiceInstancePlan voice;
        voice.scopeId = ordinal == 0 ? 5 : 9;
        voice.firstStepIndex = ordinal;
        voice.stepCount = 1;
        voice.voiceOrdinal = ordinal;
        region.voices.push_back(std::move(voice));
    }
    plan.midiVoices.push_back(std::move(region));

    const std::vector<talas::project::PreparedProjectVariableScope> scopes{
        {0, 0, {}, {}}, {1, 0, {}, {}}, {5, 0, {}, {}},
        {6, 5, {}, {}}, {7, 5, {}, {}}, {8, 6, {}, {}},
        {9, 0, {}, {}}, {10, 9, {}, {}}, {11, 5, {}, {}},
    };
    const auto authority = prepared_scope::VoiceSources::prepare(plan, &plan);
    require(authority.permitsProjectedScope(0, 6, scopes)
            && authority.permitsProjectedScope(0, 8, scopes),
        "exact current-step event and parameter scopes below their voice should be permitted");
    require(!authority.permitsProjectedScope(0, 7, scopes)
            && !authority.permitsProjectedScope(0, 11, scopes)
            && !authority.permitsProjectedScope(0, 5, scopes)
            && !authority.permitsProjectedScope(1, 6, scopes),
        "sibling, unlisted descendant, processing and other-step scopes are not projected grants");
    require(!prepared_scope::VoiceSources::prepare(plan, nullptr)
                 .permitsProjectedScope(0, 6, scopes),
        "a public plan without compiler authority cannot grant a projected scope");
    auto modified = plan;
    modified.steps[0].eventValueScopeId = 7;
    require(!prepared_scope::VoiceSources::prepare(modified, &plan)
                 .permitsProjectedScope(0, 6, scopes),
        "a plan that differs from compiler-sealed scope identities cannot grant a projection");
}

void testScopeQueries()
{
    namespace scope = talas::runtime::prepared_scope;
    using namespace talas::runtime;
    for (const auto structural : {0U, 5U, 9U})
    for (const auto events : {0U, 5U, 9U})
    {
        CompileStep step;
        step.scopeId = 5; step.structuralScopeId = structural; step.eventValueScopeId = events;
        const auto identity = scope::Identity::fromStep(step);
        require(identity.processing == 5 && identity.structural == structural && identity.events == events,
            "scope identity must retain all three roles");
        require(identity.isLocal() == (structural == 5 && events == 5),
            "local authority requires both original structural and event scopes");
    }
    std::vector<talas::project::PreparedProjectVariableScope> scopes{
        {0, 0, {}, {}}, {1, 0, {}, {}}, {2, 0, {}, {}}, {3, 1, {}, {}}, {4, 2, {}, {}}};
    allocations = 0; countAllocations = true;
    const bool ancestry = scope::isScopeOrAncestor(scopes, 3, 1)
        && scope::isScopeOrAncestor(scopes, 3, 0)
        && scope::isScopeOrAncestor(scopes, 3, 3)
        && !scope::isScopeOrAncestor(scopes, 3, 2)
        && !scope::isScopeOrAncestor(scopes, 1, 3)
        && !scope::isScopeOrAncestor(scopes, 0, 1)
        && !scope::isScopeOrAncestor(scopes, 9, 0)
        && scope::isScopeOrAncestor(scopes, 9, 9);
    countAllocations = false;
    require(ancestry && allocations == 0, "ancestry must separate parent, sibling and missing scopes without allocation");
    auto malformed = scopes;
    malformed.push_back(scopes[3]);
    require(!scope::isScopeOrAncestor(malformed, 3, 0), "duplicate ancestry records must reject lookup");
    malformed = scopes; malformed[3].parentScopeOrdinal = 3;
    require(!scope::isScopeOrAncestor(malformed, 3, 0), "self-parent cannot manufacture an ancestor");
    malformed[3].parentScopeOrdinal = 4; malformed[4].parentScopeOrdinal = 3;
    require(!scope::isScopeOrAncestor(malformed, 3, 0), "cycles must terminate without inventing ancestry");
    std::reverse(scopes.begin(), scopes.end());
    require(scope::isScopeOrAncestor(scopes, 3, 1), "scope order must not change identity lookup");

    RuntimePlan plan;
    plan.steps.resize(5);
    for (auto& step : plan.steps) step.scopeId = 5;
    plan.steps[0].scopeId = 0; plan.steps[3].scopeId = 6;
    MidiVoicePlan region;
    VoiceInstancePlan voice;
    voice.scopeId = 5; voice.firstStepIndex = 1; voice.stepCount = 2;
    region.voices.push_back(voice); plan.midiVoices.push_back(region);
    for (std::size_t index = 0; index <= plan.steps.size(); ++index)
        require(scope::midiVoiceMembershipCount(plan, index) == (index == 1 || index == 2 ? 1U : 0U),
            "voice membership must use matching scopes and half-open step ranges");
    auto ambiguous = plan;
    ambiguous.midiVoices.push_back(region);
    require(scope::midiVoiceMembershipCount(ambiguous, 1) == 2,
        "duplicate voice owners must remain ambiguous rather than selecting the first");
    ambiguous = plan; ambiguous.midiVoices[0].voices.push_back(voice);
    require(scope::midiVoiceMembershipCount(ambiguous, 2) == 2,
        "overlapping voice instances within one owner must remain ambiguous");
    for (auto first : {std::size_t{0}, std::size_t{4}, std::size_t{5}, std::numeric_limits<std::size_t>::max()})
    for (auto count : {std::size_t{0}, std::size_t{2}, std::numeric_limits<std::size_t>::max()})
    {
        auto bad = plan;
        bad.midiVoices[0].voices[0].firstStepIndex = first;
        bad.midiVoices[0].voices[0].stepCount = count;
        for (std::size_t index = 0; index <= bad.steps.size(); ++index)
        {
            // Independent small-domain interval oracle; only [0,2) is valid.
            const auto expected = first == 0 && count == 2 && index == 1 ? 1U : 0U;
            require(scope::midiVoiceMembershipCount(bad, index) == expected,
                "malformed and overflowing voice ranges must not manufacture membership");
        }
    }

    RuntimePlan lowered;
    lowered.steps.resize(1);
    lowered.steps[0].scopeId = 2;
    lowered.steps[0].structuralScopeId = 1;
    lowered.steps[0].eventValueScopeId = 1;
    voice.scopeId = 2; voice.firstStepIndex = 0; voice.stepCount = 1;
    region.voices = {voice}; lowered.midiVoices = {region};
    const auto compilerPlan = lowered;
    const auto sources = scope::VoiceSources::prepare(lowered, &compilerPlan);
    allocations = 0; countAllocations = true;
    const auto source = sources.sourceForStep(0);
    const bool bridge = source == 1
        && scope::isProcessingOrSourceAncestor(scopes, 2, source, 1)
        && scope::isProcessingOrSourceAncestor(scopes, 2, source, 0)
        && !scope::isProcessingOrSourceAncestor(scopes, 2, source, 3)
        && !scope::isProcessingOrSourceAncestor(scopes, 2, source, 4)
        && !scope::isProcessingOrSourceAncestor(scopes, 2, std::nullopt, 1);
    countAllocations = false;
    require(bridge && allocations == 0, "compiler-owned source bridge must separate structural lineage from processing siblings");
    require(!scope::VoiceSources::prepare(lowered, nullptr).sourceForStep(0)
            && !sources.sourceForStep(1), "absent compiler authority and unknown steps cannot supply a source bridge");
    auto modified = lowered;
    modified.steps[0].eventValueScopeId = 9;
    require(!scope::VoiceSources::prepare(modified, &compilerPlan).sourceForStep(0),
        "modified executable plan must not borrow a compiler source bridge");
    modified = lowered; modified.midiVoices.push_back(region);
    const auto duplicatedCompiler = modified;
    require(!scope::VoiceSources::prepare(modified, &duplicatedCompiler).sourceForStep(0),
        "ambiguous compiler voice ownership cannot supply a source bridge");
    modified = lowered; modified.midiVoices[0].voices[0].stepCount = 0;
    const auto emptyCompiler = modified;
    require(!scope::VoiceSources::prepare(modified, &emptyCompiler).sourceForStep(0),
        "unowned lowered steps cannot supply a source bridge");
}

void testForgedRuntimeScopes()
{
    using namespace talas::runtime;
    RenderSettings settings{1000.0, 16};
    settings.eventValues = {{"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber},
        {"KeyVelocity", 0.0}, {"KeyDown", 0.0}, {"ScopeGain", 1.0}};
    for (int family = 0; family != 3; ++family)
    {
        const auto compiled = compileProject(fixture(family, true, true), talas::core::makeBuiltinNodeRegistry(), settings);
        require(compiled.ok(), "nested MIDI forged-scope fixture must compile");
        const auto reject = [&](auto mutate) {
            auto graph = *compiled.graph;
            mutate(const_cast<RuntimePlan&>(graph.plan()), const_cast<RenderSettings&>(graph.renderSettings()));
            RealtimeRenderSession session(graph);
            require(!session.ready(), "forged nested voice, source or settings authority must reject preparation");
        };
        reject([](auto& plan, auto&) { plan.steps[0].structuralScopeId = plan.steps[0].scopeId + 1; });
        reject([](auto& plan, auto&) { plan.steps[0].eventValueScopeId = plan.steps[0].scopeId + 1; });
        reject([](auto& plan, auto&) { plan.steps[0].scopeId = globalRuntimeScope; });
        reject([](auto& plan, auto&) { plan.midiVoices.push_back(plan.midiVoices.front()); });
        reject([](auto& plan, auto&) { plan.midiVoices.front().voices.front().stepCount = std::numeric_limits<std::size_t>::max(); });
        reject([](auto& plan, auto&) { plan.steps[0].parameters[0].variableScopeId = plan.steps.back().scopeId; });
        reject([](auto&, auto& config) { config.preparedVariables.scopes[1].parentScopeOrdinal = 1; });
        reject([](auto&, auto& config) { config.preparedVariables.scopes.push_back(config.preparedVariables.scopes.back()); });
        RealtimeRenderSession reconstructed(PreparedRuntimeGraph{compiled.graph->plan(), compiled.graph->renderSettings()});
        require(!reconstructed.ready(), "public reconstruction cannot manufacture compiler voice source authority");
    }
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
int main(int argc, char** argv)
{
    testPreparedVoiceHierarchyLayouts();
    testProjectedVoiceScopeAuthority();
    testScopeQueries();
    testNestedSoundEventValuesAreProjected();
    testNestedVoiceCountProjectionDependencies();
    testForgedRuntimeScopes();
    testRuntimeScopes(argc > 1 && std::string_view{argv[1]} == "--golden");
}
