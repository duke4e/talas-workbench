#include "runtime/component_activation/macos/ComponentActivationMacOSHost.h"

#include "runtime/component_activation/macos/ComponentActivationXpcEnvelope.h"

#include <dispatch/dispatch.h>
#include <libproc.h>
#include <xpc/xpc.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

namespace talas::runtime::component_activation
{
namespace
{
constexpr char productionServiceIdentityV1[] =
    "com.talas.workbench.component-runtime";
constexpr char malformedServiceIdentityV1[] =
    "com.talas.workbench.component-runtime.fixture.malformed";
constexpr char hangServiceIdentityV1[] =
    "com.talas.workbench.component-runtime.fixture.hang";
constexpr char mismatchServiceIdentityV1[] =
    "com.talas.workbench.component-runtime.fixture.mismatch";
constexpr char heartbeatHangServiceIdentityV1[] =
    "com.talas.workbench.component-runtime.fixture.heartbeat-hang";
constexpr char missingServiceIdentityV1[] =
    "com.talas.workbench.component-runtime.fixture.missing";
constexpr char controlQueueIdentityV1[] =
    "com.talas.workbench.component-runtime.control.v1";
constexpr std::size_t sealedSharedStorageBytesV1 = 64U * 1024U;
constexpr std::int64_t prewarmDeadlineNanosecondsV1 = NSEC_PER_SEC * 2;
constexpr std::int64_t activationDeadlineNanosecondsV1 = NSEC_PER_SEC * 2;
constexpr std::int64_t heartbeatDeadlineNanosecondsV1 = NSEC_PER_SEC;
constexpr std::int64_t heartbeatIntervalNanosecondsV1 = NSEC_PER_MSEC * 100;

static_assert(sealedSharedStorageBytesV1
    <= maximumComponentActivationSharedStorageBytesV1);

[[nodiscard]] const char* serviceIdentity(
    ComponentActivationMacOSEndpointV1 endpoint) noexcept
{
    switch (endpoint)
    {
        case ComponentActivationMacOSEndpointV1::Production:
            return productionServiceIdentityV1;
        case ComponentActivationMacOSEndpointV1::FixtureMalformed:
            return malformedServiceIdentityV1;
        case ComponentActivationMacOSEndpointV1::FixtureHang:
            return hangServiceIdentityV1;
        case ComponentActivationMacOSEndpointV1::FixtureSessionMismatch:
            return mismatchServiceIdentityV1;
        case ComponentActivationMacOSEndpointV1::FixtureHeartbeatHang:
            return heartbeatHangServiceIdentityV1;
        case ComponentActivationMacOSEndpointV1::FixtureMissing:
            return missingServiceIdentityV1;
        case ComponentActivationMacOSEndpointV1::FixtureTerminationFailure:
        case ComponentActivationMacOSEndpointV1::FixtureProcessIdentityMismatch:
            return productionServiceIdentityV1;
    }
    return nullptr;
}

struct ActivationProcessIdentityV1
{
    std::int32_t processIdentifier = 0;
    std::uint64_t startSeconds = 0;
    std::uint64_t startMicroseconds = 0;
    uid_t effectiveUser = 0;
    bool valid = false;

    bool operator==(const ActivationProcessIdentityV1&) const = default;
};

[[nodiscard]] ActivationProcessIdentityV1 activationProcessIdentity(
    pid_t process) noexcept
{
    ActivationProcessIdentityV1 identity;
    if (process <= 0)
        return identity;
    proc_bsdinfo info{};
    if (proc_pidinfo(
            process,
            PROC_PIDTBSDINFO,
            0,
            &info,
            sizeof(info)) != static_cast<int>(sizeof(info))
        || info.pbi_pid != static_cast<std::uint32_t>(process)
        || info.pbi_uid != geteuid() || info.pbi_start_tvsec == 0)
    {
        return identity;
    }
    identity.processIdentifier = static_cast<std::int32_t>(process);
    identity.startSeconds = info.pbi_start_tvsec;
    identity.startMicroseconds = info.pbi_start_tvusec;
    identity.effectiveUser = info.pbi_uid;
    identity.valid = true;
    return identity;
}

[[nodiscard]] std::uint64_t nextRuntimeGeneration() noexcept
{
    static std::atomic<std::uint64_t> generation{1};
    auto value = generation.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<std::uint64_t>::max())
    {
        if (generation.compare_exchange_weak(
                value,
                value + 1U,
                std::memory_order_relaxed,
                std::memory_order_relaxed))
        {
            return value;
        }
    }
    return 0;
}

[[nodiscard]] std::uint64_t randomNonZeroIdentity() noexcept
{
    std::uint64_t value = 0;
    while (value == 0)
        arc4random_buf(&value, sizeof(value));
    return value;
}

void randomNonZeroNonce(
    std::array<std::uint8_t, componentActivationNonceBytesV1>& nonce) noexcept
{
    do
    {
        arc4random_buf(nonce.data(), nonce.size());
    }
    while (std::ranges::all_of(nonce, [](std::uint8_t value) {
        return value == 0;
    }));
}

[[nodiscard]] ComponentActivationSealedSessionV1 makeSealedSession(
    ComponentActivationMacOSProfileV1 profile) noexcept
{
    ComponentActivationSealedSessionV1 session;
    session.header = componentActivationHeaderV1(
        ComponentActivationRecordKindV1::SealedSession,
        sizeof(session));
    session.profileVersion = componentActivationProfileVersionV1;
    session.supportedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    if (profile
        == ComponentActivationMacOSProfileV1::
            ImmutableResource
        || profile
            == ComponentActivationMacOSProfileV1::
                MultipleImmutableOnlyResources)
    {
        session.supportedPermissions |= componentPermission(
            ComponentExecutionPermission::ImmutableResourceRead);
    }
    else if (profile
        == ComponentActivationMacOSProfileV1::BoundedAudioStream)
    {
        session.supportedPermissions |= componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);
    }
    else if (profile
        == ComponentActivationMacOSProfileV1::MultipleImmutableResources)
    {
        session.supportedPermissions |= componentPermission(
            ComponentExecutionPermission::ImmutableResourceRead);
        session.supportedPermissions |= componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);
    }
    session.safeguards = allComponentIsolationSafeguardsV1;
    session.runtimeSessionGeneration = nextRuntimeGeneration();
    session.sharedStorageIdentity = randomNonZeroIdentity();
    session.sharedStorageBytes = sealedSharedStorageBytesV1;
    randomNonZeroNonce(session.runtimeSessionNonce);
    return session;
}

[[nodiscard]] bool pristineSealedStorage(
    const ComponentActivationSealedSessionV1& session,
    const void* storage,
    std::size_t storageBytes) noexcept
{
    if (storage == nullptr || storageBytes != session.sharedStorageBytes
        || storageBytes < sizeof(session)
        || std::memcmp(storage, &session, sizeof(session)) != 0)
    {
        return false;
    }
    const auto* bytes = static_cast<const std::byte*>(storage);
    return std::all_of(
        bytes + sizeof(session),
        bytes + storageBytes,
        [](std::byte value) { return value == std::byte{}; });
}

[[nodiscard]] bool sealRuntimeBeforeComponentBytes(
    const ComponentActivationSealedSessionV1& session,
    void* storage,
    std::size_t storageBytes) noexcept
{
    return validateComponentActivationSealedSessionV1(session)
            == ComponentActivationProtocolDiagnosticV1::Valid
        && storageBytes
            >= componentActivationAudioStreamHealthPlaneOffsetV1
                + componentActivationAudioStreamHealthPlaneBytesV1
        && pristineSealedStorage(session, storage, storageBytes)
        && mprotect(
            storage,
            componentActivationAudioCanaryPlaneOffsetV1,
            PROT_READ) == 0;
}

[[nodiscard]] bool consumeComponentActivationAuthorization(
    ComponentActivationLeaseV1& source,
    std::optional<ComponentActivationLeaseV1>& destination,
    ComponentActivationAuthorizationV1& record) noexcept
{
    const auto* sourceRecord = source.record();
    if (destination.has_value() || sourceRecord == nullptr)
        return false;
    record = *sourceRecord;
    destination.emplace(std::move(source));
    const auto* destinationRecord = destination->record();
    return destinationRecord != nullptr
        && std::memcmp(destinationRecord, &record, sizeof(record)) == 0;
}

void sendFrozenComponentEntryBytes(
    xpc_connection_t connection,
    xpc_object_t message,
    dispatch_queue_t replyQueue,
    xpc_handler_t replyHandler) noexcept
{
    xpc_connection_send_message_with_reply(
        connection, message, replyQueue, replyHandler);
}

struct PendingMaterializationFile final
{
    PendingMaterializationFile() = default;
    PendingMaterializationFile(const PendingMaterializationFile&) = delete;
    PendingMaterializationFile& operator=(
        const PendingMaterializationFile&) = delete;

    ~PendingMaterializationFile()
    {
        closeDescriptor();
        cleanupPath();
    }

    void closeDescriptor() noexcept
    {
        if (descriptor >= 0)
        {
            static_cast<void>(close(descriptor));
            descriptor = -1;
        }
    }

    void cleanupPath() noexcept
    {
        if (path.front() != '\0')
        {
            static_cast<void>(unlink(path.data()));
            path.fill(0);
        }
    }

    void releasePath() noexcept
    {
        path.fill(0);
    }

    std::array<char, PATH_MAX> path{};
    int descriptor = -1;
};

[[nodiscard]] bool createMaterializationFile(
    PendingMaterializationFile& file) noexcept
{
    std::array<char, PATH_MAX> temporaryRoot{};
    const auto rootBytes = confstr(
        _CS_DARWIN_USER_TEMP_DIR,
        temporaryRoot.data(),
        temporaryRoot.size());
    if (rootBytes == 0 || rootBytes > temporaryRoot.size())
        return false;
    const auto rootLength = std::strlen(temporaryRoot.data());
    const auto separator = rootLength != 0
            && temporaryRoot[rootLength - 1U] == '/'
        ? ""
        : "/";
    const auto pathBytes = std::snprintf(
        file.path.data(),
        file.path.size(),
        "%s%sTalasComponentActivation.v1.XXXXXX",
        temporaryRoot.data(),
        separator);
    if (pathBytes <= 0
        || static_cast<std::size_t>(pathBytes) >= file.path.size())
    {
        return false;
    }
    file.descriptor = mkstemp(file.path.data());
    if (file.descriptor < 0)
        return false;
    if (ftruncate(file.descriptor, 0) != 0
        || fchmod(file.descriptor, 0500) != 0)
    {
        return false;
    }
    return true;
}

template <std::size_t Size>
[[nodiscard]] bool exactRecordText(
    const std::array<char, Size>& storage,
    std::uint32_t bytes,
    std::string_view expected) noexcept
{
    return bytes == expected.size() && bytes <= storage.size()
        && std::string_view{storage.data(), bytes} == expected;
}

[[nodiscard]] std::string canonicalDigestHex(
    const std::array<std::uint8_t, componentTrustDigestBytes>& digest)
{
    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string result(digest.size() * 2U, '0');
    for (std::size_t index = 0; index < digest.size(); ++index)
    {
        result[index * 2U] = hexadecimal[
            static_cast<std::size_t>(digest[index] >> 4U)];
        result[index * 2U + 1U] = hexadecimal[
            static_cast<std::size_t>(digest[index] & 0x0fU)];
    }
    return result;
}

[[nodiscard]] bool authorIsolatedComponentBinaryResourceContract(
    const ComponentActivationImmutableResourceGrantV1& grant,
    IsolatedComponentImmutableResourceContract& resource) noexcept
{
    if (grant.resourceKind
            != static_cast<std::uint32_t>(
                ComponentActivationImmutableResourceKindV1::Binary)
        || grant.elementFormat
            != static_cast<std::uint32_t>(
                ComponentActivationImmutableResourceElementFormatV1::RawBytes)
        || grant.elementCount == 0 || grant.byteCount != grant.elementCount
        || grant.sourceContentDigest != grant.mediatedPayloadDigest)
    {
        return false;
    }
    resource.kind = talas::core::ResourceSnapshotKind::Binary;
    return true;
}

[[nodiscard]] bool authorIsolatedComponentImmutableResourceContract(
    const ComponentActivationImmutableResourceGrantV1& grant,
    IsolatedComponentImmutableResourceContract& resource) noexcept
{
    switch (static_cast<ComponentActivationImmutableResourceKindV1>(
        grant.resourceKind))
    {
        case ComponentActivationImmutableResourceKindV1::Table:
            if (grant.elementFormat
                != static_cast<std::uint32_t>(
                    ComponentActivationImmutableResourceElementFormatV1::
                        Float64LittleEndian))
            {
                return false;
            }
            resource.kind = talas::core::ResourceSnapshotKind::Table;
            break;
        case ComponentActivationImmutableResourceKindV1::Binary:
            if (!authorIsolatedComponentBinaryResourceContract(
                    grant, resource))
            {
                return false;
            }
            break;
        default:
            return false;
    }
    resource.parameterId.assign(
        grant.parameterId.data(), grant.parameterIdBytes);
    resource.resourceId.assign(
        grant.resourceId.data(), grant.resourceIdBytes);
    resource.generation = grant.resourceGeneration;
    resource.sourceContentHash = canonicalDigestHex(
        grant.sourceContentDigest);
    resource.mediatedPayloadDigest = grant.mediatedPayloadDigest;
    resource.elementCount = grant.elementCount;
    resource.byteCount = grant.byteCount;
    return true;
}

void authorIsolatedComponentAudioStreamContract(
    const ComponentActivationAudioStreamGrantV1& grant,
    IsolatedComponentAudioStreamContract& audio)
{
    audio.parameterId.assign(
        grant.parameterId.data(), grant.parameterIdBytes);
    audio.resourceId.assign(grant.resourceId.data(), grant.resourceIdBytes);
    audio.generation = grant.resourceGeneration;
    audio.sourceContentHash = canonicalDigestHex(grant.sourceContentDigest);
    audio.sourceByteCount = grant.sourceByteCount;
    audio.sourceFrameCount = grant.sourceFrameCount;
    audio.maximumSourceFrameIncrement = grant.maximumSourceFrameIncrement;
    audio.sourceSampleRate = grant.sourceSampleRate;
    audio.streamProfileVersion = grant.streamProfileVersion;
    audio.sourceEncoding = grant.sourceEncoding;
    audio.maximumReadFrames = grant.maximumReadFrames;
    audio.decodeBlockFrames = grant.decodeBlockFrames;
    audio.sourceChannelCount = grant.sourceChannelCount;
    audio.maximumTouchedSourceBlocks = grant.maximumTouchedSourceBlocks;
    audio.requestCapacity = grant.requestCapacity;
    audio.responseCapacity = grant.responseCapacity;
    audio.workerCacheCapacity = grant.workerCacheCapacity;
    audio.lowWatermarkBlocks = grant.lowWatermarkBlocks;
    audio.highWatermarkBlocks = grant.highWatermarkBlocks;
}

[[nodiscard]] bool authorIsolatedComponentResourceSetContract(
    const ComponentActivationResourceSetGrantV1& grant,
    std::span<const ComponentActivationResourceSetMemberV1> members,
    std::span<
        const ComponentActivationImmutableResourceGrantV1* const>
        immutableGrants,
    std::span<const ComponentActivationAudioStreamGrantV1* const>
        audioGrants,
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session,
    std::span<const ComponentActivationStaticParameterV1> staticParameters,
    IsolatedComponentRuntimeContract& contract)
{
    if (validateComponentActivationResourceSetGrantV1(
            grant, authorization, session)
            != ComponentActivationProtocolDiagnosticV1::Valid
        || validateComponentActivationResourceSetMembersV1(grant, members)
            != ComponentActivationProtocolDiagnosticV1::Valid
        || immutableGrants.size() != members.size()
        || audioGrants.size() != members.size())
    {
        return false;
    }
    IsolatedComponentResourceSetContract resources;
    resources.grant = grant;
    resources.memberRecords.assign(members.begin(), members.end());
    resources.members.reserve(members.size());
    for (std::size_t index = 0; index < members.size(); ++index)
    {
        const auto kind = static_cast<
            ComponentActivationResourceSetMemberKindV1>(
                members[index].memberKind);
        if (kind == ComponentActivationResourceSetMemberKindV1::Audio)
        {
            const auto* audioGrant = audioGrants[index];
            if (audioGrant == nullptr
                || immutableGrants[index] != nullptr
                || validateComponentActivationAudioStreamGrantV1(
                       *audioGrant,
                       authorization,
                       session,
                       staticParameters)
                    != ComponentActivationProtocolDiagnosticV1::Valid)
            {
                return false;
            }
            IsolatedComponentAudioStreamContract audio;
            authorIsolatedComponentAudioStreamContract(*audioGrant, audio);
            resources.members.emplace_back(std::move(audio));
            continue;
        }
        const auto* immutableGrant = immutableGrants[index];
        if (immutableGrant == nullptr
            || audioGrants[index] != nullptr
            || validateComponentActivationImmutableResourceGrantV1(
                   *immutableGrant,
                   authorization,
                   session,
                   staticParameters)
                != ComponentActivationProtocolDiagnosticV1::Valid)
        {
            return false;
        }
        IsolatedComponentImmutableResourceContract immutable;
        if (!authorIsolatedComponentImmutableResourceContract(
                *immutableGrant, immutable)
            || (kind == ComponentActivationResourceSetMemberKindV1::Table
                && immutable.kind
                    != talas::core::ResourceSnapshotKind::Table)
            || (kind == ComponentActivationResourceSetMemberKindV1::Binary
                && immutable.kind
                    != talas::core::ResourceSnapshotKind::Binary))
        {
            return false;
        }
        resources.members.emplace_back(std::move(immutable));
    }
    contract.resources = std::move(resources);
    return true;
}

[[nodiscard]] AudioStreamWorkerState isolatedAudioWorkerState(
    ComponentActivationAudioStreamWorkerStateV1 state) noexcept
{
    switch (state)
    {
        case ComponentActivationAudioStreamWorkerStateV1::Waiting:
            return AudioStreamWorkerState::Waiting;
        case ComponentActivationAudioStreamWorkerStateV1::Filling:
            return AudioStreamWorkerState::Filling;
        case ComponentActivationAudioStreamWorkerStateV1::Ready:
            return AudioStreamWorkerState::Ready;
        case ComponentActivationAudioStreamWorkerStateV1::EndOfStream:
            return AudioStreamWorkerState::EndOfStream;
        case ComponentActivationAudioStreamWorkerStateV1::Failed:
        case ComponentActivationAudioStreamWorkerStateV1::Stopped:
            return AudioStreamWorkerState::Failed;
    }
    return AudioStreamWorkerState::Failed;
}

[[nodiscard]] IsolatedComponentAudioStreamHealthSnapshot
authorIsolatedComponentAudioStreamHealth(
    const ComponentActivationAudioStreamHealthV1& source) noexcept
{
    IsolatedComponentAudioStreamHealthSnapshot result;
    result.seekGeneration = source.seekGeneration;
    result.health.underrunEvents = source.underrunEvents;
    result.health.missingFrames = source.missingFrames;
    result.health.discontinuities = source.discontinuities;
    result.health.requestRejections = source.requestRejections;
    result.health.responseQueueFull = source.responseFullEvents;
    result.health.staleResponseBlocks = source.staleBlocks;
    result.health.decodedBlocks = source.decodedBlocks;
    result.health.decodeFailures = source.decodeFailures;
    result.health.endOfStreamFrames = source.endOfStreamFrames;
    result.health.bufferedBlocks = static_cast<std::size_t>(
        source.bufferedBlocks);
    result.health.bufferCapacity = componentActivationAudioResponseCapacityV1;
    result.health.lowWatermark = componentActivationAudioLowWatermarkBlocksV1;
    result.health.highWatermark = componentActivationAudioHighWatermarkBlocksV1;
    result.health.pendingRequests = static_cast<std::size_t>(
        source.pendingRequests);
    result.health.workerState = isolatedAudioWorkerState(source.workerState);
    return result;
}

[[nodiscard]] std::optional<IsolatedComponentRuntimeContract>
makePreparedRuntimeContract(
    std::string nodeId,
    const component_bundle::ComponentBundleSnapshot& snapshot,
    std::span<const ComponentActivationStaticParameterV1> staticParameters,
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session,
    const ComponentActivationImmutableResourceGrantV1* resourceGrant,
    const ComponentActivationAudioStreamGrantV1* audioStreamGrant,
    const ComponentActivationResourceSetGrantV1* resourceSetGrant,
    std::span<const ComponentActivationResourceSetMemberV1>
        resourceSetMembers,
    std::span<
        const ComponentActivationImmutableResourceGrantV1* const>
        resourceSetImmutableGrants,
    std::span<const ComponentActivationAudioStreamGrantV1* const>
        resourceSetAudioGrants)
{
    if (nodeId.empty()
        || validateComponentActivationAuthorizationV1(authorization, session)
            != ComponentActivationProtocolDiagnosticV1::Valid
        || !component_bundle::validateComponentBundleSnapshotIdentity(snapshot)
        || snapshot.manifest.nodes.size() != 1
        || snapshot.contentDigest != authorization.contentDigest
        || !exactRecordText(
            authorization.bundleId,
            authorization.bundleIdBytes,
            snapshot.manifest.bundleId)
        || !exactRecordText(
            authorization.semanticVersion,
            authorization.semanticVersionBytes,
            snapshot.manifest.semanticVersion)
        || !exactRecordText(
            authorization.descriptorId,
            authorization.descriptorIdBytes,
            snapshot.manifest.nodes.front().typeId)
        || staticParameters.size()
            != snapshot.manifest.nodes.front().parameters.size()
        || staticParameters.size() != authorization.staticParameterCount
        || calculateComponentActivationStaticParameterDigestV1(
               staticParameters)
            != authorization.staticParameterDigest
        || authorization.opaqueStateByteCount != 0
        || authorization.activationGeneration == 0)
    {
        return std::nullopt;
    }

    const auto& manifestNode = snapshot.manifest.nodes.front();
    const auto resourceParameterCount = std::ranges::count_if(
        staticParameters,
        [](const auto& parameter) {
            return parameter.kind == static_cast<std::uint32_t>(
                ComponentActivationStaticParameterKindV1::Resource);
        });
    if ((resourceGrant != nullptr && audioStreamGrant != nullptr)
        || (resourceSetGrant != nullptr
            && (resourceGrant != nullptr || audioStreamGrant != nullptr))
        || (resourceParameterCount == 0
            && (resourceGrant != nullptr || audioStreamGrant != nullptr
                || resourceSetGrant != nullptr))
        || (resourceSetGrant == nullptr && resourceParameterCount == 1
            && ((resourceGrant == nullptr && audioStreamGrant == nullptr)
                || (resourceGrant != nullptr
                    && validateComponentActivationImmutableResourceGrantV1(
                           *resourceGrant,
                           authorization,
                           session,
                           staticParameters)
                        != ComponentActivationProtocolDiagnosticV1::Valid)
                || (audioStreamGrant != nullptr
                    && validateComponentActivationAudioStreamGrantV1(
                           *audioStreamGrant,
                           authorization,
                           session,
                           staticParameters)
                        != ComponentActivationProtocolDiagnosticV1::Valid)))
        || (resourceSetGrant == nullptr && resourceParameterCount > 1)
        || (resourceSetGrant != nullptr
            && (static_cast<std::size_t>(resourceParameterCount) != resourceSetMembers.size()
                || resourceParameterCount < 2
                || resourceParameterCount
                    > maximumComponentActivationResourceSetMembersV1)))
    {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < staticParameters.size(); ++index)
    {
        if (std::string_view{
                staticParameters[index].id.data(),
                staticParameters[index].idBytes}
            != manifestNode.parameters[index].id)
        {
            return std::nullopt;
        }
    }

    IsolatedComponentRuntimeContract contract;
    contract.nodeId = std::move(nodeId);
    contract.typeId = manifestNode.typeId;
    contract.typeVersion = snapshot.manifest.semanticVersion;
    contract.bundleId = snapshot.manifest.bundleId;
    contract.contentDigest = authorization.contentDigest;
    contract.signerIdentityDigest = authorization.signerIdentityDigest;
    contract.requestedPermissions = authorization.requestedPermissions;
    contract.policyVersion = authorization.policyVersion;
    contract.stateSchemaVersion = manifestNode.stateSchemaVersion;
    contract.inputs.reserve(manifestNode.inputs.size());
    contract.outputs.reserve(manifestNode.outputs.size());
    contract.parameters.reserve(manifestNode.parameters.size());
    for (const auto& input : manifestNode.inputs)
    {
        contract.inputs.push_back({
            input.id,
            input.type,
            input.required,
            input.signal,
        });
    }
    for (const auto& output : manifestNode.outputs)
    {
        contract.outputs.push_back({
            output.id,
            output.type,
            output.required,
            output.signal,
        });
    }
    for (const auto& parameter : manifestNode.parameters)
    {
        contract.parameters.push_back({
            parameter.id,
            parameter.kind,
            parameter.domain,
            parameter.choices,
        });
    }
    contract.staticParameters.assign(
        staticParameters.begin(), staticParameters.end());
    if (resourceGrant != nullptr)
    {
        IsolatedComponentImmutableResourceContract resource;
        if (!authorIsolatedComponentImmutableResourceContract(
                *resourceGrant, resource))
        {
            return std::nullopt;
        }
        IsolatedComponentResourceSetContract resources;
        resources.members.emplace_back(std::move(resource));
        contract.resources = std::move(resources);
    }
    if (audioStreamGrant != nullptr)
    {
        IsolatedComponentAudioStreamContract audio;
        authorIsolatedComponentAudioStreamContract(*audioStreamGrant, audio);
        IsolatedComponentResourceSetContract resources;
        resources.members.emplace_back(std::move(audio));
        contract.resources = std::move(resources);
    }
    if (resourceSetGrant != nullptr
        && !authorIsolatedComponentResourceSetContract(
            *resourceSetGrant,
            resourceSetMembers,
            resourceSetImmutableGrants,
            resourceSetAudioGrants,
            authorization,
            session,
            staticParameters,
            contract))
    {
        return std::nullopt;
    }
    contract.sampleRate = authorization.sampleRate;
    contract.blockFrames = authorization.blockFrames;
    contract.activationGeneration = authorization.activationGeneration;
    return contract;
}
}

struct ComponentActivationMacOSHost::Impl
    : std::enable_shared_from_this<ComponentActivationMacOSHost::Impl>
{
    Impl(
        ComponentActivationMacOSEndpointV1 preparedEndpoint,
        const char* preparedServiceIdentity,
        ComponentActivationSealedSessionV1 preparedSession,
        void* preparedStorage,
        std::size_t preparedStorageBytes,
        xpc_object_t preparedSharedMemory) noexcept
        : endpoint(preparedEndpoint),
          serviceIdentity(preparedServiceIdentity),
          session(preparedSession),
          sharedStorage(preparedStorage),
          sharedStorageBytes(preparedStorageBytes),
          sharedMemory(preparedSharedMemory)
        , simulateTerminationFailure(
              preparedEndpoint
              == ComponentActivationMacOSEndpointV1::
                  FixtureTerminationFailure)
        , simulateProcessIdentityMismatch(
              preparedEndpoint
              == ComponentActivationMacOSEndpointV1::
                  FixtureProcessIdentityMismatch)
    {
    }

    ~Impl()
    {
        clearPendingActivation();
        cleanupMaterializationPath();
        if (connection != nullptr)
        {
            bool expected = false;
            if (connectionCancelled.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                xpc_connection_cancel(connection);
            }
            xpc_release(connection);
        }
        if (sharedMemory != nullptr)
            xpc_release(sharedMemory);
        if (sharedStorage != nullptr)
            static_cast<void>(munmap(sharedStorage, sharedStorageBytes));
#if !OS_OBJECT_USE_OBJC
        if (controlQueue != nullptr)
            dispatch_release(controlQueue);
#endif
    }

    [[nodiscard]] std::uint64_t advanceDeadlineToken() noexcept
    {
        auto current = deadlineToken.load(std::memory_order_relaxed);
        while (true)
        {
            auto next = current + 1U;
            if (next == 0)
                next = 1;
            if (deadlineToken.compare_exchange_weak(
                    current,
                    next,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                return next;
            }
        }
    }

    void invalidateDeadline() noexcept
    {
        static_cast<void>(advanceDeadlineToken());
    }

    [[nodiscard]] std::uint64_t armDeadline(
        std::int64_t nanoseconds) noexcept
    {
        const auto token = advanceDeadlineToken();
        const auto weak = weak_from_this();
        dispatch_after(
            dispatch_time(DISPATCH_TIME_NOW, nanoseconds),
            controlQueue,
            ^{
                if (const auto implementation = weak.lock())
                    implementation->deadlineExpired(token);
            });
        return token;
    }

    void revokeRuntimeEvidence() noexcept
    {
        if (runtimeSession.has_value())
            runtimeSession->revoke();
    }

    void revokeActivationLease() noexcept
    {
        if (activationLease.has_value())
            activationLease->revoke();
    }

    [[nodiscard]] bool audioActive() const noexcept
    {
        return stateSnapshot.load(std::memory_order_acquire)
                == ComponentActivationMacOSHostStateV1::PreparedActivation
            && !audioFailureLatched.load(std::memory_order_acquire)
            && audioHost.has_value() && !audioHost->isQuarantined()
            && activationLease.has_value() && activationLease->active();
    }

    [[nodiscard]] bool healthInspectionActive() const noexcept
    {
        const auto state = stateSnapshot.load(std::memory_order_acquire);
        return (state == ComponentActivationMacOSHostStateV1::PreparedActivation
                || state == ComponentActivationMacOSHostStateV1::Checkpointing)
            && !audioFailureLatched.load(std::memory_order_acquire)
            && audioHost.has_value() && !audioHost->isQuarantined()
            && activationLease.has_value() && activationLease->active();
    }

    [[nodiscard]] bool expectsAudioStreamHealth() const noexcept
    {
        return activationAudioStreamGrant.has_value()
            || activationResourceSetHealthBinding.has_value();
    }

    [[nodiscard]] bool primeAudioStreamHealth() noexcept
    {
        const std::lock_guard inspection(healthInspectionMutex);
        if (activationResourceSetHealthBinding.has_value())
        {
            if (activationAudioStreamGrant.has_value()
                || audioStreamHealthConsumer.has_value()
                || !audioStreamSetHealthConsumer.has_value()
                || audioStreamSetHealthConsumer->slotCount()
                    != activationResourceSetHealthBinding->slotCount)
            {
                return false;
            }
            for (std::size_t slot = 0;
                 slot < activationResourceSetHealthBinding->slotCount;
                 ++slot)
            {
                const auto observed =
                    audioStreamSetHealthConsumer->readDetailed(slot);
                if (observed.diagnostic
                    != ComponentActivationAudioStreamHealthReadDiagnosticV1::
                        Valid)
                {
                    return false;
                }
                audioStreamSetHealthCache[slot] =
                    authorIsolatedComponentAudioStreamHealth(observed.health);
            }
            IsolatedComponentAudioStreamHealthSetSnapshot complete;
            complete.streamCount = activationResourceSetHealthBinding->slotCount;
            for (std::size_t slot = 0; slot < complete.streamCount; ++slot)
            {
                if (audioStreamSetHealthConsumer->lastAcceptedSequence(slot)
                    != audioStreamSetHealthConsumer->lastAcceptedSequence(0))
                    return false;
                complete.streams[slot] = *audioStreamSetHealthCache[slot];
            }
            completeSetHealthCache = complete;
            return true;
        }
        if (!activationAudioStreamGrant.has_value())
        {
            return !audioStreamHealthConsumer.has_value()
                && !audioStreamSetHealthConsumer.has_value();
        }
        if (!audioStreamHealthConsumer.has_value()
            || audioStreamSetHealthConsumer.has_value())
            return false;
        const auto observed = audioStreamHealthConsumer->readDetailed();
        if (observed.diagnostic
            != ComponentActivationAudioStreamHealthReadDiagnosticV1::Valid)
            return false;
        audioStreamHealthCache =
            authorIsolatedComponentAudioStreamHealth(observed.health);
        return true;
    }

    [[nodiscard]] std::optional<
        IsolatedComponentAudioStreamHealthSetSnapshot> readAudioStreamHealthSet()
        noexcept
    {
        if (!healthInspectionActive())
        {
            return std::nullopt;
        }
        if (activationResourceSetHealthBinding.has_value())
        {
            if (!audioStreamSetHealthConsumer.has_value()
                || audioStreamSetHealthConsumer->slotCount()
                    != activationResourceSetHealthBinding->slotCount)
            {
                audioHost->quarantine(
                    ComponentActivationAudioQuarantineReasonV1::
                        StreamHealthInvalid);
                audioFailureLatched.store(true, std::memory_order_release);
                return std::nullopt;
            }
            for (std::size_t slot = 0;
                 slot < activationResourceSetHealthBinding->slotCount;
                 ++slot)
            {
                const auto observed =
                    audioStreamSetHealthConsumer->readDetailed(slot);
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
                auto hold = 1;
                if (slot == 0 && heldSetHealthReadForTest.compare_exchange_strong(hold, 2))
                    while (heldSetHealthReadForTest.load() == 2) usleep(1'000);
#endif
                if (observed.diagnostic
                    == ComponentActivationAudioStreamHealthReadDiagnosticV1::
                        Valid)
                {
                    audioStreamSetHealthCache[slot] =
                        authorIsolatedComponentAudioStreamHealth(
                            observed.health);
                    continue;
                }
                if (observed.diagnostic
                        == ComponentActivationAudioStreamHealthReadDiagnosticV1::
                            Busy
                    && audioStreamSetHealthCache[slot].has_value())
                {
                    continue;
                }
                audioHost->quarantine(
                    ComponentActivationAudioQuarantineReasonV1::
                        StreamHealthInvalid);
                audioFailureLatched.store(true, std::memory_order_release);
                return std::nullopt;
            }
            // This activation worker publishes every slot once per block or
            // checkpoint boundary. A partially observed round keeps the last
            // complete set; each fresh record was still validated above.
            for (std::size_t slot = 1;
                 slot < activationResourceSetHealthBinding->slotCount; ++slot)
                if (audioStreamSetHealthConsumer->lastAcceptedSequence(slot)
                    != audioStreamSetHealthConsumer->lastAcceptedSequence(0))
                    return completeSetHealthCache;
            IsolatedComponentAudioStreamHealthSetSnapshot result;
            result.streamCount =
                activationResourceSetHealthBinding->slotCount;
            for (std::size_t slot = 0; slot < result.streamCount; ++slot)
            {
                if (!audioStreamSetHealthCache[slot].has_value())
                    return std::nullopt;
                result.streams[slot] = *audioStreamSetHealthCache[slot];
            }
            completeSetHealthCache = result;
            return result;
        }
        if (!activationAudioStreamGrant.has_value()
            || !audioStreamHealthConsumer.has_value()
            || audioStreamSetHealthConsumer.has_value())
        {
            return std::nullopt;
        }
        // The inspection mutex serializes control consumers. The shared plane
        // is lock-free; a busy seqlock publication keeps the last exact value,
        // while stable absence/corruption immediately makes audio fail closed.
        const auto observed = audioStreamHealthConsumer->readDetailed();
        if (observed.diagnostic
            == ComponentActivationAudioStreamHealthReadDiagnosticV1::Valid)
        {
            audioStreamHealthCache =
                authorIsolatedComponentAudioStreamHealth(observed.health);
            IsolatedComponentAudioStreamHealthSetSnapshot result;
            result.streamCount = 1;
            result.streams[0] = *audioStreamHealthCache;
            return result;
        }
        if (observed.diagnostic
                == ComponentActivationAudioStreamHealthReadDiagnosticV1::Busy
            && audioStreamHealthCache.has_value())
        {
            IsolatedComponentAudioStreamHealthSetSnapshot result;
            result.streamCount = 1;
            result.streams[0] = *audioStreamHealthCache;
            return result;
        }
        audioHost->quarantine(
            ComponentActivationAudioQuarantineReasonV1::StreamHealthInvalid);
        audioFailureLatched.store(true, std::memory_order_release);
        return std::nullopt;
    }

    [[nodiscard]] std::optional<IsolatedComponentAudioStreamHealthSetSnapshot>
    audioStreamHealthSet() noexcept
    {
        const std::lock_guard inspection(healthInspectionMutex);
        if (!healthInspectionActive()) return std::nullopt;
        const auto observed = readAudioStreamHealthSet();
        if (!healthInspectionActive()
            || (expectsAudioStreamHealth() && !observed.has_value())) return std::nullopt;
        if (!checkpointHealthPending && !checkpointHealthHeld)
            publishedHealthSnapshot = observed;
        return publishedHealthSnapshot;
    }

    [[nodiscard]] bool beginHealthControl() noexcept
    {
        const std::lock_guard inspection(healthInspectionMutex);
        if (!healthInspectionActive()) return false;
        const auto observed = readAudioStreamHealthSet();
        if (!healthInspectionActive()
            || (expectsAudioStreamHealth() && !observed.has_value())) return false;
        if (!checkpointHealthHeld) publishedHealthSnapshot = observed;
        checkpointHealthPending = true;
        return true;
    }

    [[nodiscard]] bool finishHealthControl(
        ComponentActivationCheckpointOperationV1 operation,
        ComponentActivationCheckpointStatusV1 status) noexcept
    {
        const std::lock_guard inspection(healthInspectionMutex);
        if (!healthInspectionActive()) return false;
        const auto observed = readAudioStreamHealthSet();
        if (!healthInspectionActive()
            || (expectsAudioStreamHealth() && !observed.has_value())) return false;
        using Operation = ComponentActivationCheckpointOperationV1;
        if (operation == Operation::Begin && status == ComponentActivationCheckpointStatusV1::Captured)
        {
            publishedHealthSnapshot = observed;
            checkpointHealthHeld = true;
        }
        else if (operation == Operation::Restore || operation == Operation::Commit)
        {
            publishedHealthSnapshot = observed;
            checkpointHealthHeld = false;
        }
        else if (!checkpointHealthHeld) publishedHealthSnapshot = observed;
        checkpointHealthPending = false;
        return true;
    }

#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
    [[nodiscard]] bool publishedAudioResponseReadyForTesting() const noexcept
    {
        return audioActive() && audioHost->publishedResponseReady();
    }
#endif

    [[nodiscard]] bool tryBeginStage() noexcept
    {
        if (activationStageLease.has_value() || !activationLease.has_value())
            return false;
        auto stageLease = activationLease->tryAcquireStageLease();
        if (!stageLease.has_value() || !stageLease->active())
            return false;
        activationStageLease.emplace(std::move(*stageLease));
        return true;
    }

    void endStage() noexcept
    {
        activationStageLease.reset();
    }

    [[nodiscard]] ComponentActivationAudioOperationV1 processAudio(
        std::span<const float* const> inputChannels,
        std::span<float* const> outputChannels,
        std::uint32_t frameCount,
        ComponentActivationAudioTransportV1 transport) noexcept
    {
        if (!audioActive())
        {
            if (stateSnapshot.load(std::memory_order_acquire)
                    == ComponentActivationMacOSHostStateV1::
                        PreparedActivation
                && audioHost.has_value()
                && (audioHost->isQuarantined()
                    || !activationLease.has_value()
                    || !activationLease->active()))
            {
                audioFailureLatched.store(true, std::memory_order_release);
            }
            for (auto* channel : outputChannels)
            {
                if (channel != nullptr)
                    std::fill_n(channel, frameCount, 0.0F);
            }
            return ComponentActivationAudioOperationV1::Quarantined;
        }
        const auto operation = audioHost->process(
            inputChannels,
            outputChannels,
            frameCount,
            transport,
            activationLease->active());
        if (operation != ComponentActivationAudioOperationV1::FirstBlockSilence
            && operation != ComponentActivationAudioOperationV1::Rendered)
        {
            audioFailureLatched.store(true, std::memory_order_release);
        }
        return operation;
    }

    void quarantineAudio(IsolatedComponentQuarantineReason reason) noexcept
    {
        auto mapped = ComponentActivationAudioQuarantineReasonV1::
            ExplicitControl;
        switch (reason)
        {
            case IsolatedComponentQuarantineReason::Revoked:
                mapped = ComponentActivationAudioQuarantineReasonV1::Revoked;
                // The render session reaches this branch only after active()
                // has already observed revoked live authority. Keep the audio
                // callback lock-free; control-thread authority invalidation
                // owns the stage-gate rendezvous.
                break;
            case IsolatedComponentQuarantineReason::InvalidCallbackProfile:
                mapped = ComponentActivationAudioQuarantineReasonV1::
                    InvalidCallbackProfile;
                break;
            case IsolatedComponentQuarantineReason::InvalidTransport:
                mapped = ComponentActivationAudioQuarantineReasonV1::
                    RequestBindingMismatch;
                break;
            case IsolatedComponentQuarantineReason::NonFiniteInput:
                mapped = ComponentActivationAudioQuarantineReasonV1::
                    InputNonFinite;
                break;
            case IsolatedComponentQuarantineReason::NonFiniteOutput:
                mapped = ComponentActivationAudioQuarantineReasonV1::
                    ResponseNonFinite;
                break;
            case IsolatedComponentQuarantineReason::ProcessRejected:
                mapped = ComponentActivationAudioQuarantineReasonV1::
                    WorkerProcessRejected;
                break;
            case IsolatedComponentQuarantineReason::SequenceExhausted:
                mapped = ComponentActivationAudioQuarantineReasonV1::
                    SequenceExhausted;
                break;
        }
        if (audioHost.has_value())
            audioHost->quarantine(mapped);
        audioFailureLatched.store(true, std::memory_order_release);
    }

    void cancelConnection() noexcept
    {
        if (connection == nullptr)
            return;
        bool expected = false;
        if (connectionCancelled.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            // Keep the owning reference stable until Impl destruction. A
            // delayed XPC reply may still be executing while cancellation is
            // observed; retaining the object makes every late read bounded
            // and prevents a cancel/read use-after-release race.
            xpc_connection_cancel(connection);
        }
    }

    [[nodiscard]] bool exactCurrentPeer() const noexcept
    {
        if (connection == nullptr || !peerProcessIdentity.valid)
            return false;
        const auto currentPeer = xpc_connection_get_pid(connection);
        return currentPeer == peerProcessIdentity.processIdentifier
            && activationProcessIdentity(currentPeer) == peerProcessIdentity;
    }

    [[nodiscard]] bool terminateCurrentPeer(
        bool requireParentTermination) noexcept
    {
        if (connection == nullptr)
            return false;
        const auto currentPeer = xpc_connection_get_pid(connection);
        const auto sampledIdentity = activationProcessIdentity(currentPeer);
        if ((simulateProcessIdentityMismatch && peerProcessIdentity.valid)
            || (peerProcessIdentity.valid
                && sampledIdentity != peerProcessIdentity))
        {
            return !requireParentTermination;
        }
        if (currentPeer <= 0 || !sampledIdentity.valid
            || simulateTerminationFailure)
        {
            return false;
        }

        // macOS has no pidfd-like XPC process handle. Re-sampling the exact
        // PID/start-time/euid tuple immediately before and after SIGKILL
        // bounds PID reuse as tightly as the public PID API permits.
        errno = 0;
        if (kill(currentPeer, SIGKILL) != 0)
            return false;
        for (std::size_t attempt = 0; attempt < 200; ++attempt)
        {
            if (activationProcessIdentity(currentPeer) != sampledIdentity)
                return true;
            usleep(1'000);
        }
        return activationProcessIdentity(currentPeer) != sampledIdentity;
    }

    void cleanupMaterializationPath() noexcept
    {
        if (materializationPath.front() != '\0')
        {
            static_cast<void>(unlink(materializationPath.data()));
            materializationPath.fill(0);
        }
    }

    void failClosed(ComponentActivationMacOSHostDiagnosticV1 reason) noexcept
    {
        const auto state = stateSnapshot.load(std::memory_order_relaxed);
        if (state == ComponentActivationMacOSHostStateV1::Quarantined
            || state == ComponentActivationMacOSHostStateV1::Closed)
        {
            return;
        }
        invalidateDeadline();
        checkpointPending.reset();
        checkpointInFlight = false;
        heartbeatInFlight = false;
        clearPendingActivation();
        const auto hadExactPeer = peerProcessIdentity.valid;
        const auto terminated = terminateCurrentPeer(false);
        cleanupMaterializationPath();
        revokeActivationLease();
        revokeRuntimeEvidence();
        diagnosticSnapshot.store(
            !hadExactPeer || terminated
                ? reason
                : ComponentActivationMacOSHostDiagnosticV1::
                    ProcessTerminationFailed,
            std::memory_order_relaxed);
        stateSnapshot.store(
            ComponentActivationMacOSHostStateV1::Quarantined,
            std::memory_order_release);
        cancelConnection();
        checkpointStatusSnapshot.store(ComponentActivationCheckpointStatusV1::Fatal, std::memory_order_release);
    }

    void deadlineExpired(std::uint64_t token) noexcept
    {
        if (closing.load(std::memory_order_acquire)
            || token != deadlineToken.load(std::memory_order_acquire))
        {
            return;
        }
        failClosed(ComponentActivationMacOSHostDiagnosticV1::DeadlineExpired);
    }

    void handleConnectionEvent(xpc_object_t event) noexcept
    {
        if (closing.load(std::memory_order_acquire)
            || event == nullptr || xpc_get_type(event) != XPC_TYPE_ERROR)
        {
            return;
        }
        const auto state = stateSnapshot.load(std::memory_order_relaxed);
        failClosed(
            state == ComponentActivationMacOSHostStateV1::Sealing
                ? ComponentActivationMacOSHostDiagnosticV1::LaunchRejected
                : (state == ComponentActivationMacOSHostStateV1::Loading
                    || state == ComponentActivationMacOSHostStateV1::Canary
                    || state == ComponentActivationMacOSHostStateV1::Resetting
                    || state == ComponentActivationMacOSHostStateV1::Checkpointing
                    || state
                        == ComponentActivationMacOSHostStateV1::
                            PreparedActivation
                    || state
                        == ComponentActivationMacOSHostStateV1::TearingDown)
                    ? ComponentActivationMacOSHostDiagnosticV1::
                        ActivationRejected
                : ComponentActivationMacOSHostDiagnosticV1::ProtocolRejected);
    }

    void handleCheckpointReply(xpc_object_t reply, std::uint64_t token) noexcept
    {
        using Operation = ComponentActivationCheckpointOperationV1;
        using Status = ComponentActivationCheckpointStatusV1;
        if (closing.load(std::memory_order_acquire)
            || token != deadlineToken.load(std::memory_order_acquire)
            || stateSnapshot.load(std::memory_order_relaxed) != ComponentActivationMacOSHostStateV1::Checkpointing
            || !checkpointRequest.has_value()) return;
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
        if (holdCheckpointReplyForTest && reply != nullptr)
        {
            holdCheckpointReplyForTest = false;
            heldCheckpointReplyForTest = xpc_retain(reply);
            heldCheckpointReplyTokenForTest = token;
            return;
        }
#endif
        ComponentActivationCheckpointReplyV1 decoded;
        if (!activationLease.has_value() || !activationLease->active())
        {
            failClosed(ComponentActivationMacOSHostDiagnosticV1::Revoked);
            return;
        }
        if (!audioHost.has_value() || audioHost->isQuarantined()
            || audioFailureLatched.load(std::memory_order_acquire)
            || !exactCurrentPeer()
            || decodeComponentActivationXpcCheckpointReplyV1(reply, activationAuthorization, decoded)
                != ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            || !matchesComponentActivationCheckpointReplyV1(decoded, *checkpointRequest, activationAuthorization)
            || decoded.status == Status::Fatal || decoded.status == Status::InvalidBoundary)
        {
            failClosed(ComponentActivationMacOSHostDiagnosticV1::ActivationRejected);
            return;
        }
        auto local = ComponentActivationAudioCheckpointResultV1::Committed;
        if (decoded.request.operation == Operation::Begin)
        {
            if (decoded.status == Status::Captured) checkpointId = decoded.request.checkpointId;
            else local = audioHost->commitOfflineCheckpoint(true);
        }
        else if (decoded.request.operation == Operation::Restore)
        {
            local = audioHost->restoreOfflineCheckpoint(true);
            checkpointId = 0;
        }
        else if (decoded.request.operation == Operation::Commit)
        {
            local = audioHost->commitOfflineCheckpoint(true);
            checkpointId = 0;
        }
        if (local != ComponentActivationAudioCheckpointResultV1::Committed
            && local != ComponentActivationAudioCheckpointResultV1::Restored)
        {
            failClosed(ComponentActivationMacOSHostDiagnosticV1::ActivationRejected);
            return;
        }
        if (!finishHealthControl(decoded.request.operation, decoded.status))
        {
            failClosed(ComponentActivationMacOSHostDiagnosticV1::ActivationRejected);
            return;
        }
        invalidateDeadline();
        heartbeatCounter.store(decoded.request.command.sequence, std::memory_order_release);
        checkpointRequest.reset();
        checkpointPending.reset();
        checkpointInFlight = false;
        stateSnapshot.store(ComponentActivationMacOSHostStateV1::PreparedActivation, std::memory_order_release);
        checkpointStatusSnapshot.store(decoded.status, std::memory_order_release);
        scheduleHeartbeat();
    }

    void sendCheckpointAtBoundary(std::uint64_t token) noexcept
    {
        using Operation = ComponentActivationCheckpointOperationV1;
        if (closing.load(std::memory_order_acquire)
            || token != deadlineToken.load(std::memory_order_acquire)
            || !checkpointPending.has_value()
            || stateSnapshot.load(std::memory_order_relaxed) != ComponentActivationMacOSHostStateV1::Checkpointing)
            return;
        if (!activationLease.has_value() || !activationLease->active())
        {
            failClosed(ComponentActivationMacOSHostDiagnosticV1::Revoked);
            return;
        }
        if (!audioHost.has_value() || audioHost->isQuarantined() || !exactCurrentPeer())
        {
            failClosed(ComponentActivationMacOSHostDiagnosticV1::ActivationRejected);
            return;
        }
        const auto operation = *checkpointPending;
        if (operation != Operation::Query)
        {
            const auto boundary = operation == Operation::Begin
                ? audioHost->beginOfflineCheckpoint(true)
                : audioHost->validateOfflineCheckpointBoundary(true);
            if (boundary == ComponentActivationAudioCheckpointResultV1::MissingResponse)
            {
                const auto weak = weak_from_this();
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_MSEC), controlQueue, ^{
                    if (const auto implementation = weak.lock()) implementation->sendCheckpointAtBoundary(token);
                });
                return;
            }
            if (boundary != ComponentActivationAudioCheckpointResultV1::Captured)
            {
                failClosed(ComponentActivationMacOSHostDiagnosticV1::ActivationRejected);
                return;
            }
        }
        ComponentActivationCheckpointCommandV1 request;
        request.command = makeComponentActivationCommandV1(activationAuthorization,
            ComponentActivationRecordKindV1::Checkpoint, heartbeatCounter.load(std::memory_order_relaxed) + 1U);
        request.operation = operation;
        request.completedAudioSequence = operation == Operation::Query ? 0 : audioHost->lastSubmittedSequence();
        request.checkpointId = operation == Operation::Query ? 0
            : (operation == Operation::Begin ? request.command.sequence : checkpointId);
        auto message = createComponentActivationXpcCheckpointCommandV1(request, activationAuthorization);
        if (message == nullptr)
        {
            failClosed(ComponentActivationMacOSHostDiagnosticV1::ProtocolRejected);
            return;
        }
        checkpointRequest = request;
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
        if (checkpointRequestCorruptionForTest != 0)
        {
            auto corrupt = request;
            if (checkpointRequestCorruptionForTest == 1)
            {
                --corrupt.command.sequence;
                if (corrupt.operation == Operation::Begin) corrupt.checkpointId = corrupt.command.sequence;
            }
            else corrupt.checkpointId = 1;
            checkpointRequestCorruptionForTest = 0;
            // Retain the adversarial request as the expected echo, so tests
            // prove the helper's replay/held-ID gate independently of parent matching.
            checkpointRequest = corrupt;
            xpc_dictionary_set_data(message, "talas.component-activation.checkpoint-command.v1", &corrupt, sizeof(corrupt));
        }
#endif
        const auto weak = weak_from_this();
        xpc_connection_send_message_with_reply(connection, message, controlQueue, ^(xpc_object_t reply) {
            if (const auto implementation = weak.lock()) implementation->handleCheckpointReply(reply, token);
        });
        xpc_release(message);
    }

    void startCheckpointOnControlQueue() noexcept
    {
        checkpointInFlight = true;
        stateSnapshot.store(ComponentActivationMacOSHostStateV1::Checkpointing, std::memory_order_release);
        sendCheckpointAtBoundary(armDeadline(activationDeadlineNanosecondsV1));
    }

    ComponentActivationMacOSHostOperationV1 requestCheckpointOnControlQueue(
        ComponentActivationCheckpointOperationV1 operation) noexcept
    {
        using Operation = ComponentActivationCheckpointOperationV1;
        if (!audioActive() || shutdownStarted.load(std::memory_order_acquire)
            || checkpointPending.has_value() || checkpointInFlight
            || (operation == Operation::Begin && checkpointId != 0)
            || ((operation == Operation::Restore || operation == Operation::Commit) && checkpointId == 0))
            return ComponentActivationMacOSHostOperationV1::InvalidState;
        if (!beginHealthControl())
        {
            failClosed(ComponentActivationMacOSHostDiagnosticV1::ActivationRejected);
            return ComponentActivationMacOSHostOperationV1::Rejected;
        }
        checkpointPending = operation;
        checkpointStatusSnapshot.store(ComponentActivationCheckpointStatusV1::Pending, std::memory_order_release);
        if (!heartbeatInFlight) startCheckpointOnControlQueue();
        return ComponentActivationMacOSHostOperationV1::Scheduled;
    }

    // Exclusive offline/preparation caller only. The existing heartbeat and
    // checkpoint watchdogs own the deadline; their queue must remain runnable.
    [[nodiscard]] ComponentActivationCheckpointStatusV1 checkpointSynchronously(
        ComponentActivationCheckpointOperationV1 operation) noexcept
    {
        using Status = ComponentActivationCheckpointStatusV1;
        if (dispatch_get_specific(this) == this || activationStageLease.has_value())
            return Status::InvalidBoundary;
        struct Request
        {
            Impl* owner;
            ComponentActivationCheckpointOperationV1 operation;
            ComponentActivationMacOSHostOperationV1 result = ComponentActivationMacOSHostOperationV1::InvalidState;
        } request{this, operation};
        dispatch_sync_f(controlQueue, &request, [](void* context) {
            auto& value = *static_cast<Request*>(context);
            // Preparation transfer cannot adopt a checkpoint owned by the raw
            // host API; its original owner must finish before transfer retries.
            if (value.operation == ComponentActivationCheckpointOperationV1::Query
                && value.owner->checkpointId != 0) return;
            value.result = value.owner->requestCheckpointOnControlQueue(value.operation);
        });
        if (request.result != ComponentActivationMacOSHostOperationV1::Scheduled)
            return Status::InvalidBoundary;
        for (;;)
        {
            const auto status = checkpointStatusSnapshot.load(std::memory_order_acquire);
            if (status != Status::Pending) return status;
            usleep(1'000);
        }
    }

    [[nodiscard]] bool waitForOfflineAudioCompletion() noexcept
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (audioActive())
        {
            const auto result = audioHost->validateOfflineCheckpointBoundary(activationLease->active());
            if (result == ComponentActivationAudioCheckpointResultV1::Captured) return audioActive();
            if (result != ComponentActivationAudioCheckpointResultV1::MissingResponse
                || std::chrono::steady_clock::now() >= deadline) break;
            usleep(1'000);
        }
        quarantineAudio(IsolatedComponentQuarantineReason::ProcessRejected);
        return false;
    }

    void scheduleHeartbeat() noexcept
    {
        const auto schedule = ++heartbeatScheduleGeneration;
        const auto weak = weak_from_this();
        dispatch_after(
            dispatch_time(
                DISPATCH_TIME_NOW,
                heartbeatIntervalNanosecondsV1),
            controlQueue,
            ^{
                if (const auto implementation = weak.lock(); implementation
                    && implementation->heartbeatScheduleGeneration == schedule)
                    implementation->sendHeartbeat();
            });
    }

    void handleHeartbeatReply(
        xpc_object_t reply,
        std::uint64_t token,
        ComponentActivationMacOSHostStateV1 expectedState) noexcept
    {
        if (closing.load(std::memory_order_acquire)
            || token != deadlineToken.load(std::memory_order_acquire)
            || stateSnapshot.load(std::memory_order_relaxed)
                != expectedState)
        {
            return;
        }
        if (reply == nullptr || xpc_get_type(reply) == XPC_TYPE_ERROR)
        {
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::ProtocolRejected);
            return;
        }
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
        if (holdActivationHeartbeat && pendingActivationMessage != nullptr)
        {
            holdActivationHeartbeat = false;
            heldActivationHeartbeat = xpc_retain(reply);
            heldActivationHeartbeatToken = token;
            return;
        }
#endif
        ComponentActivationTransitionV1 transition;
        const auto decoded = expectedState
                == ComponentActivationMacOSHostStateV1::Ready
            ? decodeComponentActivationXpcSealedTransitionV1(
                reply,
                ComponentActivationRecordKindV1::Heartbeat,
                session,
                transition)
            : decodeComponentActivationXpcTransitionV1(
                reply,
                ComponentActivationRecordKindV1::Heartbeat,
                activationAuthorization,
                transition);
        if (decoded != ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            || transition.sequence
                != heartbeatCounter.load(std::memory_order_relaxed) + 1U
            || !exactCurrentPeer())
        {
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::ProtocolRejected);
            return;
        }
        invalidateDeadline();
        heartbeatCounter.store(
            transition.sequence,
            std::memory_order_release);
        heartbeatInFlight = false;
        if (shutdownPending) beginShutdownOnControlQueue();
        else if (pendingActivationMessage != nullptr) sendPendingActivation();
        else if (checkpointPending.has_value()) startCheckpointOnControlQueue();
        else scheduleHeartbeat();
    }

    void sendHeartbeat() noexcept
    {
        if (heartbeatInFlight || checkpointPending.has_value() || checkpointInFlight || shutdownPending) return;
        if (closing.load(std::memory_order_acquire)
            || connection == nullptr)
        {
            return;
        }
        const auto currentState =
            stateSnapshot.load(std::memory_order_relaxed);
        if (currentState != ComponentActivationMacOSHostStateV1::Ready
            && currentState
                != ComponentActivationMacOSHostStateV1::PreparedActivation)
        {
            return;
        }
        if (currentState
                == ComponentActivationMacOSHostStateV1::PreparedActivation
            && (!activationLease.has_value() || !activationLease->active()))
        {
            failClosed(ComponentActivationMacOSHostDiagnosticV1::Revoked);
            return;
        }
        if (currentState
                == ComponentActivationMacOSHostStateV1::PreparedActivation
            && (audioFailureLatched.load(std::memory_order_acquire)
                || !audioHost.has_value() || audioHost->isQuarantined()))
        {
            // A helper may quarantine between callbacks; revocation cannot
            // depend on another process() call setting the parent's latch.
            const auto audioReason = audioHost.has_value()
                ? audioHost->quarantineReason()
                : ComponentActivationAudioQuarantineReasonV1::ExplicitControl;
            failClosed(
                audioReason
                        == ComponentActivationAudioQuarantineReasonV1::
                            ResponseMissing
                    ? ComponentActivationMacOSHostDiagnosticV1::DeadlineExpired
                    : ComponentActivationMacOSHostDiagnosticV1::
                        ActivationRejected);
            return;
        }
        auto message = currentState
                == ComponentActivationMacOSHostStateV1::Ready
            ? createComponentActivationXpcSealedSessionV1(session)
            : createComponentActivationXpcCommandV1(
                makeComponentActivationCommandV1(
                    activationAuthorization,
                    ComponentActivationRecordKindV1::Heartbeat,
                    heartbeatCounter.load(std::memory_order_relaxed) + 1U),
                ComponentActivationRecordKindV1::Heartbeat,
                activationAuthorization);
        if (message == nullptr)
        {
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::ProtocolRejected);
            return;
        }
        heartbeatInFlight = true;
        const auto token = armDeadline(heartbeatDeadlineNanosecondsV1);
        const auto weak = weak_from_this();
        xpc_connection_send_message_with_reply(
            connection,
            message,
            controlQueue,
            ^(xpc_object_t reply) {
                if (const auto implementation = weak.lock())
                    implementation->handleHeartbeatReply(
                        reply, token, currentState);
            });
        xpc_release(message);
    }

    void handleActivationReply(
        xpc_object_t reply,
        std::uint64_t token) noexcept
    {
        if (closing.load(std::memory_order_acquire)
            || token != deadlineToken.load(std::memory_order_acquire)
            || stateSnapshot.load(std::memory_order_relaxed)
                != ComponentActivationMacOSHostStateV1::Loading)
        {
            return;
        }
        cleanupMaterializationPath();
        if (!activationLease.has_value() || !activationLease->active())
        {
            failClosed(ComponentActivationMacOSHostDiagnosticV1::Revoked);
            return;
        }
        if (reply == nullptr || xpc_get_type(reply) == XPC_TYPE_ERROR)
        {
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::ActivationRejected);
            return;
        }
        ComponentActivationTransitionV1 transition;
        if (decodeComponentActivationXpcTransitionV1(
                reply,
                ComponentActivationRecordKindV1::Prepared,
                activationAuthorization,
                transition)
                != ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            || transition.sequence
                != heartbeatCounter.load(std::memory_order_relaxed) + 1U
            || !exactCurrentPeer())
        {
            ComponentActivationTransitionV1 terminal;
            static_cast<void>(decodeComponentActivationXpcTransitionV1(
                reply,
                ComponentActivationRecordKindV1::Terminal,
                activationAuthorization,
                terminal));
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::ActivationRejected);
            return;
        }
        invalidateDeadline();
        heartbeatCounter.store(transition.sequence, std::memory_order_release);
        stateSnapshot.store(
            ComponentActivationMacOSHostStateV1::Canary,
            std::memory_order_release);
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
        canaryDeliveryRetryCount = 0;
#endif

        std::array<const float*, maximumComponentActivationAudioChannelsV1>
            inputs{};
        std::array<float*, maximumComponentActivationAudioChannelsV1> outputs{};
        for (std::size_t channel = 0;
             channel < activationAuthorization.inputChannelCount;
             ++channel)
        {
            std::fill_n(
                canaryInput[channel].begin(),
                activationAuthorization.blockFrames,
                static_cast<float>(channel + 1U) * 0.25F);
            inputs[channel] = canaryInput[channel].data();
        }
        for (std::size_t channel = 0;
             channel < activationAuthorization.outputChannelCount;
             ++channel)
        {
            std::fill_n(
                canaryOutput[channel].begin(),
                activationAuthorization.blockFrames,
                0.0F);
            outputs[channel] = canaryOutput[channel].data();
        }
        ComponentActivationAudioTransportV1 transport;
        transport.generation = activationAuthorization.activationGeneration;
        transport.flags = 1;
        const auto operation = canaryAudioHost->process(
            {inputs.data(), activationAuthorization.inputChannelCount},
            {outputs.data(), activationAuthorization.outputChannelCount},
            activationAuthorization.blockFrames,
            transport,
            activationLease->active());
        if (operation
            != ComponentActivationAudioOperationV1::FirstBlockSilence)
        {
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::ActivationRejected);
            return;
        }
        const auto callbackPeriodNanoseconds =
            (static_cast<std::uint64_t>(activationAuthorization.blockFrames)
                * static_cast<std::uint64_t>(NSEC_PER_SEC))
            / activationAuthorization.sampleRate;
        const auto canaryDeliveryNanoseconds = static_cast<std::int64_t>(
            std::max<std::uint64_t>(1, callbackPeriodNanoseconds));
        const auto weak = weak_from_this();
        dispatch_after(
            dispatch_time(DISPATCH_TIME_NOW, canaryDeliveryNanoseconds),
            controlQueue,
            ^{
                if (const auto implementation = weak.lock())
                    implementation->completeCanary();
            });
    }

    void handleResetReply(xpc_object_t reply, std::uint64_t token) noexcept
    {
        if (closing.load(std::memory_order_acquire)
            || token != deadlineToken.load(std::memory_order_acquire)
            || stateSnapshot.load(std::memory_order_relaxed)
                != ComponentActivationMacOSHostStateV1::Resetting)
        {
            return;
        }
        ComponentActivationTransitionV1 transition;
        if (reply == nullptr || xpc_get_type(reply) == XPC_TYPE_ERROR
            || decodeComponentActivationXpcTransitionV1(
                reply,
                ComponentActivationRecordKindV1::Reset,
                activationAuthorization,
                transition)
                != ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            || transition.sequence
                != heartbeatCounter.load(std::memory_order_relaxed) + 1U
            || !exactCurrentPeer())
        {
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::ActivationRejected);
            return;
        }
        if (!primeAudioStreamHealth())
        {
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::ActivationRejected);
            return;
        }
        invalidateDeadline();
        heartbeatCounter.store(transition.sequence, std::memory_order_release);
        canaryAudioHost.reset();
        diagnosticSnapshot.store(
            ComponentActivationMacOSHostDiagnosticV1::Ready,
            std::memory_order_relaxed);
        stateSnapshot.store(
            ComponentActivationMacOSHostStateV1::PreparedActivation,
            std::memory_order_release);
        scheduleHeartbeat();
    }

    void completeCanary() noexcept
    {
        if (closing.load(std::memory_order_acquire)
            || stateSnapshot.load(std::memory_order_relaxed)
                != ComponentActivationMacOSHostStateV1::Canary
            || !activationLease.has_value() || !activationLease->active()
            || !canaryAudioHost.has_value())
        {
            failClosed(ComponentActivationMacOSHostDiagnosticV1::Revoked);
            return;
        }
        std::array<float*, maximumComponentActivationAudioChannelsV1> outputs{};
        for (std::size_t channel = 0;
             channel < activationAuthorization.outputChannelCount;
             ++channel)
        {
            outputs[channel] = canaryOutput[channel].data();
        }
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
        if (!canaryAudioHost->isQuarantined()
            && !canaryAudioHost->publishedResponseReady()
            && canaryDeliveryRetryCount < 2'000U)
        {
            // The product build evaluates the response after exactly one
            // callback period and fails closed. Test builds separately prove
            // the structural XPC/data-plane exchange with bounded retries.
            // Observe readiness before consuming: a missing consume latches
            // ResponseMissing and makes every later retry irrecoverable.
            ++canaryDeliveryRetryCount;
            const auto weak = weak_from_this();
            dispatch_after(
                dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_MSEC),
                controlQueue,
                ^{
                    if (const auto implementation = weak.lock())
                        implementation->completeCanary();
                });
            return;
        }
#endif
        std::uint64_t processingNanoseconds = 0;
        const auto operation = canaryAudioHost->consumeFinal(
            {outputs.data(), activationAuthorization.outputChannelCount},
            activationAuthorization.blockFrames,
            activationLease->active(),
            processingNanoseconds);
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
        const auto processingWithinCanaryBudget = processingNanoseconds != 0;
#else
        const auto callbackPeriodNanoseconds =
            (static_cast<std::uint64_t>(activationAuthorization.blockFrames)
                * static_cast<std::uint64_t>(NSEC_PER_SEC))
            / activationAuthorization.sampleRate;
        const auto processingWithinCanaryBudget = processingNanoseconds != 0
            && processingNanoseconds <= callbackPeriodNanoseconds / 2U;
#endif
        if (operation != ComponentActivationAudioOperationV1::Rendered
            || !processingWithinCanaryBudget)
        {
            failClosed(
                operation == ComponentActivationAudioOperationV1::Missing
                    ? ComponentActivationMacOSHostDiagnosticV1::DeadlineExpired
                    : ComponentActivationMacOSHostDiagnosticV1::
                        ActivationRejected);
            return;
        }
        const auto command = makeComponentActivationCommandV1(
            activationAuthorization,
            ComponentActivationRecordKindV1::Reset,
            heartbeatCounter.load(std::memory_order_relaxed) + 1U);
        auto message = createComponentActivationXpcCommandV1(
            command,
            ComponentActivationRecordKindV1::Reset,
            activationAuthorization);
        if (message == nullptr)
        {
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::ProtocolRejected);
            return;
        }
        stateSnapshot.store(
            ComponentActivationMacOSHostStateV1::Resetting,
            std::memory_order_release);
        const auto token = armDeadline(activationDeadlineNanosecondsV1);
        const auto weak = weak_from_this();
        xpc_connection_send_message_with_reply(
            connection,
            message,
            controlQueue,
            ^(xpc_object_t reply) {
                if (const auto implementation = weak.lock())
                    implementation->handleResetReply(reply, token);
            });
        xpc_release(message);
    }

    ComponentActivationMacOSHostOperationV1 activateOnControlQueue(
        ComponentActivationLeaseV1& lease,
        const ComponentActivationAuthorizationV1& authorizationSnapshot,
        xpc_object_t message,
        const std::array<char, PATH_MAX>& preparedMaterializationPath,
        const std::optional<ComponentActivationImmutableResourceGrantV1>&
            preparedResourceGrant,
        const std::optional<ComponentActivationAudioStreamGrantV1>&
            preparedAudioStreamGrant,
        const std::optional<
            ComponentActivationAudioStreamHealthSetBindingV2>&
            preparedResourceSetHealthBinding) noexcept
    {
        if (closing.load(std::memory_order_acquire)
            || stateSnapshot.load(std::memory_order_relaxed)
                != ComponentActivationMacOSHostStateV1::Ready
            || connection == nullptr || message == nullptr
            || activationLease.has_value() || pendingActivationMessage != nullptr
            || shutdownStarted.load(std::memory_order_acquire)
            || preparedMaterializationPath.front() == '\0'
            || !lease.active())
        {
            return ComponentActivationMacOSHostOperationV1::InvalidState;
        }
        auto canaryPlaneStorage = std::span<std::byte>{
            static_cast<std::byte*>(sharedStorage)
                + componentActivationAudioCanaryPlaneOffsetV1,
            componentActivationAudioPlaneBytesV1};
        auto runtimePlaneStorage = std::span<std::byte>{
            static_cast<std::byte*>(sharedStorage)
                + componentActivationAudioRuntimePlaneOffsetV1,
            componentActivationAudioPlaneBytesV1};
        auto healthPlaneStorage = std::span<std::byte>{
            static_cast<std::byte*>(sharedStorage)
                + componentActivationAudioStreamHealthPlaneOffsetV1,
            preparedResourceSetHealthBinding.has_value()
                ? componentActivationAudioStreamHealthPlaneBytesV2
                : componentActivationAudioStreamHealthPlaneBytesV1};
        if (initializeComponentActivationAudioDataPlaneV1(
                canaryPlaneStorage, authorizationSnapshot)
                != ComponentActivationAudioDiagnosticV1::Ready)
        {
            return ComponentActivationMacOSHostOperationV1::Rejected;
        }
        if (initializeComponentActivationAudioDataPlaneV1(
                runtimePlaneStorage, authorizationSnapshot)
                != ComponentActivationAudioDiagnosticV1::Ready)
        {
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::ProtocolRejected);
            return ComponentActivationMacOSHostOperationV1::Rejected;
        }
        const auto healthInitialized =
            preparedResourceSetHealthBinding.has_value()
            ? initializeComponentActivationAudioStreamHealthPlaneV2(
                healthPlaneStorage,
                authorizationSnapshot,
                *preparedResourceSetHealthBinding)
            : initializeComponentActivationAudioStreamHealthPlaneV1(
                healthPlaneStorage, authorizationSnapshot);
        if (healthInitialized != ComponentActivationAudioDiagnosticV1::Ready)
        {
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::ProtocolRejected);
            return ComponentActivationMacOSHostOperationV1::Rejected;
        }
        auto preparedCanaryAudioHost =
            ComponentActivationAudioHostV1::attach(
                canaryPlaneStorage, authorizationSnapshot);
        auto preparedRuntimeAudioHost =
            ComponentActivationAudioHostV1::attach(
                runtimePlaneStorage, authorizationSnapshot);
        std::optional<ComponentActivationAudioStreamHealthConsumerV1>
            preparedHealthConsumer;
        std::optional<ComponentActivationAudioStreamHealthConsumerV2>
            preparedSetHealthConsumer;
        if (preparedAudioStreamGrant.has_value())
        {
            preparedHealthConsumer =
                ComponentActivationAudioStreamHealthConsumerV1::attach(
                    healthPlaneStorage, authorizationSnapshot);
        }
        if (preparedResourceSetHealthBinding.has_value())
        {
            preparedSetHealthConsumer =
                ComponentActivationAudioStreamHealthConsumerV2::attach(
                    healthPlaneStorage,
                    authorizationSnapshot,
                    *preparedResourceSetHealthBinding);
        }
        if (!preparedCanaryAudioHost.has_value()
            || !preparedRuntimeAudioHost.has_value()
            || (preparedAudioStreamGrant.has_value()
                && !preparedHealthConsumer.has_value())
            || (preparedResourceSetHealthBinding.has_value()
                && !preparedSetHealthConsumer.has_value())
            || (preparedAudioStreamGrant.has_value()
                && preparedResourceSetHealthBinding.has_value()))
        {
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::ProtocolRejected);
            return ComponentActivationMacOSHostOperationV1::Rejected;
        }
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
        if (overlapActivationHeartbeat)
        {
            overlapActivationHeartbeat = false;
            sendHeartbeat();
        }
#endif
        // A sealed Ready heartbeat must finish with its original token and
        // sequence before the load message can advance the same control stream.
        if (!heartbeatInFlight) invalidateDeadline();
        if (!consumeComponentActivationAuthorization(
                lease, activationLease, activationAuthorization))
        {
            failClosed(ComponentActivationMacOSHostDiagnosticV1::Revoked);
            return ComponentActivationMacOSHostOperationV1::Rejected;
        }
        activationResourceGrant = preparedResourceGrant;
        activationAudioStreamGrant = preparedAudioStreamGrant;
        audioStreamHealthConsumer = std::move(preparedHealthConsumer);
        activationResourceSetHealthBinding =
            preparedResourceSetHealthBinding;
        audioStreamSetHealthConsumer =
            std::move(preparedSetHealthConsumer);
        audioStreamHealthCache.reset();
        audioStreamSetHealthCache = {};
        canaryAudioHost.emplace(std::move(*preparedCanaryAudioHost));
        audioHost.emplace(std::move(*preparedRuntimeAudioHost));
        audioFailureLatched.store(false, std::memory_order_relaxed);
        materializationPath = preparedMaterializationPath;
        pendingActivationMessage = xpc_retain(message);
        if (!heartbeatInFlight) sendPendingActivation();
        return ComponentActivationMacOSHostOperationV1::Scheduled;
    }

    void clearPendingActivation() noexcept
    {
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
        if (heldCheckpointReplyForTest != nullptr)
        {
            xpc_release(heldCheckpointReplyForTest);
            heldCheckpointReplyForTest = nullptr;
        }
        if (heldActivationHeartbeat != nullptr)
        {
            xpc_release(heldActivationHeartbeat);
            heldActivationHeartbeat = nullptr;
        }
#endif
        if (pendingActivationMessage != nullptr)
        {
            xpc_release(pendingActivationMessage);
            pendingActivationMessage = nullptr;
        }
    }

    void sendPendingActivation() noexcept
    {
        if (pendingActivationMessage == nullptr) return;
        if (shutdownStarted.load(std::memory_order_acquire)
            || closing.load(std::memory_order_acquire))
        {
            finishClosed();
            return;
        }
        if (!activationLease.has_value() || !activationLease->active())
        {
            failClosed(ComponentActivationMacOSHostDiagnosticV1::Revoked);
            return;
        }
        if (!exactCurrentPeer())
        {
            failClosed(ComponentActivationMacOSHostDiagnosticV1::ActivationRejected);
            return;
        }
        stateSnapshot.store(
            ComponentActivationMacOSHostStateV1::Loading,
            std::memory_order_release);
        const auto token = armDeadline(activationDeadlineNanosecondsV1);
        const auto weak = weak_from_this();
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
        ++activationMessagesSent;
#endif
        sendFrozenComponentEntryBytes(
            connection,
            pendingActivationMessage,
            controlQueue,
            ^(xpc_object_t reply) {
                if (const auto implementation = weak.lock())
                    implementation->handleActivationReply(reply, token);
            });
        clearPendingActivation();
    }

    void handlePrewarmReply(
        xpc_object_t reply,
        std::uint64_t token) noexcept
    {
        if (closing.load(std::memory_order_acquire)
            || token != deadlineToken.load(std::memory_order_acquire)
            || stateSnapshot.load(std::memory_order_relaxed)
                != ComponentActivationMacOSHostStateV1::Sealing)
        {
            return;
        }
        if (reply == nullptr || xpc_get_type(reply) == XPC_TYPE_ERROR)
        {
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::LaunchRejected);
            return;
        }
        ComponentActivationTransitionV1 transition;
        if (decodeComponentActivationXpcSealedTransitionV1(
                reply,
                ComponentActivationRecordKindV1::SealedReady,
                session,
                transition)
                != ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            || transition.sequence != 1)
        {
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::ProtocolRejected);
            return;
        }
        const auto peer = xpc_connection_get_pid(connection);
        const auto exactPeerIdentity = activationProcessIdentity(peer);
        if (peer <= 0 || !exactPeerIdentity.valid)
        {
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::LaunchRejected);
            return;
        }
        auto authored = ComponentActivationRuntimeEvidenceSourceV1::author(
            session);
        if (!authored.has_value())
        {
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::
                    RuntimeEvidenceUnavailable);
            return;
        }
        invalidateDeadline();
        runtimeSession.emplace(std::move(authored->session));
        {
            const std::scoped_lock lock(evidenceMutex);
            evidence = authored->evidence;
        }
        peerProcessIdentity = exactPeerIdentity;
        peerProcessIdentifier.store(peer, std::memory_order_relaxed);
        heartbeatCounter.store(transition.sequence, std::memory_order_relaxed);
        diagnosticSnapshot.store(
            ComponentActivationMacOSHostDiagnosticV1::Ready,
            std::memory_order_relaxed);
        stateSnapshot.store(
            ComponentActivationMacOSHostStateV1::Ready,
            std::memory_order_release);
        scheduleHeartbeat();
    }

    void startOnControlQueue() noexcept
    {
        if (closing.load(std::memory_order_acquire))
            return;
        connection = xpc_connection_create(serviceIdentity, controlQueue);
        if (connection == nullptr)
        {
            failClosed(ComponentActivationMacOSHostDiagnosticV1::LaunchRejected);
            return;
        }
        const auto weak = weak_from_this();
        xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
            if (const auto implementation = weak.lock())
                implementation->handleConnectionEvent(event);
        });
        xpc_connection_activate(connection);

        auto message = createComponentActivationXpcSealedPrewarmV1(
            session, sharedMemory);
        if (message == nullptr)
        {
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::ProtocolRejected);
            return;
        }
        const auto token = armDeadline(prewarmDeadlineNanosecondsV1);
        xpc_connection_send_message_with_reply(
            connection,
            message,
            controlQueue,
            ^(xpc_object_t reply) {
                if (const auto implementation = weak.lock())
                    implementation->handlePrewarmReply(reply, token);
            });
        xpc_release(message);
    }

    void finishClosed() noexcept
    {
        invalidateDeadline();
        checkpointPending.reset();
        checkpointInFlight = false;
        heartbeatInFlight = false;
        clearPendingActivation();
        cleanupMaterializationPath();
        revokeActivationLease();
        revokeRuntimeEvidence();
        const auto terminated = connection == nullptr
                && !peerProcessIdentity.valid
            ? true
            : terminateCurrentPeer(peerProcessIdentity.valid);
        cancelConnection();
        diagnosticSnapshot.store(
            terminated
                ? ComponentActivationMacOSHostDiagnosticV1::Closed
                : ComponentActivationMacOSHostDiagnosticV1::
                    ProcessTerminationFailed,
            std::memory_order_relaxed);
        stateSnapshot.store(
            terminated
                ? ComponentActivationMacOSHostStateV1::Closed
                : ComponentActivationMacOSHostStateV1::Quarantined,
            std::memory_order_release);
        closing.store(true, std::memory_order_release);
        checkpointStatusSnapshot.store(ComponentActivationCheckpointStatusV1::Fatal, std::memory_order_release);
    }

    void handleTeardownReply(
        xpc_object_t reply,
        std::uint64_t token) noexcept
    {
        if (closing.load(std::memory_order_acquire)
            || token != deadlineToken.load(std::memory_order_acquire)
            || stateSnapshot.load(std::memory_order_relaxed)
                != ComponentActivationMacOSHostStateV1::TearingDown)
        {
            return;
        }
        ComponentActivationTransitionV1 transition;
        if (reply == nullptr || xpc_get_type(reply) == XPC_TYPE_ERROR
            || decodeComponentActivationXpcTransitionV1(
                reply,
                ComponentActivationRecordKindV1::Teardown,
                activationAuthorization,
                transition)
                != ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            || transition.sequence
                != heartbeatCounter.load(std::memory_order_relaxed) + 1U
            || !exactCurrentPeer())
        {
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::ActivationRejected);
            return;
        }
        heartbeatCounter.store(transition.sequence, std::memory_order_release);
        finishClosed();
    }

    void beginShutdownOnControlQueue() noexcept
    {
        if (heartbeatInFlight)
        {
            shutdownPending = true;
            checkpointPending.reset();
            return;
        }
        shutdownPending = false;
        const auto current = stateSnapshot.load(std::memory_order_relaxed);
        if (current != ComponentActivationMacOSHostStateV1::PreparedActivation
            || connection == nullptr || !activationLease.has_value())
        {
            finishClosed();
            return;
        }
        invalidateDeadline();
        revokeActivationLease();
        revokeRuntimeEvidence();
        const auto command = makeComponentActivationCommandV1(
            activationAuthorization,
            ComponentActivationRecordKindV1::Teardown,
            heartbeatCounter.load(std::memory_order_relaxed) + 1U);
        auto message = createComponentActivationXpcCommandV1(
            command,
            ComponentActivationRecordKindV1::Teardown,
            activationAuthorization);
        if (message == nullptr)
        {
            failClosed(
                ComponentActivationMacOSHostDiagnosticV1::ProtocolRejected);
            return;
        }
        stateSnapshot.store(
            ComponentActivationMacOSHostStateV1::TearingDown,
            std::memory_order_release);
        const auto token = armDeadline(activationDeadlineNanosecondsV1);
        const auto weak = weak_from_this();
        xpc_connection_send_message_with_reply(
            connection,
            message,
            controlQueue,
            ^(xpc_object_t reply) {
                if (const auto implementation = weak.lock())
                    implementation->handleTeardownReply(reply, token);
            });
        xpc_release(message);
    }

    ComponentActivationMacOSEndpointV1 endpoint =
        ComponentActivationMacOSEndpointV1::Production;
    const char* serviceIdentity = nullptr;
    ComponentActivationSealedSessionV1 session;
    void* sharedStorage = nullptr;
    std::size_t sharedStorageBytes = 0;
    xpc_object_t sharedMemory = nullptr;
    dispatch_queue_t controlQueue = nullptr;
    xpc_connection_t connection = nullptr;
    std::atomic<bool> connectionCancelled{false};
    std::optional<ComponentActivationRuntimeSessionV1> runtimeSession;
    std::optional<ComponentActivationLeaseV1> activationLease;
    std::optional<ComponentActivationStageLeaseV1> activationStageLease;
    std::optional<ComponentActivationAudioHostV1> canaryAudioHost;
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
    std::uint32_t canaryDeliveryRetryCount = 0;
#endif
    std::optional<ComponentActivationAudioHostV1> audioHost;
    std::array<
        std::array<float, maximumComponentActivationAudioFramesV1>,
        maximumComponentActivationAudioChannelsV1> canaryInput{};
    std::array<
        std::array<float, maximumComponentActivationAudioFramesV1>,
        maximumComponentActivationAudioChannelsV1> canaryOutput{};
    ComponentActivationAuthorizationV1 activationAuthorization;
    std::optional<ComponentActivationImmutableResourceGrantV1>
        activationResourceGrant;
    std::optional<ComponentActivationAudioStreamGrantV1>
        activationAudioStreamGrant;
    std::optional<ComponentActivationAudioStreamHealthSetBindingV2>
        activationResourceSetHealthBinding;
    std::optional<ComponentActivationAudioStreamHealthConsumerV1>
        audioStreamHealthConsumer;
    std::optional<ComponentActivationAudioStreamHealthConsumerV2>
        audioStreamSetHealthConsumer;
    std::optional<IsolatedComponentAudioStreamHealthSnapshot>
        audioStreamHealthCache;
    std::array<
        std::optional<IsolatedComponentAudioStreamHealthSnapshot>,
        maximumComponentActivationResourceSetAudioStreamsV1>
        audioStreamSetHealthCache{};
    std::array<char, PATH_MAX> materializationPath{};
    mutable std::mutex evidenceMutex;
    ComponentActivationRuntimeEvidenceV1 evidence;
    std::atomic<ComponentActivationMacOSHostStateV1> stateSnapshot{
        ComponentActivationMacOSHostStateV1::Prepared};
    std::atomic<ComponentActivationMacOSHostDiagnosticV1> diagnosticSnapshot{
        ComponentActivationMacOSHostDiagnosticV1::Ready};
    std::atomic<std::uint64_t> heartbeatCounter{0};
    std::atomic<std::int32_t> peerProcessIdentifier{0};
    ActivationProcessIdentityV1 peerProcessIdentity;
    bool simulateTerminationFailure = false;
    bool simulateProcessIdentityMismatch = false;
    std::atomic<bool> closing{false};
    std::atomic<bool> shutdownStarted{false};
    std::atomic<bool> audioFailureLatched{false};
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
    std::uint32_t checkpointRequestCorruptionForTest = 0;
    std::atomic<int> heldSetHealthReadForTest{0};
    bool holdCheckpointReplyForTest = false;
    xpc_object_t heldCheckpointReplyForTest = nullptr;
    std::uint64_t heldCheckpointReplyTokenForTest = 0;
#endif
    // Control/inspection only; never acquired by an audio callback, held while
    // waiting on XPC, or held during stage-gate authority revocation.
    std::mutex healthInspectionMutex;
    std::optional<IsolatedComponentAudioStreamHealthSetSnapshot> completeSetHealthCache;
    std::optional<IsolatedComponentAudioStreamHealthSetSnapshot> publishedHealthSnapshot;
    bool checkpointHealthPending = false;
    bool checkpointHealthHeld = false;
    std::uint64_t heartbeatScheduleGeneration = 0;
    xpc_object_t pendingActivationMessage = nullptr;
    bool heartbeatInFlight = false;
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
    bool overlapActivationHeartbeat = false;
    bool holdActivationHeartbeat = false;
    xpc_object_t heldActivationHeartbeat = nullptr;
    std::uint64_t heldActivationHeartbeatToken = 0;
    std::uint64_t activationMessagesSent = 0;
#endif
    bool checkpointInFlight = false;
    bool shutdownPending = false;
    std::uint64_t checkpointId = 0;
    std::optional<ComponentActivationCheckpointOperationV1> checkpointPending;
    std::optional<ComponentActivationCheckpointCommandV1> checkpointRequest;
    std::atomic<ComponentActivationCheckpointStatusV1> checkpointStatusSnapshot{
        ComponentActivationCheckpointStatusV1::Fatal};
    std::atomic<std::uint64_t> deadlineToken{1};
};

struct ComponentActivationMacOSHost::PreparedBinding final
    : PreparedIsolatedComponentBinding
{
    PreparedBinding(
        std::shared_ptr<Impl> implementation,
        IsolatedComponentRuntimeContract contract,
        bool checkpointCapable) noexcept
        : implementation_(std::move(implementation))
        , contract_(std::move(contract))
        , checkpointCapable_(checkpointCapable)
    {
    }

    ~PreparedBinding() override
    {
        if (implementation_ != nullptr)
        {
            ComponentActivationMacOSHost owner{
                std::move(implementation_)};
            owner.shutdown();
        }
    }

    [[nodiscard]] const IsolatedComponentRuntimeContract& contract()
        const noexcept override
    {
        return contract_;
    }

    [[nodiscard]] bool active() const noexcept override
    {
        return implementation_ != nullptr
            && implementation_->audioActive();
    }

    [[nodiscard]] bool supportsOfflineCheckpoint() const noexcept override
    {
        return checkpointCapable_;
    }

    class Checkpoint final : public IsolatedComponentOfflineCheckpoint
    {
    public:
        ~Checkpoint() override
        {
            if (owner_) static_cast<void>(owner_->finishOfflineCheckpoint(ComponentActivationCheckpointOperationV1::Restore));
        }
        void arm(PreparedBinding& owner) noexcept { owner_ = &owner; }
        bool commit() noexcept override
        {
            if (!owner_) return committed_;
            committed_ = owner_->finishOfflineCheckpoint(ComponentActivationCheckpointOperationV1::Commit);
            owner_ = nullptr;
            return committed_;
        }
    private:
        PreparedBinding* owner_ = nullptr;
        bool committed_ = false;
    };

    [[nodiscard]] std::unique_ptr<IsolatedComponentOfflineCheckpoint> beginOfflineCheckpoint() override
    {
        if (!checkpointCapable_ || !active() || offlineCheckpointActive_
            || implementation_->activationStageLease.has_value()) return {};
        // Own all caller-side storage before the helper can capture anything.
        auto checkpoint = std::make_unique<Checkpoint>();
        const auto status = implementation_->checkpointSynchronously(ComponentActivationCheckpointOperationV1::Begin);
        if (status != ComponentActivationCheckpointStatusV1::Captured)
        {
            if (status != ComponentActivationCheckpointStatusV1::CaptureFailed)
                implementation_->quarantineAudio(IsolatedComponentQuarantineReason::ProcessRejected);
            return {};
        }
        offlineCheckpointActive_ = true;
        checkpoint->arm(*this);
        return checkpoint;
    }

    [[nodiscard]] bool finishOfflineCheckpoint(ComponentActivationCheckpointOperationV1 operation) noexcept
    {
        const auto status = implementation_->checkpointSynchronously(operation);
        offlineCheckpointActive_ = false;
        const auto expected = operation == ComponentActivationCheckpointOperationV1::Restore
            ? ComponentActivationCheckpointStatusV1::Restored : ComponentActivationCheckpointStatusV1::Committed;
        if (status == expected) return true;
        implementation_->quarantineAudio(IsolatedComponentQuarantineReason::ProcessRejected);
        return false;
    }

    [[nodiscard]] std::optional<
        IsolatedComponentAudioStreamHealthSetSnapshot> audioStreamHealthSet()
        noexcept override
    {
        return implementation_ != nullptr
            ? implementation_->audioStreamHealthSet()
            : std::nullopt;
    }

#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
    [[nodiscard]] bool publishedAudioResponseReadyForTesting()
        const noexcept override
    {
        return implementation_ != nullptr
            && implementation_->publishedAudioResponseReadyForTesting();
    }
#endif

    [[nodiscard]] bool tryBeginStage() noexcept override
    {
        return implementation_ != nullptr
            && implementation_->tryBeginStage();
    }

    void endStage() noexcept override
    {
        if (implementation_ != nullptr)
            implementation_->endStage();
    }

    [[nodiscard]] IsolatedComponentProcessResult process(
        std::span<const float* const> inputChannels,
        std::span<float* const> outputChannels,
        std::uint32_t frameCount,
        IsolatedComponentTransport transport) noexcept override
    {
        if (implementation_ == nullptr)
            return IsolatedComponentProcessResult::Quarantined;
        ComponentActivationAudioTransportV1 audioTransport;
        audioTransport.generation = transport.generation;
        audioTransport.sourceFrame = transport.sourceFrame;
        audioTransport.flags = transport.reset ? 1U : 0U;
        const auto operation = implementation_->processAudio(
            inputChannels,
            outputChannels,
            frameCount,
            audioTransport);
        if (offlineCheckpointActive_
            && (operation == ComponentActivationAudioOperationV1::FirstBlockSilence
                || operation == ComponentActivationAudioOperationV1::Rendered)
            && !implementation_->waitForOfflineAudioCompletion())
        {
            for (auto* output : outputChannels)
                if (output) std::fill_n(output, frameCount, 0.0F);
            return IsolatedComponentProcessResult::Quarantined;
        }
        switch (operation)
        {
            case ComponentActivationAudioOperationV1::FirstBlockSilence:
                return IsolatedComponentProcessResult::FirstBlockSilence;
            case ComponentActivationAudioOperationV1::Rendered:
                return IsolatedComponentProcessResult::Rendered;
            case ComponentActivationAudioOperationV1::Acquired:
            case ComponentActivationAudioOperationV1::Published:
            case ComponentActivationAudioOperationV1::Missing:
            case ComponentActivationAudioOperationV1::InvalidArgument:
            case ComponentActivationAudioOperationV1::NonFinite:
            case ComponentActivationAudioOperationV1::SequenceExhausted:
            case ComponentActivationAudioOperationV1::Quarantined:
                return IsolatedComponentProcessResult::Quarantined;
        }
        return IsolatedComponentProcessResult::Quarantined;
    }

    void quarantine(IsolatedComponentQuarantineReason reason) noexcept override
    {
        if (implementation_ != nullptr)
            implementation_->quarantineAudio(reason);
    }

private:
    std::shared_ptr<Impl> implementation_;
    IsolatedComponentRuntimeContract contract_;
    const bool checkpointCapable_;
    // Only the exclusive offline caller accesses checkpoint ownership/process.
    bool offlineCheckpointActive_ = false;
};

bool ComponentActivationMacOSHostPreparationV1::ok() const noexcept
{
    return host != nullptr
        && diagnostic == ComponentActivationMacOSHostDiagnosticV1::Ready;
}

ComponentActivationMacOSHost::ComponentActivationMacOSHost(
    std::shared_ptr<Impl> implementation) noexcept
    : impl_(std::move(implementation))
{
}

ComponentActivationMacOSHost::~ComponentActivationMacOSHost()
{
    shutdown();
}

ComponentActivationMacOSHostPreparationV1
ComponentActivationMacOSHost::prepare(
    ComponentActivationMacOSEndpointV1 endpoint,
    ComponentActivationMacOSProfileV1 profile) noexcept
{
    ComponentActivationMacOSHostPreparationV1 result;
    const auto* identity = serviceIdentity(endpoint);
    if (identity == nullptr)
        return result;
    if (profile != ComponentActivationMacOSProfileV1::StatelessAudio
        && profile
            != ComponentActivationMacOSProfileV1::
                ImmutableResource
        && profile
            != ComponentActivationMacOSProfileV1::BoundedAudioStream
        && profile
            != ComponentActivationMacOSProfileV1::MultipleImmutableResources
        && profile
            != ComponentActivationMacOSProfileV1::
                MultipleImmutableOnlyResources)
    {
        return result;
    }
    const auto session = makeSealedSession(profile);
    if (validateComponentActivationSealedSessionV1(session)
        != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        return result;
    }
    const auto storageBytes = static_cast<std::size_t>(
        session.sharedStorageBytes);
    auto* storage = mmap(
        nullptr,
        storageBytes,
        PROT_READ | PROT_WRITE,
        MAP_ANON | MAP_SHARED,
        -1,
        0);
    if (storage == MAP_FAILED)
    {
        result.diagnostic = ComponentActivationMacOSHostDiagnosticV1::
            SharedStorageUnavailable;
        return result;
    }
    std::memcpy(storage, &session, sizeof(session));
    auto sharedMemory = xpc_shmem_create(storage, storageBytes);
    if (sharedMemory == nullptr
        || !sealRuntimeBeforeComponentBytes(
            session, storage, storageBytes))
    {
        if (sharedMemory != nullptr)
            xpc_release(sharedMemory);
        static_cast<void>(munmap(storage, storageBytes));
        result.diagnostic = ComponentActivationMacOSHostDiagnosticV1::
            SharedStorageUnavailable;
        return result;
    }
    try
    {
        auto implementation = std::make_shared<Impl>(
            endpoint,
            identity,
            session,
            storage,
            storageBytes,
            sharedMemory);
        storage = nullptr;
        sharedMemory = nullptr;
        implementation->controlQueue = dispatch_queue_create(
            controlQueueIdentityV1,
            DISPATCH_QUEUE_SERIAL);
        if (implementation->controlQueue == nullptr)
        {
            result.diagnostic = ComponentActivationMacOSHostDiagnosticV1::
                ControlQueueUnavailable;
            return result;
        }
        dispatch_queue_set_specific(implementation->controlQueue, implementation.get(), implementation.get(), nullptr);
        result.host = std::unique_ptr<ComponentActivationMacOSHost>(
            new ComponentActivationMacOSHost(std::move(implementation)));
        result.diagnostic = ComponentActivationMacOSHostDiagnosticV1::Ready;
    }
    catch (const std::bad_alloc&)
    {
        if (sharedMemory != nullptr)
            xpc_release(sharedMemory);
        if (storage != nullptr)
            static_cast<void>(munmap(storage, storageBytes));
        result.diagnostic = ComponentActivationMacOSHostDiagnosticV1::
            SharedStorageUnavailable;
    }
    return result;
}

ComponentActivationMacOSHostOperationV1
ComponentActivationMacOSHost::start() noexcept
{
    auto expected = ComponentActivationMacOSHostStateV1::Prepared;
    if (!impl_
        || !impl_->stateSnapshot.compare_exchange_strong(
            expected,
            ComponentActivationMacOSHostStateV1::Sealing,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        return ComponentActivationMacOSHostOperationV1::InvalidState;
    }
    const auto weak = std::weak_ptr<Impl>(impl_);
    dispatch_async(impl_->controlQueue, ^{
        if (const auto implementation = weak.lock())
            implementation->startOnControlQueue();
    });
    return ComponentActivationMacOSHostOperationV1::Scheduled;
}

ComponentActivationMacOSHostOperationV1
ComponentActivationMacOSHost::activate(
    ComponentActivationLeaseV1 lease,
    const component_bundle::ComponentBundleSnapshot& snapshot,
    std::span<const ComponentActivationStaticParameterV1> staticParameters)
    noexcept
{
    if (!impl_)
        return ComponentActivationMacOSHostOperationV1::Rejected;
    const auto* liveRecord = lease.record();
    if (liveRecord == nullptr)
        return ComponentActivationMacOSHostOperationV1::Rejected;
    // The authority record is immutable for the lifetime of the lease state.
    // Snapshot it once; every later live decision is made by the final lease
    // consume/revalidation, never by dereferencing a newly nullable record.
    const auto authorizationSnapshot = *liveRecord;
    if (!lease.active()
        || !component_bundle::validateComponentBundleSnapshotIdentity(snapshot)
        || snapshot.contentDigest != authorizationSnapshot.contentDigest
        || snapshot.payloads.size() != 1
        || !snapshot.payloads.front().isEntry)
    {
        return ComponentActivationMacOSHostOperationV1::Rejected;
    }
    const auto& entry = snapshot.payloads.front();
    const auto manifest = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(
            snapshot.canonicalManifest.data()),
        snapshot.canonicalManifest.size()};
    std::optional<ComponentActivationImmutableResourceGrantV1>
        preparedResourceGrant;
    if (const auto* resourceGrant = lease.resourceGrant();
        resourceGrant != nullptr)
    {
        preparedResourceGrant = *resourceGrant;
    }
    std::optional<ComponentActivationAudioStreamGrantV1>
        preparedAudioStreamGrant;
    if (const auto* audioStreamGrant = lease.audioStreamGrant();
        audioStreamGrant != nullptr)
    {
        preparedAudioStreamGrant = *audioStreamGrant;
    }
    const auto resourceSetMembers = lease.resourceSetMembers();
    std::optional<ComponentActivationAudioStreamHealthSetBindingV2>
        preparedResourceSetHealthBinding;
    if (const auto* resourceSetGrant = lease.resourceSetGrant();
        resourceSetGrant != nullptr && resourceSetGrant->audioStreamCount != 0U)
    {
        preparedResourceSetHealthBinding =
            componentActivationAudioStreamHealthSetBindingV2(
                authorizationSnapshot,
                impl_->session,
                *resourceSetGrant,
                resourceSetMembers);
        if (!preparedResourceSetHealthBinding.has_value())
            return ComponentActivationMacOSHostOperationV1::Rejected;
    }
    PendingMaterializationFile materialization;
    if (!createMaterializationFile(materialization))
        return ComponentActivationMacOSHostOperationV1::Rejected;
    auto materializationFile = xpc_fd_create(materialization.descriptor);
    materialization.closeDescriptor();
    if (materializationFile == nullptr)
        return ComponentActivationMacOSHostOperationV1::Rejected;
    const auto* audioStreamGrant = lease.audioStreamGrant();
    xpc_object_t audioSourceSharedMemory = nullptr;
    if (audioStreamGrant != nullptr)
    {
        audioSourceSharedMemory =
            createComponentActivationXpcAudioSourceSharedMemoryV1(
                *audioStreamGrant,
                lease.audioSourceBytes());
        if (audioSourceSharedMemory == nullptr)
        {
            xpc_release(materializationFile);
            return ComponentActivationMacOSHostOperationV1::Rejected;
        }
    }
    std::array<
        ComponentActivationXpcResourceSetSourceV1,
        maximumComponentActivationResourceSetMembersV1>
        resourceSetSources{};
    std::array<
        xpc_object_t,
        maximumComponentActivationResourceSetMembersV1>
        resourceSetAudioMemory{};
    bool resourceSetReady = lease.resourceSetGrant() == nullptr;
    if (lease.resourceSetGrant() != nullptr
        && !resourceSetMembers.empty()
        && resourceSetMembers.size()
            <= maximumComponentActivationResourceSetMembersV1)
    {
        resourceSetReady = true;
        for (std::size_t index = 0;
             index < resourceSetMembers.size();
             ++index)
        {
            auto& source = resourceSetSources[index];
            if (const auto* immutableGrant =
                    lease.resourceSetImmutableGrant(index);
                immutableGrant != nullptr)
            {
                source.immutableGrant = immutableGrant;
                source.immutableBytes =
                    lease.resourceSetImmutableBytes(index);
                if (source.immutableBytes.empty())
                {
                    resourceSetReady = false;
                    break;
                }
                continue;
            }
            const auto* setAudioGrant =
                lease.resourceSetAudioGrant(index);
            if (setAudioGrant == nullptr)
            {
                resourceSetReady = false;
                break;
            }
            auto memory =
                createComponentActivationXpcAudioSourceSharedMemoryV1(
                    *setAudioGrant,
                    lease.resourceSetAudioBytes(index));
            if (memory == nullptr)
            {
                resourceSetReady = false;
                break;
            }
            resourceSetAudioMemory[index] = memory;
            source.audioGrant = setAudioGrant;
            source.audioSourceSharedMemory = memory;
        }
    }
    if (!resourceSetReady)
    {
        for (auto memory : resourceSetAudioMemory)
        {
            if (memory != nullptr)
                xpc_release(memory);
        }
        if (audioSourceSharedMemory != nullptr)
            xpc_release(audioSourceSharedMemory);
        xpc_release(materializationFile);
        return ComponentActivationMacOSHostOperationV1::Rejected;
    }
    auto message = createComponentActivationXpcLoadPayloadV1(
        authorizationSnapshot,
        impl_->session,
        manifest,
        entry.bytes,
        staticParameters,
        materializationFile,
        lease.resourceGrant(),
        lease.resourceBytes(),
        audioStreamGrant,
        audioSourceSharedMemory,
        lease.resourceSetGrant(),
        resourceSetMembers,
        std::span<const ComponentActivationXpcResourceSetSourceV1>{
            resourceSetSources.data(), resourceSetMembers.size()});
    for (auto memory : resourceSetAudioMemory)
    {
        if (memory != nullptr)
            xpc_release(memory);
    }
    if (audioSourceSharedMemory != nullptr)
        xpc_release(audioSourceSharedMemory);
    xpc_release(materializationFile);
    if (message == nullptr)
        return ComponentActivationMacOSHostOperationV1::Rejected;
    auto operation = ComponentActivationMacOSHostOperationV1::InvalidState;
    auto* leasePointer = &lease;
    const auto* authorizationSnapshotPointer = &authorizationSnapshot;
    auto* operationPointer = &operation;
    const auto* materializationPathPointer = &materialization.path;
    const auto* resourceGrantPointer = &preparedResourceGrant;
    const auto* audioStreamGrantPointer = &preparedAudioStreamGrant;
    const auto* resourceSetHealthBindingPointer =
        &preparedResourceSetHealthBinding;
    dispatch_sync(impl_->controlQueue, ^{
        *operationPointer = impl_->activateOnControlQueue(
            *leasePointer,
            *authorizationSnapshotPointer,
            message,
            *materializationPathPointer,
            *resourceGrantPointer,
            *audioStreamGrantPointer,
            *resourceSetHealthBindingPointer);
    });
    xpc_release(message);
    if (operation == ComponentActivationMacOSHostOperationV1::Scheduled)
        materialization.releasePath();
    return operation;
}

ComponentActivationMacOSHostOperationV1 ComponentActivationMacOSHost::requestCheckpoint(
    ComponentActivationCheckpointOperationV1 operation) noexcept
{
    if (!impl_) return ComponentActivationMacOSHostOperationV1::InvalidState;
    auto result = ComponentActivationMacOSHostOperationV1::InvalidState;
    auto* resultPointer = &result;
    dispatch_sync(impl_->controlQueue, ^{
        *resultPointer = impl_->requestCheckpointOnControlQueue(operation);
    });
    return result;
}

ComponentActivationMacOSHostOperationV1 ComponentActivationMacOSHost::queryOfflineCheckpoint() noexcept
{
    return requestCheckpoint(ComponentActivationCheckpointOperationV1::Query);
}
ComponentActivationMacOSHostOperationV1 ComponentActivationMacOSHost::beginOfflineCheckpoint() noexcept
{
    return requestCheckpoint(ComponentActivationCheckpointOperationV1::Begin);
}
ComponentActivationMacOSHostOperationV1 ComponentActivationMacOSHost::restoreOfflineCheckpoint() noexcept
{
    return requestCheckpoint(ComponentActivationCheckpointOperationV1::Restore);
}
ComponentActivationMacOSHostOperationV1 ComponentActivationMacOSHost::commitOfflineCheckpoint() noexcept
{
    return requestCheckpoint(ComponentActivationCheckpointOperationV1::Commit);
}
ComponentActivationCheckpointStatusV1 ComponentActivationMacOSHost::checkpointStatus() const noexcept
{
    return impl_ ? impl_->checkpointStatusSnapshot.load(std::memory_order_acquire)
        : ComponentActivationCheckpointStatusV1::Fatal;
}

#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
void ComponentActivationMacOSHost::holdNextCheckpointReplyForTesting() noexcept
{
    if (impl_) dispatch_sync(impl_->controlQueue, ^{ impl_->holdCheckpointReplyForTest = true; });
}

bool ComponentActivationMacOSHost::checkpointReplyHeldForTesting() const noexcept
{
    if (!impl_) return false;
    __block bool held = false;
    dispatch_sync(impl_->controlQueue, ^{ held = impl_->heldCheckpointReplyForTest != nullptr; });
    return held;
}

void ComponentActivationMacOSHost::releaseCheckpointReplyForTesting() noexcept
{
    if (!impl_) return;
    dispatch_sync(impl_->controlQueue, ^{
        auto reply = impl_->heldCheckpointReplyForTest;
        if (reply == nullptr) return;
        impl_->heldCheckpointReplyForTest = nullptr;
        impl_->handleCheckpointReply(reply, impl_->heldCheckpointReplyTokenForTest);
        xpc_release(reply);
    });
}

void ComponentActivationMacOSHost::holdNextResourceSetHealthReadForTesting() noexcept
{
    if (impl_) impl_->heldSetHealthReadForTest.store(1);
}

bool ComponentActivationMacOSHost::resourceSetHealthReadHeldForTesting() const noexcept
{
    return impl_ && impl_->heldSetHealthReadForTest.load() == 2;
}

void ComponentActivationMacOSHost::releaseResourceSetHealthReadForTesting() noexcept
{
    if (impl_) impl_->heldSetHealthReadForTest.store(0);
}

std::optional<IsolatedComponentAudioStreamHealthSetSnapshot>
ComponentActivationMacOSHost::audioStreamHealthSetForTesting() noexcept
{
    return impl_ ? impl_->audioStreamHealthSet() : std::nullopt;
}

void ComponentActivationMacOSHost::overlapNextActivationHeartbeatForTesting(bool holdReply) noexcept
{
    if (!impl_) return;
    dispatch_sync(impl_->controlQueue, ^{
        impl_->overlapActivationHeartbeat = true;
        impl_->holdActivationHeartbeat = holdReply;
    });
}

bool ComponentActivationMacOSHost::activationHeartbeatHeldForTesting() const noexcept
{
    if (!impl_) return false;
    __block bool held = false;
    dispatch_sync(impl_->controlQueue, ^{ held = impl_->heldActivationHeartbeat != nullptr; });
    return held;
}

void ComponentActivationMacOSHost::releaseActivationHeartbeatForTesting() noexcept
{
    if (!impl_) return;
    dispatch_sync(impl_->controlQueue, ^{
        auto reply = impl_->heldActivationHeartbeat;
        if (reply == nullptr) return;
        impl_->heldActivationHeartbeat = nullptr;
        impl_->handleHeartbeatReply(reply, impl_->heldActivationHeartbeatToken,
            ComponentActivationMacOSHostStateV1::Ready);
        xpc_release(reply);
    });
}

std::uint64_t ComponentActivationMacOSHost::activationMessageCountForTesting() const noexcept
{
    if (!impl_) return 0;
    __block std::uint64_t count = 0;
    dispatch_sync(impl_->controlQueue, ^{ count = impl_->activationMessagesSent; });
    return count;
}

void ComponentActivationMacOSHost::corruptNextCheckpointRequestForTesting(std::uint32_t mode) noexcept
{
    if (impl_) dispatch_sync(impl_->controlQueue, ^{ impl_->checkpointRequestCorruptionForTest = mode; });
}
#endif

void ComponentActivationMacOSHost::shutdown() noexcept
{
    if (!impl_)
        return;
    bool expected = false;
    if (!impl_->shutdownStarted.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        return;
    }
    dispatch_sync(impl_->controlQueue, ^{
        impl_->beginShutdownOnControlQueue();
    });
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto current = impl_->stateSnapshot.load(
            std::memory_order_acquire);
        if (current == ComponentActivationMacOSHostStateV1::Closed
            || current == ComponentActivationMacOSHostStateV1::Quarantined)
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

ComponentActivationMacOSHostStateV1
ComponentActivationMacOSHost::state() const noexcept
{
    return impl_ ? impl_->stateSnapshot.load(std::memory_order_acquire)
                 : ComponentActivationMacOSHostStateV1::Closed;
}

ComponentActivationMacOSHostDiagnosticV1
ComponentActivationMacOSHost::diagnostic() const noexcept
{
    return impl_ ? impl_->diagnosticSnapshot.load(std::memory_order_acquire)
                 : ComponentActivationMacOSHostDiagnosticV1::Closed;
}

ComponentActivationSealedSessionV1
ComponentActivationMacOSHost::sealedSession() const noexcept
{
    return impl_ ? impl_->session : ComponentActivationSealedSessionV1{};
}

ComponentActivationRuntimeEvidenceV1
ComponentActivationMacOSHost::runtimeEvidence() const noexcept
{
    if (!impl_)
        return {};
    const std::scoped_lock lock(impl_->evidenceMutex);
    return impl_->evidence;
}

std::uint64_t ComponentActivationMacOSHost::heartbeatCount() const noexcept
{
    return impl_ ? impl_->heartbeatCounter.load(std::memory_order_acquire) : 0;
}

std::int32_t ComponentActivationMacOSHost::peerProcessIdentifier()
    const noexcept
{
    return impl_
        ? impl_->peerProcessIdentifier.load(std::memory_order_acquire)
        : 0;
}

bool ComponentActivationMacOSHost::sealedStorageContainsNoComponentBytes()
    const noexcept
{
    return impl_ && pristineSealedStorage(
        impl_->session,
        impl_->sharedStorage,
        impl_->sharedStorageBytes);
}

#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
bool ComponentActivationMacOSHost::publishedAudioResponseReadyForTesting()
    const noexcept
{
    return impl_ && impl_->publishedAudioResponseReadyForTesting();
}

void ComponentActivationMacOSHost::cancelConnectionForTesting() noexcept
{
    if (impl_)
        impl_->cancelConnection();
}
#endif

ComponentActivationAudioOperationV1
ComponentActivationMacOSHost::processAudio(
    std::span<const float* const> inputChannels,
    std::span<float* const> outputChannels,
    std::uint32_t frameCount,
    ComponentActivationAudioTransportV1 transport) noexcept
{
    if (!impl_)
    {
        for (auto* channel : outputChannels)
        {
            if (channel != nullptr)
                std::fill_n(channel, frameCount, 0.0F);
        }
        return ComponentActivationAudioOperationV1::Quarantined;
    }
    return impl_->processAudio(
        inputChannels,
        outputChannels,
        frameCount,
        transport);
}

std::shared_ptr<PreparedIsolatedComponentBinding>
ComponentActivationMacOSHost::takePreparedIsolatedComponentBinding(
    std::string nodeId,
    const component_bundle::ComponentBundleSnapshot& snapshot,
    std::span<const ComponentActivationStaticParameterV1> staticParameters)
    && noexcept
{
    if (!impl_ || !impl_->audioActive())
        return {};
    try
    {
        const auto* resourceSetGrant =
            impl_->activationLease->resourceSetGrant();
        const auto resourceSetMembers =
            impl_->activationLease->resourceSetMembers();
        std::array<
            const ComponentActivationImmutableResourceGrantV1*,
            maximumComponentActivationResourceSetMembersV1>
            resourceSetImmutableGrants{};
        std::array<
            const ComponentActivationAudioStreamGrantV1*,
            maximumComponentActivationResourceSetMembersV1>
            resourceSetAudioGrants{};
        for (std::size_t index = 0;
             index < resourceSetMembers.size();
             ++index)
        {
            resourceSetImmutableGrants[index] =
                impl_->activationLease->resourceSetImmutableGrant(index);
            resourceSetAudioGrants[index] =
                impl_->activationLease->resourceSetAudioGrant(index);
        }
        auto contract = makePreparedRuntimeContract(
            std::move(nodeId),
            snapshot,
            staticParameters,
            impl_->activationAuthorization,
            impl_->session,
            impl_->activationResourceGrant.has_value()
                ? &*impl_->activationResourceGrant
                : nullptr,
            impl_->activationAudioStreamGrant.has_value()
                ? &*impl_->activationAudioStreamGrant
                : nullptr,
            resourceSetGrant,
            resourceSetMembers,
            std::span{
                resourceSetImmutableGrants.data(),
                resourceSetMembers.size()},
            std::span{
                resourceSetAudioGrants.data(),
                resourceSetMembers.size()});
        if (!contract.has_value())
            return {};
        const auto capability = impl_->checkpointSynchronously(ComponentActivationCheckpointOperationV1::Query);
        if (capability != ComponentActivationCheckpointStatusV1::Supported
            && capability != ComponentActivationCheckpointStatusV1::Unsupported) return {};
        auto binding = std::make_shared<PreparedBinding>(
            impl_, std::move(*contract), capability == ComponentActivationCheckpointStatusV1::Supported);
        if (!binding->active())
            return {};
        impl_.reset();
        return binding;
    }
    catch (const std::bad_alloc&)
    {
        return {};
    }
}
}
