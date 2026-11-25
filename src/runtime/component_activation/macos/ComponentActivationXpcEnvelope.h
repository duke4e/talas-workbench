#pragma once

#include "talas/runtime/ComponentActivationProtocol.h"

#include <xpc/xpc.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace talas::runtime::component_activation
{
enum class ComponentActivationXpcEnvelopeDiagnosticV1 : std::uint8_t
{
    Valid,
    ObjectTypeInvalid,
    FieldCountInvalid,
    SealedSessionMissing,
    SealedSessionSizeInvalid,
    SharedMemoryMissing,
    SharedMemoryInvalid,
    AuthorizationMissing,
    AuthorizationSizeInvalid,
    ManifestMissing,
    ManifestSizeInvalid,
    EntryMissing,
    EntrySizeInvalid,
    EntryDigestMismatch,
    StaticParametersMissing,
    StaticParametersSizeInvalid,
    StaticParametersDigestMismatch,
    ResourceGrantMissing,
    ResourceGrantSizeInvalid,
    ResourceGrantInvalid,
    ResourcePayloadMissing,
    ResourcePayloadSizeInvalid,
    ResourcePayloadDigestMismatch,
    AudioStreamGrantMissing,
    AudioStreamGrantSizeInvalid,
    AudioStreamGrantInvalid,
    AudioSourceMissing,
    AudioSourceInvalid,
    AudioSourceSizeInvalid,
    AudioSourceDigestMismatch,
    AudioSourceMediaInvalid,
    AudioSourceAllocationFailed,
    MaterializationFileMissing,
    MaterializationFileInvalid,
    CommandMissing,
    CommandSizeInvalid,
    TransitionMissing,
    TransitionSizeInvalid,
    ProtocolInvalid,
    ResourceSetGrantMissing,
    ResourceSetGrantSizeInvalid,
    ResourceSetGrantInvalid,
    ResourceSetMembersMissing,
    ResourceSetMembersInvalid,
    ResourceSetMemberInvalid,
    ResourceSetPayloadMissing,
    ResourceSetPayloadInvalid,
    ResourceSetMaterializationFailed,
};

struct ComponentActivationXpcResourceSetSourceV1
{
    const ComponentActivationImmutableResourceGrantV1* immutableGrant =
        nullptr;
    std::span<const std::uint8_t> immutableBytes;
    const ComponentActivationAudioStreamGrantV1* audioGrant = nullptr;
    xpc_object_t audioSourceSharedMemory = nullptr;
};

struct ComponentActivationXpcResourceSetMemberViewV1
{
    ComponentActivationResourceSetMemberV1 member;
    std::optional<ComponentActivationImmutableResourceGrantV1>
        immutableGrant;
    std::span<const std::uint8_t> immutableBytes;
    std::optional<ComponentActivationAudioStreamGrantV1> audioGrant;
    xpc_object_t audioSourceSharedMemory = nullptr;
};

struct ComponentActivationXpcLoadPayloadViewV1
{
    ComponentActivationAuthorizationV1 authorization;
    std::span<const std::uint8_t> canonicalManifest;
    std::span<const std::uint8_t> entryBytes;
    std::vector<ComponentActivationStaticParameterV1>
        staticParameterStorage;
    std::span<const ComponentActivationStaticParameterV1> staticParameters;
    std::optional<ComponentActivationImmutableResourceGrantV1> resourceGrant;
    std::span<const std::uint8_t> resourceBytes;
    std::optional<ComponentActivationAudioStreamGrantV1> audioStreamGrant;
    xpc_object_t audioSourceSharedMemory = nullptr;
    std::optional<ComponentActivationResourceSetGrantV1> resourceSetGrant;
    std::vector<ComponentActivationXpcResourceSetMemberViewV1>
        resourceSetMembers;
    xpc_object_t materializationFile = nullptr;
};

class ComponentActivationPrivateAudioSourceV1
{
public:
    ComponentActivationPrivateAudioSourceV1() noexcept = default;
    ~ComponentActivationPrivateAudioSourceV1();

    ComponentActivationPrivateAudioSourceV1(
        ComponentActivationPrivateAudioSourceV1&& other) noexcept;
    ComponentActivationPrivateAudioSourceV1& operator=(
        ComponentActivationPrivateAudioSourceV1&& other) noexcept;

    ComponentActivationPrivateAudioSourceV1(
        const ComponentActivationPrivateAudioSourceV1&) = delete;
    ComponentActivationPrivateAudioSourceV1& operator=(
        const ComponentActivationPrivateAudioSourceV1&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;
    [[nodiscard]] std::size_t mappedByteCount() const noexcept;

private:
    void* mapping_ = nullptr;
    std::size_t mappedBytes_ = 0;
    std::size_t sourceBytes_ = 0;

    friend ComponentActivationXpcEnvelopeDiagnosticV1
    materializeComponentActivationPrivateAudioSourceV1(
        xpc_object_t,
        const ComponentActivationAudioStreamGrantV1&,
        ComponentActivationPrivateAudioSourceV1&) noexcept;
};

class ComponentActivationPrivateResourceSetV1
{
public:
    ComponentActivationPrivateResourceSetV1() noexcept = default;
    ~ComponentActivationPrivateResourceSetV1() = default;

    ComponentActivationPrivateResourceSetV1(
        ComponentActivationPrivateResourceSetV1&&) noexcept = default;
    ComponentActivationPrivateResourceSetV1& operator=(
        ComponentActivationPrivateResourceSetV1&&) noexcept = default;

    ComponentActivationPrivateResourceSetV1(
        const ComponentActivationPrivateResourceSetV1&) = delete;
    ComponentActivationPrivateResourceSetV1& operator=(
        const ComponentActivationPrivateResourceSetV1&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::size_t memberCount() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> audioBytes(
        std::size_t memberIndex) const noexcept;

private:
    std::array<
        ComponentActivationPrivateAudioSourceV1,
        maximumComponentActivationResourceSetMembersV1>
        audioSources_{};
    std::array<bool, maximumComponentActivationResourceSetMembersV1>
        audioMembers_{};
    std::size_t memberCount_ = 0;
    bool valid_ = false;

    friend ComponentActivationXpcEnvelopeDiagnosticV1
    materializeComponentActivationResourceSetV1(
        const ComponentActivationXpcLoadPayloadViewV1&,
        ComponentActivationPrivateResourceSetV1&) noexcept;
};

[[nodiscard]] xpc_object_t createComponentActivationXpcSealedSessionV1(
    const ComponentActivationSealedSessionV1& session) noexcept;

[[nodiscard]] ComponentActivationXpcEnvelopeDiagnosticV1
decodeComponentActivationXpcSealedSessionV1(
    xpc_object_t object,
    ComponentActivationSealedSessionV1& session) noexcept;

[[nodiscard]] xpc_object_t createComponentActivationXpcSealedPrewarmV1(
    const ComponentActivationSealedSessionV1& session,
    xpc_object_t sharedMemory) noexcept;

[[nodiscard]] ComponentActivationXpcEnvelopeDiagnosticV1
decodeComponentActivationXpcSealedPrewarmV1(
    xpc_object_t object,
    ComponentActivationSealedSessionV1& session,
    xpc_object_t& sharedMemory) noexcept;

// This is a private typed transport primitive. Product activation code may
// obtain its record only by consuming ComponentActivationAuthorityV1.
[[nodiscard]] xpc_object_t createComponentActivationXpcAuthorizationV1(
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session) noexcept;

[[nodiscard]] ComponentActivationXpcEnvelopeDiagnosticV1
decodeComponentActivationXpcAuthorizationV1(
    xpc_object_t object,
    const ComponentActivationSealedSessionV1& session,
    ComponentActivationAuthorizationV1& authorization) noexcept;

[[nodiscard]] xpc_object_t createComponentActivationXpcCheckpointCommandV1(
    const ComponentActivationCheckpointCommandV1& request,
    const ComponentActivationAuthorizationV1& authorization) noexcept;
[[nodiscard]] ComponentActivationXpcEnvelopeDiagnosticV1
 decodeComponentActivationXpcCheckpointCommandV1(
    xpc_object_t object, const ComponentActivationAuthorizationV1& authorization,
    ComponentActivationCheckpointCommandV1& request) noexcept;
[[nodiscard]] bool writeComponentActivationXpcCheckpointReplyV1(
    xpc_object_t object, const ComponentActivationCheckpointReplyV1& reply,
    const ComponentActivationAuthorizationV1& authorization) noexcept;
[[nodiscard]] ComponentActivationXpcEnvelopeDiagnosticV1
 decodeComponentActivationXpcCheckpointReplyV1(
    xpc_object_t object, const ComponentActivationAuthorizationV1& authorization,
    ComponentActivationCheckpointReplyV1& reply) noexcept;

[[nodiscard]] xpc_object_t createComponentActivationXpcCommandV1(
    const ComponentActivationCommandV1& command,
    ComponentActivationRecordKindV1 expectedKind,
    const ComponentActivationAuthorizationV1& authorization) noexcept;

[[nodiscard]] ComponentActivationXpcEnvelopeDiagnosticV1
decodeComponentActivationXpcCommandV1(
    xpc_object_t object,
    ComponentActivationRecordKindV1 expectedKind,
    const ComponentActivationAuthorizationV1& authorization,
    ComponentActivationCommandV1& command) noexcept;

// Creates one page-rounded anonymous, zero-tailed, read-only XPC memory entry.
// The caller owns the returned XPC object.
[[nodiscard]] xpc_object_t
createComponentActivationXpcAudioSourceSharedMemoryV1(
    const ComponentActivationAudioStreamGrantV1& grant,
    std::span<const std::uint8_t> sourceBytes) noexcept;

// Creates one self-contained exact set object with a bounded ordered member
// array. The caller owns the returned XPC object.
[[nodiscard]] xpc_object_t createComponentActivationXpcResourceSetV1(
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session,
    std::span<const ComponentActivationStaticParameterV1> staticParameters,
    const ComponentActivationResourceSetGrantV1& resourceSetGrant,
    std::span<const ComponentActivationResourceSetMemberV1>
        resourceSetMembers,
    std::span<const ComponentActivationXpcResourceSetSourceV1>
        resourceSetSources) noexcept;

// Maps a borrowed XPC memory entry only long enough to validate it, then owns
// an independent anonymous read-only copy. No path or descriptor is retained.
[[nodiscard]] ComponentActivationXpcEnvelopeDiagnosticV1
materializeComponentActivationPrivateAudioSourceV1(
    xpc_object_t sharedMemory,
    const ComponentActivationAudioStreamGrantV1& grant,
    ComponentActivationPrivateAudioSourceV1& source) noexcept;

// Materializes every Audio member into independent anonymous read-only memory.
// The destination is published only after the complete ordered set succeeds.
[[nodiscard]] ComponentActivationXpcEnvelopeDiagnosticV1
materializeComponentActivationResourceSetV1(
    const ComponentActivationXpcLoadPayloadViewV1& payload,
    ComponentActivationPrivateResourceSetV1& resources) noexcept;

// Byte spans borrow XPC data and remain valid only while object is alive;
// static parameters are copied into aligned bounded storage owned by the view.
[[nodiscard]] xpc_object_t createComponentActivationXpcLoadPayloadV1(
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session,
    std::span<const std::uint8_t> canonicalManifest,
    std::span<const std::uint8_t> entryBytes,
    std::span<const ComponentActivationStaticParameterV1> staticParameters,
    xpc_object_t materializationFile,
    const ComponentActivationImmutableResourceGrantV1* resourceGrant = nullptr,
    std::span<const std::uint8_t> resourceBytes = {},
    const ComponentActivationAudioStreamGrantV1* audioStreamGrant = nullptr,
    xpc_object_t audioSourceSharedMemory = nullptr,
    const ComponentActivationResourceSetGrantV1* resourceSetGrant = nullptr,
    std::span<const ComponentActivationResourceSetMemberV1>
        resourceSetMembers = {},
    std::span<const ComponentActivationXpcResourceSetSourceV1>
        resourceSetSources = {})
    noexcept;

[[nodiscard]] ComponentActivationXpcEnvelopeDiagnosticV1
decodeComponentActivationXpcLoadPayloadV1(
    xpc_object_t object,
    const ComponentActivationSealedSessionV1& session,
    ComponentActivationXpcLoadPayloadViewV1& payload) noexcept;

[[nodiscard]] bool writeComponentActivationXpcTransitionV1(
    xpc_object_t dictionary,
    const ComponentActivationTransitionV1& transition,
    ComponentActivationRecordKindV1 expectedKind,
    const ComponentActivationAuthorizationV1& authorization) noexcept;

[[nodiscard]] bool writeComponentActivationXpcSealedTransitionV1(
    xpc_object_t dictionary,
    const ComponentActivationTransitionV1& transition,
    ComponentActivationRecordKindV1 expectedKind,
    const ComponentActivationSealedSessionV1& session) noexcept;

[[nodiscard]] ComponentActivationXpcEnvelopeDiagnosticV1
decodeComponentActivationXpcSealedTransitionV1(
    xpc_object_t object,
    ComponentActivationRecordKindV1 expectedKind,
    const ComponentActivationSealedSessionV1& session,
    ComponentActivationTransitionV1& transition) noexcept;

[[nodiscard]] ComponentActivationXpcEnvelopeDiagnosticV1
decodeComponentActivationXpcTransitionV1(
    xpc_object_t object,
    ComponentActivationRecordKindV1 expectedKind,
    const ComponentActivationAuthorizationV1& authorization,
    ComponentActivationTransitionV1& transition) noexcept;
}
