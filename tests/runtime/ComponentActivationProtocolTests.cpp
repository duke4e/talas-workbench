#include "talas/runtime/ComponentActivationProtocol.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
namespace activation = talas::runtime::component_activation;
using namespace talas::runtime;

static_assert(std::is_standard_layout_v<
              activation::ComponentActivationResourceSetGrantV1>);
static_assert(std::is_trivially_copyable_v<
              activation::ComponentActivationResourceSetGrantV1>);
static_assert(std::is_standard_layout_v<
              activation::ComponentActivationResourceSetMemberV1>);
static_assert(std::is_trivially_copyable_v<
              activation::ComponentActivationResourceSetMemberV1>);

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

activation::ComponentActivationSealedSessionV1 resourceSession();
activation::ComponentActivationAuthorizationV1 resourceAuthorization(
    const activation::ComponentActivationSealedSessionV1& session);
activation::ComponentActivationImmutableResourceGrantV1 resourceGrant(
    const activation::ComponentActivationAuthorizationV1& authority);
activation::ComponentActivationImmutableResourceGrantV1 binaryResourceGrant(
    const activation::ComponentActivationAuthorizationV1& authority);

void testAggregateResourceSetBudgetIsClosedAndFailureAtomic()
{
    require(
        activation::maximumComponentActivationResourceSetMembersV1 == 8
            && activation::maximumComponentActivationResourceSetAudioStreamsV1
                == 2
            && activation::maximumComponentActivationResourceSetSourceBytesV1
                == 64U * 1024U * 1024U,
        "the multiple-resource profile must keep its exact 8/2/64 MiB limits");

    activation::ComponentActivationResourceSetBudgetV1 budget;
    const activation::ComponentActivationResourceSetBudgetMemberV1 table{
        .sourceByteCount = 1024U * 1024U,
        .transportByteCount = 1024U * 1024U,
        .privateMappedByteCount = 1024U * 1024U,
    };
    require(
        activation::tryAppendComponentActivationResourceSetBudgetMemberV1(
            budget, table)
            == activation::ComponentActivationResourceSetBudgetDiagnosticV1::
                Valid,
        "one exact immutable member should enter the aggregate budget");

    const activation::ComponentActivationResourceSetBudgetMemberV1 audio{
        .sourceByteCount = 63U * 1024U * 1024U / 2U,
        .transportByteCount = 63U * 1024U * 1024U / 2U + 4095U,
        .privateMappedByteCount = 63U * 1024U * 1024U / 2U + 4095U,
        .workingByteCount = 1024U * 1024U,
        .audioStream = true,
    };
    require(
        activation::tryAppendComponentActivationResourceSetBudgetMemberV1(
            budget, audio)
                == activation::ComponentActivationResourceSetBudgetDiagnosticV1::
                    Valid
            && activation::tryAppendComponentActivationResourceSetBudgetMemberV1(
                   budget, audio)
                == activation::ComponentActivationResourceSetBudgetDiagnosticV1::
                    Valid
            && budget.memberCount == 3 && budget.audioStreamCount == 2
            && budget.aggregateSourceBytes
                == activation::maximumComponentActivationResourceSetSourceBytesV1,
        "two Audio members and 64 MiB total should meet the exact boundary");
    require(
        activation::validateComponentActivationResourceSetBudgetV1(budget)
            == activation::ComponentActivationResourceSetBudgetDiagnosticV1::
                Valid,
        "the exact aggregate boundary should validate");

    const auto accepted = budget;
    require(
        activation::tryAppendComponentActivationResourceSetBudgetMemberV1(
            budget,
            activation::ComponentActivationResourceSetBudgetMemberV1{
                .sourceByteCount = 1,
                .transportByteCount = 1,
                .privateMappedByteCount = 1,
                .workingByteCount = 1,
                .audioStream = true,
            })
                == activation::ComponentActivationResourceSetBudgetDiagnosticV1::
                    AudioStreamCountInvalid
            && budget == accepted,
        "a third Audio member must reject without mutating the accepted budget");

    auto countBoundary = activation::ComponentActivationResourceSetBudgetV1{};
    for (std::uint16_t index = 0;
         index < activation::maximumComponentActivationResourceSetMembersV1;
         ++index)
    {
        require(
            activation::tryAppendComponentActivationResourceSetBudgetMemberV1(
                countBoundary,
                activation::ComponentActivationResourceSetBudgetMemberV1{
                    .sourceByteCount = 1,
                    .transportByteCount = 1,
                    .privateMappedByteCount = 1,
                })
                == activation::ComponentActivationResourceSetBudgetDiagnosticV1::
                    Valid,
            "each member through the fixed count boundary should validate");
    }
    const auto full = countBoundary;
    require(
        activation::tryAppendComponentActivationResourceSetBudgetMemberV1(
            countBoundary, table)
                == activation::ComponentActivationResourceSetBudgetDiagnosticV1::
                    MemberCountInvalid
            && countBoundary == full,
        "a ninth member must reject failure-atomically");

    auto overflow = activation::ComponentActivationResourceSetBudgetV1{};
    require(
        activation::tryAppendComponentActivationResourceSetBudgetMemberV1(
            overflow,
            activation::ComponentActivationResourceSetBudgetMemberV1{
                .sourceByteCount = std::numeric_limits<std::uint64_t>::max(),
                .transportByteCount = std::numeric_limits<std::uint64_t>::max(),
                .privateMappedByteCount =
                    std::numeric_limits<std::uint64_t>::max(),
            })
            == activation::ComponentActivationResourceSetBudgetDiagnosticV1::
                AggregateSourceBytesInvalid,
        "a single over-budget member must reject before aggregation");
    overflow.aggregateSourceBytes = 1;
    overflow.aggregateTransportBytes = 1;
    overflow.aggregatePrivateMappedBytes = 1;
    const auto malformed = overflow;
    require(
        activation::tryAppendComponentActivationResourceSetBudgetMemberV1(
            overflow,
            activation::ComponentActivationResourceSetBudgetMemberV1{
                .sourceByteCount = std::numeric_limits<std::uint64_t>::max(),
                .transportByteCount = std::numeric_limits<std::uint64_t>::max(),
                .privateMappedByteCount =
                    std::numeric_limits<std::uint64_t>::max(),
            })
                == activation::ComponentActivationResourceSetBudgetDiagnosticV1::
                    ArithmeticOverflow
            && overflow == malformed,
        "aggregate addition must reject arithmetic overflow without mutation");

    auto contradictory = accepted;
    contradictory.aggregateTransportBytes =
        contradictory.aggregateSourceBytes - 1;
    require(
        activation::validateComponentActivationResourceSetBudgetV1(
            contradictory)
            == activation::ComponentActivationResourceSetBudgetDiagnosticV1::
                AggregateTransportBytesInvalid,
        "transport totals smaller than source bytes must reject");

    activation::ComponentActivationResourceSetBudgetV1 bloatedTransport{
        .aggregateSourceBytes = 1,
        .aggregateTransportBytes =
            activation::maximumComponentActivationResourceSetTransportPageBytesV1
            + 1,
        .aggregatePrivateMappedBytes = 1,
        .memberCount = 1,
    };
    require(
        activation::validateComponentActivationResourceSetBudgetV1(
            bloatedTransport)
            == activation::ComponentActivationResourceSetBudgetDiagnosticV1::
                AggregateTransportBytesInvalid,
        "one member cannot claim the page-rounding overhead budget of eight members");

    activation::ComponentActivationResourceSetBudgetV1 bloatedWorking{
        .aggregateSourceBytes = 1,
        .aggregateTransportBytes = 1,
        .aggregatePrivateMappedBytes = 1,
        .aggregateWorkingBytes =
            activation::maximumComponentActivationAudioStreamWorkingBytesV1
            + 1,
        .memberCount = 1,
        .audioStreamCount = 1,
    };
    require(
        activation::validateComponentActivationResourceSetBudgetV1(
            bloatedWorking)
            == activation::ComponentActivationResourceSetBudgetDiagnosticV1::
                AggregateWorkingBytesInvalid,
        "one Audio stream cannot claim the two-stream working-memory budget");
}

void testResourceSetRecordsBindOrderIdentityAndAggregateBudget()
{
    const auto session = resourceSession();
    auto authority = resourceAuthorization(session);
    authority.staticParameterCount = 2;
    auto tableGrant = resourceGrant(authority);
    auto binaryGrant = binaryResourceGrant(authority);
    constexpr std::string_view binaryParameter = "binary";
    constexpr std::string_view binaryResource = "Binary/Blob-A";
    binaryGrant.parameterId = {};
    binaryGrant.resourceId = {};
    binaryGrant.parameterIdBytes = binaryParameter.size();
    binaryGrant.resourceIdBytes = binaryResource.size();
    std::copy(
        binaryParameter.begin(),
        binaryParameter.end(),
        binaryGrant.parameterId.begin());
    std::copy(
        binaryResource.begin(),
        binaryResource.end(),
        binaryGrant.resourceId.begin());

    activation::ComponentActivationResourceSetGrantV1 set;
    set.header = activation::componentActivationHeaderV1(
        activation::ComponentActivationRecordKindV1::ResourceSetGrant,
        sizeof(set));
    set.contentDigest = authority.contentDigest;
    set.validationNonce = authority.validationNonce;
    set.runtimeSessionNonce = authority.runtimeSessionNonce;
    set.authorizationNonce = authority.authorizationNonce;
    set.artifactGeneration = authority.artifactGeneration;
    set.activationGeneration = authority.activationGeneration;
    set.runtimeSessionGeneration = authority.runtimeSessionGeneration;
    set.sharedStorageIdentity = authority.sharedStorageIdentity;
    set.profileVersion = activation::componentActivationResourceSetProfileVersionV1;
    set.memberCount = 2;
    set.audioStreamCount = 0;
    set.aggregateSourceBytes = tableGrant.byteCount + binaryGrant.byteCount;
    set.aggregateTransportBytes = set.aggregateSourceBytes;
    set.aggregatePrivateMappedBytes = set.aggregateSourceBytes;
    set.aggregateWorkingBytes = 0;

    const auto member = [&](const auto& grant,
                            std::uint16_t ordinal,
                            auto kind) {
        activation::ComponentActivationResourceSetMemberV1 value;
        value.header = activation::componentActivationHeaderV1(
            activation::ComponentActivationRecordKindV1::ResourceSetMember,
            sizeof(value));
        const auto raw = std::as_bytes(std::span{&grant, 1U});
        value.memberGrantDigest =
            activation::calculateComponentActivationByteDigestV1(
                {reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size()});
        value.sourceContentDigest = grant.sourceContentDigest;
        value.mediatedPayloadDigest = grant.mediatedPayloadDigest;
        value.resourceGeneration = grant.resourceGeneration;
        value.sourceByteCount = grant.byteCount;
        value.transportByteCount = grant.byteCount;
        value.privateMappedByteCount = grant.byteCount;
        value.parameterIdBytes = grant.parameterIdBytes;
        value.resourceIdBytes = grant.resourceIdBytes;
        value.memberKind = static_cast<std::uint32_t>(kind);
        value.memberOrdinal = ordinal;
        value.memberCount = set.memberCount;
        std::copy_n(
            grant.parameterId.begin(),
            grant.parameterIdBytes,
            value.parameterId.begin());
        std::copy_n(
            grant.resourceId.begin(),
            grant.resourceIdBytes,
            value.resourceId.begin());
        return value;
    };
    std::array members{
        member(
            tableGrant,
            0,
            activation::ComponentActivationResourceSetMemberKindV1::Table),
        member(
            binaryGrant,
            1,
            activation::ComponentActivationResourceSetMemberKindV1::Binary),
    };
    set.resourceSetDigest =
        activation::calculateComponentActivationResourceSetDigestV1(
            set, members);
    for (auto& value : members)
        value.resourceSetDigest = set.resourceSetDigest;

    require(
        activation::validateComponentActivationResourceSetGrantV1(
            set, authority, session)
                == activation::ComponentActivationProtocolDiagnosticV1::Valid
            && activation::validateComponentActivationResourceSetMembersV1(
                   set, members)
                == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "one ordered Table/Binary set should bind the exact authority and budget");

    const auto rejectsSet = [&](auto mutate, const std::string& message) {
        auto changed = set;
        mutate(changed);
        const auto summary =
            activation::validateComponentActivationResourceSetGrantV1(
                changed, authority, session);
        const auto contents =
            activation::validateComponentActivationResourceSetMembersV1(
                changed, members);
        require(
            summary
                    != activation::ComponentActivationProtocolDiagnosticV1::
                        Valid
                || contents
                    != activation::ComponentActivationProtocolDiagnosticV1::
                        Valid,
            message);
    };
    rejectsSet(
        [](auto& value) { ++value.memberCount; },
        "set member-count drift must reject");
    rejectsSet(
        [](auto& value) { ++value.profileVersion; },
        "an unknown set profile must reject");
    rejectsSet(
        [](auto& value) { value.aggregateSourceBytes = 0; },
        "set aggregate-budget drift must reject");
    rejectsSet(
        [](auto& value) { value.resourceSetDigest.front() ^= 0xffU; },
        "set identity drift must reject");
    rejectsSet(
        [](auto& value) { value.reserved0.front() = 1; },
        "set reserved data must reject");
    rejectsSet(
        [](auto& value) { ++value.activationGeneration; },
        "a detached activation generation must reject");
    rejectsSet(
        [](auto& value) { value.validationNonce.front() ^= 0xffU; },
        "a detached validation session must reject");
    rejectsSet(
        [](auto& value) { ++value.header.protocolVersion; },
        "an unknown resource-set record version must reject");

    const auto rejectsMembers = [&](auto mutate, const std::string& message) {
        auto changed = members;
        mutate(changed);
        require(
            activation::validateComponentActivationResourceSetMembersV1(
                set, changed)
                != activation::ComponentActivationProtocolDiagnosticV1::Valid,
            message);
    };
    rejectsMembers(
        [](auto& value) { value[1].memberOrdinal = 0; },
        "duplicate member ordinals must reject");
    rejectsMembers(
        [](auto& value) { --value[1].memberCount; },
        "a member count gap must reject");
    rejectsMembers(
        [](auto& value) {
            value[1].parameterId = value[0].parameterId;
            value[1].parameterIdBytes = value[0].parameterIdBytes;
        },
        "duplicate parameter IDs must reject");
    rejectsMembers(
        [](auto& value) {
            value[1].resourceId = value[0].resourceId;
            value[1].resourceIdBytes = value[0].resourceIdBytes;
        },
        "duplicate resource IDs must reject");
    rejectsMembers(
        [](auto& value) { value[1].resourceSetDigest.front() ^= 0xffU; },
        "cross-set member replay must reject");
    rejectsMembers(
        [](auto& value) { value[1].memberGrantDigest.fill(0); },
        "an unbound member grant must reject");
    rejectsMembers(
        [](auto& value) { value[1].memberKind = 0xffU; },
        "an unknown member kind must reject");
    rejectsMembers(
        [](auto& value) {
            value[1].mediatedPayloadDigest.front() ^= 0xffU;
        },
        "a Binary source/mediated identity mismatch must reject");
    rejectsMembers(
        [](auto& value) { ++value[1].header.protocolVersion; },
        "an unknown member-record version must reject");
    rejectsMembers(
        [](auto& value) { std::swap(value[0], value[1]); },
        "member reordering must reject");
}

activation::ComponentActivationSealedSessionV1 sealedSession()
{
    activation::ComponentActivationSealedSessionV1 value;
    value.header = activation::componentActivationHeaderV1(
        activation::ComponentActivationRecordKindV1::SealedSession,
        sizeof(value));
    value.profileVersion = activation::componentActivationProfileVersionV1;
    value.supportedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    value.safeguards = allComponentIsolationSafeguardsV1;
    value.runtimeSessionGeneration = 17;
    value.sharedStorageIdentity = 23;
    value.sharedStorageBytes = 64U * 1024U;
    value.runtimeSessionNonce.fill(0x33);
    return value;
}

activation::ComponentActivationAuthorizationV1 authorization(
    const activation::ComponentActivationSealedSessionV1& session)
{
    activation::ComponentActivationAuthorizationV1 value;
    value.header = activation::componentActivationHeaderV1(
        activation::ComponentActivationRecordKindV1::Authorization,
        sizeof(value));
    constexpr std::string_view bundleId = "org.talas.activation";
    constexpr std::string_view version = "1.2.3";
    constexpr std::string_view descriptor =
        "org.talas.activation.gain";
    value.bundleIdBytes = bundleId.size();
    value.semanticVersionBytes = version.size();
    value.descriptorIdBytes = descriptor.size();
    std::copy(bundleId.begin(), bundleId.end(), value.bundleId.begin());
    std::copy(version.begin(), version.end(), value.semanticVersion.begin());
    std::copy(
        descriptor.begin(), descriptor.end(), value.descriptorId.begin());
    value.contentDigest.fill(0x11);
    value.signerIdentityDigest.fill(0x22);
    value.entryDigest.fill(0x44);
    value.requestedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    value.declaredRoles = componentRole(
        ComponentExecutionRole::RealtimeProcessor);
    value.policyVersion = currentComponentTrustPolicyVersion;
    value.sampleRate = 48'000;
    value.blockFrames = 256;
    value.inputChannelCount = 2;
    value.outputChannelCount = 2;
    activation::ComponentActivationStaticParameterV1 parameter;
    constexpr std::string_view parameterId = "gain";
    parameter.kind = static_cast<std::uint32_t>(
        activation::ComponentActivationStaticParameterKindV1::Scalar);
    parameter.idBytes = parameterId.size();
    std::copy(parameterId.begin(), parameterId.end(), parameter.id.begin());
    parameter.scalarValue = 0.5;
    const std::array parameters{parameter};
    value.staticParameterCount = parameters.size();
    value.staticParameterDigest =
        activation::calculateComponentActivationStaticParameterDigestV1(
            parameters);
    value.entryByteCount = 4096;
    value.artifactGeneration = 29;
    value.activationGeneration = 31;
    value.runtimeSessionGeneration = session.runtimeSessionGeneration;
    value.sharedStorageIdentity = session.sharedStorageIdentity;
    value.validationNonce.fill(0x5c);
    value.runtimeSessionNonce = session.runtimeSessionNonce;
    value.authorizationNonce.fill(0xa5);
    return value;
}

activation::ComponentActivationStaticParameterV1 resourceParameter()
{
    activation::ComponentActivationStaticParameterV1 value;
    value.kind = static_cast<std::uint32_t>(
        activation::ComponentActivationStaticParameterKindV1::Resource);
    constexpr std::string_view parameterId = "table";
    constexpr std::string_view resourceId = "Tables/Curve-A";
    value.idBytes = parameterId.size();
    value.textBytes = resourceId.size();
    std::copy(parameterId.begin(), parameterId.end(), value.id.begin());
    std::copy(resourceId.begin(), resourceId.end(), value.text.begin());
    return value;
}

activation::ComponentActivationSealedSessionV1 resourceSession()
{
    auto value = sealedSession();
    value.supportedPermissions |= componentPermission(
        ComponentExecutionPermission::ImmutableResourceRead);
    return value;
}

activation::ComponentActivationAuthorizationV1 resourceAuthorization(
    const activation::ComponentActivationSealedSessionV1& session)
{
    auto value = authorization(session);
    value.requestedPermissions |= componentPermission(
        ComponentExecutionPermission::ImmutableResourceRead);
    value.declaredRoles |= componentRole(
        ComponentExecutionRole::ImmutableResourceConsumer);
    const std::array parameters{resourceParameter()};
    value.staticParameterCount = parameters.size();
    value.staticParameterDigest =
        activation::calculateComponentActivationStaticParameterDigestV1(
            parameters);
    return value;
}

activation::ComponentActivationImmutableResourceGrantV1 resourceGrant(
    const activation::ComponentActivationAuthorizationV1& authority)
{
    activation::ComponentActivationImmutableResourceGrantV1 value;
    value.header = activation::componentActivationHeaderV1(
        activation::ComponentActivationRecordKindV1::ImmutableResourceGrant,
        sizeof(value));
    value.contentDigest = authority.contentDigest;
    value.sourceContentDigest.fill(0x61);
    value.mediatedPayloadDigest.fill(0x62);
    value.validationNonce = authority.validationNonce;
    value.runtimeSessionNonce = authority.runtimeSessionNonce;
    value.authorizationNonce = authority.authorizationNonce;
    value.artifactGeneration = authority.artifactGeneration;
    value.activationGeneration = authority.activationGeneration;
    value.runtimeSessionGeneration = authority.runtimeSessionGeneration;
    value.sharedStorageIdentity = authority.sharedStorageIdentity;
    value.resourceGeneration = 7;
    value.elementCount = 3;
    value.byteCount = 3 * sizeof(double);
    constexpr std::string_view parameterId = "table";
    constexpr std::string_view resourceId = "Tables/Curve-A";
    value.parameterIdBytes = parameterId.size();
    value.resourceIdBytes = resourceId.size();
    value.resourceKind = static_cast<std::uint32_t>(
        activation::ComponentActivationImmutableResourceKindV1::Table);
    value.elementFormat = static_cast<std::uint32_t>(
        activation::ComponentActivationImmutableResourceElementFormatV1::
            Float64LittleEndian);
    std::copy(
        parameterId.begin(), parameterId.end(), value.parameterId.begin());
    std::copy(resourceId.begin(), resourceId.end(), value.resourceId.begin());
    return value;
}

activation::ComponentActivationImmutableResourceGrantV1 binaryResourceGrant(
    const activation::ComponentActivationAuthorizationV1& authority)
{
    auto value = resourceGrant(authority);
    value.mediatedPayloadDigest = value.sourceContentDigest;
    value.elementCount = 3;
    value.byteCount = 3;
    value.resourceKind = static_cast<std::uint32_t>(
        activation::ComponentActivationImmutableResourceKindV1::Binary);
    value.elementFormat = static_cast<std::uint32_t>(
        activation::ComponentActivationImmutableResourceElementFormatV1::
            RawBytes);
    return value;
}

activation::ComponentActivationStaticParameterV1 audioResourceParameter()
{
    activation::ComponentActivationStaticParameterV1 value;
    value.kind = static_cast<std::uint32_t>(
        activation::ComponentActivationStaticParameterKindV1::Resource);
    constexpr std::string_view parameterId = "audio";
    constexpr std::string_view resourceId = "Audio/Voice-A";
    value.idBytes = parameterId.size();
    value.textBytes = resourceId.size();
    std::copy(parameterId.begin(), parameterId.end(), value.id.begin());
    std::copy(resourceId.begin(), resourceId.end(), value.text.begin());
    return value;
}

activation::ComponentActivationSealedSessionV1 audioStreamSession()
{
    auto value = sealedSession();
    value.supportedPermissions |= componentPermission(
        ComponentExecutionPermission::BoundedAudioStreamRead);
    return value;
}

activation::ComponentActivationAuthorizationV1 audioStreamAuthorization(
    const activation::ComponentActivationSealedSessionV1& session)
{
    auto value = authorization(session);
    value.requestedPermissions |= componentPermission(
        ComponentExecutionPermission::BoundedAudioStreamRead);
    value.declaredRoles |= componentRole(
        ComponentExecutionRole::BoundedAudioStreamConsumer);
    const std::array parameters{audioResourceParameter()};
    value.staticParameterCount = parameters.size();
    value.staticParameterDigest =
        activation::calculateComponentActivationStaticParameterDigestV1(
            parameters);
    return value;
}

activation::ComponentActivationAudioStreamGrantV1 audioStreamGrant(
    const activation::ComponentActivationAuthorizationV1& authority)
{
    activation::ComponentActivationAudioStreamGrantV1 value;
    value.header = activation::componentActivationHeaderV1(
        activation::ComponentActivationRecordKindV1::AudioStreamGrant,
        sizeof(value));
    value.contentDigest = authority.contentDigest;
    value.signerIdentityDigest = authority.signerIdentityDigest;
    value.sourceContentDigest.fill(0x73);
    value.validationNonce = authority.validationNonce;
    value.runtimeSessionNonce = authority.runtimeSessionNonce;
    value.authorizationNonce = authority.authorizationNonce;
    value.artifactGeneration = authority.artifactGeneration;
    value.activationGeneration = authority.activationGeneration;
    value.runtimeSessionGeneration = authority.runtimeSessionGeneration;
    value.sharedStorageIdentity = authority.sharedStorageIdentity;
    value.resourceGeneration = 7;
    value.sourceByteCount = 44 + 16'000 * 2 * 2;
    value.sourceFrameCount = 16'000;
    value.maximumSourceFrameIncrement = 8.0;
    constexpr std::string_view parameterId = "audio";
    constexpr std::string_view resourceId = "Audio/Voice-A";
    value.parameterIdBytes = parameterId.size();
    value.resourceIdBytes = resourceId.size();
    value.sourceSampleRate = 48'000;
    value.streamProfileVersion =
        activation::componentActivationAudioStreamProfileVersionV1;
    value.sourceEncoding = static_cast<std::uint32_t>(
        activation::ComponentActivationAudioSourceEncodingV1::Pcm16Wave);
    value.maximumReadFrames =
        activation::maximumComponentActivationAudioReadFramesV1;
    value.decodeBlockFrames =
        activation::componentActivationAudioDecodeBlockFramesV1;
    value.sourceChannelCount = 2;
    value.maximumTouchedSourceBlocks =
        activation::maximumComponentActivationAudioTouchedSourceBlocksV1;
    value.requestCapacity =
        activation::componentActivationAudioRequestCapacityV1;
    value.responseCapacity =
        activation::componentActivationAudioResponseCapacityV1;
    value.workerCacheCapacity =
        activation::componentActivationAudioWorkerCacheCapacityV1;
    value.lowWatermarkBlocks =
        activation::componentActivationAudioLowWatermarkBlocksV1;
    value.highWatermarkBlocks =
        activation::componentActivationAudioHighWatermarkBlocksV1;
    std::copy(
        parameterId.begin(), parameterId.end(), value.parameterId.begin());
    std::copy(resourceId.begin(), resourceId.end(), value.resourceId.begin());
    return value;
}

void testStaticParametersAreCanonicalAndDigestBound()
{
    activation::ComponentActivationStaticParameterV1 scalar;
    scalar.kind = static_cast<std::uint32_t>(
        activation::ComponentActivationStaticParameterKindV1::Scalar);
    scalar.idBytes = 4;
    std::copy_n("gain", 4, scalar.id.begin());
    scalar.scalarValue = 0.5;

    activation::ComponentActivationStaticParameterV1 integer;
    integer.kind = static_cast<std::uint32_t>(
        activation::ComponentActivationStaticParameterKindV1::Integer);
    integer.idBytes = 4;
    std::copy_n("mode", 4, integer.id.begin());
    integer.integerValue = 2;

    activation::ComponentActivationStaticParameterV1 text;
    text.kind = static_cast<std::uint32_t>(
        activation::ComponentActivationStaticParameterKindV1::Text);
    text.idBytes = 5;
    std::copy_n("label", 5, text.id.begin());
    text.textBytes = 3;
    std::copy_n("dry", 3, text.text.begin());

    const auto resource = resourceParameter();
    const std::array parameters{scalar, integer, text, resource};
    require(
        activation::validateComponentActivationStaticParametersV1(parameters)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "canonical scalar, integer, text, and resource parameters should validate");
    const auto digest =
        activation::calculateComponentActivationStaticParameterDigestV1(
            parameters);
    require(
        std::ranges::any_of(digest, [](auto byte) { return byte != 0; }),
        "a non-empty parameter contract should have a nonzero digest");

    auto changed = parameters;
    changed.front().scalarValue = 0.75;
    require(
        activation::calculateComponentActivationStaticParameterDigestV1(
            changed)
            != digest,
        "a changed static value must change the authorization digest");

    changed = parameters;
    changed.front().scalarValue =
        std::numeric_limits<double>::quiet_NaN();
    require(
        activation::validateComponentActivationStaticParametersV1(changed)
            == activation::ComponentActivationProtocolDiagnosticV1::ParameterInvalid,
        "non-finite scalar parameters must reject");
    changed = parameters;
    changed[1].textBytes = 1;
    changed[1].text.front() = 'x';
    require(
        activation::validateComponentActivationStaticParametersV1(changed)
            == activation::ComponentActivationProtocolDiagnosticV1::ParameterInvalid,
        "integer parameters must not smuggle text");
    changed = parameters;
    changed[2].text[changed[2].textBytes] = 'x';
    require(
        activation::validateComponentActivationStaticParametersV1(changed)
            == activation::ComponentActivationProtocolDiagnosticV1::ParameterInvalid,
        "parameter padding must be canonical zero");
    changed = parameters;
    changed[3].textBytes = 0;
    require(
        activation::validateComponentActivationStaticParametersV1(changed)
            == activation::ComponentActivationProtocolDiagnosticV1::ParameterInvalid,
        "resource parameters must bind one non-empty resource ID");
}

void testImmutableTableEncodingIsCanonicalAndBounded()
{
    const std::array values{1.0, -0.0, 0.5};
    std::array<std::uint8_t, values.size() * sizeof(double)> bytes{};
    require(
        activation::encodeComponentActivationImmutableTableV1(values, bytes)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "finite bounded tables should encode");
    const std::array<std::uint8_t, 8> one{
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x3f};
    require(
        std::equal(one.begin(), one.end(), bytes.begin()),
        "table bytes must use canonical little-endian IEEE-754 encoding");

    auto nonFinite = values;
    nonFinite[1] = std::numeric_limits<double>::infinity();
    auto unchanged = bytes;
    unchanged.fill(0xa5);
    require(
        activation::encodeComponentActivationImmutableTableV1(
            nonFinite, unchanged)
            == activation::ComponentActivationProtocolDiagnosticV1::ResourceInvalid,
        "non-finite table values must reject");
    require(
        std::ranges::all_of(
            unchanged, [](auto byte) { return byte == 0xa5; }),
        "failed table encoding must not publish partial canonical bytes");
    std::array<std::uint8_t, bytes.size() - 1> shortBytes{};
    require(
        activation::encodeComponentActivationImmutableTableV1(
            values, shortBytes)
            == activation::ComponentActivationProtocolDiagnosticV1::ResourceInvalid,
        "the destination size must bind the complete table");
}

void testImmutableBinaryCopyIsExactBoundedAndFailureAtomic()
{
    const std::array<std::uint8_t, 4> source{0x00, 0x7f, 0x80, 0xff};
    std::array<std::uint8_t, source.size()> destination{};
    require(
        activation::copyComponentActivationImmutableBinaryV1(
            source, destination)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "a bounded immutable binary should copy exactly");
    require(
        destination == source,
        "binary mediation must preserve zero and high-bit octets exactly");

    std::array<std::uint8_t, 4> unchanged{};
    unchanged.fill(0xa5);
    require(
        activation::copyComponentActivationImmutableBinaryV1(
            source, std::span<std::uint8_t>{unchanged}.first(3))
            == activation::ComponentActivationProtocolDiagnosticV1::ResourceInvalid,
        "a partial Binary destination must reject");
    require(
        std::ranges::all_of(
            unchanged, [](auto byte) { return byte == 0xa5; }),
        "a rejected Binary copy must leave the destination unchanged");

    std::array<std::uint8_t, 5> overlapping{1, 2, 3, 4, 5};
    const auto overlappingBefore = overlapping;
    require(
        activation::copyComponentActivationImmutableBinaryV1(
            std::span<const std::uint8_t>{overlapping}.first(4),
            std::span<std::uint8_t>{overlapping}.last(3))
            == activation::ComponentActivationProtocolDiagnosticV1::ResourceInvalid,
        "an overlapping wrong-sized Binary destination must reject");
    require(
        overlapping == overlappingBefore,
        "overlap must not weaken failure-atomic Binary rejection");

    std::span<const std::uint8_t> emptySource;
    std::span<std::uint8_t> emptyDestination;
    require(
        activation::copyComponentActivationImmutableBinaryV1(
            emptySource, emptyDestination)
            == activation::ComponentActivationProtocolDiagnosticV1::ResourceInvalid,
        "an empty Binary payload must reject");

    std::vector<std::uint8_t> maximum(
        activation::maximumComponentActivationImmutableBinaryBytesV1,
        0x5a);
    std::vector<std::uint8_t> maximumCopy(maximum.size());
    require(
        activation::copyComponentActivationImmutableBinaryV1(
            maximum, maximumCopy)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "the exact one-MiB Binary limit should copy");
    require(maximumCopy == maximum, "the maximum Binary copy must be exact");

    std::vector<std::uint8_t> oversized(maximum.size() + 1U, 0x33);
    std::vector<std::uint8_t> oversizedDestination(
        oversized.size(), 0xa5);
    require(
        activation::copyComponentActivationImmutableBinaryV1(
            oversized, oversizedDestination)
            == activation::ComponentActivationProtocolDiagnosticV1::ResourceInvalid,
        "a Binary payload above one MiB must reject");
    require(
        std::ranges::all_of(
            oversizedDestination, [](auto byte) { return byte == 0xa5; }),
        "an oversized Binary rejection must leave output unchanged");
}

void testImmutableResourceGrantRejectsEveryBindingMutation()
{
    const auto session = resourceSession();
    const auto authority = resourceAuthorization(session);
    const std::array parameters{resourceParameter()};
    const auto valid = resourceGrant(authority);
    static_assert(std::is_standard_layout_v<
        activation::ComponentActivationImmutableResourceGrantV1>);
    static_assert(std::is_trivially_copyable_v<
        activation::ComponentActivationImmutableResourceGrantV1>);
    require(
        activation::validateComponentActivationImmutableResourceGrantV1(
            valid, authority, session, parameters)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "the exact single-table resource grant should validate");

    const auto rejects = [&](auto mutate, std::string_view message) {
        auto changed = valid;
        mutate(changed);
        require(
            activation::validateComponentActivationImmutableResourceGrantV1(
                changed, authority, session, parameters)
                != activation::ComponentActivationProtocolDiagnosticV1::Valid,
            std::string(message));
    };
    rejects([](auto& value) { ++value.header.protocolVersion; },
        "resource protocol drift must reject");
    rejects([](auto& value) { ++value.header.magic; },
        "resource magic drift must reject");
    rejects([](auto& value) { ++value.header.recordKind; },
        "resource record-kind drift must reject");
    rejects([](auto& value) { --value.header.recordBytes; },
        "resource record-size drift must reject");
    rejects([](auto& value) { ++value.header.reserved0; },
        "resource header reservations must reject");
    rejects([](auto& value) { value.contentDigest.front() ^= 0xffU; },
        "another candidate must reject");
    rejects([](auto& value) { value.sourceContentDigest.fill(0); },
        "a missing source digest must reject");
    rejects([](auto& value) { value.mediatedPayloadDigest.fill(0); },
        "a missing mediated digest must reject");
    rejects([](auto& value) { value.validationNonce.front() ^= 0xffU; },
        "another validation session must reject");
    rejects([](auto& value) { value.runtimeSessionNonce.front() ^= 0xffU; },
        "another runtime session must reject");
    rejects([](auto& value) { value.authorizationNonce.front() ^= 0xffU; },
        "another authorization must reject");
    rejects([](auto& value) { ++value.artifactGeneration; },
        "another artifact generation must reject");
    rejects([](auto& value) { ++value.activationGeneration; },
        "another activation generation must reject");
    rejects([](auto& value) { ++value.runtimeSessionGeneration; },
        "another runtime generation must reject");
    rejects([](auto& value) { ++value.sharedStorageIdentity; },
        "another shared storage identity must reject");
    rejects([](auto& value) { value.resourceGeneration = 0; },
        "legacy resource generation zero must reject");
    rejects([](auto& value) { value.elementCount = 0; },
        "an empty table must reject");
    rejects(
        [](auto& value) {
            value.elementCount =
                activation::maximumComponentActivationImmutableTableElementsV1
                + 1;
        },
        "an over-budget table must reject");
    rejects([](auto& value) { --value.byteCount; },
        "a partial table byte count must reject");
    rejects([](auto& value) { value.parameterId[0] = 'x'; },
        "another parameter identity must reject");
    rejects([](auto& value) { --value.parameterIdBytes; },
        "a partial parameter identity must reject");
    rejects([](auto& value) { value.resourceId[0] = 'x'; },
        "another resource identity must reject");
    rejects([](auto& value) { --value.resourceIdBytes; },
        "a partial resource identity must reject");
    rejects([](auto& value) { value.parameterId[value.parameterIdBytes] = 'x'; },
        "parameter padding must be zero");
    rejects([](auto& value) { value.resourceId[value.resourceIdBytes] = 'x'; },
        "resource padding must be zero");
    rejects([](auto& value) { value.resourceKind = 99; },
        "unknown resource kinds must reject");
    rejects([](auto& value) { value.elementFormat = 99; },
        "unknown element formats must reject");
    rejects([](auto& value) { value.reserved0.front() = 1; },
        "reserved resource fields must reject");

    auto changedParameters = parameters;
    changedParameters.front().text[0] = 'x';
    require(
        activation::validateComponentActivationImmutableResourceGrantV1(
            valid, authority, session, changedParameters)
            != activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "a grant cannot be detached from its static resource parameter");
}

void testImmutableBinaryGrantRulesAreClosedAndLayoutPreserving()
{
    const auto session = resourceSession();
    const auto authority = resourceAuthorization(session);
    const std::array parameters{resourceParameter()};
    const auto valid = binaryResourceGrant(authority);
    static_assert(
        static_cast<std::uint32_t>(
            activation::ComponentActivationImmutableResourceKindV1::Binary)
        == 2);
    static_assert(
        static_cast<std::uint32_t>(
            activation::ComponentActivationImmutableResourceElementFormatV1::
                RawBytes)
        == 2);
    static_assert(
        sizeof(activation::ComponentActivationImmutableResourceGrantV1)
        == 728);
    require(
        activation::validateComponentActivationImmutableResourceGrantV1(
            valid, authority, session, parameters)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "the exact one-byte-per-element Binary grant should validate");

    const auto rejects = [&](auto mutate, std::string_view message) {
        auto changed = valid;
        mutate(changed);
        require(
            activation::validateComponentActivationImmutableResourceGrantV1(
                changed, authority, session, parameters)
                == activation::ComponentActivationProtocolDiagnosticV1::ResourceInvalid,
            std::string(message));
    };
    rejects([](auto& value) { value.elementCount = 0; },
        "an empty Binary grant must reject");
    rejects(
        [](auto& value) {
            value.elementCount =
                activation::maximumComponentActivationImmutableBinaryBytesV1
                + 1U;
            value.byteCount = value.elementCount;
        },
        "an over-budget Binary grant must reject");
    rejects([](auto& value) { ++value.byteCount; },
        "Binary element and byte counts must be equal");
    rejects(
        [](auto& value) {
            value.elementFormat = static_cast<std::uint32_t>(
                activation::
                    ComponentActivationImmutableResourceElementFormatV1::
                        Float64LittleEndian);
        },
        "Binary grants cannot claim Table element encoding");
    rejects([](auto& value) { value.sourceContentDigest.front() ^= 0xffU; },
        "raw Binary source and mediated digests must be identical");

    auto maximum = valid;
    maximum.elementCount =
        activation::maximumComponentActivationImmutableBinaryBytesV1;
    maximum.byteCount = maximum.elementCount;
    require(
        activation::validateComponentActivationImmutableResourceGrantV1(
            maximum, authority, session, parameters)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "the exact one-MiB Binary grant should validate");

    auto tableCrossPair = resourceGrant(authority);
    tableCrossPair.elementFormat = static_cast<std::uint32_t>(
        activation::ComponentActivationImmutableResourceElementFormatV1::
            RawBytes);
    require(
        activation::validateComponentActivationImmutableResourceGrantV1(
            tableCrossPair, authority, session, parameters)
            == activation::ComponentActivationProtocolDiagnosticV1::ResourceInvalid,
        "a Table grant cannot claim raw-byte semantics");

    auto maximumTable = resourceGrant(authority);
    maximumTable.elementCount =
        activation::maximumComponentActivationImmutableTableElementsV1;
    maximumTable.byteCount = maximumTable.elementCount * sizeof(double);
    require(
        activation::validateComponentActivationImmutableResourceGrantV1(
            maximumTable, authority, session, parameters)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "the exact maximum canonical Table grant should remain valid");

    auto overflowTable = resourceGrant(authority);
    overflowTable.elementCount = std::numeric_limits<std::uint64_t>::max();
    overflowTable.byteCount = 0;
    require(
        activation::validateComponentActivationImmutableResourceGrantV1(
            overflowTable, authority, session, parameters)
            == activation::ComponentActivationProtocolDiagnosticV1::ResourceInvalid,
        "Table count multiplication must fail closed before overflow");
}

void testAudioStreamGrantRejectsEveryBindingAndBudgetMutation()
{
    const auto session = audioStreamSession();
    const auto authority = audioStreamAuthorization(session);
    const std::array parameters{audioResourceParameter()};
    const auto valid = audioStreamGrant(authority);
    static_assert(std::is_standard_layout_v<
        activation::ComponentActivationAudioStreamGrantV1>);
    static_assert(std::is_trivially_copyable_v<
        activation::ComponentActivationAudioStreamGrantV1>);
    static_assert(
        sizeof(activation::ComponentActivationAudioStreamGrantV1) == 768);
    require(
        activation::validateComponentActivationAuthorizationV1(
            authority, session)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "the dedicated bounded Audio role/permission profile should validate");
    require(
        activation::validateComponentActivationAudioStreamGrantV1(
            valid, authority, session, parameters)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "the exact bounded PCM16-WAV Audio stream grant should validate");

    const auto rejects = [&](auto mutate, std::string_view message) {
        auto changed = valid;
        mutate(changed);
        require(
            activation::validateComponentActivationAudioStreamGrantV1(
                changed, authority, session, parameters)
                != activation::ComponentActivationProtocolDiagnosticV1::Valid,
            std::string(message));
    };
    rejects([](auto& value) { ++value.header.magic; },
        "Audio grant magic drift must reject");
    rejects([](auto& value) { ++value.header.protocolVersion; },
        "Audio grant protocol drift must reject");
    rejects([](auto& value) { ++value.header.recordKind; },
        "Audio grant kind drift must reject");
    rejects([](auto& value) { --value.header.recordBytes; },
        "Audio grant size drift must reject");
    rejects([](auto& value) { ++value.header.reserved0; },
        "Audio grant header reservations must reject");
    rejects([](auto& value) { value.contentDigest.front() ^= 0xffU; },
        "another Audio candidate must reject");
    rejects([](auto& value) { value.signerIdentityDigest.front() ^= 0xffU; },
        "another Audio signer must reject");
    rejects([](auto& value) { value.sourceContentDigest.fill(0); },
        "a missing raw Audio digest must reject");
    rejects([](auto& value) { value.validationNonce.front() ^= 0xffU; },
        "another Audio validation session must reject");
    rejects([](auto& value) { value.runtimeSessionNonce.front() ^= 0xffU; },
        "another Audio runtime session must reject");
    rejects([](auto& value) { value.authorizationNonce.front() ^= 0xffU; },
        "another Audio authorization must reject");
    rejects([](auto& value) { ++value.artifactGeneration; },
        "another Audio artifact generation must reject");
    rejects([](auto& value) { ++value.activationGeneration; },
        "another Audio activation generation must reject");
    rejects([](auto& value) { ++value.runtimeSessionGeneration; },
        "another Audio runtime generation must reject");
    rejects([](auto& value) { ++value.sharedStorageIdentity; },
        "another Audio shared-storage identity must reject");
    rejects([](auto& value) { value.resourceGeneration = 0; },
        "Audio resource generation zero must reject");
    rejects(
        [](auto& value) {
            value.sourceByteCount =
                activation::minimumComponentActivationAudioSourceBytesV1 - 1;
        },
        "a short WAV source must reject before parsing");
    rejects(
        [](auto& value) {
            value.sourceByteCount =
                activation::maximumComponentActivationAudioSourceBytesV1 + 1;
        },
        "an over-budget WAV source must reject");
    rejects([](auto& value) { value.sourceFrameCount = 0; },
        "an empty Audio stream must reject");
    rejects(
        [](auto& value) {
            value.sourceFrameCount = value.sourceByteCount;
        },
        "Audio frame arithmetic must reject before multiplication overflow");
    rejects([](auto& value) { value.maximumSourceFrameIncrement = 0.0; },
        "a zero source-frame increment must reject");
    rejects(
        [](auto& value) {
            value.maximumSourceFrameIncrement =
                std::numeric_limits<double>::quiet_NaN();
        },
        "a non-finite source-frame increment must reject");
    rejects(
        [](auto& value) {
            value.maximumSourceFrameIncrement =
                activation::maximumComponentActivationAudioSourceFrameIncrementV1
                + 1.0;
        },
        "an over-budget source-frame increment must reject");
    rejects([](auto& value) { --value.parameterIdBytes; },
        "a partial Audio parameter identity must reject");
    rejects([](auto& value) { --value.resourceIdBytes; },
        "a partial Audio resource identity must reject");
    rejects([](auto& value) { value.parameterId[value.parameterIdBytes] = 'x'; },
        "Audio parameter padding must be zero");
    rejects([](auto& value) { value.resourceId[value.resourceIdBytes] = 'x'; },
        "Audio resource padding must be zero");
    rejects([](auto& value) { value.sourceSampleRate = 0; },
        "a zero Audio source rate must reject");
    rejects(
        [](auto& value) {
            value.sourceSampleRate =
                activation::maximumComponentActivationAudioSourceSampleRateV1
                + 1;
        },
        "an over-budget Audio source rate must reject");
    rejects([](auto& value) { ++value.streamProfileVersion; },
        "an unknown Audio stream profile must reject");
    rejects([](auto& value) { ++value.sourceEncoding; },
        "an unknown Audio source encoding must reject");
    rejects([](auto& value) { value.maximumReadFrames = 0; },
        "a zero Audio read budget must reject");
    rejects(
        [](auto& value) {
            value.maximumReadFrames =
                activation::maximumComponentActivationAudioReadFramesV1 + 1;
        },
        "an over-budget Audio read must reject");
    rejects([](auto& value) { --value.decodeBlockFrames; },
        "Audio decode block size drift must reject");
    rejects([](auto& value) { value.sourceChannelCount = 0; },
        "zero Audio source channels must reject");
    rejects([](auto& value) { value.sourceChannelCount = 3; },
        "more than stereo Audio must reject");
    rejects([](auto& value) { --value.maximumTouchedSourceBlocks; },
        "Audio touched-block budget drift must reject");
    rejects([](auto& value) { --value.requestCapacity; },
        "Audio request capacity drift must reject");
    rejects([](auto& value) { --value.responseCapacity; },
        "Audio response capacity drift must reject");
    rejects([](auto& value) { --value.workerCacheCapacity; },
        "Audio worker-cache capacity drift must reject");
    rejects([](auto& value) { --value.lowWatermarkBlocks; },
        "Audio low-watermark drift must reject");
    rejects([](auto& value) { --value.highWatermarkBlocks; },
        "Audio high-watermark drift must reject");
    rejects([](auto& value) { ++value.reserved0; },
        "Audio scalar reservations must reject");
    rejects([](auto& value) { ++value.reserved1.front(); },
        "Audio trailing reservations must reject");

    auto changedParameters = parameters;
    changedParameters.front().text[0] = 'x';
    require(
        activation::validateComponentActivationAudioStreamGrantV1(
            valid, authority, session, changedParameters)
            != activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "an Audio grant cannot detach from its exact Resource parameter");
    const std::array duplicateResources{
        audioResourceParameter(), audioResourceParameter()};
    require(
        activation::validateComponentActivationAudioStreamGrantV1(
            valid, authority, session, duplicateResources)
            != activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "an Audio grant cannot authorize a second resource");

    const auto staticSession = resourceSession();
    const auto staticAuthority = resourceAuthorization(staticSession);
    const std::array staticParameters{resourceParameter()};
    const auto staticGrant = resourceGrant(staticAuthority);
    require(
        activation::validateComponentActivationAudioStreamGrantV1(
            audioStreamGrant(staticAuthority),
            staticAuthority,
            staticSession,
            staticParameters)
            == activation::ComponentActivationProtocolDiagnosticV1::
                PermissionsInvalid,
        "the static Table/Binary profile cannot claim an Audio stream grant");
    require(
        activation::validateComponentActivationImmutableResourceGrantV1(
            staticGrant, authority, session, parameters)
            == activation::ComponentActivationProtocolDiagnosticV1::
                PermissionsInvalid,
        "the Audio stream profile cannot claim a static resource grant");
}

TalasKernelAbiImmutableAudioStreamPrepareV3 audioStreamPrepare(
    const activation::ComponentActivationAudioStreamGrantV1& grant)
{
    TalasKernelAbiImmutableAudioStreamPrepareV3 value{};
    value.structSize = sizeof(value);
    value.apiVersion = TALAS_KERNEL_API_VERSION_V3;
    value.id = {grant.resourceId.data(), grant.resourceIdBytes};
    value.generation = grant.resourceGeneration;
    std::copy(
        grant.sourceContentDigest.begin(),
        grant.sourceContentDigest.end(),
        value.sourceContentDigest);
    value.channelCount = grant.sourceChannelCount;
    value.sampleRate = grant.sourceSampleRate;
    value.frameCount = grant.sourceFrameCount;
    value.initialSourceFrame = 0.0;
    value.maximumSourceFrameIncrement =
        grant.maximumSourceFrameIncrement;
    value.maximumReadFrames = grant.maximumReadFrames;
    return value;
}

TalasKernelAbiImmutableAudioStreamViewV3 audioStreamView(
    const TalasKernelAbiImmutableAudioStreamPrepareV3& prepare,
    const activation::ComponentActivationAudioStreamGrantV1& grant)
{
    TalasKernelAbiImmutableAudioStreamViewV3 value{};
    value.structSize = sizeof(value);
    value.apiVersion = TALAS_KERNEL_API_VERSION_V3;
    value.id = prepare.id;
    value.generation = grant.resourceGeneration;
    std::copy(
        grant.sourceContentDigest.begin(),
        grant.sourceContentDigest.end(),
        value.sourceContentDigest);
    value.channelCount = grant.sourceChannelCount;
    value.sampleRate = grant.sourceSampleRate;
    value.frameCount = grant.sourceFrameCount;
    value.streamToken = 0x1234U;
    value.maximumSourceFrameIncrement =
        prepare.maximumSourceFrameIncrement;
    value.maximumReadFrames = prepare.maximumReadFrames;
    return value;
}

TalasKernelAbiImmutableAudioStreamReadV3 audioStreamRead(
    const TalasKernelAbiImmutableAudioStreamViewV3& view,
    double* output)
{
    TalasKernelAbiImmutableAudioStreamReadV3 value{};
    value.structSize = sizeof(value);
    value.apiVersion = TALAS_KERNEL_API_VERSION_V3;
    value.streamToken = view.streamToken;
    value.absoluteSourceFrame = 0.0;
    value.sourceFrameIncrement = 1.0;
    value.frameCount = view.maximumReadFrames;
    value.outputChannelCount = view.channelCount;
    value.interleavedOutput = output;
    value.outputSampleCapacity = value.frameCount * value.outputChannelCount;
    return value;
}

void testAudioStreamAbiRecordsRejectEveryStructuralMutation()
{
    const auto session = audioStreamSession();
    const auto authority = audioStreamAuthorization(session);
    auto grant = audioStreamGrant(authority);
    grant.maximumSourceFrameIncrement =
        activation::maximumComponentActivationAudioSourceFrameIncrementV1;
    const auto prepare = audioStreamPrepare(grant);
    require(
        activation::validateComponentActivationAudioStreamPrepareV3(
            prepare, grant)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "the exact Audio v3 prepare descriptor should validate");
    auto discoveryPrepare = prepare;
    discoveryPrepare.generation = 0;
    std::fill(
        std::begin(discoveryPrepare.sourceContentDigest),
        std::end(discoveryPrepare.sourceContentDigest),
        std::uint8_t{0});
    discoveryPrepare.channelCount = 0;
    discoveryPrepare.sampleRate = 0;
    discoveryPrepare.frameCount = 0;
    require(
        activation::validateComponentActivationAudioStreamPrepareV3(
            discoveryPrepare, grant)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "a generic component should discover the exact Host-owned Audio tuple");
    auto partialDiscovery = discoveryPrepare;
    partialDiscovery.generation = grant.resourceGeneration;
    require(
        activation::validateComponentActivationAudioStreamPrepareV3(
            partialDiscovery, grant)
            != activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "partial Audio identity discovery must reject");
    const auto rejectsPrepare = [&](auto mutate, std::string_view message) {
        auto changed = prepare;
        mutate(changed);
        require(
            activation::validateComponentActivationAudioStreamPrepareV3(
                changed, grant)
                != activation::ComponentActivationProtocolDiagnosticV1::Valid,
            std::string(message));
    };
    rejectsPrepare([](auto& value) { --value.structSize; },
        "Audio prepare struct-size drift must reject");
    rejectsPrepare([](auto& value) { --value.apiVersion; },
        "Audio prepare API drift must reject");
    rejectsPrepare([](auto& value) { value.id.data = nullptr; },
        "Audio prepare must carry a resource identity pointer");
    rejectsPrepare([](auto& value) { --value.id.byteCount; },
        "Audio prepare resource identity truncation must reject");
    rejectsPrepare([](auto& value) { ++value.generation; },
        "Audio prepare resource generation drift must reject");
    rejectsPrepare([](auto& value) { value.sourceContentDigest[0] ^= 0xffU; },
        "Audio prepare digest drift must reject");
    rejectsPrepare([](auto& value) { --value.channelCount; },
        "Audio prepare channel drift must reject");
    rejectsPrepare([](auto& value) { ++value.sampleRate; },
        "Audio prepare sample-rate drift must reject");
    rejectsPrepare([](auto& value) { --value.frameCount; },
        "Audio prepare frame-count drift must reject");
    rejectsPrepare([](auto& value) { value.initialSourceFrame = -1.0; },
        "negative Audio prepare positions must reject");
    rejectsPrepare(
        [](auto& value) {
            value.initialSourceFrame =
                std::numeric_limits<double>::quiet_NaN();
        },
        "non-finite Audio prepare positions must reject");
    rejectsPrepare(
        [](auto& value) {
            value.initialSourceFrame = static_cast<double>(value.frameCount);
        },
        "Audio prepare positions at EOF must reject");
    rejectsPrepare([](auto& value) { value.maximumSourceFrameIncrement = 0.0; },
        "zero Audio prepare increments must reject");
    rejectsPrepare(
        [](auto& value) {
            value.maximumSourceFrameIncrement += 1.0;
        },
        "Audio prepare increment widening must reject");
    rejectsPrepare([](auto& value) { value.maximumReadFrames = 0; },
        "zero Audio prepare read budgets must reject");
    rejectsPrepare([](auto& value) { ++value.maximumReadFrames; },
        "Audio prepare read-budget widening must reject");
    rejectsPrepare([](auto& value) { ++value.reserved0[0]; },
        "Audio prepare reservations must reject");

    const auto view = audioStreamView(prepare, grant);
    require(
        activation::validateComponentActivationAudioStreamViewV3(
            view, prepare, grant)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "the exact Audio v3 opaque-token view should validate");
    const auto discoveredView = audioStreamView(discoveryPrepare, grant);
    require(
        activation::validateComponentActivationAudioStreamViewV3(
            discoveredView, discoveryPrepare, grant)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "discovery must return the exact grant-bound Audio view");
    const auto rejectsView = [&](auto mutate, std::string_view message) {
        auto changed = view;
        mutate(changed);
        require(
            activation::validateComponentActivationAudioStreamViewV3(
                changed, prepare, grant)
                != activation::ComponentActivationProtocolDiagnosticV1::Valid,
            std::string(message));
    };
    rejectsView([](auto& value) { --value.structSize; },
        "Audio view struct-size drift must reject");
    rejectsView([](auto& value) { --value.apiVersion; },
        "Audio view API drift must reject");
    rejectsView([](auto& value) { --value.id.byteCount; },
        "Audio view resource identity drift must reject");
    rejectsView([](auto& value) { ++value.generation; },
        "Audio view generation drift must reject");
    rejectsView([](auto& value) { value.sourceContentDigest[0] ^= 0xffU; },
        "Audio view digest drift must reject");
    rejectsView([](auto& value) { --value.channelCount; },
        "Audio view channel drift must reject");
    rejectsView([](auto& value) { ++value.sampleRate; },
        "Audio view sample-rate drift must reject");
    rejectsView([](auto& value) { --value.frameCount; },
        "Audio view frame-count drift must reject");
    rejectsView([](auto& value) { value.streamToken = 0; },
        "Audio view token zero must reject");
    rejectsView([](auto& value) { --value.maximumSourceFrameIncrement; },
        "Audio view increment drift must reject");
    rejectsView([](auto& value) { --value.maximumReadFrames; },
        "Audio view read-budget drift must reject");
    rejectsView([](auto& value) { ++value.reserved0[1]; },
        "Audio view reservations must reject");

    std::array<double, 1024> output{};
    const auto read = audioStreamRead(view, output.data());
    require(
        activation::validateComponentActivationAudioStreamReadV3(read, view)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "one exact bounded interleaved Audio v3 read should validate");
    const auto rejectsRead = [&](auto mutate, std::string_view message) {
        auto changed = read;
        mutate(changed);
        require(
            activation::validateComponentActivationAudioStreamReadV3(
                changed, view)
                != activation::ComponentActivationProtocolDiagnosticV1::Valid,
            std::string(message));
    };
    rejectsRead([](auto& value) { --value.structSize; },
        "Audio read struct-size drift must reject");
    rejectsRead([](auto& value) { --value.apiVersion; },
        "Audio read API drift must reject");
    rejectsRead([](auto& value) { ++value.streamToken; },
        "Audio read token drift must reject");
    rejectsRead([](auto& value) { value.absoluteSourceFrame = -1.0; },
        "negative Audio read positions must reject");
    rejectsRead(
        [](auto& value) {
            value.absoluteSourceFrame =
                std::numeric_limits<double>::infinity();
        },
        "non-finite Audio read positions must reject");
    rejectsRead([](auto& value) { value.sourceFrameIncrement = 0.0; },
        "zero Audio read increments must reject");
    rejectsRead(
        [](auto& value) {
            value.sourceFrameIncrement =
                std::numeric_limits<double>::quiet_NaN();
        },
        "non-finite Audio read increments must reject");
    rejectsRead(
        [&](auto& value) {
            value.sourceFrameIncrement =
                view.maximumSourceFrameIncrement + 1.0;
        },
        "Audio read increment widening must reject");
    rejectsRead([](auto& value) { value.frameCount = 0; },
        "zero-frame Audio reads must reject");
    rejectsRead([&](auto& value) { value.frameCount = view.maximumReadFrames + 1; },
        "over-budget Audio reads must reject");
    rejectsRead([](auto& value) { --value.outputChannelCount; },
        "Audio read channel mismatch must reject");
    rejectsRead([](auto& value) { ++value.reserved0; },
        "Audio read scalar reservations must reject");
    rejectsRead([](auto& value) { value.interleavedOutput = nullptr; },
        "Audio read output pointer zero must reject");
    rejectsRead([](auto& value) { --value.outputSampleCapacity; },
        "Audio read output-capacity mismatch must reject");
    rejectsRead([](auto& value) { ++value.reserved1[0]; },
        "Audio read trailing reservations must reject");
    rejectsRead([](auto& value) { value.sourceFrameIncrement = 17.0; },
        "Audio reads spanning more than 16 source blocks must reject");

    TalasKernelAbiImmutableAudioStreamReadResultV3 result{};
    result.structSize = sizeof(result);
    result.outcome = TALAS_KERNEL_ABI_AUDIO_STREAM_READ_COMPLETE;
    result.frameCount = read.frameCount;
    result.seekGeneration = 1;
    result.expectedNextSourceFrame = read.absoluteSourceFrame
        + static_cast<double>(read.frameCount) * read.sourceFrameIncrement;
    require(
        activation::validateComponentActivationAudioStreamReadResultV3(
            result, read)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "one exact complete Audio v3 result should validate");
    const auto rejectsResult = [&](auto mutate, std::string_view message) {
        auto changed = result;
        mutate(changed);
        require(
            activation::validateComponentActivationAudioStreamReadResultV3(
                changed, read)
                != activation::ComponentActivationProtocolDiagnosticV1::Valid,
            std::string(message));
    };
    rejectsResult([](auto& value) { --value.structSize; },
        "Audio result struct-size drift must reject");
    rejectsResult([](auto& value) { value.outcome = 99; },
        "unknown Audio read outcomes must reject");
    rejectsResult([](auto& value) { --value.frameCount; },
        "Audio result frame-count drift must reject");
    rejectsResult([](auto& value) { value.missingFrameCount = 1; },
        "complete Audio results cannot hide underrun frames");
    rejectsResult([](auto& value) { value.endOfStreamFrameCount = 1; },
        "complete Audio results cannot hide EOF frames");
    rejectsResult([](auto& value) { value.seekGeneration = 0; },
        "Audio result seek generation zero must reject");
    rejectsResult([](auto& value) { ++value.expectedNextSourceFrame; },
        "Audio result progression drift must reject");
    rejectsResult([](auto& value) { ++value.reserved0[1]; },
        "Audio result reservations must reject");

    auto underrun = result;
    underrun.outcome = TALAS_KERNEL_ABI_AUDIO_STREAM_READ_UNDERRUN;
    underrun.missingFrameCount = 1;
    require(
        activation::validateComponentActivationAudioStreamReadResultV3(
            underrun, read)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "an explicit nonfatal Audio underrun result should validate");
    auto eof = result;
    eof.outcome = TALAS_KERNEL_ABI_AUDIO_STREAM_READ_END_OF_STREAM;
    eof.missingFrameCount = 4;
    eof.endOfStreamFrameCount = 4;
    require(
        activation::validateComponentActivationAudioStreamReadResultV3(
            eof, read)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "an explicit nonfatal Audio EOF result should validate");
    eof.missingFrameCount = 3;
    require(
        activation::validateComponentActivationAudioStreamReadResultV3(
            eof, read)
            != activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "EOF frames cannot exceed total missing frames");
}

activation::ComponentActivationTransitionV1 transition(
    activation::ComponentActivationRecordKindV1 kind,
    const activation::ComponentActivationAuthorizationV1& authority,
    activation::ComponentActivationRuntimeStateV1 state,
    activation::ComponentActivationRuntimeDiagnosticV1 diagnostic)
{
    activation::ComponentActivationTransitionV1 value;
    value.header = activation::componentActivationHeaderV1(kind, sizeof(value));
    value.contentDigest = authority.contentDigest;
    value.artifactGeneration = authority.artifactGeneration;
    value.activationGeneration = authority.activationGeneration;
    value.runtimeSessionGeneration = authority.runtimeSessionGeneration;
    value.sharedStorageIdentity = authority.sharedStorageIdentity;
    value.validationNonce = authority.validationNonce;
    value.runtimeSessionNonce = authority.runtimeSessionNonce;
    value.authorizationNonce = authority.authorizationNonce;
    value.state = static_cast<std::uint32_t>(state);
    value.diagnostic = static_cast<std::uint32_t>(diagnostic);
    value.sequence = 1;
    return value;
}

activation::ComponentActivationTransitionV1 sealedTransition(
    activation::ComponentActivationRecordKindV1 kind,
    const activation::ComponentActivationSealedSessionV1& session,
    std::uint64_t sequence = 1)
{
    activation::ComponentActivationTransitionV1 value;
    value.header = activation::componentActivationHeaderV1(kind, sizeof(value));
    value.runtimeSessionGeneration = session.runtimeSessionGeneration;
    value.sharedStorageIdentity = session.sharedStorageIdentity;
    value.runtimeSessionNonce = session.runtimeSessionNonce;
    value.state = static_cast<std::uint32_t>(
        activation::ComponentActivationRuntimeStateV1::Sealed);
    value.diagnostic = static_cast<std::uint32_t>(
        activation::ComponentActivationRuntimeDiagnosticV1::Ready);
    value.sequence = sequence;
    return value;
}

void testLayoutsAndExactAdmittedProfileValidate()
{
    static_assert(std::is_standard_layout_v<
        activation::ComponentActivationSealedSessionV1>);
    static_assert(std::is_trivially_copyable_v<
        activation::ComponentActivationAuthorizationV1>);
    static_assert(std::is_trivially_copyable_v<
        activation::ComponentActivationTransitionV1>);

    const auto sealed = sealedSession();
    const auto authority = authorization(sealed);
    require(
        activation::validateComponentActivationSealedSessionV1(sealed)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "the exact sealed runtime profile should validate");
    require(
        activation::validateComponentActivationAuthorizationV1(
            authority, sealed)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "the exact stateless realtime-audio authorization should validate");

    for (const auto sampleRate : {44'100U, 48'000U})
    {
        for (const auto blockFrames : {256U, 512U})
        {
            auto admitted = authority;
            admitted.sampleRate = sampleRate;
            admitted.blockFrames = blockFrames;
            require(
                activation::validateComponentActivationAuthorizationV1(
                    admitted, sealed)
                    == activation::ComponentActivationProtocolDiagnosticV1::Valid,
                "every declared exact-block profile should validate");
        }
    }
}

void testSealedSessionRejectsEveryAuthorityWidening()
{
    const auto valid = sealedSession();
    const auto rejects = [&](auto mutate, std::string_view message) {
        auto changed = valid;
        mutate(changed);
        require(
            activation::validateComponentActivationSealedSessionV1(changed)
                != activation::ComponentActivationProtocolDiagnosticV1::Valid,
            std::string(message));
    };
    rejects([](auto& value) { ++value.header.protocolVersion; },
        "protocol drift must reject");
    rejects([](auto& value) { ++value.profileVersion; },
        "profile drift must reject");
    rejects(
        [](auto& value) {
            value.supportedPermissions |= componentPermission(
                ComponentExecutionPermission::StateMigration);
        },
        "state-migration authority must reject");
    rejects([](auto& value) { value.safeguards = 0; },
        "incomplete safeguards must reject");
    rejects([](auto& value) { value.runtimeSessionGeneration = 0; },
        "missing runtime generation must reject");
    rejects([](auto& value) { value.sharedStorageIdentity = 0; },
        "missing shared-storage identity must reject");
    rejects([](auto& value) { value.sharedStorageBytes = 0; },
        "empty storage must reject");
    rejects(
        [](auto& value) {
            value.sharedStorageBytes =
                activation::maximumComponentActivationSharedStorageBytesV1 + 1;
        },
        "oversized storage must reject");
    rejects([](auto& value) { value.runtimeSessionNonce.fill(0); },
        "missing runtime nonce must reject");
    rejects([](auto& value) { value.reserved1.front() = 1; },
        "reserved fields must reject");
}

void testAuthorizationRejectsCandidateProfileAndSessionMutation()
{
    const auto sealed = sealedSession();
    const auto valid = authorization(sealed);
    const auto rejects = [&](auto mutate, std::string_view message) {
        auto changed = valid;
        mutate(changed);
        require(
            activation::validateComponentActivationAuthorizationV1(
                changed, sealed)
                != activation::ComponentActivationProtocolDiagnosticV1::Valid,
            std::string(message));
    };
    rejects([](auto& value) { value.bundleId[value.bundleIdBytes] = 'x'; },
        "noncanonical candidate padding must reject");
    rejects([](auto& value) { value.contentDigest.fill(0); },
        "missing content identity must reject");
    rejects([](auto& value) { value.signerIdentityDigest.fill(0); },
        "missing signer identity must reject");
    rejects([](auto& value) { value.descriptorId[0] = 'x'; },
        "a descriptor outside the bundle namespace must reject");
    rejects(
        [](auto& value) {
            value.descriptorId[value.descriptorIdBytes - 1U] = '_';
        },
        "a noncanonical descriptor identity must reject");
    rejects([](auto& value) { value.entryDigest.fill(0); },
        "missing entry digest must reject");
    rejects([](auto& value) { value.staticParameterDigest.fill(0); },
        "missing static-parameter binding must reject");
    rejects(
        [](auto& value) {
            value.requestedPermissions |= componentPermission(
                ComponentExecutionPermission::ImmutableResourceRead);
        },
        "permission widening must reject");
    rejects([](auto& value) { value.sampleRate = 96'000; },
        "unsupported sample rate must reject");
    rejects([](auto& value) { value.blockFrames = 128; },
        "unsupported block size must reject");
    rejects([](auto& value) { value.inputChannelCount = 0; },
        "zero input channels must reject");
    rejects([](auto& value) { value.outputChannelCount = 3; },
        "excess output channels must reject");
    rejects([](auto& value) { value.opaqueStateByteCount = 1; },
        "opaque component state must reject in the stateless profile");
    rejects([](auto& value) { value.artifactGeneration = 0; },
        "missing artifact generation must reject");
    rejects([](auto& value) { ++value.runtimeSessionGeneration; },
        "another runtime generation must reject");
    rejects([](auto& value) { ++value.sharedStorageIdentity; },
        "another shared-storage identity must reject");
    rejects([](auto& value) { value.runtimeSessionNonce.front() ^= 0xffU; },
        "another runtime nonce must reject");
    rejects(
        [](auto& value) {
            value.authorizationNonce = value.validationNonce;
        },
        "nonce-domain reuse must reject");
    rejects(
        [](auto& value) {
            value.runtimeSessionNonce = value.validationNonce;
        },
        "runtime/probe nonce-domain reuse must reject");
    rejects([](auto& value) { value.reserved1.front() = 1; },
        "reserved authorization fields must reject");
}

void testSealedPreauthorizationTransitionsHaveNoArtifactAuthority()
{
    const auto sealed = sealedSession();
    const auto ready = sealedTransition(
        activation::ComponentActivationRecordKindV1::SealedReady,
        sealed);
    require(
        activation::validateComponentActivationSealedTransitionV1(
            ready,
            activation::ComponentActivationRecordKindV1::SealedReady,
            sealed)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "an exactly bound sealed-runtime acknowledgement should validate");

    const auto heartbeat = sealedTransition(
        activation::ComponentActivationRecordKindV1::Heartbeat,
        sealed,
        2);
    require(
        activation::validateComponentActivationSealedTransitionV1(
            heartbeat,
            activation::ComponentActivationRecordKindV1::Heartbeat,
            sealed)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "a later exactly bound sealed heartbeat should validate");

    const auto rejects = [&](auto mutate, std::string_view message) {
        auto changed = ready;
        mutate(changed);
        require(
            activation::validateComponentActivationSealedTransitionV1(
                changed,
                activation::ComponentActivationRecordKindV1::SealedReady,
                sealed)
                != activation::ComponentActivationProtocolDiagnosticV1::Valid,
            std::string(message));
    };
    rejects([](auto& value) { value.contentDigest.front() = 1; },
        "sealed preauthorization must not carry an artifact identity");
    rejects([](auto& value) { value.artifactGeneration = 1; },
        "sealed preauthorization must not carry an artifact generation");
    rejects([](auto& value) { value.authorizationNonce.front() = 1; },
        "sealed preauthorization must not carry authorization evidence");
    rejects([](auto& value) { ++value.sharedStorageIdentity; },
        "sealed preauthorization must bind the exact shared storage");
    rejects([](auto& value) { value.sequence = 0; },
        "sealed preauthorization must reject a missing sequence");
    rejects(
        [](auto& value) {
            value.state = static_cast<std::uint32_t>(
                activation::ComponentActivationRuntimeStateV1::Authorized);
        },
        "sealed preauthorization must not claim authorization");
}

void testTransitionsAreExactlyBoundAndClosed()
{
    const auto sealed = sealedSession();
    const auto authority = authorization(sealed);
    const auto accepted = transition(
        activation::ComponentActivationRecordKindV1::AuthorizationAccepted,
        authority,
        activation::ComponentActivationRuntimeStateV1::Authorized,
        activation::ComponentActivationRuntimeDiagnosticV1::Ready);
    require(
        activation::validateComponentActivationTransitionV1(
            accepted,
            activation::ComponentActivationRecordKindV1::AuthorizationAccepted,
            authority)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "an exactly bound authorization acknowledgement should validate");

    auto mismatch = accepted;
    mismatch.authorizationNonce.front() ^= 0xffU;
    require(
        activation::validateComponentActivationTransitionV1(
            mismatch,
            activation::ComponentActivationRecordKindV1::AuthorizationAccepted,
            authority)
            == activation::ComponentActivationProtocolDiagnosticV1::
                SessionBindingMismatch,
        "an acknowledgement for another authority must reject");

    auto malformedAuthority = authority;
    malformedAuthority.contentDigest.fill(0);
    require(
        activation::validateComponentActivationTransitionV1(
            accepted,
            activation::ComponentActivationRecordKindV1::AuthorizationAccepted,
            malformedAuthority)
            == activation::ComponentActivationProtocolDiagnosticV1::CandidateInvalid,
        "a transition must not legitimize a malformed authority record");

    auto unknown = accepted;
    unknown.state = 0xff;
    require(
        activation::validateComponentActivationTransitionV1(
            unknown,
            activation::ComponentActivationRecordKindV1::AuthorizationAccepted,
            authority)
            == activation::ComponentActivationProtocolDiagnosticV1::StateUnknown,
        "unknown runtime states must reject");

    auto contradictory = accepted;
    contradictory.state = static_cast<std::uint32_t>(
        activation::ComponentActivationRuntimeStateV1::CanaryPassed);
    require(
        activation::validateComponentActivationTransitionV1(
            contradictory,
            activation::ComponentActivationRecordKindV1::AuthorizationAccepted,
            authority)
            == activation::ComponentActivationProtocolDiagnosticV1::
                StateDiagnosticMismatch,
        "known but contradictory transition pairs must reject");

    const auto quarantined = transition(
        activation::ComponentActivationRecordKindV1::Terminal,
        authority,
        activation::ComponentActivationRuntimeStateV1::Quarantined,
        activation::ComponentActivationRuntimeDiagnosticV1::ProcessCrashed);
    require(
        activation::validateComponentActivationTransitionV1(
            quarantined,
            activation::ComponentActivationRecordKindV1::Terminal,
            authority)
            == activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "one closed quarantined terminal pair should validate");
}

void testCommandsAreSequenceAndAuthorityBound()
{
    const auto sealed = sealedSession();
    const auto authority = authorization(sealed);
    for (const auto kind : {
             activation::ComponentActivationRecordKindV1::Canary,
             activation::ComponentActivationRecordKindV1::Reset,
             activation::ComponentActivationRecordKindV1::Heartbeat,
             activation::ComponentActivationRecordKindV1::Teardown})
    {
        const auto command = activation::makeComponentActivationCommandV1(
            authority, kind, 41);
        require(
            activation::validateComponentActivationCommandV1(
                command, kind, authority)
                == activation::ComponentActivationProtocolDiagnosticV1::Valid,
            "every closed runtime command should bind one exact sequence and authority");
    }

    auto command = activation::makeComponentActivationCommandV1(
        authority,
        activation::ComponentActivationRecordKindV1::Teardown,
        41);
    command.authorizationNonce.front() ^= 0xffU;
    require(
        activation::validateComponentActivationCommandV1(
            command,
            activation::ComponentActivationRecordKindV1::Teardown,
            authority)
            == activation::ComponentActivationProtocolDiagnosticV1::
                SessionBindingMismatch,
        "a command for another consumed authority must reject");
    command = activation::makeComponentActivationCommandV1(
        authority,
        activation::ComponentActivationRecordKindV1::Teardown,
        0);
    require(
        activation::validateComponentActivationCommandV1(
            command,
            activation::ComponentActivationRecordKindV1::Teardown,
            authority)
            == activation::ComponentActivationProtocolDiagnosticV1::
                SequenceInvalid,
        "a zero command sequence must reject");
    command = activation::makeComponentActivationCommandV1(
        authority,
        activation::ComponentActivationRecordKindV1::Prepared,
        41);
    require(
        activation::validateComponentActivationCommandV1(
            command,
            activation::ComponentActivationRecordKindV1::Prepared,
            authority)
            != activation::ComponentActivationProtocolDiagnosticV1::Valid,
        "a reply-only record kind must never become a host command");
}
}

int main()
{
    testAggregateResourceSetBudgetIsClosedAndFailureAtomic();
    testResourceSetRecordsBindOrderIdentityAndAggregateBudget();
    testStaticParametersAreCanonicalAndDigestBound();
    testImmutableTableEncodingIsCanonicalAndBounded();
    testImmutableBinaryCopyIsExactBoundedAndFailureAtomic();
    testImmutableResourceGrantRejectsEveryBindingMutation();
    testImmutableBinaryGrantRulesAreClosedAndLayoutPreserving();
    testAudioStreamGrantRejectsEveryBindingAndBudgetMutation();
    testAudioStreamAbiRecordsRejectEveryStructuralMutation();
    testLayoutsAndExactAdmittedProfileValidate();
    testSealedSessionRejectsEveryAuthorityWidening();
    testAuthorizationRejectsCandidateProfileAndSessionMutation();
    testSealedPreauthorizationTransitionsHaveNoArtifactAuthority();
    testTransitionsAreExactlyBoundAndClosed();
    testCommandsAreSequenceAndAuthorityBound();
    return 0;
}
