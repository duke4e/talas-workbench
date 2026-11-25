#include "runtime/component_activation/macos/ComponentActivationXpcEnvelope.h"

#include "runtime/component_activation/ComponentActivationAudioSourceFreezer.h"
#include "runtime/component_bundle/ComponentBundleManifest.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string_view>

namespace talas::runtime::component_activation
{
namespace
{
constexpr char sealedSessionKey[] =
    "talas.component-activation.sealed-session.v1";
constexpr char sharedMemoryKey[] =
    "talas.component-activation.shared-memory.v1";
constexpr char authorizationKey[] =
    "talas.component-activation.authorization.v1";
constexpr char manifestKey[] =
    "talas.component-activation.manifest.v1";
constexpr char entryKey[] =
    "talas.component-activation.entry.v1";
constexpr char parametersKey[] =
    "talas.component-activation.parameters.v1";
constexpr char resourceGrantKey[] =
    "talas.component-activation.immutable-resource-grant.v1";
constexpr char resourcePayloadKey[] =
    "talas.component-activation.immutable-resource-payload.v1";
constexpr char audioStreamGrantKey[] =
    "talas.component-activation.audio-stream-grant.v1";
constexpr char audioSourceSharedMemoryKey[] =
    "talas.component-activation.audio-source-shmem.v1";
constexpr char resourceSetKey[] =
    "talas.component-activation.resource-set.v1";
constexpr char resourceSetGrantKey[] =
    "talas.component-activation.resource-set-grant.v1";
constexpr char resourceSetMembersKey[] =
    "talas.component-activation.resource-set-members.v1";
constexpr char resourceSetMemberKey[] =
    "talas.component-activation.resource-set-member.v1";
constexpr char materializationFileKey[] =
    "talas.component-activation.materialization-file.v1";
constexpr char commandKey[] = "talas.component-activation.command.v1";
constexpr char transitionKey[] =
    "talas.component-activation.transition.v1";

[[nodiscard]] ComponentActivationXpcEnvelopeDiagnosticV1
validateAudioSourceSharedMemoryV1(
    xpc_object_t sharedMemory,
    const ComponentActivationAudioStreamGrantV1& grant) noexcept;

template <typename Record>
[[nodiscard]] std::array<std::uint8_t, componentTrustDigestBytes>
recordDigest(const Record& record) noexcept
{
    return calculateComponentActivationByteDigestV1(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(&record),
            sizeof(record)});
}

[[nodiscard]] std::optional<std::uint64_t> resourceSetTransportBytes(
    std::uint64_t sourceBytes) noexcept
{
    constexpr auto page =
        maximumComponentActivationResourceSetTransportPageBytesV1;
    if (sourceBytes == 0
        || sourceBytes > std::numeric_limits<std::uint64_t>::max()
                - (page - 1U))
    {
        return std::nullopt;
    }
    return ((sourceBytes + page - 1U) / page) * page;
}

[[nodiscard]] bool exactResourceSetMemberSource(
    const ComponentActivationResourceSetMemberV1& member,
    const ComponentActivationXpcResourceSetSourceV1& source,
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session,
    std::span<const ComponentActivationStaticParameterV1> parameters)
    noexcept
{
    const auto expectedTransport =
        resourceSetTransportBytes(member.sourceByteCount);
    if (!expectedTransport.has_value()
        || member.transportByteCount != *expectedTransport
        || member.privateMappedByteCount != *expectedTransport)
    {
        return false;
    }
    const auto kind = static_cast<ComponentActivationResourceSetMemberKindV1>(
        member.memberKind);
    if (kind == ComponentActivationResourceSetMemberKindV1::Audio)
    {
        if (source.immutableGrant != nullptr
            || !source.immutableBytes.empty()
            || source.audioGrant == nullptr
            || source.audioSourceSharedMemory == nullptr
            || member.workingByteCount
                != maximumComponentActivationAudioStreamWorkingBytesV1
            || validateComponentActivationAudioStreamGrantV1(
                   *source.audioGrant,
                   authorization,
                   session,
                   parameters)
                != ComponentActivationProtocolDiagnosticV1::Valid
            || recordDigest(*source.audioGrant) != member.memberGrantDigest
            || source.audioGrant->sourceContentDigest
                != member.sourceContentDigest
            || source.audioGrant->sourceByteCount != member.sourceByteCount
            || source.audioGrant->resourceGeneration
                != member.resourceGeneration
            || std::string_view{
                   source.audioGrant->parameterId.data(),
                   source.audioGrant->parameterIdBytes}
                != std::string_view{
                   member.parameterId.data(), member.parameterIdBytes}
            || std::string_view{
                   source.audioGrant->resourceId.data(),
                   source.audioGrant->resourceIdBytes}
                != std::string_view{
                   member.resourceId.data(), member.resourceIdBytes})
        {
            return false;
        }
        return validateAudioSourceSharedMemoryV1(
                   source.audioSourceSharedMemory,
                   *source.audioGrant)
            == ComponentActivationXpcEnvelopeDiagnosticV1::Valid;
    }
    if ((kind != ComponentActivationResourceSetMemberKindV1::Table
            && kind != ComponentActivationResourceSetMemberKindV1::Binary)
        || source.immutableGrant == nullptr
        || source.immutableBytes.empty() || source.audioGrant != nullptr
        || source.audioSourceSharedMemory != nullptr
        || member.workingByteCount != 0
        || validateComponentActivationImmutableResourceGrantV1(
               *source.immutableGrant,
               authorization,
               session,
               parameters)
            != ComponentActivationProtocolDiagnosticV1::Valid
        || recordDigest(*source.immutableGrant) != member.memberGrantDigest
        || source.immutableGrant->sourceContentDigest
            != member.sourceContentDigest
        || source.immutableGrant->mediatedPayloadDigest
            != member.mediatedPayloadDigest
        || source.immutableGrant->byteCount != member.sourceByteCount
        || source.immutableGrant->resourceGeneration
            != member.resourceGeneration
        || source.immutableBytes.size() != member.sourceByteCount
        || calculateComponentActivationByteDigestV1(source.immutableBytes)
            != member.mediatedPayloadDigest
        || std::string_view{
               source.immutableGrant->parameterId.data(),
               source.immutableGrant->parameterIdBytes}
            != std::string_view{
               member.parameterId.data(), member.parameterIdBytes}
        || std::string_view{
               source.immutableGrant->resourceId.data(),
               source.immutableGrant->resourceIdBytes}
            != std::string_view{
               member.resourceId.data(), member.resourceIdBytes})
    {
        return false;
    }
    const auto resourceKind = static_cast<
        ComponentActivationImmutableResourceKindV1>(
        source.immutableGrant->resourceKind);
    return (kind == ComponentActivationResourceSetMemberKindV1::Table
               && resourceKind
                   == ComponentActivationImmutableResourceKindV1::Table)
        || (kind == ComponentActivationResourceSetMemberKindV1::Binary
            && resourceKind
                == ComponentActivationImmutableResourceKindV1::Binary);
}

[[nodiscard]] bool exactResourceSetPayload(
    const ComponentActivationResourceSetGrantV1* grant,
    std::span<const ComponentActivationResourceSetMemberV1> members,
    std::span<const ComponentActivationXpcResourceSetSourceV1> sources,
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session,
    std::span<const ComponentActivationStaticParameterV1> parameters)
    noexcept
{
    if (grant == nullptr || members.size() != sources.size()
        || validateComponentActivationResourceSetGrantV1(
               *grant, authorization, session)
            != ComponentActivationProtocolDiagnosticV1::Valid
        || validateComponentActivationResourceSetMembersV1(*grant, members)
            != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        return false;
    }
    std::size_t memberIndex = 0;
    for (const auto& parameter : parameters)
    {
        if (parameter.kind != static_cast<std::uint32_t>(
                ComponentActivationStaticParameterKindV1::Resource))
        {
            continue;
        }
        if (memberIndex >= members.size()
            || std::string_view{
                   parameter.id.data(), parameter.idBytes}
                != std::string_view{
                   members[memberIndex].parameterId.data(),
                   members[memberIndex].parameterIdBytes}
            || std::string_view{
                   parameter.text.data(), parameter.textBytes}
                != std::string_view{
                   members[memberIndex].resourceId.data(),
                   members[memberIndex].resourceIdBytes}
            || !exactResourceSetMemberSource(
                members[memberIndex],
                sources[memberIndex],
                authorization,
                session,
                parameters))
        {
            return false;
        }
        ++memberIndex;
    }
    return memberIndex == members.size();
}

[[nodiscard]] bool exactLoadPayload(
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session,
    std::span<const std::uint8_t> canonicalManifest,
    std::span<const std::uint8_t> entryBytes,
    std::span<const ComponentActivationStaticParameterV1> parameters,
    const ComponentActivationImmutableResourceGrantV1* resourceGrant,
    std::span<const std::uint8_t> resourceBytes,
    const ComponentActivationAudioStreamGrantV1* audioStreamGrant,
    xpc_object_t audioSourceSharedMemory,
    const ComponentActivationResourceSetGrantV1* resourceSetGrant,
    std::span<const ComponentActivationResourceSetMemberV1>
        resourceSetMembers,
    std::span<const ComponentActivationXpcResourceSetSourceV1>
        resourceSetSources)
    noexcept
{
    const auto baseValid =
        validateComponentActivationAuthorizationV1(authorization, session)
            == ComponentActivationProtocolDiagnosticV1::Valid
        && !canonicalManifest.empty()
        && canonicalManifest.size()
            <= component_bundle::maximumComponentBundleManifestBytes
        && !entryBytes.empty()
        && entryBytes.size() == authorization.entryByteCount
        && entryBytes.size() <= maximumComponentActivationEntryBytesV1
        && calculateComponentActivationByteDigestV1(entryBytes)
            == authorization.entryDigest
        && parameters.size() == authorization.staticParameterCount
        && validateComponentActivationStaticParametersV1(parameters)
            == ComponentActivationProtocolDiagnosticV1::Valid
        && calculateComponentActivationStaticParameterDigestV1(parameters)
            == authorization.staticParameterDigest;
    if (!baseValid)
        return false;
    const auto resourceCount = static_cast<std::size_t>(std::count_if(
        parameters.begin(),
        parameters.end(),
        [](const auto& parameter) {
            return parameter.kind == static_cast<std::uint32_t>(
                ComponentActivationStaticParameterKindV1::Resource);
        }));
    const auto immutablePermission = componentPermission(
        ComponentExecutionPermission::ImmutableResourceRead);
    const auto audioPermission = componentPermission(
        ComponentExecutionPermission::BoundedAudioStreamRead);
    const auto immutableProfile =
        (authorization.requestedPermissions & immutablePermission) != 0;
    const auto audioProfile =
        (authorization.requestedPermissions & audioPermission) != 0;
    if (resourceCount == 0)
    {
        return !immutableProfile && !audioProfile
            && resourceGrant == nullptr && resourceBytes.empty()
            && audioStreamGrant == nullptr
            && audioSourceSharedMemory == nullptr
            && resourceSetGrant == nullptr && resourceSetMembers.empty()
            && resourceSetSources.empty();
    }
    if (resourceCount > 1)
    {
        return (immutableProfile || audioProfile)
            && resourceGrant == nullptr && resourceBytes.empty()
            && audioStreamGrant == nullptr
            && audioSourceSharedMemory == nullptr
            && exactResourceSetPayload(
                resourceSetGrant,
                resourceSetMembers,
                resourceSetSources,
                authorization,
                session,
                parameters);
    }
    if (immutableProfile == audioProfile || resourceSetGrant != nullptr
        || !resourceSetMembers.empty() || !resourceSetSources.empty())
        return false;
    if (immutableProfile)
    {
        return audioStreamGrant == nullptr
            && audioSourceSharedMemory == nullptr
            && resourceGrant != nullptr && !resourceBytes.empty()
            && resourceBytes.size()
                <= maximumComponentActivationImmutableResourceBytesV1
            && resourceBytes.size() == resourceGrant->byteCount
            && calculateComponentActivationByteDigestV1(resourceBytes)
                == resourceGrant->mediatedPayloadDigest
            && validateComponentActivationImmutableResourceGrantV1(
                   *resourceGrant,
                   authorization,
                   session,
                   parameters)
                == ComponentActivationProtocolDiagnosticV1::Valid;
    }
    if (resourceGrant != nullptr || !resourceBytes.empty()
        || audioStreamGrant == nullptr
        || audioSourceSharedMemory == nullptr
        || xpc_get_type(audioSourceSharedMemory) != XPC_TYPE_SHMEM
        || validateComponentActivationAudioStreamGrantV1(
               *audioStreamGrant,
               authorization,
               session,
               parameters)
            != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        return false;
    }
    return validateAudioSourceSharedMemoryV1(
               audioSourceSharedMemory,
               *audioStreamGrant)
        == ComponentActivationXpcEnvelopeDiagnosticV1::Valid;
}

[[nodiscard]] std::optional<std::size_t> pageRoundedAudioSourceBytes(
    std::uint64_t sourceBytes) noexcept
{
    const auto pageSizeValue = sysconf(_SC_PAGESIZE);
    if (pageSizeValue <= 0
        || sourceBytes < minimumComponentActivationAudioSourceBytesV1
        || sourceBytes > maximumComponentActivationAudioSourceBytesV1
        || sourceBytes > std::numeric_limits<std::size_t>::max())
    {
        return std::nullopt;
    }
    const auto pageSize = static_cast<std::size_t>(pageSizeValue);
    const auto bytes = static_cast<std::size_t>(sourceBytes);
    if (bytes > std::numeric_limits<std::size_t>::max() - (pageSize - 1U))
        return std::nullopt;
    return ((bytes + pageSize - 1U) / pageSize) * pageSize;
}

[[nodiscard]] xpc_object_t createResourceSetMemberObject(
    const ComponentActivationResourceSetMemberV1& member,
    const ComponentActivationXpcResourceSetSourceV1& source) noexcept
{
    auto object = xpc_dictionary_create_empty();
    if (object == nullptr)
        return nullptr;
    xpc_dictionary_set_data(
        object, resourceSetMemberKey, &member, sizeof(member));
    const auto kind = static_cast<ComponentActivationResourceSetMemberKindV1>(
        member.memberKind);
    if (kind == ComponentActivationResourceSetMemberKindV1::Audio)
    {
        xpc_dictionary_set_data(
            object,
            audioStreamGrantKey,
            source.audioGrant,
            sizeof(*source.audioGrant));
        xpc_dictionary_set_value(
            object,
            audioSourceSharedMemoryKey,
            source.audioSourceSharedMemory);
    }
    else
    {
        xpc_dictionary_set_data(
            object,
            resourceGrantKey,
            source.immutableGrant,
            sizeof(*source.immutableGrant));
        xpc_dictionary_set_data(
            object,
            resourcePayloadKey,
            source.immutableBytes.data(),
            source.immutableBytes.size());
    }
    return object;
}

[[nodiscard]] bool exactAudioSourceBytes(
    std::span<const std::uint8_t> bytes,
    const ComponentActivationAudioStreamGrantV1& grant) noexcept
{
    if (bytes.size() != grant.sourceByteCount
        || calculateComponentActivationByteDigestV1(bytes)
            != grant.sourceContentDigest)
    {
        return false;
    }
    const auto shape = parseComponentActivationPcm16WaveV1(bytes);
    return shape.has_value()
        && shape->channelCount == grant.sourceChannelCount
        && shape->sampleRate == grant.sourceSampleRate
        && shape->frameCount == grant.sourceFrameCount;
}

class ScopedMapping
{
public:
    ScopedMapping(void* mapping, std::size_t bytes) noexcept
        : mapping_(mapping), bytes_(bytes)
    {
    }

    ~ScopedMapping()
    {
        if (mapping_ != nullptr && mapping_ != MAP_FAILED && bytes_ != 0)
            static_cast<void>(munmap(mapping_, bytes_));
    }

    ScopedMapping(const ScopedMapping&) = delete;
    ScopedMapping& operator=(const ScopedMapping&) = delete;

    [[nodiscard]] void* release() noexcept
    {
        auto* mapping = mapping_;
        mapping_ = nullptr;
        bytes_ = 0;
        return mapping;
    }

private:
    void* mapping_ = nullptr;
    std::size_t bytes_ = 0;
};

ComponentActivationXpcEnvelopeDiagnosticV1
validateAudioSourceSharedMemoryV1(
    xpc_object_t sharedMemory,
    const ComponentActivationAudioStreamGrantV1& grant) noexcept
{
    if (sharedMemory == nullptr)
        return ComponentActivationXpcEnvelopeDiagnosticV1::AudioSourceMissing;
    if (xpc_get_type(sharedMemory) != XPC_TYPE_SHMEM)
        return ComponentActivationXpcEnvelopeDiagnosticV1::AudioSourceInvalid;
    const auto expectedMappedBytes = pageRoundedAudioSourceBytes(
        grant.sourceByteCount);
    if (!expectedMappedBytes.has_value())
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            AudioSourceSizeInvalid;
    }
    void* mapping = nullptr;
    const auto mappedBytes = xpc_shmem_map(sharedMemory, &mapping);
    ScopedMapping owner{mapping, mappedBytes};
    if (mapping == nullptr || mapping == MAP_FAILED
        || mappedBytes != *expectedMappedBytes)
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            AudioSourceSizeInvalid;
    }
    const auto* data = static_cast<const std::uint8_t*>(mapping);
    const auto sourceBytes = static_cast<std::size_t>(grant.sourceByteCount);
    if (!std::all_of(
            data + sourceBytes,
            data + mappedBytes,
            [](std::uint8_t value) { return value == 0; }))
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::AudioSourceInvalid;
    }
    const std::span<const std::uint8_t> source{data, sourceBytes};
    if (calculateComponentActivationByteDigestV1(source)
        != grant.sourceContentDigest)
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            AudioSourceDigestMismatch;
    }
    const auto shape = parseComponentActivationPcm16WaveV1(source);
    if (!shape.has_value()
        || shape->channelCount != grant.sourceChannelCount
        || shape->sampleRate != grant.sourceSampleRate
        || shape->frameCount != grant.sourceFrameCount)
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            AudioSourceMediaInvalid;
    }
    return ComponentActivationXpcEnvelopeDiagnosticV1::Valid;
}
}

ComponentActivationPrivateAudioSourceV1::~ComponentActivationPrivateAudioSourceV1()
{
    if (mapping_ != nullptr && mappedBytes_ != 0)
        static_cast<void>(munmap(mapping_, mappedBytes_));
}

ComponentActivationPrivateAudioSourceV1::
ComponentActivationPrivateAudioSourceV1(
    ComponentActivationPrivateAudioSourceV1&& other) noexcept
    : mapping_(other.mapping_)
    , mappedBytes_(other.mappedBytes_)
    , sourceBytes_(other.sourceBytes_)
{
    other.mapping_ = nullptr;
    other.mappedBytes_ = 0;
    other.sourceBytes_ = 0;
}

ComponentActivationPrivateAudioSourceV1&
ComponentActivationPrivateAudioSourceV1::operator=(
    ComponentActivationPrivateAudioSourceV1&& other) noexcept
{
    if (this == &other)
        return *this;
    if (mapping_ != nullptr && mappedBytes_ != 0)
        static_cast<void>(munmap(mapping_, mappedBytes_));
    mapping_ = other.mapping_;
    mappedBytes_ = other.mappedBytes_;
    sourceBytes_ = other.sourceBytes_;
    other.mapping_ = nullptr;
    other.mappedBytes_ = 0;
    other.sourceBytes_ = 0;
    return *this;
}

bool ComponentActivationPrivateAudioSourceV1::valid() const noexcept
{
    return mapping_ != nullptr && mappedBytes_ != 0 && sourceBytes_ != 0
        && sourceBytes_ <= mappedBytes_;
}

std::span<const std::uint8_t>
ComponentActivationPrivateAudioSourceV1::bytes() const noexcept
{
    return valid()
        ? std::span<const std::uint8_t>{
              static_cast<const std::uint8_t*>(mapping_), sourceBytes_}
        : std::span<const std::uint8_t>{};
}

std::size_t
ComponentActivationPrivateAudioSourceV1::mappedByteCount() const noexcept
{
    return valid() ? mappedBytes_ : 0;
}

bool ComponentActivationPrivateResourceSetV1::valid() const noexcept
{
    return valid_ && memberCount_ != 0
        && memberCount_ <= maximumComponentActivationResourceSetMembersV1;
}

std::size_t ComponentActivationPrivateResourceSetV1::memberCount()
    const noexcept
{
    return valid() ? memberCount_ : 0;
}

std::span<const std::uint8_t>
ComponentActivationPrivateResourceSetV1::audioBytes(
    std::size_t memberIndex) const noexcept
{
    return valid() && memberIndex < memberCount_
            && audioMembers_[memberIndex]
        ? audioSources_[memberIndex].bytes()
        : std::span<const std::uint8_t>{};
}

xpc_object_t createComponentActivationXpcSealedSessionV1(
    const ComponentActivationSealedSessionV1& session) noexcept
{
    if (validateComponentActivationSealedSessionV1(session)
        != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        return nullptr;
    }
    auto message = xpc_dictionary_create_empty();
    if (message == nullptr)
        return nullptr;
    xpc_dictionary_set_data(
        message, sealedSessionKey, &session, sizeof(session));
    return message;
}

ComponentActivationXpcEnvelopeDiagnosticV1
decodeComponentActivationXpcSealedSessionV1(
    xpc_object_t object,
    ComponentActivationSealedSessionV1& session) noexcept
{
    session = {};
    if (object == nullptr || xpc_get_type(object) != XPC_TYPE_DICTIONARY)
        return ComponentActivationXpcEnvelopeDiagnosticV1::ObjectTypeInvalid;
    if (xpc_dictionary_get_count(object) != 1)
        return ComponentActivationXpcEnvelopeDiagnosticV1::FieldCountInvalid;
    std::size_t bytes = 0;
    const auto* data = xpc_dictionary_get_data(
        object, sealedSessionKey, &bytes);
    if (data == nullptr)
        return ComponentActivationXpcEnvelopeDiagnosticV1::SealedSessionMissing;
    if (bytes != sizeof(session))
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            SealedSessionSizeInvalid;
    }
    std::memcpy(&session, data, bytes);
    if (validateComponentActivationSealedSessionV1(session)
        != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        session = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::ProtocolInvalid;
    }
    return ComponentActivationXpcEnvelopeDiagnosticV1::Valid;
}

xpc_object_t createComponentActivationXpcSealedPrewarmV1(
    const ComponentActivationSealedSessionV1& session,
    xpc_object_t sharedMemory) noexcept
{
    if (validateComponentActivationSealedSessionV1(session)
            != ComponentActivationProtocolDiagnosticV1::Valid
        || sharedMemory == nullptr
        || xpc_get_type(sharedMemory) != XPC_TYPE_SHMEM)
    {
        return nullptr;
    }
    auto message = xpc_dictionary_create_empty();
    if (message == nullptr)
        return nullptr;
    xpc_dictionary_set_data(
        message, sealedSessionKey, &session, sizeof(session));
    xpc_dictionary_set_value(message, sharedMemoryKey, sharedMemory);
    return message;
}

ComponentActivationXpcEnvelopeDiagnosticV1
decodeComponentActivationXpcSealedPrewarmV1(
    xpc_object_t object,
    ComponentActivationSealedSessionV1& session,
    xpc_object_t& sharedMemory) noexcept
{
    session = {};
    sharedMemory = nullptr;
    if (object == nullptr || xpc_get_type(object) != XPC_TYPE_DICTIONARY)
        return ComponentActivationXpcEnvelopeDiagnosticV1::ObjectTypeInvalid;
    if (xpc_dictionary_get_count(object) != 2)
        return ComponentActivationXpcEnvelopeDiagnosticV1::FieldCountInvalid;
    std::size_t bytes = 0;
    const auto* data = xpc_dictionary_get_data(
        object, sealedSessionKey, &bytes);
    if (data == nullptr)
        return ComponentActivationXpcEnvelopeDiagnosticV1::SealedSessionMissing;
    if (bytes != sizeof(session))
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            SealedSessionSizeInvalid;
    }
    sharedMemory = xpc_dictionary_get_value(object, sharedMemoryKey);
    if (sharedMemory == nullptr)
        return ComponentActivationXpcEnvelopeDiagnosticV1::SharedMemoryMissing;
    if (xpc_get_type(sharedMemory) != XPC_TYPE_SHMEM)
    {
        sharedMemory = nullptr;
        return ComponentActivationXpcEnvelopeDiagnosticV1::SharedMemoryInvalid;
    }
    std::memcpy(&session, data, bytes);
    if (validateComponentActivationSealedSessionV1(session)
        != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        session = {};
        sharedMemory = nullptr;
        return ComponentActivationXpcEnvelopeDiagnosticV1::ProtocolInvalid;
    }
    return ComponentActivationXpcEnvelopeDiagnosticV1::Valid;
}

xpc_object_t createComponentActivationXpcAuthorizationV1(
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session) noexcept
{
    if (validateComponentActivationAuthorizationV1(authorization, session)
        != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        return nullptr;
    }
    auto message = xpc_dictionary_create_empty();
    if (message == nullptr)
        return nullptr;
    xpc_dictionary_set_data(
        message, authorizationKey, &authorization, sizeof(authorization));
    return message;
}

ComponentActivationXpcEnvelopeDiagnosticV1
decodeComponentActivationXpcAuthorizationV1(
    xpc_object_t object,
    const ComponentActivationSealedSessionV1& session,
    ComponentActivationAuthorizationV1& authorization) noexcept
{
    authorization = {};
    if (object == nullptr || xpc_get_type(object) != XPC_TYPE_DICTIONARY)
        return ComponentActivationXpcEnvelopeDiagnosticV1::ObjectTypeInvalid;
    if (xpc_dictionary_get_count(object) != 1)
        return ComponentActivationXpcEnvelopeDiagnosticV1::FieldCountInvalid;
    std::size_t bytes = 0;
    const auto* data = xpc_dictionary_get_data(
        object, authorizationKey, &bytes);
    if (data == nullptr)
        return ComponentActivationXpcEnvelopeDiagnosticV1::AuthorizationMissing;
    if (bytes != sizeof(authorization))
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            AuthorizationSizeInvalid;
    }
    std::memcpy(&authorization, data, bytes);
    if (validateComponentActivationAuthorizationV1(authorization, session)
        != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        authorization = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::ProtocolInvalid;
    }
    return ComponentActivationXpcEnvelopeDiagnosticV1::Valid;
}

namespace
{
constexpr char checkpointCommandKey[] = "talas.component-activation.checkpoint-command.v1";
constexpr char checkpointReplyKey[] = "talas.component-activation.checkpoint-reply.v1";

template<class Record, class Validator>
ComponentActivationXpcEnvelopeDiagnosticV1 decodeCheckpoint(
    xpc_object_t object, const char* key,
    const ComponentActivationAuthorizationV1& authorization,
    Record& record, Validator validate) noexcept
{
    record = {};
    if (object == nullptr || xpc_get_type(object) != XPC_TYPE_DICTIONARY)
        return ComponentActivationXpcEnvelopeDiagnosticV1::ObjectTypeInvalid;
    if (xpc_dictionary_get_count(object) != 1)
        return ComponentActivationXpcEnvelopeDiagnosticV1::FieldCountInvalid;
    std::size_t bytes = 0;
    const auto* data = xpc_dictionary_get_data(object, key, &bytes);
    if (data == nullptr) return ComponentActivationXpcEnvelopeDiagnosticV1::CommandMissing;
    if (bytes != sizeof(record)) return ComponentActivationXpcEnvelopeDiagnosticV1::CommandSizeInvalid;
    std::memcpy(&record, data, bytes);
    if (!validate(record, authorization))
    {
        record = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::ProtocolInvalid;
    }
    return ComponentActivationXpcEnvelopeDiagnosticV1::Valid;
}
}

xpc_object_t createComponentActivationXpcCheckpointCommandV1(
    const ComponentActivationCheckpointCommandV1& request,
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    if (!validateComponentActivationCheckpointCommandV1(request, authorization)) return nullptr;
    auto message = xpc_dictionary_create_empty();
    if (message != nullptr) xpc_dictionary_set_data(message, checkpointCommandKey, &request, sizeof(request));
    return message;
}

ComponentActivationXpcEnvelopeDiagnosticV1 decodeComponentActivationXpcCheckpointCommandV1(
    xpc_object_t object, const ComponentActivationAuthorizationV1& authorization,
    ComponentActivationCheckpointCommandV1& request) noexcept
{
    return decodeCheckpoint(object, checkpointCommandKey, authorization, request,
        validateComponentActivationCheckpointCommandV1);
}

bool writeComponentActivationXpcCheckpointReplyV1(
    xpc_object_t object, const ComponentActivationCheckpointReplyV1& reply,
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    if (object == nullptr || xpc_get_type(object) != XPC_TYPE_DICTIONARY
        || xpc_dictionary_get_count(object) != 0
        || !validateComponentActivationCheckpointReplyV1(reply, authorization)) return false;
    xpc_dictionary_set_data(object, checkpointReplyKey, &reply, sizeof(reply));
    return true;
}

ComponentActivationXpcEnvelopeDiagnosticV1 decodeComponentActivationXpcCheckpointReplyV1(
    xpc_object_t object, const ComponentActivationAuthorizationV1& authorization,
    ComponentActivationCheckpointReplyV1& reply) noexcept
{
    return decodeCheckpoint(object, checkpointReplyKey, authorization, reply,
        validateComponentActivationCheckpointReplyV1);
}

xpc_object_t createComponentActivationXpcCommandV1(
    const ComponentActivationCommandV1& command,
    ComponentActivationRecordKindV1 expectedKind,
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    if (validateComponentActivationCommandV1(
            command, expectedKind, authorization)
        != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        return nullptr;
    }
    auto message = xpc_dictionary_create_empty();
    if (message == nullptr)
        return nullptr;
    xpc_dictionary_set_data(message, commandKey, &command, sizeof(command));
    return message;
}

ComponentActivationXpcEnvelopeDiagnosticV1
decodeComponentActivationXpcCommandV1(
    xpc_object_t object,
    ComponentActivationRecordKindV1 expectedKind,
    const ComponentActivationAuthorizationV1& authorization,
    ComponentActivationCommandV1& command) noexcept
{
    command = {};
    if (object == nullptr || xpc_get_type(object) != XPC_TYPE_DICTIONARY)
        return ComponentActivationXpcEnvelopeDiagnosticV1::ObjectTypeInvalid;
    if (xpc_dictionary_get_count(object) != 1)
        return ComponentActivationXpcEnvelopeDiagnosticV1::FieldCountInvalid;
    std::size_t bytes = 0;
    const auto* data = xpc_dictionary_get_data(object, commandKey, &bytes);
    if (data == nullptr)
        return ComponentActivationXpcEnvelopeDiagnosticV1::CommandMissing;
    if (bytes != sizeof(command))
        return ComponentActivationXpcEnvelopeDiagnosticV1::CommandSizeInvalid;
    std::memcpy(&command, data, bytes);
    if (validateComponentActivationCommandV1(
            command, expectedKind, authorization)
        != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        command = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::ProtocolInvalid;
    }
    return ComponentActivationXpcEnvelopeDiagnosticV1::Valid;
}

xpc_object_t createComponentActivationXpcAudioSourceSharedMemoryV1(
    const ComponentActivationAudioStreamGrantV1& grant,
    std::span<const std::uint8_t> sourceBytes) noexcept
{
    const auto mappedBytes = pageRoundedAudioSourceBytes(
        grant.sourceByteCount);
    if (!mappedBytes.has_value()
        || !exactAudioSourceBytes(sourceBytes, grant))
    {
        return nullptr;
    }
    auto* mapping = mmap(
        nullptr,
        *mappedBytes,
        PROT_READ | PROT_WRITE,
        MAP_ANON | MAP_PRIVATE,
        -1,
        0);
    if (mapping == MAP_FAILED)
        return nullptr;
    ScopedMapping owner{mapping, *mappedBytes};
    std::memcpy(mapping, sourceBytes.data(), sourceBytes.size());
    if (mprotect(mapping, *mappedBytes, PROT_READ) != 0)
        return nullptr;
    return xpc_shmem_create(mapping, *mappedBytes);
}

ComponentActivationXpcEnvelopeDiagnosticV1
materializeComponentActivationPrivateAudioSourceV1(
    xpc_object_t sharedMemory,
    const ComponentActivationAudioStreamGrantV1& grant,
    ComponentActivationPrivateAudioSourceV1& source) noexcept
{
    source = {};
    if (sharedMemory == nullptr)
        return ComponentActivationXpcEnvelopeDiagnosticV1::AudioSourceMissing;
    if (xpc_get_type(sharedMemory) != XPC_TYPE_SHMEM)
        return ComponentActivationXpcEnvelopeDiagnosticV1::AudioSourceInvalid;
    const auto expectedMappedBytes = pageRoundedAudioSourceBytes(
        grant.sourceByteCount);
    if (!expectedMappedBytes.has_value())
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            AudioSourceSizeInvalid;
    }

    void* incomingMapping = nullptr;
    const auto incomingBytes = xpc_shmem_map(
        sharedMemory, &incomingMapping);
    ScopedMapping incoming{incomingMapping, incomingBytes};
    if (incomingMapping == nullptr || incomingMapping == MAP_FAILED
        || incomingBytes != *expectedMappedBytes)
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            AudioSourceSizeInvalid;
    }
    const auto* incomingData = static_cast<const std::uint8_t*>(
        incomingMapping);
    const auto declaredBytes = static_cast<std::size_t>(
        grant.sourceByteCount);
    if (!std::all_of(
            incomingData + declaredBytes,
            incomingData + incomingBytes,
            [](std::uint8_t value) { return value == 0; }))
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::AudioSourceInvalid;
    }
    const std::span<const std::uint8_t> incomingSource{
        incomingData, declaredBytes};
    if (calculateComponentActivationByteDigestV1(incomingSource)
        != grant.sourceContentDigest)
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            AudioSourceDigestMismatch;
    }
    const auto incomingShape = parseComponentActivationPcm16WaveV1(
        incomingSource);
    if (!incomingShape.has_value()
        || incomingShape->channelCount != grant.sourceChannelCount
        || incomingShape->sampleRate != grant.sourceSampleRate
        || incomingShape->frameCount != grant.sourceFrameCount)
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            AudioSourceMediaInvalid;
    }

    auto* privateMapping = mmap(
        nullptr,
        *expectedMappedBytes,
        PROT_READ | PROT_WRITE,
        MAP_ANON | MAP_PRIVATE,
        -1,
        0);
    if (privateMapping == MAP_FAILED)
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            AudioSourceAllocationFailed;
    }
    ScopedMapping privateOwner{privateMapping, *expectedMappedBytes};
    std::memcpy(
        privateMapping, incomingSource.data(), incomingSource.size());
    const std::span<const std::uint8_t> privateSource{
        static_cast<const std::uint8_t*>(privateMapping),
        incomingSource.size()};
    if (!exactAudioSourceBytes(privateSource, grant))
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            AudioSourceDigestMismatch;
    }
    if (mprotect(privateMapping, *expectedMappedBytes, PROT_READ) != 0)
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            AudioSourceAllocationFailed;
    }
    source.mapping_ = privateOwner.release();
    source.mappedBytes_ = *expectedMappedBytes;
    source.sourceBytes_ = incomingSource.size();
    return ComponentActivationXpcEnvelopeDiagnosticV1::Valid;
}

ComponentActivationXpcEnvelopeDiagnosticV1
materializeComponentActivationResourceSetV1(
    const ComponentActivationXpcLoadPayloadViewV1& payload,
    ComponentActivationPrivateResourceSetV1& resources) noexcept
{
    resources = {};
    if (!payload.resourceSetGrant.has_value()
        || payload.resourceSetMembers.size()
            != payload.resourceSetGrant->memberCount
        || payload.resourceSetMembers.empty()
        || payload.resourceSetMembers.size()
            > maximumComponentActivationResourceSetMembersV1)
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            ResourceSetMembersInvalid;
    }
    ComponentActivationPrivateResourceSetV1 prepared;
    prepared.memberCount_ = payload.resourceSetMembers.size();
    std::size_t audioCount = 0;
    for (std::size_t index = 0;
         index < payload.resourceSetMembers.size();
         ++index)
    {
        const auto& member = payload.resourceSetMembers[index];
        const auto kind = static_cast<
            ComponentActivationResourceSetMemberKindV1>(
            member.member.memberKind);
        if (kind != ComponentActivationResourceSetMemberKindV1::Audio)
            continue;
        if (!member.audioGrant.has_value()
            || member.audioSourceSharedMemory == nullptr)
        {
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceSetPayloadMissing;
        }
        const auto diagnostic =
            materializeComponentActivationPrivateAudioSourceV1(
                member.audioSourceSharedMemory,
                *member.audioGrant,
                prepared.audioSources_[index]);
        if (diagnostic != ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            || prepared.audioSources_[index].mappedByteCount()
                > member.member.privateMappedByteCount)
        {
            return diagnostic
                    == ComponentActivationXpcEnvelopeDiagnosticV1::Valid
                ? ComponentActivationXpcEnvelopeDiagnosticV1::
                      ResourceSetMaterializationFailed
                : diagnostic;
        }
        prepared.audioMembers_[index] = true;
        ++audioCount;
    }
    if (audioCount != payload.resourceSetGrant->audioStreamCount)
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            ResourceSetMembersInvalid;
    }
    prepared.valid_ = true;
    resources = std::move(prepared);
    return ComponentActivationXpcEnvelopeDiagnosticV1::Valid;
}

xpc_object_t createComponentActivationXpcResourceSetV1(
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session,
    std::span<const ComponentActivationStaticParameterV1> staticParameters,
    const ComponentActivationResourceSetGrantV1& resourceSetGrant,
    std::span<const ComponentActivationResourceSetMemberV1>
        resourceSetMembers,
    std::span<const ComponentActivationXpcResourceSetSourceV1>
        resourceSetSources) noexcept
{
    if (!exactResourceSetPayload(
            &resourceSetGrant,
            resourceSetMembers,
            resourceSetSources,
            authorization,
            session,
            staticParameters))
    {
        return nullptr;
    }
    auto object = xpc_dictionary_create_empty();
    auto members = xpc_array_create_empty();
    if (object == nullptr || members == nullptr)
    {
        if (object != nullptr)
            xpc_release(object);
        if (members != nullptr)
            xpc_release(members);
        return nullptr;
    }
    for (std::size_t index = 0; index < resourceSetMembers.size(); ++index)
    {
        auto member = createResourceSetMemberObject(
            resourceSetMembers[index], resourceSetSources[index]);
        if (member == nullptr)
        {
            xpc_release(members);
            xpc_release(object);
            return nullptr;
        }
        xpc_array_append_value(members, member);
        xpc_release(member);
    }
    xpc_dictionary_set_data(
        object,
        resourceSetGrantKey,
        &resourceSetGrant,
        sizeof(resourceSetGrant));
    xpc_dictionary_set_value(object, resourceSetMembersKey, members);
    xpc_release(members);
    return object;
}

xpc_object_t createComponentActivationXpcLoadPayloadV1(
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session,
    std::span<const std::uint8_t> canonicalManifest,
    std::span<const std::uint8_t> entryBytes,
    std::span<const ComponentActivationStaticParameterV1> staticParameters,
    xpc_object_t materializationFile,
    const ComponentActivationImmutableResourceGrantV1* resourceGrant,
    std::span<const std::uint8_t> resourceBytes,
    const ComponentActivationAudioStreamGrantV1* audioStreamGrant,
    xpc_object_t audioSourceSharedMemory,
    const ComponentActivationResourceSetGrantV1* resourceSetGrant,
    std::span<const ComponentActivationResourceSetMemberV1>
        resourceSetMembers,
    std::span<const ComponentActivationXpcResourceSetSourceV1>
        resourceSetSources)
    noexcept
{
    if (!exactLoadPayload(
            authorization,
            session,
            canonicalManifest,
            entryBytes,
            staticParameters,
            resourceGrant,
            resourceBytes,
            audioStreamGrant,
            audioSourceSharedMemory,
            resourceSetGrant,
            resourceSetMembers,
            resourceSetSources)
        || materializationFile == nullptr
        || xpc_get_type(materializationFile) != XPC_TYPE_FD)
    {
        return nullptr;
    }
    auto message = xpc_dictionary_create_empty();
    if (message == nullptr)
        return nullptr;
    xpc_dictionary_set_data(
        message, authorizationKey, &authorization, sizeof(authorization));
    xpc_dictionary_set_data(
        message,
        manifestKey,
        canonicalManifest.data(),
        canonicalManifest.size());
    xpc_dictionary_set_data(
        message, entryKey, entryBytes.data(), entryBytes.size());
    static constexpr std::uint8_t emptyData = 0;
    const auto* parameterData = staticParameters.empty()
        ? static_cast<const void*>(&emptyData)
        : static_cast<const void*>(staticParameters.data());
    xpc_dictionary_set_data(
        message,
        parametersKey,
        parameterData,
        staticParameters.size_bytes());
    if (resourceGrant != nullptr)
    {
        xpc_dictionary_set_data(
            message,
            resourceGrantKey,
            resourceGrant,
            sizeof(*resourceGrant));
        xpc_dictionary_set_data(
            message,
            resourcePayloadKey,
            resourceBytes.data(),
            resourceBytes.size());
    }
    if (audioStreamGrant != nullptr)
    {
        xpc_dictionary_set_data(
            message,
            audioStreamGrantKey,
            audioStreamGrant,
            sizeof(*audioStreamGrant));
        xpc_dictionary_set_value(
            message,
            audioSourceSharedMemoryKey,
            audioSourceSharedMemory);
    }
    if (resourceSetGrant != nullptr)
    {
        auto resourceSet = createComponentActivationXpcResourceSetV1(
            authorization,
            session,
            staticParameters,
            *resourceSetGrant,
            resourceSetMembers,
            resourceSetSources);
        if (resourceSet == nullptr)
        {
            xpc_release(message);
            return nullptr;
        }
        xpc_dictionary_set_value(
            message, resourceSetKey, resourceSet);
        xpc_release(resourceSet);
    }
    xpc_dictionary_set_value(
        message, materializationFileKey, materializationFile);
    return message;
}

ComponentActivationXpcEnvelopeDiagnosticV1
decodeComponentActivationXpcLoadPayloadV1(
    xpc_object_t object,
    const ComponentActivationSealedSessionV1& session,
    ComponentActivationXpcLoadPayloadViewV1& payload) noexcept
{
    payload = {};
    if (object == nullptr || xpc_get_type(object) != XPC_TYPE_DICTIONARY)
        return ComponentActivationXpcEnvelopeDiagnosticV1::ObjectTypeInvalid;
    const auto fieldCount = xpc_dictionary_get_count(object);
    if (fieldCount != 5 && fieldCount != 6 && fieldCount != 7)
        return ComponentActivationXpcEnvelopeDiagnosticV1::FieldCountInvalid;

    std::size_t authorizationBytes = 0;
    const auto* authorizationData = xpc_dictionary_get_data(
        object, authorizationKey, &authorizationBytes);
    if (authorizationData == nullptr)
        return ComponentActivationXpcEnvelopeDiagnosticV1::AuthorizationMissing;
    if (authorizationBytes != sizeof(payload.authorization))
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            AuthorizationSizeInvalid;
    }
    std::memcpy(
        &payload.authorization,
        authorizationData,
        sizeof(payload.authorization));
    if (validateComponentActivationAuthorizationV1(
            payload.authorization, session)
        != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        payload = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::ProtocolInvalid;
    }

    std::size_t manifestBytes = 0;
    const auto* manifestData = static_cast<const std::uint8_t*>(
        xpc_dictionary_get_data(object, manifestKey, &manifestBytes));
    if (manifestData == nullptr)
    {
        payload = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::ManifestMissing;
    }
    if (manifestBytes == 0
        || manifestBytes
            > component_bundle::maximumComponentBundleManifestBytes)
    {
        payload = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::ManifestSizeInvalid;
    }

    std::size_t entryBytes = 0;
    const auto* entryData = static_cast<const std::uint8_t*>(
        xpc_dictionary_get_data(object, entryKey, &entryBytes));
    if (entryData == nullptr)
    {
        payload = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::EntryMissing;
    }
    if (entryBytes == 0
        || entryBytes != payload.authorization.entryByteCount
        || entryBytes > maximumComponentActivationEntryBytesV1)
    {
        payload = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::EntrySizeInvalid;
    }
    const std::span<const std::uint8_t> entry{entryData, entryBytes};
    if (calculateComponentActivationByteDigestV1(entry)
        != payload.authorization.entryDigest)
    {
        payload = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::EntryDigestMismatch;
    }

    auto parameterObject =
        xpc_dictionary_get_value(object, parametersKey);
    if (parameterObject == nullptr)
    {
        payload = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            StaticParametersMissing;
    }
    if (xpc_get_type(parameterObject) != XPC_TYPE_DATA)
    {
        payload = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            StaticParametersSizeInvalid;
    }
    const auto parameterBytes = xpc_data_get_length(parameterObject);
    const auto expectedParameterBytes =
        static_cast<std::size_t>(payload.authorization.staticParameterCount)
        * sizeof(ComponentActivationStaticParameterV1);
    const auto* parameterData = xpc_data_get_bytes_ptr(parameterObject);
    if (parameterBytes != expectedParameterBytes
        || (parameterBytes != 0 && parameterData == nullptr))
    {
        payload = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            StaticParametersSizeInvalid;
    }
    try
    {
        payload.staticParameterStorage.resize(
            payload.authorization.staticParameterCount);
    }
    catch (...)
    {
        payload = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            StaticParametersSizeInvalid;
    }
    if (parameterBytes != 0)
    {
        std::memcpy(
            payload.staticParameterStorage.data(),
            parameterData,
            parameterBytes);
    }
    const std::span<const ComponentActivationStaticParameterV1> parameters{
        payload.staticParameterStorage};
    if (validateComponentActivationStaticParametersV1(parameters)
            != ComponentActivationProtocolDiagnosticV1::Valid
        || calculateComponentActivationStaticParameterDigestV1(parameters)
            != payload.authorization.staticParameterDigest)
    {
        payload = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            StaticParametersDigestMismatch;
    }

    const auto resourceCount = static_cast<std::size_t>(std::count_if(
        parameters.begin(),
        parameters.end(),
        [](const auto& parameter) {
            return parameter.kind == static_cast<std::uint32_t>(
                ComponentActivationStaticParameterKindV1::Resource);
        }));
    const auto immutableProfile =
        (payload.authorization.requestedPermissions
            & componentPermission(
                ComponentExecutionPermission::ImmutableResourceRead))
        != 0;
    const auto audioProfile =
        (payload.authorization.requestedPermissions
            & componentPermission(
                ComponentExecutionPermission::BoundedAudioStreamRead))
        != 0;
    if (resourceCount == 0)
    {
        if (fieldCount != 5 || immutableProfile || audioProfile)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::FieldCountInvalid;
        }
    }
    else if (resourceCount > 1)
    {
        if (fieldCount != 6 || (!immutableProfile && !audioProfile))
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceSetGrantMissing;
        }
        auto resourceSetObject = xpc_dictionary_get_value(
            object, resourceSetKey);
        if (resourceSetObject == nullptr)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceSetGrantMissing;
        }
        if (xpc_get_type(resourceSetObject) != XPC_TYPE_DICTIONARY
            || xpc_dictionary_get_count(resourceSetObject) != 2)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceSetGrantInvalid;
        }
        std::size_t grantBytes = 0;
        const auto* grantData = xpc_dictionary_get_data(
            resourceSetObject, resourceSetGrantKey, &grantBytes);
        if (grantData == nullptr)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceSetGrantMissing;
        }
        if (grantBytes != sizeof(ComponentActivationResourceSetGrantV1))
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceSetGrantSizeInvalid;
        }
        ComponentActivationResourceSetGrantV1 setGrant;
        std::memcpy(&setGrant, grantData, sizeof(setGrant));
        if (validateComponentActivationResourceSetGrantV1(
                setGrant, payload.authorization, session)
            != ComponentActivationProtocolDiagnosticV1::Valid)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceSetGrantInvalid;
        }
        auto memberArray = xpc_dictionary_get_value(
            resourceSetObject, resourceSetMembersKey);
        if (memberArray == nullptr)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceSetMembersMissing;
        }
        if (xpc_get_type(memberArray) != XPC_TYPE_ARRAY
            || xpc_array_get_count(memberArray) != setGrant.memberCount)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceSetMembersInvalid;
        }
        std::vector<ComponentActivationXpcResourceSetSourceV1> sources;
        try
        {
            payload.resourceSetMembers.reserve(setGrant.memberCount);
            sources.reserve(setGrant.memberCount);
        }
        catch (...)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceSetMaterializationFailed;
        }
        for (std::size_t index = 0; index < setGrant.memberCount; ++index)
        {
            auto memberObject = xpc_array_get_value(memberArray, index);
            if (memberObject == nullptr
                || xpc_get_type(memberObject) != XPC_TYPE_DICTIONARY
                || xpc_dictionary_get_count(memberObject) != 3)
            {
                payload = {};
                return ComponentActivationXpcEnvelopeDiagnosticV1::
                    ResourceSetMemberInvalid;
            }
            std::size_t memberBytes = 0;
            const auto* memberData = xpc_dictionary_get_data(
                memberObject, resourceSetMemberKey, &memberBytes);
            if (memberData == nullptr
                || memberBytes != sizeof(ComponentActivationResourceSetMemberV1))
            {
                payload = {};
                return ComponentActivationXpcEnvelopeDiagnosticV1::
                    ResourceSetMemberInvalid;
            }
            ComponentActivationXpcResourceSetMemberViewV1 view;
            std::memcpy(&view.member, memberData, sizeof(view.member));
            ComponentActivationXpcResourceSetSourceV1 source;
            const auto kind = static_cast<
                ComponentActivationResourceSetMemberKindV1>(
                view.member.memberKind);
            if (kind == ComponentActivationResourceSetMemberKindV1::Audio)
            {
                std::size_t audioGrantBytes = 0;
                const auto* audioGrantData = xpc_dictionary_get_data(
                    memberObject, audioStreamGrantKey, &audioGrantBytes);
                auto audioMemory = xpc_dictionary_get_value(
                    memberObject, audioSourceSharedMemoryKey);
                if (audioGrantData == nullptr
                    || audioGrantBytes
                        != sizeof(ComponentActivationAudioStreamGrantV1)
                    || audioMemory == nullptr
                    || xpc_get_type(audioMemory) != XPC_TYPE_SHMEM)
                {
                    payload = {};
                    return ComponentActivationXpcEnvelopeDiagnosticV1::
                        ResourceSetPayloadInvalid;
                }
                ComponentActivationAudioStreamGrantV1 audioGrant;
                std::memcpy(
                    &audioGrant, audioGrantData, sizeof(audioGrant));
                view.audioGrant = audioGrant;
                view.audioSourceSharedMemory = audioMemory;
                source.audioGrant = &*view.audioGrant;
                source.audioSourceSharedMemory = audioMemory;
            }
            else
            {
                std::size_t immutableGrantBytes = 0;
                const auto* immutableGrantData = xpc_dictionary_get_data(
                    memberObject, resourceGrantKey, &immutableGrantBytes);
                std::size_t immutableBytes = 0;
                const auto* immutableData = static_cast<const std::uint8_t*>(
                    xpc_dictionary_get_data(
                        memberObject,
                        resourcePayloadKey,
                        &immutableBytes));
                if (immutableGrantData == nullptr
                    || immutableGrantBytes
                        != sizeof(ComponentActivationImmutableResourceGrantV1)
                    || immutableData == nullptr || immutableBytes == 0)
                {
                    payload = {};
                    return ComponentActivationXpcEnvelopeDiagnosticV1::
                        ResourceSetPayloadInvalid;
                }
                ComponentActivationImmutableResourceGrantV1 immutableGrant;
                std::memcpy(
                    &immutableGrant,
                    immutableGrantData,
                    sizeof(immutableGrant));
                view.immutableGrant = immutableGrant;
                view.immutableBytes = {immutableData, immutableBytes};
                source.immutableGrant = &*view.immutableGrant;
                source.immutableBytes = view.immutableBytes;
            }
            try
            {
                payload.resourceSetMembers.push_back(std::move(view));
                auto& stored = payload.resourceSetMembers.back();
                source.immutableGrant = stored.immutableGrant.has_value()
                    ? &*stored.immutableGrant
                    : nullptr;
                source.audioGrant = stored.audioGrant.has_value()
                    ? &*stored.audioGrant
                    : nullptr;
                sources.push_back(source);
            }
            catch (...)
            {
                payload = {};
                return ComponentActivationXpcEnvelopeDiagnosticV1::
                    ResourceSetMaterializationFailed;
            }
        }
        std::array<
            ComponentActivationResourceSetMemberV1,
            maximumComponentActivationResourceSetMembersV1>
            records{};
        for (std::size_t index = 0;
             index < payload.resourceSetMembers.size();
             ++index)
        {
            records[index] = payload.resourceSetMembers[index].member;
        }
        const auto recordSpan = std::span<
            const ComponentActivationResourceSetMemberV1>{
            records.data(), payload.resourceSetMembers.size()};
        if (!exactResourceSetPayload(
                &setGrant,
                recordSpan,
                sources,
                payload.authorization,
                session,
                parameters))
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceSetMemberInvalid;
        }
        payload.resourceSetGrant = setGrant;
    }
    else if (immutableProfile == audioProfile)
    {
        payload = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::ResourceGrantInvalid;
    }
    else if (immutableProfile)
    {
        if (fieldCount != 7)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::ResourceGrantMissing;
        }
        std::size_t grantBytes = 0;
        const auto* grantData = xpc_dictionary_get_data(
            object, resourceGrantKey, &grantBytes);
        if (grantData == nullptr)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::ResourceGrantMissing;
        }
        if (grantBytes != sizeof(ComponentActivationImmutableResourceGrantV1))
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceGrantSizeInvalid;
        }
        ComponentActivationImmutableResourceGrantV1 grant;
        std::memcpy(&grant, grantData, sizeof(grant));
        if (validateComponentActivationImmutableResourceGrantV1(
                grant,
                payload.authorization,
                session,
                parameters)
            != ComponentActivationProtocolDiagnosticV1::Valid)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourceGrantInvalid;
        }
        std::size_t resourceByteCount = 0;
        const auto* resourceData = static_cast<const std::uint8_t*>(
            xpc_dictionary_get_data(
                object, resourcePayloadKey, &resourceByteCount));
        if (resourceData == nullptr)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourcePayloadMissing;
        }
        if (resourceByteCount == 0
            || resourceByteCount != grant.byteCount
            || resourceByteCount
                > maximumComponentActivationImmutableResourceBytesV1)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourcePayloadSizeInvalid;
        }
        const std::span<const std::uint8_t> resourceBytes{
            resourceData, resourceByteCount};
        if (calculateComponentActivationByteDigestV1(resourceBytes)
            != grant.mediatedPayloadDigest)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                ResourcePayloadDigestMismatch;
        }
        payload.resourceGrant = grant;
        payload.resourceBytes = resourceBytes;
    }
    else
    {
        if (fieldCount != 7)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                AudioStreamGrantMissing;
        }
        std::size_t grantBytes = 0;
        const auto* grantData = xpc_dictionary_get_data(
            object, audioStreamGrantKey, &grantBytes);
        if (grantData == nullptr)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                AudioStreamGrantMissing;
        }
        if (grantBytes != sizeof(ComponentActivationAudioStreamGrantV1))
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                AudioStreamGrantSizeInvalid;
        }
        ComponentActivationAudioStreamGrantV1 grant;
        std::memcpy(&grant, grantData, sizeof(grant));
        if (validateComponentActivationAudioStreamGrantV1(
                grant,
                payload.authorization,
                session,
                parameters)
            != ComponentActivationProtocolDiagnosticV1::Valid)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                AudioStreamGrantInvalid;
        }
        auto sharedMemory = xpc_dictionary_get_value(
            object, audioSourceSharedMemoryKey);
        if (sharedMemory == nullptr)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                AudioSourceMissing;
        }
        if (xpc_get_type(sharedMemory) != XPC_TYPE_SHMEM)
        {
            payload = {};
            return ComponentActivationXpcEnvelopeDiagnosticV1::
                AudioSourceInvalid;
        }
        payload.audioStreamGrant = grant;
        payload.audioSourceSharedMemory = sharedMemory;
    }

    payload.canonicalManifest = {manifestData, manifestBytes};
    payload.entryBytes = entry;
    payload.staticParameters = parameters;
    payload.materializationFile = xpc_dictionary_get_value(
        object, materializationFileKey);
    if (payload.materializationFile == nullptr)
    {
        payload = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            MaterializationFileMissing;
    }
    if (xpc_get_type(payload.materializationFile) != XPC_TYPE_FD)
    {
        payload = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            MaterializationFileInvalid;
    }
    return ComponentActivationXpcEnvelopeDiagnosticV1::Valid;
}

bool writeComponentActivationXpcTransitionV1(
    xpc_object_t dictionary,
    const ComponentActivationTransitionV1& transition,
    ComponentActivationRecordKindV1 expectedKind,
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    if (dictionary == nullptr
        || xpc_get_type(dictionary) != XPC_TYPE_DICTIONARY
        || xpc_dictionary_get_count(dictionary) != 0
        || validateComponentActivationTransitionV1(
               transition, expectedKind, authorization)
            != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        return false;
    }
    xpc_dictionary_set_data(
        dictionary, transitionKey, &transition, sizeof(transition));
    return true;
}

bool writeComponentActivationXpcSealedTransitionV1(
    xpc_object_t dictionary,
    const ComponentActivationTransitionV1& transition,
    ComponentActivationRecordKindV1 expectedKind,
    const ComponentActivationSealedSessionV1& session) noexcept
{
    if (dictionary == nullptr
        || xpc_get_type(dictionary) != XPC_TYPE_DICTIONARY
        || xpc_dictionary_get_count(dictionary) != 0
        || validateComponentActivationSealedTransitionV1(
               transition, expectedKind, session)
            != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        return false;
    }
    xpc_dictionary_set_data(
        dictionary, transitionKey, &transition, sizeof(transition));
    return true;
}

ComponentActivationXpcEnvelopeDiagnosticV1
decodeComponentActivationXpcSealedTransitionV1(
    xpc_object_t object,
    ComponentActivationRecordKindV1 expectedKind,
    const ComponentActivationSealedSessionV1& session,
    ComponentActivationTransitionV1& transition) noexcept
{
    transition = {};
    if (object == nullptr || xpc_get_type(object) != XPC_TYPE_DICTIONARY)
        return ComponentActivationXpcEnvelopeDiagnosticV1::ObjectTypeInvalid;
    if (xpc_dictionary_get_count(object) != 1)
        return ComponentActivationXpcEnvelopeDiagnosticV1::FieldCountInvalid;
    std::size_t bytes = 0;
    const auto* data = xpc_dictionary_get_data(
        object, transitionKey, &bytes);
    if (data == nullptr)
        return ComponentActivationXpcEnvelopeDiagnosticV1::TransitionMissing;
    if (bytes != sizeof(transition))
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            TransitionSizeInvalid;
    }
    std::memcpy(&transition, data, bytes);
    if (validateComponentActivationSealedTransitionV1(
            transition, expectedKind, session)
        != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        transition = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::ProtocolInvalid;
    }
    return ComponentActivationXpcEnvelopeDiagnosticV1::Valid;
}

ComponentActivationXpcEnvelopeDiagnosticV1
decodeComponentActivationXpcTransitionV1(
    xpc_object_t object,
    ComponentActivationRecordKindV1 expectedKind,
    const ComponentActivationAuthorizationV1& authorization,
    ComponentActivationTransitionV1& transition) noexcept
{
    transition = {};
    if (object == nullptr || xpc_get_type(object) != XPC_TYPE_DICTIONARY)
        return ComponentActivationXpcEnvelopeDiagnosticV1::ObjectTypeInvalid;
    if (xpc_dictionary_get_count(object) != 1)
        return ComponentActivationXpcEnvelopeDiagnosticV1::FieldCountInvalid;
    std::size_t bytes = 0;
    const auto* data = xpc_dictionary_get_data(
        object, transitionKey, &bytes);
    if (data == nullptr)
        return ComponentActivationXpcEnvelopeDiagnosticV1::TransitionMissing;
    if (bytes != sizeof(transition))
    {
        return ComponentActivationXpcEnvelopeDiagnosticV1::
            TransitionSizeInvalid;
    }
    std::memcpy(&transition, data, bytes);
    if (validateComponentActivationTransitionV1(
            transition, expectedKind, authorization)
        != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        transition = {};
        return ComponentActivationXpcEnvelopeDiagnosticV1::ProtocolInvalid;
    }
    return ComponentActivationXpcEnvelopeDiagnosticV1::Valid;
}
}
