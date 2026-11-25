#include "runtime/component_activation/macos/ComponentActivationLoader.h"
#include "runtime/KernelAbiCheckpointValidation.h"

#include "runtime/component_activation/ComponentActivationAudioStream.h"
#include "runtime/component_bundle/ComponentBundleInspection.h"
#include "runtime/component_bundle/ComponentBundleManifest.h"
#include "runtime/component_probe/ComponentBundleMachOValidation.h"
#include "runtime/component_probe/ComponentBundleProbeAbiValidation.h"

#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <os/lock.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace talas::runtime::component_activation
{
namespace
{
[[nodiscard]] bool knownStatus(TalasKernelAbiStatusV1 status) noexcept
{
    return status <= TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
}

[[nodiscard]] bool sameFileIdentity(
    const struct stat& left,
    const struct stat& right) noexcept
{
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino
        && left.st_uid == right.st_uid && left.st_nlink == right.st_nlink
        && left.st_size == right.st_size
        && (left.st_mode & S_IFMT) == (right.st_mode & S_IFMT);
}

[[nodiscard]] bool digestFile(
    int descriptor,
    std::array<std::uint8_t, componentTrustDigestBytes>& digest) noexcept
{
    CC_SHA256_CTX context{};
    if (CC_SHA256_Init(&context) != 1)
        return false;
    std::array<std::byte, 16U * 1024U> buffer{};
    std::uint64_t offset = 0;
    while (true)
    {
        const auto amount = pread(
            descriptor,
            buffer.data(),
            buffer.size(),
            static_cast<off_t>(offset));
        if (amount < 0 && errno == EINTR)
            continue;
        if (amount < 0)
            return false;
        if (amount == 0)
            break;
        if (amount > std::numeric_limits<CC_LONG>::max()
            || CC_SHA256_Update(
                   &context,
                   buffer.data(),
                   static_cast<CC_LONG>(amount)) != 1)
        {
            return false;
        }
        offset += static_cast<std::uint64_t>(amount);
    }
    return CC_SHA256_Final(digest.data(), &context) == 1;
}

class PrivateMaterializedEntry final
{
public:
    PrivateMaterializedEntry() = default;
    PrivateMaterializedEntry(const PrivateMaterializedEntry&) = delete;
    PrivateMaterializedEntry& operator=(const PrivateMaterializedEntry&) = delete;

    ~PrivateMaterializedEntry()
    {
        static_cast<void>(cleanup());
    }

    [[nodiscard]] bool create(
        int descriptor,
        std::span<const std::uint8_t> bytes,
        const std::array<std::uint8_t, componentTrustDigestBytes>& digest)
        noexcept
    {
        if (fileDescriptor_ >= 0 || descriptor < 0 || bytes.empty())
            return false;
        fileDescriptor_ = descriptor;
        struct stat initialStatus{};
        if (fstat(fileDescriptor_, &initialStatus) != 0
            || !S_ISREG(initialStatus.st_mode)
            || initialStatus.st_uid != getuid()
            || initialStatus.st_nlink != 1
            || initialStatus.st_size != 0
            || (initialStatus.st_mode & 0777) != 0500)
        {
            return false;
        }
        std::size_t written = 0;
        while (written < bytes.size())
        {
            const auto amount = pwrite(
                fileDescriptor_,
                bytes.data() + written,
                bytes.size() - written,
                static_cast<off_t>(written));
            if (amount < 0 && errno == EINTR)
                continue;
            if (amount <= 0)
                return false;
            written += static_cast<std::size_t>(amount);
        }
        if (fsync(fileDescriptor_) != 0)
            return false;
        struct stat beforeDigest{};
        struct stat afterDigest{};
        std::array<std::uint8_t, componentTrustDigestBytes> actualDigest{};
        if (fstat(fileDescriptor_, &beforeDigest) != 0
            || !S_ISREG(beforeDigest.st_mode)
            || beforeDigest.st_uid != getuid()
            || beforeDigest.st_nlink != 1
            || beforeDigest.st_size < 0
            || static_cast<std::uint64_t>(beforeDigest.st_size) != bytes.size()
            || (beforeDigest.st_mode & 0777) != 0500
            || !digestFile(fileDescriptor_, actualDigest)
            || actualDigest != digest
            || fstat(fileDescriptor_, &afterDigest) != 0
            || !sameFileIdentity(beforeDigest, afterDigest))
        {
            return false;
        }
        const auto entryPathBytes = std::snprintf(
            entryPath_.data(),
            entryPath_.size(),
            "/dev/fd/%d",
            fileDescriptor_);
        return entryPathBytes > 0
            && static_cast<std::size_t>(entryPathBytes) < entryPath_.size();
    }

    [[nodiscard]] const char* path() const noexcept
    {
        return entryPath_.data();
    }

    [[nodiscard]] bool cleanup() noexcept
    {
        bool clean = true;
        if (fileDescriptor_ >= 0)
        {
            if (close(fileDescriptor_) != 0)
                clean = false;
            fileDescriptor_ = -1;
        }
        entryPath_.fill(0);
        return clean;
    }

private:
    std::array<char, PATH_MAX> entryPath_{};
    int fileDescriptor_ = -1;
};

class PrivateImmutableResourceMapping final
{
public:
    PrivateImmutableResourceMapping() = default;
    PrivateImmutableResourceMapping(const PrivateImmutableResourceMapping&) = delete;
    PrivateImmutableResourceMapping& operator=(
        const PrivateImmutableResourceMapping&) = delete;

    ~PrivateImmutableResourceMapping()
    {
        static_cast<void>(cleanup());
    }

    [[nodiscard]] bool create(
        std::span<const std::uint8_t> bytes,
        const std::array<std::uint8_t, componentTrustDigestBytes>& digest)
        noexcept
    {
        if (mapping_ != nullptr || bytes.empty()
            || bytes.size()
                > maximumComponentActivationImmutableResourceBytesV1
            || calculateComponentActivationByteDigestV1(bytes) != digest)
        {
            return false;
        }
        const auto pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize <= 0)
            return false;
        const auto pageBytes = static_cast<std::size_t>(pageSize);
        if (bytes.size() > std::numeric_limits<std::size_t>::max()
                - (pageBytes - 1U))
        {
            return false;
        }
        mappingBytes_ = ((bytes.size() + pageBytes - 1U) / pageBytes)
            * pageBytes;
        auto* mapping = mmap(
            nullptr,
            mappingBytes_,
            PROT_READ | PROT_WRITE,
            MAP_ANON | MAP_PRIVATE,
            -1,
            0);
        if (mapping == MAP_FAILED)
        {
            mappingBytes_ = 0;
            return false;
        }
        mapping_ = mapping;
        byteCount_ = bytes.size();
        std::memcpy(mapping_, bytes.data(), bytes.size());
        const auto copied = std::span<const std::uint8_t>{
            static_cast<const std::uint8_t*>(mapping_), byteCount_};
        if (calculateComponentActivationByteDigestV1(copied) != digest
            || mprotect(mapping_, mappingBytes_, PROT_READ) != 0)
        {
            static_cast<void>(cleanup());
            return false;
        }
        return true;
    }

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept
    {
        return mapping_ != nullptr
            ? std::span<const std::uint8_t>{
                  static_cast<const std::uint8_t*>(mapping_), byteCount_}
            : std::span<const std::uint8_t>{};
    }

    [[nodiscard]] bool cleanup() noexcept
    {
        bool clean = true;
        if (mapping_ != nullptr)
        {
            if (munmap(mapping_, mappingBytes_) != 0)
                clean = false;
            mapping_ = nullptr;
        }
        mappingBytes_ = 0;
        byteCount_ = 0;
        return clean;
    }

private:
    void* mapping_ = nullptr;
    std::size_t mappingBytes_ = 0;
    std::size_t byteCount_ = 0;
};

class OwnedDescriptor final
{
public:
    explicit OwnedDescriptor(int descriptor) noexcept
        : descriptor_(descriptor)
    {
    }

    ~OwnedDescriptor()
    {
        if (descriptor_ >= 0)
            static_cast<void>(close(descriptor_));
    }

    [[nodiscard]] int release() noexcept
    {
        const auto value = descriptor_;
        descriptor_ = -1;
        return value;
    }

private:
    int descriptor_ = -1;
};

struct HostAllocationV1
{
    void* pointer = nullptr;
    std::uint64_t bytes = 0;
    std::uint64_t alignment = 0;
};

class BoundedActivationHost;

struct ComponentActivationProcessScopeV3
{
    BoundedActivationHost* host = nullptr;
    std::uint64_t outerSequence = 0;
    std::uint8_t consumedAudioSlots = 0;
};

thread_local BoundedActivationHost* activeAudioPrepareHostV3 = nullptr;
thread_local ComponentActivationProcessScopeV3 activeAudioProcessScopeV3;

template <typename Record>
[[nodiscard]] std::array<std::uint8_t, componentTrustDigestBytes>
activationRecordDigest(const Record& record) noexcept
{
    return calculateComponentActivationByteDigestV1(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(&record),
            sizeof(record)});
}

[[nodiscard]] bool exactPreparedResourceSet(
    const ComponentActivationPreparedResourceSetV1* set,
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session,
    std::span<const ComponentActivationStaticParameterV1> parameters) noexcept
{
    if (set == nullptr || set->grant == nullptr || set->members.empty()
        || set->members.size()
            > maximumComponentActivationResourceSetMembersV1
        || set->members.size() != set->grant->memberCount
        || validateComponentActivationResourceSetGrantV1(
               *set->grant, authorization, session)
            != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        return false;
    }
    std::array<
        ComponentActivationResourceSetMemberV1,
        maximumComponentActivationResourceSetMembersV1>
        records{};
    for (std::size_t index = 0; index < set->members.size(); ++index)
    {
        if (set->members[index].member == nullptr)
            return false;
        records[index] = *set->members[index].member;
    }
    const auto recordSpan = std::span<
        const ComponentActivationResourceSetMemberV1>{
        records.data(), set->members.size()};
    if (validateComponentActivationResourceSetMembersV1(
            *set->grant, recordSpan)
        != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        return false;
    }
    std::size_t memberIndex = 0;
    bool hasImmutableMember = false;
    bool hasAudioMember = false;
    for (const auto& parameter : parameters)
    {
        if (parameter.kind != static_cast<std::uint32_t>(
                ComponentActivationStaticParameterKindV1::Resource))
        {
            continue;
        }
        if (memberIndex >= set->members.size())
            return false;
        const auto& source = set->members[memberIndex];
        const auto& member = *source.member;
        if (std::string_view{parameter.id.data(), parameter.idBytes}
                != std::string_view{
                   member.parameterId.data(), member.parameterIdBytes}
            || std::string_view{parameter.text.data(), parameter.textBytes}
                != std::string_view{
                   member.resourceId.data(), member.resourceIdBytes})
        {
            return false;
        }
        const auto kind = static_cast<
            ComponentActivationResourceSetMemberKindV1>(member.memberKind);
        if (kind == ComponentActivationResourceSetMemberKindV1::Audio)
        {
            hasAudioMember = true;
            if (source.immutableGrant != nullptr
                || !source.immutableBytes.empty()
                || source.audioGrant == nullptr
                || source.audioSourceBytes.empty()
                || validateComponentActivationAudioStreamGrantV1(
                       *source.audioGrant,
                       authorization,
                       session,
                       parameters)
                    != ComponentActivationProtocolDiagnosticV1::Valid
                || activationRecordDigest(*source.audioGrant)
                    != member.memberGrantDigest
                || source.audioGrant->sourceContentDigest
                    != member.sourceContentDigest
                || source.audioGrant->sourceByteCount
                    != member.sourceByteCount
                || source.audioGrant->resourceGeneration
                    != member.resourceGeneration
                || source.audioSourceBytes.size() != member.sourceByteCount
                || calculateComponentActivationByteDigestV1(
                       source.audioSourceBytes)
                    != member.sourceContentDigest)
            {
                return false;
            }
        }
        else
        {
            hasImmutableMember = true;
            if ((kind != ComponentActivationResourceSetMemberKindV1::Table
                    && kind
                        != ComponentActivationResourceSetMemberKindV1::Binary)
                || source.immutableGrant == nullptr
                || source.immutableBytes.empty()
                || source.audioGrant != nullptr
                || !source.audioSourceBytes.empty()
                || validateComponentActivationImmutableResourceGrantV1(
                       *source.immutableGrant,
                       authorization,
                       session,
                       parameters)
                    != ComponentActivationProtocolDiagnosticV1::Valid
                || activationRecordDigest(*source.immutableGrant)
                    != member.memberGrantDigest
                || source.immutableGrant->sourceContentDigest
                    != member.sourceContentDigest
                || source.immutableGrant->mediatedPayloadDigest
                    != member.mediatedPayloadDigest
                || source.immutableGrant->resourceGeneration
                    != member.resourceGeneration
                || source.immutableGrant->byteCount != member.sourceByteCount
                || source.immutableBytes.size() != member.sourceByteCount
                || calculateComponentActivationByteDigestV1(
                       source.immutableBytes)
                    != member.mediatedPayloadDigest)
            {
                return false;
            }
            const auto resourceKind = static_cast<
                ComponentActivationImmutableResourceKindV1>(
                source.immutableGrant->resourceKind);
            if ((kind == ComponentActivationResourceSetMemberKindV1::Table
                    && resourceKind
                        != ComponentActivationImmutableResourceKindV1::Table)
                || (kind
                        == ComponentActivationResourceSetMemberKindV1::Binary
                    && resourceKind
                        != ComponentActivationImmutableResourceKindV1::Binary))
            {
                return false;
            }
        }
        ++memberIndex;
    }
    const auto hasImmutablePermission =
        (authorization.requestedPermissions
            & componentPermission(
                ComponentExecutionPermission::ImmutableResourceRead)) != 0U;
    const auto hasAudioPermission =
        (authorization.requestedPermissions
            & componentPermission(
                ComponentExecutionPermission::BoundedAudioStreamRead)) != 0U;
    return memberIndex == set->members.size()
        && hasImmutableMember == hasImmutablePermission
        && hasAudioMember == hasAudioPermission;
}

class BoundedActivationHost final
{
public:
    BoundedActivationHost()
    {
        initializeServices(
            services_,
            sizeof(services_),
            TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V1);
        initializeServices(
            servicesV2_.v1,
            sizeof(servicesV2_),
            TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V2);
        servicesV2_.resolveImmutableResource = resolveImmutableResource;
        initializeServices(
            servicesV3_.v2.v1,
            sizeof(servicesV3_),
            TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V3);
        servicesV3_.v2.resolveImmutableResource = resolveImmutableResource;
        servicesV3_.resolveImmutableAudioStream =
            resolveImmutableAudioStream;
        servicesV3_.readImmutableAudioStream = readImmutableAudioStream;
        initializeServices(
            servicesV4_.v3.v2.v1,
            sizeof(servicesV4_),
            TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V4);
        servicesV4_.v3.v2.resolveImmutableResource =
            resolveImmutableResource;
        servicesV4_.v3.resolveImmutableAudioStream =
            resolveImmutableAudioStream;
        servicesV4_.v3.readImmutableAudioStream = readImmutableAudioStream;
        bindServiceContexts();
    }

    BoundedActivationHost(const BoundedActivationHost&) = delete;
    BoundedActivationHost& operator=(const BoundedActivationHost&) = delete;

    ~BoundedActivationHost()
    {
        static_cast<void>(finish());
    }

    [[nodiscard]] const TalasKernelAbiHostServicesV1& services() const noexcept
    {
        if (resourceSetBound_)
            return servicesV4_.v3.v2.v1;
        if (audioStreamBound_)
            return servicesV3_.v2.v1;
        return resourceBound_ ? servicesV2_.v1 : services_;
    }

    [[nodiscard]] bool bindImmutableResource(
        const ComponentActivationImmutableResourceGrantV1& grant,
        std::span<const std::uint8_t> bytes) noexcept
    {
        if (resourceBound_ || bytes.empty()
            || bytes.size() != grant.byteCount
            || calculateComponentActivationByteDigestV1(bytes)
                != grant.mediatedPayloadDigest)
        {
            return false;
        }
        TalasKernelAbiImmutableResourceKindV2 viewKind = 0;
        TalasKernelAbiImmutableResourceElementFormatV2 viewElementFormat = 0;
        switch (static_cast<ComponentActivationImmutableResourceKindV1>(
            grant.resourceKind))
        {
            case ComponentActivationImmutableResourceKindV1::Table:
                if (grant.elementFormat
                        != static_cast<std::uint32_t>(
                            ComponentActivationImmutableResourceElementFormatV1::
                                Float64LittleEndian)
                    || grant.byteCount
                        != grant.elementCount * sizeof(double))
                {
                    return false;
                }
                viewKind = TALAS_KERNEL_ABI_RESOURCE_TABLE;
                viewElementFormat =
                    TALAS_KERNEL_ABI_RESOURCE_ELEMENT_F64_LITTLE_ENDIAN;
                break;
            case ComponentActivationImmutableResourceKindV1::Binary:
                if (grant.elementFormat
                        != static_cast<std::uint32_t>(
                            ComponentActivationImmutableResourceElementFormatV1::
                                RawBytes)
                    || grant.byteCount != grant.elementCount
                    || grant.sourceContentDigest
                        != grant.mediatedPayloadDigest)
                {
                    return false;
                }
                viewKind = TALAS_KERNEL_ABI_RESOURCE_BINARY;
                viewElementFormat = TALAS_KERNEL_ABI_RESOURCE_ELEMENT_RAW_BYTES;
                break;
            default:
                return false;
        }
        resourceId_.fill(0);
        std::copy_n(
            grant.resourceId.begin(), grant.resourceIdBytes, resourceId_.begin());
        resourceView_ = {};
        resourceView_.structSize = sizeof(resourceView_);
        resourceView_.kind = viewKind;
        resourceView_.elementFormat = viewElementFormat;
        resourceView_.id = {resourceId_.data(), grant.resourceIdBytes};
        resourceView_.generation = grant.resourceGeneration;
        std::copy(
            grant.sourceContentDigest.begin(),
            grant.sourceContentDigest.end(),
            resourceView_.sourceContentDigest);
        std::copy(
            grant.mediatedPayloadDigest.begin(),
            grant.mediatedPayloadDigest.end(),
            resourceView_.mediatedPayloadDigest);
        resourceView_.bytes = {bytes.data(), bytes.size()};
        resourceView_.elementCount = grant.elementCount;
        resourceBound_ = true;
        return true;
    }

    [[nodiscard]] bool bindAudioStream(
        const ComponentActivationAudioStreamGrantV1& grant,
        std::span<const std::uint8_t> sourceBytes) noexcept
    {
        if (audioStreamBound_ || resourceBound_ || sourceBytes.empty()
            || sourceBytes.size() != grant.sourceByteCount
            || calculateComponentActivationByteDigestV1(sourceBytes)
                != grant.sourceContentDigest)
        {
            return false;
        }
        audioStreamGrant_ = grant;
        audioSourceBytes_ = sourceBytes;
        audioStreamBound_ = true;
        return true;
    }

    [[nodiscard]] bool bindResourceSet(
        const ComponentActivationResourceSetGrantV1& grant,
        std::span<const ComponentActivationPreparedResourceSetMemberV1>
            members) noexcept
    {
        if (resourceSetBound_ || resourceBound_ || audioStreamBound_
            || members.empty() || members.size() != grant.memberCount
            || members.size()
                > maximumComponentActivationResourceSetMembersV1)
        {
            return false;
        }
        std::size_t immutableIndex = 0;
        std::size_t audioIndex = 0;
        for (const auto& source : members)
        {
            if (source.member == nullptr)
                return false;
            const auto kind = static_cast<
                ComponentActivationResourceSetMemberKindV1>(
                source.member->memberKind);
            if (kind == ComponentActivationResourceSetMemberKindV1::Audio)
            {
                if (audioIndex >= setAudioSlots_.size()
                    || source.audioGrant == nullptr
                    || source.audioSourceBytes.empty())
                {
                    return false;
                }
                auto& slot = setAudioSlots_[audioIndex++];
                slot.grant = *source.audioGrant;
                slot.sourceBytes = source.audioSourceBytes;
                slot.healthBinding.resourceSetDigest =
                    grant.resourceSetDigest;
                slot.healthBinding.memberGrantDigest =
                    source.member->memberGrantDigest;
                slot.healthBinding.sourceContentDigest =
                    source.member->sourceContentDigest;
                slot.healthBinding.resourceGeneration =
                    source.member->resourceGeneration;
                slot.healthBinding.memberOrdinal =
                    source.member->memberOrdinal;
                slot.healthBinding.audioSlotOrdinal =
                    static_cast<std::uint16_t>(audioIndex - 1U);
                slot.healthBinding.memberCount = grant.memberCount;
                slot.bound = true;
                continue;
            }
            if (immutableIndex >= setImmutableSlots_.size()
                || source.immutableGrant == nullptr
                || source.immutableBytes.empty())
            {
                return false;
            }
            auto& slot = setImmutableSlots_[immutableIndex++];
            const auto& immutable = *source.immutableGrant;
            TalasKernelAbiImmutableResourceKindV2 viewKind = 0;
            TalasKernelAbiImmutableResourceElementFormatV2 viewElementFormat = 0;
            if (kind == ComponentActivationResourceSetMemberKindV1::Table)
            {
                viewKind = TALAS_KERNEL_ABI_RESOURCE_TABLE;
                viewElementFormat =
                    TALAS_KERNEL_ABI_RESOURCE_ELEMENT_F64_LITTLE_ENDIAN;
            }
            else if (kind
                == ComponentActivationResourceSetMemberKindV1::Binary)
            {
                viewKind = TALAS_KERNEL_ABI_RESOURCE_BINARY;
                viewElementFormat = TALAS_KERNEL_ABI_RESOURCE_ELEMENT_RAW_BYTES;
            }
            else
            {
                return false;
            }
            std::copy_n(
                immutable.resourceId.begin(),
                immutable.resourceIdBytes,
                slot.resourceId.begin());
            slot.view.structSize = sizeof(slot.view);
            slot.view.kind = viewKind;
            slot.view.elementFormat = viewElementFormat;
            slot.view.id = {
                slot.resourceId.data(), immutable.resourceIdBytes};
            slot.view.generation = immutable.resourceGeneration;
            std::copy(
                immutable.sourceContentDigest.begin(),
                immutable.sourceContentDigest.end(),
                slot.view.sourceContentDigest);
            std::copy(
                immutable.mediatedPayloadDigest.begin(),
                immutable.mediatedPayloadDigest.end(),
                slot.view.mediatedPayloadDigest);
            slot.view.bytes = {
                source.immutableBytes.data(), source.immutableBytes.size()};
            slot.view.elementCount = immutable.elementCount;
            slot.memberOrdinal = source.member->memberOrdinal;
            slot.bound = true;
        }
        if (immutableIndex + audioIndex != members.size()
            || audioIndex != grant.audioStreamCount)
        {
            return false;
        }
        resourceSetDigest_ = grant.resourceSetDigest;
        resourceSetMemberCount_ = grant.memberCount;
        setImmutableSlotCount_ = immutableIndex;
        setAudioSlotCount_ = audioIndex;
        resourceSetBound_ = true;
        return true;
    }

    [[nodiscard]] bool beginPrepare() noexcept
    {
        if (resourceSetBound_)
        {
            if (setAudioSlotCount_ != 0
                && activeAudioPrepareHostV3 != nullptr)
            {
                audioStreamViolation_.store(true, std::memory_order_release);
                return false;
            }
            for (std::size_t index = 0;
                 index < setImmutableSlotCount_;
                 ++index)
            {
                setImmutableSlots_[index].resolved.store(
                    false, std::memory_order_release);
            }
            for (std::size_t index = 0;
                 index < setAudioSlotCount_;
                 ++index)
            {
                auto& slot = setAudioSlots_[index];
                slot.resolved.store(false, std::memory_order_release);
                slot.phase.store(
                    AudioStreamPhase::Preparing,
                    std::memory_order_release);
            }
            resourcePhase_.store(
                ResourcePhase::Preparing, std::memory_order_release);
            if (setAudioSlotCount_ != 0)
                activeAudioPrepareHostV3 = this;
            return !resourceViolation_.load(std::memory_order_acquire)
                && !audioStreamViolation_.load(std::memory_order_acquire);
        }
        resourceResolved_.store(false, std::memory_order_release);
        resourcePhase_.store(
            ResourcePhase::Preparing, std::memory_order_release);
        if (audioStreamBound_)
        {
            if (activeAudioPrepareHostV3 != nullptr)
            {
                audioStreamViolation_.store(true, std::memory_order_release);
                return false;
            }
            audioStreamResolved_.store(false, std::memory_order_release);
            audioStreamPhase_.store(
                AudioStreamPhase::Preparing, std::memory_order_release);
            activeAudioPrepareHostV3 = this;
        }
        return !resourceViolation_.load(std::memory_order_acquire)
            && !audioStreamViolation_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool finishPrepare(bool succeeded) noexcept
    {
        if (resourceSetBound_)
        {
            if (setAudioSlotCount_ != 0)
            {
                if (activeAudioPrepareHostV3 != this)
                {
                    audioStreamViolation_.store(
                        true, std::memory_order_release);
                }
                activeAudioPrepareHostV3 = nullptr;
            }
            for (std::size_t index = 0;
                 index < setImmutableSlotCount_;
                 ++index)
            {
                if (succeeded
                    && !setImmutableSlots_[index].resolved.load(
                        std::memory_order_acquire))
                {
                    resourceViolation_.store(
                        true, std::memory_order_release);
                }
            }
            for (std::size_t index = 0;
                 index < setAudioSlotCount_;
                 ++index)
            {
                auto& slot = setAudioSlots_[index];
                slot.phase.store(
                    succeeded && slot.stream != nullptr
                        ? AudioStreamPhase::Ready
                        : AudioStreamPhase::Closed,
                    std::memory_order_release);
                if (succeeded
                    && !slot.resolved.load(std::memory_order_acquire))
                {
                    audioStreamViolation_.store(
                        true, std::memory_order_release);
                }
            }
            resourcePhase_.store(
                ResourcePhase::Closed, std::memory_order_release);
            return !resourceViolation_.load(std::memory_order_acquire)
                && !audioStreamViolation_.load(std::memory_order_acquire);
        }
        if (audioStreamBound_)
        {
            if (activeAudioPrepareHostV3 != this)
                audioStreamViolation_.store(true, std::memory_order_release);
            activeAudioPrepareHostV3 = nullptr;
            audioStreamPhase_.store(
                succeeded && audioStream_ != nullptr
                    ? AudioStreamPhase::Ready
                    : AudioStreamPhase::Closed,
                std::memory_order_release);
            if (succeeded
                && !audioStreamResolved_.load(std::memory_order_acquire))
            {
                audioStreamViolation_.store(true, std::memory_order_release);
            }
        }
        resourcePhase_.store(ResourcePhase::Closed, std::memory_order_release);
        if (succeeded && resourceBound_
            && !resourceResolved_.load(std::memory_order_acquire))
        {
            resourceViolation_.store(true, std::memory_order_release);
        }
        return !resourceViolation_.load(std::memory_order_acquire)
            && !audioStreamViolation_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool beginProcess(std::uint64_t outerSequence) noexcept
    {
        if (resourceSetBound_ && setAudioSlotCount_ != 0)
        {
            auto exact = outerSequence != 0U
                && activeAudioProcessScopeV3.host == nullptr;
            for (std::size_t index = 0;
                 exact && index < setAudioSlotCount_;
                 ++index)
            {
                exact = setAudioSlots_[index].stream != nullptr
                    && setAudioSlots_[index].phase.load(
                        std::memory_order_acquire)
                        == AudioStreamPhase::Ready;
            }
            if (!exact)
            {
                audioStreamViolation_.store(
                    true, std::memory_order_release);
                return false;
            }
            activeAudioProcessScopeV3 = {this, outerSequence, 0};
            for (std::size_t index = 0;
                 index < setAudioSlotCount_;
                 ++index)
            {
                setAudioSlots_[index].phase.store(
                    AudioStreamPhase::Processing,
                    std::memory_order_release);
            }
            return true;
        }
        if (!audioStreamBound_)
            return true;
        if (outerSequence == 0U
            || activeAudioProcessScopeV3.host != nullptr
            || audioStreamPhase_.load(std::memory_order_acquire)
                != AudioStreamPhase::Ready
            || audioStream_ == nullptr)
        {
            audioStreamViolation_.store(true, std::memory_order_release);
            return false;
        }
        activeAudioProcessScopeV3 = {this, outerSequence, 0};
        audioStreamPhase_.store(
            AudioStreamPhase::Processing, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool finishProcess() noexcept
    {
        if (resourceSetBound_ && setAudioSlotCount_ != 0)
        {
            if (activeAudioProcessScopeV3.host != this
                || activeAudioProcessScopeV3.outerSequence == 0U)
            {
                audioStreamViolation_.store(
                    true, std::memory_order_release);
            }
            activeAudioProcessScopeV3 = {};
            for (std::size_t index = 0;
                 index < setAudioSlotCount_;
                 ++index)
            {
                setAudioSlots_[index].phase.store(
                    AudioStreamPhase::Ready, std::memory_order_release);
            }
            return !audioStreamViolation_.load(std::memory_order_acquire);
        }
        if (!audioStreamBound_)
            return true;
        if (activeAudioProcessScopeV3.host != this
            || activeAudioProcessScopeV3.outerSequence == 0U)
        {
            audioStreamViolation_.store(true, std::memory_order_release);
        }
        activeAudioProcessScopeV3 = {};
        audioStreamPhase_.store(
            AudioStreamPhase::Ready, std::memory_order_release);
        return !audioStreamViolation_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool replaceAudioStreamEpochAndPrewarm(
        double initialSourceFrame) noexcept
    {
        if (resourceSetBound_)
        {
            for (std::size_t index = 0;
                 index < setAudioSlotCount_;
                 ++index)
            {
                auto& slot = setAudioSlots_[index];
                if (slot.stream == nullptr
                    || slot.phase.load(std::memory_order_acquire)
                        != AudioStreamPhase::Ready
                    || !slot.stream->replaceEpochAndPrewarm(
                        initialSourceFrame,
                        std::chrono::milliseconds(250)))
                {
                    audioStreamViolation_.store(
                        true, std::memory_order_release);
                    return false;
                }
            }
            return true;
        }
        if (!audioStreamBound_)
            return true;
        return audioStream_ != nullptr
            && audioStreamPhase_.load(std::memory_order_acquire)
                == AudioStreamPhase::Ready
            && audioStream_->replaceEpochAndPrewarm(
                initialSourceFrame, std::chrono::milliseconds(250));
    }

    [[nodiscard]] auto checkpointStreams() const noexcept
    {
        std::array<ComponentActivationAudioStreamV1*, maximumComponentActivationResourceSetAudioStreamsV1> streams{};
        if (resourceSetBound_)
            for (std::size_t i = 0; i < setAudioSlotCount_; ++i) streams[i] = setAudioSlots_[i].stream.get();
        else if (audioStreamBound_) streams[0] = audioStream_.get();
        return streams;
    }

    [[nodiscard]] bool resourceStreamsHealthy() const noexcept
    {
        if (!valid()) return false;
        for (auto* stream : checkpointStreams())
        {
            if (!stream) continue;
            const auto health = stream->processingHealth();
            if (health.decodeFailures != 0
                || health.workerState == ComponentActivationAudioStreamWorkerStateV1::Failed
                || health.workerState == ComponentActivationAudioStreamWorkerStateV1::Stopped) return false;
        }
        return true;
    }

    [[nodiscard]] bool beginResourceCheckpoint() noexcept
    {
        std::lock_guard lock(resourceCheckpointInspectionMutex_);
        if (resourceCheckpointActive_ || !resourceStreamsHealthy()) return false;
        if (resourceSetBound_)
        {
            for (std::size_t i = 0; i < setAudioSlotCount_; ++i)
                if (!setAudioSlots_[i].stream || setAudioSlots_[i].phase.load(std::memory_order_acquire) != AudioStreamPhase::Ready)
                    return false;
        }
        else if (audioStreamBound_ && (!audioStream_ || audioStreamPhase_.load(std::memory_order_acquire) != AudioStreamPhase::Ready))
            return false;
        const auto streams = checkpointStreams();
        for (std::size_t i = 0; i < streams.size(); ++i)
        {
            if (!streams[i] || streams[i]->beginOfflineCheckpoint(true)) continue;
            bool clean = true;
            for (std::size_t j = 0; j < i; ++j)
                if (streams[j] && !streams[j]->restoreOfflineCheckpoint()) clean = false;
            if (!clean) audioStreamViolation_.store(true, std::memory_order_release);
            return false;
        }
        checkpointAudioHealth_ = currentAudioStreamHealth(true);
        checkpointSetHealth_ = currentAudioStreamHealthSet(true);
        resourceCheckpointActive_ = true;
        return true;
    }

    [[nodiscard]] bool finishResourceCheckpoint(bool restore) noexcept
    {
        std::lock_guard lock(resourceCheckpointInspectionMutex_);
        if (!resourceCheckpointActive_) return false;
        bool clean = resourceStreamsHealthy();
        const bool mayRestore = restore && clean;
        for (auto* stream : checkpointStreams())
            if (stream && !(mayRestore ? stream->restoreOfflineCheckpoint() : stream->commitOfflineCheckpoint())) clean = false;
        resourceCheckpointActive_ = false;
        checkpointAudioHealth_.reset();
        checkpointSetHealth_.reset();
        if (!clean) audioStreamViolation_.store(true, std::memory_order_release);
        return clean;
    }

    [[nodiscard]] std::optional<ComponentActivationAudioStreamHealthV1>
    currentAudioStreamHealth(bool captured = false) const noexcept
    {
        return audioStream_ != nullptr
            ? std::optional<ComponentActivationAudioStreamHealthV1>{
                  captured ? audioStream_->health() : audioStream_->processingHealth()}
            : std::nullopt;
    }

    [[nodiscard]] std::optional<ComponentActivationAudioStreamHealthSetV1>
    currentAudioStreamHealthSet(bool captured = false) const noexcept
    {
        if (!resourceSetBound_ || setAudioSlotCount_ == 0)
            return std::nullopt;
        ComponentActivationAudioStreamHealthSetV1 result;
        result.binding.resourceSetDigest = resourceSetDigest_;
        result.binding.memberCount = resourceSetMemberCount_;
        result.binding.slotCount =
            static_cast<std::uint16_t>(setAudioSlotCount_);
        result.slotCount = static_cast<std::uint16_t>(setAudioSlotCount_);
        for (std::size_t index = 0;
             index < setAudioSlotCount_;
             ++index)
        {
            if (setAudioSlots_[index].stream == nullptr)
                return std::nullopt;
            result.binding.slots[index] =
                setAudioSlots_[index].healthBinding;
            result.slots[index].binding =
                setAudioSlots_[index].healthBinding;
            result.slots[index].health =
                captured ? setAudioSlots_[index].stream->health() : setAudioSlots_[index].stream->processingHealth();
        }
        return result;
    }

    [[nodiscard]] std::optional<ComponentActivationAudioStreamHealthV1>
    audioStreamHealth() const noexcept
    {
        std::lock_guard lock(resourceCheckpointInspectionMutex_);
        return resourceCheckpointActive_ && resourceStreamsHealthy()
            ? checkpointAudioHealth_ : currentAudioStreamHealth();
    }

    [[nodiscard]] std::optional<ComponentActivationAudioStreamHealthSetV1>
    audioStreamHealthSet() const noexcept
    {
        std::lock_guard lock(resourceCheckpointInspectionMutex_);
        return resourceCheckpointActive_ && resourceStreamsHealthy()
            ? checkpointSetHealth_ : currentAudioStreamHealthSet();
    }

#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
    [[nodiscard]] std::array<
        TalasKernelAbiAudioStreamTokenV3,
        maximumComponentActivationResourceSetAudioStreamsV1>
    audioStreamTokensForTesting() const noexcept
    {
        std::array<
            TalasKernelAbiAudioStreamTokenV3,
            maximumComponentActivationResourceSetAudioStreamsV1>
            result{};
        for (std::size_t index = 0; index < setAudioSlotCount_; ++index)
        {
            if (setAudioSlots_[index].stream != nullptr)
                result[index] = setAudioSlots_[index].stream->view().streamToken;
        }
        return result;
    }

    void setAudioStreamHealthCountersForTest(
        std::uint64_t value,
        std::optional<std::size_t> resourceSetAudioSlot) noexcept
    {
        if (resourceSetBound_)
        {
            if (resourceSetAudioSlot.has_value())
            {
                if (*resourceSetAudioSlot < setAudioSlotCount_
                    && setAudioSlots_[*resourceSetAudioSlot].stream != nullptr)
                {
                    setAudioSlots_[*resourceSetAudioSlot]
                        .stream->setDecodeFailuresForTest(value);
                }
                return;
            }
            for (std::size_t index = 0;
                 index < setAudioSlotCount_;
                 ++index)
            {
                if (setAudioSlots_[index].stream != nullptr)
                {
                    setAudioSlots_[index].stream->setDecodeFailuresForTest(
                        value);
                }
            }
            return;
        }
        if (audioStream_ != nullptr)
            audioStream_->setDecodeFailuresForTest(value);
    }
#endif

    void stopAudioStream() noexcept
    {
        std::lock_guard lock(resourceCheckpointInspectionMutex_);
        resourceCheckpointActive_ = false;
        checkpointAudioHealth_.reset();
        checkpointSetHealth_.reset();
        if (resourceSetBound_)
        {
            for (std::size_t index = 0;
                 index < setAudioSlotCount_;
                 ++index)
            {
                auto& slot = setAudioSlots_[index];
                slot.phase.store(
                    AudioStreamPhase::Closed, std::memory_order_release);
                if (slot.stream != nullptr)
                    slot.stream->shutdown();
                slot.stream.reset();
            }
        }
        audioStreamPhase_.store(
            AudioStreamPhase::Closed, std::memory_order_release);
        if (activeAudioPrepareHostV3 == this)
            activeAudioPrepareHostV3 = nullptr;
        if (activeAudioProcessScopeV3.host == this)
            activeAudioProcessScopeV3 = {};
        if (audioStream_ != nullptr)
            audioStream_->shutdown();
        audioStream_.reset();
    }

    void setRealtimeCallbackActive(bool active) noexcept
    {
        realtimeCallbackActive_.store(active, std::memory_order_release);
    }

    [[nodiscard]] bool beginCheckpointCapture() noexcept
    {
        os_unfair_lock_lock(&lock_);
        const bool allowed = valid() && !checkpointActive_
            && !realtimeCallbackActive_.load(std::memory_order_acquire);
        if (allowed)
        {
            checkpointActive_ = true;
            checkpointCapturing_ = true;
            checkpointAllocationCalls_ = 0;
            checkpointAllocationBytes_ = 0;
        }
        os_unfair_lock_unlock(&lock_);
        return allowed;
    }

    [[nodiscard]] bool finishCheckpointCapture() noexcept
    {
        os_unfair_lock_lock(&lock_);
        checkpointCapturing_ = false;
        os_unfair_lock_unlock(&lock_);
        return valid();
    }

    void beginCheckpointDestroy() noexcept
    {
        os_unfair_lock_lock(&lock_);
        checkpointCapturing_ = false;
        checkpointDestroying_ = true;
        os_unfair_lock_unlock(&lock_);
    }

    [[nodiscard]] bool finishCheckpointDestroy() noexcept
    {
        os_unfair_lock_lock(&lock_);
        for (auto& allocation : checkpointAllocations_)
        {
            if (allocation.pointer != nullptr)
            {
                violation_ = true;
                violationObserved_.store(true, std::memory_order_release);
                std::free(allocation.pointer);
                allocation = {};
            }
        }
        checkpointActive_ = checkpointCapturing_ = checkpointDestroying_ = false;
        os_unfair_lock_unlock(&lock_);
        return valid();
    }
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
    void failNextCheckpointAllocationForTesting() noexcept
    {
        failCheckpointAllocation_.store(true, std::memory_order_release);
    }
#endif

    [[nodiscard]] bool valid() const noexcept
    {
        return !violationObserved_.load(std::memory_order_acquire)
            && !resourceViolation_.load(std::memory_order_acquire)
            && !audioStreamViolation_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool finish() noexcept
    {
        stopAudioStream();
        os_unfair_lock_lock(&lock_);
        bool result = !violation_
            && !violationObserved_.load(std::memory_order_acquire)
            && !resourceViolation_.load(std::memory_order_acquire)
            && !audioStreamViolation_.load(std::memory_order_acquire);
        for (auto* domain : {&allocations_, &checkpointAllocations_})
        for (auto& allocation : *domain)
        {
            if (allocation.pointer != nullptr)
            {
                result = false;
                std::free(allocation.pointer);
                allocation = {};
            }
        }
        os_unfair_lock_unlock(&lock_);
        return result;
    }

private:
    enum class ResourcePhase : std::uint8_t
    {
        Idle,
        Preparing,
        Closed,
    };

    enum class AudioStreamPhase : std::uint8_t
    {
        Idle,
        Preparing,
        Ready,
        Processing,
        Closed,
    };

    struct SetImmutableSlot
    {
        TalasKernelAbiImmutableResourceViewV2 view{};
        std::array<char, maximumComponentActivationDescriptorIdBytesV1>
            resourceId{};
        std::atomic<bool> resolved{false};
        std::uint16_t memberOrdinal = 0;
        bool bound = false;
    };

    struct SetAudioSlot
    {
        ComponentActivationAudioStreamGrantV1 grant{};
        std::span<const std::uint8_t> sourceBytes;
        std::unique_ptr<ComponentActivationAudioStreamV1> stream;
        std::atomic<AudioStreamPhase> phase{AudioStreamPhase::Idle};
        std::atomic<bool> resolved{false};
        ComponentActivationAudioStreamHealthSlotBindingV2 healthBinding;
        bool bound = false;
    };

    static constexpr std::uint64_t maximumAllocations = 64;
    static constexpr std::uint64_t maximumAllocationBytes = 1024U * 1024U;
    static constexpr std::uint64_t maximumDiagnostics = 32;
    static constexpr std::uint64_t maximumDiagnosticBytes = 4096;

    static void initializeServices(
        TalasKernelAbiHostServicesV1& services,
        std::uint32_t structSize,
        std::uint32_t version) noexcept
    {
        services = {};
        services.structSize = structSize;
        services.servicesVersion = version;
        services.hostContext = nullptr;
        services.allocate = allocate;
        services.deallocate = deallocate;
        services.diagnostic = diagnostic;
    }

    void bindServiceContexts() noexcept
    {
        services_.hostContext = this;
        servicesV2_.v1.hostContext = this;
        servicesV3_.v2.v1.hostContext = this;
        servicesV4_.v3.v2.v1.hostContext = this;
    }

    [[nodiscard]] static bool validAlignment(std::uint64_t value) noexcept
    {
        return value != 0 && std::has_single_bit(value)
            && value <= maximumAllocationBytes;
    }

    [[nodiscard]] static TalasKernelAbiStatusV1 resolveImmutableResource(
        void* context,
        TalasKernelAbiStringViewV1 id,
        TalasKernelAbiImmutableResourceViewV2* resource) noexcept
    {
        auto* self = static_cast<BoundedActivationHost*>(context);
        if (self == nullptr || resource == nullptr)
            return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
        if (self->resourceSetBound_)
        {
            if (self->realtimeCallbackActive_.load(
                    std::memory_order_acquire)
                || self->resourcePhase_.load(std::memory_order_acquire)
                    != ResourcePhase::Preparing
                || id.data == nullptr || id.byteCount == 0)
            {
                self->resourceViolation_.store(
                    true, std::memory_order_release);
                return TALAS_KERNEL_ABI_STATUS_NOT_PREPARED;
            }
            SetImmutableSlot* matched = nullptr;
            for (std::size_t index = 0;
                 index < self->setImmutableSlotCount_;
                 ++index)
            {
                auto& candidate = self->setImmutableSlots_[index];
                if (candidate.view.id.byteCount == id.byteCount
                    && std::memcmp(
                        candidate.view.id.data,
                        id.data,
                        static_cast<std::size_t>(id.byteCount)) == 0)
                {
                    if (matched != nullptr)
                    {
                        self->resourceViolation_.store(
                            true, std::memory_order_release);
                        return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
                    }
                    matched = &candidate;
                }
            }
            bool expected = false;
            if (matched == nullptr
                || !matched->resolved.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                self->resourceViolation_.store(
                    true, std::memory_order_release);
                return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
            }
            *resource = matched->view;
            return TALAS_KERNEL_ABI_STATUS_OK;
        }
        if (self->realtimeCallbackActive_.load(std::memory_order_acquire)
            || !self->resourceBound_
            || self->resourcePhase_.load(std::memory_order_acquire)
                != ResourcePhase::Preparing)
        {
            self->resourceViolation_.store(true, std::memory_order_release);
            return TALAS_KERNEL_ABI_STATUS_NOT_PREPARED;
        }
        if (id.data == nullptr
            || id.byteCount != self->resourceView_.id.byteCount
            || std::memcmp(
                   id.data,
                   self->resourceView_.id.data,
                   static_cast<std::size_t>(id.byteCount)) != 0)
        {
            self->resourceViolation_.store(true, std::memory_order_release);
            return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
        }
        bool expected = false;
        if (!self->resourceResolved_.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            self->resourceViolation_.store(true, std::memory_order_release);
            return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
        }
        *resource = self->resourceView_;
        return TALAS_KERNEL_ABI_STATUS_OK;
    }

    [[nodiscard]] static TalasKernelAbiStatusV1 resolveImmutableAudioStream(
        void* context,
        const TalasKernelAbiImmutableAudioStreamPrepareV3* prepare,
        TalasKernelAbiImmutableAudioStreamViewV3* stream) noexcept
    {
        auto* self = static_cast<BoundedActivationHost*>(context);
        if (self == nullptr || prepare == nullptr || stream == nullptr)
            return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
        if (self->resourceSetBound_)
        {
            if (activeAudioPrepareHostV3 != self
                || prepare->id.data == nullptr
                || prepare->id.byteCount == 0)
            {
                self->audioStreamViolation_.store(
                    true, std::memory_order_release);
                return TALAS_KERNEL_ABI_STATUS_NOT_PREPARED;
            }
            SetAudioSlot* matched = nullptr;
            std::size_t matchedIndex = self->setAudioSlotCount_;
            for (std::size_t index = 0;
                 index < self->setAudioSlotCount_;
                 ++index)
            {
                auto& candidate = self->setAudioSlots_[index];
                if (candidate.phase.load(std::memory_order_acquire)
                        != AudioStreamPhase::Preparing)
                {
                    continue;
                }
                if (candidate.grant.resourceIdBytes == prepare->id.byteCount
                    && std::memcmp(
                        candidate.grant.resourceId.data(),
                        prepare->id.data,
                        static_cast<std::size_t>(prepare->id.byteCount)) == 0)
                {
                    if (matched != nullptr)
                    {
                        self->audioStreamViolation_.store(
                            true, std::memory_order_release);
                        return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
                    }
                    matched = &candidate;
                    matchedIndex = index;
                }
            }
            bool expected = false;
            if (matched == nullptr
                || !matched->resolved.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                self->audioStreamViolation_.store(
                    true, std::memory_order_release);
                return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
            }
            auto prepared = prepareComponentActivationAudioStreamV1(
                matched->sourceBytes, matched->grant, *prepare);
            if (!prepared.ok()
                || !prepared.stream->bindResourceSetSlot(matchedIndex))
            {
                self->audioStreamViolation_.store(
                    true, std::memory_order_release);
                return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
            }
            matched->stream = std::move(prepared.stream);
            *stream = matched->stream->view();
            return TALAS_KERNEL_ABI_STATUS_OK;
        }
        if (activeAudioPrepareHostV3 != self
            || !self->audioStreamBound_
            || self->audioStreamPhase_.load(std::memory_order_acquire)
                != AudioStreamPhase::Preparing)
        {
            self->audioStreamViolation_.store(
                true, std::memory_order_release);
            return TALAS_KERNEL_ABI_STATUS_NOT_PREPARED;
        }
        bool expected = false;
        if (!self->audioStreamResolved_.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            self->audioStreamViolation_.store(
                true, std::memory_order_release);
            return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
        }
        auto prepared = prepareComponentActivationAudioStreamV1(
            self->audioSourceBytes_, self->audioStreamGrant_, *prepare);
        if (!prepared.ok())
        {
            self->audioStreamViolation_.store(
                true, std::memory_order_release);
            return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
        }
        self->audioStream_ = std::move(prepared.stream);
        *stream = self->audioStream_->view();
        return TALAS_KERNEL_ABI_STATUS_OK;
    }

    [[nodiscard]] static TalasKernelAbiStatusV1 readImmutableAudioStream(
        void* context,
        const TalasKernelAbiImmutableAudioStreamReadV3* read,
        TalasKernelAbiImmutableAudioStreamReadResultV3* result) noexcept
    {
        auto* self = static_cast<BoundedActivationHost*>(context);
        if (self == nullptr || read == nullptr || result == nullptr)
            return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
        if (self->resourceSetBound_)
        {
            if (activeAudioProcessScopeV3.host != self
                || activeAudioProcessScopeV3.outerSequence == 0U)
            {
                self->audioStreamViolation_.store(
                    true, std::memory_order_release);
                return TALAS_KERNEL_ABI_STATUS_NOT_PREPARED;
            }
            const auto slotTag = read->streamToken
                & componentActivationAudioStreamSetSlotMaskV1;
            if (slotTag == 0U || slotTag > self->setAudioSlotCount_)
            {
                self->audioStreamViolation_.store(
                    true, std::memory_order_release);
                return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
            }
            const auto matchedIndex = static_cast<std::size_t>(slotTag - 1U);
            auto& matched = self->setAudioSlots_[matchedIndex];
            if (matched.stream == nullptr
                || matched.stream->view().streamToken != read->streamToken)
            {
                self->audioStreamViolation_.store(
                    true, std::memory_order_release);
                return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
            }
            const auto consumedBit = static_cast<std::uint8_t>(
                1U << matchedIndex);
            if (matched.phase.load(std::memory_order_acquire)
                    != AudioStreamPhase::Processing
                || (activeAudioProcessScopeV3.consumedAudioSlots
                    & consumedBit) != 0U)
            {
                self->audioStreamViolation_.store(
                    true, std::memory_order_release);
                return TALAS_KERNEL_ABI_STATUS_NOT_PREPARED;
            }
            activeAudioProcessScopeV3.consumedAudioSlots |= consumedBit;
            const auto status = matched.stream->read(*read, *result);
            if (status != TALAS_KERNEL_ABI_STATUS_OK)
            {
                self->audioStreamViolation_.store(
                    true, std::memory_order_release);
            }
            return status;
        }
        if (activeAudioProcessScopeV3.host != self
            || activeAudioProcessScopeV3.outerSequence == 0U
            || activeAudioProcessScopeV3.consumedAudioSlots != 0U
            || self->audioStreamPhase_.load(std::memory_order_acquire)
                != AudioStreamPhase::Processing
            || self->audioStream_ == nullptr)
        {
            self->audioStreamViolation_.store(
                true, std::memory_order_release);
            return TALAS_KERNEL_ABI_STATUS_NOT_PREPARED;
        }
        activeAudioProcessScopeV3.consumedAudioSlots = 1U;
        const auto status = self->audioStream_->read(*read, *result);
        if (status != TALAS_KERNEL_ABI_STATUS_OK)
        {
            self->audioStreamViolation_.store(
                true, std::memory_order_release);
        }
        return status;
    }

    [[nodiscard]] static void* allocate(
        void* context,
        std::uint64_t byteCount,
        std::uint64_t alignment) noexcept
    {
        auto* self = static_cast<BoundedActivationHost*>(context);
        if (self == nullptr)
            return nullptr;
        if (self->realtimeCallbackActive_.load(std::memory_order_acquire))
        {
            self->violationObserved_.store(true, std::memory_order_release);
            return nullptr;
        }
        os_unfair_lock_lock(&self->lock_);
        auto& calls = self->checkpointActive_ ? self->checkpointAllocationCalls_ : self->allocationCalls_;
        auto& bytes = self->checkpointActive_ ? self->checkpointAllocationBytes_ : self->allocationBytes_;
        auto& allocations = self->checkpointActive_ ? self->checkpointAllocations_ : self->allocations_;
        const auto nextCalls = calls + 1U;
        if (self->realtimeCallbackActive_.load(std::memory_order_acquire)
            || (self->checkpointActive_ && !self->checkpointCapturing_)
            || byteCount == 0 || byteCount > maximumAllocationBytes
            || !validAlignment(alignment) || nextCalls > maximumAllocations
            || bytes > maximumAllocationBytes - byteCount)
        {
            calls = nextCalls;
            self->violation_ = true;
            self->violationObserved_.store(true, std::memory_order_release);
            os_unfair_lock_unlock(&self->lock_);
            return nullptr;
        }
        auto* slot = std::find_if(allocations.begin(), allocations.end(),
            [](const auto& value) { return value.pointer == nullptr; });
        if (slot == allocations.end())
        {
            calls = nextCalls;
            self->violation_ = true;
            self->violationObserved_.store(true, std::memory_order_release);
            os_unfair_lock_unlock(&self->lock_);
            return nullptr;
        }
        void* pointer = nullptr;
        const auto nativeAlignment = std::max<std::uint64_t>(alignment, sizeof(void*));
        bool failAllocation = false;
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
        if (self->checkpointActive_)
            failAllocation = self->failCheckpointAllocation_.exchange(false, std::memory_order_acq_rel);
#endif
        if (failAllocation || posix_memalign(&pointer, static_cast<std::size_t>(nativeAlignment),
                static_cast<std::size_t>(byteCount)) != 0 || pointer == nullptr)
        {
            calls = nextCalls;
            // A valid checkpoint allocation can fail without changing DSP or
            // weakening the original entry/create/prepare allocator policy.
            if (!self->checkpointActive_)
            {
                self->violation_ = true;
                self->violationObserved_.store(true, std::memory_order_release);
            }
            os_unfair_lock_unlock(&self->lock_);
            return nullptr;
        }
        calls = nextCalls;
        bytes += byteCount;
        *slot = {pointer, byteCount, alignment};
        os_unfair_lock_unlock(&self->lock_);
        return pointer;
    }

    static void deallocate(
        void* context,
        void* pointer,
        std::uint64_t byteCount,
        std::uint64_t alignment) noexcept
    {
        auto* self = static_cast<BoundedActivationHost*>(context);
        if (self == nullptr || pointer == nullptr)
            return;
        if (self->realtimeCallbackActive_.load(std::memory_order_acquire))
        {
            self->violationObserved_.store(true, std::memory_order_release);
            return;
        }
        os_unfair_lock_lock(&self->lock_);
        auto& allocations = self->checkpointActive_ ? self->checkpointAllocations_ : self->allocations_;
        const auto slot = std::find_if(
            allocations.begin(),
            allocations.end(),
            [pointer](const auto& value) { return value.pointer == pointer; });
        if (slot == allocations.end()
            || (self->checkpointActive_ && !self->checkpointCapturing_ && !self->checkpointDestroying_)
            || slot->bytes != byteCount || slot->alignment != alignment
            || self->realtimeCallbackActive_.load(std::memory_order_acquire))
        {
            self->violation_ = true;
            self->violationObserved_.store(true, std::memory_order_release);
            os_unfair_lock_unlock(&self->lock_);
            return;
        }
        *slot = {};
        os_unfair_lock_unlock(&self->lock_);
        std::free(pointer);
    }

    static void diagnostic(
        void* context,
        TalasKernelAbiDiagnosticSeverityV1 severity,
        std::uint32_t,
        TalasKernelAbiStringViewV1 message) noexcept
    {
        auto* self = static_cast<BoundedActivationHost*>(context);
        if (self == nullptr)
            return;
        if (self->realtimeCallbackActive_.load(std::memory_order_acquire))
        {
            self->violationObserved_.store(true, std::memory_order_release);
            return;
        }
        os_unfair_lock_lock(&self->lock_);
        ++self->diagnosticCalls_;
        if (self->realtimeCallbackActive_.load(std::memory_order_acquire)
            || severity < TALAS_KERNEL_ABI_DIAGNOSTIC_INFO
            || severity > TALAS_KERNEL_ABI_DIAGNOSTIC_ERROR
            || (message.byteCount != 0 && message.data == nullptr)
            || message.byteCount > maximumDiagnosticBytes
            || self->diagnosticBytes_
                > maximumDiagnosticBytes - message.byteCount
            || self->diagnosticCalls_ > maximumDiagnostics)
        {
            self->violation_ = true;
            self->violationObserved_.store(true, std::memory_order_release);
        }
        else
        {
            self->diagnosticBytes_ += message.byteCount;
        }
        os_unfair_lock_unlock(&self->lock_);
    }

    os_unfair_lock lock_ = OS_UNFAIR_LOCK_INIT;
    TalasKernelAbiHostServicesV1 services_{};
    TalasKernelAbiHostServicesV2 servicesV2_{};
    TalasKernelAbiHostServicesV3 servicesV3_{};
    TalasKernelAbiHostServicesV4 servicesV4_{};
    TalasKernelAbiImmutableResourceViewV2 resourceView_{};
    std::array<char, maximumComponentActivationDescriptorIdBytesV1>
        resourceId_{};
    std::array<HostAllocationV1, maximumAllocations> allocations_{};
    std::array<HostAllocationV1, maximumAllocations> checkpointAllocations_{};
    mutable std::mutex resourceCheckpointInspectionMutex_;
    bool resourceCheckpointActive_ = false;
    std::optional<ComponentActivationAudioStreamHealthV1> checkpointAudioHealth_;
    std::optional<ComponentActivationAudioStreamHealthSetV1> checkpointSetHealth_;
    // Protected by lock_; the original lifetime budget remains separate.
    std::uint64_t checkpointAllocationCalls_ = 0;
    std::uint64_t checkpointAllocationBytes_ = 0;
    bool checkpointActive_ = false;
    bool checkpointCapturing_ = false;
    bool checkpointDestroying_ = false;
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
    std::atomic<bool> failCheckpointAllocation_{false};
#endif
    std::atomic<bool> realtimeCallbackActive_{false};
    std::atomic<bool> violationObserved_{false};
    std::atomic<ResourcePhase> resourcePhase_{ResourcePhase::Idle};
    std::atomic<bool> resourceResolved_{false};
    std::atomic<bool> resourceViolation_{false};
    std::atomic<AudioStreamPhase> audioStreamPhase_{AudioStreamPhase::Idle};
    std::atomic<bool> audioStreamResolved_{false};
    std::atomic<bool> audioStreamViolation_{false};
    std::uint64_t allocationCalls_ = 0;
    std::uint64_t allocationBytes_ = 0;
    std::uint64_t diagnosticCalls_ = 0;
    std::uint64_t diagnosticBytes_ = 0;
    bool violation_ = false;
    bool resourceBound_ = false;
    bool audioStreamBound_ = false;
    ComponentActivationAudioStreamGrantV1 audioStreamGrant_{};
    std::span<const std::uint8_t> audioSourceBytes_;
    std::unique_ptr<ComponentActivationAudioStreamV1> audioStream_;
    std::array<
        SetImmutableSlot,
        maximumComponentActivationResourceSetMembersV1>
        setImmutableSlots_{};
    std::array<
        SetAudioSlot,
        maximumComponentActivationResourceSetAudioStreamsV1>
        setAudioSlots_{};
    std::array<std::uint8_t, componentTrustDigestBytes> resourceSetDigest_{};
    std::size_t setImmutableSlotCount_ = 0;
    std::size_t setAudioSlotCount_ = 0;
    std::uint16_t resourceSetMemberCount_ = 0;
    bool resourceSetBound_ = false;
};

[[nodiscard]] bool copyText(
    TalasKernelAbiStringViewV1 source,
    std::size_t maximumBytes,
    std::string& destination)
{
    if (source.data == nullptr || source.byteCount == 0
        || source.byteCount > maximumBytes)
    {
        return false;
    }
    destination.assign(source.data, static_cast<std::size_t>(source.byteCount));
    return true;
}

[[nodiscard]] bool copyModuleMetadata(
    const TalasKernelAbiModuleV1* source,
    component_probe::ComponentBundleProbeCopiedModuleMetadataV1& destination)
{
    if (source == nullptr || source->kernelCount == 0
        || source->kernelCount > component_bundle::maximumComponentBundleNodes
        || source->kernels == nullptr
        || !copyText(
            source->moduleId,
            maximumComponentBundleIdBytes,
            destination.moduleId)
        || !copyText(
            source->moduleVersion,
            maximumComponentSemanticVersionBytes,
            destination.moduleVersion))
    {
        return false;
    }
    destination.structSize = source->structSize;
    destination.descriptorVersion = source->descriptorVersion;
    destination.minimumApiVersion = source->minimumApiVersion;
    destination.maximumApiVersion = source->maximumApiVersion;
    destination.compatibility = source->compatibility;
    destination.sampleFormats = source->sampleFormats;
    destination.trustClass = source->trustClass;
    destination.reserved0 = source->reserved0;
    destination.capabilityFlags = source->capabilityFlags;
    destination.hasUnload = source->unload != nullptr;
    destination.reserved1 = source->reserved1;
    destination.reserved2 = source->reserved2;
    destination.kernels.reserve(static_cast<std::size_t>(source->kernelCount));
    for (std::size_t index = 0; index < source->kernelCount; ++index)
    {
        const auto& sourceKernel = source->kernels[index];
        component_probe::ComponentBundleProbeCopiedKernelMetadataV1 kernel;
        kernel.structSize = sourceKernel.structSize;
        kernel.descriptorVersion = sourceKernel.descriptorVersion;
        if (!copyText(
                sourceKernel.typeId,
                component_bundle::maximumComponentBundleStableIdBytes,
                kernel.typeId))
        {
            return false;
        }
        kernel.capabilityFlags = sourceKernel.capabilityFlags;
        kernel.hasCreate = sourceKernel.create != nullptr;
        kernel.hasPrepare = sourceKernel.prepare != nullptr;
        kernel.hasReset = sourceKernel.reset != nullptr;
        kernel.hasProcess = sourceKernel.process != nullptr;
        kernel.hasDestroy = sourceKernel.destroy != nullptr;
        kernel.reserved0 = sourceKernel.reserved0;
        kernel.reserved1 = sourceKernel.reserved1;
        destination.kernels.push_back(std::move(kernel));
    }
    return true;
}

[[nodiscard]] TalasKernelAbiCompatibilityV1 expectedCompatibility() noexcept
{
    TalasKernelAbiCompatibilityV1 value{};
    value.structSize = sizeof(value);
    value.abiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    value.pointerBits = sizeof(void*) * 8U;
    value.littleEndian = std::endian::native == std::endian::little ? 1U : 0U;
    value.doubleBits = sizeof(double) * 8U;
    value.doubleIec559 = std::numeric_limits<double>::is_iec559 ? 1U : 0U;
    value.maximumAlignment = alignof(std::max_align_t);
    return value;
}

[[nodiscard]] bool parameterProfileMatches(
    const component_bundle::ComponentBundleNode& node,
    std::span<const ComponentActivationStaticParameterV1> parameters) noexcept
{
    if (parameters.size() != node.parameters.size()
        || validateComponentActivationStaticParametersV1(parameters)
            != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        return false;
    }
    for (std::size_t index = 0; index < parameters.size(); ++index)
    {
        const auto& expected = node.parameters[index];
        const auto& parameter = parameters[index];
        if (std::string_view{parameter.id.data(), parameter.idBytes}
            != expected.id)
        {
            return false;
        }
        const auto kind = static_cast<ComponentActivationStaticParameterKindV1>(
            parameter.kind);
        switch (expected.kind)
        {
            case talas::core::ParameterKind::Expression:
                if (kind != ComponentActivationStaticParameterKindV1::Scalar)
                    return false;
                break;
            case talas::core::ParameterKind::Choice:
                if (kind != ComponentActivationStaticParameterKindV1::Integer
                    || parameter.integerValue < 0
                    || static_cast<std::uint64_t>(parameter.integerValue)
                        >= expected.choices.size())
                {
                    return false;
                }
                break;
            case talas::core::ParameterKind::Boolean:
                if (kind != ComponentActivationStaticParameterKindV1::Integer
                    || (parameter.integerValue != 0
                        && parameter.integerValue != 1))
                {
                    return false;
                }
                break;
            case talas::core::ParameterKind::Text:
                if (kind != ComponentActivationStaticParameterKindV1::Text)
                    return false;
                break;
            case talas::core::ParameterKind::Resource:
                if (kind
                        != ComponentActivationStaticParameterKindV1::Resource
                    || parameter.textBytes == 0)
                {
                    return false;
                }
                break;
        }
    }
    return true;
}

[[nodiscard]] bool manifestProfileMatches(
    const component_bundle::ComponentBundleManifest& manifest,
    const ComponentActivationAuthorizationV1& authorization,
    std::span<const ComponentActivationStaticParameterV1> parameters) noexcept
{
    const std::string_view bundleId{
        authorization.bundleId.data(), authorization.bundleIdBytes};
    const std::string_view version{
        authorization.semanticVersion.data(), authorization.semanticVersionBytes};
    const std::string_view descriptorId{
        authorization.descriptorId.data(), authorization.descriptorIdBytes};
    const auto resourceParameterCount = std::count_if(
        parameters.begin(),
        parameters.end(),
        [](const auto& parameter) {
            return parameter.kind == static_cast<std::uint32_t>(
                ComponentActivationStaticParameterKindV1::Resource);
        });
    const auto immutablePermission = componentPermission(
        ComponentExecutionPermission::ImmutableResourceRead);
    const auto audioPermission = componentPermission(
        ComponentExecutionPermission::BoundedAudioStreamRead);
    const auto hasImmutable =
        (authorization.requestedPermissions & immutablePermission) != 0;
    const auto hasAudio =
        (authorization.requestedPermissions & audioPermission) != 0;
    const auto resourceSetProfile = resourceParameterCount > 1
        && (hasImmutable || hasAudio);
    const auto audioStreamProfile = resourceParameterCount == 1
        && hasAudio && !hasImmutable;
    const auto resourceProfile = resourceParameterCount == 1
        && hasImmutable && !hasAudio;
    const auto statelessProfile = resourceParameterCount == 0
        && !hasImmutable && !hasAudio;
    const auto expectedApiVersion = resourceSetProfile
        ? TALAS_KERNEL_API_VERSION_V4
        : (audioStreamProfile
            ? TALAS_KERNEL_API_VERSION_V3
            : (resourceProfile
                ? TALAS_KERNEL_API_VERSION_V2
                : TALAS_KERNEL_API_VERSION_V1));
    if (manifest.bundleId != bundleId
        || manifest.semanticVersion != version
        || manifest.requestedPermissions != authorization.requestedPermissions
        || manifest.declaredRoles != authorization.declaredRoles
        || manifest.nodes.size() != 1 || !manifest.files.empty()
        || !manifest.dependencies.empty() || !manifest.state.migrations.empty()
        || manifest.declarativeUiPath.has_value()
        || (!statelessProfile && !resourceProfile
            && !audioStreamProfile && !resourceSetProfile)
        || resourceParameterCount
            > maximumComponentActivationResourceSetMembersV1
        || manifest.minimumEngineApiVersion != expectedApiVersion
        || manifest.maximumEngineApiVersion != expectedApiVersion
        || manifest.entry.byteCount != authorization.entryByteCount
        || manifest.entry.sha256 != authorization.entryDigest)
    {
        return false;
    }
    const auto& node = manifest.nodes.front();
    const auto audioPort = [](const auto& port) {
        return port.type == talas::core::PortType::Audio
            && port.signal.timelineBearing;
    };
    return node.typeId == descriptorId
        && node.inputs.size() == authorization.inputChannelCount
        && node.outputs.size() == authorization.outputChannelCount
        && std::ranges::all_of(node.inputs, audioPort)
        && std::ranges::all_of(node.outputs, audioPort)
        && parameterProfileMatches(node, parameters);
}

[[nodiscard]] bool exactSnapshot(
    std::span<const std::uint8_t> canonicalManifest,
    std::span<const std::uint8_t> entryBytes,
    const ComponentActivationAuthorizationV1& authorization,
    std::span<const ComponentActivationStaticParameterV1> parameters,
    component_bundle::ComponentBundleManifest& manifest)
{
    const std::string canonical{
        reinterpret_cast<const char*>(canonicalManifest.data()),
        canonicalManifest.size()};
    const auto parsed = component_bundle::parseComponentBundleManifest(canonical);
    if (!parsed.ok())
        return false;
    const auto rewritten =
        component_bundle::writeCanonicalComponentBundleManifest(*parsed.manifest);
    if (!rewritten.ok() || rewritten.canonicalJson != canonical
        || !manifestProfileMatches(*parsed.manifest, authorization, parameters))
    {
        return false;
    }
    component_bundle::ComponentBundleSnapshot snapshot;
    snapshot.manifest = *parsed.manifest;
    snapshot.canonicalManifest = canonical;
    snapshot.payloads.push_back({
        component_bundle::ComponentBundleFilePurpose::Resource,
        snapshot.manifest.entry.path,
        authorization.entryDigest,
        std::vector<std::uint8_t>(entryBytes.begin(), entryBytes.end()),
        true,
    });
    snapshot.contentDigest =
        component_bundle::calculateComponentBundleSnapshotContentDigest(snapshot);
    if (snapshot.contentDigest != authorization.contentDigest
        || !component_bundle::validateComponentBundleSnapshotIdentity(snapshot))
    {
        return false;
    }
    manifest = std::move(snapshot.manifest);
    return true;
}

[[nodiscard]] std::vector<TalasKernelAbiParameterV1> abiParameters(
    const std::vector<ComponentActivationStaticParameterV1>& parameters)
{
    std::vector<TalasKernelAbiParameterV1> result(parameters.size());
    for (std::size_t index = 0; index < parameters.size(); ++index)
    {
        const auto& source = parameters[index];
        auto& destination = result[index];
        destination.structSize = sizeof(destination);
        destination.id = {source.id.data(), source.idBytes};
        destination.scalarValue = source.scalarValue;
        destination.integerValue = source.integerValue;
        switch (static_cast<ComponentActivationStaticParameterKindV1>(
            source.kind))
        {
            case ComponentActivationStaticParameterKindV1::Scalar:
                destination.kind = TALAS_KERNEL_ABI_PARAMETER_SCALAR;
                break;
            case ComponentActivationStaticParameterKindV1::Integer:
                destination.kind = TALAS_KERNEL_ABI_PARAMETER_INTEGER;
                break;
            case ComponentActivationStaticParameterKindV1::Text:
                destination.kind = TALAS_KERNEL_ABI_PARAMETER_TEXT;
                destination.textValue = {
                    source.text.data(), source.textBytes};
                break;
            case ComponentActivationStaticParameterKindV1::Resource:
                destination.kind = TALAS_KERNEL_ABI_PARAMETER_RESOURCE;
                destination.textValue = {
                    source.text.data(), source.textBytes};
                break;
        }
    }
    return result;
}
}

struct ComponentActivationLoadedInstanceV1::Impl
{
    ~Impl()
    {
        // Successful prepare transfers a complete lifecycle here, including
        // when allocating the public loaded-instance wrapper subsequently fails.
        if (prepared && !closed) static_cast<void>(close());
    }

    [[nodiscard]] bool destroyKernelCheckpoint() noexcept
    {
        if (!checkpointActive) return true;
        bool clean = true;
        host.beginCheckpointDestroy();
        auto* payload = kernelCheckpoint;
        kernelCheckpoint = nullptr;
        if (payload != nullptr)
        {
            try { checkpointDescriptor.destroyCheckpoint(payload, &host.services()); }
            catch (...) { clean = false; }
        }
        if (!host.finishCheckpointDestroy()) clean = false;
        checkpointActive = false;
        if (!clean) prepared = false;
        return clean;
    }

    [[nodiscard]] bool close() noexcept
    {
        if (closed)
            return closeClean;
        closed = true;
        coordinatedCheckpointActive = false;
        bool clean = destroyKernelCheckpoint();
        host.stopAudioStream();
        if (instance != nullptr && descriptor.destroy != nullptr)
        {
            try
            {
                descriptor.destroy(instance, &host.services());
            }
            catch (...)
            {
                clean = false;
            }
            instance = nullptr;
        }
        if (moduleUnload != nullptr)
        {
            try
            {
                moduleUnload(moduleContext, &host.services());
            }
            catch (...)
            {
                clean = false;
            }
            moduleUnload = nullptr;
            moduleContext = nullptr;
        }
        if (!host.finish())
            clean = false;
        if (imageHandle != nullptr)
        {
            if (dlclose(imageHandle) != 0)
                clean = false;
            imageHandle = nullptr;
        }
        if (!resourceMapping.cleanup())
            clean = false;
        for (auto& mapping : resourceSetMappings)
        {
            if (!mapping.cleanup())
                clean = false;
        }
        if (!materialized.cleanup())
            clean = false;
        closeClean = clean;
        return clean;
    }

    PrivateMaterializedEntry materialized;
    PrivateImmutableResourceMapping resourceMapping;
    std::array<
        PrivateImmutableResourceMapping,
        maximumComponentActivationResourceSetMembersV1>
        resourceSetMappings{};
    BoundedActivationHost host;
    void* imageHandle = nullptr;
    TalasKernelAbiDescriptorV1 descriptor{};
    TalasKernelAbiCheckpointDescriptorV1 checkpointDescriptor{};
    void* kernelCheckpoint = nullptr;
    bool checkpointActive = false;
    bool coordinatedCheckpointActive = false;
    TalasKernelAbiModuleUnloadV1 moduleUnload = nullptr;
    void* moduleContext = nullptr;
    void* instance = nullptr;
    std::vector<ComponentActivationStaticParameterV1> parameters;
    std::vector<TalasKernelAbiParameterV1> preparedParameters;
    bool prepared = false;
    bool closed = false;
    bool closeClean = true;
};

ComponentActivationLoadedInstanceV1::ComponentActivationLoadedInstanceV1(
    std::unique_ptr<Impl> implementation) noexcept
    : impl_(std::move(implementation))
{
}

ComponentActivationLoadedInstanceV1::~ComponentActivationLoadedInstanceV1()
{
    static_cast<void>(shutdown());
}

bool ComponentActivationLoadedInstanceV1::active() const noexcept
{
    return impl_ && impl_->prepared && !impl_->closed;
}

bool ComponentActivationLoadedInstanceV1::supportsKernelOfflineCheckpoint() const noexcept
{
    return impl_ && impl_->checkpointDescriptor.capture != nullptr;
}

TalasKernelAbiStatusV1 ComponentActivationLoadedInstanceV1::beginKernelOfflineCheckpoint() noexcept
{
    if (impl_ && impl_->coordinatedCheckpointActive) return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    if (!active()) return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
    if (!supportsKernelOfflineCheckpoint()) return TALAS_KERNEL_ABI_STATUS_INCOMPATIBLE;
    if (impl_->checkpointActive) return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    if (!impl_->host.beginCheckpointCapture())
    {
        impl_->prepared = false;
        return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
    }
    impl_->checkpointActive = true;
    auto status = TalasKernelAbiStatusV1{TALAS_KERNEL_ABI_STATUS_FATAL};
    try
    {
        status = impl_->checkpointDescriptor.capture(impl_->instance,
            &impl_->host.services(), &impl_->kernelCheckpoint);
    }
    catch (...) { status = TALAS_KERNEL_ABI_STATUS_FATAL; }
    const bool hostValid = impl_->host.finishCheckpointCapture();
    if (!knownStatus(status) || (status == TALAS_KERNEL_ABI_STATUS_OK && impl_->kernelCheckpoint == nullptr))
        status = TALAS_KERNEL_ABI_STATUS_FATAL;
    if (status == TALAS_KERNEL_ABI_STATUS_OK && hostValid) return status;
    if (!hostValid || status == TALAS_KERNEL_ABI_STATUS_FATAL || status == TALAS_KERNEL_ABI_STATUS_QUARANTINED)
        impl_->prepared = false;
    if (!impl_->destroyKernelCheckpoint()) return TALAS_KERNEL_ABI_STATUS_FATAL;
    return hostValid ? status : TALAS_KERNEL_ABI_STATUS_QUARANTINED;
}

TalasKernelAbiStatusV1 ComponentActivationLoadedInstanceV1::restoreKernelOfflineCheckpoint() noexcept
{
    if (impl_ && impl_->coordinatedCheckpointActive) return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    if (!impl_ || !impl_->checkpointActive) return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    auto status = TalasKernelAbiStatusV1{TALAS_KERNEL_ABI_STATUS_QUARANTINED};
    if (active())
    {
        impl_->host.setRealtimeCallbackActive(true);
        try { status = impl_->checkpointDescriptor.restore(impl_->instance, impl_->kernelCheckpoint); }
        catch (...) { status = TALAS_KERNEL_ABI_STATUS_FATAL; }
        impl_->host.setRealtimeCallbackActive(false);
        if (status != TALAS_KERNEL_ABI_STATUS_OK || !impl_->host.valid())
        {
            impl_->prepared = false;
            status = TALAS_KERNEL_ABI_STATUS_FATAL;
        }
    }
    if (!impl_->destroyKernelCheckpoint()) return TALAS_KERNEL_ABI_STATUS_FATAL;
    return status;
}

TalasKernelAbiStatusV1 ComponentActivationLoadedInstanceV1::commitKernelOfflineCheckpoint() noexcept
{
    if (impl_ && impl_->coordinatedCheckpointActive) return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    if (!impl_ || !impl_->checkpointActive) return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    if (!impl_->destroyKernelCheckpoint()) return TALAS_KERNEL_ABI_STATUS_FATAL;
    return active() ? TALAS_KERNEL_ABI_STATUS_OK : TALAS_KERNEL_ABI_STATUS_QUARANTINED;
}

bool ComponentActivationLoadedInstanceV1::supportsOfflineCheckpoint() const noexcept
{
    return supportsKernelOfflineCheckpoint();
}

TalasKernelAbiStatusV1 ComponentActivationLoadedInstanceV1::beginOfflineCheckpoint() noexcept
{
    if (!active()) return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
    if (!supportsOfflineCheckpoint()) return TALAS_KERNEL_ABI_STATUS_INCOMPATIBLE;
    if (impl_->checkpointActive || impl_->coordinatedCheckpointActive) return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    if (!impl_->host.beginResourceCheckpoint())
    {
        if (!impl_->host.resourceStreamsHealthy())
        {
            impl_->prepared = false;
            return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
        }
        return TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
    }
    const auto status = beginKernelOfflineCheckpoint();
    if (status != TALAS_KERNEL_ABI_STATUS_OK)
    {
        if (!impl_->host.finishResourceCheckpoint(active()))
        {
            impl_->prepared = false;
            return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
        }
        return status;
    }
    impl_->coordinatedCheckpointActive = true;
    return TALAS_KERNEL_ABI_STATUS_OK;
}

TalasKernelAbiStatusV1 ComponentActivationLoadedInstanceV1::restoreOfflineCheckpoint() noexcept
{
    if (!impl_ || !impl_->coordinatedCheckpointActive) return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    // Consume coordinated ownership before calling the kernel-only primitive;
    // lifecycle calls are exclusive and aggregate health remains held by host.
    impl_->coordinatedCheckpointActive = false;
    if (!impl_->host.resourceStreamsHealthy()) impl_->prepared = false;
    const auto status = restoreKernelOfflineCheckpoint();
    if (!impl_->host.finishResourceCheckpoint(active() && status == TALAS_KERNEL_ABI_STATUS_OK))
    {
        impl_->prepared = false;
        return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
    }
    return status;
}

TalasKernelAbiStatusV1 ComponentActivationLoadedInstanceV1::commitOfflineCheckpoint() noexcept
{
    if (!impl_ || !impl_->coordinatedCheckpointActive) return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    impl_->coordinatedCheckpointActive = false;
    const auto status = commitKernelOfflineCheckpoint();
    if (!impl_->host.finishResourceCheckpoint(false))
    {
        impl_->prepared = false;
        return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
    }
    return status;
}

#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
void ComponentActivationLoadedInstanceV1::failNextCheckpointAllocationForTesting() noexcept
{
    if (impl_) impl_->host.failNextCheckpointAllocationForTesting();
}
#endif

TalasKernelAbiStatusV1 ComponentActivationLoadedInstanceV1::reset(
    const TalasKernelAbiResetV1& resetValue) noexcept
{
    if (!active() || impl_->descriptor.reset == nullptr)
        return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
    impl_->host.setRealtimeCallbackActive(true);
    auto status = TalasKernelAbiStatusV1{TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR};
    try
    {
        status = impl_->descriptor.reset(impl_->instance, &resetValue);
    }
    catch (...)
    {
        status = TALAS_KERNEL_ABI_STATUS_FATAL;
    }
    impl_->host.setRealtimeCallbackActive(false);
    if (!knownStatus(status) || !impl_->host.valid())
    {
        impl_->prepared = false;
        return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
    }
    if (status != TALAS_KERNEL_ABI_STATUS_OK)
        impl_->prepared = false;
    return status;
}

TalasKernelAbiStatusV1 ComponentActivationLoadedInstanceV1::process(
    const TalasKernelAbiProcessV1& processValue) noexcept
{
    return process(processValue, 0U);
}

TalasKernelAbiStatusV1 ComponentActivationLoadedInstanceV1::process(
    const TalasKernelAbiProcessV1& processValue,
    std::uint64_t outerSequence) noexcept
{
    if (!active() || impl_->descriptor.process == nullptr)
        return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
    if (!impl_->host.beginProcess(outerSequence))
    {
        impl_->prepared = false;
        return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
    }
    impl_->host.setRealtimeCallbackActive(true);
    auto status = TalasKernelAbiStatusV1{TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR};
    try
    {
        status = impl_->descriptor.process(impl_->instance, &processValue);
    }
    catch (...)
    {
        status = TALAS_KERNEL_ABI_STATUS_FATAL;
    }
    impl_->host.setRealtimeCallbackActive(false);
    const auto processScopeValid = impl_->host.finishProcess();
    if (!knownStatus(status) || !processScopeValid || !impl_->host.valid())
    {
        impl_->prepared = false;
        return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
    }
    if (status != TALAS_KERNEL_ABI_STATUS_OK)
        impl_->prepared = false;
    return status;
}

bool ComponentActivationLoadedInstanceV1::replaceAudioStreamEpochAndPrewarm(
    double initialSourceFrame) noexcept
{
    return active()
        && impl_->host.replaceAudioStreamEpochAndPrewarm(initialSourceFrame);
}

std::optional<ComponentActivationAudioStreamHealthV1>
ComponentActivationLoadedInstanceV1::audioStreamHealth() const noexcept
{
    return impl_ != nullptr ? impl_->host.audioStreamHealth() : std::nullopt;
}

std::optional<ComponentActivationAudioStreamHealthSetV1>
ComponentActivationLoadedInstanceV1::audioStreamHealthSet() const noexcept
{
    return impl_ != nullptr
        ? impl_->host.audioStreamHealthSet()
        : std::nullopt;
}

#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
std::array<
    TalasKernelAbiAudioStreamTokenV3,
    maximumComponentActivationResourceSetAudioStreamsV1>
ComponentActivationLoadedInstanceV1::audioStreamTokensForTesting()
    const noexcept
{
    return impl_ != nullptr
        ? impl_->host.audioStreamTokensForTesting()
        : std::array<
              TalasKernelAbiAudioStreamTokenV3,
              maximumComponentActivationResourceSetAudioStreamsV1>{};
}

void ComponentActivationLoadedInstanceV1::setAudioStreamHealthCountersForTest(
    std::uint64_t value,
    std::optional<std::size_t> resourceSetAudioSlot) noexcept
{
    if (impl_ != nullptr)
    {
        impl_->host.setAudioStreamHealthCountersForTest(
            value, resourceSetAudioSlot);
    }
}
#endif

bool ComponentActivationLoadedInstanceV1::shutdown() noexcept
{
    return !impl_ || impl_->close();
}

ComponentActivationLoaderResultV1 loadComponentActivationV1(
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session,
    std::span<const std::uint8_t> canonicalManifest,
    std::span<const std::uint8_t> entryBytes,
    std::span<const ComponentActivationStaticParameterV1> staticParameters,
    int materializationFileDescriptor,
    const ComponentActivationImmutableResourceGrantV1* resourceGrant,
    std::span<const std::uint8_t> resourceBytes,
    const ComponentActivationAudioStreamGrantV1* audioStreamGrant,
    std::span<const std::uint8_t> audioSourceBytes,
    const ComponentActivationPreparedResourceSetV1* resourceSet)
    noexcept
{
    ComponentActivationLoaderResultV1 result;
    OwnedDescriptor ownedMaterialization{materializationFileDescriptor};
    if (validateComponentActivationAuthorizationV1(authorization, session)
            != ComponentActivationProtocolDiagnosticV1::Valid
        || staticParameters.size() != authorization.staticParameterCount
        || validateComponentActivationStaticParametersV1(staticParameters)
            != ComponentActivationProtocolDiagnosticV1::Valid
        || calculateComponentActivationStaticParameterDigestV1(staticParameters)
            != authorization.staticParameterDigest)
    {
        return result;
    }
    if (entryBytes.empty()
        || entryBytes.size() != authorization.entryByteCount
        || calculateComponentActivationByteDigestV1(entryBytes)
            != authorization.entryDigest)
    {
        result.diagnostic = ComponentActivationLoaderDiagnosticV1::EntryRejected;
        return result;
    }
    const auto resourceParameterCount = std::count_if(
        staticParameters.begin(),
        staticParameters.end(),
        [](const auto& parameter) {
            return parameter.kind == static_cast<std::uint32_t>(
                ComponentActivationStaticParameterKindV1::Resource);
        });
    const auto immutablePermission = componentPermission(
        ComponentExecutionPermission::ImmutableResourceRead);
    const auto audioStreamPermission = componentPermission(
        ComponentExecutionPermission::BoundedAudioStreamRead);
    const auto hasImmutablePermission =
        (authorization.requestedPermissions & immutablePermission) != 0;
    const auto hasAudioStreamPermission =
        (authorization.requestedPermissions & audioStreamPermission) != 0;
    const auto resourceProfile = resourceParameterCount == 1
        && hasImmutablePermission && !hasAudioStreamPermission;
    const auto audioStreamProfile = resourceParameterCount == 1
        && hasAudioStreamPermission && !hasImmutablePermission;
    const auto resourceSetProfile = resourceParameterCount > 1
        && (hasImmutablePermission || hasAudioStreamPermission);
    if ((resourceParameterCount == 1
            && !resourceProfile && !audioStreamProfile)
        || (resourceParameterCount == 0
            && (hasImmutablePermission || hasAudioStreamPermission))
        || (resourceParameterCount > 1 && !resourceSetProfile)
        || (!resourceProfile
            && (resourceGrant != nullptr || !resourceBytes.empty()))
        || (!audioStreamProfile
            && (audioStreamGrant != nullptr || !audioSourceBytes.empty()))
        || (!resourceSetProfile && resourceSet != nullptr)
        || (resourceSetProfile
            && (resourceGrant != nullptr || !resourceBytes.empty()
                || audioStreamGrant != nullptr || !audioSourceBytes.empty()
                || !exactPreparedResourceSet(
                    resourceSet,
                    authorization,
                    session,
                    staticParameters)))
        || (resourceProfile
            && (resourceGrant == nullptr || resourceBytes.empty()
                || resourceBytes.size() != resourceGrant->byteCount
                || resourceBytes.size()
                    > maximumComponentActivationImmutableResourceBytesV1
                || calculateComponentActivationByteDigestV1(resourceBytes)
                    != resourceGrant->mediatedPayloadDigest
                || validateComponentActivationImmutableResourceGrantV1(
                       *resourceGrant,
                       authorization,
                       session,
                       staticParameters)
                    != ComponentActivationProtocolDiagnosticV1::Valid))
        || (audioStreamProfile
            && (audioStreamGrant == nullptr || audioSourceBytes.empty()
                || audioSourceBytes.size()
                    != audioStreamGrant->sourceByteCount
                || audioSourceBytes.size()
                    > maximumComponentActivationAudioSourceBytesV1
                || calculateComponentActivationByteDigestV1(audioSourceBytes)
                    != audioStreamGrant->sourceContentDigest
                || validateComponentActivationAudioStreamGrantV1(
                       *audioStreamGrant,
                       authorization,
                       session,
                       staticParameters)
                    != ComponentActivationProtocolDiagnosticV1::Valid)))
    {
        result.diagnostic = audioStreamProfile
            ? ComponentActivationLoaderDiagnosticV1::AudioStreamRejected
            : ComponentActivationLoaderDiagnosticV1::ResourceRejected;
        return result;
    }
    const auto macho = component_probe::validateComponentBundleMachOV1(
        entryBytes,
        component_probe::currentComponentBundleMachOArchitectureV1());
    if (!macho.ok())
    {
        result.diagnostic = ComponentActivationLoaderDiagnosticV1::EntryRejected;
        return result;
    }

    try
    {
        component_bundle::ComponentBundleManifest manifest;
        if (!exactSnapshot(
                canonicalManifest,
                entryBytes,
                authorization,
                staticParameters,
                manifest))
        {
            result.diagnostic =
                ComponentActivationLoaderDiagnosticV1::ManifestRejected;
            return result;
        }
        auto implementation = std::make_unique<
            ComponentActivationLoadedInstanceV1::Impl>();
        implementation->parameters.assign(
            staticParameters.begin(), staticParameters.end());
        implementation->preparedParameters = abiParameters(
            implementation->parameters);
        if (resourceProfile
            && (!implementation->resourceMapping.create(
                    resourceBytes, resourceGrant->mediatedPayloadDigest)
                || !implementation->host.bindImmutableResource(
                    *resourceGrant,
                    implementation->resourceMapping.bytes())))
        {
            result.diagnostic =
                ComponentActivationLoaderDiagnosticV1::ResourceRejected;
            return result;
        }
        if (audioStreamProfile
            && !implementation->host.bindAudioStream(
                *audioStreamGrant, audioSourceBytes))
        {
            result.diagnostic =
                ComponentActivationLoaderDiagnosticV1::AudioStreamRejected;
            return result;
        }
        std::array<
            ComponentActivationPreparedResourceSetMemberV1,
            maximumComponentActivationResourceSetMembersV1>
            boundSetMembers{};
        if (resourceSetProfile)
        {
            std::size_t immutableMappingIndex = 0;
            for (std::size_t index = 0;
                 index < resourceSet->members.size();
                 ++index)
            {
                boundSetMembers[index] = resourceSet->members[index];
                auto& bound = boundSetMembers[index];
                if (bound.immutableGrant == nullptr)
                    continue;
                if (immutableMappingIndex
                        >= implementation->resourceSetMappings.size()
                    || !implementation->resourceSetMappings[
                            immutableMappingIndex]
                            .create(
                                bound.immutableBytes,
                                bound.immutableGrant->mediatedPayloadDigest))
                {
                    result.diagnostic =
                        ComponentActivationLoaderDiagnosticV1::ResourceRejected;
                    return result;
                }
                bound.immutableBytes = implementation->resourceSetMappings[
                    immutableMappingIndex]
                                           .bytes();
                ++immutableMappingIndex;
            }
            if (!implementation->host.bindResourceSet(
                    *resourceSet->grant,
                    std::span<const ComponentActivationPreparedResourceSetMemberV1>{
                        boundSetMembers.data(), resourceSet->members.size()}))
            {
                result.diagnostic =
                    ComponentActivationLoaderDiagnosticV1::ResourceRejected;
                return result;
            }
        }
        if (!implementation->materialized.create(
                ownedMaterialization.release(),
                entryBytes,
                authorization.entryDigest))
        {
            result.diagnostic = ComponentActivationLoaderDiagnosticV1::LoadRejected;
            return result;
        }
        implementation->imageHandle = dlopen(
            implementation->materialized.path(),
            RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
        if (implementation->imageHandle == nullptr)
        {
            result.diagnostic = ComponentActivationLoaderDiagnosticV1::LoadRejected;
            return result;
        }
        // The mapped image no longer needs the host's file, so do not keep a
        // writable descriptor to it for the rest of the session.
        if (!implementation->materialized.cleanup())
        {
            result.diagnostic = ComponentActivationLoaderDiagnosticV1::LoadRejected;
            return result;
        }
        auto* symbol = dlsym(
            implementation->imageHandle,
            component_probe::componentBundleProbeEntrySymbolV1.data());
        if (symbol == nullptr)
        {
            result.diagnostic = ComponentActivationLoaderDiagnosticV1::LoadRejected;
            return result;
        }
        component_probe::ComponentBundleProbeEntryPointV1 entry = nullptr;
        static_assert(sizeof(entry) == sizeof(symbol));
        std::memcpy(&entry, &symbol, sizeof(entry));
        const TalasKernelAbiModuleV1* module = nullptr;
        auto entryStatus = TalasKernelAbiStatusV1{TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR};
        try
        {
            entryStatus = entry(&implementation->host.services(), &module);
        }
        catch (...)
        {
            entryStatus = TALAS_KERNEL_ABI_STATUS_FATAL;
        }
        if (!knownStatus(entryStatus) || entryStatus != TALAS_KERNEL_ABI_STATUS_OK
            || module == nullptr || !implementation->host.valid())
        {
            result.diagnostic = ComponentActivationLoaderDiagnosticV1::LoadRejected;
            return result;
        }
        component_probe::ComponentBundleProbeCopiedModuleMetadataV1 copied;
        if (!copyModuleMetadata(module, copied)
            || !component_probe::validateComponentBundleProbeAbiV1(
                    copied,
                    manifest,
                    expectedCompatibility(),
                    {})
                    .ok())
        {
            result.diagnostic = ComponentActivationLoaderDiagnosticV1::AbiRejected;
            return result;
        }
        const std::string_view descriptorId{
            authorization.descriptorId.data(), authorization.descriptorIdBytes};
        const auto selected = std::find_if(
            copied.kernels.begin(),
            copied.kernels.end(),
            [descriptorId](const auto& value) {
                return value.typeId == descriptorId;
            });
        if (selected == copied.kernels.end())
        {
            result.diagnostic = ComponentActivationLoaderDiagnosticV1::AbiRejected;
            return result;
        }
        const auto selectedIndex = static_cast<std::size_t>(
            selected - copied.kernels.begin());
        implementation->descriptor = module->kernels[selectedIndex];
        std::string selectedId;
        if (!copyText(
                implementation->descriptor.typeId,
                component_bundle::maximumComponentBundleStableIdBytes,
                selectedId)
            || selectedId != descriptorId
            || implementation->descriptor.structSize
                != sizeof(TalasKernelAbiDescriptorV1)
            || implementation->descriptor.descriptorVersion
                != TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1
            || implementation->descriptor.create == nullptr
            || implementation->descriptor.prepare == nullptr
            || implementation->descriptor.reset == nullptr
            || implementation->descriptor.process == nullptr
            || implementation->descriptor.destroy == nullptr)
        {
            result.diagnostic = ComponentActivationLoaderDiagnosticV1::AbiRejected;
            return result;
        }
        // Extension inspection occurs after module/descriptor authorization and
        // before lifecycle creation. Rejecting it must close the materialized
        // image without invoking unaccepted module lifecycle callbacks.
        bool extensionAccepted = false;
        struct ExtensionAdmissionGuard
        {
            ComponentActivationLoadedInstanceV1::Impl* implementation;
            bool& accepted;
            ~ExtensionAdmissionGuard()
            {
                if (!accepted) static_cast<void>(implementation->close());
            }
        } extensionGuard{implementation.get(), extensionAccepted};
        auto* checkpointSymbol = dlsym(implementation->imageHandle, TALAS_KERNEL_ABI_CHECKPOINT_ENTRY_SYMBOL_V1);
        if (checkpointSymbol != nullptr)
        {
            TalasKernelAbiCheckpointEntryPointV1 checkpointEntry = nullptr;
            static_assert(sizeof(checkpointEntry) == sizeof(checkpointSymbol));
            std::memcpy(&checkpointEntry, &checkpointSymbol, sizeof(checkpointEntry));
            const TalasKernelAbiCheckpointExtensionV1* extension = nullptr;
            auto checkpointStatus = TalasKernelAbiStatusV1{TALAS_KERNEL_ABI_STATUS_FATAL};
            implementation->host.setRealtimeCallbackActive(true);
            try { checkpointStatus = checkpointEntry(&extension); }
            catch (...) { checkpointStatus = TALAS_KERNEL_ABI_STATUS_FATAL; }
            implementation->host.setRealtimeCallbackActive(false);
            if (checkpointStatus != TALAS_KERNEL_ABI_STATUS_OK || extension == nullptr || !implementation->host.valid())
            {
                result.diagnostic = ComponentActivationLoaderDiagnosticV1::AbiRejected;
                return result;
            }
            std::vector<std::string_view> types;
            types.reserve(copied.kernels.size());
            for (const auto& kernel : copied.kernels) types.push_back(kernel.typeId);
            auto checked = kernel_abi_checkpoint::validateAndCopy(extension, copied.moduleId,
                copied.moduleVersion, types, component_bundle::maximumComponentBundleStableIdBytes);
            if (!checked.ok())
            {
                result.diagnostic = ComponentActivationLoaderDiagnosticV1::AbiRejected;
                return result;
            }
            implementation->checkpointDescriptor = checked.descriptors[selectedIndex];
        }
        extensionAccepted = true;
        implementation->moduleUnload = module->unload;
        implementation->moduleContext = module->moduleContext;
        auto createStatus = TalasKernelAbiStatusV1{TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR};
        try
        {
            createStatus = implementation->descriptor.create(
                &implementation->host.services(),
                &implementation->instance);
        }
        catch (...)
        {
            createStatus = TALAS_KERNEL_ABI_STATUS_FATAL;
        }
        if (!knownStatus(createStatus)
            || createStatus != TALAS_KERNEL_ABI_STATUS_OK
            || implementation->instance == nullptr
            || !implementation->host.valid())
        {
            result.diagnostic = ComponentActivationLoaderDiagnosticV1::CreateRejected;
            return result;
        }
        TalasKernelAbiPrepareV1 prepare{};
        prepare.structSize = sizeof(prepare);
        prepare.apiVersion = resourceSetProfile
            ? TALAS_KERNEL_API_VERSION_V4
            : (audioStreamProfile
                ? TALAS_KERNEL_API_VERSION_V3
                : (resourceProfile
                    ? TALAS_KERNEL_API_VERSION_V2
                    : TALAS_KERNEL_API_VERSION_V1));
        prepare.sampleRate = authorization.sampleRate;
        prepare.maximumBlockFrames = authorization.blockFrames;
        prepare.parameters = implementation->preparedParameters.data();
        prepare.parameterCount = implementation->preparedParameters.size();
        prepare.host = &implementation->host.services();
        auto prepareStatus = TalasKernelAbiStatusV1{TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR};
        if (!implementation->host.beginPrepare())
        {
            result.diagnostic = ComponentActivationLoaderDiagnosticV1::HostViolation;
            return result;
        }
        try
        {
            prepareStatus = implementation->descriptor.prepare(
                implementation->instance, &prepare);
        }
        catch (...)
        {
            prepareStatus = TALAS_KERNEL_ABI_STATUS_FATAL;
        }
        const auto prepareHostValid = implementation->host.finishPrepare(
            prepareStatus == TALAS_KERNEL_ABI_STATUS_OK);
        if (!knownStatus(prepareStatus)
            || prepareStatus != TALAS_KERNEL_ABI_STATUS_OK
            || !prepareHostValid
            || !implementation->host.valid())
        {
            result.diagnostic = ComponentActivationLoaderDiagnosticV1::PrepareRejected;
            return result;
        }
        implementation->prepared = true;
        result.instance = std::unique_ptr<ComponentActivationLoadedInstanceV1>(
            new ComponentActivationLoadedInstanceV1(
                std::move(implementation)));
        result.diagnostic = ComponentActivationLoaderDiagnosticV1::Ready;
        return result;
    }
    catch (const std::bad_alloc&)
    {
        result.diagnostic = ComponentActivationLoaderDiagnosticV1::AllocationFailed;
        return result;
    }
    catch (...)
    {
        result.diagnostic = ComponentActivationLoaderDiagnosticV1::LoadRejected;
        return result;
    }
}
}
