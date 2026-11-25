#include "talas/core/ValScalarArray.h"
#include "talas/core/AnalogSequencerArray.h"
#include "talas/core/AnalogSequencerExtraValues.h"
#include "talas/core/StaticResourceList.h"
#include "talas/project/MidiPitchBounds.h"
#include "talas/project/ProjectExpressionAudit.h"

#include "talas/project/ProjectMidiVoiceIslands.h"
#include "talas/project/ProjectValidation.h"
#include "talas/project/SmalltalkSubset.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace talas::project
{
namespace
{
// Set while auditing a language change that must keep every meaning.
thread_local std::optional<talas::expr::ExpressionLanguageVersion>
    meaningReferenceLanguage;

void appendDiagnostic(
    ProjectExpressionAuditResult& result,
    const std::string& path,
    talas::expr::Diagnostic::Code code,
    talas::expr::SourceRange range,
    std::string message)
{
    if (result.truncated)
        return;
    if (result.diagnostics.size() >= maximumProjectExpressionDiagnostics)
    {
        result.truncated = true;
        return;
    }
    result.diagnostics.push_back({
        path,
        code,
        range,
        std::move(message),
    });
}

// Whether a ?name a class's field reads is one of the class's variable
// parameters, which takes the instance's
// value as the class expands.
[[nodiscard]] bool isClassVariable(
    const ProjectDocument& document,
    std::span<const std::string_view> scope,
    std::string_view name)
{
    return std::ranges::any_of(scope, [&](std::string_view definitionId) {
        const auto definition = std::ranges::find(document.soundDefinitions, definitionId,
            &ProjectSoundDefinition::definitionId);
        return definition != document.soundDefinitions.end()
            && std::ranges::any_of(definition->exposedParameters, [&](const ProjectSoundExposedParameter& exposed) {
                   return std::ranges::any_of(exposed.bindings, [&](const ProjectSoundParameterBinding& binding) {
                       return binding.target == ProjectSoundParameterTarget::Variable && binding.variableName == name;
                   });
               });
    });
}

void appendParsedExpressionDiagnostics(
    ProjectExpressionAuditResult& result,
    std::string_view source,
    const std::string& path,
    talas::expr::ExpressionLanguageVersion language,
    const talas::expr::ExpressionDefinitionLibrary& definitions,
    std::optional<talas::expr::ValueDomain> expectedDomain,
    std::size_t sourceOffset = 0,
    bool requireConstant = false,
    bool allowStateful = true,
    const ProjectDocument* variableDocument = nullptr,
    std::span<const std::string_view> soundDefinitionScope = {},
    const ProjectVariableOwner* midiVoiceOwner = nullptr,
    bool midiPitchBound = false,
    bool allowVoiceTemplate = false,
    std::uint16_t replicationNumberVoices = 0,
    bool allowOnlyClockedStateful = false,
    std::span<const ProjectVariableOwner> midiVoiceAncestors = {})
{
    const auto diagnosticLocation = [&definitions, &path, sourceOffset](
                                        const talas::expr::Diagnostic& diagnostic) {
        if (diagnostic.definitionIndex.has_value()
            && *diagnostic.definitionIndex
                < definitions.definitions.size())
        {
            return std::pair{
                "$.expressionDefinitions["
                    + definitions.definitions[*diagnostic.definitionIndex]
                          .name
                    + "].source",
                diagnostic.range};
        }
        return std::pair{
            path,
            talas::expr::SourceRange{
                sourceOffset + diagnostic.range.offset,
                diagnostic.range.length}};
    };
    std::string specializedSource;
    const auto auditNumberVoices = replicationNumberVoices != 0
        ? replicationNumberVoices
        : allowVoiceTemplate
                && (source.find("?VoiceNumber") != std::string_view::npos
                    || source.find("?NumberVoices")
                        != std::string_view::npos)
            ? std::uint16_t{1} : std::uint16_t{0};
    if (auditNumberVoices != 0)
    {
        auto specialized = talas::expr::specializeReplicationVariablesForAudit(
            source, 1, auditNumberVoices);
        if (!specialized.has_value())
        {
            appendDiagnostic(result, path,
                talas::expr::Diagnostic::Code::BudgetExceeded,
                {sourceOffset, source.size()},
                "Replicator voice binding exceeds the expression size limit");
            return;
        }
        specializedSource = std::move(*specialized);
    }
    // A sample file's duration is known as the Sound compiles; any will do
    // to audit the rest.
    const auto withDurations = talas::expr::withSampleFileDurations(
        auditNumberVoices != 0 ? std::string_view{specializedSource} : source,
        [](std::string_view) { return std::optional<double>{1.0}; });
    const auto auditedSource = withDurations.has_value() ? std::string_view{*withDurations}
        : auditNumberVoices != 0 ? std::string_view{specializedSource} : source;
    auto parsed = talas::expr::parse(auditedSource, language, definitions);
    for (const auto& diagnostic : parsed.diagnostics)
    {
        const auto [diagnosticPath, diagnosticRange] =
            diagnosticLocation(diagnostic);
        appendDiagnostic(
            result,
            diagnosticPath,
            diagnostic.code,
            diagnosticRange,
            diagnostic.message);
    }
    if (!parsed.ok() || result.truncated)
        return;
    if (meaningReferenceLanguage.has_value()
        && *meaningReferenceLanguage != language)
    {
        const auto previous = talas::expr::parse(
            auditedSource, *meaningReferenceLanguage, definitions);
        if (previous.ok()
            && !parsed.expression->computesSameAs(*previous.expression))
        {
            appendDiagnostic(
                result,
                path,
                talas::expr::Diagnostic::Code::UnexpectedToken,
                {sourceOffset, source.size()},
                "Val sends binary operators from left to right; "
                "add parentheses to keep this expression's meaning");
            return;
        }
    }

    if (parsed.expression->usesVoiceNumber())
    {
        if (midiVoiceOwner == nullptr && !allowVoiceTemplate)
        {
            // Unbound evaluation preserves the exact marker/definition range
            // and cannot be satisfied by an external identifier resolver.
            const auto unbound = talas::expr::evaluate(*parsed.expression, {});
            for (const auto& diagnostic : unbound.diagnostics)
            {
                const auto [diagnosticPath, diagnosticRange] = diagnosticLocation(diagnostic);
                appendDiagnostic(result, diagnosticPath, diagnostic.code, diagnosticRange,
                    "?VoiceNumber requires a MIDI Voice input graph or a reusable Sound template");
            }
            return;
        }
        // Audit the template's domains with a representative positive ordinal.
        // Expanded runtime preparation owns exact instance/voice membership
        // and validates every configured ordinal before publishing a graph.
        parsed.expression = parsed.expression->specializeVoiceNumber(1);
        if (!parsed.expression.has_value())
        {
            appendDiagnostic(result, path, talas::expr::Diagnostic::Code::UnknownIdentifier,
                {sourceOffset, source.size()}, "Voice template specialization failed during audit");
            return;
        }
    }

    if (variableDocument != nullptr)
    {
        const auto diagnosticsBeforeVariables = result.diagnostics.size();
        bool readsClassVariables = false;
        bool readsEnvironment = false;
        for (const auto& reference
             : parsed.expression->variableReferenceMetadata())
        {
            if (isClassVariable(*variableDocument, soundDefinitionScope, reference.name))
            {
                readsClassVariables = true;
                continue;
            }
            if (findVisibleProjectVariableDeclaration(
                    *variableDocument,
                    reference.name,
                    soundDefinitionScope,
                    midiVoiceOwner, midiVoiceAncestors) != nullptr)
            {
                continue;
            }
            // A variable typed ?name that nothing declares takes the
            // value the editor's environment gives it as the Sound plays.
            if (!reference.definitionIndex.has_value() && reference.range.offset < auditedSource.size()
                && auditedSource[reference.range.offset] == '?')
            {
                readsEnvironment = true;
                continue;
            }
            const auto diagnostic = talas::expr::Diagnostic{
                talas::expr::Diagnostic::Code::UnknownVariable,
                reference.range,
                "Unknown or out-of-scope variable reference: "
                    + reference.name,
                reference.definitionIndex,
            };
            const auto [diagnosticPath, diagnosticRange] =
                diagnosticLocation(diagnostic);
            appendDiagnostic(
                result,
                diagnosticPath,
                diagnostic.code,
                diagnosticRange,
                diagnostic.message);
        }
        if (result.diagnostics.size() != diagnosticsBeforeVariables
            || result.truncated)
            return;
        // The instance's values arrive as the class expands, where its
        // fields are checked again.
        if (readsClassVariables || readsEnvironment)
            return;
    }

    if (allowOnlyClockedStateful && parsed.expression->isStateful()
        && !parsed.expression->usesTime())
    {
        appendDiagnostic(
            result,
            path,
            talas::expr::Diagnostic::Code::MissingState,
            {sourceOffset, source.size()},
            "Analog Sequencer arrays support clocked stateful expressions only");
        return;
    }

    if (!allowStateful && parsed.expression->isStateful())
    {
        const auto evaluated = talas::expr::evaluate(
            *parsed.expression,
            {},
            talas::expr::EvaluationOptions{
                expectedDomain.value_or(talas::expr::ValueDomain::Scalar)});
        for (const auto& diagnostic : evaluated.diagnostics)
        {
            const auto [diagnosticPath, diagnosticRange] =
                diagnosticLocation(diagnostic);
            appendDiagnostic(
                result,
                diagnosticPath,
                diagnostic.code,
                diagnosticRange,
                diagnostic.message);
        }
        return;
    }

    const auto isConstant = parsed.expression->eventReferences().empty()
        && parsed.expression->variableReferences().empty()
        && parsed.expression->identifiers().empty()
        && !parsed.expression->usesTime();
    if (requireConstant && !isConstant)
    {
        appendDiagnostic(
            result,
            path,
            talas::expr::Diagnostic::Code::UnknownIdentifier,
            {sourceOffset, source.size()},
            "Text expression must be constant");
        return;
    }
    const auto isVariableOnly = variableDocument != nullptr
        && parsed.expression->eventReferences().empty()
        && parsed.expression->identifiers().empty();
    if (!expectedDomain.has_value()
        || (!isConstant && !isVariableOnly))
        return;

    struct VariableResolver
    {
        const ProjectDocument* document = nullptr;
        std::span<const std::string_view> scope;
        const ProjectVariableOwner* midiVoiceOwner = nullptr;
        std::span<const ProjectVariableOwner> midiVoiceAncestors;
    } resolver{variableDocument, soundDefinitionScope, midiVoiceOwner, midiVoiceAncestors};
    talas::expr::EvaluationContext context;
    if (parsed.expression->usesTime())
        context.sampleRate = 1000.0;
    if (isVariableOnly)
    {
        context.user = &resolver;
        context.resolveVariable = [](
                                      void* user,
                                      std::string_view name,
                                      talas::expr::ExpressionValue& value) noexcept {
            const auto* resolver = static_cast<const VariableResolver*>(user);
            const auto* declaration = findVisibleProjectVariableDeclaration(
                *resolver->document,
                name,
                resolver->scope,
                resolver->midiVoiceOwner, resolver->midiVoiceAncestors);
            if (declaration == nullptr)
                return false;
            value = {declaration->initialValue, declaration->domain};
            return std::isfinite(value.value);
        };
    }
    auto evaluated = [&] {
        if (!parsed.expression->isStateful() || isConstant)
        {
            if (midiPitchBound)
                return evaluateMidiPitchBound(*parsed.expression, context);
            return talas::expr::evaluate(
                *parsed.expression,
                context,
                talas::expr::EvaluationOptions{*expectedDomain});
        }
        talas::expr::ExpressionState state;
        return talas::expr::prepareExpressionState(*parsed.expression, state)
            ? talas::expr::evaluateStateful(
                  *parsed.expression,
                  context,
                  state,
                  talas::expr::EvaluationOptions{*expectedDomain})
            : talas::expr::EvaluationResult{};
    }();
    for (const auto& diagnostic : evaluated.diagnostics)
    {
        const auto [diagnosticPath, diagnosticRange] =
            diagnosticLocation(diagnostic);
        appendDiagnostic(
            result,
            diagnosticPath,
            diagnostic.code,
            diagnosticRange,
            diagnostic.message);
    }
    if (evaluated.ok() && !std::isfinite(evaluated.value.value))
    {
        appendDiagnostic(
            result,
            path,
            talas::expr::Diagnostic::Code::NonFiniteResult,
            {sourceOffset, source.size()},
            "Expression result must be finite");
    }
}

void appendExpressionDiagnostics(
    ProjectExpressionAuditResult& result,
    const ProjectParameterExpression& value,
    const std::string& path,
    talas::expr::ExpressionLanguageVersion language,
    const talas::expr::ExpressionDefinitionLibrary& definitions,
    std::optional<talas::expr::ValueDomain> expectedDomain = std::nullopt,
    bool allowStateful = true,
    const ProjectDocument* variableDocument = nullptr,
    std::span<const std::string_view> soundDefinitionScope = {},
    const ProjectVariableOwner* midiVoiceOwner = nullptr,
    bool midiPitchBound = false,
    bool allowVoiceTemplate = false,
    std::uint16_t replicationNumberVoices = 0,
    std::span<const ProjectVariableOwner> midiVoiceAncestors = {})
{
    if (!value.expression.has_value() || result.truncated)
        return;
    // A Smalltalk program stands for the expression it gives as the Sound
    // compiles.
    // It rounds as the project does.
    const auto program = smalltalk::fieldProgramText(*value.expression, true, definitions.rounding);
    appendParsedExpressionDiagnostics(
        result,
        program.has_value() ? *program : *value.expression,
        path + ".expression",
        language,
        definitions,
        expectedDomain,
        0,
        false,
        allowStateful,
        variableDocument,
        soundDefinitionScope,
        midiVoiceOwner,
        midiPitchBound,
        allowVoiceTemplate,
        replicationNumberVoices, false, midiVoiceAncestors);
}

void appendExpressionDiagnostics(
    ProjectExpressionAuditResult& result,
    std::string_view expression,
    const std::string& path,
    talas::expr::ExpressionLanguageVersion language,
    const talas::expr::ExpressionDefinitionLibrary& definitions,
    std::optional<talas::expr::ValueDomain> expectedDomain = std::nullopt,
    bool allowStateful = true,
    const ProjectDocument* variableDocument = nullptr,
    std::span<const std::string_view> soundDefinitionScope = {},
    const ProjectVariableOwner* midiVoiceOwner = nullptr,
    bool midiPitchBound = false,
    bool allowVoiceTemplate = false,
    std::uint16_t replicationNumberVoices = 0,
    std::span<const ProjectVariableOwner> midiVoiceAncestors = {})
{
    if (expression.empty() || result.truncated)
        return;
    appendParsedExpressionDiagnostics(
        result,
        expression,
        path,
        language,
        definitions,
        expectedDomain,
        0,
        false,
        allowStateful,
        variableDocument,
        soundDefinitionScope,
        midiVoiceOwner,
        midiPitchBound,
        allowVoiceTemplate,
        replicationNumberVoices, false, midiVoiceAncestors);
}

[[nodiscard]] bool isAsciiSpace(char value) noexcept
{
    return std::isspace(static_cast<unsigned char>(value)) != 0;
}

enum class EmbeddedTextContract
{
    None,
    BraceList,
    OptionalScalar,
    OptionalEventScalar,
    EventMidiBytes,
    ScalarList,
    DelimitedScalarList,
};

[[nodiscard]] EmbeddedTextContract embeddedTextContract(
    std::string_view nodeTypeId,
    std::string_view parameterId)
{
    if ((nodeTypeId == "talas.multislopeFunctionGenerator"
         && (parameterId == "timePoints" || parameterId == "slopes"))
        || (nodeTypeId == "talas.multisegmentEnvelope"
            && (parameterId == "durations" || parameterId == "breakPoints"))
        || (nodeTypeId == "talas.warpedTimeIndex"
            && (parameterId == "idealTimePoints0"
                || parameterId == "idealTimePoints1"
                || parameterId == "currentTimePoints"))
        || (nodeTypeId == "talas.analogSequencer"
            && (parameterId == "durations"
                || parameterId == "dutyCycles"
                || parameterId == "pitches"
                || parameterId == "velocities"
                || parameterId == "keyDowns"
                || parameterId == "keyPitches"
                || parameterId == "keyVelocities"
                || parameterId == "keyTimbres"
                || parameterId == "extraValues"))
        || (nodeTypeId == "talas.graphicalEnvelope"
            && parameterId == "envelope"))
    {
        return EmbeddedTextContract::BraceList;
    }
    if (nodeTypeId == "talas.midiOutputController"
        && parameterId == "value")
    {
        return EmbeddedTextContract::OptionalScalar;
    }
    if ((nodeTypeId == "talas.soundToGlobalController"
         || nodeTypeId == "talas.triggeredSoundToGlobalController")
        && parameterId == "value")
    {
        return EmbeddedTextContract::OptionalEventScalar;
    }
    if (nodeTypeId == "talas.midiOutputEventInBytes" && parameterId == "bytes")
        return EmbeddedTextContract::EventMidiBytes;
    if (nodeTypeId == "talas.inputOutputCharacteristic"
        && (parameterId == "inValues" || parameterId == "outValues"))
    {
        return EmbeddedTextContract::DelimitedScalarList;
    }
    if ((nodeTypeId == "talas.waveshaper" || nodeTypeId == "talas.firFilter")
        && parameterId == "coefficients")
    {
        return EmbeddedTextContract::ScalarList;
    }
    return EmbeddedTextContract::None;
}

[[nodiscard]] std::optional<talas::expr::ValueDomain> braceTextDomain(
    std::string_view nodeTypeId,
    std::string_view parameterId,
    std::size_t expressionIndex)
{
    using talas::expr::ValueDomain;
    if (nodeTypeId == "talas.multislopeFunctionGenerator")
    {
        if (parameterId == "timePoints")
            return ValueDomain::Seconds;
        if (parameterId == "slopes")
            return ValueDomain::Scalar;
    }
    if (nodeTypeId == "talas.multisegmentEnvelope")
    {
        if (parameterId == "durations")
            return ValueDomain::Seconds;
        if (parameterId == "breakPoints")
            return ValueDomain::Scalar;
    }
    if (nodeTypeId == "talas.warpedTimeIndex"
        && (parameterId == "idealTimePoints0"
            || parameterId == "idealTimePoints1"
            || parameterId == "currentTimePoints"))
    {
        return ValueDomain::Seconds;
    }
    if (nodeTypeId == "talas.analogSequencer")
    {
        if (parameterId == "durations")
            return ValueDomain::Seconds;
        if (parameterId == "pitches" || parameterId == "keyPitches")
            return ValueDomain::NoteNumber;
        if (parameterId == "dutyCycles" || parameterId == "velocities"
            || parameterId == "keyDowns"
            || parameterId == "keyVelocities"
            || parameterId == "keyTimbres")
            return ValueDomain::Scalar;
    }
    if (nodeTypeId == "talas.graphicalEnvelope" && parameterId == "envelope")
    {
        return (expressionIndex % 2U) == 0U
            ? ValueDomain::Seconds
            : ValueDomain::Scalar;
    }
    return std::nullopt;
}

void appendBraceTextDiagnostics(
    ProjectExpressionAuditResult& result,
    std::string_view text,
    const std::string& path,
    std::string_view nodeTypeId,
    std::string_view parameterId,
    talas::expr::ExpressionLanguageVersion language,
    const talas::expr::ExpressionDefinitionLibrary& definitions)
{
    if (result.truncated)
    {
        return;
    }

    std::size_t offset = 0;
    std::size_t expressionIndex = 0;
    while (offset < text.size() && !result.truncated)
    {
        while (offset < text.size()
               && isAsciiSpace(text[offset]))
        {
            ++offset;
        }
        if (offset >= text.size())
            break;
        // A GraphicalEnvelope's loop marks
        // follow a breakpoint's value; the runtime checks their places.
        if (nodeTypeId == "talas.graphicalEnvelope" && parameterId == "envelope")
        {
            const auto rest = text.substr(offset);
            const auto mark = rest.starts_with("loopStart") ? std::size_t{9}
                : rest.starts_with("loopEnd")                ? std::size_t{7}
                                                             : std::size_t{0};
            if (mark != 0 && (rest.size() == mark || isAsciiSpace(rest[mark]) || rest[mark] == '{'))
            {
                offset += mark;
                continue;
            }
        }
        if (text[offset] != '{')
        {
            appendDiagnostic(
                result,
                path,
                talas::expr::Diagnostic::Code::UnexpectedCharacter,
                {offset, 1},
                "Text parameter must contain brace-wrapped expressions");
            return;
        }
        const auto close = text.find('}', offset + 1U);
        if (close == std::string_view::npos)
        {
            appendDiagnostic(
                result,
                path,
                talas::expr::Diagnostic::Code::ExpectedRightParen,
                {offset, text.size() - offset},
                "Text parameter contains an unterminated expression");
            return;
        }
        auto begin = offset + 1U;
        auto end = close;
        while (begin < end
               && isAsciiSpace(text[begin]))
        {
            ++begin;
        }
        while (end > begin
               && isAsciiSpace(text[end - 1U]))
        {
            --end;
        }
        if (begin == end)
        {
            appendDiagnostic(
                result,
                path,
                talas::expr::Diagnostic::Code::EmptyExpression,
                {begin, 0},
                "Text parameter contains an empty expression");
            return;
        }
        appendParsedExpressionDiagnostics(
            result,
            text.substr(begin, end - begin),
            path,
            language,
            definitions,
            braceTextDomain(nodeTypeId, parameterId, expressionIndex),
            begin,
            nodeTypeId != "talas.analogSequencer",
            true,
            nullptr,
            {},
            nullptr,
            false,
            false,
            0,
            nodeTypeId == "talas.analogSequencer");
        ++expressionIndex;
        offset = close + 1U;
    }
    if (expressionIndex == 0 && !result.truncated)
    {
        appendDiagnostic(
            result,
            path,
            talas::expr::Diagnostic::Code::EmptyExpression,
            {0, 0},
            "Text parameter must contain at least one expression");
    }
}

void appendOptionalScalarTextDiagnostics(
    ProjectExpressionAuditResult& result,
    std::string_view text,
    const std::string& path,
    talas::expr::ExpressionLanguageVersion language,
    const talas::expr::ExpressionDefinitionLibrary& definitions,
    bool allowEventValues)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && isAsciiSpace(text[begin]))
        ++begin;
    while (end > begin && isAsciiSpace(text[end - 1U]))
        --end;
    if (begin == end)
        return;
    if (text[begin] == '{' && text[end - 1U] == '}')
    {
        ++begin;
        --end;
        while (begin < end && isAsciiSpace(text[begin]))
            ++begin;
        while (end > begin && isAsciiSpace(text[end - 1U]))
            --end;
    }
    if (begin == end)
    {
        appendDiagnostic(
            result,
            path,
            talas::expr::Diagnostic::Code::EmptyExpression,
            {begin, 0},
            "Text parameter contains an empty expression");
        return;
    }
    appendParsedExpressionDiagnostics(
        result,
        text.substr(begin, end - begin),
        path,
        language,
        definitions,
        talas::expr::ValueDomain::Scalar,
        begin,
        !allowEventValues);
    if (!allowEventValues || result.truncated)
        return;

    const auto source = text.substr(begin, end - begin);
    const auto parsed = talas::expr::parse(
        source,
        language,
        definitions);
    if (!parsed.ok() || !parsed.expression.has_value())
        return;
    if (!parsed.expression->variableReferences().empty())
    {
        appendDiagnostic(
            result,
            path,
            talas::expr::Diagnostic::Code::UnknownVariable,
            {begin, source.size()},
            "Dynamic embedded Scalar text does not support project-variable references");
    }
    if (!parsed.expression->identifiers().empty())
    {
        appendDiagnostic(
            result,
            path,
            talas::expr::Diagnostic::Code::UnknownIdentifier,
            {begin, source.size()},
            "Dynamic embedded Scalar text does not support runtime identifiers");
    }
}

void appendEventMidiByteTextDiagnostics(
    ProjectExpressionAuditResult& result,
    std::string_view text,
    const std::string& path,
    talas::expr::ExpressionLanguageVersion language,
    const talas::expr::ExpressionDefinitionLibrary& definitions)
{
    std::size_t offset = 0;
    while (offset < text.size() && !result.truncated)
    {
        while (offset < text.size() && isAsciiSpace(text[offset]))
            ++offset;
        if (offset >= text.size())
            return;
        if (text[offset] != '{')
        {
            while (offset < text.size() && !isAsciiSpace(text[offset]))
                ++offset;
            continue;
        }
        const auto begin = offset + 1U;
        const auto end = text.find('}', begin);
        if (end == std::string_view::npos)
        {
            appendDiagnostic(
                result,
                path,
                talas::expr::Diagnostic::Code::ExpectedRightParen,
                {offset, text.size() - offset},
                "MIDI byte text contains an unterminated expression");
            return;
        }
        const auto source = text.substr(begin, end - begin);
        const auto parsed = talas::expr::parse(source, language, definitions);
        for (const auto& diagnostic : parsed.diagnostics)
        {
            appendDiagnostic(
                result,
                path,
                diagnostic.code,
                {begin + diagnostic.range.offset, diagnostic.range.length},
                diagnostic.message);
        }
        if (parsed.ok() && !result.truncated)
        {
            if (parsed.expression->isStateful())
            {
                const auto evaluated = talas::expr::evaluate(
                    *parsed.expression,
                    {},
                    {talas::expr::ValueDomain::Scalar});
                for (const auto& diagnostic : evaluated.diagnostics)
                {
                    appendDiagnostic(
                        result,
                        path,
                        diagnostic.code,
                        {begin + diagnostic.range.offset,
                         diagnostic.range.length},
                        diagnostic.message);
                }
            }
            else if (!parsed.expression->identifiers().empty())
            {
                appendDiagnostic(
                    result,
                    path,
                    talas::expr::Diagnostic::Code::UnknownIdentifier,
                    {begin, source.size()},
                    "MIDI byte expression cannot contain identifiers");
            }
            else if (!parsed.expression->variableReferences().empty())
            {
                for (const auto& reference
                     : parsed.expression->variableReferenceMetadata())
                {
                    auto diagnosticPath = path;
                    auto diagnosticRange = talas::expr::SourceRange{
                        begin + reference.range.offset,
                        reference.range.length,
                    };
                    if (reference.definitionIndex.has_value()
                        && *reference.definitionIndex
                            < definitions.definitions.size())
                    {
                        diagnosticPath = "$.expressionDefinitions["
                            + definitions.definitions[
                                  *reference.definitionIndex]
                                  .name
                            + "].source";
                        diagnosticRange = reference.range;
                    }
                    appendDiagnostic(
                        result,
                        diagnosticPath,
                        talas::expr::Diagnostic::Code::UnknownVariable,
                        diagnosticRange,
                        "MIDI byte expression cannot contain project variable @"
                            + reference.name);
                }
            }
            else if (parsed.expression->eventReferences().empty())
            {
                const auto evaluated = talas::expr::evaluate(
                    *parsed.expression,
                    {},
                    {talas::expr::ValueDomain::Scalar});
                for (const auto& diagnostic : evaluated.diagnostics)
                {
                    appendDiagnostic(
                        result,
                        path,
                        diagnostic.code,
                        {begin + diagnostic.range.offset,
                         diagnostic.range.length},
                        diagnostic.message);
                }
            }
        }
        offset = end + 1U;
        if (offset < text.size() && !isAsciiSpace(text[offset]))
        {
            appendDiagnostic(
                result,
                path,
                talas::expr::Diagnostic::Code::TrailingInput,
                {offset, 1},
                "MIDI byte expression must be followed by whitespace");
            return;
        }
    }
}

[[nodiscard]] bool isScalarListDelimiter(
    char value,
    bool allowDelimiters) noexcept
{
    return isAsciiSpace(value)
        || (allowDelimiters
            && (value == ',' || value == ';' || value == '[' || value == ']'
                || value == '(' || value == ')'));
}

void appendScalarListTextDiagnostics(
    ProjectExpressionAuditResult& result,
    std::string_view text,
    const std::string& path,
    talas::expr::ExpressionLanguageVersion language,
    const talas::expr::ExpressionDefinitionLibrary& definitions,
    bool allowDelimiters)
{
    std::size_t offset = 0;
    std::size_t expressionCount = 0;
    while (offset < text.size() && !result.truncated)
    {
        while (offset < text.size()
               && isScalarListDelimiter(text[offset], allowDelimiters))
        {
            ++offset;
        }
        if (offset >= text.size())
            break;
        std::size_t begin = offset;
        std::size_t end = offset;
        if (text[offset] == '{')
        {
            begin = offset + 1U;
            end = text.find('}', begin);
            if (end == std::string_view::npos)
            {
                appendDiagnostic(
                    result,
                    path,
                    talas::expr::Diagnostic::Code::ExpectedRightParen,
                    {offset, text.size() - offset},
                    "Scalar list contains an unterminated expression");
                return;
            }
            offset = end + 1U;
        }
        else
        {
            while (offset < text.size()
                   && !isScalarListDelimiter(text[offset], allowDelimiters))
            {
                ++offset;
            }
            end = offset;
        }
        appendParsedExpressionDiagnostics(
            result,
            text.substr(begin, end - begin),
            path,
            language,
            definitions,
            talas::expr::ValueDomain::Scalar,
            begin,
            true);
        ++expressionCount;
    }
    if (expressionCount == 0 && !result.truncated)
    {
        appendDiagnostic(
            result,
            path,
            talas::expr::Diagnostic::Code::EmptyExpression,
            {0, 0},
            "Scalar list must contain at least one expression");
    }
}

void appendEmbeddedTextDiagnostics(
    ProjectExpressionAuditResult& result,
    std::string_view text,
    const std::string& path,
    std::string_view nodeTypeId,
    std::string_view parameterId,
    talas::expr::ExpressionLanguageVersion language,
    const talas::expr::ExpressionDefinitionLibrary& definitions)
{
    // A braced Smalltalk program stands for the numbers it gives as the
    // Sound compiles.
    auto numbers = smalltalk::listFieldNumbers(text, true, definitions.rounding);
    if (!numbers.has_value())
        numbers = smalltalk::fieldProgramText(text, true, definitions.rounding);
    if (numbers.has_value())
        text = *numbers;
    if (nodeTypeId == "talas.analogSequencer"
        && parameterId == "generatedIndex")
    {
        if (!talas::core::parseAnalogSequencerGeneratedIndex(text).has_value())
        {
            appendDiagnostic(
                result,
                path,
                talas::expr::Diagnostic::Code::UnexpectedCharacter,
                {0, text.size()},
                "GeneratedIndex requires a blank value or one non-reserved !Name target");
        }
        return;
    }
    if (nodeTypeId == "talas.analogSequencer" && parameterId == "extraValues")
    {
        const auto arrays = talas::core::parseAnalogSequencerExtraValues(text);
        if (!arrays)
        {
            appendDiagnostic(result, path, talas::expr::Diagnostic::Code::UnexpectedCharacter,
                {0, text.size()}, "ExtraValues requires distinct named #(!Event value ...) arrays with 1 to 256 finite members");
            return;
        }
        for (const auto& array : *arrays)
            for (const auto& expression : array.expressions)
                appendParsedExpressionDiagnostics(result, expression, path, language,
                    definitions, talas::expr::ValueDomain::Scalar, 0, false, true,
                    nullptr, {}, nullptr, false, false, 0, true);
        return;
    }
    if (nodeTypeId == "talas.analogSequencer"
        && (parameterId == "durations" || parameterId == "dutyCycles"
            || parameterId == "pitches"
            || parameterId == "velocities" || parameterId == "keyDowns"
            || parameterId == "keyPitches"
            || parameterId == "keyVelocities"
            || parameterId == "keyTimbres"))
    {
        auto literal = text;
        while (!literal.empty() && isAsciiSpace(literal.front()))
            literal.remove_prefix(1);
        while (!literal.empty() && isAsciiSpace(literal.back()))
            literal.remove_suffix(1);
        // Empty pitch/key arrays select the other sequencer mode. Runtime
        // preparation checks the required arrays after selecting that mode.
        if (literal.empty() && parameterId != "durations"
            && parameterId != "dutyCycles")
            return;
        const auto array = talas::core::parseAnalogSequencerArray(text);
        if (!array)
        {
            appendDiagnostic(result, path,
                talas::expr::Diagnostic::Code::UnexpectedCharacter,
                {0, text.size()},
                "Analog Sequencer array requires 1 to 256 numeric, reference or brace-expression members");
            return;
        }
        if (!array->numericLiteral)
            for (const auto& member : array->members)
            {
                // Structural parsing already checked finite bare numbers;
                // those use the field's native domain like #(...) literals.
                if (member.implicitDomainNumber) continue;
                appendParsedExpressionDiagnostics(result, member.source, path,
                    language, definitions, braceTextDomain(nodeTypeId, parameterId, 0),
                    member.offset, false, true, nullptr, {}, nullptr, false, false, 0, true);
            }
        return;
    }
    if (nodeTypeId == "talas.inputOutputCharacteristic"
        && (parameterId == "inValues" || parameterId == "outValues")
        && talas::core::val_scalar_array::beginsArray(text))
    {
        if (!talas::core::val_scalar_array::parse(text).has_value())
        {
            appendDiagnostic(
                result, path, talas::expr::Diagnostic::Code::UnexpectedCharacter,
                {0, text.size()},
                "Val breakpoint array requires 2 to 32 finite scalar numbers or !EventValue members");
        }
        return;
    }
    switch (embeddedTextContract(nodeTypeId, parameterId))
    {
    case EmbeddedTextContract::None:
        return;
    case EmbeddedTextContract::BraceList:
        appendBraceTextDiagnostics(
            result, text, path, nodeTypeId, parameterId, language, definitions);
        return;
    case EmbeddedTextContract::OptionalScalar:
        appendOptionalScalarTextDiagnostics(
            result, text, path, language, definitions, false);
        return;
    case EmbeddedTextContract::OptionalEventScalar:
        appendOptionalScalarTextDiagnostics(
            result, text, path, language, definitions, true);
        return;
    case EmbeddedTextContract::EventMidiBytes:
        appendEventMidiByteTextDiagnostics(
            result, text, path, language, definitions);
        return;
    case EmbeddedTextContract::ScalarList:
        appendScalarListTextDiagnostics(
            result, text, path, language, definitions, false);
        return;
    case EmbeddedTextContract::DelimitedScalarList:
        appendScalarListTextDiagnostics(
            result, text, path, language, definitions, true);
        return;
    }
}

void appendBoundSoundTextDiagnostics(
    ProjectExpressionAuditResult& result,
    const ProjectDocument& document,
    std::string_view definitionId,
    std::string_view exposedParameterId,
    std::string_view text,
    const std::string& path,
    talas::expr::ExpressionLanguageVersion language,
    const talas::expr::ExpressionDefinitionLibrary& definitions,
    std::size_t depth = 0)
{
    if (depth > document.soundDefinitions.size() || result.truncated)
        return;
    const auto definition = std::ranges::find(
        document.soundDefinitions,
        definitionId,
        &ProjectSoundDefinition::definitionId);
    if (definition == document.soundDefinitions.end())
        return;
    const auto exposed = std::ranges::find_if(
        definition->exposedParameters,
        [&](const auto& candidate) {
            return candidate.parameterId == exposedParameterId
                || std::ranges::find(
                       candidate.aliases, exposedParameterId)
                    != candidate.aliases.end();
        });
    if (exposed == definition->exposedParameters.end())
        return;
    for (const auto& binding : exposed->bindings)
    {
        if (binding.target != ProjectSoundParameterTarget::NodeParameter
            || result.truncated)
        {
            continue;
        }
        const auto node = std::ranges::find(
            definition->graph.nodes,
            binding.nodeId,
            &ProjectNode::instanceId);
        if (node == definition->graph.nodes.end())
            continue;
        if (node->soundDefinitionId.has_value())
        {
            appendBoundSoundTextDiagnostics(
                result,
                document,
                *node->soundDefinitionId,
                binding.parameterId,
                text,
                path,
                language,
                definitions,
                depth + 1U);
        }
        else
        {
            appendEmbeddedTextDiagnostics(
                result,
                text,
                path,
                node->typeId,
                binding.parameterId,
                language,
                definitions);
        }
    }
}

[[nodiscard]] const ProjectSoundExposedParameter* findExposedParameter(
    const ProjectSoundDefinition& definition,
    std::string_view parameterId)
{
    const auto found = std::ranges::find_if(
        definition.exposedParameters,
        [&](const auto& candidate) {
            return candidate.parameterId == parameterId
                || std::ranges::find(candidate.aliases, parameterId)
                    != candidate.aliases.end();
        });
    return found == definition.exposedParameters.end() ? nullptr : &*found;
}

[[nodiscard]] bool soundBindingsAllowStatefulExpressions(
    const ProjectDocument& document,
    const ProjectSoundDefinition& definition,
    const ProjectSoundExposedParameter& exposed,
    std::size_t depth = 0)
{
    if (depth >= document.soundDefinitions.size()
        || exposed.bindings.empty())
    {
        return false;
    }
    for (const auto& binding : exposed.bindings)
    {
        if (binding.target == ProjectSoundParameterTarget::EventValue)
            return false;
        const auto node = std::ranges::find(
            definition.graph.nodes,
            binding.nodeId,
            &ProjectNode::instanceId);
        if (node == definition.graph.nodes.end())
            return false;
        if (!node->soundDefinitionId.has_value())
            continue;
        const auto nestedDefinition = std::ranges::find(
            document.soundDefinitions,
            *node->soundDefinitionId,
            &ProjectSoundDefinition::definitionId);
        if (nestedDefinition == document.soundDefinitions.end())
            return false;
        const auto* nestedExposed =
            findExposedParameter(*nestedDefinition, binding.parameterId);
        if (nestedExposed == nullptr
            || !soundBindingsAllowStatefulExpressions(
                document,
                *nestedDefinition,
                *nestedExposed,
                depth + 1U))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool soundBindingsAllowStatefulExpressions(
    const ProjectDocument& document,
    std::string_view definitionId,
    std::string_view exposedParameterId)
{
    const auto definition = std::ranges::find(
        document.soundDefinitions,
        definitionId,
        &ProjectSoundDefinition::definitionId);
    if (definition == document.soundDefinitions.end())
        return false;
    const auto* exposed = findExposedParameter(*definition, exposedParameterId);
    return exposed != nullptr
        && soundBindingsAllowStatefulExpressions(
            document, *definition, *exposed);
}

struct MidiVoiceOwnerContext
{
    ProjectVariableOwner owner;
    std::vector<ProjectVariableOwner> ancestors;
};
using MidiVoiceOwnersByNode =
    std::unordered_map<std::string, MidiVoiceOwnerContext>;

[[nodiscard]] MidiVoiceOwnersByNode midiVoiceOwnersByNode(
    const ProjectGraph& graph,
    std::string_view definitionId)
{
    MidiVoiceOwnersByNode result;
    const auto islands = discoverProjectMidiVoiceInputHierarchy(graph);
    if (!islands.ok()) return result;
    result.reserve(graph.nodes.size());
    for (const auto& island : islands.islands)
    {
        MidiVoiceOwnerContext context;
        context.owner = {ProjectVariableOwnerKind::MidiVoice,
            std::string{definitionId}, island.ownerNodeId};
        auto parent = island.parentOwnerNodeId;
        while (!parent.empty())
        {
            context.ancestors.push_back({ProjectVariableOwnerKind::MidiVoice,
                std::string{definitionId}, parent});
            const auto ancestor = std::ranges::find(islands.islands, parent,
                &ProjectMidiVoiceLocalIsland::ownerNodeId);
            if (ancestor == islands.islands.end()) return {};
            parent = ancestor->parentOwnerNodeId;
        }
        for (const auto& nodeId : island.nodeIds)
            result.emplace(nodeId, context);
    }
    return result;
}

[[nodiscard]] std::unordered_map<std::string, std::uint16_t>
replicatorVoiceCountsByNode(
    const ProjectDocument& document,
    const ProjectGraph& graph,
    talas::expr::ExpressionLanguageVersion language)
{
    std::unordered_map<std::string, std::vector<std::string>> incoming;
    for (const auto& edge : graph.connections)
        incoming[edge.toNode].push_back(edge.fromNode);
    std::vector<const ProjectNode*> wrappers;
    for (const auto& wrapper : graph.nodes)
        if (wrapper.typeId != "talas.replicator"
            && wrapper.typeId != "talas.stereoizer"
            && wrapper.typeId != "talas.surroundifer")
            continue;
        else
            wrappers.push_back(&wrapper);
    const auto closureSize = [&](const ProjectNode* wrapper) {
        std::unordered_set<std::string> visited;
        std::vector<std::string> pending{wrapper->instanceId};
        for (std::size_t index = 0; index < pending.size(); ++index)
        {
            const auto id = pending[index];
            if (!visited.insert(id).second)
                continue;
            const auto found = incoming.find(id);
            if (found != incoming.end())
                pending.insert(pending.end(),
                    found->second.begin(), found->second.end());
        }
        return visited.size();
    };
    std::ranges::stable_sort(wrappers,
        [&](const auto* left, const auto* right) {
            return closureSize(left) < closureSize(right);
        });

    std::unordered_map<std::string, std::uint16_t> owned;
    for (const auto* wrapper : wrappers)
    {
        std::uint16_t numberVoices = wrapper->typeId == "talas.stereoizer"
            ? std::uint16_t{2}
            : wrapper->typeId == "talas.surroundifer"
                ? std::uint16_t{6} : std::uint16_t{1};
        std::unordered_set<std::string> sharedRoots;
        if (wrapper->typeId == "talas.stereoizer"
            || wrapper->typeId == "talas.surroundifer")
        {
            const auto field = std::ranges::find(
                wrapper->parameters, std::string_view{"sharedSounds"},
                &ProjectParameterExpression::parameterId);
            if (field != wrapper->parameters.end()
                && field->text.has_value() && !field->text->empty())
                if (const auto listed =
                        talas::core::parseBoundedStaticResourceList(*field->text))
                    sharedRoots.insert(listed->begin(), listed->end());
            for (const auto& edge : graph.connections)
                if (edge.toNode == wrapper->instanceId
                    && edge.toPort == "rightIn")
                    sharedRoots.insert(edge.fromNode);
        }
        if (wrapper->typeId == "talas.replicator")
        {
            const auto countParameter = std::ranges::find(
                wrapper->parameters, std::string_view{"number"},
                &ProjectParameterExpression::parameterId);
            const auto source = countParameter == wrapper->parameters.end()
                ? std::string{"1"}
                : countParameter->expression.value_or("");
            const auto parsed = talas::expr::parse(
                source, language,
                document.expressionDefinitions);
            if (parsed.ok())
            {
                const auto evaluated = talas::expr::evaluate(
                    *parsed.expression, {},
                    {talas::expr::ValueDomain::Scalar});
                if (evaluated.ok()
                    && std::isfinite(evaluated.value.value)
                    && evaluated.value.value >= 1.0
                    && evaluated.value.value <= 64.0
                    && std::floor(evaluated.value.value)
                        == evaluated.value.value)
                    numberVoices = static_cast<std::uint16_t>(
                        evaluated.value.value);
            }
        }
        std::vector<std::string> pending;
        for (const auto& edge : graph.connections)
            if (edge.toNode == wrapper->instanceId
                && edge.toPort == "audioIn")
                pending.push_back(edge.fromNode);
        for (std::size_t index = 0; index < pending.size(); ++index)
        {
            const auto id = pending[index];
            if (sharedRoots.contains(id))
                continue;
            if (!owned.emplace(id, numberVoices).second)
                continue;
            const auto found = incoming.find(id);
            if (found != incoming.end())
                pending.insert(pending.end(),
                    found->second.begin(), found->second.end());
        }
    }
    return owned;
}

void auditGraph(
    ProjectExpressionAuditResult& result,
    const ProjectDocument& document,
    const ProjectGraph& graph,
    const std::string& path,
    const talas::core::NodeRegistry* registry,
    talas::expr::ExpressionLanguageVersion language,
    const talas::expr::ExpressionDefinitionLibrary& definitions,
    std::span<const std::string_view> soundDefinitionScope)
{
    const auto voiceOwners = midiVoiceOwnersByNode(
        graph,
        soundDefinitionScope.empty()
            ? std::string_view{}
            : soundDefinitionScope.back());
    const auto replicationOwners = replicatorVoiceCountsByNode(
        document, graph, language);
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size() && !result.truncated;
         ++nodeIndex)
    {
        const auto& node = graph.nodes[nodeIndex];
        const auto nodePath = path + ".nodes["
            + std::to_string(nodeIndex) + "]";
        const auto descriptor = registry != nullptr
            ? describeProjectNode(document, node, *registry)
            : std::optional<talas::core::NodeTypeDescriptor>{};
        const auto voiceOwner = voiceOwners.find(node.instanceId);
        const auto* midiVoiceOwner = voiceOwner == voiceOwners.end()
            ? nullptr
            : &voiceOwner->second.owner;
        const auto replicationOwner = replicationOwners.find(node.instanceId);
        const auto replicationNumberVoices =
            node.typeId != "talas.replicator"
                && node.typeId != "talas.stereoizer"
                && node.typeId != "talas.surroundifer"
                && replicationOwner != replicationOwners.end()
            ? replicationOwner->second : std::uint16_t{0};
        for (std::size_t parameterIndex = 0;
             parameterIndex < node.parameters.size()
                 && !result.truncated;
             ++parameterIndex)
        {
            const auto& value = node.parameters[parameterIndex];
            const auto* parameterDescriptor = descriptor.has_value()
                ? [&]() -> const talas::core::ParameterDescriptor* {
                      const auto found = std::ranges::find(
                          descriptor->parameters,
                          value.parameterId,
                          &talas::core::ParameterDescriptor::id);
                      return found == descriptor->parameters.end()
                          ? nullptr
                          : &*found;
                  }()
                : nullptr;
            auto parameterScope = std::vector<std::string_view>{
                soundDefinitionScope.begin(),
                soundDefinitionScope.end()};
            appendExpressionDiagnostics(
                result,
                value,
                nodePath + ".parameters["
                    + std::to_string(parameterIndex) + "]",
                language,
                definitions,
                parameterDescriptor != nullptr
                    ? std::optional{parameterDescriptor->domain}
                    : std::nullopt,
                !node.soundDefinitionId.has_value()
                    || soundBindingsAllowStatefulExpressions(
                        document,
                        *node.soundDefinitionId,
                        value.parameterId),
                &document,
                parameterScope,
                midiVoiceOwner,
                isMidiPitchBound(node.typeId, value.parameterId),
                node.typeId != "talas.midiVoice"
                    && (!soundDefinitionScope.empty() || node.soundDefinitionId.has_value()),
                replicationNumberVoices,
                voiceOwner == voiceOwners.end() ? std::span<const ProjectVariableOwner>{}
                    : voiceOwner->second.ancestors);
            if (value.text.has_value())
            {
                const auto valuePath = nodePath + ".parameters["
                    + std::to_string(parameterIndex) + "].text";
                if (node.soundDefinitionId.has_value())
                {
                    appendBoundSoundTextDiagnostics(
                        result,
                        document,
                        *node.soundDefinitionId,
                        value.parameterId,
                        *value.text,
                        valuePath,
                        language,
                        definitions);
                }
                else
                {
                    appendEmbeddedTextDiagnostics(
                        result,
                        *value.text,
                        valuePath,
                        node.typeId,
                        value.parameterId,
                        language,
                        definitions);
                }
            }
        }
        if (registry == nullptr || node.soundDefinitionId.has_value())
            continue;
        if (!descriptor.has_value())
            continue;
        for (const auto& parameter : descriptor->parameters)
        {
            const auto overridden = std::ranges::any_of(
                node.parameters,
                [&](const auto& value) {
                    return value.parameterId == parameter.id;
                });
            if (overridden)
                continue;
            if (parameter.kind == talas::core::ParameterKind::Expression
                && !parameter.defaultExpression.empty())
            {
                appendExpressionDiagnostics(
                    result,
                    parameter.defaultExpression,
                    nodePath + ".parameters." + parameter.id + ".default",
                    language,
                    definitions,
                    parameter.domain,
                    true,
                    &document,
                    soundDefinitionScope,
                    midiVoiceOwner,
                    isMidiPitchBound(node.typeId, parameter.id),
                    node.typeId != "talas.midiVoice" && !soundDefinitionScope.empty(),
                    replicationNumberVoices,
                    voiceOwner == voiceOwners.end() ? std::span<const ProjectVariableOwner>{}
                        : voiceOwner->second.ancestors);
            }
            else if (parameter.kind == talas::core::ParameterKind::Text
                     && !parameter.defaultText.empty())
            {
                appendEmbeddedTextDiagnostics(
                    result,
                    parameter.defaultText,
                    nodePath + ".parameters." + parameter.id + ".default.text",
                    node.typeId,
                    parameter.id,
                    language,
                    definitions);
            }
        }
    }
}

ProjectExpressionAuditResult auditProjectExpressionsImpl(
    const ProjectDocument& document,
    const talas::core::NodeRegistry* registry,
    talas::expr::ExpressionLanguageVersion language)
{
    ProjectExpressionAuditResult result;
    result.diagnostics.reserve(maximumProjectExpressionDiagnostics);
    const auto library = talas::expr::parse(
        "0",
        language,
        document.expressionDefinitions);
    for (const auto& diagnostic : library.diagnostics)
    {
        const auto path = diagnostic.definitionIndex.has_value()
                && *diagnostic.definitionIndex
                    < document.expressionDefinitions.definitions.size()
            ? "$.expressionDefinitions["
                + document.expressionDefinitions
                      .definitions[*diagnostic.definitionIndex]
                      .name
                + "].source"
            : "$.expressionDefinitions";
        appendDiagnostic(
            result,
            path,
            diagnostic.code,
            diagnostic.range,
            diagnostic.message);
    }
    if (!library.ok() || result.truncated)
        return result;
    auditGraph(
        result,
        document,
        document.graph,
        "$.graph",
        registry,
        language,
        document.expressionDefinitions,
        {});
    for (std::size_t definitionIndex = 0;
         definitionIndex < document.soundDefinitions.size()
             && !result.truncated;
         ++definitionIndex)
    {
        const auto& definition =
            document.soundDefinitions[definitionIndex];
        const auto definitionPath = "$.soundDefinitions["
            + std::to_string(definitionIndex) + "]";
        const std::array<std::string_view, 1> definitionScope{
            definition.definitionId};
        auditGraph(
            result,
            document,
            definition.graph,
            definitionPath + ".graph",
            registry,
            language,
            document.expressionDefinitions,
            definitionScope);
        for (std::size_t parameterIndex = 0;
             parameterIndex < definition.exposedParameters.size()
                 && !result.truncated;
             ++parameterIndex)
        {
            const auto& exposed =
                definition.exposedParameters[parameterIndex];
            const auto exposedPath = definitionPath + ".parameters["
                + std::to_string(parameterIndex) + "].default";
            appendExpressionDiagnostics(
                result,
                exposed.defaultValue,
                exposedPath,
                language,
                document.expressionDefinitions,
                exposed.domain,
                soundBindingsAllowStatefulExpressions(
                    document, definition, exposed),
                &document,
                definitionScope,
                nullptr,
                false,
                true);
            if (exposed.defaultValue.text.has_value())
            {
                appendBoundSoundTextDiagnostics(
                    result,
                    document,
                    definition.definitionId,
                    exposed.parameterId,
                    *exposed.defaultValue.text,
                    exposedPath + ".text",
                    language,
                    document.expressionDefinitions);
            }
        }
    }
    return result;
}
}

ProjectExpressionAuditResult auditProjectExpressions(
    const ProjectDocument& document,
    talas::expr::ExpressionLanguageVersion language)
{
    return auditProjectExpressionsImpl(document, nullptr, language);
}

ProjectExpressionAuditResult auditProjectExpressions(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    talas::expr::ExpressionLanguageVersion language)
{
    return auditProjectExpressionsImpl(document, &registry, language);
}

ProjectExpressionAuditResult auditProjectExpressionsKeepingMeaning(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    talas::expr::ExpressionLanguageVersion language,
    talas::expr::ExpressionLanguageVersion previous)
{
    struct Reference
    {
        explicit Reference(talas::expr::ExpressionLanguageVersion value)
        {
            meaningReferenceLanguage = value;
        }
        ~Reference()
        {
            meaningReferenceLanguage.reset();
        }
        Reference(const Reference&) = delete;
        Reference& operator=(const Reference&) = delete;
    } reference{previous};
    auto result = auditProjectExpressionsImpl(document, &registry, language);
    // A definition's body must keep its meaning even where it is unused.
    const auto& definitions = document.expressionDefinitions.definitions;
    for (std::size_t index = 0;
         index < definitions.size() && result.ok();
         ++index)
    {
        auto invocation = definitions[index].name + "(";
        for (std::size_t argument = 0;
             argument < definitions[index].parameters.size();
             ++argument)
        {
            invocation += argument == 0 ? "0" : ",0";
        }
        invocation += ")";
        const auto current = talas::expr::parse(
            invocation, language, document.expressionDefinitions);
        const auto before = talas::expr::parse(
            invocation, previous, document.expressionDefinitions);
        if (current.ok() && before.ok()
            && !current.expression->computesSameAs(*before.expression))
        {
            appendDiagnostic(
                result,
                "$.expressionDefinitions[" + definitions[index].name + "].source",
                talas::expr::Diagnostic::Code::UnexpectedToken,
                {0, definitions[index].source.size()},
                "Val sends binary operators from left to right; "
                "add parentheses to keep this definition's meaning");
        }
    }
    return result;
}

ProjectExpressionAuditResult auditProjectParameterExpression(
    const ProjectDocument& document,
    const ProjectNode& node,
    const ProjectParameterExpression& value,
    const talas::core::ParameterDescriptor& descriptor,
    talas::expr::ExpressionLanguageVersion language,
    std::string path)
{
    ProjectExpressionAuditResult result;
    result.diagnostics.reserve(maximumProjectExpressionDiagnostics);
    auto parameterScope = std::vector<std::string_view>{};
    const ProjectGraph* owningGraph = &document.graph;
    for (const auto& definition : document.soundDefinitions)
    {
        const auto found = std::ranges::find_if(
            definition.graph.nodes,
            [&](const ProjectNode& candidate) {
                return &candidate == &node;
            });
        if (found != definition.graph.nodes.end())
        {
            parameterScope.push_back(definition.definitionId);
            owningGraph = &definition.graph;
            break;
        }
    }
    const auto voiceOwners = midiVoiceOwnersByNode(
        *owningGraph,
        parameterScope.empty()
            ? std::string_view{}
            : parameterScope.back());
    const auto replicationOwners = replicatorVoiceCountsByNode(
        document, *owningGraph, language);
    const auto replicationOwner = replicationOwners.find(node.instanceId);
    const auto replicationNumberVoices =
        node.typeId != "talas.replicator"
            && node.typeId != "talas.stereoizer"
            && node.typeId != "talas.surroundifer"
            && replicationOwner != replicationOwners.end()
        ? replicationOwner->second : std::uint16_t{0};
    const auto voiceOwner = voiceOwners.find(node.instanceId);
    appendExpressionDiagnostics(
        result,
        value,
        path,
        language,
        document.expressionDefinitions,
        descriptor.domain,
        !node.soundDefinitionId.has_value()
            || soundBindingsAllowStatefulExpressions(
                document,
                *node.soundDefinitionId,
                value.parameterId),
        &document,
        parameterScope,
        voiceOwner == voiceOwners.end() ? nullptr : &voiceOwner->second.owner,
        isMidiPitchBound(node.typeId, value.parameterId),
        node.typeId != "talas.midiVoice"
            && (!parameterScope.empty() || node.soundDefinitionId.has_value()),
        replicationNumberVoices,
        voiceOwner == voiceOwners.end() ? std::span<const ProjectVariableOwner>{}
            : voiceOwner->second.ancestors);
    if (value.text.has_value())
    {
        if (node.soundDefinitionId.has_value())
        {
            appendBoundSoundTextDiagnostics(
                result,
                document,
                *node.soundDefinitionId,
                value.parameterId,
                *value.text,
                path + ".text",
                language,
                document.expressionDefinitions);
        }
        else
        {
            appendEmbeddedTextDiagnostics(
                result,
                *value.text,
                path + ".text",
                node.typeId,
                value.parameterId,
                language,
                document.expressionDefinitions);
        }
    }
    return result;
}

ProjectVariableReferencePaths findProjectVariableReferencePaths(
    const ProjectDocument& document,
    const ProjectVariableOwner& owner,
    std::string_view variableId)
{
    ProjectVariableReferencePaths result;
    result.paths.reserve(std::min<std::size_t>(
        maximumPreparedProjectVariableReferenceUses, 64U));
    const auto appendValue = [&](
        const ProjectParameterExpression& value,
        const std::string& path,
        std::span<const std::string_view> scope,
        const ProjectVariableOwner* midiVoiceOwner,
        std::span<const ProjectVariableOwner> midiVoiceAncestors = {}) {
        if (!value.expression.has_value() || result.truncated)
            return;
        const auto parsed = talas::expr::parse(
            *value.expression,
            document.expressionLanguage,
            document.expressionDefinitions);
        if (!parsed.ok())
            return;
        for (const auto& reference
             : parsed.expression->variableReferenceMetadata())
        {
            const auto* declaration = findVisibleProjectVariableDeclaration(
                document, reference.name, scope, midiVoiceOwner, midiVoiceAncestors);
            if (declaration == nullptr
                || declaration->owner != owner
                || declaration->variableId != variableId)
            {
                continue;
            }
            auto referencePath = path + ".expression";
            if (reference.definitionIndex.has_value()
                && *reference.definitionIndex
                    < document.expressionDefinitions.definitions.size())
            {
                referencePath = "$.expressionDefinitions["
                    + document.expressionDefinitions.definitions[
                          *reference.definitionIndex]
                          .name
                    + "].source";
            }
            if (std::ranges::find(result.paths, referencePath)
                != result.paths.end())
            {
                continue;
            }
            if (result.paths.size()
                >= maximumPreparedProjectVariableReferenceUses)
            {
                result.truncated = true;
                return;
            }
            result.paths.push_back(std::move(referencePath));
        }
    };
    const auto appendGraph = [&](
        const ProjectGraph& graph,
        const std::string& path,
        std::span<const std::string_view> scope) {
        const auto voiceOwners = midiVoiceOwnersByNode(
            graph,
            scope.empty() ? std::string_view{} : scope.back());
        for (std::size_t nodeIndex = 0;
             nodeIndex < graph.nodes.size() && !result.truncated;
             ++nodeIndex)
        {
            const auto& node = graph.nodes[nodeIndex];
            const auto nodePath = path + ".nodes["
                + std::to_string(nodeIndex) + "]";
            for (std::size_t parameterIndex = 0;
                 parameterIndex < node.parameters.size()
                     && !result.truncated;
                 ++parameterIndex)
            {
                const auto voiceOwner = voiceOwners.find(node.instanceId);
                appendValue(
                    node.parameters[parameterIndex],
                    nodePath + ".parameters["
                        + std::to_string(parameterIndex) + "]",
                    scope,
                    voiceOwner == voiceOwners.end()
                        ? nullptr
                        : &voiceOwner->second.owner,
                    voiceOwner == voiceOwners.end() ? std::span<const ProjectVariableOwner>{}
                        : voiceOwner->second.ancestors);
            }
        }
    };
    appendGraph(document.graph, "$.graph", {});
    for (std::size_t definitionIndex = 0;
         definitionIndex < document.soundDefinitions.size()
             && !result.truncated;
         ++definitionIndex)
    {
        const auto& definition = document.soundDefinitions[definitionIndex];
        const std::array<std::string_view, 1> scope{
            definition.definitionId};
        const auto definitionPath = "$.soundDefinitions["
            + std::to_string(definitionIndex) + "]";
        appendGraph(
            definition.graph,
            definitionPath + ".graph",
            scope);
        for (std::size_t parameterIndex = 0;
             parameterIndex < definition.exposedParameters.size()
                 && !result.truncated;
             ++parameterIndex)
        {
            appendValue(
                definition.exposedParameters[parameterIndex].defaultValue,
                definitionPath + ".parameters["
                    + std::to_string(parameterIndex) + "].default",
                scope,
                nullptr);
        }
    }
    return result;
}
}
