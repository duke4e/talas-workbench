#include "runtime/component_bundle/ComponentBundleManifest.h"

#include "project/JsonValue.h"
#include "project/SemanticVersion.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <ranges>
#include <unordered_set>

namespace talas::runtime::component_bundle
{
namespace
{
namespace json = talas::project::json;

constexpr std::string_view realtimeProcessorRole = "realtime-processor";
constexpr std::string_view immutableResourceConsumerRole =
    "immutable-resource-consumer";
constexpr std::string_view boundedAudioStreamConsumerRole =
    "bounded-audio-stream-consumer";
constexpr std::string_view stateMigratorRole = "state-migrator";
constexpr std::string_view declarativeUiProviderRole =
    "declarative-ui-provider";

constexpr std::string_view realtimeAudioPermission = "realtime-audio";
constexpr std::string_view immutableResourceReadPermission =
    "immutable-resource-read";
constexpr std::string_view boundedAudioStreamReadPermission =
    "bounded-audio-stream-read";
constexpr std::string_view stateMigrationPermission = "state-migration";
constexpr std::string_view declarativeUiPermission = "declarative-ui";

bool addError(
    std::vector<ComponentBundleManifestError>& errors,
    ComponentBundleManifestErrorCode code,
    std::string path,
    std::string summary)
{
    if (errors.empty())
    {
        errors.push_back({
            code,
            std::move(path),
            std::move(summary),
        });
    }
    return false;
}

[[nodiscard]] bool isAsciiLower(char character)
{
    return character >= 'a' && character <= 'z';
}

[[nodiscard]] bool isAsciiDigit(char character)
{
    return character >= '0' && character <= '9';
}

[[nodiscard]] bool validStableId(std::string_view value)
{
    if (value.empty() || value.size() > maximumComponentBundleStableIdBytes
        || !isAsciiLower(value.front()))
    {
        return false;
    }
    return std::ranges::all_of(
        value,
        [](char character) {
            return isAsciiLower(character)
                || isAsciiDigit(character)
                || character == '-';
        });
}

[[nodiscard]] bool validReverseDns(
    std::string_view value,
    std::size_t maximumBytes)
{
    if (value.empty() || value.size() > maximumBytes
        || value.find('.') == std::string_view::npos)
    {
        return false;
    }

    std::size_t start = 0;
    while (start < value.size())
    {
        const auto separator = value.find('.', start);
        const auto segment = value.substr(
            start,
            separator == std::string_view::npos
                ? value.size() - start
                : separator - start);
        if (segment.empty() || !isAsciiLower(segment.front())
            || segment.back() == '-')
        {
            return false;
        }
        for (const auto character : segment)
        {
            if (!isAsciiLower(character)
                && !isAsciiDigit(character)
                && character != '-')
            {
                return false;
            }
        }
        if (separator == std::string_view::npos)
            return true;
        start = separator + 1U;
    }
    return false;
}

[[nodiscard]] bool validBundleId(std::string_view value)
{
    return validReverseDns(value, maximumComponentBundleIdBytes);
}

[[nodiscard]] bool validOwnedTypeId(
    std::string_view bundleId,
    std::string_view typeId)
{
    if (typeId.size() > maximumComponentBundleStableIdBytes
        || typeId.size() <= bundleId.size() + 1U
        || !typeId.starts_with(bundleId)
        || typeId[bundleId.size()] != '.')
    {
        return false;
    }
    return validReverseDns(typeId, maximumComponentBundleStableIdBytes);
}

[[nodiscard]] bool validPayloadPath(std::string_view path)
{
    if (path.empty() || path.size() > maximumComponentBundlePathBytes
        || path.front() == '/' || path.back() == '/'
        || path.find('\\') != std::string_view::npos)
    {
        return false;
    }

    std::size_t start = 0;
    while (start < path.size())
    {
        const auto separator = path.find('/', start);
        const auto segment = path.substr(
            start,
            separator == std::string_view::npos
                ? path.size() - start
                : separator - start);
        if (segment.empty() || segment == "." || segment == "..")
            return false;
        for (const auto character : segment)
        {
            if (!(character >= 'A' && character <= 'Z')
                && !isAsciiLower(character)
                && !isAsciiDigit(character)
                && character != '-' && character != '_'
                && character != '.')
            {
                return false;
            }
        }
        if (separator == std::string_view::npos)
            return true;
        start = separator + 1U;
    }
    return false;
}

[[nodiscard]] std::string asciiFold(std::string_view value)
{
    std::string folded;
    folded.reserve(value.size());
    for (const unsigned char character : value)
    {
        folded.push_back(static_cast<char>(
            character >= 'A' && character <= 'Z'
                ? character - 'A' + 'a'
                : character));
    }
    return folded;
}

[[nodiscard]] bool validLicenseExpression(std::string_view value)
{
    if (value.empty() || value.size() > 256U)
        return false;
    return std::ranges::all_of(
        value,
        [](char character) {
            return (character >= 'A' && character <= 'Z')
                || isAsciiLower(character)
                || isAsciiDigit(character)
                || character == '-' || character == '.'
                || character == '+' || character == '('
                || character == ')' || character == ' ';
        });
}

template <typename Enum>
[[nodiscard]] bool isOneOf(
    Enum value,
    std::initializer_list<Enum> known)
{
    return std::ranges::find(known, value) != known.end();
}

[[nodiscard]] bool validPortSignal(
    talas::core::PortType type,
    const talas::core::PortSignalContract& signal)
{
    using talas::core::AudioBusLayoutKind;
    using talas::core::PortEdgeSemantics;
    using talas::core::PortSignalRate;
    using talas::core::PortType;
    using talas::core::SpectralFrameFormat;

    const auto noLayout =
        signal.audioLayout == AudioBusLayoutKind::NotApplicable;
    const auto noSpectrum =
        signal.spectralFormat == SpectralFrameFormat::NotApplicable;
    if (!isOneOf(
            signal.rate,
            {PortSignalRate::Constant, PortSignalRate::Block,
             PortSignalRate::Sample, PortSignalRate::Event,
             PortSignalRate::Frame, PortSignalRate::VariableOrUnknown})
        || !isOneOf(
            signal.audioLayout,
            {AudioBusLayoutKind::NotApplicable, AudioBusLayoutKind::Mono,
             AudioBusLayoutKind::Stereo, AudioBusLayoutKind::Discrete4,
             AudioBusLayoutKind::Discrete6, AudioBusLayoutKind::Discrete8,
             AudioBusLayoutKind::VariableOrUnknown})
        || !isOneOf(
            signal.edge,
            {PortEdgeSemantics::NotApplicable,
             PortEdgeSemantics::Continuous, PortEdgeSemantics::Gate,
             PortEdgeSemantics::Trigger})
        || !isOneOf(
            signal.spectralFormat,
            {SpectralFrameFormat::NotApplicable,
             SpectralFrameFormat::LegacyPartialTriples,
             SpectralFrameFormat::PartialSpectrumV1,
             SpectralFrameFormat::ComplexSpectrumV1,
             SpectralFrameFormat::VariableOrUnknown}))
    {
        return false;
    }
    switch (type)
    {
    case PortType::Audio:
        return signal.rate == PortSignalRate::Sample
            && signal.audioLayout != AudioBusLayoutKind::NotApplicable
            && signal.edge == PortEdgeSemantics::Continuous
            && noSpectrum;
    case PortType::Control:
        return (signal.rate == PortSignalRate::Constant
                || signal.rate == PortSignalRate::Block
                || signal.rate == PortSignalRate::Sample)
            && noLayout && noSpectrum
            && signal.edge != PortEdgeSemantics::NotApplicable;
    case PortType::Event:
    case PortType::NoteStream:
        return signal.rate == PortSignalRate::Event
            && noLayout && noSpectrum
            && signal.edge == PortEdgeSemantics::Trigger;
    case PortType::SampleBuffer:
    case PortType::Wavetable:
        return signal.rate == PortSignalRate::Block
            && noLayout && noSpectrum
            && signal.edge == PortEdgeSemantics::Continuous;
    case PortType::Spectrum:
        return signal.rate == PortSignalRate::Frame
            && noLayout
            && signal.edge == PortEdgeSemantics::Continuous
            && (signal.spectralFormat
                    == SpectralFrameFormat::LegacyPartialTriples
                || signal.spectralFormat
                    == SpectralFrameFormat::PartialSpectrumV1
                || signal.spectralFormat
                    == SpectralFrameFormat::VariableOrUnknown);
    case PortType::ComplexSpectrum:
        return signal.rate == PortSignalRate::Frame
            && noLayout
            && signal.edge == PortEdgeSemantics::Continuous
            && signal.spectralFormat
                == SpectralFrameFormat::ComplexSpectrumV1;
    }
    return false;
}

[[nodiscard]] bool validPayloadRecord(
    const ComponentBundlePayloadRecord& record,
    std::string_view path,
    std::vector<ComponentBundleManifestError>& errors)
{
    if (!validPayloadPath(record.path))
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::InvalidPath,
            std::string(path) + ".path",
            "Component payload path is not canonical");
    }
    const auto foldedPath = asciiFold(record.path);
    if (foldedPath == "manifest.json" || foldedPath == "signature.cms")
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::InvalidPath,
            std::string(path) + ".path",
            "Component payload cannot replace reserved bundle metadata");
    }
    if (record.byteCount > maximumComponentBundleFileBytes)
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::BudgetExceeded,
            std::string(path) + ".bytes",
            "Component payload exceeds the per-file byte limit");
    }
    return true;
}

[[nodiscard]] bool knownParameterKind(talas::core::ParameterKind kind);
[[nodiscard]] bool knownValueDomain(talas::expr::ValueDomain domain);

[[nodiscard]] bool validParameter(
    const ComponentBundleParameter& parameter,
    std::string path,
    std::vector<ComponentBundleManifestError>& errors)
{
    if (!validStableId(parameter.id))
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::InvalidIdentity,
            path + ".id",
            "Component parameter identity is not canonical");
    }
    if (!knownParameterKind(parameter.kind)
        || !knownValueDomain(parameter.domain))
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::InvalidEnum,
            path,
            "Component parameter metadata contains an unknown enum");
    }
    if (parameter.choices.size()
        > maximumComponentBundleChoicesPerParameter)
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::BudgetExceeded,
            path + ".choices",
            "Component parameter choices exceed the supported limit");
    }
    const auto isChoice = parameter.kind == talas::core::ParameterKind::Choice;
    if (isChoice != !parameter.choices.empty())
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::ContradictoryDeclaration,
            path + ".choices",
            "Choice metadata contradicts the parameter kind");
    }
    std::unordered_set<std::string> choices;
    for (const auto& choice : parameter.choices)
    {
        if (!validStableId(choice))
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::InvalidIdentity,
                path + ".choices",
                "Component parameter choice identity is not canonical");
        }
        if (!choices.insert(choice).second)
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::DuplicateIdentity,
                path + ".choices",
                "Component parameter choice identities must be unique");
        }
    }
    return true;
}

[[nodiscard]] bool knownParameterKind(talas::core::ParameterKind kind)
{
    using talas::core::ParameterKind;
    switch (kind)
    {
    case ParameterKind::Expression:
    case ParameterKind::Choice:
    case ParameterKind::Boolean:
    case ParameterKind::Resource:
    case ParameterKind::Text:
        return true;
    }
    return false;
}

[[nodiscard]] bool knownValueDomain(talas::expr::ValueDomain domain)
{
    using talas::expr::ValueDomain;
    switch (domain)
    {
    case ValueDomain::Scalar:
    case ValueDomain::Hertz:
    case ValueDomain::Seconds:
    case ValueDomain::NoteNumber:
    case ValueDomain::Samples:
        return true;
    }
    return false;
}

[[nodiscard]] bool addEstimate(
    std::size_t& estimate,
    std::size_t amount)
{
    if (amount > maximumComponentBundleManifestBytes
        || estimate > maximumComponentBundleManifestBytes - amount)
    {
        return false;
    }
    estimate += amount;
    return true;
}

[[nodiscard]] bool addEscapedEstimate(
    std::size_t& estimate,
    std::string_view value)
{
    if (!addEstimate(estimate, 2U))
        return false;
    for (const unsigned char character : value)
    {
        const auto bytes = character < 0x20U
            ? 6U
            : (character == '"' || character == '\\' ? 2U : 1U);
        if (!addEstimate(estimate, bytes))
            return false;
    }
    return true;
}

[[nodiscard]] bool validateWriterPreflight(
    const ComponentBundleManifest& manifest,
    std::vector<ComponentBundleManifestError>& errors)
{
    if (manifest.files.size() > maximumComponentBundleFiles
        || manifest.nodes.size() > maximumComponentBundleNodes
        || manifest.dependencies.size() > maximumComponentBundleDependencies
        || manifest.state.migrations.size()
            > maximumComponentBundleMigrationEdges
        || manifest.provenance.notices.size()
            > maximumComponentBundleNotices)
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::BudgetExceeded,
            "$",
            "Component manifest model exceeds a collection limit");
    }

    std::size_t estimate = 1024U;
    const auto addText = [&](std::string_view value) {
        return addEscapedEstimate(estimate, value);
    };
    if (!addText(manifest.bundleId)
        || !addText(manifest.semanticVersion)
        || !addEstimate(estimate, 256U)
        || !addText(manifest.entry.path))
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::BudgetExceeded,
            "$",
            "Canonical component manifest exceeds its byte limit");
    }
    for (const auto& file : manifest.files)
    {
        if (!addEstimate(estimate, 256U) || !addText(file.path))
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::BudgetExceeded,
                "$.files",
                "Canonical component file index exceeds the manifest byte limit");
        }
    }
    for (const auto& node : manifest.nodes)
    {
        if (node.inputs.size() > maximumComponentBundleInputsPerNode
            || node.outputs.size() > maximumComponentBundleOutputsPerNode
            || node.parameters.size()
                > maximumComponentBundleParametersPerNode)
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::BudgetExceeded,
                "$.nodes",
                "Component node interface exceeds a collection limit");
        }
        if (!addEstimate(estimate, 256U) || !addText(node.typeId))
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::BudgetExceeded,
                "$.nodes",
                "Canonical component nodes exceed the manifest byte limit");
        }
        for (const auto* ports : {&node.inputs, &node.outputs})
        {
            for (const auto& port : *ports)
            {
                if (!addEstimate(estimate, 384U) || !addText(port.id))
                {
                    return addError(
                        errors,
                        ComponentBundleManifestErrorCode::BudgetExceeded,
                        "$.nodes",
                        "Canonical component ports exceed the manifest byte limit");
                }
            }
        }
        for (const auto& parameter : node.parameters)
        {
            if (parameter.choices.size()
                > maximumComponentBundleChoicesPerParameter)
            {
                return addError(
                    errors,
                    ComponentBundleManifestErrorCode::BudgetExceeded,
                    "$.nodes",
                    "Component parameter choices exceed a collection limit");
            }
            if (!addEstimate(estimate, 192U) || !addText(parameter.id))
            {
                return addError(
                    errors,
                    ComponentBundleManifestErrorCode::BudgetExceeded,
                    "$.nodes",
                    "Canonical component parameters exceed the manifest byte limit");
            }
            for (const auto& choice : parameter.choices)
            {
                if (!addEstimate(estimate, 4U) || !addText(choice))
                {
                    return addError(
                        errors,
                        ComponentBundleManifestErrorCode::BudgetExceeded,
                        "$.nodes",
                        "Canonical component choices exceed the manifest byte limit");
                }
            }
        }
    }
    for (const auto& dependency : manifest.dependencies)
    {
        if (!addEstimate(estimate, 256U)
            || !addText(dependency.bundleId)
            || !addText(dependency.minimumVersion)
            || !addText(dependency.maximumExclusiveVersion))
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::BudgetExceeded,
                "$.dependencies",
                "Canonical dependencies exceed the manifest byte limit");
        }
    }
    if (!addEstimate(
            estimate,
            manifest.state.migrations.size() * 128U)
        || !addText(manifest.provenance.publisher)
        || !addText(manifest.provenance.licenseExpression))
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::BudgetExceeded,
            "$",
            "Canonical component metadata exceeds the manifest byte limit");
    }
    for (const auto& notice : manifest.provenance.notices)
    {
        if (!addEstimate(estimate, 4U) || !addText(notice))
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::BudgetExceeded,
                "$.provenance.notices",
                "Canonical provenance notices exceed the manifest byte limit");
        }
    }
    if (manifest.declarativeUiPath
        && !addText(*manifest.declarativeUiPath))
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::BudgetExceeded,
            "$.declarativeUiPath",
            "Canonical UI path exceeds the manifest byte limit");
    }
    return true;
}

[[nodiscard]] bool validateManifest(
    const ComponentBundleManifest& manifest,
    std::vector<ComponentBundleManifestError>& errors)
{
    if (!validateWriterPreflight(manifest, errors))
        return false;
    if (!validBundleId(manifest.bundleId))
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::InvalidIdentity,
            "$.bundleId",
            "Component bundle identity is not canonical reverse-DNS");
    }
    if (!talas::project::isCanonicalSemanticVersion(
            manifest.semanticVersion,
            maximumComponentSemanticVersionBytes))
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::InvalidSemanticVersion,
            "$.semanticVersion",
            "Component semantic version is not canonical");
    }
    if (manifest.minimumEngineAbiVersion == 0
        || manifest.maximumEngineAbiVersion
            < manifest.minimumEngineAbiVersion
        || manifest.minimumEngineApiVersion == 0
        || manifest.maximumEngineApiVersion
            < manifest.minimumEngineApiVersion)
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::ContradictoryDeclaration,
            "$.engine",
            "Component engine version range is invalid");
    }
    if (manifest.declaredRoles == 0
        || (manifest.declaredRoles & ~allComponentExecutionRolesV1) != 0)
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::InvalidEnum,
            "$.roles",
            "Component roles are empty or unknown");
    }
    if (manifest.requestedPermissions == 0
        || (manifest.requestedPermissions
            & ~allComponentExecutionPermissionsV1) != 0)
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::InvalidEnum,
            "$.permissions",
            "Component permissions are empty or unknown");
    }
    if (manifest.requestedPermissions
        != componentPermissionsForRoles(manifest.declaredRoles))
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::ContradictoryDeclaration,
            "$.permissions",
            "Component permissions do not exactly match declared roles");
    }

    if (!validPayloadRecord(manifest.entry, "$.entry", errors))
        return false;
    if (!manifest.entry.path.starts_with("Payload/"))
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::InvalidPath,
            "$.entry.path",
            "Component entry must remain under Payload");
    }
    if (manifest.files.size() > maximumComponentBundleFiles)
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::BudgetExceeded,
            "$.files",
            "Component file index exceeds the supported limit");
    }

    std::unordered_set<std::string> foldedPaths;
    foldedPaths.insert(asciiFold(manifest.entry.path));
    auto totalBytes = manifest.entry.byteCount;
    std::size_t uiFileCount = 0;
    for (std::size_t index = 0; index < manifest.files.size(); ++index)
    {
        const auto& file = manifest.files[index];
        const auto path = "$.files[" + std::to_string(index) + "]";
        if (!validPayloadRecord(file, path, errors))
            return false;
        if (!foldedPaths.insert(asciiFold(file.path)).second)
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::DuplicateIdentity,
                "$.files",
                "Component payload paths collide under ASCII case folding");
        }
        if (file.byteCount > maximumComponentBundlePayloadBytes
            || totalBytes
                > maximumComponentBundlePayloadBytes - file.byteCount)
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::BudgetExceeded,
                "$.files",
                "Component payload bytes exceed the aggregate limit");
        }
        totalBytes += file.byteCount;
        switch (file.purpose)
        {
        case ComponentBundleFilePurpose::Resource:
        case ComponentBundleFilePurpose::License:
        case ComponentBundleFilePurpose::MigrationData:
            break;
        case ComponentBundleFilePurpose::DeclarativeUi:
            ++uiFileCount;
            break;
        default:
            return addError(
                errors,
                ComponentBundleManifestErrorCode::InvalidEnum,
                path + ".purpose",
                "Component file purpose is unknown");
        }
    }

    if (manifest.nodes.empty()
        || manifest.nodes.size() > maximumComponentBundleNodes)
    {
        return addError(
            errors,
            manifest.nodes.empty()
                ? ComponentBundleManifestErrorCode::ContradictoryDeclaration
                : ComponentBundleManifestErrorCode::BudgetExceeded,
            "$.nodes",
            "Component node declarations are empty or exceed the limit");
    }
    std::unordered_set<std::string> typeIds;
    for (std::size_t nodeIndex = 0;
         nodeIndex < manifest.nodes.size();
         ++nodeIndex)
    {
        const auto& node = manifest.nodes[nodeIndex];
        const auto path = "$.nodes[" + std::to_string(nodeIndex) + "]";
        if (!validOwnedTypeId(manifest.bundleId, node.typeId))
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::InvalidIdentity,
                path + ".typeId",
                "Component node type is outside the bundle namespace");
        }
        if (!typeIds.insert(node.typeId).second)
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::DuplicateIdentity,
                "$.nodes",
                "Component node type identities must be unique");
        }
        if (node.stateSchemaVersion == 0
            || node.stateSchemaVersion
                != manifest.state.currentSchemaVersion)
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::ContradictoryDeclaration,
                path + ".stateSchemaVersion",
                "Component node state schema contradicts the bundle state model");
        }
        if (node.inputs.size() > maximumComponentBundleInputsPerNode
            || node.outputs.size() > maximumComponentBundleOutputsPerNode
            || node.parameters.size()
                > maximumComponentBundleParametersPerNode)
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::BudgetExceeded,
                path,
                "Component node interface exceeds the supported limit");
        }
        std::unordered_set<std::string> portIds;
        const auto validatePorts = [&](const auto& ports, std::string_view label) {
            for (std::size_t index = 0; index < ports.size(); ++index)
            {
                const auto& port = ports[index];
                const auto portPath = path + "." + std::string(label)
                    + "[" + std::to_string(index) + "]";
                if (!validStableId(port.id))
                {
                    return addError(
                        errors,
                        ComponentBundleManifestErrorCode::InvalidIdentity,
                        portPath + ".id",
                        "Component port identity is not canonical");
                }
                if (!portIds.insert(port.id).second)
                {
                    return addError(
                        errors,
                        ComponentBundleManifestErrorCode::DuplicateIdentity,
                        path,
                        "Component port identities must be unique per node");
                }
                if (!validPortSignal(port.type, port.signal))
                {
                    return addError(
                        errors,
                        ComponentBundleManifestErrorCode::ContradictoryDeclaration,
                        portPath,
                        "Component port signal contract contradicts its type");
                }
            }
            return true;
        };
        if (!validatePorts(node.inputs, "inputs")
            || !validatePorts(node.outputs, "outputs"))
        {
            return false;
        }

        std::unordered_set<std::string> parameterIds;
        for (std::size_t index = 0; index < node.parameters.size(); ++index)
        {
            const auto& parameter = node.parameters[index];
            const auto parameterPath = path + ".parameters["
                + std::to_string(index) + "]";
            if (!validParameter(parameter, parameterPath, errors))
                return false;
            if (!parameterIds.insert(parameter.id).second)
            {
                return addError(
                    errors,
                    ComponentBundleManifestErrorCode::DuplicateIdentity,
                    path + ".parameters",
                    "Component parameter identities must be unique per node");
            }
        }
    }

    if (manifest.dependencies.size() > maximumComponentBundleDependencies)
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::BudgetExceeded,
            "$.dependencies",
            "Component dependencies exceed the supported limit");
    }
    std::unordered_set<std::string> dependencyIds;
    for (std::size_t index = 0; index < manifest.dependencies.size(); ++index)
    {
        const auto& dependency = manifest.dependencies[index];
        const auto path = "$.dependencies[" + std::to_string(index) + "]";
        if (!validBundleId(dependency.bundleId))
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::InvalidIdentity,
                path + ".bundleId",
                "Component dependency identity is not canonical");
        }
        if (dependency.bundleId == manifest.bundleId)
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::ContradictoryDeclaration,
                path + ".bundleId",
                "Component bundle cannot depend on itself");
        }
        if (!dependencyIds.insert(dependency.bundleId).second)
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::DuplicateIdentity,
                "$.dependencies",
                "Component dependency identities must be unique");
        }
        if (!talas::project::isCanonicalSemanticVersion(
                dependency.minimumVersion,
                maximumComponentSemanticVersionBytes)
            || !talas::project::isCanonicalSemanticVersion(
                dependency.maximumExclusiveVersion,
                maximumComponentSemanticVersionBytes))
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::InvalidSemanticVersion,
                path,
                "Component dependency version bound is not canonical");
        }
        if (talas::project::compareCanonicalSemanticVersionPrecedence(
                dependency.minimumVersion,
                dependency.maximumExclusiveVersion,
                maximumComponentSemanticVersionBytes)
            != talas::project::SemanticVersionOrder::Less)
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::ContradictoryDeclaration,
                path,
                "Component dependency version range is empty or reversed");
        }
    }

    if (manifest.state.currentSchemaVersion == 0
        || manifest.state.migrations.size()
            > maximumComponentBundleMigrationEdges)
    {
        return addError(
            errors,
            manifest.state.currentSchemaVersion == 0
                ? ComponentBundleManifestErrorCode::InvalidInteger
                : ComponentBundleManifestErrorCode::BudgetExceeded,
            "$.state",
            "Component state model is invalid or exceeds the limit");
    }
    std::unordered_set<std::uint32_t> migrationSources;
    for (std::size_t index = 0;
         index < manifest.state.migrations.size();
         ++index)
    {
        const auto& migration = manifest.state.migrations[index];
        if (migration.fromSchemaVersion == 0
            || migration.toSchemaVersion <= migration.fromSchemaVersion
            || migration.toSchemaVersion
                > manifest.state.currentSchemaVersion)
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::ContradictoryDeclaration,
                "$.state.migrations[" + std::to_string(index) + "]",
                "Component migration edge must move forward within the current schema");
        }
        if (!migrationSources.insert(migration.fromSchemaVersion).second)
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::DuplicateIdentity,
                "$.state.migrations",
                "Component migration sources must be unique");
        }
    }

    if (manifest.provenance.publisher.empty()
        || manifest.provenance.publisher.size()
            > maximumComponentBundleProvenanceTextBytes
        || !json::isValidUtf8(manifest.provenance.publisher)
        || !validLicenseExpression(manifest.provenance.licenseExpression)
        || manifest.provenance.notices.size()
            > maximumComponentBundleNotices)
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::ContradictoryDeclaration,
            "$.provenance",
            "Component provenance is empty, malformed, or exceeds its limit");
    }
    std::unordered_set<std::string> notices;
    for (const auto& notice : manifest.provenance.notices)
    {
        if (notice.empty()
            || notice.size() > maximumComponentBundleProvenanceTextBytes
            || !json::isValidUtf8(notice))
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::BudgetExceeded,
                "$.provenance.notices",
                "Component provenance notice is empty or oversized");
        }
        if (!notices.insert(notice).second)
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::DuplicateIdentity,
                "$.provenance.notices",
                "Component provenance notices must be unique");
        }
    }

    const auto hasUiRole = (manifest.declaredRoles
        & componentRole(ComponentExecutionRole::DeclarativeUiProvider)) != 0;
    if (hasUiRole != manifest.declarativeUiPath.has_value())
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::ContradictoryDeclaration,
            "$.declarativeUiPath",
            "Declarative UI path must exactly match the declared UI role");
    }
    if (manifest.declarativeUiPath)
    {
        if (!validPayloadPath(*manifest.declarativeUiPath)
            || uiFileCount != 1)
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::ContradictoryDeclaration,
                "$.declarativeUiPath",
                "Declarative UI requires exactly one canonical UI asset");
        }
        const auto found = std::ranges::find_if(
            manifest.files,
            [&](const auto& file) {
                return file.purpose
                        == ComponentBundleFilePurpose::DeclarativeUi
                    && file.path == *manifest.declarativeUiPath;
            });
        if (found == manifest.files.end())
        {
            return addError(
                errors,
                ComponentBundleManifestErrorCode::ContradictoryDeclaration,
                "$.declarativeUiPath",
                "Declarative UI path does not identify the declared UI asset");
        }
    }
    else if (uiFileCount != 0)
    {
        return addError(
            errors,
            ComponentBundleManifestErrorCode::ContradictoryDeclaration,
            "$.files",
            "UI-purpose assets require a declarative UI role and path");
    }
    return true;
}

void normalizeManifest(ComponentBundleManifest& manifest)
{
    std::ranges::sort(
        manifest.files,
        {},
        &ComponentBundleFileRecord::path);
    std::ranges::sort(
        manifest.nodes,
        {},
        &ComponentBundleNode::typeId);
    std::ranges::sort(
        manifest.dependencies,
        {},
        &ComponentBundleDependency::bundleId);
    std::ranges::sort(
        manifest.state.migrations,
        {},
        &ComponentBundleMigrationEdge::fromSchemaVersion);
    std::ranges::sort(manifest.provenance.notices);
}

class Decoder
{
public:
    ComponentBundleManifestParseResult decode(const json::Value& root)
    {
        ComponentBundleManifest manifest;
        if (!closedObject(
                root,
                {"format", "version", "bundleId", "semanticVersion",
                 "engine", "roles", "permissions", "entry", "files",
                 "nodes", "dependencies", "state", "provenance",
                 "declarativeUiPath"},
                "$"))
        {
            return result_;
        }

        std::string format;
        std::uint32_t version = 0;
        if (!requiredString(root, "format", "$.format", format)
            || !requiredUnsigned(root, "version", "$.version", version)
            || !requiredString(root, "bundleId", "$.bundleId", manifest.bundleId)
            || !requiredString(
                root,
                "semanticVersion",
                "$.semanticVersion",
                manifest.semanticVersion))
        {
            return result_;
        }
        if (format != componentBundleManifestFormat)
        {
            fail(
                ComponentBundleManifestErrorCode::ContradictoryDeclaration,
                "$.format",
                "Unsupported component manifest format");
            return result_;
        }
        if (version != componentBundleManifestVersion)
        {
            fail(
                ComponentBundleManifestErrorCode::ContradictoryDeclaration,
                "$.version",
                "Unsupported component manifest version");
            return result_;
        }

        const auto* engine = required(root, "engine", "$.engine");
        const auto* roles = required(root, "roles", "$.roles");
        const auto* permissions = required(root, "permissions", "$.permissions");
        const auto* entry = required(root, "entry", "$.entry");
        const auto* files = required(root, "files", "$.files");
        const auto* nodes = required(root, "nodes", "$.nodes");
        const auto* dependencies = required(root, "dependencies", "$.dependencies");
        const auto* state = required(root, "state", "$.state");
        const auto* provenance = required(root, "provenance", "$.provenance");
        const auto* ui = required(root, "declarativeUiPath", "$.declarativeUiPath");
        if (engine == nullptr || roles == nullptr || permissions == nullptr
            || entry == nullptr || files == nullptr || nodes == nullptr
            || dependencies == nullptr || state == nullptr
            || provenance == nullptr || ui == nullptr)
        {
            return result_;
        }

        if (!decodeEngine(*engine, manifest)
            || !decodeRoles(*roles, manifest.declaredRoles)
            || !decodePermissions(*permissions, manifest.requestedPermissions)
            || !decodePayload(*entry, "$.entry", manifest.entry)
            || !decodeFiles(*files, manifest.files)
            || !decodeNodes(*nodes, manifest.nodes)
            || !decodeDependencies(*dependencies, manifest.dependencies)
            || !decodeState(*state, manifest.state)
            || !decodeProvenance(*provenance, manifest.provenance)
            || !decodeOptionalPath(*ui, manifest.declarativeUiPath))
        {
            return result_;
        }

        if (!validateManifest(manifest, result_.errors))
            return result_;
        normalizeManifest(manifest);
        result_.manifest = std::move(manifest);
        return result_;
    }

private:
    bool fail(
        ComponentBundleManifestErrorCode code,
        std::string path,
        std::string summary)
    {
        return addError(
            result_.errors,
            code,
            std::move(path),
            std::move(summary));
    }

    [[nodiscard]] bool closedObject(
        const json::Value& value,
        std::initializer_list<std::string_view> keys,
        std::string_view path)
    {
        if (!value.isObject())
        {
            return fail(
                ComponentBundleManifestErrorCode::TypeMismatch,
                std::string(path),
                "Expected a component manifest object");
        }
        for (const auto& [key, ignored] : value.object())
        {
            (void)ignored;
            if (std::ranges::find(keys, key) == keys.end())
            {
                return fail(
                    ComponentBundleManifestErrorCode::UnknownField,
                    std::string(path),
                    "Component manifest object contains an unknown field");
            }
        }
        return true;
    }

    [[nodiscard]] const json::Value* required(
        const json::Value& object,
        std::string_view key,
        std::string path)
    {
        const auto* value = json::findObjectMember(object, key);
        if (value == nullptr)
        {
            fail(
                ComponentBundleManifestErrorCode::MissingField,
                std::move(path),
                "Component manifest is missing a required field");
        }
        return value;
    }

    [[nodiscard]] bool stringValue(
        const json::Value& value,
        std::string path,
        std::string& output)
    {
        if (!value.isString())
        {
            return fail(
                ComponentBundleManifestErrorCode::TypeMismatch,
                std::move(path),
                "Expected a component manifest string");
        }
        output = value.string();
        return true;
    }

    [[nodiscard]] bool requiredString(
        const json::Value& object,
        std::string_view key,
        std::string path,
        std::string& output)
    {
        const auto* value = required(object, key, path);
        return value != nullptr
            && stringValue(*value, std::move(path), output);
    }

    template <typename Unsigned>
    [[nodiscard]] bool unsignedValue(
        const json::Value& value,
        std::string path,
        Unsigned& output)
    {
        if (!value.isNumber())
        {
            return fail(
                ComponentBundleManifestErrorCode::TypeMismatch,
                std::move(path),
                "Expected a component manifest integer");
        }
        const auto token = value.numberToken();
        if (token.empty()
            || (token.size() > 1U && token.front() == '0')
            || !std::ranges::all_of(token, isAsciiDigit))
        {
            return fail(
                ComponentBundleManifestErrorCode::InvalidInteger,
                std::move(path),
                "Component manifest integer is not canonical unsigned decimal");
        }
        std::uint64_t parsed = 0;
        const auto result = std::from_chars(
            token.data(),
            token.data() + token.size(),
            parsed);
        if (result.ec != std::errc{}
            || result.ptr != token.data() + token.size()
            || parsed > std::numeric_limits<Unsigned>::max())
        {
            return fail(
                ComponentBundleManifestErrorCode::InvalidInteger,
                std::move(path),
                "Component manifest integer is out of range");
        }
        output = static_cast<Unsigned>(parsed);
        return true;
    }

    template <typename Unsigned>
    [[nodiscard]] bool requiredUnsigned(
        const json::Value& object,
        std::string_view key,
        std::string path,
        Unsigned& output)
    {
        const auto* value = required(object, key, path);
        return value != nullptr
            && unsignedValue(*value, std::move(path), output);
    }

    [[nodiscard]] bool requiredBoolean(
        const json::Value& object,
        std::string_view key,
        std::string path,
        bool& output)
    {
        const auto* value = required(object, key, path);
        if (value == nullptr)
            return false;
        if (!value->isBoolean())
        {
            return fail(
                ComponentBundleManifestErrorCode::TypeMismatch,
                std::move(path),
                "Expected a component manifest boolean");
        }
        output = value->boolean();
        return true;
    }

    [[nodiscard]] bool arrayValue(
        const json::Value& value,
        std::string path)
    {
        if (!value.isArray())
        {
            return fail(
                ComponentBundleManifestErrorCode::TypeMismatch,
                std::move(path),
                "Expected a component manifest array");
        }
        return true;
    }

    [[nodiscard]] bool decodeEngine(
        const json::Value& value,
        ComponentBundleManifest& manifest)
    {
        return closedObject(
                   value,
                   {"abiMinimum", "abiMaximum", "apiMinimum", "apiMaximum"},
                   "$.engine")
            && requiredUnsigned(
                value,
                "abiMinimum",
                "$.engine.abiMinimum",
                manifest.minimumEngineAbiVersion)
            && requiredUnsigned(
                value,
                "abiMaximum",
                "$.engine.abiMaximum",
                manifest.maximumEngineAbiVersion)
            && requiredUnsigned(
                value,
                "apiMinimum",
                "$.engine.apiMinimum",
                manifest.minimumEngineApiVersion)
            && requiredUnsigned(
                value,
                "apiMaximum",
                "$.engine.apiMaximum",
                manifest.maximumEngineApiVersion);
    }

    [[nodiscard]] bool decodeRoles(
        const json::Value& value,
        ComponentExecutionRoles& output)
    {
        if (!arrayValue(value, "$.roles"))
            return false;
        std::unordered_set<std::string_view> seen;
        for (const auto& item : value.array())
        {
            if (!item.isString())
            {
                return fail(
                    ComponentBundleManifestErrorCode::TypeMismatch,
                    "$.roles",
                    "Component role entries must be strings");
            }
            const auto role = std::string_view(item.string());
            if (!seen.insert(role).second)
            {
                return fail(
                    ComponentBundleManifestErrorCode::DuplicateIdentity,
                    "$.roles",
                    "Component role entries must be unique");
            }
            if (role == realtimeProcessorRole)
                output |= componentRole(ComponentExecutionRole::RealtimeProcessor);
            else if (role == immutableResourceConsumerRole)
                output |= componentRole(
                    ComponentExecutionRole::ImmutableResourceConsumer);
            else if (role == boundedAudioStreamConsumerRole)
                output |= componentRole(
                    ComponentExecutionRole::BoundedAudioStreamConsumer);
            else if (role == stateMigratorRole)
                output |= componentRole(ComponentExecutionRole::StateMigrator);
            else if (role == declarativeUiProviderRole)
                output |= componentRole(
                    ComponentExecutionRole::DeclarativeUiProvider);
            else
            {
                return fail(
                    ComponentBundleManifestErrorCode::InvalidEnum,
                    "$.roles",
                    "Component role is unknown");
            }
        }
        return true;
    }

    [[nodiscard]] bool decodePermissions(
        const json::Value& value,
        ComponentExecutionPermissions& output)
    {
        if (!arrayValue(value, "$.permissions"))
            return false;
        std::unordered_set<std::string_view> seen;
        for (const auto& item : value.array())
        {
            if (!item.isString())
            {
                return fail(
                    ComponentBundleManifestErrorCode::TypeMismatch,
                    "$.permissions",
                    "Component permission entries must be strings");
            }
            const auto permission = std::string_view(item.string());
            if (!seen.insert(permission).second)
            {
                return fail(
                    ComponentBundleManifestErrorCode::DuplicateIdentity,
                    "$.permissions",
                    "Component permission entries must be unique");
            }
            if (permission == realtimeAudioPermission)
                output |= componentPermission(ComponentExecutionPermission::RealtimeAudio);
            else if (permission == immutableResourceReadPermission)
                output |= componentPermission(
                    ComponentExecutionPermission::ImmutableResourceRead);
            else if (permission == boundedAudioStreamReadPermission)
                output |= componentPermission(
                    ComponentExecutionPermission::BoundedAudioStreamRead);
            else if (permission == stateMigrationPermission)
                output |= componentPermission(ComponentExecutionPermission::StateMigration);
            else if (permission == declarativeUiPermission)
                output |= componentPermission(ComponentExecutionPermission::DeclarativeUi);
            else
            {
                return fail(
                    ComponentBundleManifestErrorCode::InvalidEnum,
                    "$.permissions",
                    "Component permission is unknown");
            }
        }
        return true;
    }

    [[nodiscard]] bool decodeDigest(
        const json::Value& value,
        std::string path,
        std::array<std::uint8_t, componentTrustDigestBytes>& output)
    {
        if (!value.isString() || value.string().size() != output.size() * 2U)
        {
            return fail(
                ComponentBundleManifestErrorCode::InvalidDigest,
                std::move(path),
                "Component payload digest must be lowercase SHA-256");
        }
        const auto nibble = [](char character) -> int {
            if (character >= '0' && character <= '9')
                return character - '0';
            if (character >= 'a' && character <= 'f')
                return character - 'a' + 10;
            return -1;
        };
        for (std::size_t index = 0; index < output.size(); ++index)
        {
            const auto high = nibble(value.string()[index * 2U]);
            const auto low = nibble(value.string()[index * 2U + 1U]);
            if (high < 0 || low < 0)
            {
                return fail(
                    ComponentBundleManifestErrorCode::InvalidDigest,
                    std::move(path),
                    "Component payload digest must be lowercase SHA-256");
            }
            output[index] = static_cast<std::uint8_t>((high << 4U) | low);
        }
        return true;
    }

    [[nodiscard]] bool decodePayload(
        const json::Value& value,
        std::string path,
        ComponentBundlePayloadRecord& output)
    {
        if (!closedObject(value, {"path", "bytes", "sha256"}, path))
            return false;
        const auto* digest = required(value, "sha256", path + ".sha256");
        return requiredString(value, "path", path + ".path", output.path)
            && requiredUnsigned(value, "bytes", path + ".bytes", output.byteCount)
            && digest != nullptr
            && decodeDigest(*digest, path + ".sha256", output.sha256);
    }

    [[nodiscard]] bool decodeFilePurpose(
        const json::Value& value,
        std::string path,
        ComponentBundleFilePurpose& output)
    {
        if (!value.isString())
        {
            return fail(
                ComponentBundleManifestErrorCode::TypeMismatch,
                std::move(path),
                "Component file purpose must be a string");
        }
        if (value.string() == "resource")
            output = ComponentBundleFilePurpose::Resource;
        else if (value.string() == "declarative-ui")
            output = ComponentBundleFilePurpose::DeclarativeUi;
        else if (value.string() == "license")
            output = ComponentBundleFilePurpose::License;
        else if (value.string() == "migration-data")
            output = ComponentBundleFilePurpose::MigrationData;
        else
        {
            return fail(
                ComponentBundleManifestErrorCode::InvalidEnum,
                std::move(path),
                "Component file purpose is unknown");
        }
        return true;
    }

    [[nodiscard]] bool decodeFiles(
        const json::Value& value,
        std::vector<ComponentBundleFileRecord>& output)
    {
        if (!arrayValue(value, "$.files"))
            return false;
        if (value.array().size() > maximumComponentBundleFiles)
        {
            return fail(
                ComponentBundleManifestErrorCode::BudgetExceeded,
                "$.files",
                "Component file index exceeds the supported limit");
        }
        output.reserve(value.array().size());
        for (std::size_t index = 0; index < value.array().size(); ++index)
        {
            const auto path = "$.files[" + std::to_string(index) + "]";
            const auto& item = value.array()[index];
            if (!closedObject(item, {"path", "purpose", "bytes", "sha256"}, path))
                return false;
            ComponentBundleFileRecord file;
            const auto* purpose = required(item, "purpose", path + ".purpose");
            const auto* digest = required(item, "sha256", path + ".sha256");
            if (!requiredString(item, "path", path + ".path", file.path)
                || !requiredUnsigned(item, "bytes", path + ".bytes", file.byteCount)
                || purpose == nullptr || digest == nullptr
                || !decodeFilePurpose(*purpose, path + ".purpose", file.purpose)
                || !decodeDigest(*digest, path + ".sha256", file.sha256))
            {
                return false;
            }
            output.push_back(std::move(file));
        }
        return true;
    }

    [[nodiscard]] bool decodePortType(
        std::string_view value,
        talas::core::PortType& output)
    {
        using talas::core::PortType;
        if (value == "audio") output = PortType::Audio;
        else if (value == "control") output = PortType::Control;
        else if (value == "event") output = PortType::Event;
        else if (value == "sample-buffer") output = PortType::SampleBuffer;
        else if (value == "wavetable") output = PortType::Wavetable;
        else if (value == "spectrum") output = PortType::Spectrum;
        else if (value == "complex-spectrum") output = PortType::ComplexSpectrum;
        else if (value == "note-stream") output = PortType::NoteStream;
        else return false;
        return true;
    }

    [[nodiscard]] bool decodeRate(
        std::string_view value,
        talas::core::PortSignalRate& output)
    {
        using talas::core::PortSignalRate;
        if (value == "constant") output = PortSignalRate::Constant;
        else if (value == "block") output = PortSignalRate::Block;
        else if (value == "sample") output = PortSignalRate::Sample;
        else if (value == "event") output = PortSignalRate::Event;
        else if (value == "frame") output = PortSignalRate::Frame;
        else if (value == "variable-or-unknown") output = PortSignalRate::VariableOrUnknown;
        else return false;
        return true;
    }

    [[nodiscard]] bool decodeAudioLayout(
        std::string_view value,
        talas::core::AudioBusLayoutKind& output)
    {
        using talas::core::AudioBusLayoutKind;
        if (value == "not-applicable") output = AudioBusLayoutKind::NotApplicable;
        else if (value == "mono") output = AudioBusLayoutKind::Mono;
        else if (value == "stereo") output = AudioBusLayoutKind::Stereo;
        else if (value == "discrete-4") output = AudioBusLayoutKind::Discrete4;
        else if (value == "discrete-6") output = AudioBusLayoutKind::Discrete6;
        else if (value == "discrete-8") output = AudioBusLayoutKind::Discrete8;
        else if (value == "variable-or-unknown") output = AudioBusLayoutKind::VariableOrUnknown;
        else return false;
        return true;
    }

    [[nodiscard]] bool decodeEdge(
        std::string_view value,
        talas::core::PortEdgeSemantics& output)
    {
        using talas::core::PortEdgeSemantics;
        if (value == "not-applicable") output = PortEdgeSemantics::NotApplicable;
        else if (value == "continuous") output = PortEdgeSemantics::Continuous;
        else if (value == "gate") output = PortEdgeSemantics::Gate;
        else if (value == "trigger") output = PortEdgeSemantics::Trigger;
        else return false;
        return true;
    }

    [[nodiscard]] bool decodeSpectralFormat(
        std::string_view value,
        talas::core::SpectralFrameFormat& output)
    {
        using talas::core::SpectralFrameFormat;
        if (value == "not-applicable") output = SpectralFrameFormat::NotApplicable;
        else if (value == "legacy-partial-triples") output = SpectralFrameFormat::LegacyPartialTriples;
        else if (value == "partial-spectrum-v1") output = SpectralFrameFormat::PartialSpectrumV1;
        else if (value == "complex-spectrum-v1") output = SpectralFrameFormat::ComplexSpectrumV1;
        else if (value == "variable-or-unknown") output = SpectralFrameFormat::VariableOrUnknown;
        else return false;
        return true;
    }

    [[nodiscard]] bool decodePort(
        const json::Value& value,
        std::string path,
        ComponentBundlePort& output)
    {
        if (!closedObject(
                value,
                {"id", "type", "required", "rate", "audioLayout", "edge",
                 "spectralFormat", "timelineBearing"},
                path))
        {
            return false;
        }
        std::string type;
        std::string rate;
        std::string layout;
        std::string edge;
        std::string spectral;
        if (!requiredString(value, "id", path + ".id", output.id)
            || !requiredString(value, "type", path + ".type", type)
            || !requiredBoolean(value, "required", path + ".required", output.required)
            || !requiredString(value, "rate", path + ".rate", rate)
            || !requiredString(value, "audioLayout", path + ".audioLayout", layout)
            || !requiredString(value, "edge", path + ".edge", edge)
            || !requiredString(value, "spectralFormat", path + ".spectralFormat", spectral)
            || !requiredBoolean(
                value,
                "timelineBearing",
                path + ".timelineBearing",
                output.signal.timelineBearing))
        {
            return false;
        }
        if (!decodePortType(type, output.type)
            || !decodeRate(rate, output.signal.rate)
            || !decodeAudioLayout(layout, output.signal.audioLayout)
            || !decodeEdge(edge, output.signal.edge)
            || !decodeSpectralFormat(spectral, output.signal.spectralFormat))
        {
            return fail(
                ComponentBundleManifestErrorCode::InvalidEnum,
                std::move(path),
                "Component port metadata contains an unknown enum");
        }
        return true;
    }

    [[nodiscard]] bool decodePorts(
        const json::Value& value,
        std::string path,
        std::size_t maximum,
        std::vector<ComponentBundlePort>& output)
    {
        if (!arrayValue(value, path))
            return false;
        if (value.array().size() > maximum)
        {
            return fail(
                ComponentBundleManifestErrorCode::BudgetExceeded,
                std::move(path),
                "Component ports exceed the supported limit");
        }
        output.reserve(value.array().size());
        for (std::size_t index = 0; index < value.array().size(); ++index)
        {
            ComponentBundlePort port;
            if (!decodePort(
                    value.array()[index],
                    path + "[" + std::to_string(index) + "]",
                    port))
            {
                return false;
            }
            output.push_back(std::move(port));
        }
        return true;
    }

    [[nodiscard]] bool decodeParameterKind(
        std::string_view value,
        talas::core::ParameterKind& output)
    {
        using talas::core::ParameterKind;
        if (value == "expression") output = ParameterKind::Expression;
        else if (value == "choice") output = ParameterKind::Choice;
        else if (value == "boolean") output = ParameterKind::Boolean;
        else if (value == "resource") output = ParameterKind::Resource;
        else if (value == "text") output = ParameterKind::Text;
        else return false;
        return true;
    }

    [[nodiscard]] bool decodeDomain(
        std::string_view value,
        talas::expr::ValueDomain& output)
    {
        using talas::expr::ValueDomain;
        if (value == "scalar") output = ValueDomain::Scalar;
        else if (value == "hertz") output = ValueDomain::Hertz;
        else if (value == "seconds") output = ValueDomain::Seconds;
        else if (value == "note-number") output = ValueDomain::NoteNumber;
        else if (value == "samples") output = ValueDomain::Samples;
        else return false;
        return true;
    }

    [[nodiscard]] bool decodeParameter(
        const json::Value& value,
        std::string path,
        ComponentBundleParameter& output)
    {
        if (!closedObject(value, {"id", "kind", "domain", "choices"}, path))
            return false;
        std::string kind;
        std::string domain;
        const auto* choices = required(value, "choices", path + ".choices");
        if (!requiredString(value, "id", path + ".id", output.id)
            || !requiredString(value, "kind", path + ".kind", kind)
            || !requiredString(value, "domain", path + ".domain", domain)
            || choices == nullptr || !arrayValue(*choices, path + ".choices"))
        {
            return false;
        }
        if (!decodeParameterKind(kind, output.kind)
            || !decodeDomain(domain, output.domain))
        {
            return fail(
                ComponentBundleManifestErrorCode::InvalidEnum,
                std::move(path),
                "Component parameter metadata contains an unknown enum");
        }
        if (choices->array().size() > maximumComponentBundleChoicesPerParameter)
        {
            return fail(
                ComponentBundleManifestErrorCode::BudgetExceeded,
                path + ".choices",
                "Component parameter choices exceed the supported limit");
        }
        output.choices.reserve(choices->array().size());
        for (const auto& choice : choices->array())
        {
            if (!choice.isString())
            {
                return fail(
                    ComponentBundleManifestErrorCode::TypeMismatch,
                    path + ".choices",
                    "Component parameter choices must be strings");
            }
            output.choices.push_back(choice.string());
        }
        return true;
    }

    [[nodiscard]] bool decodeNodes(
        const json::Value& value,
        std::vector<ComponentBundleNode>& output)
    {
        if (!arrayValue(value, "$.nodes"))
            return false;
        if (value.array().size() > maximumComponentBundleNodes)
        {
            return fail(
                ComponentBundleManifestErrorCode::BudgetExceeded,
                "$.nodes",
                "Component nodes exceed the supported limit");
        }
        output.reserve(value.array().size());
        for (std::size_t index = 0; index < value.array().size(); ++index)
        {
            const auto path = "$.nodes[" + std::to_string(index) + "]";
            const auto& item = value.array()[index];
            if (!closedObject(
                    item,
                    {"typeId", "stateSchemaVersion", "inputs", "outputs", "parameters"},
                    path))
            {
                return false;
            }
            ComponentBundleNode node;
            const auto* inputs = required(item, "inputs", path + ".inputs");
            const auto* outputs = required(item, "outputs", path + ".outputs");
            const auto* parameters = required(item, "parameters", path + ".parameters");
            if (!requiredString(item, "typeId", path + ".typeId", node.typeId)
                || !requiredUnsigned(
                    item,
                    "stateSchemaVersion",
                    path + ".stateSchemaVersion",
                    node.stateSchemaVersion)
                || inputs == nullptr || outputs == nullptr || parameters == nullptr
                || !decodePorts(
                    *inputs,
                    path + ".inputs",
                    maximumComponentBundleInputsPerNode,
                    node.inputs)
                || !decodePorts(
                    *outputs,
                    path + ".outputs",
                    maximumComponentBundleOutputsPerNode,
                    node.outputs)
                || !arrayValue(*parameters, path + ".parameters"))
            {
                return false;
            }
            if (parameters->array().size()
                > maximumComponentBundleParametersPerNode)
            {
                return fail(
                    ComponentBundleManifestErrorCode::BudgetExceeded,
                    path + ".parameters",
                    "Component parameters exceed the supported limit");
            }
            node.parameters.reserve(parameters->array().size());
            for (std::size_t parameterIndex = 0;
                 parameterIndex < parameters->array().size();
                 ++parameterIndex)
            {
                ComponentBundleParameter parameter;
                if (!decodeParameter(
                        parameters->array()[parameterIndex],
                        path + ".parameters["
                            + std::to_string(parameterIndex) + "]",
                        parameter))
                {
                    return false;
                }
                node.parameters.push_back(std::move(parameter));
            }
            output.push_back(std::move(node));
        }
        return true;
    }

    [[nodiscard]] bool decodeDependencies(
        const json::Value& value,
        std::vector<ComponentBundleDependency>& output)
    {
        if (!arrayValue(value, "$.dependencies"))
            return false;
        if (value.array().size() > maximumComponentBundleDependencies)
        {
            return fail(
                ComponentBundleManifestErrorCode::BudgetExceeded,
                "$.dependencies",
                "Component dependencies exceed the supported limit");
        }
        output.reserve(value.array().size());
        for (std::size_t index = 0; index < value.array().size(); ++index)
        {
            const auto path = "$.dependencies[" + std::to_string(index) + "]";
            const auto& item = value.array()[index];
            if (!closedObject(
                    item,
                    {"bundleId", "minimumVersion", "maximumExclusiveVersion", "optional"},
                    path))
            {
                return false;
            }
            ComponentBundleDependency dependency;
            if (!requiredString(item, "bundleId", path + ".bundleId", dependency.bundleId)
                || !requiredString(
                    item,
                    "minimumVersion",
                    path + ".minimumVersion",
                    dependency.minimumVersion)
                || !requiredString(
                    item,
                    "maximumExclusiveVersion",
                    path + ".maximumExclusiveVersion",
                    dependency.maximumExclusiveVersion)
                || !requiredBoolean(
                    item,
                    "optional",
                    path + ".optional",
                    dependency.optional))
            {
                return false;
            }
            output.push_back(std::move(dependency));
        }
        return true;
    }

    [[nodiscard]] bool decodeState(
        const json::Value& value,
        ComponentBundleStateModel& output)
    {
        if (!closedObject(value, {"currentSchemaVersion", "migrations"}, "$.state"))
            return false;
        const auto* migrations = required(value, "migrations", "$.state.migrations");
        if (!requiredUnsigned(
                value,
                "currentSchemaVersion",
                "$.state.currentSchemaVersion",
                output.currentSchemaVersion)
            || migrations == nullptr
            || !arrayValue(*migrations, "$.state.migrations"))
        {
            return false;
        }
        if (migrations->array().size() > maximumComponentBundleMigrationEdges)
        {
            return fail(
                ComponentBundleManifestErrorCode::BudgetExceeded,
                "$.state.migrations",
                "Component migrations exceed the supported limit");
        }
        output.migrations.reserve(migrations->array().size());
        for (std::size_t index = 0; index < migrations->array().size(); ++index)
        {
            const auto path = "$.state.migrations[" + std::to_string(index) + "]";
            const auto& item = migrations->array()[index];
            if (!closedObject(item, {"from", "to", "mode"}, path))
                return false;
            ComponentBundleMigrationEdge edge;
            std::string mode;
            if (!requiredUnsigned(item, "from", path + ".from", edge.fromSchemaVersion)
                || !requiredUnsigned(item, "to", path + ".to", edge.toSchemaVersion)
                || !requiredString(item, "mode", path + ".mode", mode))
            {
                return false;
            }
            if (mode != "host-mediated")
            {
                return fail(
                    ComponentBundleManifestErrorCode::InvalidEnum,
                    path + ".mode",
                    "Component migration mode is unknown");
            }
            output.migrations.push_back(edge);
        }
        return true;
    }

    [[nodiscard]] bool decodeProvenance(
        const json::Value& value,
        ComponentBundleProvenance& output)
    {
        if (!closedObject(value, {"publisher", "licenseExpression", "notices"}, "$.provenance"))
            return false;
        const auto* notices = required(value, "notices", "$.provenance.notices");
        if (!requiredString(value, "publisher", "$.provenance.publisher", output.publisher)
            || !requiredString(
                value,
                "licenseExpression",
                "$.provenance.licenseExpression",
                output.licenseExpression)
            || notices == nullptr
            || !arrayValue(*notices, "$.provenance.notices"))
        {
            return false;
        }
        if (notices->array().size() > maximumComponentBundleNotices)
        {
            return fail(
                ComponentBundleManifestErrorCode::BudgetExceeded,
                "$.provenance.notices",
                "Component provenance notices exceed the supported limit");
        }
        output.notices.reserve(notices->array().size());
        for (const auto& notice : notices->array())
        {
            if (!notice.isString())
            {
                return fail(
                    ComponentBundleManifestErrorCode::TypeMismatch,
                    "$.provenance.notices",
                    "Component provenance notices must be strings");
            }
            output.notices.push_back(notice.string());
        }
        return true;
    }

    [[nodiscard]] bool decodeOptionalPath(
        const json::Value& value,
        std::optional<std::string>& output)
    {
        if (value.isNull())
        {
            output.reset();
            return true;
        }
        if (!value.isString())
        {
            return fail(
                ComponentBundleManifestErrorCode::TypeMismatch,
                "$.declarativeUiPath",
                "Declarative UI path must be a string or null");
        }
        output = value.string();
        return true;
    }

    ComponentBundleManifestParseResult result_;
};

[[nodiscard]] std::string_view filePurposeName(ComponentBundleFilePurpose purpose)
{
    switch (purpose)
    {
    case ComponentBundleFilePurpose::Resource: return "resource";
    case ComponentBundleFilePurpose::DeclarativeUi: return "declarative-ui";
    case ComponentBundleFilePurpose::License: return "license";
    case ComponentBundleFilePurpose::MigrationData: return "migration-data";
    }
    return {};
}

[[nodiscard]] std::string_view portTypeName(talas::core::PortType type)
{
    using talas::core::PortType;
    switch (type)
    {
    case PortType::Audio: return "audio";
    case PortType::Control: return "control";
    case PortType::Event: return "event";
    case PortType::SampleBuffer: return "sample-buffer";
    case PortType::Wavetable: return "wavetable";
    case PortType::Spectrum: return "spectrum";
    case PortType::ComplexSpectrum: return "complex-spectrum";
    case PortType::NoteStream: return "note-stream";
    }
    return {};
}

[[nodiscard]] std::string_view rateName(talas::core::PortSignalRate rate)
{
    using talas::core::PortSignalRate;
    switch (rate)
    {
    case PortSignalRate::Constant: return "constant";
    case PortSignalRate::Block: return "block";
    case PortSignalRate::Sample: return "sample";
    case PortSignalRate::Event: return "event";
    case PortSignalRate::Frame: return "frame";
    case PortSignalRate::VariableOrUnknown: return "variable-or-unknown";
    }
    return {};
}

[[nodiscard]] std::string_view layoutName(talas::core::AudioBusLayoutKind layout)
{
    using talas::core::AudioBusLayoutKind;
    switch (layout)
    {
    case AudioBusLayoutKind::NotApplicable: return "not-applicable";
    case AudioBusLayoutKind::Mono: return "mono";
    case AudioBusLayoutKind::Stereo: return "stereo";
    case AudioBusLayoutKind::Discrete4: return "discrete-4";
    case AudioBusLayoutKind::Discrete6: return "discrete-6";
    case AudioBusLayoutKind::Discrete8: return "discrete-8";
    case AudioBusLayoutKind::VariableOrUnknown: return "variable-or-unknown";
    }
    return {};
}

[[nodiscard]] std::string_view edgeName(talas::core::PortEdgeSemantics edge)
{
    using talas::core::PortEdgeSemantics;
    switch (edge)
    {
    case PortEdgeSemantics::NotApplicable: return "not-applicable";
    case PortEdgeSemantics::Continuous: return "continuous";
    case PortEdgeSemantics::Gate: return "gate";
    case PortEdgeSemantics::Trigger: return "trigger";
    }
    return {};
}

[[nodiscard]] std::string_view spectralName(talas::core::SpectralFrameFormat format)
{
    using talas::core::SpectralFrameFormat;
    switch (format)
    {
    case SpectralFrameFormat::NotApplicable: return "not-applicable";
    case SpectralFrameFormat::LegacyPartialTriples: return "legacy-partial-triples";
    case SpectralFrameFormat::PartialSpectrumV1: return "partial-spectrum-v1";
    case SpectralFrameFormat::ComplexSpectrumV1: return "complex-spectrum-v1";
    case SpectralFrameFormat::VariableOrUnknown: return "variable-or-unknown";
    }
    return {};
}

[[nodiscard]] std::string_view parameterKindName(talas::core::ParameterKind kind)
{
    using talas::core::ParameterKind;
    switch (kind)
    {
    case ParameterKind::Expression: return "expression";
    case ParameterKind::Choice: return "choice";
    case ParameterKind::Boolean: return "boolean";
    case ParameterKind::Resource: return "resource";
    case ParameterKind::Text: return "text";
    }
    return {};
}

[[nodiscard]] std::string_view domainName(talas::expr::ValueDomain domain)
{
    using talas::expr::ValueDomain;
    switch (domain)
    {
    case ValueDomain::Scalar: return "scalar";
    case ValueDomain::Hertz: return "hertz";
    case ValueDomain::Seconds: return "seconds";
    case ValueDomain::NoteNumber: return "note-number";
    case ValueDomain::Samples: return "samples";
    }
    return {};
}

void appendString(std::string& output, std::string_view value)
{
    json::appendEscapedString(output, value);
}

void appendKey(std::string& output, std::string_view key)
{
    appendString(output, key);
    output.push_back(':');
}

void appendDigest(
    std::string& output,
    const std::array<std::uint8_t, componentTrustDigestBytes>& digest)
{
    constexpr std::string_view hex = "0123456789abcdef";
    output.push_back('"');
    for (const auto byte : digest)
    {
        output.push_back(hex[byte >> 4U]);
        output.push_back(hex[byte & 0x0fU]);
    }
    output.push_back('"');
}

void appendPayload(
    std::string& output,
    const ComponentBundlePayloadRecord& record)
{
    output.push_back('{');
    appendKey(output, "path"); appendString(output, record.path);
    output += ","; appendKey(output, "bytes"); output += std::to_string(record.byteCount);
    output += ","; appendKey(output, "sha256"); appendDigest(output, record.sha256);
    output.push_back('}');
}

void appendPort(std::string& output, const ComponentBundlePort& port)
{
    output.push_back('{');
    appendKey(output, "id"); appendString(output, port.id);
    output += ","; appendKey(output, "type"); appendString(output, portTypeName(port.type));
    output += ","; appendKey(output, "required"); output += port.required ? "true" : "false";
    output += ","; appendKey(output, "rate"); appendString(output, rateName(port.signal.rate));
    output += ","; appendKey(output, "audioLayout"); appendString(output, layoutName(port.signal.audioLayout));
    output += ","; appendKey(output, "edge"); appendString(output, edgeName(port.signal.edge));
    output += ","; appendKey(output, "spectralFormat"); appendString(output, spectralName(port.signal.spectralFormat));
    output += ","; appendKey(output, "timelineBearing"); output += port.signal.timelineBearing ? "true" : "false";
    output.push_back('}');
}

template <typename Values, typename Append>
void appendArray(std::string& output, const Values& values, Append append)
{
    output.push_back('[');
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0)
            output.push_back(',');
        append(output, values[index]);
    }
    output.push_back(']');
}

void appendRoleArray(std::string& output, ComponentExecutionRoles roles)
{
    output.push_back('[');
    bool first = true;
    const auto append = [&](std::string_view name) {
        if (!first) output.push_back(',');
        first = false;
        appendString(output, name);
    };
    if ((roles & componentRole(ComponentExecutionRole::RealtimeProcessor)) != 0)
        append(realtimeProcessorRole);
    if ((roles & componentRole(ComponentExecutionRole::ImmutableResourceConsumer)) != 0)
        append(immutableResourceConsumerRole);
    if ((roles & componentRole(ComponentExecutionRole::BoundedAudioStreamConsumer)) != 0)
        append(boundedAudioStreamConsumerRole);
    if ((roles & componentRole(ComponentExecutionRole::StateMigrator)) != 0)
        append(stateMigratorRole);
    if ((roles & componentRole(ComponentExecutionRole::DeclarativeUiProvider)) != 0)
        append(declarativeUiProviderRole);
    output.push_back(']');
}

void appendPermissionArray(
    std::string& output,
    ComponentExecutionPermissions permissions)
{
    output.push_back('[');
    bool first = true;
    const auto append = [&](std::string_view name) {
        if (!first) output.push_back(',');
        first = false;
        appendString(output, name);
    };
    if ((permissions & componentPermission(ComponentExecutionPermission::RealtimeAudio)) != 0)
        append(realtimeAudioPermission);
    if ((permissions & componentPermission(ComponentExecutionPermission::ImmutableResourceRead)) != 0)
        append(immutableResourceReadPermission);
    if ((permissions & componentPermission(ComponentExecutionPermission::BoundedAudioStreamRead)) != 0)
        append(boundedAudioStreamReadPermission);
    if ((permissions & componentPermission(ComponentExecutionPermission::StateMigration)) != 0)
        append(stateMigrationPermission);
    if ((permissions & componentPermission(ComponentExecutionPermission::DeclarativeUi)) != 0)
        append(declarativeUiPermission);
    output.push_back(']');
}

[[nodiscard]] std::string writeCanonical(const ComponentBundleManifest& manifest)
{
    std::string output;
    output.reserve(4096);
    output.push_back('{');
    appendKey(output, "format"); appendString(output, componentBundleManifestFormat);
    output += ","; appendKey(output, "version"); output += "1";
    output += ","; appendKey(output, "bundleId"); appendString(output, manifest.bundleId);
    output += ","; appendKey(output, "semanticVersion"); appendString(output, manifest.semanticVersion);
    output += ","; appendKey(output, "engine");
    output.push_back('{');
    appendKey(output, "abiMinimum"); output += std::to_string(manifest.minimumEngineAbiVersion);
    output += ","; appendKey(output, "abiMaximum"); output += std::to_string(manifest.maximumEngineAbiVersion);
    output += ","; appendKey(output, "apiMinimum"); output += std::to_string(manifest.minimumEngineApiVersion);
    output += ","; appendKey(output, "apiMaximum"); output += std::to_string(manifest.maximumEngineApiVersion);
    output.push_back('}');
    output += ","; appendKey(output, "roles"); appendRoleArray(output, manifest.declaredRoles);
    output += ","; appendKey(output, "permissions"); appendPermissionArray(output, manifest.requestedPermissions);
    output += ","; appendKey(output, "entry"); appendPayload(output, manifest.entry);
    output += ","; appendKey(output, "files");
    appendArray(output, manifest.files, [](std::string& destination, const auto& file) {
        destination.push_back('{');
        appendKey(destination, "path"); appendString(destination, file.path);
        destination += ","; appendKey(destination, "purpose"); appendString(destination, filePurposeName(file.purpose));
        destination += ","; appendKey(destination, "bytes"); destination += std::to_string(file.byteCount);
        destination += ","; appendKey(destination, "sha256"); appendDigest(destination, file.sha256);
        destination.push_back('}');
    });
    output += ","; appendKey(output, "nodes");
    appendArray(output, manifest.nodes, [](std::string& destination, const auto& node) {
        destination.push_back('{');
        appendKey(destination, "typeId"); appendString(destination, node.typeId);
        destination += ","; appendKey(destination, "stateSchemaVersion"); destination += std::to_string(node.stateSchemaVersion);
        destination += ","; appendKey(destination, "inputs"); appendArray(destination, node.inputs, appendPort);
        destination += ","; appendKey(destination, "outputs"); appendArray(destination, node.outputs, appendPort);
        destination += ","; appendKey(destination, "parameters");
        appendArray(destination, node.parameters, [](std::string& parameterOutput, const auto& parameter) {
            parameterOutput.push_back('{');
            appendKey(parameterOutput, "id"); appendString(parameterOutput, parameter.id);
            parameterOutput += ","; appendKey(parameterOutput, "kind"); appendString(parameterOutput, parameterKindName(parameter.kind));
            parameterOutput += ","; appendKey(parameterOutput, "domain"); appendString(parameterOutput, domainName(parameter.domain));
            parameterOutput += ","; appendKey(parameterOutput, "choices");
            appendArray(parameterOutput, parameter.choices, [](std::string& choiceOutput, const auto& choice) {
                appendString(choiceOutput, choice);
            });
            parameterOutput.push_back('}');
        });
        destination.push_back('}');
    });
    output += ","; appendKey(output, "dependencies");
    appendArray(output, manifest.dependencies, [](std::string& destination, const auto& dependency) {
        destination.push_back('{');
        appendKey(destination, "bundleId"); appendString(destination, dependency.bundleId);
        destination += ","; appendKey(destination, "minimumVersion"); appendString(destination, dependency.minimumVersion);
        destination += ","; appendKey(destination, "maximumExclusiveVersion"); appendString(destination, dependency.maximumExclusiveVersion);
        destination += ","; appendKey(destination, "optional"); destination += dependency.optional ? "true" : "false";
        destination.push_back('}');
    });
    output += ","; appendKey(output, "state");
    output.push_back('{');
    appendKey(output, "currentSchemaVersion"); output += std::to_string(manifest.state.currentSchemaVersion);
    output += ","; appendKey(output, "migrations");
    appendArray(output, manifest.state.migrations, [](std::string& destination, const auto& migration) {
        destination.push_back('{');
        appendKey(destination, "from"); destination += std::to_string(migration.fromSchemaVersion);
        destination += ","; appendKey(destination, "to"); destination += std::to_string(migration.toSchemaVersion);
        destination += ","; appendKey(destination, "mode"); appendString(destination, "host-mediated");
        destination.push_back('}');
    });
    output.push_back('}');
    output += ","; appendKey(output, "provenance");
    output.push_back('{');
    appendKey(output, "publisher"); appendString(output, manifest.provenance.publisher);
    output += ","; appendKey(output, "licenseExpression"); appendString(output, manifest.provenance.licenseExpression);
    output += ","; appendKey(output, "notices");
    appendArray(output, manifest.provenance.notices, [](std::string& destination, const auto& notice) {
        appendString(destination, notice);
    });
    output.push_back('}');
    output += ","; appendKey(output, "declarativeUiPath");
    if (manifest.declarativeUiPath)
        appendString(output, *manifest.declarativeUiPath);
    else
        output += "null";
    output.push_back('}');
    return output;
}
}

ComponentBundleManifestParseResult parseComponentBundleManifest(
    std::string_view source)
{
    json::ParseLimits limits;
    limits.maximumInputBytes = maximumComponentBundleManifestBytes;
    limits.maximumNestingDepth = maximumComponentBundleManifestJsonDepth;
    limits.maximumValues = maximumComponentBundleManifestJsonValues;
    limits.maximumDecodedStringBytes = maximumComponentBundleManifestStringBytes;
    limits.maximumContainerElements = maximumComponentBundleManifestContainerElements;
    limits.requireValidUtf8 = true;
    const auto parsed = json::parse(source, limits);
    if (!parsed.ok())
    {
        ComponentBundleManifestParseResult result;
        const auto budget = !parsed.errors.empty()
            && parsed.errors.front().message.find("limit")
                != std::string::npos;
        addError(
            result.errors,
            budget
                ? ComponentBundleManifestErrorCode::BudgetExceeded
                : ComponentBundleManifestErrorCode::JsonInvalid,
            "$",
            budget
                ? "Component manifest JSON exceeds a parser budget"
                : "Component manifest JSON is malformed");
        return result;
    }
    Decoder decoder;
    return decoder.decode(parsed.value);
}

ComponentBundleManifestWriteResult writeCanonicalComponentBundleManifest(
    const ComponentBundleManifest& manifest)
{
    ComponentBundleManifestWriteResult result;
    if (!validateManifest(manifest, result.errors))
        return result;
    auto normalized = manifest;
    normalizeManifest(normalized);
    result.canonicalJson = writeCanonical(normalized);
    if (result.canonicalJson.size() > maximumComponentBundleManifestBytes)
    {
        result.canonicalJson.clear();
        addError(
            result.errors,
            ComponentBundleManifestErrorCode::BudgetExceeded,
            "$",
            "Canonical component manifest exceeds its byte limit");
    }
    return result;
}
}
