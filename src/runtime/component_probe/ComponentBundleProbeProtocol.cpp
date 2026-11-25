#include "runtime/component_probe/ComponentBundleProbeProtocol.h"

#include <limits>
#include <string_view>
#include <type_traits>

namespace talas::runtime::component_probe
{
namespace
{
template <typename Value, std::size_t Size>
[[nodiscard]] constexpr bool allZero(
    const std::array<Value, Size>& values) noexcept
{
    for (const auto value : values)
    {
        if (value != Value{})
            return false;
    }
    return true;
}

template <std::size_t Size>
[[nodiscard]] constexpr bool canonicalText(
    const std::array<char, Size>& storage,
    std::uint32_t byteCount) noexcept
{
    if (byteCount == 0 || byteCount > Size)
        return false;
    for (std::size_t index = 0; index < byteCount; ++index)
    {
        const auto byte = static_cast<unsigned char>(storage[index]);
        if (byte < 0x21 || byte > 0x7e)
            return false;
    }
    for (std::size_t index = byteCount; index < Size; ++index)
    {
        if (storage[index] != 0)
            return false;
    }
    return true;
}

[[nodiscard]] constexpr bool asciiLower(char value) noexcept
{
    return value >= 'a' && value <= 'z';
}

[[nodiscard]] constexpr bool asciiDigit(char value) noexcept
{
    return value >= '0' && value <= '9';
}

[[nodiscard]] constexpr bool canonicalReverseDns(
    std::string_view value) noexcept
{
    if (value.empty() || value.find('.') == std::string_view::npos)
        return false;
    std::size_t segmentStart = 0;
    while (segmentStart < value.size())
    {
        const auto separator = value.find('.', segmentStart);
        const auto segmentEnd = separator == std::string_view::npos
            ? value.size()
            : separator;
        if (segmentEnd == segmentStart
            || !asciiLower(value[segmentStart])
            || value[segmentEnd - 1U] == '-')
        {
            return false;
        }
        for (auto index = segmentStart; index < segmentEnd; ++index)
        {
            const auto valueByte = value[index];
            if (!asciiLower(valueByte) && !asciiDigit(valueByte)
                && valueByte != '-')
            {
                return false;
            }
        }
        if (separator == std::string_view::npos)
            return true;
        segmentStart = separator + 1U;
    }
    return false;
}

[[nodiscard]] constexpr bool ownedNodeId(
    std::string_view bundleId,
    std::string_view nodeId) noexcept
{
    return nodeId.size() > bundleId.size() + 1U
        && nodeId.starts_with(bundleId)
        && nodeId[bundleId.size()] == '.'
        && canonicalReverseDns(nodeId);
}

[[nodiscard]] constexpr ComponentBundleProbeProtocolDiagnosticV1
validateHeader(
    const ComponentBundleProbeRecordHeaderV1& header,
    ComponentBundleProbeRecordKindV1 kind,
    std::size_t recordBytes) noexcept
{
    if (header.magic != componentBundleProbeMagicV1)
        return ComponentBundleProbeProtocolDiagnosticV1::MagicMismatch;
    if (header.protocolVersion != componentBundleProbeProtocolVersionV1)
        return ComponentBundleProbeProtocolDiagnosticV1::ProtocolMismatch;
    if (header.recordKind != static_cast<std::uint16_t>(kind))
        return ComponentBundleProbeProtocolDiagnosticV1::RecordKindMismatch;
    if (header.recordBytes != recordBytes)
        return ComponentBundleProbeProtocolDiagnosticV1::RecordSizeMismatch;
    if (header.reserved0 != 0)
        return ComponentBundleProbeProtocolDiagnosticV1::ReservedNonZero;
    return ComponentBundleProbeProtocolDiagnosticV1::Valid;
}

[[nodiscard]] constexpr bool knownState(ComponentProbeState state) noexcept
{
    switch (state)
    {
        case ComponentProbeState::NotRun:
        case ComponentProbeState::Clean:
        case ComponentProbeState::Rejected:
        case ComponentProbeState::ControlledFatal:
        case ComponentProbeState::AbnormalTermination:
        case ComponentProbeState::TimedOut:
        case ComponentProbeState::ProtocolError:
            return true;
    }
    return false;
}

[[nodiscard]] constexpr bool knownDiagnostic(
    ComponentBundleProbeResultDiagnosticV1 diagnostic) noexcept
{
    switch (diagnostic)
    {
        case ComponentBundleProbeResultDiagnosticV1::Ready:
        case ComponentBundleProbeResultDiagnosticV1::RequestRejected:
        case ComponentBundleProbeResultDiagnosticV1::EntryRejected:
        case ComponentBundleProbeResultDiagnosticV1::AbiRejected:
        case ComponentBundleProbeResultDiagnosticV1::LoadRejected:
        case ComponentBundleProbeResultDiagnosticV1::EntrySymbolMissing:
        case ComponentBundleProbeResultDiagnosticV1::MetadataRejected:
        case ComponentBundleProbeResultDiagnosticV1::UnloadRejected:
        case ComponentBundleProbeResultDiagnosticV1::ModuleControlledFatal:
        case ComponentBundleProbeResultDiagnosticV1::ProcessCrashed:
        case ComponentBundleProbeResultDiagnosticV1::DeadlineExpired:
        case ComponentBundleProbeResultDiagnosticV1::ProtocolRejected:
            return true;
    }
    return false;
}

[[nodiscard]] constexpr bool matchingStateDiagnostic(
    ComponentProbeState state,
    ComponentBundleProbeResultDiagnosticV1 diagnostic) noexcept
{
    switch (state)
    {
        case ComponentProbeState::Clean:
            return diagnostic == ComponentBundleProbeResultDiagnosticV1::Ready;
        case ComponentProbeState::Rejected:
            return diagnostic
                    == ComponentBundleProbeResultDiagnosticV1::RequestRejected
                || diagnostic
                    == ComponentBundleProbeResultDiagnosticV1::EntryRejected
                || diagnostic
                    == ComponentBundleProbeResultDiagnosticV1::AbiRejected
                || diagnostic
                    == ComponentBundleProbeResultDiagnosticV1::LoadRejected
                || diagnostic
                    == ComponentBundleProbeResultDiagnosticV1::EntrySymbolMissing
                || diagnostic
                    == ComponentBundleProbeResultDiagnosticV1::MetadataRejected
                || diagnostic
                    == ComponentBundleProbeResultDiagnosticV1::UnloadRejected;
        case ComponentProbeState::ControlledFatal:
            return diagnostic
                == ComponentBundleProbeResultDiagnosticV1::ModuleControlledFatal;
        case ComponentProbeState::AbnormalTermination:
            return diagnostic
                == ComponentBundleProbeResultDiagnosticV1::ProcessCrashed;
        case ComponentProbeState::TimedOut:
            return diagnostic
                == ComponentBundleProbeResultDiagnosticV1::DeadlineExpired;
        case ComponentProbeState::ProtocolError:
            return diagnostic
                == ComponentBundleProbeResultDiagnosticV1::ProtocolRejected;
        case ComponentProbeState::NotRun:
            return false;
    }
    return false;
}
}

ComponentBundleProbeProtocolDiagnosticV1
validateComponentBundleProbeRequestV1(
    const ComponentBundleProbeRequestV1& request) noexcept
{
    const auto header = validateHeader(
        request.header,
        ComponentBundleProbeRecordKindV1::Request,
        sizeof(request));
    if (header != ComponentBundleProbeProtocolDiagnosticV1::Valid)
        return header;
    if (request.reserved0 != 0 || !allZero(request.reserved1))
        return ComponentBundleProbeProtocolDiagnosticV1::ReservedNonZero;
    if (!canonicalText(request.bundleId, request.bundleIdBytes)
        || !canonicalText(
            request.semanticVersion,
            request.semanticVersionBytes)
        || allZero(request.contentDigest)
        || allZero(request.signerIdentityDigest)
        || request.policyVersion != currentComponentTrustPolicyVersion)
    {
        return ComponentBundleProbeProtocolDiagnosticV1::CandidateInvalid;
    }
    if (request.requestedPermissions == 0
        || (request.requestedPermissions
                & ~allComponentExecutionPermissionsV1)
            != 0
        || request.declaredRoles == 0
        || (request.declaredRoles & ~allComponentExecutionRolesV1) != 0
        || request.requestedPermissions
            != componentPermissionsForRoles(request.declaredRoles))
    {
        return ComponentBundleProbeProtocolDiagnosticV1::PermissionsInvalid;
    }
    if (request.entryByteCount == 0
        || request.entryByteCount > maximumComponentBundleProbeEntryBytesV1)
    {
        return ComponentBundleProbeProtocolDiagnosticV1::EntryBudgetInvalid;
    }
    if (request.artifactGeneration == 0
        || allZero(request.validationNonce))
    {
        return ComponentBundleProbeProtocolDiagnosticV1::SessionBindingInvalid;
    }
    return ComponentBundleProbeProtocolDiagnosticV1::Valid;
}

ComponentBundleProbeProtocolDiagnosticV1
validateComponentBundleProbeExpectedAbiV1(
    const ComponentBundleProbeExpectedAbiV1& expected,
    const ComponentBundleProbeRequestV1& request) noexcept
{
    const auto validRequest = validateComponentBundleProbeRequestV1(request);
    if (validRequest != ComponentBundleProbeProtocolDiagnosticV1::Valid)
        return validRequest;
    const auto header = validateHeader(
        expected.header,
        ComponentBundleProbeRecordKindV1::ExpectedAbi,
        sizeof(expected));
    if (header != ComponentBundleProbeProtocolDiagnosticV1::Valid)
        return header;
    if (expected.reserved0 != 0 || !allZero(expected.reserved1))
        return ComponentBundleProbeProtocolDiagnosticV1::ReservedNonZero;
    if (allZero(expected.entryDigest) || expected.entryByteCount == 0
        || expected.entryByteCount
            > maximumComponentBundleProbeEntryBytesV1)
    {
        return ComponentBundleProbeProtocolDiagnosticV1::EntryBindingInvalid;
    }
    if (expected.entryByteCount != request.entryByteCount)
    {
        return ComponentBundleProbeProtocolDiagnosticV1::EntryBindingMismatch;
    }
    if (expected.minimumEngineAbiVersion == 0
        || expected.maximumEngineAbiVersion
            < expected.minimumEngineAbiVersion
        || expected.minimumEngineApiVersion == 0
        || expected.maximumEngineApiVersion
            < expected.minimumEngineApiVersion)
    {
        return ComponentBundleProbeProtocolDiagnosticV1::ExpectedAbiInvalid;
    }
    if (expected.nodeCount == 0
        || expected.nodeCount > maximumComponentBundleProbeNodesV1)
    {
        return ComponentBundleProbeProtocolDiagnosticV1::NodeSetInvalid;
    }

    const std::string_view bundleId{
        request.bundleId.data(),
        request.bundleIdBytes,
    };
    for (std::size_t index = 0;
         index < maximumComponentBundleProbeNodesV1;
         ++index)
    {
        if (index >= expected.nodeCount)
        {
            if (expected.nodeIdBytes[index] != 0
                || !allZero(expected.nodeIds[index]))
            {
                return ComponentBundleProbeProtocolDiagnosticV1::NodeSetInvalid;
            }
            continue;
        }
        if (!canonicalText(
                expected.nodeIds[index],
                expected.nodeIdBytes[index]))
        {
            return ComponentBundleProbeProtocolDiagnosticV1::NodeSetInvalid;
        }
        const std::string_view nodeId{
            expected.nodeIds[index].data(),
            expected.nodeIdBytes[index],
        };
        if (!ownedNodeId(bundleId, nodeId))
            return ComponentBundleProbeProtocolDiagnosticV1::NodeSetInvalid;
        for (std::size_t previous = 0; previous < index; ++previous)
        {
            const std::string_view previousId{
                expected.nodeIds[previous].data(),
                expected.nodeIdBytes[previous],
            };
            if (nodeId == previousId)
                return ComponentBundleProbeProtocolDiagnosticV1::NodeSetInvalid;
        }
    }
    return ComponentBundleProbeProtocolDiagnosticV1::Valid;
}

ComponentBundleProbeProtocolDiagnosticV1
validateComponentBundleProbeStartedV1(
    const ComponentBundleProbeStartedV1& started,
    const ComponentBundleProbeRequestV1& request) noexcept
{
    const auto validRequest = validateComponentBundleProbeRequestV1(request);
    if (validRequest != ComponentBundleProbeProtocolDiagnosticV1::Valid)
        return validRequest;
    const auto header = validateHeader(
        started.header,
        ComponentBundleProbeRecordKindV1::Started,
        sizeof(started));
    if (header != ComponentBundleProbeProtocolDiagnosticV1::Valid)
        return header;
    if (started.reserved0 != 0)
        return ComponentBundleProbeProtocolDiagnosticV1::ReservedNonZero;
    if (started.processIdentifier == 0
        || started.processIdentifier
            > static_cast<std::uint32_t>(
                std::numeric_limits<std::int32_t>::max())
        || started.startSeconds == 0 || started.startMicroseconds >= 1'000'000)
    {
        return ComponentBundleProbeProtocolDiagnosticV1::
            ProcessIdentityInvalid;
    }
    if (started.contentDigest != request.contentDigest)
        return ComponentBundleProbeProtocolDiagnosticV1::CandidateMismatch;
    if (started.artifactGeneration != request.artifactGeneration
        || started.validationNonce != request.validationNonce)
    {
        return ComponentBundleProbeProtocolDiagnosticV1::
            SessionBindingMismatch;
    }
    return ComponentBundleProbeProtocolDiagnosticV1::Valid;
}

ComponentBundleProbeProtocolDiagnosticV1
validateComponentBundleProbeAcknowledgementV1(
    const ComponentBundleProbeAcknowledgementV1& acknowledgement,
    const ComponentBundleProbeRequestV1& request) noexcept
{
    const auto validRequest = validateComponentBundleProbeRequestV1(request);
    if (validRequest != ComponentBundleProbeProtocolDiagnosticV1::Valid)
        return validRequest;
    const auto header = validateHeader(
        acknowledgement.header,
        ComponentBundleProbeRecordKindV1::Acknowledgement,
        sizeof(acknowledgement));
    if (header != ComponentBundleProbeProtocolDiagnosticV1::Valid)
        return header;
    if (!allZero(acknowledgement.reserved0))
        return ComponentBundleProbeProtocolDiagnosticV1::ReservedNonZero;
    if (acknowledgement.contentDigest != request.contentDigest)
        return ComponentBundleProbeProtocolDiagnosticV1::CandidateMismatch;
    if (acknowledgement.artifactGeneration != request.artifactGeneration
        || acknowledgement.validationNonce != request.validationNonce)
    {
        return ComponentBundleProbeProtocolDiagnosticV1::
            SessionBindingMismatch;
    }
    return ComponentBundleProbeProtocolDiagnosticV1::Valid;
}

ComponentBundleProbeProtocolDiagnosticV1
validateComponentBundleProbeFinalizationV1(
    const ComponentBundleProbeFinalizationV1& finalization,
    const ComponentBundleProbeRequestV1& request) noexcept
{
    const auto validRequest = validateComponentBundleProbeRequestV1(request);
    if (validRequest != ComponentBundleProbeProtocolDiagnosticV1::Valid)
        return validRequest;
    const auto header = validateHeader(
        finalization.header,
        ComponentBundleProbeRecordKindV1::Finalization,
        sizeof(finalization));
    if (header != ComponentBundleProbeProtocolDiagnosticV1::Valid)
        return header;
    if (!allZero(finalization.reserved0))
        return ComponentBundleProbeProtocolDiagnosticV1::ReservedNonZero;
    if (finalization.contentDigest != request.contentDigest)
        return ComponentBundleProbeProtocolDiagnosticV1::CandidateMismatch;
    if (finalization.artifactGeneration != request.artifactGeneration
        || finalization.validationNonce != request.validationNonce)
    {
        return ComponentBundleProbeProtocolDiagnosticV1::
            SessionBindingMismatch;
    }
    return ComponentBundleProbeProtocolDiagnosticV1::Valid;
}

ComponentBundleProbeProtocolDiagnosticV1
validateComponentBundleProbeTerminationReadyV1(
    const ComponentBundleProbeTerminationReadyV1& ready,
    const ComponentBundleProbeRequestV1& request) noexcept
{
    const auto validRequest = validateComponentBundleProbeRequestV1(request);
    if (validRequest != ComponentBundleProbeProtocolDiagnosticV1::Valid)
        return validRequest;
    const auto header = validateHeader(
        ready.header,
        ComponentBundleProbeRecordKindV1::TerminationReady,
        sizeof(ready));
    if (header != ComponentBundleProbeProtocolDiagnosticV1::Valid)
        return header;
    if (!allZero(ready.reserved0))
        return ComponentBundleProbeProtocolDiagnosticV1::ReservedNonZero;
    if (ready.contentDigest != request.contentDigest)
        return ComponentBundleProbeProtocolDiagnosticV1::CandidateMismatch;
    if (ready.artifactGeneration != request.artifactGeneration
        || ready.validationNonce != request.validationNonce)
    {
        return ComponentBundleProbeProtocolDiagnosticV1::
            SessionBindingMismatch;
    }
    return ComponentBundleProbeProtocolDiagnosticV1::Valid;
}

ComponentBundleProbeProtocolDiagnosticV1
validateComponentBundleProbeResultV1(
    const ComponentBundleProbeResultV1& result,
    const ComponentBundleProbeRequestV1& request) noexcept
{
    const auto validRequest = validateComponentBundleProbeRequestV1(request);
    if (validRequest != ComponentBundleProbeProtocolDiagnosticV1::Valid)
        return validRequest;
    const auto header = validateHeader(
        result.header,
        ComponentBundleProbeRecordKindV1::Result,
        sizeof(result));
    if (header != ComponentBundleProbeProtocolDiagnosticV1::Valid)
        return header;
    if (result.reserved0 != 0 || result.reserved1 != 0
        || !allZero(result.reserved2))
    {
        return ComponentBundleProbeProtocolDiagnosticV1::ReservedNonZero;
    }
    const auto state = static_cast<ComponentProbeState>(result.state);
    const auto diagnostic = static_cast<ComponentBundleProbeResultDiagnosticV1>(
        result.diagnostic);
    if (!knownState(state) || state == ComponentProbeState::NotRun)
        return ComponentBundleProbeProtocolDiagnosticV1::StateUnknown;
    if (!knownDiagnostic(diagnostic))
        return ComponentBundleProbeProtocolDiagnosticV1::DiagnosticUnknown;
    if (!matchingStateDiagnostic(state, diagnostic))
    {
        return ComponentBundleProbeProtocolDiagnosticV1::StateDiagnosticMismatch;
    }
    if (result.contentDigest != request.contentDigest)
        return ComponentBundleProbeProtocolDiagnosticV1::CandidateMismatch;
    if (result.artifactGeneration != request.artifactGeneration
        || result.validationNonce != request.validationNonce)
    {
        return ComponentBundleProbeProtocolDiagnosticV1::SessionBindingMismatch;
    }
    return ComponentBundleProbeProtocolDiagnosticV1::Valid;
}

static_assert(std::is_trivially_copyable_v<ComponentBundleProbeRecordHeaderV1>);
static_assert(std::is_trivially_copyable_v<ComponentBundleProbeRequestV1>);
static_assert(std::is_trivially_copyable_v<ComponentBundleProbeResultV1>);
static_assert(std::is_trivially_copyable_v<ComponentBundleProbeStartedV1>);
static_assert(
    std::is_trivially_copyable_v<ComponentBundleProbeAcknowledgementV1>);
static_assert(
    std::is_trivially_copyable_v<ComponentBundleProbeFinalizationV1>);
static_assert(
    std::is_trivially_copyable_v<ComponentBundleProbeTerminationReadyV1>);
static_assert(
    std::is_trivially_copyable_v<ComponentBundleProbeExpectedAbiV1>);
}
