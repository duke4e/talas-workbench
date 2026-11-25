#include "talas/runtime/ComponentActivationProtocol.h"

#include "talas/project/ProjectAssetInspection.h"

#include <algorithm>
#include <cmath>
#include <bit>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>

namespace talas::runtime::component_activation
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

[[nodiscard]] constexpr bool checkedAdd(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result) noexcept
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return false;
    result = left + right;
    return true;
}

[[nodiscard]] ComponentActivationResourceSetBudgetDiagnosticV1
validateResourceSetBudgetMember(
    const ComponentActivationResourceSetBudgetMemberV1& member) noexcept
{
    if (member.sourceByteCount == 0
        || member.sourceByteCount
            > maximumComponentActivationResourceSetSourceBytesV1)
    {
        return ComponentActivationResourceSetBudgetDiagnosticV1::
            AggregateSourceBytesInvalid;
    }
    if (member.transportByteCount < member.sourceByteCount
        || member.transportByteCount
            > member.sourceByteCount
                + maximumComponentActivationResourceSetTransportPageBytesV1
                - 1U)
    {
        return ComponentActivationResourceSetBudgetDiagnosticV1::
            AggregateTransportBytesInvalid;
    }
    if (member.privateMappedByteCount < member.sourceByteCount
        || member.privateMappedByteCount
            > member.sourceByteCount
                + maximumComponentActivationResourceSetTransportPageBytesV1
                - 1U)
    {
        return ComponentActivationResourceSetBudgetDiagnosticV1::
            AggregatePrivateMappedBytesInvalid;
    }
    if (member.workingByteCount
            > maximumComponentActivationAudioStreamWorkingBytesV1
        || (member.audioStream && member.workingByteCount == 0)
        || (!member.audioStream && member.workingByteCount != 0))
    {
        return ComponentActivationResourceSetBudgetDiagnosticV1::
            AggregateWorkingBytesInvalid;
    }
    return ComponentActivationResourceSetBudgetDiagnosticV1::Valid;
}

template <std::size_t Size, std::size_t SourceSize>
[[nodiscard]] bool appendDigestBytes(
    std::array<std::uint8_t, Size>& destination,
    std::size_t& offset,
    const std::array<std::uint8_t, SourceSize>& source) noexcept
{
    if (offset > destination.size()
        || source.size() > destination.size() - offset)
        return false;
    std::copy(source.begin(), source.end(), destination.begin() + offset);
    offset += source.size();
    return true;
}

template <std::size_t Size>
[[nodiscard]] bool appendDigestText(
    std::array<std::uint8_t, Size>& destination,
    std::size_t& offset,
    const char* source,
    std::size_t sourceBytes) noexcept
{
    if (offset > destination.size()
        || sourceBytes > destination.size() - offset)
        return false;
    std::transform(
        source,
        source + sourceBytes,
        destination.begin() + offset,
        [](char value) { return static_cast<std::uint8_t>(value); });
    offset += sourceBytes;
    return true;
}

template <std::size_t Size, typename Integer>
[[nodiscard]] bool appendDigestInteger(
    std::array<std::uint8_t, Size>& destination,
    std::size_t& offset,
    Integer value) noexcept
{
    static_assert(std::is_unsigned_v<Integer>);
    if (offset > destination.size()
        || sizeof(Integer) > destination.size() - offset)
        return false;
    for (std::size_t index = 0; index < sizeof(Integer); ++index)
    {
        destination[offset++] = static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
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

[[nodiscard]] constexpr bool canonicalStableId(
    const std::array<
        char,
        maximumComponentActivationDescriptorIdBytesV1>& storage,
    std::uint32_t byteCount) noexcept
{
    const auto lower = [](char value) {
        return value >= 'a' && value <= 'z';
    };
    const auto digit = [](char value) {
        return value >= '0' && value <= '9';
    };
    if (byteCount == 0 || byteCount > storage.size()
        || !lower(storage.front()))
    {
        return false;
    }
    for (std::size_t index = 0; index < byteCount; ++index)
    {
        const auto character = storage[index];
        if (!lower(character) && !digit(character)
            && character != '-')
        {
            return false;
        }
    }
    for (std::size_t index = byteCount; index < storage.size(); ++index)
    {
        if (storage[index] != 0)
            return false;
    }
    return true;
}

[[nodiscard]] constexpr bool canonicalParameterText(
    const std::array<
        char,
        maximumComponentActivationParameterTextBytesV1>& storage,
    std::uint32_t byteCount) noexcept
{
    if (byteCount > storage.size())
        return false;
    for (std::size_t index = 0; index < byteCount; ++index)
    {
        if (storage[index] == 0)
            return false;
    }
    for (std::size_t index = byteCount; index < storage.size(); ++index)
    {
        if (storage[index] != 0)
            return false;
    }
    return true;
}

[[nodiscard]] constexpr std::uint8_t hexadecimalNibble(char value) noexcept
{
    if (value >= '0' && value <= '9')
        return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
        return static_cast<std::uint8_t>(value - 'a' + 10);
    return 0xffU;
}

[[nodiscard]] constexpr ComponentActivationProtocolDiagnosticV1
validateHeader(
    const ComponentActivationRecordHeaderV1& header,
    ComponentActivationRecordKindV1 kind,
    std::size_t recordBytes) noexcept
{
    if (header.magic != componentActivationMagicV1)
        return ComponentActivationProtocolDiagnosticV1::MagicMismatch;
    if (header.protocolVersion != componentActivationProtocolVersionV1)
        return ComponentActivationProtocolDiagnosticV1::ProtocolMismatch;
    if (header.recordKind != static_cast<std::uint16_t>(kind))
        return ComponentActivationProtocolDiagnosticV1::RecordKindMismatch;
    if (header.recordBytes != recordBytes)
        return ComponentActivationProtocolDiagnosticV1::RecordSizeMismatch;
    if (header.reserved0 != 0)
        return ComponentActivationProtocolDiagnosticV1::ReservedNonZero;
    return ComponentActivationProtocolDiagnosticV1::Valid;
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
            const auto character = value[index];
            if (!asciiLower(character) && !asciiDigit(character)
                && character != '-')
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

[[nodiscard]] constexpr bool knownState(
    ComponentActivationRuntimeStateV1 state) noexcept
{
    switch (state)
    {
        case ComponentActivationRuntimeStateV1::Sealed:
        case ComponentActivationRuntimeStateV1::Authorized:
        case ComponentActivationRuntimeStateV1::Prepared:
        case ComponentActivationRuntimeStateV1::CanaryPassed:
        case ComponentActivationRuntimeStateV1::Quarantined:
        case ComponentActivationRuntimeStateV1::Closed:
            return true;
    }
    return false;
}

[[nodiscard]] constexpr bool knownDiagnostic(
    ComponentActivationRuntimeDiagnosticV1 diagnostic) noexcept
{
    switch (diagnostic)
    {
        case ComponentActivationRuntimeDiagnosticV1::Ready:
        case ComponentActivationRuntimeDiagnosticV1::AuthorizationRejected:
        case ComponentActivationRuntimeDiagnosticV1::EntryRejected:
        case ComponentActivationRuntimeDiagnosticV1::LoadRejected:
        case ComponentActivationRuntimeDiagnosticV1::AbiRejected:
        case ComponentActivationRuntimeDiagnosticV1::PrepareRejected:
        case ComponentActivationRuntimeDiagnosticV1::CanaryRejected:
        case ComponentActivationRuntimeDiagnosticV1::DeadlineExpired:
        case ComponentActivationRuntimeDiagnosticV1::NonFiniteOutput:
        case ComponentActivationRuntimeDiagnosticV1::ProcessCrashed:
        case ComponentActivationRuntimeDiagnosticV1::ProtocolRejected:
        case ComponentActivationRuntimeDiagnosticV1::Revoked:
        case ComponentActivationRuntimeDiagnosticV1::Closed:
            return true;
    }
    return false;
}

[[nodiscard]] constexpr bool failureDiagnostic(
    ComponentActivationRuntimeDiagnosticV1 diagnostic) noexcept
{
    return diagnostic != ComponentActivationRuntimeDiagnosticV1::Ready
        && diagnostic != ComponentActivationRuntimeDiagnosticV1::Closed;
}

[[nodiscard]] constexpr bool validTransitionPair(
    ComponentActivationRecordKindV1 kind,
    ComponentActivationRuntimeStateV1 state,
    ComponentActivationRuntimeDiagnosticV1 diagnostic) noexcept
{
    const auto ready = diagnostic
        == ComponentActivationRuntimeDiagnosticV1::Ready;
    const auto quarantined = state
            == ComponentActivationRuntimeStateV1::Quarantined
        && failureDiagnostic(diagnostic);
    switch (kind)
    {
        case ComponentActivationRecordKindV1::AuthorizationAccepted:
            return state == ComponentActivationRuntimeStateV1::Authorized
                && ready;
        case ComponentActivationRecordKindV1::Prepared:
        case ComponentActivationRecordKindV1::Reset:
            return (state == ComponentActivationRuntimeStateV1::Prepared
                       && ready)
                || quarantined;
        case ComponentActivationRecordKindV1::Canary:
            return (state == ComponentActivationRuntimeStateV1::CanaryPassed
                       && ready)
                || quarantined;
        case ComponentActivationRecordKindV1::Heartbeat:
            return ((state == ComponentActivationRuntimeStateV1::Prepared
                        || state
                            == ComponentActivationRuntimeStateV1::CanaryPassed)
                       && ready)
                || quarantined;
        case ComponentActivationRecordKindV1::Teardown:
            return state == ComponentActivationRuntimeStateV1::Closed
                && diagnostic
                    == ComponentActivationRuntimeDiagnosticV1::Closed;
        case ComponentActivationRecordKindV1::Terminal:
            return quarantined
                || (state == ComponentActivationRuntimeStateV1::Closed
                    && diagnostic
                        == ComponentActivationRuntimeDiagnosticV1::Closed);
        case ComponentActivationRecordKindV1::SealedReady:
        case ComponentActivationRecordKindV1::SealedSession:
        case ComponentActivationRecordKindV1::Authorization:
        case ComponentActivationRecordKindV1::ImmutableResourceGrant:
        case ComponentActivationRecordKindV1::AudioStreamGrant:
        case ComponentActivationRecordKindV1::ResourceSetGrant:
        case ComponentActivationRecordKindV1::ResourceSetMember:
        case ComponentActivationRecordKindV1::Checkpoint:
            return false;
    }
    return false;
}

[[nodiscard]] constexpr bool commandKind(
    ComponentActivationRecordKindV1 kind) noexcept
{
    return kind == ComponentActivationRecordKindV1::Canary
        || kind == ComponentActivationRecordKindV1::Reset
        || kind == ComponentActivationRecordKindV1::Heartbeat
        || kind == ComponentActivationRecordKindV1::Teardown
        || kind == ComponentActivationRecordKindV1::Checkpoint;
}

[[nodiscard]] ComponentActivationSealedSessionV1 structuralSession(
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    ComponentActivationSealedSessionV1 session;
    session.header = componentActivationHeaderV1(
        ComponentActivationRecordKindV1::SealedSession,
        sizeof(session));
    session.profileVersion = componentActivationProfileVersionV1;
    session.supportedPermissions = authorization.requestedPermissions;
    session.safeguards = allComponentIsolationSafeguardsV1;
    session.runtimeSessionGeneration = authorization.runtimeSessionGeneration;
    session.sharedStorageIdentity = authorization.sharedStorageIdentity;
    session.sharedStorageBytes = 1;
    session.runtimeSessionNonce = authorization.runtimeSessionNonce;
    return session;
}
}

ComponentActivationResourceSetBudgetDiagnosticV1
validateComponentActivationResourceSetBudgetV1(
    const ComponentActivationResourceSetBudgetV1& budget) noexcept
{
    if (budget.memberCount == 0
        || budget.memberCount
            > maximumComponentActivationResourceSetMembersV1)
    {
        return ComponentActivationResourceSetBudgetDiagnosticV1::
            MemberCountInvalid;
    }
    if (budget.audioStreamCount
            > maximumComponentActivationResourceSetAudioStreamsV1
        || budget.audioStreamCount > budget.memberCount)
    {
        return ComponentActivationResourceSetBudgetDiagnosticV1::
            AudioStreamCountInvalid;
    }
    if (budget.aggregateSourceBytes == 0
        || budget.aggregateSourceBytes
            > maximumComponentActivationResourceSetSourceBytesV1)
    {
        return ComponentActivationResourceSetBudgetDiagnosticV1::
            AggregateSourceBytesInvalid;
    }
    const auto aggregateRoundingOverhead =
        static_cast<std::uint64_t>(budget.memberCount)
        * (maximumComponentActivationResourceSetTransportPageBytesV1 - 1U);
    std::uint64_t maximumDerivedMappedBytes = 0;
    if (!checkedAdd(
            budget.aggregateSourceBytes,
            aggregateRoundingOverhead,
            maximumDerivedMappedBytes))
    {
        return ComponentActivationResourceSetBudgetDiagnosticV1::
            ArithmeticOverflow;
    }
    if (budget.aggregateTransportBytes < budget.aggregateSourceBytes
        || budget.aggregateTransportBytes > maximumDerivedMappedBytes)
    {
        return ComponentActivationResourceSetBudgetDiagnosticV1::
            AggregateTransportBytesInvalid;
    }
    if (budget.aggregatePrivateMappedBytes < budget.aggregateSourceBytes
        || budget.aggregatePrivateMappedBytes > maximumDerivedMappedBytes)
    {
        return ComponentActivationResourceSetBudgetDiagnosticV1::
            AggregatePrivateMappedBytesInvalid;
    }
    const auto maximumDerivedWorkingBytes =
        static_cast<std::uint64_t>(budget.audioStreamCount)
        * maximumComponentActivationAudioStreamWorkingBytesV1;
    if (budget.aggregateWorkingBytes > maximumDerivedWorkingBytes
        || (budget.audioStreamCount == 0
            && budget.aggregateWorkingBytes != 0)
        || (budget.audioStreamCount != 0
            && budget.aggregateWorkingBytes == 0))
    {
        return ComponentActivationResourceSetBudgetDiagnosticV1::
            AggregateWorkingBytesInvalid;
    }
    return ComponentActivationResourceSetBudgetDiagnosticV1::Valid;
}

ComponentActivationResourceSetBudgetDiagnosticV1
tryAppendComponentActivationResourceSetBudgetMemberV1(
    ComponentActivationResourceSetBudgetV1& budget,
    const ComponentActivationResourceSetBudgetMemberV1& member) noexcept
{
    if (budget.memberCount
        >= maximumComponentActivationResourceSetMembersV1)
    {
        return ComponentActivationResourceSetBudgetDiagnosticV1::
            MemberCountInvalid;
    }
    if (member.audioStream
        && budget.audioStreamCount
            >= maximumComponentActivationResourceSetAudioStreamsV1)
    {
        return ComponentActivationResourceSetBudgetDiagnosticV1::
            AudioStreamCountInvalid;
    }

    ComponentActivationResourceSetBudgetV1 candidate = budget;
    if (!checkedAdd(
            candidate.aggregateSourceBytes,
            member.sourceByteCount,
            candidate.aggregateSourceBytes)
        || !checkedAdd(
            candidate.aggregateTransportBytes,
            member.transportByteCount,
            candidate.aggregateTransportBytes)
        || !checkedAdd(
            candidate.aggregatePrivateMappedBytes,
            member.privateMappedByteCount,
            candidate.aggregatePrivateMappedBytes)
        || !checkedAdd(
            candidate.aggregateWorkingBytes,
            member.workingByteCount,
            candidate.aggregateWorkingBytes))
    {
        return ComponentActivationResourceSetBudgetDiagnosticV1::
            ArithmeticOverflow;
    }

    const auto memberDiagnostic = validateResourceSetBudgetMember(member);
    if (memberDiagnostic
        != ComponentActivationResourceSetBudgetDiagnosticV1::Valid)
    {
        return memberDiagnostic;
    }
    ++candidate.memberCount;
    if (member.audioStream)
        ++candidate.audioStreamCount;
    const auto diagnostic =
        validateComponentActivationResourceSetBudgetV1(candidate);
    if (diagnostic
        != ComponentActivationResourceSetBudgetDiagnosticV1::Valid)
    {
        return diagnostic;
    }
    budget = candidate;
    return ComponentActivationResourceSetBudgetDiagnosticV1::Valid;
}

std::array<std::uint8_t, componentTrustDigestBytes>
calculateComponentActivationResourceSetDigestV1(
    const ComponentActivationResourceSetGrantV1& grant,
    std::span<const ComponentActivationResourceSetMemberV1> members) noexcept
{
    if (grant.memberCount == 0
        || grant.memberCount
            > maximumComponentActivationResourceSetMembersV1
        || members.size() != grant.memberCount)
    {
        return {};
    }

    std::array<std::uint8_t, 8192> material{};
    std::size_t offset = 0;
    const auto append16 = [&](std::uint16_t value) {
        return appendDigestInteger(material, offset, value);
    };
    const auto append32 = [&](std::uint32_t value) {
        return appendDigestInteger(material, offset, value);
    };
    const auto append64 = [&](std::uint64_t value) {
        return appendDigestInteger(material, offset, value);
    };
    constexpr std::array<std::uint8_t, 16> domain{
        'K', 'C', 'R', 'E', 'S', 'O', 'U', 'R',
        'C', 'E', 'S', 'E', 'T', 'V', '1', 0};
    if (!appendDigestBytes(material, offset, domain)
        || !appendDigestBytes(material, offset, grant.contentDigest)
        || !appendDigestBytes(material, offset, grant.validationNonce)
        || !appendDigestBytes(material, offset, grant.runtimeSessionNonce)
        || !appendDigestBytes(material, offset, grant.authorizationNonce)
        || !append64(grant.artifactGeneration)
        || !append64(grant.activationGeneration)
        || !append64(grant.runtimeSessionGeneration)
        || !append64(grant.sharedStorageIdentity)
        || !append64(grant.aggregateSourceBytes)
        || !append64(grant.aggregateTransportBytes)
        || !append64(grant.aggregatePrivateMappedBytes)
        || !append64(grant.aggregateWorkingBytes)
        || !append32(grant.profileVersion)
        || !append16(grant.memberCount)
        || !append16(grant.audioStreamCount))
    {
        return {};
    }

    for (const auto& member : members)
    {
        if (member.parameterIdBytes > member.parameterId.size()
            || member.resourceIdBytes > member.resourceId.size()
            || !appendDigestBytes(
                material, offset, member.memberGrantDigest)
            || !appendDigestBytes(
                material, offset, member.sourceContentDigest)
            || !appendDigestBytes(
                material, offset, member.mediatedPayloadDigest)
            || !append64(member.resourceGeneration)
            || !append64(member.sourceByteCount)
            || !append64(member.transportByteCount)
            || !append64(member.privateMappedByteCount)
            || !append64(member.workingByteCount)
            || !append32(member.parameterIdBytes)
            || !append32(member.resourceIdBytes)
            || !append32(member.memberKind)
            || !append16(member.memberOrdinal)
            || !append16(member.memberCount)
            || !appendDigestText(
                material,
                offset,
                member.parameterId.data(),
                member.parameterIdBytes)
            || !appendDigestText(
                material,
                offset,
                member.resourceId.data(),
                member.resourceIdBytes))
        {
            return {};
        }
    }
    return calculateComponentActivationByteDigestV1(
        std::span<const std::uint8_t>{material.data(), offset});
}

ComponentActivationProtocolDiagnosticV1
validateComponentActivationResourceSetGrantV1(
    const ComponentActivationResourceSetGrantV1& grant,
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session) noexcept
{
    const auto authority = validateComponentActivationAuthorizationV1(
        authorization, session);
    if (authority != ComponentActivationProtocolDiagnosticV1::Valid)
        return authority;
    const auto header = validateHeader(
        grant.header,
        ComponentActivationRecordKindV1::ResourceSetGrant,
        sizeof(grant));
    if (header != ComponentActivationProtocolDiagnosticV1::Valid)
        return header;
    if (!allZero(grant.reserved0))
        return ComponentActivationProtocolDiagnosticV1::ReservedNonZero;
    if (grant.profileVersion
            != componentActivationResourceSetProfileVersionV1
        || grant.memberCount > authorization.staticParameterCount)
    {
        return ComponentActivationProtocolDiagnosticV1::ProfileMismatch;
    }
    if (allZero(grant.resourceSetDigest)
        || grant.contentDigest != authorization.contentDigest)
    {
        return ComponentActivationProtocolDiagnosticV1::ResourceInvalid;
    }
    if (grant.validationNonce != authorization.validationNonce
        || grant.runtimeSessionNonce != authorization.runtimeSessionNonce
        || grant.authorizationNonce != authorization.authorizationNonce
        || grant.artifactGeneration != authorization.artifactGeneration
        || grant.activationGeneration != authorization.activationGeneration
        || grant.runtimeSessionGeneration
            != authorization.runtimeSessionGeneration
        || grant.sharedStorageIdentity != authorization.sharedStorageIdentity)
    {
        return ComponentActivationProtocolDiagnosticV1::
            SessionBindingMismatch;
    }
    const ComponentActivationResourceSetBudgetV1 budget{
        .aggregateSourceBytes = grant.aggregateSourceBytes,
        .aggregateTransportBytes = grant.aggregateTransportBytes,
        .aggregatePrivateMappedBytes = grant.aggregatePrivateMappedBytes,
        .aggregateWorkingBytes = grant.aggregateWorkingBytes,
        .memberCount = grant.memberCount,
        .audioStreamCount = grant.audioStreamCount,
    };
    return validateComponentActivationResourceSetBudgetV1(budget)
            == ComponentActivationResourceSetBudgetDiagnosticV1::Valid
        ? ComponentActivationProtocolDiagnosticV1::Valid
        : ComponentActivationProtocolDiagnosticV1::ResourceInvalid;
}

ComponentActivationProtocolDiagnosticV1
validateComponentActivationResourceSetMembersV1(
    const ComponentActivationResourceSetGrantV1& grant,
    std::span<const ComponentActivationResourceSetMemberV1> members) noexcept
{
    const auto grantHeader = validateHeader(
        grant.header,
        ComponentActivationRecordKindV1::ResourceSetGrant,
        sizeof(grant));
    if (grantHeader != ComponentActivationProtocolDiagnosticV1::Valid)
        return grantHeader;
    if (grant.profileVersion
            != componentActivationResourceSetProfileVersionV1
        || allZero(grant.resourceSetDigest)
        || !allZero(grant.reserved0)
        || members.size() != grant.memberCount
        || members.empty()
        || members.size() > maximumComponentActivationResourceSetMembersV1)
    {
        return ComponentActivationProtocolDiagnosticV1::ResourceInvalid;
    }

    ComponentActivationResourceSetBudgetV1 budget;
    for (std::size_t index = 0; index < members.size(); ++index)
    {
        const auto& member = members[index];
        const auto header = validateHeader(
            member.header,
            ComponentActivationRecordKindV1::ResourceSetMember,
            sizeof(member));
        if (header != ComponentActivationProtocolDiagnosticV1::Valid)
            return header;
        if (!allZero(member.reserved0))
            return ComponentActivationProtocolDiagnosticV1::ReservedNonZero;
        if (member.resourceSetDigest != grant.resourceSetDigest
            || allZero(member.memberGrantDigest)
            || allZero(member.sourceContentDigest)
            || allZero(member.mediatedPayloadDigest)
            || member.resourceGeneration == 0
            || member.memberOrdinal != index
            || member.memberCount != grant.memberCount
            || !canonicalStableId(
                member.parameterId, member.parameterIdBytes)
            || !canonicalText(member.resourceId, member.resourceIdBytes))
        {
            return ComponentActivationProtocolDiagnosticV1::ResourceInvalid;
        }
        const auto kind = static_cast<ComponentActivationResourceSetMemberKindV1>(
            member.memberKind);
        if (kind != ComponentActivationResourceSetMemberKindV1::Table
            && kind != ComponentActivationResourceSetMemberKindV1::Binary
            && kind != ComponentActivationResourceSetMemberKindV1::Audio)
        {
            return ComponentActivationProtocolDiagnosticV1::ResourceInvalid;
        }
        if ((kind == ComponentActivationResourceSetMemberKindV1::Binary
                || kind == ComponentActivationResourceSetMemberKindV1::Audio)
            && member.sourceContentDigest != member.mediatedPayloadDigest)
        {
            return ComponentActivationProtocolDiagnosticV1::ResourceInvalid;
        }
        const auto budgetDiagnostic =
            tryAppendComponentActivationResourceSetBudgetMemberV1(
                budget,
                {
                    .sourceByteCount = member.sourceByteCount,
                    .transportByteCount = member.transportByteCount,
                    .privateMappedByteCount = member.privateMappedByteCount,
                    .workingByteCount = member.workingByteCount,
                    .audioStream = kind
                        == ComponentActivationResourceSetMemberKindV1::Audio,
                });
        if (budgetDiagnostic
            != ComponentActivationResourceSetBudgetDiagnosticV1::Valid)
        {
            return ComponentActivationProtocolDiagnosticV1::ResourceInvalid;
        }
        const std::string_view parameterId{
            member.parameterId.data(), member.parameterIdBytes};
        const std::string_view resourceId{
            member.resourceId.data(), member.resourceIdBytes};
        for (std::size_t prior = 0; prior < index; ++prior)
        {
            const std::string_view priorParameter{
                members[prior].parameterId.data(),
                members[prior].parameterIdBytes};
            const std::string_view priorResource{
                members[prior].resourceId.data(),
                members[prior].resourceIdBytes};
            if (parameterId == priorParameter || resourceId == priorResource)
                return ComponentActivationProtocolDiagnosticV1::ResourceInvalid;
        }
    }
    if (budget.memberCount != grant.memberCount
        || budget.audioStreamCount != grant.audioStreamCount
        || budget.aggregateSourceBytes != grant.aggregateSourceBytes
        || budget.aggregateTransportBytes != grant.aggregateTransportBytes
        || budget.aggregatePrivateMappedBytes
            != grant.aggregatePrivateMappedBytes
        || budget.aggregateWorkingBytes != grant.aggregateWorkingBytes
        || calculateComponentActivationResourceSetDigestV1(grant, members)
            != grant.resourceSetDigest)
    {
        return ComponentActivationProtocolDiagnosticV1::ResourceInvalid;
    }
    return ComponentActivationProtocolDiagnosticV1::Valid;
}

ComponentActivationProtocolDiagnosticV1
validateComponentActivationSealedSessionV1(
    const ComponentActivationSealedSessionV1& session) noexcept
{
    const auto header = validateHeader(
        session.header,
        ComponentActivationRecordKindV1::SealedSession,
        sizeof(session));
    if (header != ComponentActivationProtocolDiagnosticV1::Valid)
        return header;
    if (session.reserved0 != 0 || !allZero(session.reserved1))
        return ComponentActivationProtocolDiagnosticV1::ReservedNonZero;
    if (session.profileVersion != componentActivationProfileVersionV1)
        return ComponentActivationProtocolDiagnosticV1::ProfileMismatch;
    const auto realtimePermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    const auto resourcePermissions = realtimePermissions
        | componentPermission(
            ComponentExecutionPermission::ImmutableResourceRead);
    const auto audioStreamPermissions = realtimePermissions
        | componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);
    const auto resourceSetPermissions = resourcePermissions
        | componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);
    if ((session.supportedPermissions != realtimePermissions
            && session.supportedPermissions != resourcePermissions
            && session.supportedPermissions != audioStreamPermissions
            && session.supportedPermissions != resourceSetPermissions)
        || session.safeguards != allComponentIsolationSafeguardsV1)
    {
        return ComponentActivationProtocolDiagnosticV1::PermissionsInvalid;
    }
    if (session.runtimeSessionGeneration == 0
        || session.sharedStorageIdentity == 0
        || session.sharedStorageBytes == 0
        || session.sharedStorageBytes
            > maximumComponentActivationSharedStorageBytesV1
        || allZero(session.runtimeSessionNonce))
    {
        return ComponentActivationProtocolDiagnosticV1::SessionBindingInvalid;
    }
    return ComponentActivationProtocolDiagnosticV1::Valid;
}

ComponentActivationProtocolDiagnosticV1
validateComponentActivationStaticParametersV1(
    std::span<const ComponentActivationStaticParameterV1> parameters)
    noexcept
{
    if (parameters.size() > maximumComponentActivationStaticParametersV1)
        return ComponentActivationProtocolDiagnosticV1::ParameterInvalid;
    for (const auto& parameter : parameters)
    {
        if (parameter.reserved0 != 0 || !allZero(parameter.reserved1)
            || !canonicalStableId(parameter.id, parameter.idBytes))
        {
            return ComponentActivationProtocolDiagnosticV1::ParameterInvalid;
        }
        const auto kind = static_cast<ComponentActivationStaticParameterKindV1>(
            parameter.kind);
        switch (kind)
        {
            case ComponentActivationStaticParameterKindV1::Scalar:
                if (!std::isfinite(parameter.scalarValue)
                    || parameter.integerValue != 0
                    || parameter.textBytes != 0
                    || !allZero(parameter.text))
                {
                    return ComponentActivationProtocolDiagnosticV1::
                        ParameterInvalid;
                }
                break;
            case ComponentActivationStaticParameterKindV1::Integer:
                if (parameter.scalarValue != 0.0
                    || parameter.textBytes != 0
                    || !allZero(parameter.text))
                {
                    return ComponentActivationProtocolDiagnosticV1::
                        ParameterInvalid;
                }
                break;
            case ComponentActivationStaticParameterKindV1::Text:
                if (parameter.scalarValue != 0.0
                    || parameter.integerValue != 0
                    || !canonicalParameterText(
                        parameter.text, parameter.textBytes))
                {
                    return ComponentActivationProtocolDiagnosticV1::
                        ParameterInvalid;
                }
                break;
            case ComponentActivationStaticParameterKindV1::Resource:
                if (parameter.scalarValue != 0.0
                    || parameter.integerValue != 0
                    || parameter.textBytes == 0
                    || parameter.textBytes
                        > maximumComponentActivationDescriptorIdBytesV1
                    || !canonicalParameterText(
                        parameter.text, parameter.textBytes))
                {
                    return ComponentActivationProtocolDiagnosticV1::
                        ParameterInvalid;
                }
                break;
            default:
                return ComponentActivationProtocolDiagnosticV1::
                    ParameterInvalid;
        }
    }
    return ComponentActivationProtocolDiagnosticV1::Valid;
}

std::array<std::uint8_t, componentTrustDigestBytes>
calculateComponentActivationByteDigestV1(
    std::span<const std::uint8_t> bytes) noexcept
{
    std::array<std::uint8_t, componentTrustDigestBytes> digest{};
    try
    {
        const auto hexadecimal = talas::project::calculateProjectAssetSha256(
            bytes);
        if (hexadecimal.size() != digest.size() * 2U)
            return {};
        for (std::size_t index = 0; index < digest.size(); ++index)
        {
            const auto high = hexadecimalNibble(hexadecimal[index * 2U]);
            const auto low = hexadecimalNibble(
                hexadecimal[index * 2U + 1U]);
            if (high > 0xfU || low > 0xfU)
                return {};
            digest[index] = static_cast<std::uint8_t>(
                static_cast<std::uint8_t>(high << 4U) | low);
        }
        return digest;
    }
    catch (...)
    {
        return {};
    }
}

std::array<std::uint8_t, componentTrustDigestBytes>
calculateComponentActivationStaticParameterDigestV1(
    std::span<const ComponentActivationStaticParameterV1> parameters)
    noexcept
{
    if (parameters.empty())
        return {};
    if (validateComponentActivationStaticParametersV1(parameters)
        != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        return {};
    }
    const auto raw = std::as_bytes(parameters);
    return calculateComponentActivationByteDigestV1(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(raw.data()),
            raw.size()});
}

std::array<std::uint8_t, componentTrustDigestBytes>
calculateComponentActivationAuthorizationDigestV1(
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    return calculateComponentActivationByteDigestV1(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(&authorization),
            sizeof(authorization)});
}

ComponentActivationProtocolDiagnosticV1
validateComponentActivationAuthorizationV1(
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session) noexcept
{
    const auto validSession =
        validateComponentActivationSealedSessionV1(session);
    if (validSession != ComponentActivationProtocolDiagnosticV1::Valid)
        return validSession;
    const auto header = validateHeader(
        authorization.header,
        ComponentActivationRecordKindV1::Authorization,
        sizeof(authorization));
    if (header != ComponentActivationProtocolDiagnosticV1::Valid)
        return header;
    if (!allZero(authorization.reserved1))
        return ComponentActivationProtocolDiagnosticV1::ReservedNonZero;
    if (!canonicalText(
            authorization.bundleId,
            authorization.bundleIdBytes)
        || !canonicalText(
            authorization.semanticVersion,
            authorization.semanticVersionBytes)
        || allZero(authorization.contentDigest)
        || allZero(authorization.signerIdentityDigest))
    {
        return ComponentActivationProtocolDiagnosticV1::CandidateInvalid;
    }

    ComponentApprovalFingerprint candidate;
    candidate.bundleId = {
        authorization.bundleId.data(), authorization.bundleIdBytes};
    candidate.semanticVersion = {
        authorization.semanticVersion.data(),
        authorization.semanticVersionBytes};
    candidate.contentDigest = authorization.contentDigest;
    candidate.signerIdentityDigest = authorization.signerIdentityDigest;
    candidate.requestedPermissions = authorization.requestedPermissions;
    candidate.policyVersion = authorization.policyVersion;
    candidate.hasContentDigest = true;
    candidate.hasSignerIdentity = true;
    const auto candidateMatch = matchComponentOwnerApproval(
        candidate, &candidate);
    if (candidateMatch.ownerApproval
            != ComponentOwnerApprovalState::ExactMatch
        || candidateMatch.diagnostic
            != ComponentOwnerApprovalMatchCode::ExactMatch)
    {
        return ComponentActivationProtocolDiagnosticV1::CandidateInvalid;
    }
    const auto realtimePermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    const auto resourcePermissions = realtimePermissions
        | componentPermission(
            ComponentExecutionPermission::ImmutableResourceRead);
    const auto audioStreamPermissions = realtimePermissions
        | componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);
    const auto resourceSetPermissions = resourcePermissions
        | componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);
    const auto realtimeRoles = componentRole(
        ComponentExecutionRole::RealtimeProcessor);
    const auto resourceRoles = realtimeRoles
        | componentRole(ComponentExecutionRole::ImmutableResourceConsumer);
    const auto audioStreamRoles = realtimeRoles
        | componentRole(
            ComponentExecutionRole::BoundedAudioStreamConsumer);
    const auto resourceSetRoles = resourceRoles
        | componentRole(
            ComponentExecutionRole::BoundedAudioStreamConsumer);
    const auto realtimeProfile =
        authorization.requestedPermissions == realtimePermissions
        && authorization.declaredRoles == realtimeRoles;
    const auto resourceProfile =
        authorization.requestedPermissions == resourcePermissions
        && authorization.declaredRoles == resourceRoles;
    const auto audioStreamProfile =
        authorization.requestedPermissions == audioStreamPermissions
        && authorization.declaredRoles == audioStreamRoles;
    const auto resourceSetProfile =
        authorization.requestedPermissions == resourceSetPermissions
        && authorization.declaredRoles == resourceSetRoles;
    if ((!realtimeProfile && !resourceProfile && !audioStreamProfile
            && !resourceSetProfile)
        || authorization.requestedPermissions
            != session.supportedPermissions)
    {
        return ComponentActivationProtocolDiagnosticV1::PermissionsInvalid;
    }
    if (!canonicalText(
            authorization.descriptorId,
            authorization.descriptorIdBytes))
    {
        return ComponentActivationProtocolDiagnosticV1::DescriptorInvalid;
    }
    const std::string_view descriptorId{
        authorization.descriptorId.data(), authorization.descriptorIdBytes};
    if (descriptorId.size() <= candidate.bundleId.size() + 1U
        || !descriptorId.starts_with(candidate.bundleId)
        || descriptorId[candidate.bundleId.size()] != '.'
        || !canonicalReverseDns(descriptorId))
    {
        return ComponentActivationProtocolDiagnosticV1::DescriptorInvalid;
    }
    if (allZero(authorization.entryDigest)
        || authorization.entryByteCount == 0
        || authorization.entryByteCount
            > maximumComponentActivationEntryBytesV1)
    {
        return ComponentActivationProtocolDiagnosticV1::EntryBindingInvalid;
    }
    if ((authorization.staticParameterCount == 0)
        != allZero(authorization.staticParameterDigest))
    {
        return ComponentActivationProtocolDiagnosticV1::ParameterInvalid;
    }
    if (!componentActivationSupportsSampleRateV1(authorization.sampleRate)
        || !componentActivationSupportsBlockFramesV1(
            authorization.blockFrames)
        || !componentActivationSupportsChannelCountV1(
            authorization.inputChannelCount)
        || !componentActivationSupportsChannelCountV1(
            authorization.outputChannelCount)
        || authorization.staticParameterCount
            > maximumComponentActivationStaticParametersV1
        || authorization.opaqueStateByteCount != 0)
    {
        return ComponentActivationProtocolDiagnosticV1::AudioProfileInvalid;
    }
    if (authorization.artifactGeneration == 0
        || authorization.activationGeneration == 0
        || allZero(authorization.validationNonce)
        || allZero(authorization.authorizationNonce)
        || authorization.runtimeSessionGeneration
            != session.runtimeSessionGeneration
        || authorization.sharedStorageIdentity
            != session.sharedStorageIdentity
        || authorization.runtimeSessionNonce
            != session.runtimeSessionNonce
        || authorization.authorizationNonce
            == authorization.runtimeSessionNonce
        || authorization.authorizationNonce
            == authorization.validationNonce
        || authorization.runtimeSessionNonce
            == authorization.validationNonce)
    {
        return ComponentActivationProtocolDiagnosticV1::SessionBindingMismatch;
    }
    return ComponentActivationProtocolDiagnosticV1::Valid;
}

ComponentActivationProtocolDiagnosticV1
encodeComponentActivationImmutableTableV1(
    std::span<const double> values,
    std::span<std::uint8_t> destination) noexcept
{
    if (values.empty()
        || values.size() > maximumComponentActivationImmutableTableElementsV1
        || destination.size() != values.size() * sizeof(double))
    {
        return ComponentActivationProtocolDiagnosticV1::ResourceInvalid;
    }
    for (const auto value : values)
    {
        if (!std::isfinite(value))
            return ComponentActivationProtocolDiagnosticV1::ResourceInvalid;
    }
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        const auto bits = std::bit_cast<std::uint64_t>(values[index]);
        for (std::size_t byte = 0; byte < sizeof(bits); ++byte)
        {
            destination[index * sizeof(bits) + byte] =
                static_cast<std::uint8_t>(bits >> (byte * 8U));
        }
    }
    return ComponentActivationProtocolDiagnosticV1::Valid;
}

ComponentActivationProtocolDiagnosticV1
copyComponentActivationImmutableBinaryV1(
    std::span<const std::uint8_t> bytes,
    std::span<std::uint8_t> destination) noexcept
{
    if (bytes.empty()
        || bytes.size() > maximumComponentActivationImmutableBinaryBytesV1
        || destination.size() != bytes.size())
    {
        return ComponentActivationProtocolDiagnosticV1::ResourceInvalid;
    }
    std::memmove(destination.data(), bytes.data(), bytes.size());
    return ComponentActivationProtocolDiagnosticV1::Valid;
}

ComponentActivationProtocolDiagnosticV1
validateComponentActivationImmutableResourceGrantV1(
    const ComponentActivationImmutableResourceGrantV1& grant,
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session,
    std::span<const ComponentActivationStaticParameterV1> parameters)
    noexcept
{
    const auto validAuthorization = validateComponentActivationAuthorizationV1(
        authorization, session);
    if (validAuthorization != ComponentActivationProtocolDiagnosticV1::Valid)
        return validAuthorization;
    const auto header = validateHeader(
        grant.header,
        ComponentActivationRecordKindV1::ImmutableResourceGrant,
        sizeof(grant));
    if (header != ComponentActivationProtocolDiagnosticV1::Valid)
        return header;
    if (!allZero(grant.reserved0))
        return ComponentActivationProtocolDiagnosticV1::ReservedNonZero;

    const auto expectedPermissions = componentPermission(
            ComponentExecutionPermission::RealtimeAudio)
        | componentPermission(
            ComponentExecutionPermission::ImmutableResourceRead);
    const auto expectedRoles = componentRole(
            ComponentExecutionRole::RealtimeProcessor)
        | componentRole(ComponentExecutionRole::ImmutableResourceConsumer);
    const auto resourceSetPermissions = expectedPermissions
        | componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);
    const auto resourceSetRoles = expectedRoles
        | componentRole(
            ComponentExecutionRole::BoundedAudioStreamConsumer);
    if (!((authorization.requestedPermissions == expectedPermissions
              && authorization.declaredRoles == expectedRoles)
            || (authorization.requestedPermissions == resourceSetPermissions
                && authorization.declaredRoles == resourceSetRoles)))
    {
        return ComponentActivationProtocolDiagnosticV1::PermissionsInvalid;
    }
    if (grant.contentDigest != authorization.contentDigest
        || grant.validationNonce != authorization.validationNonce
        || grant.runtimeSessionNonce != authorization.runtimeSessionNonce
        || grant.authorizationNonce != authorization.authorizationNonce
        || grant.artifactGeneration != authorization.artifactGeneration
        || grant.activationGeneration != authorization.activationGeneration
        || grant.runtimeSessionGeneration
            != authorization.runtimeSessionGeneration
        || grant.sharedStorageIdentity
            != authorization.sharedStorageIdentity)
    {
        return ComponentActivationProtocolDiagnosticV1::
            SessionBindingMismatch;
    }
    if (allZero(grant.sourceContentDigest)
        || allZero(grant.mediatedPayloadDigest)
        || grant.resourceGeneration == 0
        || grant.elementCount == 0
        || grant.byteCount == 0
        || grant.byteCount
            > maximumComponentActivationImmutableResourceBytesV1
        || !canonicalStableId(grant.parameterId, grant.parameterIdBytes)
        || !canonicalText(grant.resourceId, grant.resourceIdBytes))
    {
        return ComponentActivationProtocolDiagnosticV1::ResourceInvalid;
    }
    switch (static_cast<ComponentActivationImmutableResourceKindV1>(
        grant.resourceKind))
    {
        case ComponentActivationImmutableResourceKindV1::Table:
            if (grant.elementFormat
                    != static_cast<std::uint32_t>(
                        ComponentActivationImmutableResourceElementFormatV1::
                            Float64LittleEndian)
                || grant.elementCount
                    > maximumComponentActivationImmutableTableElementsV1
                || grant.elementCount
                    > std::numeric_limits<std::uint64_t>::max()
                        / sizeof(double)
                || grant.byteCount != grant.elementCount * sizeof(double))
            {
                return ComponentActivationProtocolDiagnosticV1::ResourceInvalid;
            }
            break;
        case ComponentActivationImmutableResourceKindV1::Binary:
            if (grant.elementFormat
                    != static_cast<std::uint32_t>(
                        ComponentActivationImmutableResourceElementFormatV1::
                            RawBytes)
                || grant.elementCount
                    > maximumComponentActivationImmutableBinaryBytesV1
                || grant.byteCount != grant.elementCount
                || grant.sourceContentDigest != grant.mediatedPayloadDigest)
            {
                return ComponentActivationProtocolDiagnosticV1::ResourceInvalid;
            }
            break;
        default:
            return ComponentActivationProtocolDiagnosticV1::ResourceInvalid;
    }
    if (parameters.size() != authorization.staticParameterCount
        || validateComponentActivationStaticParametersV1(parameters)
            != ComponentActivationProtocolDiagnosticV1::Valid
        || calculateComponentActivationStaticParameterDigestV1(parameters)
            != authorization.staticParameterDigest)
    {
        return ComponentActivationProtocolDiagnosticV1::ParameterInvalid;
    }
    const ComponentActivationStaticParameterV1* resource = nullptr;
    for (const auto& parameter : parameters)
    {
        if (parameter.kind != static_cast<std::uint32_t>(
                ComponentActivationStaticParameterKindV1::Resource))
        {
            continue;
        }
        if (std::string_view{parameter.id.data(), parameter.idBytes}
                == std::string_view{
                    grant.parameterId.data(), grant.parameterIdBytes}
            && std::string_view{parameter.text.data(), parameter.textBytes}
                == std::string_view{
                    grant.resourceId.data(), grant.resourceIdBytes})
        {
            if (resource != nullptr)
            {
                return ComponentActivationProtocolDiagnosticV1::
                    ResourceInvalid;
            }
            resource = &parameter;
        }
    }
    if (resource == nullptr)
    {
        return ComponentActivationProtocolDiagnosticV1::ResourceInvalid;
    }
    return ComponentActivationProtocolDiagnosticV1::Valid;
}

ComponentActivationProtocolDiagnosticV1
validateComponentActivationAudioStreamGrantV1(
    const ComponentActivationAudioStreamGrantV1& grant,
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session,
    std::span<const ComponentActivationStaticParameterV1> parameters)
    noexcept
{
    const auto validAuthorization = validateComponentActivationAuthorizationV1(
        authorization, session);
    if (validAuthorization != ComponentActivationProtocolDiagnosticV1::Valid)
        return validAuthorization;
    const auto header = validateHeader(
        grant.header,
        ComponentActivationRecordKindV1::AudioStreamGrant,
        sizeof(grant));
    if (header != ComponentActivationProtocolDiagnosticV1::Valid)
        return header;
    if (grant.reserved0 != 0 || !allZero(grant.reserved1))
        return ComponentActivationProtocolDiagnosticV1::ReservedNonZero;

    const auto expectedPermissions = componentPermission(
            ComponentExecutionPermission::RealtimeAudio)
        | componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);
    const auto expectedRoles = componentRole(
            ComponentExecutionRole::RealtimeProcessor)
        | componentRole(
            ComponentExecutionRole::BoundedAudioStreamConsumer);
    const auto resourceSetPermissions = expectedPermissions
        | componentPermission(
            ComponentExecutionPermission::ImmutableResourceRead);
    const auto resourceSetRoles = expectedRoles
        | componentRole(
            ComponentExecutionRole::ImmutableResourceConsumer);
    if (!((authorization.requestedPermissions == expectedPermissions
              && authorization.declaredRoles == expectedRoles)
            || (authorization.requestedPermissions == resourceSetPermissions
                && authorization.declaredRoles == resourceSetRoles)))
    {
        return ComponentActivationProtocolDiagnosticV1::PermissionsInvalid;
    }
    if (grant.contentDigest != authorization.contentDigest
        || grant.signerIdentityDigest
            != authorization.signerIdentityDigest
        || grant.validationNonce != authorization.validationNonce
        || grant.runtimeSessionNonce != authorization.runtimeSessionNonce
        || grant.authorizationNonce != authorization.authorizationNonce
        || grant.artifactGeneration != authorization.artifactGeneration
        || grant.activationGeneration != authorization.activationGeneration
        || grant.runtimeSessionGeneration
            != authorization.runtimeSessionGeneration
        || grant.sharedStorageIdentity
            != authorization.sharedStorageIdentity)
    {
        return ComponentActivationProtocolDiagnosticV1::
            SessionBindingMismatch;
    }
    if (allZero(grant.sourceContentDigest)
        || grant.resourceGeneration == 0
        || grant.sourceByteCount
            < minimumComponentActivationAudioSourceBytesV1
        || grant.sourceByteCount
            > maximumComponentActivationAudioSourceBytesV1
        || grant.sourceFrameCount == 0
        || !std::isfinite(grant.maximumSourceFrameIncrement)
        || grant.maximumSourceFrameIncrement <= 0.0
        || grant.maximumSourceFrameIncrement
            > maximumComponentActivationAudioSourceFrameIncrementV1
        || !canonicalStableId(grant.parameterId, grant.parameterIdBytes)
        || !canonicalText(grant.resourceId, grant.resourceIdBytes)
        || grant.sourceSampleRate == 0
        || grant.sourceSampleRate
            > maximumComponentActivationAudioSourceSampleRateV1
        || grant.streamProfileVersion
            != componentActivationAudioStreamProfileVersionV1
        || grant.sourceEncoding
            != static_cast<std::uint32_t>(
                ComponentActivationAudioSourceEncodingV1::Pcm16Wave)
        || grant.maximumReadFrames == 0
        || grant.maximumReadFrames
            > maximumComponentActivationAudioReadFramesV1
        || grant.decodeBlockFrames
            != componentActivationAudioDecodeBlockFramesV1
        || (grant.sourceChannelCount != 1
            && grant.sourceChannelCount != 2)
        || grant.maximumTouchedSourceBlocks
            != maximumComponentActivationAudioTouchedSourceBlocksV1
        || grant.requestCapacity
            != componentActivationAudioRequestCapacityV1
        || grant.responseCapacity
            != componentActivationAudioResponseCapacityV1
        || grant.workerCacheCapacity
            != componentActivationAudioWorkerCacheCapacityV1
        || grant.lowWatermarkBlocks
            != componentActivationAudioLowWatermarkBlocksV1
        || grant.highWatermarkBlocks
            != componentActivationAudioHighWatermarkBlocksV1)
    {
        return ComponentActivationProtocolDiagnosticV1::AudioStreamInvalid;
    }
    const auto bytesPerFrame = static_cast<std::uint64_t>(
        grant.sourceChannelCount) * 2U;
    const auto possiblePcmBytes = grant.sourceByteCount
        - minimumComponentActivationAudioSourceBytesV1;
    if (grant.sourceFrameCount > possiblePcmBytes / bytesPerFrame)
        return ComponentActivationProtocolDiagnosticV1::AudioStreamInvalid;

    if (parameters.size() != authorization.staticParameterCount
        || validateComponentActivationStaticParametersV1(parameters)
            != ComponentActivationProtocolDiagnosticV1::Valid
        || calculateComponentActivationStaticParameterDigestV1(parameters)
            != authorization.staticParameterDigest)
    {
        return ComponentActivationProtocolDiagnosticV1::ParameterInvalid;
    }
    const ComponentActivationStaticParameterV1* resource = nullptr;
    for (const auto& parameter : parameters)
    {
        if (parameter.kind != static_cast<std::uint32_t>(
                ComponentActivationStaticParameterKindV1::Resource))
        {
            continue;
        }
        if (std::string_view{parameter.id.data(), parameter.idBytes}
                == std::string_view{
                    grant.parameterId.data(), grant.parameterIdBytes}
            && std::string_view{parameter.text.data(), parameter.textBytes}
                == std::string_view{
                    grant.resourceId.data(), grant.resourceIdBytes})
        {
            if (resource != nullptr)
            {
                return ComponentActivationProtocolDiagnosticV1::
                    AudioStreamInvalid;
            }
            resource = &parameter;
        }
    }
    if (resource == nullptr)
    {
        return ComponentActivationProtocolDiagnosticV1::AudioStreamInvalid;
    }
    return ComponentActivationProtocolDiagnosticV1::Valid;
}

ComponentActivationProtocolDiagnosticV1
validateComponentActivationAudioStreamPrepareV3(
    const TalasKernelAbiImmutableAudioStreamPrepareV3& prepare,
    const ComponentActivationAudioStreamGrantV1& grant) noexcept
{
    const auto identityAbsent = prepare.generation == 0
        && std::all_of(
            std::begin(prepare.sourceContentDigest),
            std::end(prepare.sourceContentDigest),
            [](std::uint8_t value) { return value == 0U; })
        && prepare.channelCount == 0U
        && prepare.sampleRate == 0U
        && prepare.frameCount == 0U;
    const auto identityExact =
        prepare.generation == grant.resourceGeneration
        && std::memcmp(
            prepare.sourceContentDigest,
            grant.sourceContentDigest.data(),
            grant.sourceContentDigest.size()) == 0
        && prepare.channelCount == grant.sourceChannelCount
        && prepare.sampleRate == grant.sourceSampleRate
        && prepare.frameCount == grant.sourceFrameCount;
    if (grant.resourceIdBytes == 0
        || grant.resourceIdBytes > grant.resourceId.size()
        || prepare.structSize != sizeof(prepare)
        || prepare.apiVersion != TALAS_KERNEL_API_VERSION_V3
        || prepare.id.data == nullptr
        || prepare.id.byteCount != grant.resourceIdBytes
        || std::memcmp(
            prepare.id.data,
            grant.resourceId.data(),
            grant.resourceIdBytes)
            != 0
        || (!identityAbsent && !identityExact)
        || !std::isfinite(prepare.initialSourceFrame)
        || prepare.initialSourceFrame < 0.0
        || prepare.initialSourceFrame
            >= static_cast<double>(grant.sourceFrameCount)
        || !std::isfinite(prepare.maximumSourceFrameIncrement)
        || prepare.maximumSourceFrameIncrement <= 0.0
        || prepare.maximumSourceFrameIncrement
            > grant.maximumSourceFrameIncrement
        || prepare.maximumReadFrames == 0
        || prepare.maximumReadFrames > grant.maximumReadFrames
        || prepare.reserved0[0] != 0
        || prepare.reserved0[1] != 0)
    {
        return ComponentActivationProtocolDiagnosticV1::AudioStreamInvalid;
    }
    return ComponentActivationProtocolDiagnosticV1::Valid;
}

ComponentActivationProtocolDiagnosticV1
validateComponentActivationAudioStreamViewV3(
    const TalasKernelAbiImmutableAudioStreamViewV3& view,
    const TalasKernelAbiImmutableAudioStreamPrepareV3& prepare,
    const ComponentActivationAudioStreamGrantV1& grant) noexcept
{
    if (validateComponentActivationAudioStreamPrepareV3(prepare, grant)
        != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        return ComponentActivationProtocolDiagnosticV1::AudioStreamInvalid;
    }
    if (view.structSize != sizeof(view)
        || view.apiVersion != TALAS_KERNEL_API_VERSION_V3
        || view.id.data == nullptr
        || view.id.byteCount != prepare.id.byteCount
        || std::memcmp(
            view.id.data, prepare.id.data, prepare.id.byteCount)
            != 0
        || view.generation != grant.resourceGeneration
        || std::memcmp(
            view.sourceContentDigest,
            grant.sourceContentDigest.data(),
            sizeof(view.sourceContentDigest))
            != 0
        || view.channelCount != grant.sourceChannelCount
        || view.sampleRate != grant.sourceSampleRate
        || view.frameCount != grant.sourceFrameCount
        || view.streamToken == 0
        || view.maximumSourceFrameIncrement
            != prepare.maximumSourceFrameIncrement
        || view.maximumReadFrames != prepare.maximumReadFrames
        || view.reserved0[0] != 0
        || view.reserved0[1] != 0)
    {
        return ComponentActivationProtocolDiagnosticV1::AudioStreamInvalid;
    }
    return ComponentActivationProtocolDiagnosticV1::Valid;
}

ComponentActivationProtocolDiagnosticV1
validateComponentActivationAudioStreamReadV3(
    const TalasKernelAbiImmutableAudioStreamReadV3& read,
    const TalasKernelAbiImmutableAudioStreamViewV3& view) noexcept
{
    if (read.structSize != sizeof(read)
        || read.apiVersion != TALAS_KERNEL_API_VERSION_V3
        || read.streamToken == 0
        || read.streamToken != view.streamToken
        || !std::isfinite(read.absoluteSourceFrame)
        || read.absoluteSourceFrame < 0.0
        || !std::isfinite(read.sourceFrameIncrement)
        || read.sourceFrameIncrement <= 0.0
        || read.sourceFrameIncrement
            > view.maximumSourceFrameIncrement
        || read.frameCount == 0
        || read.frameCount > view.maximumReadFrames
        || read.frameCount
            > maximumComponentActivationAudioReadFramesV1
        || read.outputChannelCount != view.channelCount
        || read.outputChannelCount == 0
        || read.reserved0 != 0
        || read.interleavedOutput == nullptr
        || read.reserved1[0] != 0
        || read.reserved1[1] != 0
        || read.frameCount
            > std::numeric_limits<std::uint64_t>::max()
                / read.outputChannelCount
        || read.outputSampleCapacity
            != read.frameCount * read.outputChannelCount)
    {
        return ComponentActivationProtocolDiagnosticV1::AudioStreamInvalid;
    }

    const auto finalPosition = static_cast<long double>(
            read.absoluteSourceFrame)
        + static_cast<long double>(read.frameCount - 1U)
            * static_cast<long double>(read.sourceFrameIncrement);
    const auto maximumFrame = static_cast<long double>(
        std::numeric_limits<std::uint64_t>::max() - 1U);
    if (!std::isfinite(finalPosition) || finalPosition > maximumFrame)
        return ComponentActivationProtocolDiagnosticV1::AudioStreamInvalid;
    const auto firstFrame = static_cast<std::uint64_t>(
        std::floor(static_cast<long double>(read.absoluteSourceFrame)));
    const auto finalRightFrame = static_cast<std::uint64_t>(
        std::floor(finalPosition)) + 1U;
    const auto firstBlock = firstFrame
        / componentActivationAudioDecodeBlockFramesV1;
    const auto finalBlock = finalRightFrame
        / componentActivationAudioDecodeBlockFramesV1;
    if (finalBlock < firstBlock
        || finalBlock - firstBlock + 1U
            > maximumComponentActivationAudioTouchedSourceBlocksV1)
    {
        return ComponentActivationProtocolDiagnosticV1::AudioStreamInvalid;
    }
    return ComponentActivationProtocolDiagnosticV1::Valid;
}

ComponentActivationProtocolDiagnosticV1
validateComponentActivationAudioStreamReadResultV3(
    const TalasKernelAbiImmutableAudioStreamReadResultV3& result,
    const TalasKernelAbiImmutableAudioStreamReadV3& read) noexcept
{
    const auto expectedNextSourceFrame = read.absoluteSourceFrame
        + static_cast<double>(read.frameCount) * read.sourceFrameIncrement;
    if (result.structSize != sizeof(result)
        || result.frameCount != read.frameCount
        || result.missingFrameCount > result.frameCount
        || result.endOfStreamFrameCount > result.missingFrameCount
        || result.seekGeneration == 0
        || !std::isfinite(result.expectedNextSourceFrame)
        || !std::isfinite(expectedNextSourceFrame)
        || result.expectedNextSourceFrame != expectedNextSourceFrame
        || result.reserved0[0] != 0
        || result.reserved0[1] != 0)
    {
        return ComponentActivationProtocolDiagnosticV1::AudioStreamInvalid;
    }
    switch (result.outcome)
    {
        case TALAS_KERNEL_ABI_AUDIO_STREAM_READ_COMPLETE:
            if (result.missingFrameCount != 0
                || result.endOfStreamFrameCount != 0)
            {
                return ComponentActivationProtocolDiagnosticV1::
                    AudioStreamInvalid;
            }
            break;
        case TALAS_KERNEL_ABI_AUDIO_STREAM_READ_UNDERRUN:
            if (result.missingFrameCount == 0
                || result.endOfStreamFrameCount != 0)
            {
                return ComponentActivationProtocolDiagnosticV1::
                    AudioStreamInvalid;
            }
            break;
        case TALAS_KERNEL_ABI_AUDIO_STREAM_READ_END_OF_STREAM:
            if (result.endOfStreamFrameCount == 0)
            {
                return ComponentActivationProtocolDiagnosticV1::
                    AudioStreamInvalid;
            }
            break;
        default:
            return ComponentActivationProtocolDiagnosticV1::AudioStreamInvalid;
    }
    return ComponentActivationProtocolDiagnosticV1::Valid;
}

ComponentActivationProtocolDiagnosticV1
validateComponentActivationCommandV1(
    const ComponentActivationCommandV1& command,
    ComponentActivationRecordKindV1 expectedKind,
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    if (!commandKind(expectedKind))
        return ComponentActivationProtocolDiagnosticV1::RecordKindMismatch;
    const auto validAuthorization = validateComponentActivationAuthorizationV1(
        authorization, structuralSession(authorization));
    if (validAuthorization != ComponentActivationProtocolDiagnosticV1::Valid)
        return validAuthorization;
    const auto header = validateHeader(
        command.header, expectedKind, sizeof(command));
    if (header != ComponentActivationProtocolDiagnosticV1::Valid)
        return header;
    if (!allZero(command.reserved0))
        return ComponentActivationProtocolDiagnosticV1::ReservedNonZero;
    if (command.sequence == 0)
        return ComponentActivationProtocolDiagnosticV1::SequenceInvalid;
    if (command.contentDigest != authorization.contentDigest
        || command.artifactGeneration != authorization.artifactGeneration
        || command.activationGeneration != authorization.activationGeneration
        || command.runtimeSessionGeneration
            != authorization.runtimeSessionGeneration
        || command.sharedStorageIdentity
            != authorization.sharedStorageIdentity
        || command.validationNonce != authorization.validationNonce
        || command.runtimeSessionNonce != authorization.runtimeSessionNonce
        || command.authorizationNonce != authorization.authorizationNonce)
    {
        return ComponentActivationProtocolDiagnosticV1::SessionBindingMismatch;
    }
    return ComponentActivationProtocolDiagnosticV1::Valid;
}

bool validateComponentActivationCheckpointCommandV1(
    const ComponentActivationCheckpointCommandV1& request,
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    using Operation = ComponentActivationCheckpointOperationV1;
    if (validateComponentActivationCommandV1(request.command,
            ComponentActivationRecordKindV1::Checkpoint, authorization)
            != ComponentActivationProtocolDiagnosticV1::Valid
        || request.reserved0 != 0
        || request.command.sequence == std::numeric_limits<std::uint64_t>::max()
        || request.completedAudioSequence > std::numeric_limits<std::uint64_t>::max() / 2U)
        return false;
    switch (request.operation)
    {
        case Operation::Query:
            return request.completedAudioSequence == 0 && request.checkpointId == 0;
        case Operation::Begin:
            return request.checkpointId == request.command.sequence;
        case Operation::Restore:
        case Operation::Commit:
            return request.checkpointId != 0 && request.checkpointId < request.command.sequence;
    }
    return false;
}

bool validateComponentActivationCheckpointReplyV1(
    const ComponentActivationCheckpointReplyV1& reply,
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    using Operation = ComponentActivationCheckpointOperationV1;
    using Status = ComponentActivationCheckpointStatusV1;
    if (reply.reserved0 != 0
        || !validateComponentActivationCheckpointCommandV1(reply.request, authorization))
        return false;
    if (reply.status == Status::Fatal) return true;
    switch (reply.request.operation)
    {
        case Operation::Query:
            return reply.status == Status::Supported || reply.status == Status::Unsupported;
        case Operation::Begin:
            return reply.status == Status::Captured || reply.status == Status::Unsupported
                || reply.status == Status::CaptureFailed || reply.status == Status::InvalidBoundary;
        case Operation::Restore:
            return reply.status == Status::Restored || reply.status == Status::InvalidBoundary;
        case Operation::Commit:
            return reply.status == Status::Committed || reply.status == Status::InvalidBoundary;
    }
    return false;
}

bool matchesComponentActivationCheckpointReplyV1(
    const ComponentActivationCheckpointReplyV1& reply,
    const ComponentActivationCheckpointCommandV1& expected,
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    return validateComponentActivationCheckpointReplyV1(reply, authorization)
        && validateComponentActivationCheckpointCommandV1(expected, authorization)
        && std::memcmp(&reply.request, &expected, sizeof(expected)) == 0;
}

ComponentActivationCommandV1 makeComponentActivationCommandV1(
    const ComponentActivationAuthorizationV1& authorization,
    ComponentActivationRecordKindV1 kind,
    std::uint64_t sequence) noexcept
{
    ComponentActivationCommandV1 command;
    command.header = componentActivationHeaderV1(kind, sizeof(command));
    command.contentDigest = authorization.contentDigest;
    command.artifactGeneration = authorization.artifactGeneration;
    command.activationGeneration = authorization.activationGeneration;
    command.runtimeSessionGeneration = authorization.runtimeSessionGeneration;
    command.sharedStorageIdentity = authorization.sharedStorageIdentity;
    command.validationNonce = authorization.validationNonce;
    command.runtimeSessionNonce = authorization.runtimeSessionNonce;
    command.authorizationNonce = authorization.authorizationNonce;
    command.sequence = sequence;
    return command;
}

ComponentActivationProtocolDiagnosticV1
validateComponentActivationSealedTransitionV1(
    const ComponentActivationTransitionV1& transition,
    ComponentActivationRecordKindV1 expectedKind,
    const ComponentActivationSealedSessionV1& session) noexcept
{
    const auto validSession =
        validateComponentActivationSealedSessionV1(session);
    if (validSession != ComponentActivationProtocolDiagnosticV1::Valid)
        return validSession;
    if (expectedKind != ComponentActivationRecordKindV1::SealedReady
        && expectedKind != ComponentActivationRecordKindV1::Heartbeat)
    {
        return ComponentActivationProtocolDiagnosticV1::RecordKindMismatch;
    }
    const auto header = validateHeader(
        transition.header, expectedKind, sizeof(transition));
    if (header != ComponentActivationProtocolDiagnosticV1::Valid)
        return header;
    if (!allZero(transition.reserved0))
        return ComponentActivationProtocolDiagnosticV1::ReservedNonZero;
    if (transition.sequence == 0)
        return ComponentActivationProtocolDiagnosticV1::SequenceInvalid;
    if (!allZero(transition.contentDigest)
        || transition.artifactGeneration != 0
        || transition.activationGeneration != 0
        || !allZero(transition.validationNonce)
        || !allZero(transition.authorizationNonce)
        || transition.runtimeSessionGeneration
            != session.runtimeSessionGeneration
        || transition.sharedStorageIdentity
            != session.sharedStorageIdentity
        || transition.runtimeSessionNonce != session.runtimeSessionNonce)
    {
        return ComponentActivationProtocolDiagnosticV1::SessionBindingMismatch;
    }
    const auto state = static_cast<ComponentActivationRuntimeStateV1>(
        transition.state);
    const auto diagnostic =
        static_cast<ComponentActivationRuntimeDiagnosticV1>(
            transition.diagnostic);
    if (!knownState(state))
        return ComponentActivationProtocolDiagnosticV1::StateUnknown;
    if (!knownDiagnostic(diagnostic))
        return ComponentActivationProtocolDiagnosticV1::DiagnosticUnknown;
    if (state != ComponentActivationRuntimeStateV1::Sealed
        || diagnostic != ComponentActivationRuntimeDiagnosticV1::Ready)
    {
        return ComponentActivationProtocolDiagnosticV1::
            StateDiagnosticMismatch;
    }
    return ComponentActivationProtocolDiagnosticV1::Valid;
}

ComponentActivationProtocolDiagnosticV1
validateComponentActivationTransitionV1(
    const ComponentActivationTransitionV1& transition,
    ComponentActivationRecordKindV1 expectedKind,
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    const auto validAuthorization =
        validateComponentActivationAuthorizationV1(
            authorization, structuralSession(authorization));
    if (validAuthorization != ComponentActivationProtocolDiagnosticV1::Valid)
        return validAuthorization;
    const auto header = validateHeader(
        transition.header, expectedKind, sizeof(transition));
    if (header != ComponentActivationProtocolDiagnosticV1::Valid)
        return header;
    if (!allZero(transition.reserved0))
        return ComponentActivationProtocolDiagnosticV1::ReservedNonZero;
    if (transition.sequence == 0)
        return ComponentActivationProtocolDiagnosticV1::SequenceInvalid;
    if (transition.contentDigest != authorization.contentDigest
        || transition.artifactGeneration != authorization.artifactGeneration
        || transition.activationGeneration
            != authorization.activationGeneration
        || transition.runtimeSessionGeneration
            != authorization.runtimeSessionGeneration
        || transition.sharedStorageIdentity
            != authorization.sharedStorageIdentity
        || transition.validationNonce != authorization.validationNonce
        || transition.runtimeSessionNonce
            != authorization.runtimeSessionNonce
        || transition.authorizationNonce
            != authorization.authorizationNonce)
    {
        return ComponentActivationProtocolDiagnosticV1::SessionBindingMismatch;
    }
    const auto state = static_cast<ComponentActivationRuntimeStateV1>(
        transition.state);
    const auto diagnostic =
        static_cast<ComponentActivationRuntimeDiagnosticV1>(
            transition.diagnostic);
    if (!knownState(state))
        return ComponentActivationProtocolDiagnosticV1::StateUnknown;
    if (!knownDiagnostic(diagnostic))
        return ComponentActivationProtocolDiagnosticV1::DiagnosticUnknown;
    if (!validTransitionPair(expectedKind, state, diagnostic))
    {
        return ComponentActivationProtocolDiagnosticV1::
            StateDiagnosticMismatch;
    }
    return ComponentActivationProtocolDiagnosticV1::Valid;
}
}
